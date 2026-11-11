#pragma once

#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>
#include "graphics/presentation/traceWindow.h"

// Encoder fences (default on; KYTY_ENCODER_FENCES=0 turns them off): the encoders of the shared command stream run one after the other on
// the GPU, like the PS5 command processor runs its packets. Guest memory is reached through several Metal objects at once (a buffer per start page, buffer-backed texture views, raw GPU
// addresses inside argument buffers); Metal only orders encoders that use the same object, so a producer kernel and the kernel that reads
// its output can overlap (observed: indirect dispatch arguments read before the producer wrote them; a render pass over a linear render target
// writing into the memory of a compute queue buffer while the kernel still read it - the GPU hang of cs=0xf5875e8c tile queue, its per-tile
// counts turned into the target's half floats 0x38003800, um136 vs codex145-147). Each encoder updates a fence when it
// ends and the next one waits for it before its first command. No CPU wait.
//
// Call OrderBegin right after the encoder is created and OrderEnd right before endEncoding, for every encoder recorded into the shared
// command buffers (the helper buffers that commit and wait on their own need no fences).
namespace Libs::Graphics::Metal {

inline bool EncoderFencesEnabled() {
	static const bool on = std::getenv("KYTY_ENCODER_FENCES") == nullptr || std::getenv("KYTY_ENCODER_FENCES")[0] != '0';
	return on;
}

// KYTY_CS_OVERLAP=1: compute dispatches whose guest memory accesses do not conflict run at the same time, as the PS5 runs independent
// compute work. The strict chain above is kept for everything else. A dispatch declares the guest address ranges it reads and writes
// (OrderAccess); consecutive dispatches that touch disjoint memory (no write against a read or a write, at 16 KiB page granularity) form a
// group: they wait for the encoders before the group, not for each other, and whatever follows waits for all of them. A dispatch that reaches
// guest memory through raw pointers (the BDA page table) or through GDS is declared "unknown" and is never overlapped.
struct OrderAccess {
	uint64_t begin = 0;
	uint64_t end   = 0;
	bool     write = false;
};

// KYTY_CS_OVERLAP=1 turns it on, =toggle alternates 600 presented frames on / 600 off inside one run and logs the frame rate of every
// phase ([CsOverlapPhase]): the scene of an idle run changes from run to run, only the phases of one run are comparable.
inline bool CsOverlapEnabled() {
	static const int mode = [] {
		const char* v = std::getenv("KYTY_CS_OVERLAP");
		if (v == nullptr || v[0] == '0') return 0;
		return std::string_view(v) == "toggle" ? 2 : 1;
	}();
	if (mode != 2) return mode == 1;
	struct Phase {
		std::mutex mutex;
		uint64_t   index = ~0ull, start_flip = 0;
		std::chrono::steady_clock::time_point start;
	};
	static Phase ph;
	const uint64_t flip  = Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed);
	const uint64_t index = flip / 600;
	if (index != ph.index) {
		std::lock_guard<std::mutex> lock(ph.mutex);
		if (index != ph.index) {
			const auto now = std::chrono::steady_clock::now();
			if (ph.index != ~0ull) {
				const double secs = std::chrono::duration<double>(now - ph.start).count();
				std::fprintf(stderr, "[CsOverlapPhase] mode=%s flips=%llu seconds=%.1f fps=%.1f\n", (ph.index & 1) ? "ON " : "OFF",
				             (unsigned long long)(flip - ph.start_flip), secs, secs > 0 ? (flip - ph.start_flip) / secs : 0.0);
			}
			ph.index = index; ph.start_flip = flip; ph.start = now;
		}
	}
	return (index & 1) != 0;
}

namespace OrderDetail {
constexpr unsigned kMaxGroup = 8;
enum class Kind { None, Full, Leader, Member };
struct State {
	std::mutex                  mutex;
	std::vector<id<MTLFence>>   frontier;      // the encoder that begins next waits for all of these
	__strong id<MTLFence>       ring[64] = {};
	unsigned                    next = 0;
	bool                        group_open = false;
	std::vector<id<MTLFence>>   group_base;    // the frontier when the group was opened: members wait for these, not for each other
	std::vector<OrderAccess>    group_access;
	unsigned                    group_size = 0;
	Kind                        current = Kind::None;
	std::atomic<uint64_t>       stat_dispatches {0}, stat_overlapped {0}, stat_closed_conflict {0}, stat_closed_unknown {0};
};
inline State& Get() {
	static State s;
	return s;
}
constexpr uint64_t kPage = 16384;
inline bool Conflicts(const std::vector<OrderAccess>& group, const OrderAccess* acc, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		const uint64_t b = acc[i].begin & ~(kPage - 1), e = (acc[i].end + kPage - 1) & ~(kPage - 1);
		for (const OrderAccess& g: group) {
			if (!acc[i].write && !g.write) continue;
			const uint64_t gb = g.begin & ~(kPage - 1), ge = (g.end + kPage - 1) & ~(kPage - 1);
			if (b < ge && gb < e) return true;
		}
	}
	return false;
}
inline void WaitAll(id<MTLComputeCommandEncoder> e, const std::vector<id<MTLFence>>& fences) {
	for (id<MTLFence> f: fences) [e waitForFence:f];
}
inline id<MTLFence> NextFenceLocked(State& s, id<MTLDevice> device) {
	__strong id<MTLFence>& slot = s.ring[s.next++ % 64];
	if (slot == nil) slot = [device newFence];
	return slot;
}
} // namespace OrderDetail

