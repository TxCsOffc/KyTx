// Guest CPU abstraction: how the emulator runs x86-64 PS5 guest code.
//
// x86-64 hosts call guest code directly (inline asm in runtimeLinker.cpp / kernel/pthread.cpp).
// arm64 hosts (Apple Silicon, native build) run guest code under FEXCore, embedded in-process.
// Guest and host share one identity-mapped address space: a guest pointer is a host pointer.
//
// Guest -> host: every HLE function registered with the loader gets a tiny x86 stub
//   ("mov rax, rcx; syscall; ret") in guest-visible memory; FEX's syscall handler marshals SysV registers into an
//   AAPCS64 call of the host function and writes the result back.
// Host -> guest: CallGuest() uses FEX's re-entrant JIT callback entry; the guest returns through a
//   CALLBACKRET (0F 3E) sentinel. Host code that calls a guest function pointer directly (common in
//   HLE code) faults on the non-executable guest page; the fault handler redirects it to CallGuest.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__aarch64__) || defined(_M_ARM64)
#define KYTY_GUEST_CPU_EMULATED 1
#else
#define KYTY_GUEST_CPU_EMULATED 0
#endif

namespace Loader::GuestCpu {

// True when guest code runs under an x86-64 emulator (FEX) rather than natively.
constexpr bool IsEmulated() { return KYTY_GUEST_CPU_EMULATED != 0; }

#if KYTY_GUEST_CPU_EMULATED

// Create the FEX context and install the host fault hooks. Call once, early, on the main thread.
// Exits the process if FEX cannot be initialised.
void Init();

// KYTY_FEX_TRACE_TRIGGER=<file>: when the file appears (it is removed; polled from the flip tick, at most every 200 ms) the next
// KYTY_FEX_TRACE_BURST (default 20000) guest->host calls print their begin and end lines ("[fex] HLE #n -> ..." / "<- ..."), the way the first
// KYTY_FEX_TRACE calls do at start. A terminal `touch` therefore records the start and the end of every host call for about one frame.
void TraceBurstPoll();

// Run the guest function at `entry` on the calling host thread with SysV integer arguments
// (at most 6 are passed in registers; the rest are not supported) and return guest RAX.
// `guest_stack_top`: when non-zero, guest RSP starts there (16-byte aligned by the callee);
// otherwise the guest continues on its current stack (nested call) or on a per-thread stack.
// Re-entrant: guest -> host -> guest nesting is supported.
uint64_t CallGuest(uint64_t entry, const uint64_t* args, size_t count, uint64_t guest_stack_top = 0);

// Return a guest-callable address for the host function `host_fn` (an x86 stub that dispatches
// into the host). Idempotent for the same host_fn. `name` is used for diagnostics.
uint64_t GetHostFunctionStub(uint64_t host_fn, const char* name);

// True when `addr` is inside one of the HLE stub pages created by GetHostFunctionStub.
bool IsHostFunctionStub(uint64_t addr);

// FS segment base (PS5 TLS: fs:[0] holds the TCB self pointer) for guest code run by the calling
// host thread. Applies to the current and all later guest calls on this thread.
void SetThreadFsBase(uint64_t fs_base);
uint64_t GetThreadFsBase();

// Provider used to lazily set FS base the first time a host thread enters guest code.
using FsBaseProvider = uint64_t (*)();
void SetFsBaseProvider(FsBaseProvider provider);

// Guest code was modified (relocations, patches, self-modifying code): drop translated blocks.
void InvalidateCode(uint64_t start, uint64_t length);

// Host fault hook (SIGSEGV/SIGBUS). Returns true if the fault belonged to FEX (JIT guard page) or
// was a host->guest direct call that has been redirected; the caller must then resume `ucontext`.
bool HandleHostFault(int sig, void* siginfo, void* ucontext);

// Snapshot of the calling thread's guest registers at the innermost guest->host transition
// (valid inside an HLE function). Returns false outside guest calls.
struct GuestRegs {
	uint64_t gpr[16]; // RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8..R15 (x86 encoding order)
	uint64_t rip;
};
bool GetCurrentGuestRegs(GuestRegs* out);

// Diagnostic only: FEX's best RIP reconstruction for a fault inside its JIT cache.
// FEX can fall back to a saved RIP; this is a location hint, not a reconstructed register file.
struct GuestFaultLocation {
	uint64_t rip = 0;
	uint64_t block_entry = 0;
	uint64_t saved_rip = 0;
};
bool GetFaultGuestLocation(uint64_t host_pc, GuestFaultLocation* out);

// Diagnostics counters.
struct Stats {
	uint64_t hle_calls;
	uint64_t guest_calls;
	uint64_t redirected_host_calls;
	uint64_t stubs;
};
Stats GetStats();

#else

inline uint64_t GetHostFunctionStub(uint64_t host_fn, const char* /*name*/) { return host_fn; }
inline bool     IsHostFunctionStub(uint64_t /*addr*/) { return false; }
inline void     InvalidateCode(uint64_t /*start*/, uint64_t /*length*/) {}

#endif

} // namespace Loader::GuestCpu
