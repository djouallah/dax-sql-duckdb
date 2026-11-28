#define DUCKDB_EXTENSION_MAIN

#include "dax_sql_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/parser_options.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "dax_sql_grammar.hpp"
#include "js_engine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>

namespace duckdb {

// The extension's state for one database: the compiler and glue in QuickJS, and the views' names
// (lower case), so a statement that names none skips the JS.
struct DaxSqlState {
	JsEngine js;
	std::mutex names_lock;
	vector<string> names;
	//! The databases whose views are defined: those at LOAD, and each attached since (#8)
	std::mutex databases_lock;
	std::set<string> databases;
	//! The lakehouses' and workspaces' names by GUID (lower case), from OneLake's blob listings
	std::mutex lakes_lock;
	unordered_map<string, string> display;
	//! The catalogs the extension attached (lower case): a catalog of that name is the extension's
	//! to attach afresh; any other is the session's, read as it is
	std::set<string> attached;
	//! The session's first database: a view kept there reads its tables on the search path; one
	//! kept in another database reads them in it
	string home;

	string Own(const string &db) const {
		return StringUtil::Lower(db) == StringUtil::Lower(home) ? "" : db;
	}

	void RefreshNames() {
		auto text = js.Call("names", {});
		std::lock_guard<std::mutex> guard(names_lock);
		names = StringUtil::Split(text, '\n');
	}

	bool MightBeOurs(const string &query) {
		auto lower = StringUtil::Lower(query);
		if (lower.find("semantic") != string::npos) {
			return true;
		}
		std::lock_guard<std::mutex> guard(names_lock);
		for (auto &name : names) {
			if (!name.empty() && lower.find(name) != string::npos) {
				return true;
			}
		}
		return false;
	}
};

struct DaxSqlFunctionInfo : public TableFunctionInfo {
	explicit DaxSqlFunctionInfo(shared_ptr<DaxSqlState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<DaxSqlState> state;
};

// Set while the extension runs statements of its own (FrameView, the stored rows, SHOW's rows):
// their errors are theirs, not a model query's.
static thread_local bool framing = false;

//===--------------------------------------------------------------------===//
// Model queries: plain SQL that DuckDB parses, and that fails to bind on the view's name. On that
// error the statement's own text goes to the glue (dax-sql's compileStatement for each model query
// in it, DESCRIBE v as its rows); the SQL that comes back is parsed, its body put in the
// statement's place, and DuckDB binds it again. The extension's own statements are grammar rules
// (dax_sql_grammar.cpp).
//===--------------------------------------------------------------------===//
static unique_ptr<SQLStatement> ParseOne(ClientContext &context, const string &sql) {
	// the connection's parser settings
	auto options = ParserOptions::Builtin();
	options.identifier_case_mode = Settings::Get<PreserveIdentifierCaseSetting>(context);
	options.integer_division = Settings::Get<IntegerDivisionSetting>(context);
	options.regex_match_operator_semantics = Settings::Get<RegexMatchOperatorSemanticsSetting>(context);
	options.max_expression_depth = Settings::Get<MaxExpressionDepthSetting>(context);
	Parser parser(options);
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1) {
		return nullptr;
	}
	return std::move(parser.statements[0]);
}

//! The body of `fresh` in place of `statement`'s: both the same kind of statement, parsed from the
//! same text but for its model queries
static bool SwapBody(SQLStatement &statement, SQLStatement &fresh) {
	if (statement.type != fresh.type) {
		return false;
	}
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		statement.Cast<SelectStatement>().node = std::move(fresh.Cast<SelectStatement>().node);
		break;
	case StatementType::EXPLAIN_STATEMENT:
		statement.Cast<ExplainStatement>().stmt = std::move(fresh.Cast<ExplainStatement>().stmt);
		break;
	case StatementType::CREATE_STATEMENT:
		statement.Cast<CreateStatement>().info = std::move(fresh.Cast<CreateStatement>().info);
		break;
	case StatementType::INSERT_STATEMENT:
		statement.Cast<InsertStatement>().node = std::move(fresh.Cast<InsertStatement>().node);
		break;
	case StatementType::COPY_STATEMENT:
		statement.Cast<CopyStatement>().info = std::move(fresh.Cast<CopyStatement>().info);
		break;
	case StatementType::PREPARE_STATEMENT:
		statement.Cast<PrepareStatement>().statement = std::move(fresh.Cast<PrepareStatement>().statement);
		break;
	default:
		return false;
	}
	statement.named_param_map = fresh.named_param_map;
	statement.has_anonymous_parameters = fresh.has_anonymous_parameters;
	return true;
}

static void CatchUpDatabases(ClientContext &context, DaxSqlState &state);
static vector<vector<string>> QueryRows(Connection &con, const string &sql);

//! What a CREATE or DROP SEMANTIC VIEW changed, to put back if its transaction rolls back (#7): the
//! view's stored definition before it (existed false: there was no view)
struct ViewUndo {
	string db, schema, name;
	bool existed = false;
	string path, text;
};

static void UndoViewChange(ClientContext &context, DaxSqlState &state, const ViewUndo &undo);

class DaxSqlRebindState : public ClientContextState {
public:
	static constexpr const char *KEY = "dax_sql_rebind";

	explicit DaxSqlRebindState(shared_ptr<DaxSqlState> state_p) : state(std::move(state_p)) {
	}

	bool CanRequestRebind() override {
		return true;
	}

	void QueryBegin(ClientContext &context) override {
		if (!framing) {
			CatchUpDatabases(context, *state);
		}
	}

	void Record(ViewUndo undo) {
		changes.push_back(std::move(undo));
	}

	using ClientContextState::TransactionRollback;

	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		changes.clear();
	}

	// The glue and the stored rows are not transactional: what the transaction changed is put back,
	// the last change first.
	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		auto undo = std::move(changes);
		changes.clear();
		for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
			UndoViewChange(context, *state, *it);
		}
	}

	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) override {
		if (framing || (error.Type() != ExceptionType::CATALOG && error.Type() != ExceptionType::BINDER)) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		if (statement.query.empty() || !state->MightBeOurs(statement.query)) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		// a compiler error (SEMANTIC, UNSUPPORTED, ...) is thrown from here, as the statement's error
		auto sql = state->js.Call("bind_sql", {statement.query});
		if (sql.empty()) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		auto fresh = ParseOne(context, sql);
		if (!fresh || !SwapBody(statement, *fresh)) {
			return RebindQueryInfo::DO_NOT_REBIND;
		}
		// the compiled text names no view: a second failure is DuckDB's own
		statement.query = sql;
		return RebindQueryInfo::ATTEMPT_TO_REBIND;
	}

private:
	shared_ptr<DaxSqlState> state;
	vector<ViewUndo> changes;
};