// The strict chain: wait for everything recorded so far.
inline void OrderFullBeginLocked(OrderDetail::State& s) {
	s.group_open = false;
	s.current    = OrderDetail::Kind::Full;
}
inline void OrderBegin(id<MTLComputeCommandEncoder> e) {
	if (!EncoderFencesEnabled()) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	for (id<MTLFence> f: s.frontier) [e waitForFence:f];
	OrderFullBeginLocked(s);
}
inline void OrderBegin(id<MTLBlitCommandEncoder> e) {
	if (!EncoderFencesEnabled()) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	for (id<MTLFence> f: s.frontier) [e waitForFence:f];
	OrderFullBeginLocked(s);
}
inline void OrderBegin(id<MTLRenderCommandEncoder> e) {
	if (!EncoderFencesEnabled()) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	for (id<MTLFence> f: s.frontier) [e waitForFence:f beforeStages:MTLRenderStageVertex];
	OrderFullBeginLocked(s);
}

// A compute dispatch with its declared accesses (call right before the dispatch is encoded, after the bindings).
inline void OrderBeginCompute(id<MTLComputeCommandEncoder> e, const OrderAccess* acc, size_t n, bool unknown) {
	if (!EncoderFencesEnabled()) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	s.stat_dispatches.fetch_add(1, std::memory_order_relaxed);
	const bool conflict = unknown || !s.group_open || s.group_size >= OrderDetail::kMaxGroup || OrderDetail::Conflicts(s.group_access, acc, n);
	if (conflict) {
		if (unknown) s.stat_closed_unknown.fetch_add(1, std::memory_order_relaxed);
		else if (s.group_open) s.stat_closed_conflict.fetch_add(1, std::memory_order_relaxed);
		OrderDetail::WaitAll(e, s.frontier);
		s.group_base   = s.frontier;
		s.group_access.assign(acc, acc + n);
		s.group_size   = 1;
		s.group_open   = !unknown;
		s.current      = OrderDetail::Kind::Leader;
	} else {
		OrderDetail::WaitAll(e, s.group_base);
		s.group_access.insert(s.group_access.end(), acc, acc + n);
		++s.group_size;
		s.current = OrderDetail::Kind::Member;
		s.stat_overlapped.fetch_add(1, std::memory_order_relaxed);
	}
	const uint64_t total = s.stat_dispatches.load(std::memory_order_relaxed);
	if (total % 4000 == 0) {
		std::fprintf(stderr, "[CsOverlap] dispatches=%llu overlapped=%llu (%.0f%%) group closed by conflict=%llu by unknown access=%llu\n",
		             (unsigned long long)total, (unsigned long long)s.stat_overlapped.load(), 100.0 * s.stat_overlapped.load() / total,
		             (unsigned long long)s.stat_closed_conflict.load(), (unsigned long long)s.stat_closed_unknown.load());
	}
}

// After the fence update of an encoder: what the next encoder has to wait for.
inline void OrderEndLocked(OrderDetail::State& s, id<MTLFence> f) {
	switch (s.current) {
		case OrderDetail::Kind::Member:
			s.frontier.push_back(f);
			break;
		case OrderDetail::Kind::Leader:
			// the leader waited for the whole frontier, so its fence stands for everything before it
			s.frontier.assign(1, f);
			break;
		case OrderDetail::Kind::Full:
			s.frontier.assign(1, f);
			s.group_open = false;
			break;
		case OrderDetail::Kind::None:
			// no begin was recorded (a dispatch that was dropped): keep the old fences too
			s.frontier.push_back(f);
			s.group_open = false;
			break;
	}
	s.current = OrderDetail::Kind::None;
}
inline void OrderEnd(id<MTLComputeCommandEncoder> e) {
	if (!EncoderFencesEnabled() || e.device == nil) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	id<MTLFence> f = OrderDetail::NextFenceLocked(s, e.device);
	[e updateFence:f];
	OrderEndLocked(s, f);
}
inline void OrderEnd(id<MTLBlitCommandEncoder> e) {
	if (!EncoderFencesEnabled() || e.device == nil) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	id<MTLFence> f = OrderDetail::NextFenceLocked(s, e.device);
	[e updateFence:f];
	OrderEndLocked(s, f);
}
inline void OrderEnd(id<MTLRenderCommandEncoder> e) {
	if (!EncoderFencesEnabled() || e.device == nil) return;
	OrderDetail::State& s = OrderDetail::Get();
	std::lock_guard<std::mutex> lock(s.mutex);
	id<MTLFence> f = OrderDetail::NextFenceLocked(s, e.device);
	[e updateFence:f afterStages:MTLRenderStageFragment];
	OrderEndLocked(s, f);
}

// Called after the active encoder has ended. A guest barrier terminates an
// optional overlap group even when its next declared ranges appear disjoint.
// Keep every frontier fence: subsequent encoders must wait for the whole past.
inline void OrderMemoryBarrier() {
	auto& s = OrderDetail::Get();
	std::lock_guard lock(s.mutex);
	s.group_open = false;
	s.group_base.clear();
	s.group_access.clear();
	s.group_size = 0;
	s.current = OrderDetail::Kind::None;
}

} // namespace Libs::Graphics::Metal
