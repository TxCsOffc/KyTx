#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/flatSrtControl.h"
#include "graphics/host_gpu/renderer/pipeline/compiledSrtExperiment.h"
#include "graphics/host_gpu/renderer/productionProfile.h"
#include "graphics/host_gpu/metal/metalShaderAbi.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#if defined(__APPLE__)
#include "graphics/host_gpu/metal/metalRenderer.h"
#include "graphics/host_gpu/metal/metalShader.h"
#endif

#include <filesystem>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_set>
#include <xxhash.h>
#include <cstdlib>
#include <mutex>

// KYTY_MSL_ONLY coverage: per-shader unsupported log (once per hash) and translated/unsupported counts.
namespace {
std::mutex s_msl_log_mutex;
std::unordered_set<uint64_t> s_msl_logged;
std::atomic<uint32_t> s_msl_translated {0};
std::atomic<uint32_t> s_msl_unsupported {0};

void MslCoverageCount(bool translated, Libs::Graphics::ShaderType stage) {
	(void)stage;
	if (translated) {
		s_msl_translated.fetch_add(1);
	} else {
		s_msl_unsupported.fetch_add(1);
	}
	const uint32_t total = s_msl_translated.load() + s_msl_unsupported.load();
	if (total % 50 == 0) {
		std::fprintf(stderr, "[MSL-coverage] translated=%u unsupported=%u\n", s_msl_translated.load(), s_msl_unsupported.load());
	}
}

void MslLogUnsupported(uint64_t hash, Libs::Graphics::ShaderType stage, const std::vector<std::string>& reasons) {
	std::lock_guard<std::mutex> lock(s_msl_log_mutex);
	if (!s_msl_logged.insert(hash).second) {
		return;
	}
	std::fprintf(stderr, "[MSL-unsupported] hash=0x%016llx stage=%u reasons=%zu\n", (unsigned long long)hash,
	             static_cast<uint32_t>(stage), reasons.size());
	for (size_t i = 0; i < reasons.size() && i < 8; ++i) {
		std::fprintf(stderr, "[MSL-unsupported]   %s\n", reasons[i].c_str());
	}
}
} // namespace