//===--------------------------------------------------------------------===//
// Model queries compiled as they are parsed, so a client that binds a statement before it runs
// it binds plain SQL. The Python client does (con.sql: ClientContext::BindStatement, which takes
// no rebind), and so would every caller of a statement's schema. DuckDB calls a parser override
// only when allow_parser_override_extension is not 'default': LOAD sets it to 'fallback', where a
// text the override does not claim is parsed as usual.
//
// The override claims a text only when it compiled a model query in it. A text that holds one of
// the extension's own statements (a CALL) is left to the usual parse: the views it creates or
// replaces change what the statements after it compile to. A statement that does not compile
// here is left as it is, and DaxSqlRebindState compiles it when it fails to bind, or reports the
// compiler's error then.
//===--------------------------------------------------------------------===//
struct DaxSqlParserInfo : public ParserExtensionInfo {
	explicit DaxSqlParserInfo(shared_ptr<DaxSqlState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<DaxSqlState> state;
};

static ParserOverrideResult DaxSqlParserOverride(ParserExtensionInfo *info, const string &query,
                                                 ParserOptions &options) {
	auto &state = *info->Cast<DaxSqlParserInfo>().state;
	if (framing || !options.context) {
		return ParserOverrideResult();
	}
	// a database attached since the last statement: its views, before the text is read
	CatchUpDatabases(*options.context, state);
	if (!state.MightBeOurs(query)) {
		return ParserOverrideResult();
	}
	// A model query dax-sql read and refused (SEMANTIC, UNSUPPORTED): its error is the statement's,
	// thrown after the usual parse's errors had their turn (#6). A text it does not read (SYNTAX) is
	// DuckDB's.
	ErrorData refused;
	try {
		// the usual parse of the whole text, with this override off
		auto usual = options;
		usual.parser_override_setting = AllowParserOverride::DEFAULT_OVERRIDE;
		Parser parser(usual);
		parser.ParseQuery(query);
		bool compiled = false;
		for (auto &statement : parser.statements) {
			if (statement->type == StatementType::CALL_STATEMENT) {
				return ParserOverrideResult();
			}
			if (statement->query.empty() || !state.MightBeOurs(statement->query)) {
				continue;
			}
			string sql;
			try {
				sql = state.js.Call("bind_sql", {statement->query});
			} catch (std::exception &ex) {
				ErrorData error(ex);
				// in a text of one statement: in a script, a statement before it may replace the view
				if (parser.statements.size() == 1 &&
				    (error.Type() == ExceptionType::BINDER || error.Type() == ExceptionType::NOT_IMPLEMENTED)) {
					refused = std::move(error);
					break;
				}
				continue;
			}
			if (sql.empty()) {
				continue;
			}
			auto fresh = ParseOne(*options.context, sql);
			if (!fresh || !SwapBody(*statement, *fresh)) {
				continue;
			}
			// The statement keeps its own text: its location and length are in the text parsed, and
			// a client takes the text as the query (the Python client wraps it again to show it,
			// which compiles it again here).
			compiled = true;
		}
		if (!refused.HasError()) {
			if (!compiled) {
				return ParserOverrideResult();
			}
			return ParserOverrideResult(std::move(parser.statements));
		}
	} catch (std::exception &) {
		// a syntax error is the usual parse's to report
		return ParserOverrideResult();
	}
	refused.Throw();
}

class DaxSqlConnectionCallback : public ExtensionCallback {
public:
	explicit DaxSqlConnectionCallback(shared_ptr<DaxSqlState> state_p) : state(std::move(state_p)) {
	}
	void OnConnectionOpened(ClientContext &context) override {
		context.registered_state->GetOrCreate<DaxSqlRebindState>(DaxSqlRebindState::KEY, state);
	}

private:
	shared_ptr<DaxSqlState> state;
};

//===--------------------------------------------------------------------===//
// dax_sql_define(db, schema, name, path, mode): what CREATE SEMANTIC VIEW is rewritten to ('' for
// the session's database or schema). It reads the file, defines the view in the glue (an error if
// the file does not compile), attaches its OneLake lakehouses, and returns the row stored in the
// schema's dax_sql_semantic_views: none when IF NOT EXISTS finds the view there.
//===--------------------------------------------------------------------===//
//! The rows a table function of the extension's returns, computed at bind
struct DefineData : public TableFunctionData {
	vector<vector<Value>> rows;
};

struct DefineState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static string ReadFile(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = handle->GetFileSize();
	string text(size, '\0');
	if (size > 0) {
		handle->Read(&text[0], size, 0);
	}
	return text;
}

static string JsonString(const string &s) {
	string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out + "\"";
}

// A model's text, as the glue reads it: a file's own text, or for a folder (a PBIP's definition/,
// a .SemanticModel folder, a Fabric semantic view's folder of Ossie files) one JSON object of its
// files' texts, { "<file in it>": "<text>", ... }. The path is read as a file first; one that is no
// file is a folder, every file in it at any depth given to dax-sql's readModel, which picks what it
// reads and refuses what is no model (#11; until djouallah/dax-sql#35 says which files it reads).
static string ReadModel(ClientContext &context, const string &path) {
	string not_a_file;
	try {
		auto text = ReadFile(context, path);
		// a OneLake folder reads as an empty file: no model is empty
		if (!text.empty()) {
			return text;
		}
		not_a_file = path + " is empty";
	} catch (std::exception &ex) {
		not_a_file = ErrorData(ex).RawMessage();
	}
	auto &fs = FileSystem::GetFileSystem(context);
	auto dir = path;
	while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) {
		dir.pop_back();
	}
	vector<string> files;
	try {
		for (auto &info : fs.Glob(dir + "/**/*")) {
			files.push_back(info.path);
		}
	} catch (std::exception &) {
		files.clear();
	}
	if (files.empty()) {
		bool folder = false;
		try {
			folder = fs.DirectoryExists(dir);
		} catch (std::exception &) {
		}
		if (!folder) {
			throw IOException("%s", not_a_file);
		}
	}
	std::sort(files.begin(), files.end());
	string out = "{";
	idx_t read = 0;
	for (auto &file : files) {
		string text;
		try {
			text = ReadFile(context, file);
		} catch (std::exception &) {
			// a folder the glob listed as a file
			continue;
		}
		auto key = StringUtil::StartsWith(file, dir) ? file.substr(dir.size() + 1) : file;
		out += (read++ ? ",\n" : "") + JsonString(key) + ": " + JsonString(text);
	}
	return out + "}";
}

