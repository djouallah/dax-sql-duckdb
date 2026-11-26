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
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "js_engine.hpp"
#include "onelake_fs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace duckdb {

// The extension's state for one database: the compiler and glue in QuickJS, and the views' names
// (lower case), so a statement that names none skips the JS.
struct DaxSqlState {
	JsEngine js;
	std::mutex names_lock;
	vector<string> names;

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

struct DaxSqlParserInfo : public ParserExtensionInfo {
	explicit DaxSqlParserInfo(shared_ptr<DaxSqlState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<DaxSqlState> state;
};

struct DaxSqlFunctionInfo : public TableFunctionInfo {
	explicit DaxSqlFunctionInfo(shared_ptr<DaxSqlState> state_p) : state(std::move(state_p)) {
	}
	shared_ptr<DaxSqlState> state;
};

//===--------------------------------------------------------------------===//
// The parser override: a statement that names a semantic view, or says SEMANTIC, goes through
// the glue's rewrite (dax-sql's compileStatement for model queries), and DuckDB parses what
// comes back. Any other statement is left to DuckDB's parser.
//===--------------------------------------------------------------------===//
// Set while the extension runs a view's frame statements (FrameView): they are its own SQL, and
// are left to DuckDB's parser.
static thread_local bool framing = false;

static ParserOverrideResult DaxSqlParserOverride(ParserExtensionInfo *info, const string &query,
                                                 ParserOptions &options) {
	auto &state = *info->Cast<DaxSqlParserInfo>().state;
	if (framing || !state.MightBeOurs(query)) {
		return ParserOverrideResult();
	}
	auto sql = state.js.Call("rewrite", {query});
	if (sql.empty()) {
		return ParserOverrideResult();
	}
	// DROP SEMANTIC VIEW forgets the view as it is rewritten.
	state.RefreshNames();
	auto inner = options;
	inner.parser_override_setting = AllowParserOverride::DEFAULT_OVERRIDE;
	Parser parser(inner);
	parser.ParseQuery(sql);
	return ParserOverrideResult(std::move(parser.statements));
}

//===--------------------------------------------------------------------===//
// dax_sql_define(db, schema, name, path, mode): what CREATE SEMANTIC VIEW is rewritten to ('' for
// the session's database or schema). It reads the file, defines the view in the glue (an error if
// the file does not compile), attaches its OneLake lakehouses, and returns the row stored in the
// schema's dax_sql_semantic_views: none when IF NOT EXISTS finds the view there.
//===--------------------------------------------------------------------===//
struct DefineData : public TableFunctionData {
	vector<Value> row;
	bool emit = false;
};

struct DefineState : public GlobalTableFunctionState {
	bool done = false;
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

// A model's text, as the glue reads it: a file's own text, or for a folder of TMDL files (a PBIP's
// definition/, a .SemanticModel folder) one JSON object, { "<file>": "<text>", ... }. A path that
// does not end in a model file's extension is globbed for **/*.tmdl first; with none found it is
// read as a file, and the glue says what it takes.
static string ReadModel(ClientContext &context, const string &path) {
	auto lower = StringUtil::Lower(path);
	for (auto ext : {".bim", ".tmdl", ".yaml", ".yml"}) {
		if (StringUtil::EndsWith(lower, ext)) {
			return ReadFile(context, path);
		}
	}
	auto &fs = FileSystem::GetFileSystem(context);
	auto dir = path;
	while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) {
		dir.pop_back();
	}
	vector<string> files;
	try {
		for (auto &info : fs.Glob(dir + "/**/*.tmdl")) {
			files.push_back(info.path);
		}
	} catch (std::exception &) {
		files.clear();
	}
	if (files.empty()) {
		return ReadFile(context, path);
	}
	std::sort(files.begin(), files.end());
	string out = "{";
	for (idx_t i = 0; i < files.size(); i++) {
		auto key = StringUtil::StartsWith(files[i], dir) ? files[i].substr(dir.size() + 1) : files[i];
		out += (i ? ",\n" : "") + JsonString(key) + ": " + JsonString(ReadFile(context, files[i]));
	}
	return out + "}";
}

//===--------------------------------------------------------------------===//
// OneLake: a Direct Lake view's lakehouses, each attached once through OneLake's Iceberg REST
// catalog under the name the glue gives it (EXTENSION.md, Where the data comes from). The token
// is the one of a secret DuckDB finds for the lakehouse, when that secret holds one.
//===--------------------------------------------------------------------===//
static constexpr const char *ONELAKE_CATALOG = "https://onelake.table.fabric.microsoft.com/iceberg";

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
// stores it in the secret. Native builds only (the browser has no CLI).
static constexpr const char *AZ_STORAGE_TOKEN =
    "az account get-access-token --resource https://storage.azure.com --query accessToken -o tsv";

static string CliToken() {
#ifdef __EMSCRIPTEN__
	return "";
#else
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
#endif
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

// The lines after the first of the glue's define: "<catalog> <workspace>/<item>", one per lakehouse.
// The secret is looked up in `lookup`, a context in a transaction (the statement's), or else in a
// transaction of `con`'s own. Returns the first attach's error, empty when every lakehouse is attached.
static string AttachOneLake(Connection &con, optional_ptr<ClientContext> lookup, const vector<string> &outcome) {
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
		string sql = "ATTACH IF NOT EXISTS " + KeywordHelper::WriteQuoted(location);
		sql += " AS " + KeywordHelper::WriteQuoted(catalog, '"');
		sql += " (TYPE iceberg, URI " + KeywordHelper::WriteQuoted(ONELAKE_CATALOG) + ", ACCESS_DELEGATION_MODE 'none'";
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
// side lacks at those snapshots. The glue writes both statements; a step that fails leaves the
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

static void FrameView(Connection &con, DaxSqlState &state, const string &name) {
	struct Framing {
		Framing() {
			framing = true;
		}
		~Framing() {
			framing = false;
		}
	} guard;
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
		auto check = state.js.Call("frame", {name, row, at});
		if (check.empty()) {
			return;
		}
		error = QueryOneRow(con, check, row);
		if (!error.empty()) {
			state.js.Call("frame_failed", {name, "orphans", error});
			return;
		}
		state.js.Call("orphans_found", {name, row});
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
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("dax_sql_refresh: the view's name is NULL");
	}
	auto name = StringValue::Get(input.inputs[0]);
	{
		Connection con(*context.db);
		FrameView(con, state, name);
	}
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"view_name", "data_as_of"};
	auto data = make_uniq<DefineData>();
	data->emit = true;
	data->row = {Value(name), Value(state.js.Call("data_as_of", {name}))};
	return std::move(data);
}

static unique_ptr<FunctionData> DefineBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &state = *input.info->Cast<DaxSqlFunctionInfo>().state;
	vector<string> args;
	for (auto &v : input.inputs) {
		if (v.IsNull()) {
			throw InvalidInputException("dax_sql_define: an argument is NULL");
		}
		args.push_back(StringValue::Get(v));
	}
	auto db = args[0];
	auto schema = args[1];
	auto &name = args[2];
	// Not given: the session's database and schema.
	if (db.empty()) {
		db = DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();
	}
	if (schema.empty()) {
		schema = ClientData::Get(context).catalog_search_path->GetDefault().GetSchema().GetIdentifierName();
	}
	if (schema.empty()) {
		schema = DEFAULT_SCHEMA;
	}
	auto &path = args[3];
	auto &mode = args[4];
	auto text = ReadModel(context, path);
	auto outcome = StringUtil::Split(state.js.Call("define", {db, schema, name, path, text, mode}), '\n');
	state.RefreshNames();
	if (outcome.size() > 1) {
		Connection con(*context.db);
		// Not attached: the view reads its tables from the search path (a copy of the lakehouse).
		auto error = AttachOneLake(con, &context, outcome);
		if (!error.empty()) {
			state.js.Call("localize", {name, error});
		} else {
			FrameView(con, state, name);
		}
	}

	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP};
	names = {"view_name", "path", "definition", "created_on"};
	auto data = make_uniq<DefineData>();
	data->emit = !outcome.empty() && outcome[0] == "created";
	data->row = {Value(name), Value(path), Value(text), Value::TIMESTAMP(Timestamp::GetCurrentTimestamp())};
	return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> DefineInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DefineState>();
}