namespace Libs::Graphics {

namespace {

// Bounded UFC5 descriptor experiment. No renderer policy, guest reads or Vulkan
// work is removed; only immutable, pure descriptor words are evaluated directly.
template <bool Flat>
bool MaterializeEvaluationExperiment(const ShaderRecompiler::IR::ResourcePlan& plan,
                                     const ShaderRecompiler::IR::SrtRuntime& runtime,
                                     ShaderRecompiler::IR::ResourceSnapshot& resources,
                                     ShaderRecompiler::IR::ResourceSpecialization& specialization) {
	using namespace ShaderRecompiler::IR;
	static const uint64_t target = [] {
		const auto* hash = std::getenv(Flat ? "KYTY_FLAT_SRT_SHADER" : "KYTY_DESCRIPTOR_GATHER_SHADER");
		return hash ? std::strtoull(hash, nullptr, 16) : 0xd3dcf81c43080fd0ull;
	}();
	static const bool configured = std::getenv(Flat ? "KYTY_FLAT_SRT_CONTROL_FILE" : "KYTY_DESCRIPTOR_GATHER_CONTROL_FILE") != nullptr;
	if (!configured || plan.shader_hash != target) {
		if constexpr (Flat) return MaterializeEvaluationExperiment<false>(plan, runtime, resources, specialization);
		else return MaterializeResources(plan, runtime, resources, specialization);
	}
	struct Control {
		std::string path = std::getenv(Flat ? "KYTY_FLAT_SRT_CONTROL_FILE" : "KYTY_DESCRIPTOR_GATHER_CONTROL_FILE");
		DescriptorEvaluationMode mode = DescriptorEvaluationMode::Off;
		DescriptorEvaluationStats stats;
		FlatSrtEvaluationStats flat_stats;
		uint64_t materializations = 0, fast = 0, blocked = 0, cpu_ns = 0, sequence = 0;
		bool failed = false;
		uint64_t window_start = ProfileClockNs(), next_poll = 0, next_report = 0;
		FILE* csv = [] {
			const auto* path = std::getenv(Flat ? "KYTY_FLAT_SRT_CSV" : "KYTY_DESCRIPTOR_GATHER_CSV");
			FILE* file = path ? std::fopen(path, "a") : nullptr;
			if (file && std::ftell(file) == 0) {
				std::fprintf(file, "host_ns,thread,shader,mode,window_ms,materializations,fast_materializations,blocked_materializations,cpu_ms,descriptor_calls,eligible_calls,fallback_calls,eligible_words,descriptor_checks,descriptor_mismatches,full_checks,full_mismatches,raw_calls,raw_eligible,raw_fallback,recipe_evaluations,user_data_evaluations,memo_hits,reference_shadow_ms,candidate_shadow_ms\n");
				std::fflush(file);
			}
			return file;
		}();
		const char* Name() const {
			return mode == DescriptorEvaluationMode::Off ? "off" :
			       mode == DescriptorEvaluationMode::Shadow ? "shadow" : "on";
		}
		void Report(uint64_t now) {
			if (csv && materializations) {
				std::fprintf(csv, "%llu,%llu,0x%016llx,%s,%.3f,%llu,%llu,%llu,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f\n",
				    static_cast<unsigned long long>(now), static_cast<unsigned long long>(ProfileThreadId()),
				    static_cast<unsigned long long>(target), Name(), (now-window_start)/1e6,
				    static_cast<unsigned long long>(materializations), static_cast<unsigned long long>(fast),
				    static_cast<unsigned long long>(blocked), cpu_ns/1e6,
				    static_cast<unsigned long long>(stats.calls), static_cast<unsigned long long>(stats.eligible),
				    static_cast<unsigned long long>(stats.fallback), static_cast<unsigned long long>(stats.words),
				    static_cast<unsigned long long>(stats.descriptor_checks), static_cast<unsigned long long>(stats.descriptor_mismatches),
				    static_cast<unsigned long long>(Flat ? flat_stats.full_checks : stats.full_checks),
				    static_cast<unsigned long long>(Flat ? flat_stats.full_mismatches : stats.full_mismatches),
				    static_cast<unsigned long long>(flat_stats.raw_calls), static_cast<unsigned long long>(flat_stats.eligible),
				    static_cast<unsigned long long>(flat_stats.fallback), static_cast<unsigned long long>(flat_stats.recipe_evaluations),
				    static_cast<unsigned long long>(flat_stats.user_data_evaluations), static_cast<unsigned long long>(flat_stats.memo_hits),
				    flat_stats.reference_ns / 1e6, flat_stats.candidate_ns / 1e6);
				std::fflush(csv);
			}
			stats = {};
			flat_stats = {};
			materializations = fast = blocked = cpu_ns = 0;
			window_start = now;
			next_report = now + 2'000'000'000ull;
		}
		~Control() { Report(ProfileClockNs()); if (csv) std::fclose(csv); }
	};
	static thread_local Control control;
	const auto now = ProfileClockNs();
	if (now >= control.next_poll) {
		control.next_poll = now + 500'000'000ull;
		if (FILE* file = std::fopen(control.path.c_str(), "r")) {
			char text[32] {};
			if (std::fgets(text, sizeof(text), file)) {
				auto requested = control.mode;
				if (std::strncmp(text, "off", 3) == 0) requested = DescriptorEvaluationMode::Off;
				else if (std::strncmp(text, "shadow", 6) == 0) requested = DescriptorEvaluationMode::Shadow;
				else if (std::strncmp(text, "on", 2) == 0) requested = DescriptorEvaluationMode::Fast;
				if (control.failed) requested = DescriptorEvaluationMode::Off;
				if (requested != control.mode) {
					control.Report(now);
					control.mode = requested;
					std::fprintf(stderr, "[%s] mode=%s shader=0x%016llx thread=%llu\n", Flat ? "flat-srt" : "descriptor-gather", control.Name(),
					    static_cast<unsigned long long>(target), static_cast<unsigned long long>(ProfileThreadId()));
					std::fflush(stderr);
				}
			}
			std::fclose(file);
		}
	}
	if (now >= control.next_report) control.Report(now);
	++control.materializations;
	++control.sequence;
	const auto begin = ProfileClockNs();
	bool result;
	if (control.mode == DescriptorEvaluationMode::Shadow && !control.failed &&
	    ((Flat ? plan.flat_srt_full_checks : plan.descriptor_gather_full_checks) == 0 || (control.sequence & 63u) == 0)) {
		if constexpr (Flat) result = ShadowFlatSrtMaterializeResources(plan, runtime, resources, specialization, control.flat_stats);
		else result = ShadowMaterializeResources(plan, runtime, resources, specialization, control.stats);
	} else {
		auto mode = control.mode;
		if (mode == DescriptorEvaluationMode::Fast && (!(Flat ? plan.flat_srt_verified : plan.descriptor_gather_verified) || control.failed)) {
			mode = DescriptorEvaluationMode::Off;
			++control.blocked;
		}
		if (mode == DescriptorEvaluationMode::Fast) ++control.fast;
		if constexpr (Flat) {
			auto candidate_runtime = runtime;
			candidate_runtime.compiled_flat_srt = mode == DescriptorEvaluationMode::Fast;
			candidate_runtime.flat_srt_stats = &control.flat_stats;
			result = MaterializeResources(plan, candidate_runtime, resources, specialization);
		} else result = MaterializeResources(plan, runtime, resources, specialization, mode, &control.stats);
	}
	control.cpu_ns += ProfileClockNs() - begin;
	if ((Flat ? plan.flat_srt_rejected : plan.descriptor_gather_rejected) && !control.failed) {
		control.failed = true;
		std::fprintf(stderr, "[%s] MISMATCH: experiment rejected; reverting to reference\n", Flat ? "flat-srt" : "descriptor-gather");
		std::fflush(stderr);
		control.Report(ProfileClockNs());
		control.mode = DescriptorEvaluationMode::Off;
	}
	return result;
}

bool MaterializeDescriptorExperiment(const ShaderRecompiler::IR::ResourcePlan& plan,
                                     const ShaderRecompiler::IR::SrtRuntime& runtime,
                                     ShaderRecompiler::IR::ResourceSnapshot& resources,
                                     ShaderRecompiler::IR::ResourceSpecialization& specialization) {
	using namespace ShaderRecompiler::IR;
	// Route to the compiled evaluator whenever it is configured OR running on its default-on
	// setting; otherwise MaterializeCompiledSrtExperiment would never be reached (DEC-0004).
	static const bool compiled_configured =
	    std::getenv("KYTY_COMPILED_SRT_CONTROL_FILE") != nullptr || CompiledSrtDefaultEnabled();
	if (compiled_configured) return MaterializeCompiledSrtExperiment(plan, runtime, resources, specialization);
	static const auto* configured = std::getenv("KYTY_FLAT_SRT_CONTROL_FILE");
	if (!configured) return MaterializeEvaluationExperiment<false>(plan, runtime, resources, specialization);
	static std::atomic<bool> rejected = false;
	struct Control {
		struct Aggregate {
			FlatSrtEvaluationStats stats;
			uint64_t calls=0, fast=0, blocked=0, compared=0, reference=0, no_recipe=0;
			uint64_t cpu=0, fast_cpu=0, reference_cpu=0, shadow_cpu=0, fast_recipes=0;
		};
		std::unordered_map<uint64_t, Aggregate> shaders;
		std::unordered_map<const ResourcePlan*, uint64_t> sequences;
		std::string path = configured;
		DescriptorEvaluationMode mode = DescriptorEvaluationMode::Off;
		bool all = false;
		uint64_t target = 0xd3dcf81c43080fd0ull;
		uint64_t window_start=ProfileClockNs(), next_poll=0, next_report=0;
		FILE* csv=nullptr;
		Control() {
			if (const char* setting=std::getenv("KYTY_FLAT_SRT_SHADER")) {
				all=std::strcmp(setting,"all")==0;
				if (!all) target=std::strtoull(setting,nullptr,16);
			}
			if (const char* output=std::getenv("KYTY_FLAT_SRT_CSV")) csv=std::fopen(output,"a");
			if (csv && std::ftell(csv)==0) {
				std::fprintf(csv,"host_ns,thread,shader,mode,window_ms,materializations,fast_materializations,blocked_materializations,cpu_ms,descriptor_calls,eligible_calls,fallback_calls,eligible_words,descriptor_checks,descriptor_mismatches,full_checks,full_mismatches,raw_calls,raw_eligible,raw_fallback,recipe_evaluations,user_data_evaluations,memo_hits,reference_shadow_ms,candidate_shadow_ms,record_kind,scope,shadow_materializations,reference_materializations,no_recipe_materializations,fast_cpu_ms,reference_cpu_ms,shadow_cpu_ms,fast_recipe_evaluations\n");
				std::fflush(csv);
			}
		}
		const char* Name() const { return mode==DescriptorEvaluationMode::Off ? "off" : mode==DescriptorEvaluationMode::Shadow ? "shadow" : "on"; }
		void Row(uint64_t now, const std::string& shader, const char* kind, const Aggregate& a) {
			if (!csv) return;
			const auto scope=all ? std::string("all") : fmt::format("{:016x}",target);
			std::fprintf(csv,"%llu,%llu,%s,%s,%.3f,%llu,%llu,%llu,%.6f,0,0,0,0,0,0,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%s,%s,%llu,%llu,%llu,%.6f,%.6f,%.6f,%llu\n",
			    static_cast<unsigned long long>(now),static_cast<unsigned long long>(ProfileThreadId()),shader.c_str(),Name(),(now-window_start)/1e6,
			    static_cast<unsigned long long>(a.calls),static_cast<unsigned long long>(a.fast),static_cast<unsigned long long>(a.blocked),a.cpu/1e6,
			    static_cast<unsigned long long>(a.stats.full_checks),static_cast<unsigned long long>(a.stats.full_mismatches),
			    static_cast<unsigned long long>(a.stats.raw_calls),static_cast<unsigned long long>(a.stats.eligible),static_cast<unsigned long long>(a.stats.fallback),
			    static_cast<unsigned long long>(a.stats.recipe_evaluations),static_cast<unsigned long long>(a.stats.user_data_evaluations),static_cast<unsigned long long>(a.stats.memo_hits),
			    a.stats.reference_ns/1e6,a.stats.candidate_ns/1e6,kind,scope.c_str(),
			    static_cast<unsigned long long>(a.compared),static_cast<unsigned long long>(a.reference),static_cast<unsigned long long>(a.no_recipe),
			    a.fast_cpu/1e6,a.reference_cpu/1e6,a.shadow_cpu/1e6,static_cast<unsigned long long>(a.fast_recipes));
		}
		void Report(uint64_t now) {
			Aggregate total;
			for (const auto& [hash,a]:shaders) {
				Row(now,fmt::format("0x{:016x}",hash),"shader",a);
#define SUM_FIELD(field) total.field+=a.field
				SUM_FIELD(calls); SUM_FIELD(fast); SUM_FIELD(blocked); SUM_FIELD(compared); SUM_FIELD(reference); SUM_FIELD(no_recipe);
				SUM_FIELD(cpu); SUM_FIELD(fast_cpu); SUM_FIELD(reference_cpu); SUM_FIELD(shadow_cpu); SUM_FIELD(fast_recipes);
				SUM_FIELD(stats.raw_calls); SUM_FIELD(stats.eligible); SUM_FIELD(stats.fallback); SUM_FIELD(stats.recipe_evaluations);
				SUM_FIELD(stats.user_data_evaluations); SUM_FIELD(stats.memo_hits); SUM_FIELD(stats.full_checks); SUM_FIELD(stats.full_mismatches);
				SUM_FIELD(stats.reference_ns); SUM_FIELD(stats.candidate_ns);
#undef SUM_FIELD
			}
			if (total.calls) Row(now,"all","summary",total);
			if (csv) std::fflush(csv);
			shaders.clear(); window_start=now; next_report=now+2'000'000'000ull;
		}
		~Control() { Report(ProfileClockNs()); if (csv) std::fclose(csv); }
	};
	static thread_local Control control;
	const auto now=ProfileClockNs();
	if (now>=control.next_poll) {
		control.next_poll=now+500'000'000ull;
		if (FILE* file=std::fopen(control.path.c_str(),"r")) {
			char text[96]{}, mode_text[16]{}, scope_text[32]{};
			if (std::fgets(text,sizeof(text),file)) {
				const int count=std::sscanf(text,"%15s %31s",mode_text,scope_text);
				auto requested=control.mode; bool valid=count>=1;
				if (std::strcmp(mode_text,"off")==0) requested=DescriptorEvaluationMode::Off;
				else if (std::strcmp(mode_text,"shadow")==0) requested=DescriptorEvaluationMode::Shadow;
				else if (std::strcmp(mode_text,"on")==0) requested=DescriptorEvaluationMode::Fast;
				else valid=false;
				bool all=control.all; auto target=control.target;
				if (count==2) {
					all=std::strcmp(scope_text,"all")==0;
					if (!all) {
						valid &= std::strlen(scope_text)==16 && std::all_of(scope_text,scope_text+std::strlen(scope_text),[](unsigned char c){return std::isxdigit(c)!=0;});
						if (valid) target=std::strtoull(scope_text,nullptr,16);
					}
				}
				if (rejected.load(std::memory_order_relaxed)) requested=DescriptorEvaluationMode::Off;
				if (valid && (requested!=control.mode || all!=control.all || (!all && target!=control.target))) {
					control.Report(now); control.mode=requested; control.all=all; control.target=target;
					std::fprintf(stderr,"[flat-srt] mode=%s scope=%s thread=%llu\n",control.Name(),all ? "all" : fmt::format("{:016x}",target).c_str(),static_cast<unsigned long long>(ProfileThreadId()));
					std::fflush(stderr);
				}
			}
			std::fclose(file);
		}
	}
	if (now>=control.next_report) control.Report(now);
	if (!control.all && plan.shader_hash!=control.target)
		return MaterializeEvaluationExperiment<false>(plan,runtime,resources,specialization);
	auto& a=control.shaders[plan.shader_hash]; ++a.calls;
	const bool has_recipes=!plan.flat_srt_recipes.empty();
	if (!has_recipes) ++a.no_recipe;
	uint64_t sequence=0;
	if (control.mode!=DescriptorEvaluationMode::Off) sequence=++control.sequences[&plan];
	const auto action=ChooseFlatSrtAction(control.mode,has_recipes,plan.flat_srt_verified,plan.flat_srt_full_checks,sequence,
	    rejected.load(std::memory_order_relaxed) || plan.flat_srt_rejected);
	const auto recipes_before=a.stats.recipe_evaluations;
	const auto begin=ProfileClockNs();
	bool result;
	if (action==FlatSrtAction::Compare) {
		++a.compared;
		result=ShadowFlatSrtMaterializeResources(plan,runtime,resources,specialization,a.stats);
	} else {
		auto selected=runtime;
		selected.compiled_flat_srt=action==FlatSrtAction::Candidate;
		selected.flat_srt_stats=&a.stats;
		if (selected.compiled_flat_srt) ++a.fast;
		else { ++a.reference; if (control.mode==DescriptorEvaluationMode::Fast) ++a.blocked; }
		result=MaterializeResources(plan,selected,resources,specialization);
	}
	const auto elapsed=ProfileClockNs()-begin; a.cpu+=elapsed;
	if (action==FlatSrtAction::Compare) a.shadow_cpu+=elapsed;
	else if (action==FlatSrtAction::Candidate) { a.fast_cpu+=elapsed; a.fast_recipes+=a.stats.recipe_evaluations-recipes_before; }
	else a.reference_cpu+=elapsed;
	if (plan.flat_srt_rejected && !rejected.exchange(true,std::memory_order_relaxed)) {
		std::fprintf(stderr,"[flat-srt] MISMATCH shader=0x%016llx; disabling all scopes\n",static_cast<unsigned long long>(plan.shader_hash));
		std::fflush(stderr); control.Report(ProfileClockNs()); control.mode=DescriptorEvaluationMode::Off;
	}
	return result;
}

bool MaterializeDiagnosticWindow(const ShaderRecompiler::IR::ResourcePlan& plan,
                                 const ShaderRecompiler::IR::SrtRuntime& runtime,
                                 ShaderRecompiler::IR::ResourceSnapshot& resources,
                                 ShaderRecompiler::IR::ResourceSpecialization& specialization) {
	using namespace ShaderRecompiler::IR;
	static const auto* control_path = std::getenv("KYTY_MATERIALIZER_PROFILE_CONTROL_FILE");
	if (!control_path) return MaterializeDescriptorExperiment(plan, runtime, resources, specialization);
	struct Window {
		struct Aggregate { uint64_t calls = 0, samples = 0, cpu = 0, sampled_cpu = 0; SrtEvaluationProfile detail; };
		std::unordered_map<uint64_t, Aggregate> shaders;
		std::unordered_set<const ResourcePlan*> described;
		uint64_t sequence = 0, next_poll = 0, until = 0, next_report = 0, start = 0;
		FILE* csv = [] {
			const auto* path = std::getenv("KYTY_MATERIALIZER_PROFILE_CSV");
			FILE* f = path ? std::fopen(path, "a+") : nullptr;
			if (f) {
				std::fseek(f, 0, SEEK_END);
				if (std::ftell(f) == 0) std::fprintf(f, "kind,host_ns,thread,shader,plan,calls,samples,cpu_ns,phase,opcode,words,hits,misses,failures,source,dword,metadata\n");
				std::fflush(f);
			}
			return f;
		}();
		void Report(uint64_t now, const char* kind = "aggregate") {
			if (csv) {
				static constexpr const char* phases[] = {"flat_srt","uniform_fill","buffers","images","samplers","specialization"};
				for (const auto& [shader, a] : shaders) {
					std::fprintf(csv, "%s,%llu,%llu,0x%016llx,0,%llu,%llu,%llu,,,0,0,0,0,0,0,window_start=%llu;sample_cpu_ns=%llu;sample_every=32\n",
					    kind, now, ProfileThreadId(), shader, a.calls, a.samples, a.cpu, start, a.sampled_cpu);
					for (size_t p = 0; p < a.detail.phases.size(); ++p)
						std::fprintf(csv, "phase,%llu,%llu,0x%016llx,0,0,%llu,%llu,%s,,0,0,0,0,0,0,\n",
						    now, ProfileThreadId(), shader, a.samples, a.detail.phases[p], phases[p]);
					for (size_t op = 0; op < a.detail.opcodes.size(); ++op) {
						const auto& o = a.detail.opcodes[op];
						if (!o.visits && !o.words) continue;
						const auto name = op == static_cast<size_t>(ValueOpcode::Count) ? std::string_view("immediate") : ValueOpcodeName(static_cast<ValueOpcode>(op));
						std::fprintf(csv, "opcode,%llu,%llu,0x%016llx,0,%llu,%llu,%llu,,%.*s,%llu,%llu,%llu,%llu,0,0,\n",
						    now, ProfileThreadId(), shader, o.visits, a.samples, o.word_ns, static_cast<int>(name.size()), name.data(),
						    o.words, o.hits, o.misses, o.failures);
					}
				}
				std::fprintf(csv, "marker,%llu,%llu,0,0,0,0,0,,,0,0,0,0,0,0,%s\n", now, ProfileThreadId(), kind);
				std::fflush(csv);
			}
			shaders.clear(); start = now; next_report = now + 2'000'000'000ull;
		}
		void Describe(const ResourcePlan& p, uint64_t now) {
			if (!csv || !described.insert(&p).second) return;
			for (size_t source = 0; source < p.descriptor_sources.size(); ++source) {
				const auto& desc = p.descriptor_sources[source];
				for (size_t word = 0; word < desc.dword_count; ++word) {
					const auto root = desc.dwords[word].Resolve();
					const auto* inst = root.TryInstruction();
					const auto root_name = inst ? std::string(ValueOpcodeName(inst->GetOpcode())) : "immediate_" + TypeName(root.GetType());
					std::unordered_map<std::string, size_t> dependencies;
					std::unordered_set<const Inst*> seen;
					std::vector<Value> pending {root};
					bool bounded = true;
					while (!pending.empty()) {
						const auto value = pending.back().Resolve(); pending.pop_back();
						const auto* node = value.TryInstruction();
						if (!node || !seen.insert(node).second) continue;
						if (seen.size() > 2048) { bounded = false; break; }
						++dependencies[std::string(ValueOpcodeName(node->GetOpcode()))];
						for (size_t i = 0; i < node->NumArgs(); ++i) pending.push_back(node->Arg(i));
						if (node->GetOpcode() == ValueOpcode::ReadConst && node->NumArgs() == 2) {
							const auto slot = node->Arg(1).Resolve();
							if (slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < p.srt_reads.size())
								pending.push_back(p.srt_reads[slot.U32()].value);
						}
					}
					std::string metadata = fmt::format("pure_word={};complete_gather={};bounded={};dependencies=",
					    (root.IsImmediate() && root.GetType() == Type::U32) || (inst && inst->GetOpcode() == ValueOpcode::GetUserData),
					    source < p.descriptor_gathers.size() && p.descriptor_gathers[source].eligible, bounded);
					for (const auto& [op, n] : dependencies) metadata += fmt::format("{}:{}|", op, n);
					std::fprintf(csv, "graph,%llu,%llu,0x%016llx,0x%llx,0,0,0,,%s,0,0,0,0,%zu,%zu,%s\n",
					    now, ProfileThreadId(), p.shader_hash, reinterpret_cast<uintptr_t>(&p), root_name.c_str(), source, word, metadata.c_str());
				}
			}
		}
		~Window() { Report(ProfileClockNs(), "shutdown"); if (csv) std::fclose(csv); }
	};
	static thread_local Window window;
	const auto now = ProfileClockNs();
	std::optional<ProfileCpuScope> diagnostic;
	const auto diagnostic_scope = [&] { if (auto* s = ProfileThread().scheduler) diagnostic.emplace(*s, "profile_materializer_diagnostic_cpu"); };
	if (now >= window.next_poll) {
		diagnostic_scope();
		window.next_poll = now + 500'000'000ull;
		if (FILE* f = std::fopen(control_path, "r")) {
			char text[16] {}; const bool arm = std::fgets(text, sizeof(text), f) && std::strncmp(text, "on", 2) == 0;
			std::fclose(f);
			if (arm && window.csv) {
				window.Report(now, "arm");
				const auto* seconds = std::getenv("KYTY_MATERIALIZER_PROFILE_SECONDS");
				const auto duration = seconds ? std::clamp(std::atoi(seconds), 1, 120) : 20;
				window.until = now + duration * 1'000'000'000ull;
				if (FILE* consume = std::fopen(control_path, "w")) { std::fputs("off\n", consume); std::fclose(consume); }
				std::fprintf(stderr, "[materializer-profile] reference window armed for %d seconds\n", duration);
				std::fflush(stderr);
			}
		}
		diagnostic.reset();
	}
	if (!window.until) return MaterializeDescriptorExperiment(plan, runtime, resources, specialization);
	if (now >= window.until) {
		diagnostic_scope(); window.Report(now, "complete"); window.until = 0; diagnostic.reset();
		return MaterializeDescriptorExperiment(plan, runtime, resources, specialization);
	}
	diagnostic_scope();
	if (now >= window.next_report) window.Report(now);
	uint64_t choice = (++window.sequence) + 0x9e3779b97f4a7c15ull;
	choice = (choice ^ (choice >> 30)) * 0xbf58476d1ce4e5b9ull;
	choice = (choice ^ (choice >> 27)) * 0x94d049bb133111ebull;
	const bool sample = ((choice ^ (choice >> 31)) & 31u) == 0;
	if (sample) window.Describe(plan, now);
	auto& aggregate = window.shaders[plan.shader_hash];
	++aggregate.calls;
	std::optional<SrtEvaluationProfile> observation;
	SrtRuntime observed = runtime;
	if (sample) { observation.emplace(); observed.profile = &*observation; }
	diagnostic.reset();
	const auto begin = ProfileClockNs();
	const bool result = MaterializeResources(plan, observed, resources, specialization);
	const auto elapsed = ProfileClockNs() - begin;
	diagnostic_scope();
	aggregate.cpu += elapsed;
	if (sample) {
		++aggregate.samples; aggregate.sampled_cpu += elapsed;
		for (size_t i = 0; i < observation->phases.size(); ++i) aggregate.detail.phases[i] += observation->phases[i];
		for (size_t i = 0; i < observation->opcodes.size(); ++i) {
			auto& a = aggregate.detail.opcodes[i]; const auto& b = observation->opcodes[i];
			a.visits += b.visits; a.hits += b.hits; a.misses += b.misses; a.failures += b.failures; a.words += b.words; a.word_ns += b.word_ns;
		}
	}
	return result;
}

struct MemorySnapshot {
	uint64_t blocks = 0;
	uint64_t usage = 0;
};

bool VramEventsEnabled() {
	static const bool enabled = std::getenv("KYTY_VRAM_EVENTS") != nullptr;
	return enabled;
}

MemorySnapshot ReadMemorySnapshot(const GraphicContext& graphics) {
	MemorySnapshot snapshot;
	graphics.SampleDeviceLocalUsage(snapshot.usage, snapshot.blocks);
	return snapshot;
}

void LogVramEvent(const char* kind, uint64_t id, MemorySnapshot before, MemorySnapshot after,
                  uint64_t detail = 0) {
	if (std::FILE* log = std::fopen("D:/PS5/vram-events.txt", "a"); log != nullptr) {
		const auto gap_before = static_cast<int64_t>(before.usage) - static_cast<int64_t>(before.blocks);
		const auto gap_after = static_cast<int64_t>(after.usage) - static_cast<int64_t>(after.blocks);
		std::fprintf(log,
		             "%s id=0x%016llx detail=0x%016llx vma=%llu->%lluMB driver=%llu->%lluMB "
		             "gap=%lld->%lldMB gap_delta=%lldMB gap_delta_kb=%lld\n",
		             kind, static_cast<unsigned long long>(id),
		             static_cast<unsigned long long>(detail),
		             static_cast<unsigned long long>(before.blocks / (1024 * 1024)),
		             static_cast<unsigned long long>(after.blocks / (1024 * 1024)),
		             static_cast<unsigned long long>(before.usage / (1024 * 1024)),
		             static_cast<unsigned long long>(after.usage / (1024 * 1024)),
		             static_cast<long long>(gap_before / (1024 * 1024)),
		             static_cast<long long>(gap_after / (1024 * 1024)),
		             static_cast<long long>((gap_after - gap_before) / (1024 * 1024)),
		             static_cast<long long>((gap_after - gap_before) / 1024));
		std::fclose(log);
	}
}

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	ProfileDetailScope profile("cpu_specialization_memory_read", address, values.size_bytes());
	const bool success = !values.empty() &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, values.data(), values.size_bytes());
	if (success) if (auto* scheduler = ProfileThread().scheduler) scheduler->ProfileBufferUse(
	    "cpu_consume_specialization", address, values.size_bytes(), 0);
	return success;
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
	static const bool dump_all = [] {
		const char* value = std::getenv("KYTY_DUMP_SHADERS");
		return value != nullptr && value[0] == '1';
	}();
	if (std::getenv("KYTY_DUMP_DISASM") != nullptr && !code.empty()) {
		// KYTY_DUMP_DISASM=1: RDNA listing named by the guest shader hash (see the [MSL] variant log for the map).
		ShaderRecompiler::Decoder::Program decoded;
		ShaderRecompiler::Decoder::DecodeProgram(code, decoded);
		const auto disasm_path = std::filesystem::path("/tmp/msl_direct") / fmt::format("disasm_{:016x}.txt", shader_hash);
		std::error_code disasm_ec;
		std::filesystem::create_directories(disasm_path.parent_path(), disasm_ec);
		if (FILE* f = std::fopen(disasm_path.c_str(), "w"); f != nullptr) {
			const std::string text = ShaderRecompiler::Decoder::ProgramToString(decoded);
			std::fwrite(text.data(), 1, text.size(), f);
			std::fclose(f);
		}
	}
	if (dump_all && !code.empty()) {
		// KYTY_DUMP_SHADERS=1: keep every original guest shader, named by hash.
		const auto all_path = std::filesystem::path("_TempData/shaders") /
		                      fmt::format("{}_{:016x}.bin", stage_name, shader_hash);
		std::error_code ec;
		std::filesystem::create_directories(all_path.parent_path(), ec);
		if (!std::filesystem::exists(all_path, ec)) {
			if (FILE* f = std::fopen(all_path.c_str(), "wb"); f != nullptr) {
				std::fwrite(code.data(), 1, code.size_bytes(), f);
				std::fclose(f);
			}
		}
	}
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

std::size_t PipelineCache::GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const {
	std::size_t hash = 0;
	PipelineKeyHash::Mix(hash, key.rendering.color_count);
	for (uint32_t i = 0; i < key.rendering.color_count; i++) {
		PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.color_formats[i]));
	}
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
	PipelineKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
	for (const auto id: key.vertex_shader_ids) {
		PipelineKeyHash::Mix(hash, id);
	}
	PipelineKeyHash::Mix(hash, key.ps_shader_id);
	PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
	for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
		PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
	}
	PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
	for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
		PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
	}
	PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
	return hash;
}

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	// Base Metal key; direct MSL additionally hashes the specialized source below.
	// The Metal function cache is keyed by hash alone, so permutations that change the
	// fragment output type (integer vs float attachments) must fold that state into
	// the key. Otherwise a uint4 variant is reused for a UNORM attachment and Metal
	// rejects every pipeline built from it.
	static uint64_t MetalFunctionVariant(const ShaderRecompiler::CompileOptions& options) {
		if (options.stage != ShaderType::Pixel || options.input_info.pixel == nullptr) {
			return 0;
		}
		const auto& pixel = *options.input_info.pixel;
		uint64_t hash = 1469598103934665603ull;
		const auto mix = [&](uint64_t value) {
			hash ^= value;
			hash *= 1099511628211ull;
		};
		mix(pixel.dual_source_blending);
		mix(pixel.alpha_blend_source_remap);
		for (size_t i = 0; i < std::size(pixel.target_output_mode); ++i) {
			mix(pixel.target_output_mode[i]);
			mix(pixel.target_integer[i]);
		}
		for (const auto& mapping: pixel.target_export_mapping) {
			for (uint32_t component = 0; component < 4; ++component) {
				mix(mapping.Map(component));
			}
		}
		return hash;
	}

	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword) {
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);

		// KYTY_MSL_DIRECT=<0|1|2>: exercise the direct RDNA->MSL emitter alongside the
		// existing SPIR-V path. 1 = log coverage, 2 = also dump the generated source.
		static const int s_msl_direct = [] {
			const char* value = std::getenv("KYTY_MSL_DIRECT");
			return value != nullptr ? std::atoi(value) : 0;
		}();
		if (s_msl_direct > 0) {
			auto msl = ShaderRecompiler::Msl::EmitProgram(result.program, options.input_info);
			LOGF("[MSL] hash=0x%016" PRIx64 " stage=%u success=%d unsupported=%zu\n",
			     options.shader_hash, static_cast<uint32_t>(options.stage),
			     msl.success ? 1 : 0, msl.unsupported.size());
			for (size_t i = 0; i < msl.unsupported.size() && i < 8; ++i) {
				LOGF("[MSL]   unsupported: %s\n", msl.unsupported[i].c_str());
			}
			if (s_msl_direct > 1) {
				const auto path = std::filesystem::path("/tmp/msl_direct") /
				                  fmt::format("shader_{:016x}.metal", options.shader_hash);
				std::error_code ec;
				std::filesystem::create_directories(path.parent_path(), ec);
				if (FILE* file = std::fopen(path.c_str(), "w"); file != nullptr) {
					std::fwrite(msl.source.data(), 1, msl.source.size(), file);
					std::fclose(file);
				}
				const auto report_path = std::filesystem::path("/tmp/msl_direct") /
				                         fmt::format("shader_{:016x}.report", options.shader_hash);
				if (FILE* report = std::fopen(report_path.c_str(), "w"); report != nullptr) {
					std::fprintf(report, "stage=%u success=%d unsupported=%zu\n",
					             static_cast<uint32_t>(options.stage), msl.success ? 1 : 0,
					             msl.unsupported.size());
					for (const auto& item: msl.unsupported) {
						std::fprintf(report, "%s\n", item.c_str());
					}
					std::fclose(report);
				}
			}
		}

		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