//===--------------------------------------------------------------------===//
// OneLake: a Direct Lake view's lakehouses, each attached once through OneLake's Iceberg REST
// catalog under the name the glue gives it (EXTENSION.md, Where the data comes from). The token
// is the one of a secret DuckDB finds for the lakehouse, when that secret holds one.
//===--------------------------------------------------------------------===//
static constexpr const char *ONELAKE_CATALOG = "https://onelake.table.fabric.microsoft.com/iceberg";
//! How long a lakehouse's table metadata is kept between queries (MAX_TABLE_STALENESS). A framed
//! view reads its pinned snapshots, which no newer metadata changes; framing attaches afresh.
static constexpr const char *ONELAKE_STALENESS = "10 minutes";

// The value of `key` in the secret of `type` DuckDB finds for `path`; empty when there is none (or
// no such secret type, as azure in the browser).
static string SecretValue(ClientContext &context, const string &path, const string &type, const char *key) {
	try {
		auto &secrets = SecretManager::Get(context);
		auto match = secrets.LookupSecret(CatalogTransaction::GetSystemCatalogTransaction(context), path, type);
		if (!match.HasMatch()) {
			return "";
		}
		auto secret = dynamic_cast<const KeyValueSecret *>(&match.GetSecret());
		Value value;
		if (!secret || !secret->TryGetValue(key, value) || value.IsNull()) {
			return "";
		}
		return value.ToString();
	} catch (std::exception &) {
		return "";
	}
}

// A storage token from the Azure CLI, as a credential_chain secret's `cli` step gets it. A POC hack:
// the azure extension resolves the chain but keeps the token to itself, until duckdb-azure#191
// stores it in the secret.
static constexpr const char *AZ_STORAGE_TOKEN =
    "az account get-access-token --resource https://storage.azure.com --query accessToken -o tsv";

static string CliToken() {
#ifdef _WIN32
	auto command = string(AZ_STORAGE_TOKEN) + " 2>NUL";
	auto pipe = _popen(command.c_str(), "r");
#else
	auto command = string(AZ_STORAGE_TOKEN) + " 2>/dev/null";
	auto pipe = popen(command.c_str(), "r");
#endif
	if (!pipe) {
		return "";
	}
	string out;
	char buffer[4096];
	while (fgets(buffer, sizeof(buffer), pipe)) {
		out += buffer;
	}
#ifdef _WIN32
	_pclose(pipe);
#else
	pclose(pipe);
#endif
	StringUtil::Trim(out);
	return out;
}

// The chain of the credential_chain Azure secret DuckDB finds for `path`, lower case ("default" when
// it names none); empty when that secret is not a credential_chain one.
static string CredentialChain(ClientContext &context, const string &path) {
	try {
		auto &secrets = SecretManager::Get(context);
		auto match = secrets.LookupSecret(CatalogTransaction::GetSystemCatalogTransaction(context), path, "azure");
		if (!match.HasMatch() || match.GetSecret().GetProvider().GetIdentifierName() != "credential_chain") {
			return "";
		}
		auto chain = StringUtil::Lower(SecretValue(context, path, "azure", "chain"));
		return chain.empty() ? "default" : chain;
	} catch (std::exception &) {
		return "";
	}
}

static string Env(const char *name) {
	auto value = std::getenv(name);
	return value ? string(value) : string();
}

// The string value of `key` in a flat JSON object, as an OAuth token response has it.
static string JsonString(const string &json, const string &key) {
	auto at = json.find("\"" + key + "\"");
	if (at == string::npos) {
		return "";
	}
	auto start = json.find('"', json.find(':', at) + 1);
	auto end = start == string::npos ? string::npos : json.find('"', start + 1);
	return end == string::npos ? "" : json.substr(start + 1, end - start - 1);
}

// A storage token for a federated (OIDC) identity, as the chain's `workload_identity` step gets it,
// with no CLI. The JWT in AZURE_FEDERATED_TOKEN_FILE (GitHub Actions' OIDC token, a Kubernetes
// service account's) is exchanged with Entra for AZURE_CLIENT_ID in AZURE_TENANT_ID, through
// DuckDB's own HTTP client (httpfs's). A hack until duckdb-azure#191 stores the chain's token in
// the secret. `why` says what went wrong when there is no token.
static string WorkloadIdentityToken(ClientContext &context, string &why) {
	auto tenant = Env("AZURE_TENANT_ID");
	auto client = Env("AZURE_CLIENT_ID");
	auto file = Env("AZURE_FEDERATED_TOKEN_FILE");
	if (tenant.empty() || client.empty() || file.empty()) {
		why = "workload_identity needs AZURE_TENANT_ID, AZURE_CLIENT_ID and AZURE_FEDERATED_TOKEN_FILE";
		return "";
	}
	try {
		auto assertion = ReadFile(context, file);
		StringUtil::Trim(assertion);
		auto url = "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/token";
		string body = "client_id=" + client;
		body += "&scope=https%3A%2F%2Fstorage.azure.com%2F.default&grant_type=client_credentials";
		body += "&client_assertion_type=urn%3Aietf%3Aparams%3Aoauth%3Aclient-assertion-type%3Ajwt-bearer";
		body += "&client_assertion=" + assertion;
		ExtensionHelper::TryAutoLoadExtension(context, "httpfs");
		auto &http = HTTPUtil::Get(*context.db);
		auto params = http.InitializeParameters(context, url);
		HTTPHeaders headers;
		headers.Insert("Content-Type", "application/x-www-form-urlencoded");
		PostRequestInfo post(url, headers, *params, const_data_ptr_cast(body.c_str()), body.size());
		auto response = http.Request(post);
		auto &reply = post.buffer_out.empty() ? response->body : post.buffer_out;
		auto token = JsonString(reply, "access_token");
		if (token.empty()) {
			auto status = to_string(int(response->status));
			why = "workload_identity: Entra answered " + status + " " + JsonString(reply, "error_description");
		}
		return token;
	} catch (std::exception &ex) {
		why = string("workload_identity: ") + ex.what();
		return "";
	}
}

static string AzureToken(ClientContext &context, const string &workspace, const string &item, string &why) {
	auto abfss = "abfss://" + workspace + "@onelake.dfs.fabric.microsoft.com/" + item;
	auto token = SecretValue(context, abfss, "azure", "access_token");
	if (token.empty()) {
		// A credential_chain secret that keeps its token (REFRESH 'auto', duckdb-azure#191).
		token = SecretValue(context, abfss, "azure", "token");
	}
	// A credential_chain secret: its steps, in the chain's order as far as these two go.
	auto chain = CredentialChain(context, abfss);
	auto has = [&](const char *step) {
		return chain == "default" || chain.find(step) != string::npos;
	};
	if (token.empty() && has("workload_identity")) {
		token = WorkloadIdentityToken(context, why);
	}
	if (token.empty() && has("cli")) {
		token = CliToken();
	}
	if (token.empty() && why.empty()) {
		why = chain.empty() ? "no credential_chain azure secret for " + abfss : "chain '" + chain + "' gave none";
	}
	if (token.empty()) {
		// No azure extension (the browser): the bearer token of an http secret for OneLake.
		auto https = "https://onelake.dfs.fabric.microsoft.com/" + workspace + "/" + item;
		token = SecretValue(context, https, "http", "bearer_token");
	}
	return token;
}

