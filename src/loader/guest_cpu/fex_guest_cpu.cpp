// GuestCpu on FEXCore (FEX-darwin, MIT): runs PS5 x86-64 guest code on arm64 macOS hosts.
// See guest_cpu.h for the overall model. Compiled only for arm64 builds.
#include "graphics/presentation/traceWindow.h"
#include "loader/guest_cpu/guest_cpu.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/LongJump.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------
// AAPCS64 call trampolines (defined in the top-level asm block below).
// ---------------------------------------------------------------------------------------------
namespace Loader::GuestCpu::Detail {

// Host call frame shared by both trampolines. Offsets are hard-coded in the asm.
struct alignas(16) HostCallFrame {
	uint64_t    fn;        // 0
	uint64_t    x[8];      // 8
	uint64_t    stack[8];  // 72: outgoing stack arguments (callee's [sp], [sp+8], ...)
	uint64_t    pad;       // 136
	__uint128_t v[8];      // 144
	uint64_t    ret_x0;    // 272
	uint64_t    ret_x1;    // 280
	__uint128_t ret_v0;    // 288
	uint64_t    target;    // 304: bridge only: guest function the host tried to call
	uint64_t    pad2;      // 312
};
static_assert(offsetof(HostCallFrame, x) == 8);
static_assert(offsetof(HostCallFrame, stack) == 72);
static_assert(offsetof(HostCallFrame, v) == 144);
static_assert(offsetof(HostCallFrame, ret_x0) == 272);
static_assert(offsetof(HostCallFrame, ret_v0) == 288);
static_assert(sizeof(HostCallFrame) == 320);

} // namespace Loader::GuestCpu::Detail

extern "C" {
// Calls frame->fn with x0-x7, v0-v7 and 8 stack words from the frame; stores x0, x1, q0 back.
void kyty_fex_host_call(Loader::GuestCpu::Detail::HostCallFrame* frame);
// Entry point a faulting host->guest call is redirected to (x0 = frame, lr = original return).
void kyty_fex_bridge_entry();
// C++ side of the bridge: runs the guest function, fills ret_*, returns the frame.
Loader::GuestCpu::Detail::HostCallFrame* kyty_fex_bridge_run(Loader::GuestCpu::Detail::HostCallFrame* frame);
}