#if defined(__APPLE__)
		if (Metal::UseNativeMetalBackend()) {
			const uint64_t metal_hash = options.shader_hash ^ MetalFunctionVariant(options);
			// KYTY_MSL_BACKEND=1 compiles the direct RDNA->MSL translation. Shaders
			// the MSL backend cannot express fall back to the SPIR-V path below.
			static const bool s_msl_backend = [] {
				const char* value = std::getenv("KYTY_MSL_BACKEND");
				return value != nullptr && value[0] == '1';
			}();
			// Compute kernels stay on the SPIR-V path by default: the MSL dispatcher
			// can place threadgroup barriers inside divergent state-machine cases,
			// which deadlocks the GPU (progress timeout). Opt in with KYTY_MSL_COMPUTE=1.
			static const bool s_msl_compute = [] {
				const char* value = std::getenv("KYTY_MSL_COMPUTE");
				return value != nullptr && value[0] == '1';
			}();
			const bool msl_stage_allowed =
			    options.stage != ShaderType::Compute || s_msl_compute;
			// KYTY_MSL_ONLY=1: the direct RDNA->MSL emitter is the only Metal path. No SPIR-V
			// compile happens here. A shader it cannot translate is logged once and registered
			// without a Metal function, so draws that use it are skipped (see draw paths).
			static const bool s_msl_only = [] {
				const char* value = std::getenv("KYTY_MSL_ONLY");
				return value != nullptr && value[0] == '1';
			}();
			if (s_msl_only) {
				auto msl = ShaderRecompiler::Msl::EmitProgram(result.program, options.input_info);
				uint64_t msl_hash = Metal::NativeMslFunctionKey(
				    metal_hash, static_cast<uint32_t>(options.stage), msl.source, msl.success);
				// KYTY_MSL_OVERRIDE_FILE=<path> KYTY_MSL_OVERRIDE_HASH=<hash>: compile the given MSL file
				// instead of the emitted source for that one shader (diagnostic probes only).
				if (const char* ov_file = std::getenv("KYTY_MSL_OVERRIDE_FILE")) {
					const char* ov_hash = std::getenv("KYTY_MSL_OVERRIDE_HASH");
					if (ov_hash != nullptr && (msl_hash == std::strtoull(ov_hash, nullptr, 0) ||
					                           metal_hash == std::strtoull(ov_hash, nullptr, 0))) {
						if (std::FILE* f = std::fopen(ov_file, "rb")) {
							std::string text;
							char buf[4096];
							size_t n = 0;
							while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
							std::fclose(f);
							msl.source = std::move(text);
							msl.success = true;
							std::fprintf(stderr, "[MSL] override hash=0x%016llx from %s\n",
							           static_cast<unsigned long long>(msl_hash), ov_file);
						}
					}
				}
				// KYTY_MSL_SKIP_STAGE=<n>: treat every shader of that stage as unsupported (bisect only).
				if (const char* skip_stage = std::getenv("KYTY_MSL_SKIP_STAGE")) {
					if (static_cast<uint32_t>(options.stage) == static_cast<uint32_t>(std::atoi(skip_stage))) {
						msl.success = false;
					}
				}
				// KYTY_MSL_SKIP_HASH=<hash>[,<hash>...]: treat these shaders as unsupported (their draws/dispatches are
				// skipped). Used to bisect which shader causes a GPU fault.
				if (const char* skip = std::getenv("KYTY_MSL_SKIP_HASH")) {
					const std::string list = std::string(",") + skip + ",";
					const std::string needle = ",0x" + fmt::format("{:016x}", msl_hash) + ",";
					const std::string legacy_needle = ",0x" + fmt::format("{:016x}", metal_hash) + ",";
					if (list.find(needle) != std::string::npos || list.find(legacy_needle) != std::string::npos) {
						msl.success = false;
					}
				}
				// Include every baked resource format/stride and static state, not only fragment exports.
				// Recompute after diagnostics so a failed/overridden permutation cannot reuse another function.
				msl_hash = Metal::NativeMslFunctionKey(
				    metal_hash, static_cast<uint32_t>(options.stage), msl.source, msl.success);
				if (std::getenv("KYTY_MSL_DUMP_BY_HASH") != nullptr) {
					std::fprintf(stderr, "[MSL] variant 0x%016llx from shader 0x%016llx\n",
					             static_cast<unsigned long long>(msl_hash), static_cast<unsigned long long>(options.shader_hash));
					if (msl.success) {
						const auto dump_path = std::filesystem::path("/tmp/msl_direct") / fmt::format("mh_{:016x}.metal", msl_hash);
						std::error_code dump_ec;
						std::filesystem::create_directories(dump_path.parent_path(), dump_ec);
						if (FILE* dump_file = std::fopen(dump_path.c_str(), "w"); dump_file != nullptr) {
							std::fwrite(msl.source.data(), 1, msl.source.size(), dump_file);
							std::fclose(dump_file);
						}
					}
				}
				if (msl.success) {
					auto compiled = Metal::MetalShaderManager::Instance().CompileMslToFunction(
					    msl.source, "main0", static_cast<uint32_t>(options.stage), msl_hash);
					if (compiled.success) {
						MslCoverageCount(true, options.stage);
						const auto id = ++next_shader_id;
						auto info = std::move(result.program).TakeCompiledInfo();
						info.shader_hash = msl_hash;
						info.native_msl  = true;
						return {
						    .specialization = std::move(specialization),
						    .program        = std::move(info),
						    .handle         = {.id = id, .module = nullptr},
						};
					}
					MslLogUnsupported(msl_hash, options.stage, {"MSL function compile failed: " + compiled.error_log});
				} else {
					MslLogUnsupported(msl_hash, options.stage, msl.unsupported);
				}
				MslCoverageCount(false, options.stage);
				const auto id = ++next_shader_id;
				auto info = std::move(result.program).TakeCompiledInfo();
				info.shader_hash = msl_hash;
				info.native_msl  = true;
				return {
				    .specialization = std::move(specialization),
				    .program        = std::move(info),
				    .handle         = {.id = id, .module = nullptr},
				};
			}
			if (s_msl_backend && msl_stage_allowed) {
				auto msl = ShaderRecompiler::Msl::EmitProgram(result.program, options.input_info);
				if (msl.success) {
					const uint64_t msl_hash = Metal::NativeMslFunctionKey(
					    metal_hash, static_cast<uint32_t>(options.stage), msl.source);
					auto compiled = Metal::MetalShaderManager::Instance().CompileMslToFunction(
					    msl.source, "main0", static_cast<uint32_t>(options.stage), msl_hash);
					if (compiled.success) {
						const auto id = ++next_shader_id;
						auto info = std::move(result.program).TakeCompiledInfo();
						// Draws look the Metal function up by this hash; it must be the
						// variant hash, not the raw shader hash.
						info.shader_hash = msl_hash;
						info.native_msl  = true;
						return {
						    .specialization = std::move(specialization),
						    .program        = std::move(info),
						    .handle         = {.id = id, .module = nullptr},
						};
					}
				}
				LOGF("[MSL] backend fallback for hash=0x%016" PRIx64 " unsupported=%zu\n",
				     options.shader_hash, msl.unsupported.size());
			}
			const auto spvc_abi = Metal::ComputeSpvcAbiLayout(result.program.bindings, options.stage);
			Metal::MetalShaderManager::Instance().CompileSpirvToFunction(
			    result.spirv, "main0", static_cast<uint32_t>(options.stage), metal_hash, &spvc_abi);
			const auto id = ++next_shader_id;
			auto info = std::move(result.program).TakeCompiledInfo();
			info.shader_hash = metal_hash;
			return {
			    .specialization = std::move(specialization),
			    .program        = std::move(info),
			    .handle         = {.id = id, .module = nullptr},
			};
		}
