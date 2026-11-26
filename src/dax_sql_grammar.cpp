#include "dax_sql_grammar.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// Grammar
//===--------------------------------------------------------------------===//
// One PEG rule per entry, reusing DuckDB's own (QualifiedName, ColId, StringLiteral, IfExists,
// IfNotExists, TableAlias, Parens). The words SEMANTIC, VIEWS, METRICS, ... are literals, not
// keywords, so they stay usable as names. Every rule is prefixed DaxSql, so the grammar can be
// active beside another extension's.
const char *const DAX_SQL_RULES[] = {
    "DaxSqlStatement <- DaxSqlCreateStmt / DaxSqlDropStmt / DaxSqlRefreshStmt / DaxSqlShowStmt",

    // 'CREATE' ('OR' 'REPLACE')? 'SEMANTIC' 'VIEW' IfNotExists? QualifiedName 'FROM' DaxSqlString
    "DaxSqlCreateStmt <- 'CREATE' DaxSqlOrReplace? 'SEMANTIC' 'VIEW' IfNotExists? QualifiedName 'FROM' "
    "DaxSqlString",
    "DaxSqlOrReplace <- 'OR' 'REPLACE'",
    // adjacent string literals are one string, as in DuckDB's SQL
    "DaxSqlString <- StringLiteral+",

    "DaxSqlDropStmt <- 'DROP' 'SEMANTIC' 'VIEW' IfExists? QualifiedName",

    "DaxSqlRefreshStmt <- 'REFRESH' 'SEMANTIC' 'VIEW' QualifiedName",

    "DaxSqlShowStmt <- 'SHOW' 'SEMANTIC' DaxSqlShowTarget",
    "DaxSqlShowTarget <- DaxSqlShowViews / DaxSqlShowMembers",
    "DaxSqlShowViews <- 'VIEWS'",
    "DaxSqlShowMembers <- DaxSqlMemberKind 'IN' QualifiedName DaxSqlForMetric?",
    "DaxSqlMemberKind <- 'DIMENSIONS' / 'METRICS'",
    "DaxSqlForMetric <- 'FOR' 'METRIC' ColId",

    // (SHOW SEMANTIC ...) where a table can be
    "DaxSqlShowTableRef <- Parens(DaxSqlShowStmt) TableAlias?",
};

//===--------------------------------------------------------------------===//
// Parse result helpers: a rule's children by their position in its sequence
//===--------------------------------------------------------------------===//
ListParseResult &List(ParseResult &parse_result) {
	return parse_result.Cast<ListParseResult>();
}

//! The alternative matched by a rule whose body is an ordered choice
ParseResult &Choice(ParseResult &parse_result) {
	if (parse_result.type == ParseResultType::CHOICE) {
		return parse_result.Cast<ChoiceParseResult>().GetResult();
	}
	return List(parse_result).Child<ChoiceParseResult>(0).GetResult();
}

bool Present(ListParseResult &list, idx_t index) {
	return list.Child<OptionalParseResult>(index).HasResult();
}

ParseResult &Optional(ListParseResult &list, idx_t index) {
	return list.Child<OptionalParseResult>(index).GetResult();
}

//! The elements of an `X+` child
vector<reference<ParseResult>> Repeated(ParseResult &parse_result) {
	optional_ptr<ParseResult> current = &parse_result;
	if (current->type == ParseResultType::LIST) {
		auto &list = current->Cast<ListParseResult>();
		if (list.GetChildren().size() == 1 && list.GetChild(0).type == ParseResultType::REPEAT) {
			current = &list.GetChild(0);
		}
	}
	vector<reference<ParseResult>> result;
	for (auto &child : current->Cast<RepeatParseResult>().GetChildren()) {
		result.push_back(child);
	}
	return result;
}

bool Is(ParseResult &parse_result, const char *rule_name) {
	return StringUtil::CIEquals(parse_result.Name(), rule_name);
}