// A GET of OneLake's blob endpoint with a storage token: the body, empty when it does not answer 200.
static string BlobGet(ClientContext &context, const string &url, const string &token) {
	try {
		ExtensionHelper::TryAutoLoadExtension(context, "httpfs");
		auto &http = HTTPUtil::Get(*context.db);
		auto params = http.InitializeParameters(context, url);
		HTTPHeaders headers;
		headers.Insert("Authorization", "Bearer " + token);
		headers.Insert("x-ms-version", "2023-11-03");
		string body;
		GetRequestInfo get(
		    url, headers, *params, [](const HTTPResponse &) { return true; },
		    [&](const_data_ptr_t data, idx_t length) {
			    body.append(const_char_ptr_cast(data), length);
			    return true;
		    });
		auto response = http.Request(get);
		if (!response || int(response->status) != 200) {
			return "";
		}
		return body.empty() ? response->body : body;
	} catch (std::exception &) {
		return "";
	}
}

// The text of each <tag>...</tag> in `xml`, XML's escapes undone.
static vector<string> XmlValues(const string &xml, const string &tag) {
	vector<string> values;
	auto open = "<" + tag + ">", close = "</" + tag + ">";
	for (auto at = xml.find(open); at != string::npos; at = xml.find(open, at)) {
		at += open.size();
		auto end = xml.find(close, at);
		if (end == string::npos) {
			break;
		}
		auto value = xml.substr(at, end - at);
		for (auto &e : vector<std::pair<string, string>> {
		         {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
			value = StringUtil::Replace(value, e.first, e.second);
		}
		values.push_back(value);
		at = end;
	}
	return values;
}

// Each listed <block> of a blob listing, every page of it: the block's <name> and its metadata's <id>.
static vector<std::pair<string, string>> BlobListing(ClientContext &context, const string &url, const string &token,
                                                     const string &block, const string &id) {
	vector<std::pair<string, string>> out;
	string marker;
	for (int page = 0; page < 100; page++) {
		auto xml = BlobGet(context, url + (marker.empty() ? "" : "&marker=" + StringUtil::URLEncode(marker)), token);
		if (xml.empty()) {
			break;
		}
		auto open = "<" + block + ">", close = "</" + block + ">";
		for (auto at = xml.find(open); at != string::npos; at = xml.find(open, at)) {
			auto end = xml.find(close, at);
			if (end == string::npos) {
				break;
			}
			auto part = xml.substr(at, end - at);
			auto names = XmlValues(part, "Name");
			auto ids = XmlValues(part, id);
			if (!names.empty() && !ids.empty()) {
				out.emplace_back(names[0], ids[0]);
			}
			at = end;
		}
		auto next = XmlValues(xml, "NextMarker");
		if (next.empty() || next[0].empty()) {
			break;
		}
		marker = next[0];
	}
	return out;
}

// A lakehouse's name in Fabric, from OneLake's blob listings with the storage token: the workspaces
// the identity sees (each one's name and GUID), then the workspace's items (each one's name and
// GUID). Kept for the session; empty when it is not found.
static string LakehouseName(ClientContext &context, DaxSqlState &state, const string &workspace, const string &item,
                            const string &token) {
	auto ws = StringUtil::Lower(workspace), id = StringUtil::Lower(item);
	{
		std::lock_guard<std::mutex> guard(state.lakes_lock);
		auto found = state.display.find(id);
		if (found != state.display.end()) {
			return found->second;
		}
	}
	if (token.empty()) {
		return "";
	}
	static constexpr const char *BLOB = "https://onelake.blob.fabric.microsoft.com/";
	string ws_name;
	{
		std::lock_guard<std::mutex> guard(state.lakes_lock);
		auto found = state.display.find(ws);
		if (found != state.display.end()) {
			ws_name = found->second;
		}
	}
	if (ws_name.empty()) {
		for (auto &w : BlobListing(context, string(BLOB) + "?comp=list&include=metadata", token, "Container",
		                           "WorkspaceObjectId")) {
			std::lock_guard<std::mutex> guard(state.lakes_lock);
			state.display[StringUtil::Lower(w.second)] = w.first;
			if (StringUtil::Lower(w.second) == ws) {
				ws_name = w.first;
			}
		}
	}
	string name;
	if (!ws_name.empty()) {
		auto url =
		    string(BLOB) + StringUtil::URLEncode(ws_name) + "?restype=container&comp=list&delimiter=/&include=metadata";
		for (auto &i : BlobListing(context, url, token, "BlobPrefix", "ArtifactId")) {
			// "nem.Lakehouse/": a lakehouse is its name; another kind keeps its kind ("x_warehouse")
			auto text = i.first;
			if (StringUtil::EndsWith(text, "/")) {
				text.pop_back();
			}
			auto dot = text.rfind('.');
			if (dot != string::npos) {
				auto kind = text.substr(dot + 1);
				text = text.substr(0, dot) + (kind == "Lakehouse" ? "" : "_" + StringUtil::Lower(kind));
			}
			std::lock_guard<std::mutex> guard(state.lakes_lock);
			state.display[StringUtil::Lower(i.second)] = text;
			if (StringUtil::Lower(i.second) == id) {
				name = text;
			}
		}
	}
	std::lock_guard<std::mutex> guard(state.lakes_lock);
	state.display[id] = name;
	return name;
}

// The lines after the first of the glue's define: "<catalog> <workspace>/<item>", one per lakehouse.
// The secret is looked up in `lookup`, a context in a transaction (the statement's), or else in a
// transaction of `con`'s own. Returns the first attach's error, empty when every lakehouse is attached.
//
// Each lakehouse is attached read-only (the extension never writes to it), its tables' metadata
// kept for ONELAKE_STALENESS between queries instead of asked of the catalog each time (the data
// files are cached by DuckDB's external file cache). It is attached afresh each time a view is
// framed (CREATE, a database opened, REFRESH): the snapshots read are the catalog's current ones,
// and the token a new one.
//
// The catalog is the lakehouse's name in Fabric (`nem`), so its tables are nem.schema.table; the
// GUID name the glue gives when the name is not found. A catalog of that name already in the
// session, and not the extension's, is the session's tables: they are read as they are, and
// nothing is attached. The glue is told which catalog each lakehouse is read from.
static string AttachOneLake(Connection &con, optional_ptr<ClientContext> lookup, const vector<string> &outcome,
                            DaxSqlState &state) {
	for (idx_t i = 1; i < outcome.size(); i++) {
		auto space = outcome[i].find(' ');
		auto slash = outcome[i].find('/');
		if (space == string::npos || slash == string::npos) {
			continue;
		}
		auto catalog = outcome[i].substr(0, space);
		auto location = outcome[i].substr(space + 1);
		auto workspace = outcome[i].substr(space + 1, slash - space - 1);
		auto item = outcome[i].substr(slash + 1);
		string token, why;
		if (lookup) {
			token = AzureToken(*lookup, workspace, item, why);
		} else {
			con.BeginTransaction();
			token = AzureToken(*con.context, workspace, item, why);
			con.Commit();
		}
		auto named = LakehouseName(*con.context, state, workspace, item, token);
		if (!named.empty()) {
			catalog = named;
		}
		state.js.Call("set_catalog", {item, catalog});
		auto taken = !QueryRows(con, "SELECT 1 FROM duckdb_databases() WHERE lower(database_name) = lower(" +
		                                 KeywordHelper::WriteQuoted(catalog) + ")")
		                  .empty();
		{
			std::lock_guard<std::mutex> guard(state.lakes_lock);
			if (taken && !state.attached.count(StringUtil::Lower(catalog))) {
				// the session's own: its tables are the view's
				continue;
			}
			state.attached.insert(StringUtil::Lower(catalog));
		}
		con.Query("DETACH DATABASE IF EXISTS " + KeywordHelper::WriteQuoted(catalog, '"'));
		string sql = "ATTACH " + KeywordHelper::WriteQuoted(location);
		sql += " AS " + KeywordHelper::WriteQuoted(catalog, '"');
		sql += " (TYPE iceberg, URI " + KeywordHelper::WriteQuoted(ONELAKE_CATALOG) + ", ACCESS_DELEGATION_MODE 'none'";
		sql += ", READ_ONLY, MAX_TABLE_STALENESS " + KeywordHelper::WriteQuoted(ONELAKE_STALENESS);
		if (!token.empty()) {
			sql += ", TOKEN " + KeywordHelper::WriteQuoted(token);
		}
		sql += ")";
		auto result = con.Query(sql);
		if (result->HasError()) {
			return result->GetError() + (token.empty() && !why.empty() ? " (no token: " + why + ")" : "");
		}
	}
	return "";
}

//===--------------------------------------------------------------------===//
// The frame (EXTENSION.md, Facts from the data): a Direct Lake view's tables pinned to the
// snapshots they are at now, and dax-sql told which relationships' many sides hold keys their one
// side lacks, and which text columns hold trailing spaces, at those snapshots (its factsCheck, one
// statement). The glue writes the statements; a step that fails leaves the
// view as the step before left it (unpinned, or pinned with no answers), and the reason is shown.
//===--------------------------------------------------------------------===//
// A statement's one row, its values tab-separated; the error when it fails or has not one row.
static string QueryOneRow(Connection &con, const string &sql, string &row) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		return result->GetError();
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() != 1) {
		return "not one row";
	}
	row.clear();
	for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
		if (c) {
			row += '\t';
		}
		row += chunk->GetValue(c, 0).ToString();
	}
	return "";
}

