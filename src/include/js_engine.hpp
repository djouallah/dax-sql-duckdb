#pragma once

#include "duckdb.hpp"

#include <mutex>

struct JSRuntime;
struct JSContext;

namespace duckdb {

//! dax-sql and the glue (js/glue.js) running in QuickJS: one runtime per database, used by one
//! thread at a time.
class JsEngine {
public:
	JsEngine();
	~JsEngine();

	//! Calls globalThis.dax_sql[fn] with string arguments and returns its result as a string. A JS
	//! error is thrown as the DuckDB exception its code names (SYNTAX, SEMANTIC, UNSUPPORTED,
	//! CATALOG; any other is invalid input), keeping its message.
	string Call(const char *fn, const vector<string> &args);

private:
	std::mutex lock;
	JSRuntime *rt;
	JSContext *ctx;
};

} // namespace duckdb
