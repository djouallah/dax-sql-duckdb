#include "js_engine.hpp"

#include "dax_sql_js_modules.hpp"
#include "duckdb/common/exception.hpp"
#include "quickjs.h"

#include <cstring>

namespace duckdb {

// The JS stack the compiler may use. It stays well inside the smallest thread stack a query runs
// on (1 MB on Windows), so a deep query is a JS error, not a crash.
static constexpr size_t JS_STACK_SIZE = 512 * 1024;

static const dax_sql_js::Module *FindModule(const char *name) {
	for (size_t i = 0; i < dax_sql_js::MODULE_COUNT; i++) {
		if (strcmp(dax_sql_js::MODULES[i].name, name) == 0) {
			return &dax_sql_js::MODULES[i];
		}
	}
	return nullptr;
}

// The modules are the ones embedded at build time (cmake/embed_js.cmake), by their path:
// glue.js, dax-sql/src/..., dax-sql/formats/..., yaml/dist/...
static JSModuleDef *LoadModule(JSContext *ctx, const char *name, void *opaque) {
	auto module = FindModule(name);
	if (!module) {
		JS_ThrowReferenceError(ctx, "module %s is not part of the dax_sql extension", name);
		return nullptr;
	}
	auto compiled = JS_Eval(ctx, reinterpret_cast<const char *>(module->data), module->size, name,
	                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	if (JS_IsException(compiled)) {
		return nullptr;
	}
	auto def = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
	JS_FreeValue(ctx, compiled);
	return def;
}

static string ToString(JSContext *ctx, JSValueConst value) {
	size_t len;
	auto text = JS_ToCStringLen(ctx, &len, value);
	if (!text) {
		return "(no message)";
	}
	string result(text, len);
	JS_FreeCString(ctx, text);
	return result;
}

// The pending exception, as the DuckDB exception its code names.
[[noreturn]] static void ThrowPending(JSContext *ctx) {
	auto error = JS_GetException(ctx);
	string message;
	string code;
	if (JS_IsError(error)) {
		auto m = JS_GetPropertyStr(ctx, error, "message");
		message = ToString(ctx, m);
		JS_FreeValue(ctx, m);
		auto c = JS_GetPropertyStr(ctx, error, "code");
		if (JS_IsString(c)) {
			code = ToString(ctx, c);
		}
		JS_FreeValue(ctx, c);
	} else {
		message = ToString(ctx, error);
	}
	JS_FreeValue(ctx, error);
	if (code == "SYNTAX") {
		throw ParserException(message);
	}
	if (code == "SEMANTIC") {
		throw BinderException(message);
	}
	if (code == "UNSUPPORTED") {
		throw NotImplementedException(message);
	}
	if (code == "CATALOG") {
		throw CatalogException(message);
	}
	throw InvalidInputException(message);
}

JsEngine::JsEngine() {
	rt = JS_NewRuntime();
	JS_SetMaxStackSize(rt, JS_STACK_SIZE);
	JS_SetModuleLoaderFunc(rt, nullptr, LoadModule, nullptr);
	ctx = JS_NewContext(rt);
	auto glue = FindModule("glue.js");
	if (!glue) {
		throw InternalException("dax_sql: glue.js is not embedded");
	}
	auto evaluated =
	    JS_Eval(ctx, reinterpret_cast<const char *>(glue->data), glue->size, "glue.js", JS_EVAL_TYPE_MODULE);
	if (JS_IsException(evaluated)) {
		ThrowPending(ctx);
	}
	// A module's evaluation is a promise: settle it.
	JSContext *job_ctx;
	while (JS_ExecutePendingJob(rt, &job_ctx) > 0) {
	}
	if (JS_IsPromise(evaluated) && JS_PromiseState(ctx, evaluated) == JS_PROMISE_REJECTED) {
		auto reason = JS_PromiseResult(ctx, evaluated);
		JS_FreeValue(ctx, evaluated);
		JS_Throw(ctx, reason);
		ThrowPending(ctx);
	}
	JS_FreeValue(ctx, evaluated);
}

JsEngine::~JsEngine() {
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
}

string JsEngine::Call(const char *fn, const vector<string> &args) {
	std::lock_guard<std::mutex> guard(lock);
	// The thread can differ from one call to the next: the stack limit is from this one's top.
	JS_UpdateStackTop(rt);
	auto global = JS_GetGlobalObject(ctx);
	auto api = JS_GetPropertyStr(ctx, global, "dax_sql");
	auto function = JS_GetPropertyStr(ctx, api, fn);
	vector<JSValue> argv;
	for (auto &arg : args) {
		argv.push_back(JS_NewStringLen(ctx, arg.c_str(), arg.size()));
	}
	auto result = JS_Call(ctx, function, api, static_cast<int>(argv.size()), argv.data());
	for (auto &arg : argv) {
		JS_FreeValue(ctx, arg);
	}
	JS_FreeValue(ctx, function);
	JS_FreeValue(ctx, api);
	JS_FreeValue(ctx, global);
	if (JS_IsException(result)) {
		ThrowPending(ctx);
	}
	auto text = ToString(ctx, result);
	JS_FreeValue(ctx, result);
	return text;
}

} // namespace duckdb