//! While it lives, the statements the extension runs are its own (`framing`)
struct Framing {
	Framing() : was(framing) {
		framing = true;
	}
	~Framing() {
		framing = was;
	}
	bool was;
};

static void FrameView(Connection &con, DaxSqlState &state, const string &name) {
	Framing guard;
	try {
		auto pins = state.js.Call("frame_sql", {name});
		if (pins.empty()) {
			return;
		}
		string row;
		auto error = QueryOneRow(con, pins, row);
		if (!error.empty()) {
			state.js.Call("frame_failed", {name, "pins", error});
			return;
		}
		auto at = Timestamp::ToString(Timestamp::GetCurrentTimestamp()) + " UTC";
		// The data at those snapshots: one statement for every fact (dax-sql's factsCheck)
		auto check = state.js.Call("frame", {name, row, at});
		if (check.empty()) {
			return;
		}
		error = QueryOneRow(con, check, row);
		if (!error.empty()) {
			state.js.Call("frame_failed", {name, "facts", error});
			return;
		}
		state.js.Call("facts_found", {name, row});
	} catch (std::exception &e) {
		// The view stays as the last step left it: today's SQL at worst.
		try {
			state.js.Call("frame_failed", {name, "pins", ErrorData(e).RawMessage()});
		} catch (...) {
		}
	}
}

// dax_sql_refresh(name): what REFRESH SEMANTIC VIEW is rewritten to. The view framed again on its
// tables' current snapshots; one row, the view and what SHOW SEMANTIC VIEWS says of its frame.
static unique_ptr<FunctionData> RefreshBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &state = *input.info->Cast<DaxSqlFunctionInfo>().state;
	auto parsed = Statement(state, input, "dax_sql_refresh", "refresh", 3);
	// the view by its full name; its rows say its own
	auto &name = parsed[1];
	{
		Connection con(*context.db);
		// The view's lakehouses attached afresh: their current snapshots, not the cached metadata
		auto catalogs = StringUtil::Split(state.js.Call("catalogs", {name}), '\n');
		auto error = AttachOneLake(con, &context, catalogs, state);
		if (!error.empty()) {
			state.js.Call("frame_failed", {name, "pins", error});
		} else {
			FrameView(con, state, name);
		}
	}
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"view_name", "data_as_of"};
	auto data = make_uniq<DefineData>();
	data->rows.push_back({Value(parsed[2]), Value(state.js.Call("data_as_of", {name}))});
	return std::move(data);
}

//! A view defined from its file, as dax_sql_define and dax_sql_create do it
struct DefinedView {
	string db, schema, name, path, text;
	bool created = false;
};

static vector<string> StringArgs(TableFunctionBindInput &input, const char *function) {
	vector<string> args;
	for (auto &v : input.inputs) {
		if (v.IsNull()) {
			throw InvalidInputException("%s: an argument is NULL", function);
		}
		args.push_back(v.type().id() == LogicalTypeId::BOOLEAN ? (BooleanValue::Get(v) ? "true" : "false")
		                                                       : StringValue::Get(v));
	}
	return args;
}

// Reads the file, defines the view in the glue (an error if the file does not compile), attaches
// its OneLake lakehouses and frames it. db and schema '' are the session's.
// A view's name in full, every part quoted, as the glue finds it among views of the same name.
static string FullName(const string &db, const string &schema, const string &name) {
	auto quote = [](const string &part) {
		return KeywordHelper::WriteQuoted(part, '"');
	};
	return quote(db) + "." + quote(schema) + "." + quote(name);
}