//! A QualifiedName child, as the table functions take it: database, schema ('' when not given), name
vector<Value> NameValues(PEGTransformer &transformer, ListParseResult &list, idx_t index) {
	auto name = transformer.Transform<QualifiedName>(list, index);
	if (name.Path().size() > 3) {
		throw ParserException("a semantic view is named [database.][schema.]name, not \"%s\"", name.ToString());
	}
	return {Value(name.Catalog().GetIdentifierName()), Value(name.Schema().GetIdentifierName()),
	        Value(name.Name().GetIdentifierName())};
}

//! A call of one of the extension's table functions
unique_ptr<TableRef> Call(const string &function, vector<Value> args) {
	vector<unique_ptr<ParsedExpression>> children;
	for (auto &arg : args) {
		children.push_back(ConstantExpression::FromValue(std::move(arg)));
	}
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>(Identifier(function), std::move(children));
	return std::move(ref);
}

//! SELECT * FROM <ref>
unique_ptr<SQLStatement> SelectAll(unique_ptr<TableRef> ref) {
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(ref);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	return std::move(statement);
}

//===--------------------------------------------------------------------===//
// Transforms
//===--------------------------------------------------------------------===//
// StringLiteral+
string TransformDaxSqlString(PEGTransformer &transformer, ParseResult &parse_result) {
	string result;
	for (auto &literal : Repeated(List(parse_result).GetChild(0))) {
		result += transformer.Transform<string>(literal.get());
	}
	return result;
}

// 'CREATE' DaxSqlOrReplace? 'SEMANTIC' 'VIEW' IfNotExists? QualifiedName 'FROM' DaxSqlString
unique_ptr<SQLStatement> TransformDaxSqlCreateStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto or_replace = Present(list, 1);
	auto if_not_exists = Present(list, 4);
	if (or_replace && if_not_exists) {
		throw ParserException("CREATE OR REPLACE SEMANTIC VIEW cannot be combined with IF NOT EXISTS");
	}
	auto args = NameValues(transformer, list, 5);
	args.push_back(Value(transformer.Transform<string>(list, 7)));
	args.push_back(Value(or_replace ? "replace" : if_not_exists ? "if_not_exists" : "create"));
	return SelectAll(Call("dax_sql_create", std::move(args)));
}

// 'DROP' 'SEMANTIC' 'VIEW' IfExists? QualifiedName
unique_ptr<SQLStatement> TransformDaxSqlDropStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto args = NameValues(transformer, list, 4);
	args.push_back(Value::BOOLEAN(Present(list, 3)));
	return SelectAll(Call("dax_sql_drop", std::move(args)));
}

// 'REFRESH' 'SEMANTIC' 'VIEW' QualifiedName
unique_ptr<SQLStatement> TransformDaxSqlRefreshStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	auto args = NameValues(transformer, List(parse_result), 3);
	return SelectAll(Call("dax_sql_refresh", {args[2]}));
}

// 'SHOW' 'SEMANTIC' (DaxSqlShowViews / DaxSqlShowMembers): dax_sql_show(kind, view, metric)
unique_ptr<TableRef> ShowCall(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &target = Choice(List(parse_result).GetChild(2));
	if (Is(target, "DaxSqlShowViews")) {
		return Call("dax_sql_show", {Value("views"), Value(""), Value("")});
	}
	// DaxSqlMemberKind 'IN' QualifiedName DaxSqlForMetric?
	auto &list = List(target);
	auto kind = StringUtil::Lower(Choice(list.GetChild(0)).Cast<KeywordParseResult>().keyword);
	auto view = NameValues(transformer, list, 2)[2];
	string metric;
	if (Present(list, 3)) {
		if (kind != "dimensions") {
			throw ParserException("FOR METRIC goes with SHOW SEMANTIC DIMENSIONS");
		}
		metric = transformer.Transform<string>(List(Optional(list, 3)), 2);
	}
	return Call("dax_sql_show", {Value(kind), view, Value(metric)});
}

