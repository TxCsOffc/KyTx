#include "graphics/host_gpu/metal/metalEnv.h"
#include "graphics/host_gpu/metal/metalShader.h"
#include "graphics/host_gpu/metal/metalShaderAbi.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "common/logging/log.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include "graphics/shader/shader.h"

#include <spirv_cross/spirv_msl.hpp>
#include <atomic>
#include <unordered_set>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <regex>

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

namespace Libs::Graphics::Metal {

uint64_t NativeMslFunctionKey(uint64_t shader_hash, uint32_t stage,
                            std::string_view source, bool emitted) {
	uint64_t hash = 14695981039346656037ull;
	const auto mix = [&](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ull; };
	for (unsigned shift = 0; shift < 64; shift += 8) mix(uint8_t(shader_hash >> shift));
	for (unsigned shift = 0; shift < 32; shift += 8) mix(uint8_t(stage >> shift));
	mix(emitted ? 1 : 0);
	for (unsigned char byte: source) mix(byte);
	return hash != 0 ? hash : 1; // Zero is reserved for an absent shader in the draw paths.
}

// Hashes whose MSL declares the descriptor argument buffer at buffer 0 (see ShaderHasArgumentBuffer0).
static std::unordered_set<uint64_t> s_arg_buffer0_hashes;
static void RecordArgBuffer0(uint64_t hash) { s_arg_buffer0_hashes.insert(hash); }
bool ShaderHasArgumentBuffer0(uint64_t hash) { return s_arg_buffer0_hashes.count(hash) != 0; }

// KYTY_PS_PROBE=alpha1|uv|tex|cb: diagnostic rewrite of fragment entry returns.
// Overwrites the [[color(0)]] output with a known value before each `return out;`.
static std::string PsIdentBefore(const std::string& s, size_t end) {
	size_t e = end;
	while (e > 0 && s[e - 1] == ' ') --e;
	size_t b = e;
	while (b > 0 && (std::isalnum(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '_')) --b;
	return s.substr(b, e - b);
}

// KYTY_VS_DBG=<hash>: vertex-stage probe. For vertex_id < 8 writes 8 uints into buffer 27 (per id:
// vertex_id, instance_id, position x/y/z/w as float bits, 2 spare). Diagnostic only.
static void ApplyVsDbg(std::string& msl, uint64_t hash) {
	const char* only = KYTY_ENV("KYTY_VS_DBG");
	if (only == nullptr || std::strtoull(only, nullptr, 0) != hash) return;
	const size_t entry = msl.find("vertex main0_out main0(");
	if (entry == std::string::npos) return;
	const size_t vid_tag = msl.find("[[vertex_id]]", entry);
	const size_t pos_tag = msl.find("[[position]]");
	if (vid_tag == std::string::npos || pos_tag == std::string::npos) return;
	const std::string vid = PsIdentBefore(msl, vid_tag);
	const std::string pos = PsIdentBefore(msl, pos_tag);
	const size_t iid_tag = msl.find("[[instance_id]]", entry);
	const std::string iid = iid_tag != std::string::npos ? PsIdentBefore(msl, iid_tag) : std::string("0u");
	const std::string param = "device uint* kyty_vsdbg [[buffer(27)]], ";
	msl.insert(entry + std::strlen("vertex main0_out main0("), param);
	const std::string dump =
	    "\n\t{ uint _v = uint(" + vid + "); if (_v < 8u) { uint _o = _v * 8u; kyty_vsdbg[_o + 0u] = _v; "
	    "kyty_vsdbg[_o + 1u] = uint(" + iid + "); kyty_vsdbg[_o + 2u] = as_type<uint>(out." + pos + ".x); "
	    "kyty_vsdbg[_o + 3u] = as_type<uint>(out." + pos + ".y); kyty_vsdbg[_o + 4u] = as_type<uint>(out." + pos + ".z); "
	    "kyty_vsdbg[_o + 5u] = as_type<uint>(out." + pos + ".w); } }\n";
	const std::string ret = "return out;";
	size_t at = entry;
	int n = 0;
	while ((at = msl.find(ret, at)) != std::string::npos) {
		msl.insert(at, dump);
		at += dump.size() + ret.size();
		++n;
	}
	std::fprintf(stderr, "[VsDbg] hash=0x%016llx vid=%s pos=%s returns=%d\n", (unsigned long long)hash, vid.c_str(), pos.c_str(), n);
}

static void InstrumentFirstNan(std::string& msl, size_t frag) {
	const size_t entry_end = msl.find("KytyStageOut out = {};", frag);
	if (entry_end == std::string::npos) return;
	std::unordered_set<std::string> floats;
	std::string                     result = msl.substr(0, entry_end + std::strlen("KytyStageOut out = {};"));
	result += "\n\tuint kyty_first_nan = 0u;";
	size_t at = entry_end + std::strlen("KytyStageOut out = {};");
	while (at < msl.size()) {
		size_t eol = msl.find('\n', at);
		if (eol == std::string::npos) eol = msl.size();
		const std::string line = msl.substr(at, eol - at);
		result += line;
		// "\tfloat v123 = (float)0;" declares, "\t\tv123 = <expr>;" assigns.
		size_t b = line.find_first_not_of('\t');
		if (b != std::string::npos) {
			if (line.compare(b, 7, "float v") == 0) {
				const size_t sp = line.find(' ', b + 6);
				if (sp != std::string::npos && line.find(" = (float)0;", sp) == sp) floats.insert(line.substr(b + 6, sp - b - 6));
			} else if (line[b] == 'v' && line.size() > b + 1 && std::isdigit(static_cast<unsigned char>(line[b + 1])) && line.back() == ';') {
				const size_t sp = line.find(' ', b);
				if (sp != std::string::npos && line.compare(sp, 3, " = ") == 0 && floats.count(line.substr(b, sp - b)) != 0) {
					const std::string name = line.substr(b, sp - b);
					result += " if (kyty_first_nan == 0u && isnan(" + name + ")) kyty_first_nan = " + name.substr(1) + "u;";
				}
			}
		}
		if (eol < msl.size()) result += '\n';
		at = eol + 1;
	}
	msl = std::move(result);
}

// Guest arithmetic requires IEEE special values and precise floating-point functions.
static void ApplyMslMathMode(MTLCompileOptions* opts) {
	if (@available(macOS 15.0, *)) {
		opts.mathMode = MTLMathModeSafe;
		opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
	} else opts.fastMathEnabled = NO;
}

static std::string SanitizeMslSource(std::string_view msl) {
	std::string result;
	result.reserve(msl.size() + 256);

	std::istringstream stream {std::string(msl)};
	std::string line;
	bool in_descriptor_struct = false;
	uint32_t next_id = 0;

	while (std::getline(stream, line)) {
		if (line.find("struct spvDescriptorSetBuffer0") != std::string::npos) {
			in_descriptor_struct = true;
			next_id = 0;
			result += line;
			result += '\n';
			continue;
		}

		if (in_descriptor_struct) {
			if (line.find("};") != std::string::npos) {
				in_descriptor_struct = false;
				result += line;
				result += '\n';
				continue;
			}

			auto id_pos = line.find("[[id(");
			if (id_pos != std::string::npos) {
				size_t num_start = id_pos + 5;
				size_t num_end = line.find(")]]", num_start);
				if (num_end != std::string::npos) {
					uint32_t cur_id = static_cast<uint32_t>(std::stoul(line.substr(num_start, num_end - num_start)));
					static const bool s_no_renum = KYTY_ENV("KYTY_MSL_NO_RENUMBER") != nullptr;
					if (cur_id < next_id && !s_no_renum) {
						static std::atomic<uint32_t> s_renum_log {0};
						if (s_renum_log.fetch_add(1) < 60) {
							std::fprintf(stderr, "[MslSanitize] renumber id %u -> %u\n", cur_id, next_id);
						}
						line = line.substr(0, num_start) + std::to_string(next_id) + line.substr(num_end);
						cur_id = next_id;
					}

					uint32_t count = 1;
					auto last_semi = line.rfind(';');
					if (last_semi != std::string::npos) {
						auto close_bracket = line.rfind(']', last_semi);
						if (close_bracket != std::string::npos) {
							auto open_bracket = line.rfind('[', close_bracket);
							if (open_bracket != std::string::npos && open_bracket < close_bracket) {
								std::string dim = line.substr(open_bracket + 1, close_bracket - open_bracket - 1);
								if (!dim.empty() && std::isdigit(dim[0])) {
									count = static_cast<uint32_t>(std::stoul(dim));
								}
							}
						}
					}
					auto arr_pos = line.find("array<");
					if (arr_pos != std::string::npos) {
						auto last_gt = line.rfind('>');
						if (last_gt != std::string::npos) {
							auto last_comma = line.rfind(',', last_gt);
							if (last_comma != std::string::npos && last_comma < last_gt) {
								std::string dim = line.substr(last_comma + 1, last_gt - last_comma - 1);
								size_t start = dim.find_first_not_of(" \t");
								if (start != std::string::npos && std::isdigit(dim[start])) {
									count = static_cast<uint32_t>(std::stoul(dim.substr(start)));
								}
							}
						}
					}
					next_id = cur_id + count;
				}
			}
			result += line;
			result += '\n';
			continue;
		}

		auto buf_size_pos = line.find("BufferSize = &spvDescriptorSet");
		if (buf_size_pos != std::string::npos) {
			auto eq_pos = line.find('=', buf_size_pos);
			auto semi_pos = line.find(';', eq_pos);
			if (eq_pos != std::string::npos && semi_pos != std::string::npos) {
				line = line.substr(0, eq_pos + 2) + "spvBufferSizeConstants;" + line.substr(semi_pos + 1);
			}
		}

		result += line;
		result += '\n';
	}
	return result;
}

MetalShaderManager& MetalShaderManager::Instance() {
	static MetalShaderManager instance;
	return instance;
}

MetalShaderManager::MetalShaderManager() = default;
MetalShaderManager::~MetalShaderManager() {
	ClearCache();
}

void MetalShaderManager::ClearCache() {
	m_function_cache.clear();
}

// MSL disk cache key: schema version (bump when translator/sanitizer output changes) and
// probe mode, so stale or probed entries are never reused across builds or experiments.
static constexpr const char* kMslCacheSchema = "msl-s5-strict-semantics";

static bool MslDiskCacheDisabled() {
	static const bool s_off = KYTY_ENV("KYTY_MSL_NO_DISK_CACHE") != nullptr ||
	                          KYTY_ENV("KYTY_PS_PROBE") != nullptr ||
	                          KYTY_ENV("KYTY_VS_PROBE") != nullptr ||
	                          KYTY_ENV("KYTY_SPVC_ABI") != nullptr ||
	                          KYTY_ENV("KYTY_MSL_ONLY") != nullptr;
	return s_off;
}

static NSString* MslCacheDir() {
	const char* probe = KYTY_ENV("KYTY_PS_PROBE");
	NSString* base = [NSString stringWithFormat:@"_PipelineCache/msl/%s", kMslCacheSchema];
	return probe != nullptr ? [base stringByAppendingFormat:@"/probe-%s", probe] : base;
}

static NSString* MslCachePath(uint64_t hash) {
	return [NSString stringWithFormat:@"%@/0x%016llx.metal", MslCacheDir(), (unsigned long long)hash];
}

#ifdef __OBJC__
id<MTLFunction> MetalShaderManager::GetFunction(uint64_t hash) {
	if (hash == 0) return nil;
	auto it = m_function_cache.find(hash);
	if (it != m_function_cache.end()) {
		return it->second;
	}

	NSString* path = MslCachePath(hash);
	if (!MslDiskCacheDisabled() && [[NSFileManager defaultManager] fileExistsAtPath:path]) {
		NSString* disk_source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
		if (disk_source) {
			std::string sanitized = SanitizeMslSource([disk_source UTF8String]);

			if (sanitized.find("[[buffer(0)]]") != std::string::npos) {
				RecordArgBuffer0(hash);
			}


			ApplyVsDbg(sanitized, hash);
			NSString* source_str = [NSString stringWithUTF8String:sanitized.c_str()];
			id<MTLDevice> device = MetalContext::Instance().GetDevice();
			if (!device) {
				MetalContext::Instance().Initialize();
				device = MetalContext::Instance().GetDevice();
			}
			MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
#if defined(MTLLanguageVersion4_0)
			opts.languageVersion = MTLLanguageVersion4_0;
#elif defined(MTLLanguageVersion3_2)
			opts.languageVersion = MTLLanguageVersion3_2;
#else
			opts.languageVersion = MTLLanguageVersion3_0;
#endif
			ApplyMslMathMode(opts);
			NSError* err = nil;
			id<MTLLibrary> lib = [device newLibraryWithSource:source_str options:opts error:&err];
			if (lib) {
				id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
				if (!fn) {
					NSArray<NSString*>* names = [lib functionNames];
					if ([names count] > 0) fn = [lib newFunctionWithName:[names firstObject]];
				}
				if (fn) {
					m_function_cache.emplace(hash, fn);
					static std::atomic<uint32_t> s_disk_loaded {0};
					if (s_disk_loaded.fetch_add(1) < 20) {
						std::fprintf(stderr, "[MetalShaderManager] Loaded MSL for 0x%016llx from disk cache\n", (unsigned long long)hash);
					}
					return fn;
				}
			}
		}
	}
	return nil;
}
#else
id MetalShaderManager::GetFunction(uint64_t hash) {
	auto it = m_function_cache.find(hash);
	return it != m_function_cache.end() ? it->second : nullptr;
}
#endif


MetalShaderCompileResult MetalShaderManager::CompileSpirvToFunction(
    std::span<const uint32_t> spirv,
    const char* entry_point,
    uint32_t stage,
    uint64_t shader_hash,
    const SpvcAbiLayout* abi) {
	(void)stage;
	MetalShaderCompileResult result {};
	if (spirv.empty()) {
		result.error_log = "Empty SPIR-V buffer";
		return result;
	}

#ifdef __OBJC__
	auto cached = GetFunction(shader_hash);
	if (cached != nil) {
		result.function = cached;
		result.success  = true;
		return result;
	}
#endif

	try {
		spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());

		spirv_cross::CompilerMSL::Options options {};
		options.set_msl_version(3, 2);
		options.platform                      = spirv_cross::CompilerMSL::Options::macOS;
		options.enable_decoration_binding     = true;
		options.argument_buffers              = true;
		options.force_native_arrays           = true;
		options.ios_use_simdgroup_functions   = true;
		options.use_fast_math_pragmas         = true;
		options.agx_manual_cube_grad_fixup    = true;
		options.readwrite_texture_fences      = false;
		compiler.set_msl_options(options);

		// KYTY_SPVC_ABI: pin every descriptor to the id range from ComputeSpvcAbiLayout so arrays
		// no longer take consecutive ids that collide with the neighbouring kinds.
		static const bool s_spvc_abi = KYTY_ENV("KYTY_SPVC_ABI") != nullptr;
		if (s_spvc_abi && abi != nullptr) {
			const auto model = compiler.get_execution_model();
			for (const auto& e: abi->entries) {
				spirv_cross::MSLResourceBinding rb {};
				rb.stage    = model;
				rb.desc_set = 0;
				rb.binding  = e.binding;
				rb.count    = e.count;
				using K = ShaderRecompiler::IR::DescriptorBindingKind;
				if (e.kind == K::Samplers) {
					rb.msl_sampler = e.base_id;
				} else if (ShaderRecompiler::IR::ImageBindingResourceClass(e.kind) != ShaderRecompiler::IR::ImageResourceClass::None) {
					rb.msl_texture = e.base_id;
				} else {
					rb.msl_buffer = e.base_id;
				}
				compiler.add_msl_resource_binding(rb);
			}
		}

		result.msl_source = SanitizeMslSource(compiler.compile());
		if (result.msl_source.find("[[buffer(0)]]") != std::string::npos) {
			RecordArgBuffer0(shader_hash);
		}
		{
			// Diagnostic: total threadgroup memory declared by this MSL (Metal limit 32768 bytes).
			size_t tg_total = 0;
			for (size_t pos = 0; (pos = result.msl_source.find("threadgroup ", pos)) != std::string::npos; ++pos) {
				const size_t lb = result.msl_source.find('[', pos);
				const size_t eol = result.msl_source.find(';', pos);
				if (lb == std::string::npos || eol == std::string::npos || lb > eol) continue;
				const size_t rb = result.msl_source.find(']', lb);
				if (rb == std::string::npos || rb > eol) continue;
				const size_t count = std::strtoull(result.msl_source.substr(lb + 1, rb - lb - 1).c_str(), nullptr, 10);
				const std::string line = result.msl_source.substr(pos, lb - pos);
				size_t elem = 4;
				if (line.find("half") != std::string::npos || line.find("ushort") != std::string::npos) elem = 2;
				if (line.find("uchar") != std::string::npos) elem = 1;
				if (line.find("float4") != std::string::npos || line.find("uint4") != std::string::npos) elem = 16;
				else if (line.find("float2") != std::string::npos || line.find("uint2") != std::string::npos) elem = 8;
				tg_total += elem * count;
			}
			if (tg_total > 32768) {
				std::fprintf(stderr, "[TgOver] hash=0x%016llx threadgroup_bytes=%zu\n", (unsigned long long)shader_hash, tg_total);
			}
		}



		ApplyVsDbg(result.msl_source, shader_hash);
		static int s_dump_cnt = 0;
		if (s_dump_cnt < 30) {
			char path[128];
			snprintf(path, sizeof(path), "/tmp/msl_dump/shader_%016llx.metal", (unsigned long long)shader_hash);
			FILE* f = fopen(path, "w");
			if (f) {
				fputs(result.msl_source.c_str(), f);
				fclose(f);
				s_dump_cnt++;
			}
		}
	} catch (const std::exception& e) {
		result.error_log = std::string("SPIRV-Cross exception: ") + e.what();
		LOGF_COLOR(Log::Color::BrightRed,
		           "MetalShader: SPIRV translation failed for hash 0x%016llx: %s\n",
		           static_cast<unsigned long long>(shader_hash), e.what());
		return result;
	}

#ifdef __OBJC__
	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			MetalContext::Instance().Initialize();
			device = MetalContext::Instance().GetDevice();
			if (!device) {
				result.error_log = "Metal device not initialized";
				return result;
			}
		}

