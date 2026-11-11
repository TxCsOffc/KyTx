// Pipeline lab for the MSL that the KyTx RDNA->MSL backend emits (see tools/bench/msl_static_stats.py for the static view).
//
// Build/run: tools/bench/run_msl_kernel_lab.sh   (does not start the emulator; needs only the Xcode command line tools)
//
// What it does, for one dumped .metal file at a time (so the CPU load stays low: one MTLCompilerService job at a time):
//   1. compile  every requested VARIANT of the source with newLibraryWithSource and create the compute pipeline the way KyTx does
//      (MTLComputePipelineDescriptor, MTLPipelineOptionNone) and report: library compile ms, pipeline (back end) compile ms,
//      maxTotalThreadsPerThreadgroup, threadExecutionWidth, staticThreadgroupMemoryLength. A low maxTotalThreadsPerThreadgroup
//      (< 1024) means the register allocator ran out of room: high register pressure.
//   2. optionally (--bench) build synthetic inputs from the kernel's own argument-buffer struct and time every variant on the GPU
//      with MTLCommandBuffer GPUStartTime/GPUEndTime, interleaving the variants round by round to cancel drift.
//
// Variants are text transformations of the source (a variant is "tok+tok+..."):
//   base      the source as emitted; options as KyTx uses them (MTLMathModeSafe + MTLMathFloatingPointFunctionsPrecise, see
//             ApplyMslMathMode in metalShader.mm). NOTE: language version: KyTx guards 4_0 / 3_2 with '#if defined(MTLLanguageVersion4_0)',
//             which is false for an enumerator, so the emulator really compiles as MSL 3.0. 'base' therefore uses 3.0.
//   l32 l40   language version 3.2 / 4.0 instead of 3.0
//   fast      MTLMathModeFast (KYTY_MSL_MATH=fast)         relaxed   MTLMathModeRelaxed
//   fnfast    safe math, but mathFloatingPointFunctions = Fast (fast exp2/log2/sin/sqrt/rsqrt implementations only)
//   nobounds  EXPERIMENT: every buffer-load bounds check removed ('(idx < (sizes[N]>>2) ? load : 0u)' -> 'load', byte-range checks -> true)
//   noftz     EXPERIMENT: kyty_ftz() is the identity
//   noguard   EXPERIMENT: the runaway-loop guard 'if (++kyty_loopN > 8192u) { break; }' is removed
//   g2 g4     bench only: multiply the group count of this variant (scaling checks)
//   t1 t2 t4  bench only: multiply the threads per group of this variant (default t2 for nohalf, t1 otherwise)
//   nosel     EXPERIMENT: the exec-mask zeroing selects 'vN = (vM ? vK : 0u)' become 'vN = vK' (what do the per-instruction lane selects cost)
//   nohalf    EXPERIMENT: the second wave64 half is deleted (every '_h' statement removed). Half the guest lanes per host thread; to
//             compare it with the pair kernel the bench dispatches the same groups with twice as many threads (= one guest lane per host
//             thread, i.e. the cost of NOT doing the pair lowering, ignoring the cross-lane / LDS semantics the pair lowering exists for).
//
// Usage: msl_kernel_lab [options] file.metal
//   --variants a,b,c     variants to run (default: base,nobounds,fast,noftz,nobounds+noftz+fast)
//   --no-pso             library compile only (pixel shaders)
//   --cold               defeat the Metal compiler caches (nonce comment + a never-true early return) so compile ms are cold-cache numbers
//   --dump-dir DIR       write each transformed source there (local debugging)
//   --bufset PATH/PREFIX replay real buffer dumps (PREFIX-b<slot>-*.bin) instead of synthetic ones; sizes[] = real size, --slack N zero bytes behind
//   --bench              GPU timing (compute kernels only); options below
//   --compare            with --bench: run every variant once on cleared outputs and compare the written texels with base (path check)
//   --groups X[,Y[,Z]]   threadgroups per dispatch of the base variant      --tg X[,Y[,Z]]  host threads per group (default 32)
//   --reps N --warmup N  repetitions (default 30 / 3)                     --per-cb N     dispatches per command buffer (default 1)
//   --fill small|float|zero|rand   default content of the buffers         --bufbytes N   size of each buffer (default 8 MiB)
//   --set B:D=V          dword D of buffer B := V (decimal, 0x.., or f:1.5 for a float)    --setrange B:D0-D1=V   a run of dwords
//   --fillbuf B=mode     override the fill mode of one buffer (small|float|zero|rand|one)   --tilepat B=W   dword i := (i % W) | ((i / W) << 16)             --pc D=V       push-constant dword D := V
//   --tex WxH            size of every 2D texture (default 1024x1024)    --texfill float|zero
//   --max-ms X           give up a variant whose single dispatch takes longer (default 400 ms)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

static std::string ReadFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return {};
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static std::vector<std::string> Split(const std::string& s, char sep) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : s) {
		if (c == sep) {
			out.push_back(cur);
			cur.clear();
		} else cur += c;
	}
	out.push_back(cur);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Text transformations
// ---------------------------------------------------------------------------------------------------------------------------------

static size_t MatchOpenBack(const std::string& s, size_t close_pos) {
	int depth = 0;
	for (size_t i = close_pos + 1; i-- > 0;) {
		const char c = s[i];
		if (c == ')') ++depth;
		else if (c == '(') {
			if (--depth == 0) return i;
		} else if (c == '\n') return std::string::npos;
	}
	return std::string::npos;
}

static size_t MatchCloseFwd(const std::string& s, size_t open_pos) {
	int depth = 0;
	for (size_t i = open_pos; i < s.size(); ++i) {
		const char c = s[i];
		if (c == '(') ++depth;
		else if (c == ')') {
			if (--depth == 0) return i;
		} else if (c == '\n') return std::string::npos;
	}
	return std::string::npos;
}

