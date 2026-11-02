#include "graphics/host_gpu/metal/metalPassTimer.h"

#include "graphics/host_gpu/metal/metalContext.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Metal {
namespace {

constexpr NSUInteger kSamplesPerBuffer = 4096; // two per pass

struct PassSlot {
	std::string label;
	uint32_t    items = 0;
};

// Samples of one command buffer: a pooled counter sample buffer and the labels of its passes (slot i = samples 2i, 2i+1).
struct CbSamples {
	id<MTLCounterSampleBuffer> buffer = nil;
	std::vector<PassSlot>      slots;
};

struct Stats {
	uint64_t ticks = 0;
	uint64_t passes = 0;
	uint64_t items = 0;
};

std::mutex                                      g_mutex;
std::vector<id<MTLCounterSampleBuffer>>         g_pool;      // free sample buffers
size_t                                          g_created = 0;
id<MTLCommandBuffer>                            g_cmd = nil; // command buffer the open samples belong to
std::unique_ptr<CbSamples>                      g_open;
std::unordered_map<std::string, Stats>          g_stats;
uint64_t                                        g_total_ticks = 0;
std::chrono::steady_clock::time_point           g_last_report {};
double                                          g_ns_per_tick = 0.0;

id<MTLCounterSet> TimestampSet(id<MTLDevice> device) {
	for (id<MTLCounterSet> set in device.counterSets) {
		if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) return set;
	}
	return nil;
}

// GPU ticks to nanoseconds from two (CPU, GPU) timestamp pairs taken some time apart.
void CalibrateLocked(id<MTLDevice> device) {
	static MTLTimestamp s_cpu0 = 0, s_gpu0 = 0;
	MTLTimestamp cpu = 0, gpu = 0;
	[device sampleTimestamps:&cpu gpuTimestamp:&gpu];
	if (s_cpu0 == 0) {
		s_cpu0 = cpu;
		s_gpu0 = gpu;
		return;
	}
	if (gpu <= s_gpu0 || cpu <= s_cpu0) return;
	mach_timebase_info_data_t tb {};
	mach_timebase_info(&tb);
	const double cpu_ns = static_cast<double>(cpu - s_cpu0) * tb.numer / tb.denom;
	g_ns_per_tick = cpu_ns / static_cast<double>(gpu - s_gpu0);
}