asm(R"(
	.text
	.p2align 4
	.globl _kyty_fex_host_call
_kyty_fex_host_call:
	stp   x29, x30, [sp, #-32]!
	mov   x29, sp
	str   x19, [sp, #16]
	mov   x19, x0
	sub   sp, sp, #64
	ldp   x9, x10, [x19, #72]
	stp   x9, x10, [sp, #0]
	ldp   x9, x10, [x19, #88]
	stp   x9, x10, [sp, #16]
	ldp   x9, x10, [x19, #104]
	stp   x9, x10, [sp, #32]
	ldp   x9, x10, [x19, #120]
	stp   x9, x10, [sp, #48]
	ldp   q0, q1, [x19, #144]
	ldp   q2, q3, [x19, #176]
	ldp   q4, q5, [x19, #208]
	ldp   q6, q7, [x19, #240]
	ldp   x0, x1, [x19, #8]
	ldp   x2, x3, [x19, #24]
	ldp   x4, x5, [x19, #40]
	ldp   x6, x7, [x19, #56]
	ldr   x9, [x19, #0]
	blr   x9
	stp   x0, x1, [x19, #272]
	str   q0, [x19, #288]
	add   sp, sp, #64
	ldr   x19, [sp, #16]
	ldp   x29, x30, [sp], #32
	ret

	.p2align 4
	.globl _kyty_fex_bridge_entry
_kyty_fex_bridge_entry:
	stp   x29, x30, [sp, #-16]!
	mov   x29, sp
	bl    _kyty_fex_bridge_run
	ldr   q0, [x0, #288]
	ldr   x1, [x0, #280]
	ldr   x0, [x0, #272]
	ldp   x29, x30, [sp], #16
	ret
)");

namespace Loader::GuestCpu {
namespace {

using Detail::HostCallFrame;
namespace X86 = FEXCore::X86State;

constexpr uint64_t kStubSlotSize   = 8;
constexpr uint64_t kStubRegionSize = 0x100000; // 131072 stubs
constexpr uint64_t kOwnStackSize   = 0x200000; // per host thread, for top-level guest calls
constexpr int      kMaxBridgeDepth = 64;
constexpr uint64_t kRedZone        = 128;
constexpr uint64_t kHostPage       = 0x4000;
// FEX's call-ret prediction stack (x25). Allocated by the frontend, as in Linux ThreadManager.cpp.
constexpr uint64_t kCallRetSize      = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
constexpr uint64_t kCallRetAllocSize = kCallRetSize + 2 * kHostPage;
constexpr uint64_t CallRetDefault(uint64_t alloc) { return alloc + kHostPage + kCallRetSize / 4; }

// ---------------------------------------------------------------------------------------------
// Configuration knobs (environment), for A/B measurements.
// ---------------------------------------------------------------------------------------------
// KYTY_TRACE_LASTCALL=1: per-thread last returned HLE call and in-flight (entered, not returned)
// call, dumped every 2 s. A thread blocked inside an HLE call shows it as inflight.
struct LastCallTable {
	std::mutex                                                          mutex;
	std::unordered_map<uint64_t, std::string>                           inflight;
	std::unordered_map<uint64_t, std::pair<std::string, uint64_t>>      last_ret;
	std::chrono::steady_clock::time_point                               last_dump {};
};

LastCallTable& GetLastCallTable() {
	static LastCallTable table;
	return table;
}

bool LastCallEnabled() {
	static const bool s_on = std::getenv("KYTY_TRACE_LASTCALL") != nullptr;
	return s_on;
}

void LastCallEnter(const char* name, uint64_t a0, uint64_t a1) {
	if (!LastCallEnabled()) return;
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	auto& t = GetLastCallTable();
	std::lock_guard<std::mutex> lock(t.mutex);
	char buf[160];
	std::snprintf(buf, sizeof(buf), "%s(0x%llx,0x%llx)", name, static_cast<unsigned long long>(a0),
	              static_cast<unsigned long long>(a1));
	t.inflight[tid] = buf;
}

void LastCallLeave(const char* name, uint64_t ret, uint64_t a0, uint64_t a1) {
	if (!LastCallEnabled()) return;
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	auto& t = GetLastCallTable();
	std::lock_guard<std::mutex> lock(t.mutex);
	t.inflight.erase(tid);
	t.last_ret[tid] = {name, ret};
	if (std::strstr(name, "Signal") != nullptr || std::strstr(name, "Broadcast") != nullptr) {
		std::fprintf(stderr, "[signal] tid=%llu fn=%s a0=0x%llx a1=0x%llx ret=0x%llx\n",
		             static_cast<unsigned long long>(tid), name, static_cast<unsigned long long>(a0),
		             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(ret));
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - t.last_dump >= std::chrono::seconds(2)) {
		t.last_dump = now;
		const uint64_t now_ms = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
		for (const auto& [id, c]: t.last_ret) {
			const auto fl = t.inflight.find(id);
			std::fprintf(stderr, "[lastcall] t=%llu tid=%llu fn=%s ret=0x%llx inflight=%s\n",
			             static_cast<unsigned long long>(now_ms), static_cast<unsigned long long>(id),
			             c.first.c_str(), static_cast<unsigned long long>(c.second),
			             fl != t.inflight.end() ? fl->second.c_str() : "-");
		}
	}
}

int EnvInt(const char* name, int def) {
	const char* v = std::getenv(name);
	return (v != nullptr && *v != '\0') ? std::atoi(v) : def;
}

void MsgHandler(LogMan::DebugLevels level, const char* message) {
	if (level <= LogMan::DebugLevels::INFO) {
		std::fprintf(stderr, "[fex %d] %s\n", static_cast<int>(level), message);
	}
}
void AssertHandler(const char* message) {
	std::fprintf(stderr, "[fex ASSERT] %s\n", message);
	std::abort();
}

bool SysctlFlag(const char* name) {
	int    v = 0;
	size_t s = sizeof(v);
	return sysctlbyname(name, &v, &s, nullptr, 0) == 0 && v != 0;
}

// FEX::FetchHostFeatures reads ID registers / sysfs (Linux only); derive the bits from sysctl.
FEXCore::HostFeatures MakeHostFeatures() {
	FEXCore::HostFeatures f {};
	uint64_t line = 64;
	size_t   size = sizeof(line);
	if (sysctlbyname("hw.cachelinesize", &line, &size, nullptr, 0) != 0 || line == 0) {
		line = 64;
	}
	f.DCacheLineLog2               = static_cast<uint32_t>(std::log2(static_cast<double>(line) / 4.0));
	f.SupportsCacheMaintenanceOps  = 0;
	f.SupportsAES                  = SysctlFlag("hw.optional.arm.FEAT_AES");
	f.SupportsCRC                  = SysctlFlag("hw.optional.arm.FEAT_CRC32");
	f.SupportsAtomics              = SysctlFlag("hw.optional.arm.FEAT_LSE");
	f.SupportsRCPC                 = SysctlFlag("hw.optional.arm.FEAT_LRCPC");
	f.SupportsTSOImm9              = SysctlFlag("hw.optional.arm.FEAT_LRCPC2");
	f.SupportsPMULL_128Bit         = SysctlFlag("hw.optional.arm.FEAT_PMULL");
	f.SupportsSHA                  = SysctlFlag("hw.optional.arm.FEAT_SHA1") && SysctlFlag("hw.optional.arm.FEAT_SHA256");
	f.SupportsFlagM                = SysctlFlag("hw.optional.arm.FEAT_FlagM");
	f.SupportsFlagM2               = SysctlFlag("hw.optional.arm.FEAT_FlagM2");
	f.SupportsFRINTTS              = SysctlFlag("hw.optional.arm.FEAT_FRINTTS");
	f.SupportsRAND                 = false;
	f.SupportsSVE128               = false;
	f.SupportsSVE256               = false;
	f.SupportsAVX                  = true; // NEON host: AVX emulated as two 128-bit halves
	f.SupportsCPUIndexInTPIDRRO    = false;
	f.HostType                     = FEXCore::HostFeatures::HostTypeEnum::Linux; // no Darwin enum value
	// CPUID leaf 1/4/0xB report one logical CPU per MIDR entry. EL0 cannot read MIDR on Darwin;
	// use a uniform Apple-implementer value (hybrid P/E cores are hidden from the guest anyway).
	int    ncpu     = 0;
	size_t ncpu_len = sizeof(ncpu);
	if (sysctlbyname("hw.logicalcpu", &ncpu, &ncpu_len, nullptr, 0) != 0 || ncpu <= 0) {
		ncpu = 8;
	}
	f.CPUMIDRs.assign(static_cast<size_t>(ncpu), 0x610F0000u);
	return f;
}

// ---------------------------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------------------------
struct StubEntry {
	uint64_t    host_fn;
	const char* name;
};

struct Globals {
	fextl::unique_ptr<FEXCore::Context::Context> ctx;

	uint8_t*                               stub_base = nullptr;
	std::mutex                             stub_mutex;
	std::vector<StubEntry>                 stubs; // reserved up front: never reallocates
	std::atomic<size_t>                    stub_count {0};
	std::unordered_map<uint64_t, uint32_t> stub_by_fn;
	std::deque<std::string>                stub_names;

	uint8_t* callback_ret = nullptr; // guest address of "0F 3E" (FEX CALLBACKRET)

	FsBaseProvider fs_provider = nullptr;

	std::atomic<uint64_t> hle_calls {0};
	std::atomic<uint64_t> guest_calls {0};
	std::atomic<uint64_t> redirected {0};
	std::atomic<uint64_t> guest_syscalls {0};
	std::atomic<uint64_t> unaligned {0};
	int                   trace_limit = 0;
	std::atomic<int>      traced {0};
};

Globals* g = nullptr;

// ---------------------------------------------------------------------------------------------
// Per host thread state
// ---------------------------------------------------------------------------------------------
struct ThreadState {
	FEXCore::Core::InternalThreadState*         thread = nullptr;
	FEXCore::Core::CPUState::gdt_segment        gdt[32] {};
	uint64_t                                    fs_base   = 0;
	int                                         depth     = 0; // active guest calls on this thread
	uint8_t*                                    own_stack = nullptr;
	static constexpr int                        kMaxHleRegsDepth = 16;
	GuestRegs                                   hle_regs[kMaxHleRegsDepth] {};
	int                                         hle_regs_depth = 0;
	HostCallFrame                               bridge[kMaxBridgeDepth] {};
	int                                         bridge_depth = 0;
	uint64_t                                    callret_alloc = 0; // call-ret stack incl. guard pages

	~ThreadState() {
		if (thread != nullptr && g != nullptr && g->ctx) {
			g->ctx->DestroyThread(thread);
		}
		if (own_stack != nullptr) {
			munmap(own_stack, kOwnStackSize);
		}
		if (callret_alloc != 0) {
			munmap(reinterpret_cast<void*>(callret_alloc), kCallRetAllocSize);
		}
	}
};

thread_local ThreadState* t_state = nullptr;
thread_local std::unique_ptr<ThreadState> t_state_owner;

FEXCore::Core::CPUState& State(ThreadState* ts) { return ts->thread->CurrentFrame->State; }

ThreadState* GetThreadState() {
	if (t_state != nullptr) {
		return t_state;
	}
	t_state_owner = std::make_unique<ThreadState>();
	auto* ts      = t_state_owner.get();

	FEXCore::Core::CPUState st {};
	st.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = ts->gdt;
	st.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = ts->gdt;
	st.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
	auto* cs  = FEXCore::Core::CPUState::GetSegmentFromIndex(st, st.cs_idx);
	FEXCore::Core::CPUState::SetGDTBase(cs, 0);
	FEXCore::Core::CPUState::SetGDTLimit(cs, 0xF'FFFFU);
	cs->L        = 1; // 64-bit code segment
	cs->D        = 0;
	st.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*cs);
	st.mxcsr     = 0x1F80;

	ts->thread = g->ctx->CreateThread(&st);
	if (ts->thread == nullptr) {
		std::fprintf(stderr, "[fex] CreateThread failed\n");
		std::abort();
	}
	ts->thread->CurrentFrame->Pointers.ThunkCallbackRet = reinterpret_cast<uintptr_t>(g->callback_ret);

	void* callret = mmap(nullptr, kCallRetAllocSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (callret == MAP_FAILED) {
		std::fprintf(stderr, "[fex] call-ret stack allocation failed\n");
		std::abort();
	}
	ts->callret_alloc = reinterpret_cast<uint64_t>(callret);
	mprotect(static_cast<uint8_t*>(callret) + kHostPage, kCallRetSize, PROT_READ | PROT_WRITE);
	ts->thread->CallRetStackBase = static_cast<uint8_t*>(callret) + kHostPage;
	ts->thread->CurrentFrame->State.callret_sp = CallRetDefault(ts->callret_alloc);
	t_state = ts;
	return ts;
}

uint8_t* MapGuestPages(uint64_t size, const char* what) {
	void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED) {
		std::fprintf(stderr, "[fex] mmap(%s) failed\n", what);
		std::abort();
	}
	return static_cast<uint8_t*>(p);
}

inline bool InStubRegion(uint64_t addr) {
	return g != nullptr && addr >= reinterpret_cast<uint64_t>(g->stub_base) &&
	       addr < reinterpret_cast<uint64_t>(g->stub_base) + kStubRegionSize;
}

// Host memory protection of the region containing addr. Returns false if unmapped.
bool QueryRegion(uint64_t addr, uint64_t* base, uint64_t* size, vm_prot_t* prot) {
	mach_vm_address_t              a = addr;
	mach_vm_size_t                 s = 0;
	vm_region_basic_info_data_64_t info {};
	mach_msg_type_number_t         count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t                    object {};
	if (mach_vm_region(mach_task_self(), &a, &s, VM_REGION_BASIC_INFO_64,
	                   reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS) {
		return false;
	}
	if (a > addr) {
		return false; // addr itself is unmapped
	}
	*base = a;
	*size = s;
	*prot = info.protection;
	return true;
}

const char* HostSymbol(uint64_t addr) {
	Dl_info info {};
	if (dladdr(reinterpret_cast<void*>(addr), &info) != 0 && info.dli_sname != nullptr) {
		return info.dli_sname;
	}
	return "?";
}

// ---------------------------------------------------------------------------------------------
// Guest call core
// ---------------------------------------------------------------------------------------------
struct GuestCallArgs {
	uint64_t        gpr[6] {};
	const uint64_t* stack       = nullptr;
	size_t          stack_count = 0;
	const __uint128_t* xmm      = nullptr; // 8 entries or null
};

struct GuestCallResult {
	uint64_t    rax = 0;
	uint64_t    rdx = 0;
	__uint128_t xmm0 = 0;
};

GuestCallResult RunGuest(uint64_t entry, const GuestCallArgs& a, uint64_t stack_top) {
	auto* ts = GetThreadState();
	auto& st = State(ts);
	g->guest_calls.fetch_add(1, std::memory_order_relaxed);

	if (ts->fs_base == 0 && g->fs_provider != nullptr) {
		ts->fs_base = g->fs_provider();
	}
	st.fs_cached = ts->fs_base;

	const uint64_t saved_rip = st.rip;
	const uint64_t saved_rsp = st.gregs[X86::REG_RSP];

	uint64_t top = stack_top;
	if (top == 0) {
		if (ts->depth > 0) {
			top = saved_rsp - kRedZone;
		} else {
			if (ts->own_stack == nullptr) {
				ts->own_stack = MapGuestPages(kOwnStackSize, "guest stack");
			}
			top = reinterpret_cast<uint64_t>(ts->own_stack) + kOwnStackSize - 64;
		}
	}
	// Stack args land at [base], [base+8]...; FEX pushes the return address at R-16, so the callee
	// sees arg0 at entry RSP+8. R must be 8 mod 16 for the SysV entry alignment (RSP+8 aligned).
	uint64_t base = (top - a.stack_count * 8u) & ~uint64_t {15};
	for (size_t i = 0; i < a.stack_count; i++) {
		reinterpret_cast<uint64_t*>(base)[i] = a.stack[i];
	}
	st.gregs[X86::REG_RSP] = base + 8;

	static constexpr uint32_t kArgRegs[6] = {X86::REG_RDI, X86::REG_RSI, X86::REG_RDX,
	                                         X86::REG_RCX, X86::REG_R8,  X86::REG_R9};
	for (int i = 0; i < 6; i++) {
		st.gregs[kArgRegs[i]] = a.gpr[i];
	}
	if (a.xmm != nullptr) {
		for (int i = 0; i < 8; i++) {
			std::memcpy(st.xmm.sse.data[i], &a.xmm[i], 16);
		}
	}
	st.gregs[X86::REG_RAX] = 8; // AL = vector registers used, for variadic callees

	ts->depth++;
	g->ctx->HandleCallback(ts->thread, entry);
	ts->depth--;

	GuestCallResult r;
	r.rax = st.gregs[X86::REG_RAX];
	r.rdx = st.gregs[X86::REG_RDX];
	std::memcpy(&r.xmm0, st.xmm.sse.data[0], 16);

	st.rip                 = saved_rip;
	st.gregs[X86::REG_RSP] = saved_rsp;
	return r;
}

// ---------------------------------------------------------------------------------------------
// FEX frontend hooks
// ---------------------------------------------------------------------------------------------
class KytySignalDelegator final : public FEXCore::SignalDelegator {
public:
	uintptr_t GetThunkCallbackRET() const override {
		return g != nullptr ? reinterpret_cast<uintptr_t>(g->callback_ret) : 0;
	}
};

class KytyThunkHandler final : public FEXCore::ThunkHandler {
public:
	FEXCore::ThunkedFunction* LookupThunk(const FEXCore::IR::SHA256Sum& /*sha*/) override { return nullptr; }
};

void DispatchHle(FEXCore::Core::CpuStateFrame* frame, uint64_t index) {
	auto&       st    = frame->State;
	const auto& entry = g->stubs[index];
	auto*       ts    = __builtin_expect(t_state != nullptr, 1) ? t_state : GetThreadState();

	const uint64_t rip = st.rip;
	const auto*    sp  = reinterpret_cast<const uint64_t*>(st.gregs[X86::REG_RSP]);
	// sp[0] = return address into guest; sp[1..] = SysV stack arguments.

	const uint64_t guest_ret_rip = sp[0];
	if (__builtin_expect(ts->hle_regs_depth < ThreadState::kMaxHleRegsDepth, 1)) {
		auto& r = ts->hle_regs[ts->hle_regs_depth++];
		std::memcpy(r.gpr, st.gregs, sizeof(r.gpr));
		r.rip = guest_ret_rip;
	}

	// Up to 10 stack argument words are forwarded (the callee's signature is unknown). A thread's
	// first frames sit just below the top of its stack mapping, so never read past a page boundary
	// unless the next page is mapped.
	uint64_t stack_args[10] {};
	{
		const uint64_t first = reinterpret_cast<uint64_t>(sp + 1);
		const uint64_t limit = (first | (kHostPage - 1)) + 1;
		if (__builtin_expect(first + sizeof(stack_args) <= limit, 1)) {
			std::memcpy(stack_args, sp + 1, sizeof(stack_args));
		} else {
			uint64_t  base = 0;
			uint64_t  size = 0;
			vm_prot_t prot = 0;
			uint64_t  end  = limit;
			if (QueryRegion(limit, &base, &size, &prot) && (prot & VM_PROT_READ) != 0) {
				end = base + size;
			}
			const size_t words = std::min<uint64_t>(10, (end > first) ? (end - first) / 8 : 0);
			std::memcpy(stack_args, sp + 1, words * 8);
		}
	}

	HostCallFrame f {};
	f.fn   = entry.host_fn;
	f.x[0] = st.gregs[X86::REG_RDI];
	f.x[1] = st.gregs[X86::REG_RSI];
	f.x[2] = st.gregs[X86::REG_RDX];
	f.x[3] = st.gregs[X86::REG_RAX]; // the stub moved RCX to RAX: `syscall` overwrites RCX and R11
	f.x[4] = st.gregs[X86::REG_R8];
	f.x[5] = st.gregs[X86::REG_R9];
	f.x[6] = stack_args[0];
	f.x[7] = stack_args[1];
	for (int i = 0; i < 8; i++) {
		f.stack[i] = stack_args[2 + i];
		*reinterpret_cast<__uint128_t*>(&f.v[i]) = *reinterpret_cast<const __uint128_t*>(st.xmm.sse.data[i]);
	}

	const auto n = g->hle_calls.fetch_add(1, std::memory_order_relaxed);
	// Restrict a diagnostic HLE trace to one guest module without spending its
	// quota on unrelated rendering/thread calls (begin:end, end exclusive).
	static const auto trace_caller = [] {
		const char* range = std::getenv("KYTY_FEX_TRACE_CALLER");
		const char* colon = range != nullptr ? std::strchr(range, ':') : nullptr;
		return std::pair<uint64_t, uint64_t> {
		    range != nullptr ? std::strtoull(range, nullptr, 0) : 0,
		    colon != nullptr ? std::strtoull(colon + 1, nullptr, 0) : UINT64_MAX};
	}();
	const bool trace = __builtin_expect(g->trace_limit > 0, 0) && Libs::Graphics::TraceWindow::Active() &&
	                   guest_ret_rip >= trace_caller.first && guest_ret_rip < trace_caller.second &&
	                   g->traced.load(std::memory_order_relaxed) < g->trace_limit &&
	                   g->traced.fetch_add(1, std::memory_order_relaxed) < g->trace_limit;
	if (__builtin_expect(trace, 0)) {
		std::fprintf(stderr,
		             "[fex] HLE #%llu -> %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) from guest 0x%llx\n",
		             static_cast<unsigned long long>(n), entry.name,
		             static_cast<unsigned long long>(f.x[0]), static_cast<unsigned long long>(f.x[1]),
		             static_cast<unsigned long long>(f.x[2]), static_cast<unsigned long long>(f.x[3]),
		             static_cast<unsigned long long>(f.x[4]), static_cast<unsigned long long>(f.x[5]),
		             static_cast<unsigned long long>(guest_ret_rip));
	}

	LastCallEnter(entry.name, f.x[0], f.x[1]);
	kyty_fex_host_call(&f);
	LastCallLeave(entry.name, f.ret_x0, f.x[0], f.x[1]);

	if (__builtin_expect(trace, 0)) {
		uint64_t tid = 0;
		pthread_threadid_np(nullptr, &tid);
		std::fprintf(stderr, "[fex] HLE #%llu <- %s = 0x%llx (returning to guest 0x%llx) tid=%llu\n",
		             static_cast<unsigned long long>(n), entry.name,
		             static_cast<unsigned long long>(f.ret_x0), static_cast<unsigned long long>(guest_ret_rip),
		             static_cast<unsigned long long>(tid));
	}

	if (__builtin_expect(ts->hle_regs_depth > 0, 1)) {
		--ts->hle_regs_depth;
	}
	st.gregs[X86::REG_RAX] = f.ret_x0;
	st.gregs[X86::REG_RDX] = f.ret_x1;
	*reinterpret_cast<__uint128_t*>(st.xmm.sse.data[0]) = *reinterpret_cast<const __uint128_t*>(&f.ret_v0);
	st.rip = rip + 2; // continue at the stub's `ret`
	// `syscall` set RCX/R11 to the return RIP / RFLAGS; both are caller-saved, so leave them.
}

class KytySyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
	void HandleSyscall(FEXCore::Core::CpuStateFrame* frame) override {
		auto&          st  = frame->State;
		const uint64_t rip = st.rip;
		if (__builtin_expect(InStubRegion(rip), 1)) {
			const uint64_t index = (rip - reinterpret_cast<uint64_t>(g->stub_base)) / kStubSlotSize;
			if (__builtin_expect(index < g->stub_count.load(std::memory_order_acquire), 1)) {
				DispatchHle(frame, index);
				return;
			}
		}
		// A raw `syscall` in guest code. PS5 games reach the kernel through libkernel (HLE), so
		// this is unexpected: report it and fail the call.
		const auto n = g->guest_syscalls.fetch_add(1, std::memory_order_relaxed);
		if (n < 32) {
			std::fprintf(stderr, "[fex] guest raw syscall %llu at rip=0x%llx: returning -ENOSYS\n",
			             static_cast<unsigned long long>(st.gregs[X86::REG_RAX]),
			             static_cast<unsigned long long>(rip));
		}
		st.gregs[X86::REG_RAX] = static_cast<uint64_t>(-38);
		st.rip                 = rip + 2;
	}

	FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* /*thread*/,
	                                                            uint64_t address) override {
		uint64_t  base = 0;
		uint64_t  size = 0;
		vm_prot_t prot = 0;
		if (!QueryRegion(address, &base, &size, &prot) || (prot & VM_PROT_READ) == 0) {
			std::fprintf(stderr, "[fex] guest executes unmapped/unreadable memory at 0x%llx\n",
			             static_cast<unsigned long long>(address));
			return {0, 0, false};
		}
		if ((prot & VM_PROT_EXECUTE) != 0) {
			// Guest memory is never host-executable on arm64, so this is host (arm64) code:
			// a host function pointer leaked to the guest without an HLE stub.
			std::fprintf(stderr,
			             "[fex] FATAL: guest jumped into host code at 0x%llx (%s). Wrap it with "
			             "GuestCpu::GetHostFunctionStub().\n",
			             static_cast<unsigned long long>(address), HostSymbol(address));
			std::abort();
		}
		return {base, size, (prot & VM_PROT_WRITE) != 0};
	}

	std::optional<FEXCore::ExecutableFileSectionInfo>
	LookupExecutableFileSection(FEXCore::Core::InternalThreadState* /*thread*/, uint64_t /*addr*/) override {
		return std::nullopt;
	}
};

KytySignalDelegator* g_signals  = nullptr;
KytySyscallHandler*  g_syscalls = nullptr;
KytyThunkHandler*    g_thunks   = nullptr;

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
void Init() {
	if (g != nullptr) {
		return;
	}
	LogMan::Msg::InstallHandler(MsgHandler);
	LogMan::Throw::InstallHandler(AssertHandler);

	g              = new Globals();
	g->trace_limit = EnvInt("KYTY_FEX_TRACE", 64);

	// ReloadMetaLayer() rebuilds the meta layer from the config layers and drops earlier Set()s,
	// so it must run before the overrides (otherwise IS64BIT_MODE is lost and the JIT uses the
	// 32-bit static register map while the frontend decodes 64-bit code).
	FEXCore::Config::Initialize();
	FEXCore::Config::ReloadMetaLayer();
	FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
	FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, EnvInt("KYTY_FEX_MULTIBLOCK", 1) ? "1" : "0");
	FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, EnvInt("KYTY_FEX_TSO", 1) ? "1" : "0");
	FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS,
	                     std::to_string(EnvInt("KYTY_FEX_SMC", FEXCore::Config::CONFIG_SMC_MTRACK)));
	if (const char* dump = std::getenv("KYTY_FEX_DUMPIR"); dump != nullptr) {
		FEXCore::Config::Set(FEXCore::Config::CONFIG_DUMPIR, dump); // e.g. "stderr" or a directory
	}
	if (const int maxinst = EnvInt("KYTY_FEX_MAXINST", 0); maxinst > 0) {
		FEXCore::Config::Set(FEXCore::Config::CONFIG_MAXINST, std::to_string(maxinst));
	}
	if (EnvInt("KYTY_FEX_DISASM", 0) != 0) {
		FEXCore::Config::Set(FEXCore::Config::CONFIG_DISASSEMBLE, "1");
	}

	// Guest-visible helper code: CALLBACKRET sentinel and HLE stubs. Readable (FEX decodes it),
	// never host-executable.
	g->callback_ret    = MapGuestPages(0x4000, "callback ret");
	g->callback_ret[0] = 0x0F;
	g->callback_ret[1] = 0x3E;
	mprotect(g->callback_ret, 0x4000, PROT_READ);

	g->stub_base = MapGuestPages(kStubRegionSize, "hle stubs");
	g->stubs.reserve(kStubRegionSize / kStubSlotSize);

	g->ctx = FEXCore::Context::Context::CreateNewContext(MakeHostFeatures());
	g_signals  = new KytySignalDelegator();
	g_syscalls = new KytySyscallHandler();
	g_thunks   = new KytyThunkHandler();
	g->ctx->SetSignalDelegator(g_signals);
	g->ctx->SetSyscallHandler(g_syscalls);
	g->ctx->SetThunkHandler(g_thunks);
	if (!g->ctx->InitCore()) {
		std::fprintf(stderr, "[fex] InitCore failed\n");
		std::abort();
	}
	if (const int period = EnvInt("KYTY_FEX_STATS", 0); period > 0) {
		std::thread([period] {
			Stats prev {};
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(period));
				const auto st = GetStats();
				std::fprintf(stderr,
				             "[fex] stats/%ds: hle_calls=%llu (+%llu) guest_calls=%llu (+%llu) redirected=%llu (+%llu) "
				             "unaligned=%llu stubs=%llu\n",
				             period, static_cast<unsigned long long>(st.hle_calls),
				             static_cast<unsigned long long>(st.hle_calls - prev.hle_calls),
				             static_cast<unsigned long long>(st.guest_calls),
				             static_cast<unsigned long long>(st.guest_calls - prev.guest_calls),
				             static_cast<unsigned long long>(st.redirected_host_calls),
				             static_cast<unsigned long long>(st.redirected_host_calls - prev.redirected_host_calls),
				             static_cast<unsigned long long>(g->unaligned.load()), static_cast<unsigned long long>(st.stubs));
				prev = st;
			}
		}).detach();
	}
	std::fprintf(stderr, "[fex] FEXCore initialised (TSO=%d multiblock=%d smc=%d trace=%d)\n",
	             EnvInt("KYTY_FEX_TSO", 1), EnvInt("KYTY_FEX_MULTIBLOCK", 1),
	             EnvInt("KYTY_FEX_SMC", FEXCore::Config::CONFIG_SMC_MTRACK), g->trace_limit);
}

uint64_t CallGuest(uint64_t entry, const uint64_t* args, size_t count, uint64_t guest_stack_top) {
	GuestCallArgs a;
	for (size_t i = 0; i < count && i < 6; i++) {
		a.gpr[i] = args[i];
	}
	if (count > 6) {
		a.stack       = args + 6;
		a.stack_count = count - 6;
	}
	return RunGuest(entry, a, guest_stack_top).rax;
}

uint64_t GetHostFunctionStub(uint64_t host_fn, const char* name) {
	if (host_fn == 0) {
		return 0;
	}
	if (g == nullptr) {
		Init(); // HLE symbol registration can run before main() calls Init()
	}
	if (InStubRegion(host_fn)) {
		return host_fn;
	}
	std::lock_guard lock(g->stub_mutex);
	if (auto it = g->stub_by_fn.find(host_fn); it != g->stub_by_fn.end()) {
		return reinterpret_cast<uint64_t>(g->stub_base) + it->second * kStubSlotSize;
	}
	const size_t index = g->stub_count.load(std::memory_order_relaxed);
	if ((index + 1) * kStubSlotSize > kStubRegionSize) {
		std::fprintf(stderr, "[fex] out of HLE stub slots\n");
		std::abort();
	}
	g->stub_names.emplace_back(name != nullptr ? name : HostSymbol(host_fn));
	g->stubs.push_back({host_fn, g->stub_names.back().c_str()});

	// mov rax, rcx ; syscall ; ret ; int3 x2
	// (`syscall` clobbers RCX, the 4th SysV argument; RAX only carries AL for variadic calls.)
	uint8_t* slot   = g->stub_base + index * kStubSlotSize;
	const uint8_t code[kStubSlotSize] = {0x48, 0x89, 0xC8, 0x0F, 0x05, 0xC3, 0xCC, 0xCC};
	std::memcpy(slot, code, sizeof(code));

	g->stub_by_fn.emplace(host_fn, static_cast<uint32_t>(index));
	g->stub_count.store(index + 1, std::memory_order_release);
	return reinterpret_cast<uint64_t>(slot);
}

bool IsHostFunctionStub(uint64_t addr) { return InStubRegion(addr); }

void SetThreadFsBase(uint64_t fs_base) {
	auto* ts    = GetThreadState();
	ts->fs_base = fs_base;
	State(ts).fs_cached = fs_base;
}

uint64_t GetThreadFsBase() { return t_state != nullptr ? t_state->fs_base : 0; }

void SetFsBaseProvider(FsBaseProvider provider) {
	Init();
	g->fs_provider = provider;
}

void InvalidateCode(uint64_t start, uint64_t length) {
	if (g != nullptr && g->ctx) {
		// The caller must hold CodeInvalidationMutex exclusively (FEX asserts it with try_lock(),
		// which would otherwise take the lock and never release it, deadlocking every JIT thread).
		std::unique_lock lock(g->ctx->GetCodeInvalidationMutex());
		g->ctx->InvalidateCodeBuffersCodeRange(start, length);
	}
}

bool GetCurrentGuestRegs(GuestRegs* out) {
	if (t_state == nullptr || t_state->hle_regs_depth == 0) {
		return false;
	}
	*out = t_state->hle_regs[t_state->hle_regs_depth - 1];
	return true;
}

bool GetFaultGuestLocation(uint64_t host_pc, GuestFaultLocation* out) {
	// No new thread state, loader locks, or mutation of the faulting CPU context.
	if (out == nullptr || g == nullptr || !g->ctx || t_state == nullptr || t_state->thread == nullptr ||
	    t_state->thread->CurrentFrame == nullptr || !g->ctx->IsAddressInCodeBuffer(t_state->thread, host_pc)) {
		return false;
	}
	out->saved_rip = State(t_state).rip;
	out->block_entry = g->ctx->GetGuestBlockEntry(t_state->thread);
	out->rip = g->ctx->RestoreRIPFromHostPC(t_state->thread, host_pc);
	return true;
}

Stats GetStats() {
	Stats s {};
	if (g != nullptr) {
		s.hle_calls             = g->hle_calls.load();
		s.guest_calls           = g->guest_calls.load();
		s.redirected_host_calls = g->redirected.load();
		s.stubs                 = g->stub_count.load();
	}
	return s;
}

static_assert(offsetof(__darwin_arm_thread_state64, __fp) == 29 * 8 &&
                  offsetof(__darwin_arm_thread_state64, __lr) == 30 * 8,
              "x0..x30 must be contiguous for FEX's GPR array");

bool HandleHostFault(int /*sig*/, void* siginfo, void* ucontext) {
	if (g == nullptr) {
		return false;
	}
	auto*          uc   = static_cast<ucontext_t*>(ucontext);
	auto*          mc   = uc->uc_mcontext;
	const uint64_t addr = reinterpret_cast<uint64_t>(static_cast<siginfo_t*>(siginfo)->si_addr);
	auto*          ts   = t_state;

	// 1. FEX JIT temp-buffer guard page: restart compilation with a larger buffer
	//    (Linux: SignalDelegator::HandleFrontendSIGSEGV).
	if (ts != nullptr && ts->thread != nullptr && ts->thread->JITGuardPage != 0 &&
	    addr >= ts->thread->JITGuardPage && addr < ts->thread->JITGuardPage + 0x4000) {
		auto*    t = ts->thread;
		uint64_t gprs[32] {};
		for (int i = 0; i < 29; ++i) {
			gprs[i] = mc->__ss.__x[i];
		}
		gprs[29] = mc->__ss.__fp;
		gprs[30] = mc->__ss.__lr;
		gprs[31] = mc->__ss.__sp;
		__uint128_t fprs[32];
		for (int i = 0; i < 32; ++i) {
			fprs[i] = mc->__ns.__v[i];
		}
		uint64_t pc = mc->__ss.__pc;
		FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(t->RestartJump, t->JITGuardOverflowArgument, gprs, fprs, &pc);
		for (int i = 0; i < 29; ++i) {
			mc->__ss.__x[i] = gprs[i];
		}
		mc->__ss.__fp = gprs[29];
		mc->__ss.__lr = gprs[30];
		mc->__ss.__sp = gprs[31];
		for (int i = 0; i < 32; ++i) {
			mc->__ns.__v[i] = fprs[i];
		}
		mc->__ss.__pc = pc;
		return true;
	}

	// 2. Unaligned atomic / acquire-release access from JIT code (TSO emulation uses LDAPR/STLR/LSE,
	//    which require alignment): FEX backpatches the instruction (Linux: SigbusHandler).
	//    Darwin reports it as a data abort with DFSC 0x21 (alignment fault).
	{
		const uint32_t esr    = static_cast<uint32_t>(mc->__es.__esr);
		const uint32_t ec_da  = (esr >> 26u) & 0x3fu;
		const bool     align  = (ec_da == 0x24 || ec_da == 0x25) && (esr & 0x3fu) == 0x21u;
		const uint64_t pc_now = mc->__ss.__pc;
		if (align && ts != nullptr && ts->thread != nullptr && g->ctx->IsAddressInCodeBuffer(ts->thread, pc_now)) {
			// __x[0..28], __fp, __lr are contiguous: the 31-entry GPR array FEX expects.
			const auto result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
			    ts->thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc_now, &mc->__ss.__x[0]);
			if (result.has_value()) {
				mc->__ss.__pc = pc_now + *result;
				g->unaligned.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}
	}

	// 3. Call-ret stack over/underflow into its guard pages: reset it (SyscallsSMCTracking.cpp).
	if (ts != nullptr && ts->callret_alloc != 0 && addr >= ts->callret_alloc &&
	    addr < ts->callret_alloc + kCallRetAllocSize) {
		mc->__ss.__x[25] = CallRetDefault(ts->callret_alloc);
		return true;
	}

	// 4. Host code called a guest function pointer directly: instruction fetch from a readable,
	//    non-executable guest page. Redirect into the bridge, which runs it under FEX.
	const uint64_t pc = mc->__ss.__pc;
	const uint32_t ec = (static_cast<uint32_t>(mc->__es.__esr) >> 26u) & 0x3fu;
	if ((ec == 0x20 || ec == 0x21 || addr == pc) && pc == addr) {
		uint64_t  base = 0;
		uint64_t  size = 0;
		vm_prot_t prot = 0;
		if (!QueryRegion(pc, &base, &size, &prot) || (prot & VM_PROT_READ) == 0 ||
		    (prot & VM_PROT_EXECUTE) != 0) {
			return false;
		}
		if (ts == nullptr) {
			ts = GetThreadState(); // not async-signal-safe, but only reached on a synchronous fault
		}
		if (ts->bridge_depth >= kMaxBridgeDepth) {
			return false;
		}
		auto& f = ts->bridge[ts->bridge_depth++];
		f       = {};
		f.target = pc;
		for (int i = 0; i < 8; i++) {
			f.x[i] = mc->__ss.__x[i];
			f.v[i] = mc->__ns.__v[i];
		}
		const auto* host_sp = reinterpret_cast<const uint64_t*>(mc->__ss.__sp);
		for (int i = 0; i < 8; i++) {
			f.stack[i] = host_sp[i];
		}
		mc->__ss.__x[0] = reinterpret_cast<uint64_t>(&f);
		mc->__ss.__pc   = reinterpret_cast<uint64_t>(&kyty_fex_bridge_entry);
		g->redirected.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	return false;
}

} // namespace Loader::GuestCpu

// Hook from Common::VirtualMemory::FlushInstructionCache (common/ cannot depend on loader/).
extern "C" void kyty_guest_code_modified(uint64_t address, uint64_t size) {
	Loader::GuestCpu::InvalidateCode(address, size);
}

extern "C" Loader::GuestCpu::Detail::HostCallFrame* kyty_fex_bridge_run(Loader::GuestCpu::Detail::HostCallFrame* f) {
	using namespace Loader::GuestCpu;
	// AAPCS -> SysV: x0-x5 -> rdi..r9; x6, x7 and the host stack words -> guest stack arguments.
	uint64_t stack_args[10];
	stack_args[0] = f->x[6];
	stack_args[1] = f->x[7];
	for (int i = 0; i < 8; i++) {
		stack_args[2 + i] = f->stack[i];
	}
	GuestCallArgs a;
	for (int i = 0; i < 6; i++) {
		a.gpr[i] = f->x[i];
	}
	a.stack       = stack_args;
	a.stack_count = 10;
	a.xmm         = f->v;
	const auto r  = RunGuest(f->target, a, 0);
	f->ret_x0     = r.rax;
	f->ret_x1     = r.rdx;
	f->ret_v0     = r.xmm0;
	t_state->bridge_depth--;
	return f;
}