// '(COND < (sizes[N] >> 2) ? LOAD : 0u)'  ->  '(LOAD)'   (a leading 'pred && ' of COND is kept: '(pred ? LOAD : 0u)')
static std::string StripDwordBounds(const std::string& s, int* sites, int* skipped) {
	static const char* kMark = " < (sizes[";
	static const char* kTail = "] >> 2) ? ";
	std::string out;
	out.reserve(s.size());
	size_t last = 0, pos = 0;
	while ((pos = s.find(kMark, pos)) != std::string::npos) {
		size_t p = pos + std::strlen(kMark), q = p;
		while (q < s.size() && isdigit((unsigned char)s[q])) ++q;
		if (q == p || s.compare(q, std::strlen(kTail), kTail) != 0) {
			pos = p;
			continue;
		}
		const size_t load_begin = q + std::strlen(kTail);
		int depth = 0;
		size_t e = load_begin;
		for (; e < s.size(); ++e) {
			const char c = s[e];
			if (c == '(') ++depth;
			else if (c == ')') {
				if (depth == 0) break;
				--depth;
			} else if (c == '\n') {
				e = std::string::npos;
				break;
			}
		}
		size_t start = std::string::npos;
		{
			int d = 0;
			for (size_t i = pos; i-- > 0;) {
				const char c = s[i];
				if (c == ')') ++d;
				else if (c == '(') {
					if (d == 0) {
						start = i;
						break;
					}
					--d;
				} else if (c == '\n') break;
			}
		}
		if (e == std::string::npos || e >= s.size() || start == std::string::npos || start < last) {
			++*skipped;
			pos = p;
			continue;
		}
		std::string load = s.substr(load_begin, e - load_begin);
		if (load.size() < 5 || load.compare(load.size() - 5, 5, " : 0u") != 0) {
			++*skipped;
			pos = p;
			continue;
		}
		load.resize(load.size() - 5);
		// last top-level '&&' of the condition
		size_t pred_end = std::string::npos;
		{
			int d = 0;
			for (size_t i = start + 1; i + 1 < pos; ++i) {
				const char c = s[i];
				if (c == '(') ++d;
				else if (c == ')') --d;
				else if (d == 0 && c == '&' && s[i + 1] == '&') pred_end = i;
			}
		}
		out.append(s, last, start - last);
		if (pred_end != std::string::npos) {
			std::string pred = s.substr(start + 1, pred_end - (start + 1));
			while (!pred.empty() && pred.back() == ' ') pred.pop_back();
			out += "(" + pred + " ? " + load + " : 0u)";
		} else {
			out += "(" + load + ")";
		}
		last = e + 1;
		pos = e + 1;
		++*sites;
	}
	out.append(s, last, std::string::npos);
	return out;
}

// 'A <= sizes[N] && K <= sizes[N] - A'  ->  'true'   (A is a parenthesised byte offset)
static std::string StripRangeBounds(const std::string& s, int* sites, int* skipped) {
	static const char* kMark = " <= sizes[";
	std::string out;
	out.reserve(s.size());
	size_t last = 0, pos = 0;
	while ((pos = s.find(kMark, pos)) != std::string::npos) {
		size_t p = pos + std::strlen(kMark), q = p;
		while (q < s.size() && isdigit((unsigned char)s[q])) ++q;
		if (q == p || s.compare(q, 5, "] && ") != 0 || pos == 0 || s[pos - 1] != ')') {
			pos = p;
			continue;
		}
		const size_t a_start = MatchOpenBack(s, pos - 1);
		// right side: <const>u <= sizes[N] - (A)
		size_t r = q + 5;
		const size_t r2 = s.find(" <= sizes[", r);
		if (a_start == std::string::npos || a_start < last || r2 == std::string::npos || r2 - r > 16) {
			++*skipped;
			pos = p;
			continue;
		}
		size_t t = r2 + std::strlen(kMark);
		while (t < s.size() && isdigit((unsigned char)s[t])) ++t;
		if (s.compare(t, 4, "] - ") != 0 || t + 4 >= s.size() || s[t + 4] != '(') {
			++*skipped;
			pos = p;
			continue;
		}
		const size_t a2_end = MatchCloseFwd(s, t + 4);
		if (a2_end == std::string::npos) {
			++*skipped;
			pos = p;
			continue;
		}
		out.append(s, last, a_start - last);
		out += "true";
		last = a2_end + 1;
		pos = a2_end + 1;
		++*sites;
	}
	out.append(s, last, std::string::npos);
	return out;
}

static std::string FtzIdentity(const std::string& s, int* sites) {
	const std::string head = "inline float kyty_ftz(float v) {";
	const size_t a = s.find(head);
	if (a == std::string::npos) return s;
	const size_t b = s.find("\n}\n", a);
	if (b == std::string::npos) return s;
	*sites = 1;
	return s.substr(0, a) + head + "\n\treturn v;" + s.substr(b);
}

static std::string DropLoopGuard(const std::string& s, int* sites) {
	std::string out;
	out.reserve(s.size());
	size_t last = 0, pos = 0;
	while ((pos = s.find("if (++kyty_loop", pos)) != std::string::npos) {
		const size_t b = s.find("{ break; }", pos);
		if (b == std::string::npos || b - pos > 60) {
			pos += 10;
			continue;
		}
		out.append(s, last, pos - last);
		last = b + std::strlen("{ break; }");
		pos = last;
		++*sites;
	}
	out.append(s, last, std::string::npos);
	return out;
}

// EXPERIMENT 'nosel': 'vN = (vM ? vK : 0x00000000u);' -> 'vN = vK;'  (the exec-mask zeroing select of an inactive lane is dropped; inactive lanes
// then carry garbage instead of 0, which is wrong whenever such a value is used by an active lane or a ballot). Measures what those selects cost.
static bool IsIdent(char c);
static std::string DropZeroSelects(const std::string& s, int* sites) {
	static const std::string kTail = " : 0x00000000u);";
	std::string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		size_t e = s.find('\n', i);
		if (e == std::string::npos) e = s.size();
		const std::string line = s.substr(i, e - i);
		i = e + 1;
		bool done = false;
		size_t b = 0;
		while (b < line.size() && line[b] == '\t') ++b;
		const size_t eq = line.find(" = (v", b);
		const size_t q = line.find(" ? ", b);
		if (b < line.size() && line[b] == 'v' && eq != std::string::npos && q != std::string::npos && q > eq && line.size() > kTail.size() &&
		    line.compare(line.size() - kTail.size(), kTail.size(), kTail) == 0) {
			const std::string cond = line.substr(eq + 4, q - (eq + 4));
			const std::string val = line.substr(q + 3, line.size() - kTail.size() - (q + 3));
			bool ok = !cond.empty() && !val.empty() && val[0] == 'v';
			for (char c : cond) ok &= IsIdent(c);
			for (char c : val) ok &= IsIdent(c);
			if (ok) {
				out += line.substr(0, eq) + " = " + val + ";\n";
				++*sites;
				done = true;
			}
		}
		if (!done) {
			out += line;
			out += '\n';
		}
	}
	return out;
}

