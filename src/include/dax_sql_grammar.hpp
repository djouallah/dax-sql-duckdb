//===----------------------------------------------------------------------===//
// dax_sql: the extension's statements as a grammar extension of DuckDB's PEG parser
//
//   CREATE [OR REPLACE] SEMANTIC VIEW [IF NOT EXISTS] [db.][schema.]v FROM 'path'
//   DROP SEMANTIC VIEW [IF EXISTS] v
//   REFRESH SEMANTIC VIEW v
//   SHOW SEMANTIC VIEWS | DIMENSIONS IN v [FOR METRIC m] | METRICS IN v
//   (SHOW SEMANTIC ...) [alias], wherever a table can be
//
// Each statement is lowered to CALL dax_sql_create / dax_sql_drop / dax_sql_refresh /
// dax_sql_show(...), the extension's table functions: a CALL, not a SELECT, so that a client
// runs it at once and keeps its rows (the Python client wraps a SELECT's text in a subquery).
// (SHOW SEMANTIC ...) as a table is the table function itself. Model queries are plain SQL and
// need no grammar: they are compiled as they are parsed, or when they fail to bind
// (dax_sql_extension.cpp, DaxSqlParserOverride and DaxSqlRebindState).
//
// The grammar plumbing (transforms, activation on every connection, the CLI's 'shell' grammar)
// follows altertable-ai/duckdb-semantic_layer's src/parser/semantic_layer_grammar.cpp
// (MIT License, Copyright (c) 2026 Altertable; THIRD_PARTY_NOTICES.md).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;

class DaxSqlGrammarExtension final : public GrammarExtension {
public:
	static constexpr const char *NAME = "dax_sql";

	DaxSqlGrammarExtension();

	vector<GrammarChange> GetChanges() const override;

	//! Adds this extension to the connection's active grammar extensions
	void Activate(ClientContext &context);

private:
	shared_ptr<CompiledGrammar> GetCompiledGrammar(ClientContext &context, const vector<string> &active);

	mutex grammar_lock;
	case_insensitive_map_t<shared_ptr<CompiledGrammar>> compiled_grammars;
};

//! Registers the grammar, and activates it on every connection: those opened later at once,
//! those open now (the one running LOAD among them) when their current query ends.
void RegisterDaxSqlGrammar(DatabaseInstance &db);

} // namespace duckdb
