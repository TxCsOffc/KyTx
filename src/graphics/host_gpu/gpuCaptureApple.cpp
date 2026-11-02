#include "graphics/host_gpu/gpuCaptureApple.h"

#if defined(__APPLE__)

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <system_error>

// Debug aid: record whole frames as a Metal .gputrace for inspection in Xcode.
// Requires MTL_CAPTURE_ENABLED=1 before Metal initialises (main.cpp sets it when
// KYTY_GPU_CAPTURE=1). Creating _TempData/gpu_capture starts a capture at the next present;
// it stops after the requested number of presents (file contents, default 2).

namespace Libs::Graphics {
namespace {

using Id  = void*;
using Sel = void*;

struct ObjcApi {
	Id (*get_class)(const char*)     = nullptr;
	Sel (*register_sel)(const char*) = nullptr;
	void* msg_send                   = nullptr;
	Id (*create_device)()            = nullptr;
	bool ok                          = false;
};

const ObjcApi& Api() {
	static const ObjcApi api = [] {
		ObjcApi a;
		a.get_class     = reinterpret_cast<Id (*)(const char*)>(dlsym(RTLD_DEFAULT, "objc_getClass"));
		a.register_sel  = reinterpret_cast<Sel (*)(const char*)>(dlsym(RTLD_DEFAULT, "sel_registerName"));
		a.msg_send      = dlsym(RTLD_DEFAULT, "objc_msgSend");
		a.create_device = reinterpret_cast<Id (*)()>(dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"));
		a.ok = a.get_class && a.register_sel && a.msg_send && a.create_device;
		return a;
	}();
	return api;
}

template <typename R = Id, typename... Args>
R Send(Id object, const char* selector, Args... args) {
	const auto& api = Api();
	auto*       fn  = reinterpret_cast<R (*)(Id, Sel, Args...)>(api.msg_send);
	return fn(object, api.register_sel(selector), args...);
}

int  g_frames_left = 0;
bool g_active      = false;

void Start(int frames) {
	const auto& api = Api();
	if (!api.ok) {
		std::fprintf(stderr, "[gpucapture] Objective-C/Metal symbols not found\n");
		return;
	}
	Id device  = api.create_device();
	Id manager = Send(api.get_class("MTLCaptureManager"), "sharedCaptureManager");
	if (device == nullptr || manager == nullptr) {
		std::fprintf(stderr, "[gpucapture] no device or capture manager\n");
		return;
	}
	constexpr long destination_gputrace = 2; // MTLCaptureDestinationGPUTraceDocument
	if (!Send<bool>(manager, "supportsDestination:", destination_gputrace)) {
		std::fprintf(stderr, "[gpucapture] .gputrace destination unsupported (set MTL_CAPTURE_ENABLED=1)\n");
		return;
	}
	char name[128];
	std::snprintf(name, sizeof(name), "_TempData/fc27-%ld.gputrace", static_cast<long>(std::time(nullptr)));
	const auto path    = std::filesystem::absolute(name).string();
	Id         ns_path = Send(api.get_class("NSString"), "stringWithUTF8String:", path.c_str());
	Id         url     = Send(api.get_class("NSURL"), "fileURLWithPath:", ns_path);
	Id         desc    = Send(Send(api.get_class("MTLCaptureDescriptor"), "alloc"), "init");
	Send<void>(desc, "setCaptureObject:", device);
	Send<void>(desc, "setDestination:", destination_gputrace);
	Send<void>(desc, "setOutputURL:", url);
	Id error = nullptr;
	if (!Send<bool>(manager, "startCaptureWithDescriptor:error:", desc, &error)) {
		Id          description = error != nullptr ? Send(error, "localizedDescription") : nullptr;
		const char* text = description != nullptr ? Send<const char*>(description, "UTF8String") : "?";
		std::fprintf(stderr, "[gpucapture] start failed: %s\n", text);
		return;
	}
	g_active      = true;
	g_frames_left = frames;
	std::fprintf(stderr, "[gpucapture] capturing %d frame(s) to %s\n", frames, path.c_str());
	std::fflush(stderr);
}

void Stop() {
	const auto& api     = Api();
	Id          manager = Send(api.get_class("MTLCaptureManager"), "sharedCaptureManager");
	Send<void>(manager, "stopCapture");
	g_active = false;
	std::fprintf(stderr, "[gpucapture] capture finished\n");
	std::fflush(stderr);
}

} // namespace

void GpuCaptureOnPresent() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_CAPTURE");
		return value != nullptr && value[0] == '1';
	}();
	if (!enabled) {
		return;
	}
	if (g_active) {
		if (--g_frames_left <= 0) {
			Stop();
		}
		return;
	}
	const std::filesystem::path trigger = "_TempData/gpu_capture";
	std::error_code             ec;
	if (!std::filesystem::exists(trigger, ec)) {
		return;
	}
	int frames = 2;
	if (FILE* f = std::fopen(trigger.c_str(), "r"); f != nullptr) {
		int value = 0;
		if (std::fscanf(f, "%d", &value) == 1 && value > 0 && value < 100) {
			frames = value;
		}
		std::fclose(f);
	}
	std::filesystem::remove(trigger, ec);
	Start(frames);
}

} // namespace Libs::Graphics

#endif
