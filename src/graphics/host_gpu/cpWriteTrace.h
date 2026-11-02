#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "graphics/presentation/traceWindow.h"

// Diagnostic: the guest memory writes the command processor does on the CPU at the moment it reads the packet (WRITE_DATA, DMA_DATA fills and
// copies that the buffer cache takes for "not modified by the GPU", proven fills). GPU work recorded earlier is still queued when they land, so a
// kernel that runs later sees the new bytes instead of the ones the game's command order promised. Header-only so the CP code and the Metal
// code (which prints the matching writes when the indirect-arguments guard fires) share one ring without a link dependency.
namespace Libs::Graphics {

struct CpWriteNote {
	uint64_t addr  = 0;
	uint64_t bytes = 0;
	uint32_t clock = 0;
	uint32_t first = 0; // first dword written
	char     kind[16] = {};
};

constexpr uint32_t kCpWriteRing = 65536;

inline std::atomic<uint32_t>& CpTraceClock() {
	static std::atomic<uint32_t> c {0};
	return c;
}
inline uint32_t CpTraceTick() {
	return CpTraceClock().fetch_add(1, std::memory_order_relaxed) + 1;
}
inline CpWriteNote* CpTraceRing() {
	static CpWriteNote* ring = new CpWriteNote[kCpWriteRing];
	return ring;
}

// Presented-frame counter for the log lines (the command processor runs ahead of the presented frame, so it is only a rough position).
inline uint64_t CpTraceFlip() { return Libs::Graphics::TraceWindow::g_flip.load(std::memory_order_relaxed); }

inline bool CpTraceEnabled() {
	static const bool on = std::getenv("KYTY_TRACE_CP_WRITES") != nullptr;
	return on;
}

// KYTY_LOG_CP_WRITES=<lo>,<hi>[,<min bytes>] (hex, with KYTY_TRACE_CP_WRITES=1): print every command-processor write that overlaps [lo, hi).
inline void CpTraceLog(uint64_t addr, uint64_t bytes, const char* kind, uint32_t first, uint64_t flip) {
	static unsigned long long min_bytes = 0;
	static const std::pair<uint64_t, uint64_t> range = [] {
		unsigned long long lo = 0, hi = 0;
		if (const char* v = std::getenv("KYTY_LOG_CP_WRITES")) std::sscanf(v, "%llx,%llx,%llx", &lo, &hi, &min_bytes);
		return std::pair<uint64_t, uint64_t> {lo, hi};
	}();
	if (range.second == 0 || addr >= range.second || addr + bytes <= range.first || bytes < min_bytes) return;
	static std::atomic<uint32_t> printed {0};
	if (printed.fetch_add(1) < 4000) {
		std::fprintf(stderr, "[CpWrite] flip=%llu kind=%s addr=0x%llx bytes=%llu value=0x%x\n", static_cast<unsigned long long>(flip), kind,
		             static_cast<unsigned long long>(addr), static_cast<unsigned long long>(bytes), first);
	}
}

inline void CpTraceWrite(uint64_t addr, uint64_t bytes, const char* kind, uint32_t first);
inline void CpTraceWriteImpl(uint64_t addr, uint64_t bytes, const char* kind, uint32_t first, uint64_t flip) {
	CpTraceLog(addr, bytes, kind, first, flip);
}

inline void CpTraceWrite(uint64_t addr, uint64_t bytes, const char* kind, uint32_t first) {
	if (!CpTraceEnabled()) return;
	CpTraceWriteImpl(addr, bytes, kind, first, CpTraceFlip());
	const uint32_t clock = CpTraceTick();
	CpWriteNote&   n     = CpTraceRing()[clock % kCpWriteRing];
	n.addr               = addr;
	n.bytes              = bytes;
	n.first              = first;
	std::snprintf(n.kind, sizeof(n.kind), "%s", kind);
	n.clock = clock; // written last: a reader that sees the clock sees the rest
}

} // namespace Libs::Graphics