#endif

		const auto before_module = VramEventsEnabled() ? ReadMemorySnapshot(graphics) : MemorySnapshot {};
		const auto module = CompileSPV(result.spirv, device);
		const auto id     = ++next_shader_id;
		if (VramEventsEnabled()) {
			LogVramEvent("shader_module", id, before_module, ReadMemorySnapshot(graphics),
			             options.shader_hash);
		}
		EXIT_IF(module == nullptr);
		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = id, .module = module},
		};
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ProfileDetailScope profile("cpu_program_get", params.hash);
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.logical_stage;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}

		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		{ ProfileDetailScope key_profile("cpu_program_static_key", params.hash);
		  BuildStageStaticKey(input_info, lookup_key.static_state); }
		uint64_t profile_key = params.hash;
		if (profile.Active()) {
			ProfileCpuScope fingerprint(*ProfileThread().scheduler, "profile_fingerprint_cpu");
			profile_key = ProfileFingerprint(lookup_key.static_state.data(), lookup_key.static_state.size() * sizeof(uint32_t), params.hash);
			profile_key ^= static_cast<uint64_t>(stage) << 56;
			profile.SetIdentity(profile_key, lookup_key.static_state.size());
		}
		auto                                         entry = programs.find(lookup_key);
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		};
		if (entry != programs.end()) {
			ProfileDetailEvent("cache_program_source_hit", profile_key);
			{ ProfileDetailScope materialize("cpu_materialize_resources", params.hash);
			EXIT_IF(!MaterializeDiagnosticWindow(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization)); }
			if (const auto permutation = std::ranges::find_if(
			        entry->second.permutations, [&](const Permutation& candidate) {
				        const auto& layout = candidate.program.bindings;
				        return layout.push_data_start_dword ==
				                   ShaderRecompiler::IR::PushData::StartFor(
				                       push_data_cursor, layout.ShaderDataDwords()) &&
				               candidate.specialization == entry->second.specialization;
			        });
			    permutation != entry->second.permutations.end()) {
				ProfileDetailEvent("cache_program_permutation_hit", profile_key);
				input_info.stage = {.program   = &permutation->program,
				                    .resources = &entry->second.resources};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		ShaderStageInputInfo stage_input {};
		ProfileDetailEvent(entry == programs.end() ? "cache_program_source_miss" : "cache_program_permutation_miss", profile_key);
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		const char* label = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		ShaderRecompiler::CompileOptions options;
		options.optimization_type = Config::GetShaderOptimizationType();
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;

		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			options.wave_size = input_info.wave_size;
			if (stage == ShaderType::Mesh || stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size = stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else {
			options.wave_size = input_info.wave_size;
		}
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		if (entry == programs.end()) {
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program)).first;
			ProfileDetailScope materialize("cpu_materialize_resources", params.hash);
			EXIT_IF(!MaterializeDiagnosticWindow(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization));
		}
		entry->second.permutations.push_back(CompilePermutation(
		    stage_name, options, std::move(translated), entry->second.specialization, push_data_cursor));
		const auto& permutation = entry->second.permutations.back();
		input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return permutation.handle;
	}

	explicit ProgramCache(GraphicContext& graphics): graphics(graphics), device(graphics.device) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ProgramKey                                                  lookup_key;
	GraphicContext&                                             graphics;
	vk::Device                                                  device;
	uint64_t                                                    next_shader_id = 0;
};