static bool IsIdent(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Does the line contain an identifier ending in "_h" (v123_h, kyty_phi_4_v5_h)?  Position of the first one, or npos.
static size_t FindHalfIdent(const std::string& l, size_t from = 0) {
	for (size_t i = from; i + 1 < l.size(); ++i) {
		if (l[i] == '_' && l[i + 1] == 'h' && i > 0 && isdigit((unsigned char)l[i - 1]) && (i + 2 >= l.size() || !IsIdent(l[i + 2]))) return i;
	}
	return std::string::npos;
}

static std::string DropHalf(const std::string& s, int* removed, int* rewritten) {
	std::string out;
	out.reserve(s.size() / 2 + 1024);
	size_t i = 0;
	while (i < s.size()) {
		size_t e = s.find('\n', i);
		if (e == std::string::npos) e = s.size();
		std::string line = s.substr(i, e - i);
		i = e + 1;
		if (FindHalfIdent(line) == std::string::npos) {
			out += line;
			out += '\n';
			continue;
		}
		// the zero-initialised register declaration stays (the identifier may be referenced by a rewritten line)
		size_t b = 0;
		while (b < line.size() && (line[b] == '\t' || line[b] == ' ')) ++b;
		std::string t = line.substr(b);
		if (t.find(" = (") != std::string::npos && t.find(")0;") != std::string::npos && t.size() < 64) {
			out += line;
			out += '\n';
			continue;
		}
		// LHS of an assignment / const declaration
		std::string lhs;
		{
			std::string u = t;
			if (u.compare(0, 6, "const ") == 0) {
				const size_t sp = u.find(' ', 6);
				if (sp != std::string::npos) u = u.substr(sp + 1);
			}
			size_t k = 0;
			while (k < u.size() && IsIdent(u[k])) ++k;
			if (k > 0 && u.compare(k, 3, " = ") == 0) lhs = u.substr(0, k);
		}
		const bool lhs_half = lhs.size() > 2 && lhs.compare(lhs.size() - 2, 2, "_h") == 0;
		const bool control = (t.compare(0, 4, "if (") == 0 || t.compare(0, 7, "while (") == 0 || t.compare(0, 6, "} else") == 0) && !t.empty() && t.back() == '{';
		if (lhs_half) {
			++*removed;
			continue;
		}
		if (!lhs.empty() || control) {
			// value of the first half stands in for the second: strip the suffix
			std::string r;
			size_t p = 0, q;
			while ((q = FindHalfIdent(line, p)) != std::string::npos) {
				r.append(line, p, q - p);
				p = q + 2;
			}
			r.append(line, p, std::string::npos);
			out += r;
			out += '\n';
			++*rewritten;
			continue;
		}
		++*removed; // a store / void call on the second half
	}
	return out;
}

struct VariantSpec {
	std::string name;
	bool fast = false, relaxed = false, fnfast = false;
	int lang = 30;
	bool nobounds = false, noftz = false, noguard = false, nohalf = false, nosel = false;
	uint32_t gmul = 1; // bench only: group count multiplier (g2 / g4 tokens)
	uint32_t tmul = 0; // bench only: threads-per-group multiplier (t1 / t2 / t4 tokens); 0 = default (2 for nohalf, else 1)
};

static VariantSpec ParseVariant(const std::string& name) {
	VariantSpec v;
	v.name = name;
	for (const std::string& tok : Split(name, '+')) {
		if (tok == "base") {
		} else if (tok == "fast") v.fast = true;
		else if (tok == "relaxed") v.relaxed = true;
		else if (tok == "fnfast") v.fnfast = true;
		else if (tok == "l32") v.lang = 32;
		else if (tok == "l40") v.lang = 40;
		else if (tok == "nobounds") v.nobounds = true;
		else if (tok == "noftz") v.noftz = true;
		else if (tok == "noguard") v.noguard = true;
		else if (tok == "nohalf") v.nohalf = true;
		else if (tok == "nosel") v.nosel = true;
		else if (tok.size() == 2 && tok[0] == 'g' && isdigit((unsigned char)tok[1])) v.gmul = (uint32_t)(tok[1] - '0');
		else if (tok.size() == 2 && tok[0] == 't' && isdigit((unsigned char)tok[1])) v.tmul = (uint32_t)(tok[1] - '0');
		else {
			std::fprintf(stderr, "unknown variant token '%s'\n", tok.c_str());
			std::exit(2);
		}
	}
	return v;
}

static std::string Transform(const std::string& src, const VariantSpec& v, std::string* note) {
	std::string s = src;
	char buf[256];
	if (v.nohalf) {
		int removed = 0, rewritten = 0;
		s = DropHalf(s, &removed, &rewritten);
		std::snprintf(buf, sizeof(buf), "nohalf(removed %d lines, rewrote %d) ", removed, rewritten);
		*note += buf;
	}
	if (v.nosel) {
		int n = 0;
		s = DropZeroSelects(s, &n);
		std::snprintf(buf, sizeof(buf), "nosel(%d) ", n);
		*note += buf;
	}
	if (v.nobounds) {
		int dw = 0, dw_skip = 0, rg = 0, rg_skip = 0;
		s = StripDwordBounds(s, &dw, &dw_skip);
		s = StripRangeBounds(s, &rg, &rg_skip);
		size_t left = 0;
		for (size_t p = 0; (p = s.find("sizes[", p)) != std::string::npos; p += 6) ++left;
		std::snprintf(buf, sizeof(buf), "nobounds(dword %d range %d, skipped %d, sizes[] left %zu) ", dw, rg, dw_skip + rg_skip, left);
		*note += buf;
	}
	if (v.noftz) {
		int n = 0;
		s = FtzIdentity(s, &n);
		*note += n ? "noftz " : "noftz(NOT FOUND) ";
	}
	if (v.noguard) {
		int n = 0;
		s = DropLoopGuard(s, &n);
		std::snprintf(buf, sizeof(buf), "noguard(%d) ", n);
		*note += buf;
	}
	return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------------------------------------------------------------

struct Compiled {
	VariantSpec spec;
	std::string note;
	bool lib_ok = false, pso_ok = false;
	double lib_ms = 0, pso_ms = 0;
	NSUInteger max_threads = 0, exec_width = 0, tg_mem = 0;
	std::string error;
	id<MTLLibrary> lib = nil;
	id<MTLFunction> fn = nil;
	id<MTLComputePipelineState> pso = nil;
	std::string source;
	uint32_t group_mult = 1, tg_mult = 1;
};

static void ApplyOptions(MTLCompileOptions* opts, const VariantSpec& v) {
	if (v.lang == 40) opts.languageVersion = MTLLanguageVersion4_0;
	else if (v.lang == 32) opts.languageVersion = MTLLanguageVersion3_2;
	else opts.languageVersion = MTLLanguageVersion3_0;
	if (@available(macOS 15.0, *)) {
		if (v.fast) opts.mathMode = MTLMathModeFast;
		else if (v.relaxed) opts.mathMode = MTLMathModeRelaxed;
		else {
			opts.mathMode = MTLMathModeSafe;
			opts.mathFloatingPointFunctions = v.fnfast ? MTLMathFloatingPointFunctionsFast : MTLMathFloatingPointFunctionsPrecise;
		}
	} else {
		opts.fastMathEnabled = v.fast ? YES : NO;
	}
}

static bool g_cold = false; // --cold: defeat the Metal compiler caches (they key on the source / the AIR) so compile times are cold-cache numbers

static std::string MakeCold(const std::string& s) {
	static uint64_t counter = 0;
	const uint64_t nonce = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count() + (++counter);
	char buf[200];
	std::string out = s;
	// kernels: a never-true early return changes the AIR (a cold back end compile); costs one compare at entry
	const std::string anchor = "(void)ab; (void)pc_data; (void)sizes;";
	const size_t a = out.find(anchor);
	if (a != std::string::npos && out.find("kernel void") != std::string::npos) {
		std::snprintf(buf, sizeof(buf), " if (tgid.x == 0xfffff000u + %lluu && liid == 0xffffffffu) { return; }", (unsigned long long)(nonce & 0xfff));
		out.insert(a + anchor.size(), buf);
	}
	std::snprintf(buf, sizeof(buf), "\n// lab-nonce %llu\n", (unsigned long long)nonce);
	out += buf;
	return out;
}

static void CompileVariant(id<MTLDevice> dev, const std::string& base_src, const VariantSpec& spec, bool make_pso, const std::string& dump_dir,
                           const std::string& tag, Compiled* out) {
	out->spec = spec;
	out->source = Transform(base_src, spec, &out->note);
	if (g_cold) out->source = MakeCold(out->source);
	out->group_mult = spec.gmul;
	out->tg_mult = spec.tmul ? spec.tmul : (spec.nohalf ? 2u : 1u);
	if (!dump_dir.empty()) {
		std::string path = dump_dir + "/" + tag + "." + spec.name + ".metal";
		std::ofstream(path, std::ios::binary) << out->source;
	}
	@autoreleasepool {
		MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
		ApplyOptions(opts, spec);
		NSString* ns = [[NSString alloc] initWithBytes:out->source.data() length:out->source.size() encoding:NSUTF8StringEncoding];
		NSError* err = nil;
		const auto t0 = Clock::now();
		id<MTLLibrary> lib = [dev newLibraryWithSource:ns options:opts error:&err];
		out->lib_ms = MsSince(t0);
		if (!lib) {
			out->error = err ? [[err localizedDescription] UTF8String] : "library failed";
			if (out->error.size() > 400) out->error.resize(400);
			return;
		}
		out->lib_ok = true;
		out->lib = lib;
		id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
		if (!fn && lib.functionNames.count > 0) fn = [lib newFunctionWithName:lib.functionNames.firstObject];
		out->fn = fn;
		if (!make_pso || !fn) return;
		MTLComputePipelineDescriptor* desc = [[MTLComputePipelineDescriptor alloc] init];
		desc.computeFunction = fn;
		desc.label = [NSString stringWithFormat:@"lab %s", spec.name.c_str()];
		err = nil;
		const auto t1 = Clock::now();
		id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:&err];
		out->pso_ms = MsSince(t1);
		if (!pso) {
			out->error = err ? [[err localizedDescription] UTF8String] : "pipeline failed";
			if (out->error.size() > 400) out->error.resize(400);
			return;
		}
		out->pso_ok = true;
		out->pso = pso;
		out->max_threads = pso.maxTotalThreadsPerThreadgroup;
		out->exec_width = pso.threadExecutionWidth;
		out->tg_mem = pso.staticThreadgroupMemoryLength;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Synthetic inputs
// ---------------------------------------------------------------------------------------------------------------------------------

struct Rng {
	uint32_t s = 0x9e3779b9u;
	uint32_t Next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	float F() { return (Next() >> 8) * (1.0f / 16777216.0f); }
};

struct ArgMember {
	std::string type, name;
	int id = 0, count = 1;
};

static std::vector<ArgMember> ParseArgStruct(const std::string& src) {
	std::vector<ArgMember> out;
	const size_t a = src.find("struct KytyArgumentBuffer {");
	if (a == std::string::npos) return out;
	const size_t b = src.find("\n};", a);
	for (const std::string& line : Split(src.substr(a, b - a), '\n')) {
		const size_t idp = line.find("[[id(");
		if (idp == std::string::npos) continue;
		ArgMember m;
		m.id = std::atoi(line.c_str() + idp + 5);
		std::string before = line.substr(0, idp);
		while (!before.empty() && (before.back() == ' ' || before.back() == '\t')) before.pop_back();
		size_t n = before.size();
		while (n > 0 && IsIdent(before[n - 1])) --n;
		m.name = before.substr(n);
		m.type = before.substr(0, n);
		const size_t close = line.find(")]]", idp);
		if (close != std::string::npos && close + 3 < line.size() && line[close + 3] == '[') m.count = std::atoi(line.c_str() + close + 4);
		size_t f = m.type.find_first_not_of(" \t");
		m.type = f == std::string::npos ? "" : m.type.substr(f);
		out.push_back(m);
	}
	return out;
}

struct BenchConfig {
	std::vector<uint32_t> groups = {1024, 1, 1};
	std::vector<uint32_t> tg = {32, 1, 1};
	int reps = 30, warmup = 3, per_cb = 1;
	std::string fill = "small";
	std::map<int, std::string> fillbuf;
	std::vector<std::pair<std::pair<int, int>, std::string>> sets;           // (buffer, dword) = value
	std::vector<std::pair<std::pair<int, std::pair<int, int>>, std::string>> setranges; // (buffer, (d0,d1)) = value
	std::vector<std::pair<int, std::string>> pcs;
	std::map<int, uint32_t> tilepat; // buffer -> width W: dword i = (i % W) | ((i / W) << 16), a distinct 16:16 coordinate per entry
	size_t bufbytes = 8u << 20;
	uint32_t tex_w = 1024, tex_h = 1024;
	std::string texfill = "float";
	double max_ms = 400.0;
	bool compare = false;
	std::string bufset; // --bufset PATH/PREFIX: replay real buffer dumps
	size_t slack = 4u << 20;
};

static uint32_t ParseValue(const std::string& v) {
	if (v.compare(0, 2, "f:") == 0) {
		const float f = std::strtof(v.c_str() + 2, nullptr);
		uint32_t u;
		std::memcpy(&u, &f, 4);
		return u;
	}
	return (uint32_t)std::strtoull(v.c_str(), nullptr, 0);
}

static void FillDwords(uint32_t* p, size_t n, const std::string& mode, Rng& rng) {
	for (size_t i = 0; i < n; ++i) {
		if (mode == "zero") p[i] = 0;
		else if (mode == "one") p[i] = 1;
		else if (mode == "rand") p[i] = rng.Next();
		else if (mode == "float") {
			const float f = rng.F();
			std::memcpy(&p[i], &f, 4);
		} else p[i] = rng.Next() & 0xffu; // small
	}
}

static id<MTLTexture> MakeTexture(id<MTLDevice> dev, const std::string& type, const BenchConfig& cfg, Rng& rng, std::string* err) {
	std::string base = type.substr(0, type.find('<'));
	std::string args = type.find('<') == std::string::npos ? "" : type.substr(type.find('<') + 1);
	std::string comp = args.substr(0, args.find_first_of(",>"));
	const bool writable = args.find("write") != std::string::npos;
	MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
	NSUInteger bpp = 8;
	bool is_depth = base.compare(0, 5, "depth") == 0;
	if (is_depth) {
		d.pixelFormat = MTLPixelFormatDepth32Float;
		bpp = 4;
	} else if (comp == "uint") {
		d.pixelFormat = MTLPixelFormatRGBA32Uint;
		bpp = 16;
	} else if (comp == "int") {
		d.pixelFormat = MTLPixelFormatRGBA32Sint;
		bpp = 16;
	} else {
		d.pixelFormat = MTLPixelFormatRGBA16Float;
		bpp = 8;
	}
	NSUInteger slices = 1;
	if (base == "texture2d" || base == "depth2d") d.textureType = MTLTextureType2D;
	else if (base == "texture2d_array" || base == "depth2d_array") {
		d.textureType = MTLTextureType2DArray;
		slices = 2;
		d.arrayLength = slices;
	} else if (base == "texture3d") {
		d.textureType = MTLTextureType3D;
		d.depth = 16;
	} else if (base == "texturecube" || base == "depthcube") {
		d.textureType = MTLTextureTypeCube;
		slices = 6;
	} else if (base == "texture1d") d.textureType = MTLTextureType1D;
	else if (base == "texture1d_array") {
		d.textureType = MTLTextureType1DArray;
		slices = 2;
		d.arrayLength = slices;
	} else {
		*err = "unsupported texture type " + type;
		return nil;
	}
	const bool one_d = d.textureType == MTLTextureType1D || d.textureType == MTLTextureType1DArray;
	d.width = one_d ? cfg.tex_w : cfg.tex_w;
	d.height = one_d ? 1 : cfg.tex_h;
	if (d.textureType == MTLTextureTypeCube) d.height = d.width = std::min<NSUInteger>(d.width, 512);
	d.mipmapLevelCount = 1;
	d.storageMode = MTLStorageModeShared;
	d.usage = MTLTextureUsageShaderRead | (writable ? MTLTextureUsageShaderWrite : 0);
	id<MTLTexture> t = [dev newTextureWithDescriptor:d];
	if (!t) {
		*err = "newTexture failed for " + type;
		return nil;
	}
	if (!is_depth && cfg.texfill != "zero") {
		const NSUInteger row = d.width * bpp;
		std::vector<uint8_t> rowdata(row);
		const NSUInteger depth = d.textureType == MTLTextureType3D ? d.depth : 1;
		for (NSUInteger s = 0; s < slices; ++s) {
			for (NSUInteger z = 0; z < depth; ++z) {
				for (NSUInteger y = 0; y < d.height; ++y) {
					if (comp == "uint" || comp == "int") {
						uint32_t* p = (uint32_t*)rowdata.data();
						for (NSUInteger i = 0; i < row / 4; ++i) p[i] = rng.Next() & 0xffu;
					} else {
						_Float16* p = (_Float16*)rowdata.data();
						for (NSUInteger i = 0; i < row / 2; ++i) p[i] = (_Float16)rng.F();
					}
					MTLRegion r = d.textureType == MTLTextureType3D ? MTLRegionMake3D(0, y, z, d.width, 1, 1) : MTLRegionMake2D(0, y, d.width, 1);
					if (one_d) r = MTLRegionMake1D(0, d.width);
					if (d.textureType == MTLTextureType3D) [t replaceRegion:r mipmapLevel:0 slice:0 withBytes:rowdata.data() bytesPerRow:row bytesPerImage:row * d.height];
					else [t replaceRegion:r mipmapLevel:0 slice:s withBytes:rowdata.data() bytesPerRow:row bytesPerImage:0];
					if (one_d) break;
				}
			}
		}
	}
	return t;
}

struct Inputs {
	std::vector<ArgMember> members;
	std::vector<id<MTLBuffer>> buffers; // slot -> buffer (nil when unused)
	std::vector<id<MTLTexture>> textures;
	std::vector<std::pair<int, id<MTLTexture>>> tex_by_id;
	std::vector<bool> tex_writable; // parallel to tex_by_id: declared read_write / write
	id<MTLBuffer> srt = nil;
	id<MTLSamplerState> sampler = nil;
	uint32_t sizes[64] = {};
	size_t buf_len[64] = {}; // allocated bytes of each buffer
	uint32_t pc[32] = {};
	std::vector<id<MTLResource>> resident;
};

static bool BuildInputs(id<MTLDevice> dev, const std::string& src, const BenchConfig& cfg, Inputs* in, std::string* err) {
	in->members = ParseArgStruct(src);
	if (in->members.empty()) {
		*err = "no KytyArgumentBuffer struct";
		return false;
	}
	Rng rng;
	std::set<int> used;
	for (size_t p = 0; (p = src.find("ab.buffers[", p)) != std::string::npos; p += 11) used.insert(std::atoi(src.c_str() + p + 11));
	in->buffers.assign(64, nil);
	for (int b : used) {
		if (b < 0 || b >= 64) continue;
		id<MTLBuffer> buf = [dev newBufferWithLength:cfg.bufbytes options:MTLResourceStorageModeShared];
		if (!buf) {
			*err = "buffer alloc failed";
			return false;
		}
		const auto it = cfg.fillbuf.find(b);
		FillDwords((uint32_t*)buf.contents, cfg.bufbytes / 4, it != cfg.fillbuf.end() ? it->second : cfg.fill, rng);
		in->buffers[b] = buf;
		in->sizes[b] = (uint32_t)cfg.bufbytes;
		in->buf_len[b] = cfg.bufbytes;
		in->resident.push_back(buf);
	}
	if (!cfg.bufset.empty()) {
		// Replay of real buffer contents dumped by the emulator: <dir>/<hash>-<pid>-<n>-b<slot>-<addr>.bin. sizes[slot] is the real size; the allocation
		// carries --slack zero bytes behind it so an unchecked (nobounds) read just past the end sees zeros instead of faulting.
		const size_t slash = cfg.bufset.find_last_of('/');
		NSString* dir = [NSString stringWithUTF8String:cfg.bufset.substr(0, slash).c_str()];
		const std::string stem = cfg.bufset.substr(slash + 1) + "-b";
		for (NSString* name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
			const std::string n = [name UTF8String];
			if (n.compare(0, stem.size(), stem) != 0) continue;
			const int slot = std::atoi(n.c_str() + stem.size());
			if (slot < 0 || slot >= 64 || used.count(slot) == 0) continue;
			const std::string data = ReadFile(std::string([dir UTF8String]) + "/" + n);
			const size_t len = ((data.size() + 4095) & ~size_t(4095)) + cfg.slack;
			id<MTLBuffer> buf = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
			if (!buf) {
				*err = "replay buffer alloc failed";
				return false;
			}
			std::memset(buf.contents, 0, len);
			std::memcpy(buf.contents, data.data(), data.size());
			if (in->buffers[slot] != nil) in->resident.erase(std::remove(in->resident.begin(), in->resident.end(), (id<MTLResource>)in->buffers[slot]), in->resident.end());
			in->buffers[slot] = buf;
			in->sizes[slot] = (uint32_t)data.size();
			in->buf_len[slot] = len;
			in->resident.push_back(buf);
			std::printf("replay: slot %d <- %s (%zu bytes)\n", slot, n.c_str(), data.size());
		}
	}
	for (const auto& tp : cfg.tilepat) {
		const int b = tp.first;
		if (b < 0 || b >= 64 || in->buffers[b] == nil || tp.second == 0) continue;
		uint32_t* w = (uint32_t*)in->buffers[b].contents;
		for (size_t i = 0; i < cfg.bufbytes / 4; ++i) w[i] = (uint32_t)(i % tp.second) | (uint32_t)((i / tp.second) << 16);
	}
	for (const auto& s : cfg.sets) {
		const int b = s.first.first, d = s.first.second;
		if (b < 0 || b >= 64 || in->buffers[b] == nil || (size_t)d * 4 + 4 > cfg.bufbytes) {
			std::fprintf(stderr, "warning: --set %d:%d ignored (buffer not used by the kernel)\n", b, d);
			continue;
		}
		((uint32_t*)in->buffers[b].contents)[d] = ParseValue(s.second);
	}
	for (const auto& s : cfg.setranges) {
		const int b = s.first.first;
		if (b < 0 || b >= 64 || in->buffers[b] == nil) {
			std::fprintf(stderr, "warning: --setrange %d ignored (buffer not used by the kernel)\n", b);
			continue;
		}
		for (int d = s.first.second.first; d <= s.first.second.second && (size_t)d * 4 + 4 <= cfg.bufbytes; ++d)
			((uint32_t*)in->buffers[b].contents)[d] = ParseValue(s.second);
	}
	for (const auto& p : cfg.pcs) in->pc[p.first & 31] = ParseValue(p.second);
	in->srt = [dev newBufferWithLength:(1u << 16) options:MTLResourceStorageModeShared];
	FillDwords((uint32_t*)in->srt.contents, (1u << 16) / 4, "small", rng);
	in->resident.push_back(in->srt);
	MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.supportArgumentBuffers = YES;
	in->sampler = [dev newSamplerStateWithDescriptor:sd];
	for (const ArgMember& m : in->members) {
		if (m.type.compare(0, 7, "texture") == 0 || m.type.compare(0, 5, "depth") == 0) {
			id<MTLTexture> t = MakeTexture(dev, m.type, cfg, rng, err);
			if (!t) return false;
			in->textures.push_back(t);
			in->tex_by_id.push_back({m.id, t});
			in->tex_writable.push_back(m.type.find("write") != std::string::npos);
			in->resident.push_back(t);
		}
	}
	return true;
}

static void EncodeArgs(id<MTLArgumentEncoder> enc, const Inputs& in) {
	for (const ArgMember& m : in.members) {
		if (m.name == "buffers") {
			for (int i = 0; i < m.count && i < 64; ++i)
				if (in.buffers[i] != nil) [enc setBuffer:in.buffers[i] offset:0 atIndex:m.id + i];
		} else if (m.name == "flattened_srt") {
			[enc setBuffer:in.srt offset:0 atIndex:m.id];
		} else if (m.name == "samplers") {
			for (int i = 0; i < m.count; ++i) [enc setSamplerState:in.sampler atIndex:m.id + i];
		}
	}
	for (const auto& t : in.tex_by_id) [enc setTexture:t.second atIndex:t.first];
}

struct VariantRun {
	Compiled* c = nullptr;
	id<MTLBuffer> argbuf = nil;
	std::vector<double> ms;
	bool dead = false;
	std::string why;
};

static double Median(std::vector<double> v) {
	if (v.empty()) return 0;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

static int RunBench(id<MTLDevice> dev, const std::string& tag, const std::string& base_src, std::vector<Compiled>& cs, const BenchConfig& cfg) {
	Inputs in;
	std::string err;
	if (!BuildInputs(dev, base_src, cfg, &in, &err)) {
		std::printf("BENCH %s inputs failed: %s\n", tag.c_str(), err.c_str());
		return 1;
	}
	id<MTLCommandQueue> q = [dev newCommandQueue];
	std::vector<VariantRun> runs;
	for (Compiled& c : cs) {
		VariantRun r;
		r.c = &c;
		if (!c.pso_ok) {
			r.dead = true;
			r.why = "no pipeline";
		} else if (cfg.tg[0] * c.tg_mult * cfg.tg[1] * cfg.tg[2] > c.max_threads) {
			r.dead = true;
			r.why = "threadgroup larger than maxTotalThreadsPerThreadgroup";
		} else {
			id<MTLArgumentEncoder> enc = [c.fn newArgumentEncoderWithBufferIndex:0];
			r.argbuf = [dev newBufferWithLength:enc.encodedLength options:MTLResourceStorageModeShared];
			[enc setArgumentBuffer:r.argbuf offset:0];
			EncodeArgs(enc, in);
		}
		runs.push_back(std::move(r));
	}
	auto dispatch = [&](VariantRun& r, int n_dispatch, double* ms) -> bool {
		id<MTLCommandBuffer> cb = [q commandBuffer];
		id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
		[e setComputePipelineState:r.c->pso];
		[e setBuffer:r.argbuf offset:0 atIndex:0];
		[e setBytes:in.pc length:sizeof(in.pc) atIndex:1];
		[e setBytes:in.sizes length:sizeof(in.sizes) atIndex:25];
		for (const auto& res : in.resident) [e useResource:res usage:MTLResourceUsageRead | MTLResourceUsageWrite];
		for (int k = 0; k < n_dispatch; ++k) {
			[e dispatchThreadgroups:MTLSizeMake(cfg.groups[0] * r.c->group_mult, cfg.groups[1], cfg.groups[2])
			    threadsPerThreadgroup:MTLSizeMake(cfg.tg[0] * r.c->tg_mult, cfg.tg[1], cfg.tg[2])];
		}
		[e endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		if (cb.status != MTLCommandBufferStatusCompleted) {
			r.why = cb.error ? [[cb.error localizedDescription] UTF8String] : "command buffer failed";
			return false;
		}
		*ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0 / n_dispatch;
		return true;
	};
	// probe: one dispatch each, so a slow or faulting variant is found before the repetitions
	for (VariantRun& r : runs) {
		if (r.dead) continue;
		double ms = 0;
		if (!dispatch(r, 1, &ms)) {
			r.dead = true;
			continue;
		}
		if (ms > cfg.max_ms) {
			r.dead = true;
			r.why = "one dispatch took " + std::to_string(ms) + " ms (> --max-ms)";
		}
	}
	for (int i = 0; i < cfg.warmup + cfg.reps; ++i) {
		for (VariantRun& r : runs) {
			if (r.dead) continue;
			double ms = 0;
			if (!dispatch(r, cfg.per_cb, &ms)) {
				r.dead = true;
				continue;
			}
			if (i >= cfg.warmup) r.ms.push_back(ms);
		}
	}
	if (cfg.compare) {
		// Output check: every variant runs once on cleared output textures; the written texels are compared with base. Equal texels mean the
		// variant computed the same thing for those lanes, so its timing is a timing of the same work (data-dependent paths!).
		std::vector<std::vector<std::vector<uint8_t>>> snaps(runs.size());
		std::vector<std::vector<std::vector<uint32_t>>> bsnaps(runs.size()); // per variant, per buffer slot
		std::vector<std::vector<uint32_t>> pristine(64);
		for (int b = 0; b < 64; ++b)
			if (in.buffers[b] != nil) pristine[b].assign((uint32_t*)in.buffers[b].contents, (uint32_t*)in.buffers[b].contents + in.buf_len[b] / 4);
		std::vector<NSUInteger> bpps(in.tex_by_id.size(), 0);
		for (size_t vi = 0; vi < runs.size(); ++vi) {
			VariantRun& r = runs[vi];
			if (r.dead) continue;
			for (size_t ti = 0; ti < in.tex_by_id.size(); ++ti) {
				if (!in.tex_writable[ti]) continue;
				id<MTLTexture> t = in.tex_by_id[ti].second;
				const NSUInteger bpp = t.pixelFormat == MTLPixelFormatRGBA32Uint || t.pixelFormat == MTLPixelFormatRGBA32Sint ? 16 : 8;
				bpps[ti] = bpp;
				const NSUInteger w = std::min<NSUInteger>(t.width, 1024), h = std::min<NSUInteger>(t.height, 1024);
				std::vector<uint8_t> zero(t.width * bpp, 0);
				for (NSUInteger y = 0; y < t.height; ++y) [t replaceRegion:MTLRegionMake2D(0, y, t.width, 1) mipmapLevel:0 slice:0 withBytes:zero.data() bytesPerRow:t.width * bpp bytesPerImage:0];
				(void)w;
				(void)h;
			}
			for (int b = 0; b < 64; ++b)
				if (in.buffers[b] != nil) std::memcpy(in.buffers[b].contents, pristine[b].data(), in.buf_len[b]);
			double ms = 0;
			if (!dispatch(r, 1, &ms)) {
				r.dead = true;
				continue;
			}
			bsnaps[vi].resize(64);
			for (int b = 0; b < 64; ++b)
				if (in.buffers[b] != nil) bsnaps[vi][b].assign((uint32_t*)in.buffers[b].contents, (uint32_t*)in.buffers[b].contents + in.buf_len[b] / 4);
			snaps[vi].resize(in.tex_by_id.size());
			for (size_t ti = 0; ti < in.tex_by_id.size(); ++ti) {
				if (!in.tex_writable[ti]) continue;
				id<MTLTexture> t = in.tex_by_id[ti].second;
				const NSUInteger w = std::min<NSUInteger>(t.width, 1024), h = std::min<NSUInteger>(t.height, 1024);
				snaps[vi][ti].resize(w * h * bpps[ti]);
				[t getBytes:snaps[vi][ti].data() bytesPerRow:w * bpps[ti] bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 slice:0];
			}
		}
		size_t bi = 0;
		for (size_t vi = 0; vi < runs.size(); ++vi)
			if (runs[vi].c->spec.name == "base") bi = vi;
		std::printf("COMPARE vs base (first 1024x1024 texels of every written texture; every buffer dword changed by the dispatch)\n");
		for (size_t vi = 0; vi < runs.size(); ++vi) {
			if (runs[vi].dead || bsnaps[vi].empty() || bsnaps[bi].empty()) continue;
			for (int b = 0; b < 64; ++b) {
				if (in.buffers[b] == nil) continue;
				size_t ch_v = 0, ch_b = 0, same = 0;
				for (size_t i = 0; i < pristine[b].size(); ++i) {
					const bool cv = bsnaps[vi][b][i] != pristine[b][i], cb = bsnaps[bi][b][i] != pristine[b][i];
					ch_v += cv;
					ch_b += cb;
					same += cv && cb && bsnaps[vi][b][i] == bsnaps[bi][b][i];
				}
				if (ch_v || ch_b)
					std::printf("  %-26s buffer %d: dwords changed base=%zu variant=%zu, changed in both and identical=%zu\n", runs[vi].c->spec.name.c_str(), b, ch_b, ch_v, same);
			}
		}
		for (size_t vi = 0; vi < runs.size(); ++vi) {
			if (runs[vi].dead || snaps[vi].empty() || snaps[bi].empty()) continue;
			for (size_t ti = 0; ti < in.tex_by_id.size(); ++ti) {
				if (!in.tex_writable[ti] || snaps[vi][ti].empty() || snaps[bi][ti].empty()) continue;
				const NSUInteger bpp = bpps[ti];
				size_t nz_b = 0, nz_v = 0, same = 0, differ = 0, total = snaps[vi][ti].size() / bpp;
				for (size_t i = 0; i < total; ++i) {
					const uint8_t* a = &snaps[bi][ti][i * bpp];
					const uint8_t* b = &snaps[vi][ti][i * bpp];
					bool za = true, zb = true;
					for (NSUInteger k = 0; k < bpp; ++k) {
						za &= a[k] == 0;
						zb &= b[k] == 0;
					}
					nz_b += !za;
					nz_v += !zb;
					if (!za && !zb) {
						if (std::memcmp(a, b, bpp) == 0) ++same;
						else ++differ;
					}
				}
				std::printf("  %-26s image id %d: nonzero texels base=%zu variant=%zu, both nonzero: identical=%zu different=%zu\n", runs[vi].c->spec.name.c_str(),
				            in.tex_by_id[ti].first, nz_b, nz_v, same, differ);
			}
		}
	}
	double base_med = 0, base_min = 0;
	for (VariantRun& r : runs) {
		if (r.dead || r.ms.empty()) continue;
		if (r.c->spec.name == "base") {
			base_med = Median(r.ms);
			base_min = *std::min_element(r.ms.begin(), r.ms.end());
		}
	}
	std::printf("BENCH %s groups=%ux%ux%u tg=%ux%ux%u reps=%d per_cb=%d fill=%s bufbytes=%zu\n", tag.c_str(), cfg.groups[0], cfg.groups[1], cfg.groups[2],
	            cfg.tg[0], cfg.tg[1], cfg.tg[2], cfg.reps, cfg.per_cb, cfg.fill.c_str(), cfg.bufbytes);
	std::printf("%-26s %10s %10s %8s %8s %8s  %s\n", "variant", "median_ms", "min_ms", "med/base", "min/base", "spread%", "note");
	for (VariantRun& r : runs) {
		if (r.dead || r.ms.empty()) {
			std::printf("%-26s %10s %10s %8s %8s %8s  %s\n", r.c->spec.name.c_str(), "-", "-", "-", "-", "-", r.why.c_str());
			continue;
		}
		const double med = Median(r.ms);
		const double mn = *std::min_element(r.ms.begin(), r.ms.end());
		std::vector<double> s = r.ms;
		std::sort(s.begin(), s.end());
		const double spread = 100.0 * (s[s.size() * 3 / 4] - s[s.size() / 4]) / med;
		std::printf("%-26s %10.3f %10.3f %8.3f %8.3f %8.1f  groups x%u, threads/group x%u\n", r.c->spec.name.c_str(), med, mn,
		            base_med > 0 ? med / base_med : 0.0, base_min > 0 ? mn / base_min : 0.0, spread, r.c->group_mult, r.c->tg_mult);
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static std::vector<uint32_t> ParseDims(const std::string& s) {
	std::vector<uint32_t> v;
	for (const std::string& p : Split(s, ',')) v.push_back((uint32_t)std::strtoul(p.c_str(), nullptr, 0));
	while (v.size() < 3) v.push_back(1);
	return v;
}

int main(int argc, char** argv) {
	std::string file, variants = "base,nobounds,fast,noftz,nobounds+noftz+fast", dump_dir;
	bool make_pso = true, bench = false;
	BenchConfig cfg;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "missing value for %s\n", a.c_str());
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "--variants") variants = next();
		else if (a == "--no-pso") make_pso = false;
		else if (a == "--cold") g_cold = true;
		else if (a == "--compare") cfg.compare = true;
		else if (a == "--bufset") cfg.bufset = next();
		else if (a == "--slack") cfg.slack = (size_t)std::strtoull(next().c_str(), nullptr, 0);
		else if (a == "--dump-dir") dump_dir = next();
		else if (a == "--bench") bench = true;
		else if (a == "--groups") cfg.groups = ParseDims(next());
		else if (a == "--tg") cfg.tg = ParseDims(next());
		else if (a == "--reps") cfg.reps = std::atoi(next().c_str());
		else if (a == "--warmup") cfg.warmup = std::atoi(next().c_str());
		else if (a == "--per-cb") cfg.per_cb = std::atoi(next().c_str());
		else if (a == "--fill") cfg.fill = next();
		else if (a == "--bufbytes") cfg.bufbytes = (size_t)std::strtoull(next().c_str(), nullptr, 0);
		else if (a == "--max-ms") cfg.max_ms = std::atof(next().c_str());
		else if (a == "--texfill") cfg.texfill = next();
		else if (a == "--tex") {
			const std::string t = next();
			cfg.tex_w = (uint32_t)std::atoi(t.c_str());
			const size_t x = t.find('x');
			cfg.tex_h = x == std::string::npos ? cfg.tex_w : (uint32_t)std::atoi(t.c_str() + x + 1);
		} else if (a == "--set") {
			const std::string t = next(); // B:D=V
			const size_t c = t.find(':'), e = t.find('=');
			cfg.sets.push_back({{std::atoi(t.c_str()), std::atoi(t.c_str() + c + 1)}, t.substr(e + 1)});
		} else if (a == "--setrange") {
			const std::string t = next(); // B:D0-D1=V
			const size_t c = t.find(':'), d = t.find('-', c), e = t.find('=');
			cfg.setranges.push_back({{std::atoi(t.c_str()), {std::atoi(t.c_str() + c + 1), std::atoi(t.c_str() + d + 1)}}, t.substr(e + 1)});
		} else if (a == "--fillbuf") {
			const std::string t = next(); // B=mode
			const size_t e = t.find('=');
			cfg.fillbuf[std::atoi(t.c_str())] = t.substr(e + 1);
		} else if (a == "--tilepat") {
			const std::string t = next(); // B=W
			const size_t e = t.find('=');
			cfg.tilepat[std::atoi(t.c_str())] = (uint32_t)std::atoi(t.c_str() + e + 1);
		} else if (a == "--pc") {
			const std::string t = next();
			const size_t e = t.find('=');
			cfg.pcs.push_back({std::atoi(t.c_str()), t.substr(e + 1)});
		} else if (a[0] == '-') {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 2;
		} else file = a;
	}
	if (file.empty()) {
		std::fprintf(stderr, "usage: msl_kernel_lab [options] file.metal  (see the header of msl_kernel_lab.mm)\n");
		return 2;
	}
	const std::string src = ReadFile(file);
	if (src.empty()) {
		std::fprintf(stderr, "cannot read %s\n", file.c_str());
		return 2;
	}
	std::string tag = file.substr(file.find_last_of('/') + 1);
	if (tag.size() > 6 && tag.compare(tag.size() - 6, 6, ".metal") == 0) tag.resize(tag.size() - 6);

	@autoreleasepool {
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		std::printf("device: %s  | file: %s (%zu KB, %zu lines)\n", [dev.name UTF8String], tag.c_str(), src.size() / 1024,
		            (size_t)std::count(src.begin(), src.end(), '\n'));
		std::printf("%-26s %-6s %9s %9s %8s %6s %8s  %s\n", "variant", "status", "lib_ms", "pso_ms", "maxThr", "exec", "tgmem", "note");
		std::vector<Compiled> compiled(Split(variants, ',').size());
		size_t idx = 0;
		for (const std::string& vname : Split(variants, ',')) {
			Compiled& c = compiled[idx++];
			CompileVariant(dev, src, ParseVariant(vname), make_pso, dump_dir, tag, &c);
			const char* st = !c.lib_ok ? "LIBERR" : (make_pso ? (c.pso_ok ? "ok" : "PSOERR") : "lib-ok");
			std::printf("%-26s %-6s %9.0f %9.0f %8lu %6lu %8lu  %s%s\n", c.spec.name.c_str(), st, c.lib_ms, c.pso_ms, (unsigned long)c.max_threads,
			            (unsigned long)c.exec_width, (unsigned long)c.tg_mem, c.note.c_str(), c.error.empty() ? "" : ("ERROR: " + c.error).c_str());
			std::fflush(stdout);
		}
		if (bench) {
			if (src.find("kernel void") == std::string::npos) {
				std::printf("BENCH skipped: not a compute kernel\n");
			} else {
				RunBench(dev, tag, src, compiled, cfg);
			}
		}
	}
	return 0;
}
