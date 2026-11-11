// Implementation of metalGuestWrites.h, included once at the end of metalRenderer.mm (outside its namespaces).
#include "graphics/host_gpu/metal/metalGuestWrites.h"

#include "common/virtualMemory.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Metal::GuestWrites {
namespace {

constexpr uint64_t kPage     = 0x4000; // guest pages and Apple Silicon host pages are 16 KiB
constexpr uint64_t kPageMask = kPage - 1u;
// A host write announced by InvalidateHostWrite may be a syscall writing guest memory for a while after the call (a file
// read of a large asset); its pages are not re-protected before this has passed.
constexpr int64_t kHostWriteCooldownNs = 500'000'000;

struct Page {
	uint64_t              generation = 0;
	bool                  watched    = false;
	std::vector<uint64_t> aliases; // guest virtual pages protected for it
};

struct State {
	std::atomic_flag lock = ATOMIC_FLAG_INIT;
	uint64_t         generation = 1;
	uint64_t         unknown    = 0;
	uint64_t         mapping_generation = UINT64_MAX;
	std::vector<LibKernel::Memory::GuestBackingMapping> mappings; // sorted by address
	std::unordered_map<uint64_t, Page>     pages;   // physical page -> state
	std::unordered_map<uint64_t, uint64_t> aliases; // protected guest page -> physical page
	struct Recent {
		uint64_t begin, end;
		int64_t  until;
	};
	std::deque<Recent> recent; // physical ranges of announced host writes
	Stats              stats;
};

State& S() {
	static State* state = new State; // never destroyed: the fault handler may run during exit
	return *state;
}

class Guard {
public:
	explicit Guard(State& s): m_s(s) {
		while (m_s.lock.test_and_set(std::memory_order_acquire)) {
			std::atomic_signal_fence(std::memory_order_seq_cst);
		}
	}
	~Guard() { m_s.lock.clear(std::memory_order_release); }

private:
	State& m_s;
};

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Unprotect(uint64_t va_page) {
	(void)LibKernel::Memory::ProtectGuestHostMemory(va_page, kPage, Common::VirtualMemory::Mode::ReadWrite);
}

void UnwatchLocked(State& s, Page& page) {
	for (uint64_t va: page.aliases) {
		Unprotect(va);
		s.aliases.erase(va);
	}
	page.aliases.clear();
	page.watched = false;
}

// Re-reads the guest mappings when they changed. A change may have added aliases nobody protected, so every copy becomes
// stale and every watch is dropped (the pages are unprotected; Watch() protects the current aliases again).
void RefreshMappingsLocked(State& s) {
	std::vector<LibKernel::Memory::GuestBackingMapping> updated;
	uint64_t generation = s.mapping_generation;
	if (!LibKernel::Memory::GetGuestBackingMappings(generation, updated)) return;
	const bool first = s.mapping_generation == UINT64_MAX;
	s.mapping_generation = generation;
	std::sort(updated.begin(), updated.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
	s.mappings = std::move(updated);
	if (first) return;
	for (auto& [physical, page]: s.pages) {
		if (page.watched) UnwatchLocked(s, page);
	}
	s.unknown = ++s.generation;
}

// Calls fn(physical_page, virtual_page) for every page of [vaddr, vaddr+size). False when a byte is unmapped.
template <typename Fn>
bool ForEachPage(const State& s, uint64_t vaddr, uint64_t size, Fn&& fn) {
	if (size == 0) return true;
	uint64_t       va  = vaddr & ~kPageMask;
	const uint64_t end = vaddr + size;
	auto it = std::upper_bound(s.mappings.begin(), s.mappings.end(), va, [](uint64_t a, const auto& m) { return a < m.address; });
	if (it == s.mappings.begin()) return false;
	--it;
	while (va < end) {
		if (it == s.mappings.end() || va < it->address) return false;
		if (va >= it->address + it->size) {
			++it;
			continue;
		}
		fn(((it->backing_offset + (va - it->address)) & ~kPageMask), va);
		va += kPage;
	}
	return true;
}

bool InCooldownLocked(State& s, uint64_t physical, int64_t now) {
	while (!s.recent.empty() && s.recent.front().until <= now) s.recent.pop_front();
	for (const auto& r: s.recent) {
		if (physical < r.end && physical + kPage > r.begin) return true;
	}
	return false;
}

void AdvanceLocked(State& s, uint64_t physical) {
	auto it = s.pages.find(physical);
	if (it != s.pages.end()) it->second.generation = ++s.generation;
}

} // namespace

uint64_t Begin() {
	auto& s = S();
	Guard g(s);
	return ++s.generation;
}

void Watch(uint64_t vaddr, uint64_t size) {
	auto& s = S();
	Guard g(s);
	RefreshMappingsLocked(s);
	std::vector<uint64_t> wanted;
	(void)ForEachPage(s, vaddr, size, [&](uint64_t physical, uint64_t) { wanted.push_back(physical); });
	if (wanted.empty()) return;
	std::sort(wanted.begin(), wanted.end());
	wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
	const uint64_t low = wanted.front(), high = wanted.back() + kPage;
	// Every mapping that aliases part of the physical span: all of them must fault on a CPU write.
	std::vector<const LibKernel::Memory::GuestBackingMapping*> candidates;
	for (const auto& m: s.mappings) {
		if (m.backing_offset < high && low < m.backing_offset + m.size) candidates.push_back(&m);
	}
	const int64_t         now = NowNs();
	std::vector<uint64_t> protect;
	for (uint64_t physical: wanted) {
		auto& page = s.pages[physical];
		if (page.watched || InCooldownLocked(s, physical, now)) continue;
		for (const auto* m: candidates) {
			if (physical < m->backing_offset || physical >= m->backing_offset + m->size) continue;
			const uint64_t va = m->address + (physical - m->backing_offset);
			page.aliases.push_back(va);
			s.aliases[va] = physical;
			protect.push_back(va);
		}
		page.watched = !page.aliases.empty();
		++s.stats.watched_pages;
	}
	std::sort(protect.begin(), protect.end());
	for (size_t i = 0; i < protect.size();) {
		size_t j = i + 1;
		while (j < protect.size() && protect[j] == protect[j - 1] + kPage) ++j;
		if (!LibKernel::Memory::ProtectGuestHostMemory(protect[i], (j - i) * kPage, Common::VirtualMemory::Mode::Read)) {
			// Not protectable: those pages stay unwatched (never current).
			for (size_t k = i; k < j; ++k) {
				const auto found = s.aliases.find(protect[k]);
				if (found == s.aliases.end()) continue;
				auto& page = s.pages[found->second];
				UnwatchLocked(s, page);
			}
		}
		i = j;
	}
}

bool Current(uint64_t vaddr, uint64_t size, uint64_t generation) {
	auto& s = S();
	Guard g(s);
	if (LibKernel::Memory::GetGuestBackingGeneration() != s.mapping_generation) RefreshMappingsLocked(s);
	if (s.unknown > generation) return false;
	bool current = true;
	const bool mapped = ForEachPage(s, vaddr, size, [&](uint64_t physical, uint64_t) {
		if (!current) return;
		const auto it = s.pages.find(physical);
		current = it != s.pages.end() && it->second.watched && it->second.generation < generation;
	});
	return mapped && current;
}

void NoteWrite(uint64_t vaddr, uint64_t size) {
	auto& s = S();
	Guard g(s);
	++s.stats.gpu_notes;
	if (s.pages.empty()) return;
	if (LibKernel::Memory::GetGuestBackingGeneration() != s.mapping_generation) RefreshMappingsLocked(s);
	if (!ForEachPage(s, vaddr, size, [&](uint64_t physical, uint64_t) { AdvanceLocked(s, physical); })) {
		s.unknown = ++s.generation; // part of the range is not mapped now: be conservative
	}
}

void NotePhysicalWrite(std::span<const PhysicalRange> ranges) {
	auto& s = S();
	Guard g(s);
	++s.stats.gpu_notes;
	if (s.pages.empty()) return;
	uint64_t last = UINT64_MAX;
	for (const auto& r: ranges) {
		if (r.size == 0) continue;
		for (uint64_t p = r.physical & ~kPageMask; p < r.physical + r.size; p += kPage) {
			if (p == last) continue;
			AdvanceLocked(s, p);
			last = p;
		}
	}
}

void NoteUnknownWrite() {
	auto& s = S();
	Guard g(s);
	++s.stats.unknown;
	s.unknown = ++s.generation;
}

void InvalidateHostWrite(uint64_t vaddr, uint64_t size) {
	auto& s = S();
	Guard g(s);
	++s.stats.host_invalidations;
	if (LibKernel::Memory::GetGuestBackingGeneration() != s.mapping_generation) RefreshMappingsLocked(s);
	const int64_t until = NowNs() + kHostWriteCooldownNs;
	uint64_t      begin = UINT64_MAX, end = 0;
	const bool mapped = ForEachPage(s, vaddr, size, [&](uint64_t physical, uint64_t) {
		auto it = s.pages.find(physical);
		if (it != s.pages.end()) {
			if (it->second.watched) UnwatchLocked(s, it->second);
			it->second.generation = ++s.generation;
		}
		if (begin != UINT64_MAX && physical == end) {
			end += kPage;
		} else {
			if (begin != UINT64_MAX) s.recent.push_back({begin, end, until});
			begin = physical;
			end   = physical + kPage;
		}
	});
	if (begin != UINT64_MAX) s.recent.push_back({begin, end, until});
	while (s.recent.size() > 4096) s.recent.pop_front();
	if (!mapped) s.unknown = ++s.generation;
}

void NoteMappingChange() {
	auto& s = S();
	Guard g(s);
	for (auto& [physical, page]: s.pages) {
		if (page.watched) UnwatchLocked(s, page);
	}
	s.unknown = ++s.generation;
}

bool HandleCpuWriteFault(uint64_t vaddr) noexcept {
	auto& s = S();
	Guard g(s);
	const auto found = s.aliases.find(vaddr & ~kPageMask);
	if (found == s.aliases.end()) return false;
	auto& page = s.pages[found->second];
	UnwatchLocked(s, page);
	page.generation = ++s.generation;
	++s.stats.faults;
	return true;
}

Stats TakeStats() {
	auto& s = S();
	Guard g(s);
	const Stats out = s.stats;
	s.stats         = {};
	return out;
}

} // namespace Libs::Graphics::Metal::GuestWrites