PipelineCache::PipelineCache(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore),
      m_program_cache(std::make_unique<ProgramCache>(graphics)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	InitializeDriverCache();
}

void PipelineCache::TrimPipelines() {
	// Pipelines are driver-managed memory that VMA cannot see and no cache GC can reclaim. Without
	// a bound they accumulate for the whole session - measured at ~4.2 MB each and ~3.3 GB in-fight
	// on UFC 5, which pushed the process past the driver budget and spilled to shared system memory.
	//
	// A pipeline may only be destroyed once the GPU has passed the tick at which it was last used;
	// before that a submitted command buffer still references it. Candidates are therefore filtered
	// through MasterSemaphore::IsFree and evicted oldest-use-first.
	//
	// KYTY_PIPELINE_BUDGET is the maximum number of pipelines to retain. 0 disables trimming, which
	// is the default until a budget has been measured against the live working set.
	static const size_t budget = [] {
		const char* env = std::getenv("KYTY_PIPELINE_BUDGET");
		if (env == nullptr) {
			return size_t {0};
		}
		char*      end    = nullptr;
		const auto parsed = std::strtoull(env, &end, 10);
		return end == env ? size_t {0} : static_cast<size_t>(parsed);
	}();
	if (budget == 0) {
		return;
	}
	const size_t total = m_graphics_pipelines.size() + m_compute_pipelines.size();
	if (total <= budget) {
		return;
	}

	struct Candidate {
		uint64_t tick     = 0;
		bool     compute  = false;
		uint64_t compute_key = 0;
		const GraphicsPipelineKey* graphics_key = nullptr;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(total);
	for (const auto& [key, pipeline]: m_graphics_pipelines) {
		if (m_master_semaphore.IsFree(pipeline->last_used_tick)) {
			candidates.push_back({pipeline->last_used_tick, false, 0, &key});
		}
	}
	for (const auto& [key, pipeline]: m_compute_pipelines) {
		if (m_master_semaphore.IsFree(pipeline->last_used_tick)) {
			candidates.push_back({pipeline->last_used_tick, true, key, nullptr});
		}
	}
	std::sort(candidates.begin(), candidates.end(),
	          [](const Candidate& a, const Candidate& b) { return a.tick < b.tick; });

	size_t remaining = total;
	size_t evicted   = 0;
	for (const auto& candidate: candidates) {
		if (remaining <= budget) {
			break;
		}
		const auto destroy = [this](const Pipeline& pipeline, const char* kind, uint64_t id) {
			const auto before = VramEventsEnabled() ? ReadMemorySnapshot(m_graphics) : MemorySnapshot {};
			{
				VramAttributionScope vram(m_graphics, "pipeline_destroy");
				m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
			}
			if (VramEventsEnabled()) {
				LogVramEvent(kind, id, before, ReadMemorySnapshot(m_graphics));
			}
			m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
		};
		if (candidate.compute) {
			const auto found = m_compute_pipelines.find(candidate.compute_key);
			if (found == m_compute_pipelines.end()) {
				continue;
			}
			destroy(*found->second, "compute_pipeline_destroy", candidate.compute_key);
			m_compute_pipelines.erase(found);
		} else {
			const auto found = m_graphics_pipelines.find(*candidate.graphics_key);
			if (found == m_graphics_pipelines.end()) {
				continue;
			}
			destroy(*found->second, "graphics_pipeline_destroy",
			        (found->first.vertex_shader_ids[0] << 32u) | found->first.ps_shader_id);
			m_graphics_pipelines.erase(found);
		}
		--remaining;
		++evicted;
	}
	if (evicted != 0) {
		if (std::FILE* f = std::fopen("D:/PS5/pipeline-census.txt", "a"); f != nullptr) {
			std::fprintf(f, "PipeTrim: evicted=%zu remaining=%zu budget=%zu candidates=%zu\n",
			             evicted, remaining, budget, candidates.size());
			std::fclose(f);
		}
	}
}

PipelineCache::~PipelineCache() {
#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		return;
	}
#endif
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		PipelineCacheLog("Vulkan pipeline cache: disabled (unknown git revision)");
		return;
	}
	static const bool force_cache = [] {
		const char* env = std::getenv("KYTY_FORCE_PIPELINE_CACHE");
		return env == nullptr || env[0] != '0';
	}();
	if (git_hash.ends_with("-dirty") && !force_cache) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (dirty build)");
		return;
	}

	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

