#pragma once
// Test double for the three kernel memory functions metalGuestWrites.inc uses: a mapping table the test edits and a record
// of the host protection each guest page was given.
#include "kernel/memory.h"

#include <map>
#include <vector>

namespace GuestWritesShim {
inline std::vector<Libs::LibKernel::Memory::GuestBackingMapping> g_mappings;
inline uint64_t                                                   g_generation = 1;
inline std::map<uint64_t, ::Common::VirtualMemory::Mode>      g_protection; // guest page -> last mode set
inline bool ReadOnly(uint64_t page) {
	const auto it = g_protection.find(page);
	return it != g_protection.end() && it->second == ::Common::VirtualMemory::Mode::Read;
}
} // namespace GuestWritesShim

namespace Libs::LibKernel::Memory {
inline bool GetGuestBackingMappings(uint64_t& generation, std::vector<GuestBackingMapping>& mappings) {
	if (generation == GuestWritesShim::g_generation) return false;
	generation = GuestWritesShim::g_generation;
	mappings   = GuestWritesShim::g_mappings;
	return true;
}
inline uint64_t GetGuestBackingGeneration() { return GuestWritesShim::g_generation; }
inline bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size, Common::VirtualMemory::Mode mode) {
	for (uint64_t p = vaddr; p < vaddr + size; p += 0x4000) GuestWritesShim::g_protection[p] = mode;
	return true;
}
} // namespace Libs::LibKernel::Memory
