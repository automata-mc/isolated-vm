#include "isolate/node_wrapper.h"
#include "isolate/util.h"
#include "isolate/environment.h"
#include "isolate/node_wrapper.h"
#include "isolate/platform_delegate.h"
#include "context_handle.h"
#include "external_copy_handle.h"
#include "isolate_handle.h"
#include "lib_handle.h"
#include "native_module_handle.h"
#include "reference_handle.h"
#include "script_handle.h"

#include <memory>
#include <mutex>

using namespace v8;

namespace ivm {

/**
 * The whole library is transferable so you can Inception the library into your isolates.
 */
class LibraryHandle : public TransferableHandle {
	private:
		class LibraryHandleTransferable : public Transferable {
			public:
				Local<Value> TransferIn() final {
					return LibraryHandle::Get();
				}
		};

	public:
		static Local<FunctionTemplate> Definition() {
			return Inherit<TransferableHandle>(MakeClass(
				"isolated_vm", nullptr,
				"Context", ClassHandle::GetFunctionTemplate<ContextHandle>(),
				"ExternalCopy", ClassHandle::GetFunctionTemplate<ExternalCopyHandle>(),
				"Isolate", ClassHandle::GetFunctionTemplate<IsolateHandle>(),
				"NativeModule", ClassHandle::GetFunctionTemplate<NativeModuleHandle>(),
				"Reference", ClassHandle::GetFunctionTemplate<ReferenceHandle>(),
				"Script", ClassHandle::GetFunctionTemplate<ScriptHandle>()
			));
		}

		std::unique_ptr<Transferable> TransferOut() final {
			return std::make_unique<LibraryHandleTransferable>();
		}

		static Local<Object> Get() {
			Local<Object> library = ClassHandle::NewInstance<LibraryHandle>().As<Object>();
			Unmaybe(library->Set(Isolate::GetCurrent()->GetCurrentContext(), v8_symbol("lib"), ClassHandle::NewInstance<LibHandle>()));
			return library;
		}
};

// Module entry point
std::atomic<bool> did_global_init{false};
std::mutex default_isolates_mutex;
std::unordered_map<v8::Isolate*, std::shared_ptr<IsolateHolder>> default_isolates;
extern "C"
void init(Local<Object> target) {
	// Create default isolate env
	Isolate* isolate = Isolate::GetCurrent();
	Local<Context> context = isolate->GetCurrentContext();
	// Maybe this would happen if you include the module from `vm`?
	assert(default_isolates.find(isolate) == default_isolates.end());
	{
		std::lock_guard<std::mutex> lock{default_isolates_mutex};
		default_isolates.insert(std::make_pair(isolate, IsolateEnvironment::New(isolate, context)));
	}
	Unmaybe(target->Set(context, v8_symbol("ivm"), LibraryHandle::Get()));
	auto platform = node::GetMainThreadMultiIsolatePlatform();
	assert(platform != nullptr);
	platform->AddIsolateFinishedCallback(isolate, [](void* param) {
		auto it = [&]() {
			std::lock_guard<std::mutex> lock{default_isolates_mutex};
			return default_isolates.find(static_cast<v8::Isolate*>(param));
		}();
		it->second->ReleaseAndJoin();
		std::lock_guard<std::mutex> lock{default_isolates_mutex};
		default_isolates.erase(it);
	}, isolate);

	if (!did_global_init.exchange(true)) {
		// These flags will override limits set through code. Since the main node isolate is already
		// created we can reset these so they won't affect the isolates we make.
		int argc = 4;
		const char* flags[] = {
			"--max-semi-space-size", "0",
			"--max-old-space-size", "0"
		};
		V8::SetFlagsFromCommandLine(&argc, const_cast<char**>(flags), false);

		PlatformDelegate::InitializeDelegate();
	}
}

} // namespace ivm

NODE_MODULE_CONTEXT_AWARE(isolated_vm, ivm::init) // NOLINT
