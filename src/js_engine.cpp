#include "js_engine.hpp"

#include "dax_sql_js_modules.hpp"
#include "duckdb/common/exception.hpp"
#include "quickjs.h"

#include <cstring>
#include <exception>
#include <functional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace duckdb {

// The JS runs on a thread of its own, with a stack of this size: the thread a query is planned on
// can have 1 MB (Windows' main thread), less than compiling a large model takes. The JS may use
// half of it, so a deeper query is a JS error, not a crash.
static constexpr size_t THREAD_STACK_SIZE = 16 * 1024 * 1024;
static constexpr size_t JS_STACK_SIZE = THREAD_STACK_SIZE / 2;

// Runs work on a new thread with a THREAD_STACK_SIZE stack and waits for it; what it throws is
// thrown here.
static void RunOnJsThread(const std::function<void()> &work) {
	struct Job {
		const std::function<void()> *work;
		std::exception_ptr error;
		static void Run(Job &job) {
			try {
				(*job.work)();
			} catch (...) {
				job.error = std::current_exception();
			}
		}
	};
	Job job;
	job.work = &work;
#ifdef _WIN32
	auto entry = [](void *arg) -> unsigned {
		Job::Run(*static_cast<Job *>(arg));
		return 0;
	};
	auto handle = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, static_cast<unsigned>(THREAD_STACK_SIZE), entry,
	                                                      &job, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
	if (!handle) {
		throw InternalException("dax_sql: could not start the JS thread");
	}
	WaitForSingleObject(handle, INFINITE);
	CloseHandle(handle);
#else
	auto entry = [](void *arg) -> void * {
		Job::Run(*static_cast<Job *>(arg));
		return nullptr;
	};
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
	pthread_t thread;
	auto started = pthread_create(&thread, &attr, entry, &job);
	pthread_attr_destroy(&attr);
	if (started != 0) {
		throw InternalException("dax_sql: could not start the JS thread");
	}
	pthread_join(thread, nullptr);
#endif
	if (job.error) {
		std::rethrow_exception(job.error);
	}
}

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
	RunOnJsThread([&]() { Start(); });
}

void JsEngine::Start() {
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
	string text;
	RunOnJsThread([&]() { text = CallHere(fn, args); });
	return text;
}

string JsEngine::CallHere(const char *fn, const vector<string> &args) {
	// Each call is on a thread of its own: the stack limit is from this one's top.
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
