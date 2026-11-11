#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "loader/guest_cpu/guest_cpu.h"

namespace Libs::Graphics::TraceWindow {

// KYTY_TRACE_FRAMES=<first>:<last-exclusive>: draw traces (KYTY_BIND_TRACE) are emitted only
// between those flip indexes. Flips are counted in Presenter::PrepareFrame (shared by backends).
inline std::atomic<uint64_t> g_flip {0};
inline std::atomic<uint64_t> g_draw_in_frame {0};

inline bool Active() {
	static const bool s_has_window = std::getenv("KYTY_TRACE_FRAMES") != nullptr;
	static const uint64_t s_first = [] {
		const char* v = std::getenv("KYTY_TRACE_FRAMES");
		return v != nullptr ? std::strtoull(v, nullptr, 10) : 0ull;
	}();
	static const uint64_t s_last = [] {
		const char* v = std::getenv("KYTY_TRACE_FRAMES");
		const char* c = v != nullptr ? std::strchr(v, ':') : nullptr;
		return c != nullptr ? std::strtoull(c + 1, nullptr, 10) : 0ull;
	}();
	if (!s_has_window) return true;
	const uint64_t f = g_flip.load(std::memory_order_relaxed);
	return f >= s_first && f < s_last;
}

// Per-frame draw index for trace lines.
inline uint64_t NextDraw() { return g_draw_in_frame.fetch_add(1, std::memory_order_relaxed); }

inline void OnFlip() {
	const uint64_t f = g_flip.fetch_add(1, std::memory_order_relaxed);
	g_draw_in_frame.store(0, std::memory_order_relaxed);
#if KYTY_GUEST_CPU_EMULATED
	Loader::GuestCpu::TraceBurstPoll();
#endif
	static const bool s_mark = std::getenv("KYTY_TRACE_PM4") != nullptr;
	if (s_mark) std::fprintf(stderr, "[Flip] %llu\n", static_cast<unsigned long long>(f));
}

} // namespace Libs::Graphics::TraceWindow