void PipelineCache::Save() {
	if (m_driver_cache == nullptr) {
		return;
	}

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		return;
	}
	payload.resize(size);
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return;
	}
	PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {}", payload.size(),
	                 Common::PathToString(m_driver_cache_path));
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	ProfileDetailScope profile("cpu_graphics_programs");
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		ProfileDetailScope prepare("cpu_prepare_vertex_program", vertex_regs.es_regs.data_addr);
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info[0].mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		{ ProfileDetailScope prepare("cpu_prepare_pixel_program", pixel_regs.ps_regs.data_addr);
		  pixel_params = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info); }
#if defined(__APPLE__)
		for (uint32_t i = 0; i < std::size(pixel_info.target_integer); i++) {
			const auto channel_type = context.GetRenderTarget(i).info.channel_type;
			pixel_info.target_integer[i] =
			    channel_type == Prospero::ChannelType::kUInt   ? 1u
			    : channel_type == Prospero::ChannelType::kSInt ? 2u
			                                                   : 0u;
		}
#endif
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; }) &&
		           ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0]) ==
		               BlendMappingSupport::SourceAlpha) {
			// Preserve logical alpha when the export mapping moves it.
			pixel_info.alpha_blend_source_remap = true;
			pixel_info.dual_source_blending     = true;
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = {};
		}
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info[tess_active ? 2u : 0u].clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
#if defined(__APPLE__)
	const bool negative_one_to_one = !context.GetClipControl().dx_clip_space;
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		vertex_info[i].clip_space.remap_z = negative_one_to_one;
	}