static DefinedView DefineView(ClientContext &context, DaxSqlState &state, const vector<string> &args) {
	DefinedView v;
	v.db = args[0];
	v.schema = args[1];
	v.name = args[2];
	v.path = args[3];
	auto &mode = args[4];
	if (v.db.empty()) {
		v.db = DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();
	}
	if (v.schema.empty()) {
		v.schema = ClientData::Get(context).catalog_search_path->GetDefault().GetSchema().GetIdentifierName();
	}
	if (v.schema.empty()) {
		v.schema = DEFAULT_SCHEMA;
	}
	v.text = ReadModel(context, v.path);
	auto outcome = StringUtil::Split(
	    state.js.Call("define", {v.db, v.schema, v.name, v.path, v.text, mode, state.Own(v.db)}), '\n');
	state.RefreshNames();
	if (outcome.size() > 1) {
		Connection con(*context.db);
		// Not attached: the view reads its tables from the search path (a copy of the lakehouse).
		auto error = AttachOneLake(con, &context, outcome, state);
		if (!error.empty()) {
			state.js.Call("localize", {FullName(v.db, v.schema, v.name), error});
		} else {
			FrameView(con, state, FullName(v.db, v.schema, v.name));
		}
	}
	v.created = !outcome.empty() && outcome[0] == "created";
	return v;
}

static string Quoted(const string &name) {
	return KeywordHelper::WriteQuoted(name, '"');
}

// A statement of the extension's own, in a connection of its own; its error is thrown.
static void Run(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		result->ThrowError();
	}
}

// The view's row in its schema's dax_sql_semantic_views, where a database file keeps it: the
// file's text, so the view comes back when the file is opened (RestoreViews).
static void StoreView(DatabaseInstance &db, const DefinedView &v) {
	Framing guard;
	Connection con(db);
	auto table = Quoted(v.db) + "." + Quoted(v.schema) + ".dax_sql_semantic_views";
	Run(con, "CREATE TABLE IF NOT EXISTS " + table +
	             " (view_name VARCHAR, path VARCHAR, definition VARCHAR, created_on TIMESTAMP)");
	Run(con, "DELETE FROM " + table + " WHERE lower(view_name) = lower(" + KeywordHelper::WriteQuoted(v.name) + ")");
	Run(con, "INSERT INTO " + table + " VALUES (" + Value(v.name).ToSQLString() + ", " + Value(v.path).ToSQLString() +
	             ", " + Value(v.text).ToSQLString() + ", current_timestamp::TIMESTAMP)");
}

// One of the extension's statements, as the grammar wrote it (its one argument), read by the glue
// (dax-sql's rewrite): its fields, the first its kind, which must be `kind`, with `size` fields.
static vector<string> Statement(DaxSqlState &state, TableFunctionBindInput &input, const char *function,
                                const string &kind, idx_t size) {
	auto args = StringArgs(input, function);
	if (args.size() != 1) {
		throw InvalidInputException("%s takes the statement's text", function);
	}
	auto line = state.js.Call("statement", {args[0]});
	vector<string> fields;
	if (kind == "sql") {
		// SHOW's SELECT may hold a tab: only the first one splits
		auto tab = line.find('\t');
		fields = {line.substr(0, tab), tab == string::npos ? "" : line.substr(tab + 1)};
	} else {
		// every field kept, an empty one too (CREATE's db and schema may be '')
		idx_t start = 0;
		for (idx_t i = 0; i <= line.size(); i++) {
			if (i == line.size() || line[i] == '\t') {
				fields.push_back(line.substr(start, i - start));
				start = i + 1;
			}
		}
	}
	if (fields.size() != size || fields[0] != kind) {
		throw InvalidInputException("%s: %s is not a %s statement", function, args[0], kind);
	}
	return fields;
}

// The view's stored row as it is now (existed false: none); db and schema '' are the session's.
static ViewUndo StoredView(ClientContext &context, const string &db, const string &schema, const string &name) {
	ViewUndo undo;
	undo.db = db.empty() ? DatabaseManager::GetDefaultDatabase(context).GetIdentifierName() : db;
	undo.schema = schema;
	if (undo.schema.empty()) {
		undo.schema = ClientData::Get(context).catalog_search_path->GetDefault().GetSchema().GetIdentifierName();
	}
	if (undo.schema.empty()) {
		undo.schema = DEFAULT_SCHEMA;
	}
	undo.name = name;
	Framing guard;
	Connection con(*context.db);
	auto rows = QueryRows(con, "SELECT view_name, path, definition FROM " + Quoted(undo.db) + "." +
	                               Quoted(undo.schema) + ".dax_sql_semantic_views WHERE lower(view_name) = lower(" +
	                               KeywordHelper::WriteQuoted(name) + ")");
	if (!rows.empty() && rows[0].size() == 3) {
		undo.existed = true;
		undo.name = rows[0][0];
		undo.path = rows[0][1];
		undo.text = rows[0][2];
	}
	return undo;
}

static void RecordChange(ClientContext &context, ViewUndo undo) {
	auto rebind = context.registered_state->Get<DaxSqlRebindState>(DaxSqlRebindState::KEY);
	if (rebind) {
		rebind->Record(std::move(undo));
	}
}

// The view put back as it was before a change its transaction rolled back: defined again from its
// stored text and stored again, or forgotten and its row deleted. A failure leaves it as it is.
static void UndoViewChange(ClientContext &context, DaxSqlState &state, const ViewUndo &undo) {
	try {
		Framing guard;
		Connection con(*context.db);
		if (undo.existed) {
			auto outcome = StringUtil::Split(state.js.Call("define", {undo.db, undo.schema, undo.name, undo.path,
			                                                          undo.text, "replace", state.Own(undo.db)}),
			                                 '\n');
			state.RefreshNames();
			if (outcome.size() > 1) {
				auto error = AttachOneLake(con, nullptr, outcome, state);
				if (!error.empty()) {
					state.js.Call("localize", {FullName(undo.db, undo.schema, undo.name), error});
				} else {
					FrameView(con, state, FullName(undo.db, undo.schema, undo.name));
				}
			}
			DefinedView v;
			v.db = undo.db;
			v.schema = undo.schema;
			v.name = undo.name;
			v.path = undo.path;
			v.text = undo.text;
			StoreView(*context.db, v);
		} else {
			state.js.Call("forget", {Quoted(undo.db) + "." + Quoted(undo.schema) + "." + Quoted(undo.name), "true"});
			state.RefreshNames();
			con.Query("DELETE FROM " + Quoted(undo.db) + "." + Quoted(undo.schema) +
			          ".dax_sql_semantic_views WHERE lower(view_name) = lower(" +
			          KeywordHelper::WriteQuoted(undo.name) + ")");
		}
	} catch (std::exception &) {
		// a rollback does not fail: the view stays as the statement left it
	}
}

