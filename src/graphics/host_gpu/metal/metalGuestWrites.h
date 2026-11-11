#pragma once
// Write generations of guest physical memory, for caches that hold a copy of guest bytes (canonical images, canonical render
// targets). A copy made at generation G is still exact while no page it was made from was written after G:
//   - guest CPU code and emulator host code write through guest virtual addresses; a watched page is write-protected, its
//     first write faults (HandleCpuWriteFault), the page is unprotected and its generation advances;
//   - host writes announced before they happen (file reads into guest memory, kernel invalidations) unprotect the page and
//     advance it, and the page is left unwatched for a short cooldown so a racing re-protect cannot fail a host syscall;
//   - writes that do not fault (GPU work encoded by the renderer, backing-store writes) are reported at the point they are
//     encoded or performed (NoteWrite / NotePhysicalWrite); a GPU writer whose range is unknown advances every page
//     (NoteUnknownWrite).
// A page that is not watched is never reported current: unwatched memory may change without notice.
#include <cstdint>
#include <span>

namespace Libs::Graphics::Metal::GuestWrites {

struct PhysicalRange {
	uint64_t physical = 0;
	uint64_t size     = 0;
};

// A new generation for a copy that is about to be made. Call after Watch() and after reporting the writes that precede it.
[[nodiscard]] uint64_t Begin();
// Write-protects every page backing [vaddr, vaddr+size) through all its guest aliases. Pages in their cooldown stay
// unwatched.
void Watch(uint64_t vaddr, uint64_t size);
// True when every page backing [vaddr, vaddr+size) is watched and was not written after `generation`.
[[nodiscard]] bool Current(uint64_t vaddr, uint64_t size, uint64_t generation);

// A write that does not fault: GPU work at encode time, a backing-store write.
void NoteWrite(uint64_t vaddr, uint64_t size);
void NotePhysicalWrite(std::span<const PhysicalRange> ranges);
// A GPU writer with an unknown target (flat/global stores).
void NoteUnknownWrite();
// A host write announced before it happens (it may be a syscall that cannot take a fault).
void InvalidateHostWrite(uint64_t vaddr, uint64_t size);
// Guest mapping or protection change of the range: aliases may have changed under the watch.
void NoteMappingChange();

// Write fault at `vaddr`: true when the page was watched (it is now unprotected and advanced).
[[nodiscard]] bool HandleCpuWriteFault(uint64_t vaddr) noexcept;

struct Stats {
	uint64_t faults = 0, watched_pages = 0, gpu_notes = 0, unknown = 0, host_invalidations = 0;
};
[[nodiscard]] Stats TakeStats();

} // namespace Libs::Graphics::Metal::GuestWrites