#endif
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_program_cache->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
#if defined(__APPLE__)
	const auto max_lds_dwords = Metal::UseNativeMetalBackend() ? 8192u :
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize / 4u;
#else
	const auto max_lds_dwords =
	    m_graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize / 4u;
#endif
	if (input_info.lds_size_dwords > max_lds_dwords) {
		static std::atomic_bool warned = false;
		if (!warned.exchange(true, std::memory_order_relaxed)) {
			PipelineCacheLog("GPU warning: game compute shader requests {} bytes of LDS, but "
			                 "the Vulkan device limit is {} bytes. Clamping LDS; rendering may "
			                 "be incorrect.",
			                 input_info.lds_size_dwords * 4u, max_lds_dwords * 4u);
		}
	}
	input_info.lds_size_dwords = std::min(input_info.lds_size_dwords, max_lds_dwords);
	// KYTY_CS_LDS_MIN=<dwords>: raise the LDS declaration of every compute shader (diagnosis).
	static const uint32_t lds_min = [] {
		const char* value = std::getenv("KYTY_CS_LDS_MIN");
		return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
	}();
	if (lds_min != 0 && input_info.lds_size_dwords < lds_min) {
		input_info.lds_size_dwords = std::min(lds_min, max_lds_dwords);
	}
	uint32_t          push_data_cursor = 0;
	auto program = m_program_cache->Get(params, input_info, push_data_cursor);
	{
		static std::mutex                   mutex;
		static std::unordered_set<uint64_t> seen;
		std::lock_guard                     lock(mutex);
		if (input_info.stage.program != nullptr &&
		    seen.insert(input_info.stage.program->shader_hash).second) {
			std::fprintf(stderr, "[cs] hash=0x%016llx lds_dwords=%u threads=%ux%ux%u wave=%u\n",
			             static_cast<unsigned long long>(input_info.stage.program->shader_hash),
			             input_info.lds_size_dwords, input_info.threads_num[0],
			             input_info.threads_num[1], input_info.threads_num[2], input_info.wave_size);
		}
	}
	return program;
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		key.vertex_shader_ids[i] = programs.vertex[i].id;
	}
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = 0;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const auto slot = colors[i].target_slot;
		EXIT_IF(slot >= RENDER_COLOR_ATTACHMENTS_MAX);
		rendering.color_count = std::max(rendering.color_count, slot + 1);
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[slot] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[slot] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		const bool alpha_remap =
		    slot == 0 && ps_input_info != nullptr && ps_input_info->alpha_blend_source_remap;
		static_params.blend_enable[slot] = bc.enable && !rt.info.blend_bypass;
		if (static_params.blend_enable[slot] && IsIntegerFormat(colors[i].desc.view_info.format)) {
			static_params.blend_enable[slot] = false;
		}
		if (static_params.blend_enable[slot] && !alpha_remap &&
		    ClassifyBlendMapping(bc, colors[i].export_mapping) != BlendMappingSupport::Direct) {
			static_params.blend_enable[slot] = false;
			static std::atomic_bool warned = false;
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Warning: blending disabled for unsupported color mapping "
				    "(slot={} mapping=0x{:02x} color={}/{} alpha={}/{} separate={}).\n",
				    slot, colors[i].export_mapping.packed, bc.color_srcblend, bc.color_destblend,
				    bc.alpha_srcblend, bc.alpha_destblend, bc.separate_alpha_blend ? 1 : 0));
			}
		}
		if (alpha_remap) {
			static_params.blend_alpha_source_remap = true;
		}
		if (static_params.blend_enable[slot]) {
			static_params.color_srcblend[slot]       = bc.color_srcblend;
			static_params.color_comb_fcn[slot]       = bc.color_comb_fcn;
			static_params.color_destblend[slot]      = bc.color_destblend;
			static_params.separate_alpha_blend[slot] = bc.separate_alpha_blend;
			if (bc.separate_alpha_blend) {
				static_params.alpha_srcblend[slot]  = bc.alpha_srcblend;
				static_params.alpha_comb_fcn[slot]  = bc.alpha_comb_fcn;
				static_params.alpha_destblend[slot] = bc.alpha_destblend;
			}
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		static const bool force_one = [] {
			const char* env = std::getenv("KYTY_NOATT_SAMPLES1");
			return env != nullptr && env[0] == '1';
		}();
		if (force_one) {
			attachment_samples = 1;
		}
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = !clip_control.min_z_clip_disable && !clip_control.clip_disable;
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	const bool rect_list = Prospero::IsRectList(command.GetUserConfig().GetPrimType());
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
		}
		for (int attribute = 0; attribute < vs_input_info.resources_num; attribute++) {
			const auto binding = vs_input_info.resources_dst[attribute].buffer_index;
			EXIT_IF(binding < 0 || binding >= vs_input_info.buffers_num);
			key.vertex_input.attributes[attribute] = {
			    .offset = static_cast<uint32_t>(vs_input_info.resources[attribute].Base48() -
			                                    vs_input_info.buffers[binding].addr),
			    .binding = static_cast<uint8_t>(binding),
			};
		}
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		iter->second->last_used_tick = m_master_semaphore.CurrentTick();
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	const auto before_pipeline = VramEventsEnabled() ? ReadMemorySnapshot(m_graphics) : MemorySnapshot {};
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
	                       ps_input_info, programs, static_params, m_driver_cache);
	if (VramEventsEnabled()) {
		LogVramEvent("graphics_pipeline", (vs_id << 32u) | ps_id, before_pipeline,
		             ReadMemorySnapshot(m_graphics));
	}
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