// dax_sql_create(db, schema, name, path, mode): what CREATE SEMANTIC VIEW is (dax_sql_grammar.cpp).
// mode: 'create', 'replace' or 'if_not_exists'. One row, the view and its frame; none when IF NOT
// EXISTS finds it.
static unique_ptr<FunctionData> CreateBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &state = *input.info->Cast<DaxSqlFunctionInfo>().state;
	auto parsed = Statement(state, input, "dax_sql_create", "create", 6);
	vector<string> args(parsed.begin() + 1, parsed.end());
	auto before = StoredView(context, args[0], args[1], args[2]);
	auto v = DefineView(context, state, args);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"view_name", "data_as_of"};
	auto data = make_uniq<DefineData>();
	if (v.created) {
		// before the row is stored: a store that fails (a read-only database) rolls the view back too
		RecordChange(context, std::move(before));
		StoreView(*context.db, v);
		data->rows.push_back({Value(v.name), Value(state.js.Call("data_as_of", {FullName(v.db, v.schema, v.name)}))});
	}
	return std::move(data);
}

// dax_sql_drop(db, schema, name, if_exists): what DROP SEMANTIC VIEW is. The view forgotten and its
// stored row deleted; one row, the view, or none when IF EXISTS finds none.
static unique_ptr<FunctionData> DropBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &state = *input.info->Cast<DaxSqlFunctionInfo>().state;
	auto parsed = Statement(state, input, "dax_sql_drop", "drop", 3);
	auto dropped = state.js.Call("forget", {parsed[1], parsed[2]});
	state.RefreshNames();
	return_types = {LogicalType::VARCHAR};
	names = {"view_name"};
	auto data = make_uniq<DefineData>();
	if (!dropped.empty()) {
		auto parts = StringUtil::Split(dropped, '\t');
		if (parts.size() == 3) {
			RecordChange(context, StoredView(context, parts[0], parts[1], parts[2]));
			Framing guard;
			Connection con(*context.db);
			// a view kept nowhere (its database read-only, say) has no row to delete
			con.Query("DELETE FROM " + Quoted(parts[0]) + "." + Quoted(parts[1]) +
			          ".dax_sql_semantic_views WHERE lower(view_name) = lower(" + KeywordHelper::WriteQuoted(parts[2]) +
			          ")");
			data->rows.push_back({Value(parts[2])});
		}
	}
	return std::move(data);
}

// dax_sql_show(kind, view, metric): what SHOW SEMANTIC VIEWS / DIMENSIONS / METRICS is, as a
// statement or as a table. The glue lists the rows as a SELECT over VALUES, run here.
static unique_ptr<FunctionData> ShowBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &state = *input.info->Cast<DaxSqlFunctionInfo>().state;
	auto parsed = Statement(state, input, "dax_sql_show", "sql", 2);
	auto &sql = parsed[1];
	Framing guard;
	Connection con(*context.db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		result->ThrowError();
	}
	return_types = result->GetTypes();
	names = result->GetNames();
	auto data = make_uniq<DefineData>();
	auto &collection = result->Collection();
	for (auto &chunk : collection.Chunks()) {
		for (idx_t r = 0; r < chunk.size(); r++) {
			vector<Value> values;
			for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
				values.push_back(chunk.GetValue(c, r));
			}
			data->rows.push_back(std::move(values));
		}
	}
	return std::move(data);
}

// dax_sql_define(db, schema, name, path, mode): the view defined, and the row to store for it (none
// when IF NOT EXISTS finds it). What dax_sql_rewrite() shows CREATE SEMANTIC VIEW as.
static unique_ptr<FunctionData> DefineBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &state = *input.info->Cast<DaxSqlFunctionInfo>().state;
	auto v = DefineView(context, state, StringArgs(input, "dax_sql_define"));
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP};
	names = {"view_name", "path", "definition", "created_on"};
	auto data = make_uniq<DefineData>();
	if (v.created) {
		data->rows.push_back(
		    {Value(v.name), Value(v.path), Value(v.text), Value::TIMESTAMP(Timestamp::GetCurrentTimestamp())});
	}
	return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> DefineInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DefineState>();
}

static void DefineFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<DefineData>();
	auto &state = input.global_state->Cast<DefineState>();
	idx_t count = 0;
	while (state.offset < data.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = data.rows[state.offset++];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Load: the views stored in the databases attached now, then the grammar and the rebind hook.
//===--------------------------------------------------------------------===//
// The rows of a query, as text; none when it fails.
static vector<vector<string>> QueryRows(Connection &con, const string &sql) {
	vector<vector<string>> rows;
	auto result = con.Query(sql);
	if (result->HasError()) {
		return rows;
	}
	while (true) {
		auto chunk = result->FetchRaw();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		for (idx_t r = 0; r < chunk->size(); r++) {
			vector<string> row;
			for (idx_t c = 0; c < chunk->ColumnCount(); c++) {
				row.push_back(chunk->GetValue(c, r).ToString());
			}
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

static constexpr const char *STORED_TABLES =
    "SELECT database_name, schema_name FROM duckdb_tables() WHERE table_name = 'dax_sql_semantic_views'";

// The views stored in a database file (or in every database, with none named), defined again.
static void RestoreViews(DatabaseInstance &db, DaxSqlState &state, const string &database = "") {
	Framing guard;
	Connection con(db);
	auto stored = string(STORED_TABLES);
	if (!database.empty()) {
		// pushed down: only that database's catalog is read
		stored += " AND database_name = " + KeywordHelper::WriteQuoted(database);
	}
	for (auto &table : QueryRows(con, stored)) {
		auto &database = table[0];
		auto &schema = table[1];
		auto from = KeywordHelper::WriteQuoted(database, '"') + "." + KeywordHelper::WriteQuoted(schema, '"');
		auto views = QueryRows(con, "SELECT view_name, path, definition FROM " + from + ".dax_sql_semantic_views");
		for (auto &view : views) {
			auto outcome =
			    state.js.Call("define", {database, schema, view[0], view[1], view[2], "restore", state.Own(database)});
			// Not attached (no secret yet, no azure extension): the view reads the search path.
			auto lines = StringUtil::Split(outcome, '\n');
			auto error = AttachOneLake(con, nullptr, lines, state);
			if (!error.empty()) {
				state.js.Call("localize", {FullName(database, schema, view[0]), error});
			} else if (lines.size() > 1) {
				FrameView(con, state, FullName(database, schema, view[0]));
			}
		}
	}
	state.RefreshNames();
}

// The databases a view can be stored in: DuckDB's own, not the system's, the temporary one or the
// extension's OneLake catalogs.
static std::set<string> StoringDatabases(DatabaseManager &manager) {
	std::set<string> names;
	for (auto &attached : manager.GetDatabases()) {
		if (attached->IsSystem() || attached->IsTemporary()) {
			continue;
		}
		auto name = attached->GetName().GetIdentifierName();
		if (StringUtil::StartsWith(name, "dax_sql_onelake_") || attached->GetCatalog().GetCatalogType() != "duckdb") {
			continue;
		}
		names.insert(name);
	}
	return names;
}

// A database file attached since LOAD has its stored views defined; one detached, its views
// forgotten (#8). Called as a text is parsed and as a query begins: cheap when nothing changed.
static void CatchUpDatabases(ClientContext &context, DaxSqlState &state) {
	auto now = StoringDatabases(DatabaseManager::Get(context));
	vector<string> attached, detached;
	{
		std::lock_guard<std::mutex> guard(state.databases_lock);
		if (now == state.databases) {
			return;
		}
		for (auto &name : now) {
			if (!state.databases.count(name)) {
				attached.push_back(name);
			}
		}
		for (auto &name : state.databases) {
			if (!now.count(name)) {
				detached.push_back(name);
			}
		}
		state.databases = now;
	}
	for (auto &name : detached) {
		state.js.Call("forget_database", {name});
	}
	for (auto &name : attached) {
		try {
			RestoreViews(*context.db, state, name);
		} catch (std::exception &) {
			// a file whose stored views cannot be read is still the user's to query
		}
	}
	state.RefreshNames();
}

// dax_sql_rewrite(sql): the SQL a text stands for, its model queries compiled in place and the
// extension's statements as the SQL they are, without running it or changing a view; the text
// itself when nothing in it is the extension's. For the drift check and for reading.
static scalar_function_t RewriteFunction(shared_ptr<DaxSqlState> state) {
	return [state](DataChunk &args, ExpressionState &expr, Vector &result) {
		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t sql) {
			auto text = sql.GetString();
			auto out = state->js.Call("rewrite", {text, "dry"});
			return StringVector::AddString(result, out.empty() ? text : out);
		});
	};
}

// dax_sql_facts(view) and dax_sql_set_facts(view, json): a view's frame as JSON ({ pins, orphans }),
// and a frame given as JSON. For the CI checks only (.github/scripts/drift.py, facts.py): the
// drift check gives dax-sql in Node the extension's frame, and the facts check compares answers
// with and without the orphan answers at the same snapshots.
static scalar_function_t FactsFunction(shared_ptr<DaxSqlState> state) {
	return [state](DataChunk &args, ExpressionState &expr, Vector &result) {
		UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t name) {
			return StringVector::AddString(result, state->js.Call("facts", {name.GetString()}));
		});
	};
}