		MTLCompileOptions* compile_options = [[MTLCompileOptions alloc] init];
#if defined(MTLLanguageVersion4_0)
		compile_options.languageVersion    = MTLLanguageVersion4_0;
#elif defined(MTLLanguageVersion3_2)
		compile_options.languageVersion    = MTLLanguageVersion3_2;
#else
		compile_options.languageVersion    = MTLLanguageVersion3_0;
#endif
		ApplyMslMathMode(compile_options);

		NSError*  error      = nil;
		NSString* source_str = [NSString stringWithUTF8String:result.msl_source.c_str()];
		id<MTLLibrary> library =
		    [device newLibraryWithSource:source_str options:compile_options error:&error];
		if (!library) {
			result.error_log =
			    error ? [[error localizedDescription] UTF8String] : "Library compilation failed";
			LOGF_COLOR(Log::Color::BrightRed,
			           "MetalShader: MTLLibrary compilation failed for hash 0x%016llx: %s\n",
			           static_cast<unsigned long long>(shader_hash), result.error_log.c_str());
			return result;
		}

		NSString*       entry_name = entry_point ? [NSString stringWithUTF8String:entry_point] : @"main0";
		id<MTLFunction> function   = [library newFunctionWithName:entry_name];
		if (!function) {
			NSArray<NSString*>* names = [library functionNames];
			if ([names count] > 0) {
				function = [library newFunctionWithName:[names firstObject]];
			}
		}

