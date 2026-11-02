#pragma once

#include "graphics/host_gpu/renderer/pipeline/flatSrtControl.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Libs::Graphics {

// Keep the evaluator and its tests independent of the Vulkan profiling headers.
uint64_t ProfileClockNs() noexcept;
uint64_t ProfileThreadId() noexcept;

// UFC5 resource materialization, serialized by the existing resource-plan owner.
// On by default (EXP-0011: +41.6% present rate A-B-A, 27919 shadow checks, zero mismatches,
// candidate 75.3% cheaper than the interpreter on compared calls). KYTY_COMPILED_SRT=0 restores
// the interpreter. A launch control file still drives the mode so the staged off/shadow/on
// experiment keeps working, and a mismatch still disables the candidate for the process.
inline bool CompiledSrtDefaultEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_COMPILED_SRT");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}
inline bool MaterializeCompiledSrtExperiment(
    const ShaderRecompiler::IR::ResourcePlan& plan,
    const ShaderRecompiler::IR::SrtRuntime& runtime,
    ShaderRecompiler::IR::ResourceSnapshot& resources,
    ShaderRecompiler::IR::ResourceSpecialization& specialization) {

	using namespace ShaderRecompiler::IR;
	static const char* path = std::getenv("KYTY_COMPILED_SRT_CONTROL_FILE");
	if (!path && !CompiledSrtDefaultEnabled()) {
		return MaterializeResources(plan, runtime, resources, specialization);
	}
	static std::atomic<bool> rejected {false};
	struct Aggregate {
		CompiledSrtEvaluationStats stats;
		uint64_t calls = 0, candidate = 0, reference = 0, compared = 0, unsupported = 0;
		uint64_t cpu_ns = 0, candidate_ns = 0, reference_ns = 0, comparison_ns = 0, compile_ns = 0;
	};
	struct Control {
		std::unordered_map<uint64_t, Aggregate> shaders;
		uint64_t next_poll = 0, next_report = 0, window_start = ProfileClockNs();
		DescriptorEvaluationMode mode = DescriptorEvaluationMode::Off;
		FILE* csv = nullptr;
		Control() {
			if (const char* output = std::getenv("KYTY_COMPILED_SRT_CSV")) csv = std::fopen(output, "a");
			if (csv && std::ftell(csv) == 0) {
				std::fprintf(csv, "host_ns,thread,shader,mode,record_kind,window_ms,calls,candidate_calls,reference_calls,comparison_calls,unsupported_calls,cpu_ms,candidate_cpu_ms,reference_cpu_ms,comparison_cpu_ms,compile_ms,full_checks,full_mismatches,shadow_reference_ms,shadow_candidate_ms,rejected\n");
				std::fflush(csv);
			}
		}
		const char* Name() const {
			return mode == DescriptorEvaluationMode::Off ? "off" : mode == DescriptorEvaluationMode::Shadow ? "shadow" : "on";
		}
		void Row(uint64_t now, uint64_t hash, const char* kind, const Aggregate& a) {
			if (!csv) return;
			std::fprintf(csv, "%llu,%llu,0x%016llx,%s,%s,%.3f,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%.6f,%.6f,%u\n",
			    static_cast<unsigned long long>(now), static_cast<unsigned long long>(ProfileThreadId()),
			    static_cast<unsigned long long>(hash), Name(), kind, (now - window_start) / 1e6,
			    static_cast<unsigned long long>(a.calls), static_cast<unsigned long long>(a.candidate),
			    static_cast<unsigned long long>(a.reference), static_cast<unsigned long long>(a.compared),
			    static_cast<unsigned long long>(a.unsupported), a.cpu_ns / 1e6, a.candidate_ns / 1e6,
			    a.reference_ns / 1e6, a.comparison_ns / 1e6, a.compile_ns / 1e6,
			    static_cast<unsigned long long>(a.stats.full_checks), static_cast<unsigned long long>(a.stats.full_mismatches),
			    a.stats.reference_ns / 1e6, a.stats.candidate_ns / 1e6, static_cast<unsigned>(rejected.load(std::memory_order_relaxed)));
		}
		void Report(uint64_t now) {
			Aggregate total;
			for (const auto& [hash, a]: shaders) {
				Row(now, hash, "shader", a);
				total.calls += a.calls; total.candidate += a.candidate; total.reference += a.reference;
				total.compared += a.compared; total.unsupported += a.unsupported; total.cpu_ns += a.cpu_ns;
				total.candidate_ns += a.candidate_ns; total.reference_ns += a.reference_ns;
				total.comparison_ns += a.comparison_ns; total.compile_ns += a.compile_ns;
				total.stats.full_checks += a.stats.full_checks; total.stats.full_mismatches += a.stats.full_mismatches;
				total.stats.reference_ns += a.stats.reference_ns; total.stats.candidate_ns += a.stats.candidate_ns;
			}
			if (total.calls) Row(now, 0, "total", total);
			if (csv) std::fflush(csv);
			shaders.clear(); window_start = now; next_report = now + 2'000'000'000ull;
		}
		~Control() { Report(ProfileClockNs()); if (csv) std::fclose(csv); }
	};
	static thread_local Control control;
	static thread_local bool control_defaulted = false;
	if (!control_defaulted) {
		control_defaulted = true;
		// With no control file the feature runs on its default, so start in the fast mode
		// rather than waiting for a poll that will never find a file.
		if (!path) control.mode = DescriptorEvaluationMode::Fast;
	}
	const auto now = ProfileClockNs();
	if (path && now >= control.next_poll) {
		control.next_poll = now + 500'000'000ull;
		if (FILE* file = std::fopen(path, "r")) {
			char text[32] {};
			if (std::fgets(text, sizeof(text), file)) {
				auto requested = DescriptorEvaluationMode::Off;
				if (std::strncmp(text, "shadow", 6) == 0) requested = DescriptorEvaluationMode::Shadow;
				else if (std::strncmp(text, "on", 2) == 0) requested = DescriptorEvaluationMode::Fast;
				if (rejected.load(std::memory_order_relaxed)) requested = DescriptorEvaluationMode::Off;
				if (requested != control.mode) {
					control.Report(now); control.mode = requested;
					std::fprintf(stderr, "[compiled-srt] mode=%s thread=%llu\n", control.Name(), static_cast<unsigned long long>(ProfileThreadId()));
					std::fflush(stderr);
				}
			}
			std::fclose(file);
		}
	}
	if (rejected.load(std::memory_order_relaxed) && control.mode != DescriptorEvaluationMode::Off) {
		control.Report(now); control.mode = DescriptorEvaluationMode::Off;
	}
	if (now >= control.next_report) control.Report(now);
	auto& a = control.shaders[plan.shader_hash]; ++a.calls;
	const auto sequence = control.mode == DescriptorEvaluationMode::Off ? 0 : ++plan.compiled_srt_sequence;
	bool eligible = false;
	if (control.mode != DescriptorEvaluationMode::Off && !runtime.profile && !runtime.compiled_flat_srt) {
		const bool needs_compile = !plan.compiled_srt_built;
		const auto begin = needs_compile ? ProfileClockNs() : 0;
		eligible = CompileSrtPlan(plan) != nullptr;
		if (needs_compile) a.compile_ns += ProfileClockNs() - begin;
	}
	if (control.mode != DescriptorEvaluationMode::Off && !eligible) ++a.unsupported;
	// Reuse the existing reference/compare/candidate policy: first check per plan,
	// then every 64 shadow calls or every 512 enabled calls. Mismatches cannot be reset live.
	const auto action = eligible ? ChooseFlatSrtAction(control.mode, eligible, plan.compiled_srt_verified,
	    plan.compiled_srt_full_checks, sequence,
	    rejected.load(std::memory_order_relaxed) || plan.compiled_srt_rejected) : FlatSrtAction::Reference;
	const auto begin = ProfileClockNs();
	bool result;
	if (action == FlatSrtAction::Compare) {
		++a.compared;
		result = ShadowCompiledSrtMaterializeResources(plan, runtime, resources, specialization, a.stats);
	} else if (action == FlatSrtAction::Candidate) {
		++a.candidate;
		result = MaterializeCompiledSrtResources(plan, runtime, resources, specialization);
	} else {
		++a.reference;
		result = MaterializeResources(plan, runtime, resources, specialization);
	}
	const auto elapsed = ProfileClockNs() - begin;
	a.cpu_ns += elapsed;
	if (action == FlatSrtAction::Compare) a.comparison_ns += elapsed;
	else if (action == FlatSrtAction::Candidate) a.candidate_ns += elapsed;
	else a.reference_ns += elapsed;
	if (plan.compiled_srt_rejected && !rejected.exchange(true, std::memory_order_relaxed)) {
		std::fprintf(stderr, "[compiled-srt] MISMATCH shader=0x%016llx; disabling candidate\n", static_cast<unsigned long long>(plan.shader_hash));
		std::fflush(stderr); control.Report(ProfileClockNs()); control.mode = DescriptorEvaluationMode::Off;
	}
	return result;
}

} // namespace Libs::Graphics