static scalar_function_t SetFactsFunction(shared_ptr<DaxSqlState> state) {
	return [state](DataChunk &args, ExpressionState &expr, Vector &result) {
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    args.data[0], args.data[1], result, args.size(), [&](string_t name, string_t json) {
			    state->js.Call("set_facts", {name.GetString(), json.GetString()});
			    return StringVector::AddString(result, state->js.Call("facts", {name.GetString()}));
		    });
	};
}

static void DaxSqlVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, DaxSqlExtension().Version());
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto state = make_shared_ptr<DaxSqlState>();

	loader.RegisterFunction(ScalarFunction("dax_sql_version", {}, LogicalType::VARCHAR, DaxSqlVersionFun));
	ScalarFunction rewrite("dax_sql_rewrite", {LogicalType::VARCHAR}, LogicalType::VARCHAR, RewriteFunction(state));
	loader.RegisterFunction(rewrite);
	TableFunction define(
	    "dax_sql_define",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	    DefineFunction, DefineBind, DefineInit);
	define.function_info = make_shared_ptr<DaxSqlFunctionInfo>(state);
	loader.RegisterFunction(define);
	TableFunction refresh("dax_sql_refresh", {LogicalType::VARCHAR}, DefineFunction, RefreshBind, DefineInit);
	refresh.function_info = make_shared_ptr<DaxSqlFunctionInfo>(state);
	loader.RegisterFunction(refresh);
	// CREATE, DROP and SHOW SEMANTIC: the statement's text (dax_sql_grammar.cpp), which the glue reads
	TableFunction create("dax_sql_create", {LogicalType::VARCHAR}, DefineFunction, CreateBind, DefineInit);
	create.function_info = make_shared_ptr<DaxSqlFunctionInfo>(state);
	loader.RegisterFunction(create);
	TableFunction drop("dax_sql_drop", {LogicalType::VARCHAR}, DefineFunction, DropBind, DefineInit);
	drop.function_info = make_shared_ptr<DaxSqlFunctionInfo>(state);
	loader.RegisterFunction(drop);
	TableFunction show("dax_sql_show", {LogicalType::VARCHAR}, DefineFunction, ShowBind, DefineInit);
	show.function_info = make_shared_ptr<DaxSqlFunctionInfo>(state);
	loader.RegisterFunction(show);
	ScalarFunction facts("dax_sql_facts", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FactsFunction(state));
	facts.SetVolatile();
	loader.RegisterFunction(facts);
	ScalarFunction set_facts("dax_sql_set_facts", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                         SetFactsFunction(state));
	set_facts.SetVolatile();
	loader.RegisterFunction(set_facts);

	state->databases = StoringDatabases(DatabaseManager::Get(db));
	for (auto &attached : DatabaseManager::Get(db).GetDatabases()) {
		if (attached->IsInitialDatabase()) {
			state->home = attached->GetName().GetIdentifierName();
		}
	}
	RestoreViews(db, *state);

	// The statements: grammar rules (dax_sql_grammar.cpp). The model queries: compiled when they fail
	// to bind, on every connection, those open now and those opened later.
	RegisterDaxSqlGrammar(db);
	ExtensionCallback::Register(DBConfig::GetConfig(db), make_shared_ptr<DaxSqlConnectionCallback>(state));
	for (auto &connection : db.GetConnectionManager().GetConnectionList()) {
		connection->registered_state->GetOrCreate<DaxSqlRebindState>(DaxSqlRebindState::KEY, state);
	}

	// ... and as they are parsed (DaxSqlParserOverride), with the parser override allowed unless the
	// user chose a setting of their own
	ParserExtension parser;
	parser.parser_override = DaxSqlParserOverride;
	parser.parser_info = make_shared_ptr<DaxSqlParserInfo>(state);
	ParserExtension::Register(DBConfig::GetConfig(db), std::move(parser));
	{
		Connection con(db);
		auto current = QueryRows(con, "SELECT current_setting('allow_parser_override_extension')::VARCHAR");
		if (current.size() == 1 && StringUtil::CIEquals(current[0][0], "default")) {
			con.Query("SET GLOBAL allow_parser_override_extension = 'fallback'");
		}
	}
}

void DaxSqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string DaxSqlExtension::Name() {
	return "dax_sql";
}

std::string DaxSqlExtension::Version() const {
#ifdef EXT_VERSION_DAX_SQL
	return EXT_VERSION_DAX_SQL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dax_sql, loader) {
	duckdb::LoadInternal(loader);
}
}