static void DefineFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<DefineData>();
	auto &state = input.global_state->Cast<DefineState>();
	if (state.done || !data.emit) {
		return;
	}
	for (idx_t c = 0; c < data.row.size(); c++) {
		output.SetValue(c, 0, data.row[c]);
	}
	output.SetCardinality(1);
	state.done = true;
}

//===--------------------------------------------------------------------===//
// Load: the views stored in the databases attached now, then the parser override.
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

static void RestoreViews(DatabaseInstance &db, DaxSqlState &state) {
	Connection con(db);
	for (auto &table : QueryRows(con, STORED_TABLES)) {
		auto &database = table[0];
		auto &schema = table[1];
		auto from = KeywordHelper::WriteQuoted(database, '"') + "." + KeywordHelper::WriteQuoted(schema, '"');
		auto views = QueryRows(con, "SELECT view_name, path, definition FROM " + from + ".dax_sql_semantic_views");
		for (auto &view : views) {
			auto outcome = state.js.Call("define", {database, schema, view[0], view[1], view[2], "restore"});
			// Not attached (no secret yet, no azure extension): the view reads the search path.
			auto lines = StringUtil::Split(outcome, '\n');
			auto error = AttachOneLake(con, nullptr, lines);
			if (!error.empty()) {
				state.js.Call("localize", {view[0], error});
			} else if (lines.size() > 1) {
				FrameView(con, state, view[0]);
			}
		}
	}
	state.RefreshNames();
}

// dax_sql_rewrite(sql): the SQL DuckDB runs for a text, as the parser override rewrites it, without
// running it or changing a view; the text itself when nothing in it is the extension's.
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
	ScalarFunction facts("dax_sql_facts", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FactsFunction(state));
	facts.SetVolatile();
	loader.RegisterFunction(facts);
	ScalarFunction set_facts("dax_sql_set_facts", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                         SetFactsFunction(state));
	set_facts.SetVolatile();
	loader.RegisterFunction(set_facts);

#ifdef __EMSCRIPTEN__
	// The browser has no azure extension: OneLake's abfss:// files are read over https (httpfs).
	db.GetFileSystem().RegisterSubSystem(make_uniq<OneLakeHttpFileSystem>(db));
#endif

	RestoreViews(db, *state);

	auto &config = DBConfig::GetConfig(db);
	ParserExtension parser;
	parser.parser_override = DaxSqlParserOverride;
	parser.parser_info = make_shared_ptr<DaxSqlParserInfo>(state);
	ParserExtension::Register(config, parser);
	config.SetOptionByName("allow_parser_override_extension", Value("FALLBACK"));
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