#if defined(__APPLE__)
	if (Metal::UseNativeMetalBackend()) {
		EXIT_NOT_IMPLEMENTED(cached->metal_pipeline == nullptr);
		auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
		return *iter->second;
	}
#endif
	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	// TEMPORARY DIAGNOSTIC (session 17): pipelines are only destroyed in ~PipelineCache, so they
	// accumulate for the whole session. Driver-managed memory (outside VMA) grew 12 MB -> 3.3 GB
	// during one fight; this reports the count so bytes-per-pipeline can be estimated.
	{
		static std::chrono::steady_clock::time_point next_report {};
		static uint64_t                             s_census_mark = 0;
		const auto                                  now = std::chrono::steady_clock::now();
		if (now >= next_report) {
			next_report = now + std::chrono::seconds(2);
			if (std::FILE* f = std::fopen("D:/PS5/pipeline-census.txt", "a"); f != nullptr) {
				size_t driver_cache_bytes = 0;
				if (m_driver_cache != nullptr) {
					(void)m_graphics.device.getPipelineCacheData(m_driver_cache,
					                                            &driver_cache_bytes, nullptr);
				}
				size_t live = 0;
				size_t in_flight = 0;
				const auto tally = [&](const auto& pipelines) {
					for (const auto& [key, pipeline]: pipelines) {
						(void)key;
						if (pipeline->last_used_tick >= s_census_mark) {
							++live;
						}
						if (!m_master_semaphore.IsFree(pipeline->last_used_tick)) {
							++in_flight;
						}
					}
				};
				tally(m_graphics_pipelines);
				tally(m_compute_pipelines);
				std::fprintf(f,
				             "PipeCensus: graphics=%zu compute=%zu total=%zu | live_since_last=%zu "
				             "in_flight=%zu | shader_modules=%llu driver_cache=%u cache_data=%zuMB\n",
				             m_graphics_pipelines.size(), m_compute_pipelines.size(),
				             m_graphics_pipelines.size() + m_compute_pipelines.size(), live, in_flight,
				             static_cast<unsigned long long>(m_program_cache->next_shader_id),
				             m_driver_cache != nullptr ? 1u : 0u,
				             driver_cache_bytes / (1024 * 1024));
				s_census_mark = m_master_semaphore.CurrentTick();
				std::fclose(f);
			}
		}
	}
	EXIT_IF(!inserted);
	iter->second->last_used_tick = m_master_semaphore.CurrentTick();
	TrimPipelines();

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		iter->second->last_used_tick = m_master_semaphore.CurrentTick();
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	const auto before_pipeline = VramEventsEnabled() ? ReadMemorySnapshot(m_graphics) : MemorySnapshot {};
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);
	if (VramEventsEnabled()) {
		LogVramEvent("compute_pipeline", compute_program.id, before_pipeline,
		             ReadMemorySnapshot(m_graphics));
	}

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);
	iter->second->last_used_tick = m_master_semaphore.CurrentTick();
	TrimPipelines();

	return *iter->second;
}
} // namespace Libs::Graphics