		if (!function) {
			result.error_log = "Entry point function not found in MTLLibrary";
			return result;
		}

		result.library  = library;
		result.function = function;
		result.success  = true;
		m_function_cache.emplace(shader_hash, function);

		NSString* msl_dir = MslCacheDir();
		NSString* msl_path = MslCachePath(shader_hash);
		if (!MslDiskCacheDisabled()) {
			[[NSFileManager defaultManager] createDirectoryAtPath:msl_dir withIntermediateDirectories:YES attributes:nil error:nil];
			[source_str writeToFile:msl_path atomically:YES encoding:NSUTF8StringEncoding error:nil];
		}

		LOGF_COLOR(Log::Color::BrightGreen,
		           "[MSL] Compiled PS5 shader 0x%016llx to Metal function '%s' (saved to %s)\n",
		           static_cast<unsigned long long>(shader_hash),
		           [[function name] UTF8String],
		           [msl_path UTF8String]);
		return result;
	}
#else
	result.error_log = "Compiled without Objective-C support";
	return result;
#endif
}

MetalShaderCompileResult MetalShaderManager::CompileMslToFunction(
    const std::string& msl_source_in,
    const char* entry_point,
    uint32_t stage,
    uint64_t shader_hash) {
	(void)stage;
	MetalShaderCompileResult result {};
	if (msl_source_in.empty()) {
		result.error_log = "Empty MSL source";
		return result;
	}
	std::string msl_source = msl_source_in;

#ifdef __OBJC__
	auto cached = GetFunction(shader_hash);
	if (cached != nil) {
		result.function = cached;
		result.success  = true;
		return result;
	}
#endif

	result.msl_source = msl_source;

#ifdef __OBJC__
	@autoreleasepool {
		id<MTLDevice> device = MetalContext::Instance().GetDevice();
		if (!device) {
			MetalContext::Instance().Initialize();
			device = MetalContext::Instance().GetDevice();
			if (!device) {
				result.error_log = "Metal device not initialized";
				return result;
			}
		}

		MTLCompileOptions* compile_options = [[MTLCompileOptions alloc] init];
#if defined(MTLLanguageVersion4_0)
		compile_options.languageVersion = MTLLanguageVersion4_0;
#elif defined(MTLLanguageVersion3_2)
		compile_options.languageVersion = MTLLanguageVersion3_2;
#else
		compile_options.languageVersion = MTLLanguageVersion3_0;
#endif
		ApplyMslMathMode(compile_options);

		NSError*  error      = nil;

		NSString* source_str = [NSString stringWithUTF8String:result.msl_source.c_str()];
		// KYTY_MSL_DUMP_DIR=<dir>: write the generated MSL of every directly compiled program as <dir>/<hash>.metal.
		if (const char* dump_dir = KYTY_ENV("KYTY_MSL_DUMP_DIR")) {
			NSString* directory = [NSString stringWithUTF8String:dump_dir];
			NSError* dump_error = nil;
			if (directory == nil || directory.length == 0) {
				std::fprintf(stderr, "[MslDump] invalid or empty KYTY_MSL_DUMP_DIR\n");
			} else if (![[NSFileManager defaultManager] createDirectoryAtPath:directory
			                                      withIntermediateDirectories:YES attributes:nil error:&dump_error]) {
				std::fprintf(stderr, "[MslDump] cannot create %s: %s\n", dump_dir,
				             dump_error.localizedDescription.UTF8String);
			} else {
				NSString* filename = [NSString stringWithFormat:@"0x%016llx.metal", static_cast<unsigned long long>(shader_hash)];
				NSString* path = [directory stringByAppendingPathComponent:filename];
				if (![source_str writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:&dump_error]) {
					std::fprintf(stderr, "[MslDump] cannot write %s: %s\n", path.UTF8String,
					             dump_error.localizedDescription.UTF8String);
				}
			}
		}
		id<MTLLibrary> library =
		    [device newLibraryWithSource:source_str options:compile_options error:&error];
		if (!library) {
			result.error_log = error ? [[error localizedDescription] UTF8String]
			                         : "MSL library compilation failed";
			std::fprintf(stderr, "[MSL] direct compile FAILED 0x%016llx: %s\n",
			             static_cast<unsigned long long>(shader_hash), result.error_log.c_str());
			LOGF_COLOR(Log::Color::BrightRed,
			           "MetalShader: direct MSL compilation failed for hash 0x%016llx: %s\n",
			           static_cast<unsigned long long>(shader_hash), result.error_log.c_str());
			return result;
		}

		const char* entry_name_c = entry_point ? entry_point : "main0";
		id<MTLFunction> function =
		    [library newFunctionWithName:[NSString stringWithUTF8String:entry_name_c]];
		if (!function) {
			NSArray<NSString*>* names = [library functionNames];
			if ([names count] > 0) {
				function = [library newFunctionWithName:[names firstObject]];
			}
		}
		if (!function) {
			result.error_log = "Entry point function not found in MSLLibrary";
			return result;
		}

		result.library  = library;
		result.function = function;
		result.success  = true;
		m_function_cache.emplace(shader_hash, function);
		static std::atomic<uint32_t> s_msl_compiled {0};
		if (s_msl_compiled.fetch_add(1) < 200) {
			std::fprintf(stderr, "[MSL] direct compiled 0x%016llx entry=%s\n",
			             static_cast<unsigned long long>(shader_hash), entry_name_c);
		}
		return result;
	}
#else
	result.error_log = "Compiled without Objective-C support";
	return result;
#endif
}

} // namespace Libs::Graphics::Metal