unique_ptr<SQLStatement> TransformDaxSqlShowStmt(PEGTransformer &transformer, ParseResult &parse_result) {
	return SelectAll(ShowCall(transformer, parse_result));
}

unique_ptr<SQLStatement> TransformDaxSqlStatement(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.Transform<unique_ptr<SQLStatement>>(Choice(parse_result));
}

// Parens(DaxSqlShowStmt) TableAlias?
unique_ptr<TableRef> TransformDaxSqlShowTableRef(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = List(parse_result);
	auto result = ShowCall(transformer, PEGTransformerFactory::ExtractResultFromParens(list.GetChild(0)));
	if (Present(list, 1)) {
		auto alias = transformer.Transform<TableAlias>(Optional(list, 1));
		result->alias = alias.name;
		result->column_name_alias = alias.column_name_alias;
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Transform plumbing
//===--------------------------------------------------------------------===//
template <class T>
using rule_transform_t = T (*)(PEGTransformer &transformer, ParseResult &parse_result);

template <class T>
grammar_transform_process_function_t MakeTransform(rule_transform_t<T> transform) {
	return [transform](PEGTransformer &transformer, ParseResult &parse_result) -> arena_ptr<TransformProcess> {
		return transformer.MakeProcess<FinalizeTransformProcess>(
		    transformer, parse_result,
		    [transform](PEGTransformer &inner_transformer,
		                ParseResult &inner_result) -> arena_ptr<TransformResultValue> {
			    return inner_transformer.MakeResult<T>(transform(inner_transformer, inner_result));
		    });
	};
}

template <class T>
void SetTransform(vector<GrammarChange> &changes, const char *rule_name, rule_transform_t<T> transform) {
	changes.push_back(GrammarChange::SetTransformProcess(rule_name, MakeTransform<T>(transform)));
}

//===--------------------------------------------------------------------===//
// Activation
//===--------------------------------------------------------------------===//
//! The DuckDB CLI runs `SET active_grammar_extensions=['shell']` as the first query of every
//! connection it opens, replacing a grammar activated when the connection opened: add it back
//! after that query. Any other list is the user's choice and is left alone.
class DaxSqlShellGrammarRestore final : public ClientContextState {
public:
	static constexpr const char *KEY = "dax_sql_shell_grammar_restore";

	explicit DaxSqlShellGrammarRestore(shared_ptr<DaxSqlGrammarExtension> grammar_p) : grammar(std::move(grammar_p)) {
	}

	void QueryEnd(ClientContext &context) override {
		auto &active = ClientConfig::GetConfig(context).active_grammar_extensions;
		if (active.size() == 1 && StringUtil::CIEquals(active[0], "shell")) {
			grammar->Activate(context);
		}
		context.registered_state->Remove(KEY);
	}

private:
	shared_ptr<DaxSqlGrammarExtension> grammar;
};

//! Connections opened after LOAD parse with the grammar from the start
class DaxSqlGrammarActivation final : public ExtensionCallback {
public:
	explicit DaxSqlGrammarActivation(shared_ptr<DaxSqlGrammarExtension> grammar_p) : grammar(std::move(grammar_p)) {
	}

	void OnConnectionOpened(ClientContext &context) override {
		grammar->Activate(context);
		context.registered_state->GetOrCreate<DaxSqlShellGrammarRestore>(DaxSqlShellGrammarRestore::KEY, grammar);
	}

private:
	shared_ptr<DaxSqlGrammarExtension> grammar;
};

//! Connections already open (the one running LOAD among them) switch on their own thread once
//! their current query ends, since another thread may be parsing with their grammar now. So
//! `LOAD dax_sql; CREATE SEMANTIC VIEW ...` sent as one string does not parse: send them apart.
class DaxSqlPendingGrammarActivation final : public ClientContextState {
public:
	static constexpr const char *KEY = "dax_sql_pending_grammar_activation";

	explicit DaxSqlPendingGrammarActivation(shared_ptr<DaxSqlGrammarExtension> grammar_p)
	    : grammar(std::move(grammar_p)) {
	}

	void QueryEnd(ClientContext &context) override {
		grammar->Activate(context);
		// once: a later `SET active_grammar_extensions` by the user must stick
		context.registered_state->Remove(KEY);
	}

private:
	shared_ptr<DaxSqlGrammarExtension> grammar;
};

} // namespace

DaxSqlGrammarExtension::DaxSqlGrammarExtension()
    : GrammarExtension(NAME, "CREATE / DROP / REFRESH / SHOW SEMANTIC VIEW, and (SHOW SEMANTIC ...) as a table") {
}

vector<GrammarChange> DaxSqlGrammarExtension::GetChanges() const {
	vector<GrammarChange> changes;
	for (auto rule : DAX_SQL_RULES) {
		changes.push_back(GrammarChange::AddRule(rule));
	}
	// ahead of DuckDB's own statements and table references, which would otherwise take a prefix
	// (DROP VIEW, SHOW <table>, a parenthesized subquery)
	changes.push_back(GrammarChange::PrependChoice("Statement", "DaxSqlStatement"));
	changes.push_back(GrammarChange::PrependChoice("InnerTableRef", "DaxSqlShowTableRef"));

	SetTransform(changes, "DaxSqlStatement", TransformDaxSqlStatement);
	SetTransform(changes, "DaxSqlCreateStmt", TransformDaxSqlCreateStmt);
	SetTransform(changes, "DaxSqlDropStmt", TransformDaxSqlDropStmt);
	SetTransform(changes, "DaxSqlRefreshStmt", TransformDaxSqlRefreshStmt);
	SetTransform(changes, "DaxSqlShowStmt", TransformDaxSqlShowStmt);
	SetTransform(changes, "DaxSqlShowTableRef", TransformDaxSqlShowTableRef);
	SetTransform(changes, "DaxSqlString", TransformDaxSqlString);
	return changes;
}

shared_ptr<CompiledGrammar> DaxSqlGrammarExtension::GetCompiledGrammar(ClientContext &context,
                                                                       const vector<string> &active) {
	auto key = StringUtil::Join(active, ",");
	lock_guard<mutex> guard(grammar_lock);
	auto &compiled = compiled_grammars[key];
	if (!compiled) {
		if (active.size() == 1) {
			vector<reference<GrammarExtension>> extensions;
			extensions.emplace_back(*this);
			compiled = CompiledGrammar::Create(extensions);
		} else {
			compiled = CompiledGrammar::Create(context, active);
		}
	}
	return compiled;
}

void DaxSqlGrammarExtension::Activate(ClientContext &context) {
	auto &config = ClientConfig::GetConfig(context);
	auto &active = config.active_grammar_extensions;
	for (auto &name : active) {
		if (StringUtil::CIEquals(name, NAME)) {
			return;
		}
	}
	active.emplace_back(NAME);
	// the same as appending to `SET active_grammar_extensions`
	config.cached_grammar = GetCompiledGrammar(context, active);
}

void RegisterDaxSqlGrammar(DatabaseInstance &db) {
	auto grammar = make_shared_ptr<DaxSqlGrammarExtension>();
	GrammarExtension::Register(db, grammar);
	ExtensionCallback::Register(DBConfig::GetConfig(db), make_shared_ptr<DaxSqlGrammarActivation>(grammar));
	for (auto &connection : db.GetConnectionManager().GetConnectionList()) {
		connection->registered_state->GetOrCreate<DaxSqlPendingGrammarActivation>(DaxSqlPendingGrammarActivation::KEY,
		                                                                          grammar);
	}
}

} // namespace duckdb