// The open sample set for `cmd` (a different command buffer drops the old set: it was committed elsewhere).
CbSamples* OpenLocked(id<MTLCommandBuffer> cmd) {
	if (g_cmd != cmd) {
		if (g_open && g_open->buffer != nil) g_pool.push_back(g_open->buffer);
		g_open.reset();
		g_cmd = cmd;
	}
	if (!g_open) {
		id<MTLCounterSampleBuffer> buffer = nil;
		if (!g_pool.empty()) {
			buffer = g_pool.back();
			g_pool.pop_back();
		} else if (g_created < 64) {
			id<MTLDevice> device = MetalContext::Instance().GetDevice();
			id<MTLCounterSet> set = TimestampSet(device);
			if (set == nil || ![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return nullptr;
			MTLCounterSampleBufferDescriptor* d = [[MTLCounterSampleBufferDescriptor alloc] init];
			d.counterSet = set;
			d.storageMode = MTLStorageModeShared;
			d.sampleCount = kSamplesPerBuffer;
			NSError* error = nil;
			buffer = [device newCounterSampleBufferWithDescriptor:d error:&error];
			if (buffer == nil) return nullptr;
			++g_created;
		}
		if (buffer == nil) return nullptr;
		g_open = std::make_unique<CbSamples>();
		g_open->buffer = buffer;
	}
	if (g_open->slots.size() * 2 + 2 > kSamplesPerBuffer) return nullptr;
	return g_open.get();
}

void ReportLocked() {
	const auto now = std::chrono::steady_clock::now();
	if (now - g_last_report < std::chrono::seconds(3) || g_total_ticks == 0) return;
	g_last_report = now;
	CalibrateLocked(MetalContext::Instance().GetDevice());
	std::vector<std::pair<std::string, Stats>> rows(g_stats.begin(), g_stats.end());
	std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
	uint64_t passes = 0;
	for (const auto& [l, s]: rows) passes += s.passes;
	std::fprintf(stderr, "[GpuPass] %.1f ms GPU in %llu passes (%zu kinds) since last report; top:\n",
	             g_ns_per_tick > 0.0 ? g_total_ticks * g_ns_per_tick / 1e6 : 0.0, static_cast<unsigned long long>(passes), rows.size());
	for (size_t i = 0; i < rows.size() && i < 15; ++i) {
		const auto& [label, s] = rows[i];
		std::fprintf(stderr, "[GpuPass]   %5.1f%% %8.2f ms  passes=%-5llu items=%-6llu %s\n", 100.0 * s.ticks / g_total_ticks,
		             g_ns_per_tick > 0.0 ? s.ticks * g_ns_per_tick / 1e6 : 0.0, static_cast<unsigned long long>(s.passes),
		             static_cast<unsigned long long>(s.items), label.c_str());
	}
	g_stats.clear();
	g_total_ticks = 0;
}

} // namespace

bool PassTimerEnabled() {
	static const bool s_on = std::getenv("KYTY_GPU_PASS_TIME") != nullptr;
	return s_on;
}

void PassTimerAttachRender(id<MTLCommandBuffer> cmd, MTLRenderPassDescriptor* desc) {
	if (!PassTimerEnabled() || cmd == nil || desc == nil) return;
	std::lock_guard lock(g_mutex);
	CbSamples* s = OpenLocked(cmd);
	if (s == nullptr) return;
	const NSUInteger i = s->slots.size() * 2;
	MTLRenderPassSampleBufferAttachmentDescriptor* a = desc.sampleBufferAttachments[0];
	a.sampleBuffer = s->buffer;
	a.startOfVertexSampleIndex = i;
	a.endOfVertexSampleIndex = MTLCounterDontSample;
	a.startOfFragmentSampleIndex = MTLCounterDontSample;
	a.endOfFragmentSampleIndex = i + 1;
	s->slots.push_back({"render", 0});
}

MTLComputePassDescriptor* PassTimerComputeDescriptor(id<MTLCommandBuffer> cmd) {
	if (!PassTimerEnabled() || cmd == nil) return nil;
	std::lock_guard lock(g_mutex);
	CbSamples* s = OpenLocked(cmd);
	if (s == nullptr) return nil;
	const NSUInteger i = s->slots.size() * 2;
	MTLComputePassDescriptor* d = [MTLComputePassDescriptor computePassDescriptor];
	d.sampleBufferAttachments[0].sampleBuffer = s->buffer;
	d.sampleBufferAttachments[0].startOfEncoderSampleIndex = i;
	d.sampleBufferAttachments[0].endOfEncoderSampleIndex = i + 1;
	s->slots.push_back({"compute", 0});
	return d;
}

std::string g_current;

void PassTimerNote(const char* label) {
	if (!PassTimerEnabled() || label == nullptr) return;
	std::lock_guard lock(g_mutex);
	g_current = label;
}

void PassTimerItem() {
	if (!PassTimerEnabled()) return;
	std::lock_guard lock(g_mutex);
	if (!g_open || g_open->slots.empty()) return;
	auto& slot = g_open->slots.back();
	if (slot.items++ == 0 && !g_current.empty()) slot.label = (slot.label == "render" ? "R " : "C ") + g_current;
}

void PassTimerOnCommit(id<MTLCommandBuffer> cmd) {
	if (!PassTimerEnabled() || cmd == nil) return;
	std::shared_ptr<CbSamples> samples;
	{
		std::lock_guard lock(g_mutex);
		if (g_cmd != cmd || !g_open) return;
		samples = std::shared_ptr<CbSamples>(g_open.release());
		g_cmd = nil;
	}
	if (samples->slots.empty()) {
		std::lock_guard lock(g_mutex);
		g_pool.push_back(samples->buffer);
		return;
	}
	[cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
		NSData* data = [samples->buffer resolveCounterRange:NSMakeRange(0, samples->slots.size() * 2)];
		std::lock_guard lock(g_mutex);
		if (data != nil && data.length >= samples->slots.size() * 2 * sizeof(MTLCounterResultTimestamp)) {
			const auto* ts = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
			for (size_t k = 0; k < samples->slots.size(); ++k) {
				const uint64_t a = ts[2 * k].timestamp, b = ts[2 * k + 1].timestamp;
				if (a == MTLCounterErrorValue || b == MTLCounterErrorValue || b <= a) continue;
				auto& st = g_stats[samples->slots[k].label];
				st.ticks += b - a;
				st.passes += 1;
				st.items += samples->slots[k].items;
				g_total_ticks += b - a;
			}
		}
		g_pool.push_back(samples->buffer);
		ReportLocked();
	}];
}

} // namespace Libs::Graphics::Metal
