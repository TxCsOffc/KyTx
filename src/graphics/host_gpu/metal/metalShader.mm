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

// KYTY_VS_PROBE=pos: override the vertex position with a fixed full-screen triangle
// generated from vertex_id; original varyings are left untouched.
static void ApplyVsProbe(std::string& msl) {
	const char* mode_env = KYTY_ENV("KYTY_VS_PROBE");
	if (mode_env == nullptr || std::string(mode_env) != "pos") return;
	const size_t entry = msl.find("vertex main0_out");
	const size_t pos_tag = msl.find("[[position]]");
	const size_t vid_tag = msl.find("[[vertex_id]]", entry);
	if (entry == std::string::npos || pos_tag == std::string::npos || vid_tag == std::string::npos) return;
	const std::string pos = PsIdentBefore(msl, pos_tag);
	const std::string vid = PsIdentBefore(msl, vid_tag);
	const std::string inject =
	    "{ uint _vid = uint(" + vid + "); float2 _p = float2(float((_vid << 1u) & 2u), float(_vid & 2u)); "
	    "out." + pos + " = float4(_p * 2.0 - 1.0, 0.0, 1.0); } ";
	const std::string ret = "return out;";
	size_t at = entry;
	int injected = 0;
	while ((at = msl.find(ret, at)) != std::string::npos) {
		msl.insert(at, inject);
		at += inject.size() + ret.size();
		++injected;
	}
	std::fprintf(stderr, "[VsProbe] pos=%s vid=%s returns=%d\n", pos.c_str(), vid.c_str(), injected);
}

// KYTY_CS_PROBE=1 + KYTY_PROBE_HASH=<kernel hash>: write a constant into the kernel's first 3D
// storage image at the start of main0 (unconditionally). If the image then stays zero, the write
// does not land; if it fills, the kernel itself exits early on its inputs.
static void ApplyCsProbe(std::string& msl, uint64_t hash) {
	if (KYTY_ENV("KYTY_CS_PROBE") == nullptr) return;
	if (const char* only = KYTY_ENV("KYTY_PROBE_HASH")) {
		if (std::strtoull(only, nullptr, 0) != hash) return;
	}
	const size_t kernel = msl.find("kernel void main0(");
	if (kernel == std::string::npos) return;
	const size_t wr = msl.rfind("access::write>", kernel);
	if (wr == std::string::npos) return;
	const size_t gt = msl.find("> ", wr + std::strlen("access::write>"));
	if (gt == std::string::npos) return;
	const size_t name_begin = gt + 2;
	const size_t name_end = msl.find(" [[id(", name_begin);
	if (name_end == std::string::npos) return;
	std::string name = msl.substr(name_begin, name_end - name_begin);
	const bool is_array = msl.rfind("array<", wr) != std::string::npos && msl.rfind("array<", wr) > msl.rfind("struct", wr);
	const size_t brace = msl.find('{', kernel);
	if (brace == std::string::npos) return;
	// Debug counter: every executing thread increments kyty_dbg[0] (bound at buffer 26).
	msl.insert(kernel + std::strlen("kernel void main0("), "device atomic_uint* kyty_dbg [[buffer(26)]], ");
	const size_t brace2 = msl.find('{', kernel);
	const std::string tex_expr = "spvDescriptorSet0." + name + (is_array ? "[0u]" : "");
	const std::string dims = "\n\tatomic_store_explicit(kyty_dbg + 1, uint(" + tex_expr + ".get_width()), memory_order_relaxed);"
	                         "\n\tatomic_store_explicit(kyty_dbg + 2, uint(" + tex_expr + ".get_height()), memory_order_relaxed);"
	                         "\n\tatomic_store_explicit(kyty_dbg + 3, uint(" + tex_expr + ".get_depth()), memory_order_relaxed);";
	const std::string inject = "\n\tatomic_fetch_add_explicit(kyty_dbg, 1u, memory_order_relaxed);" + dims + "\n\tspvDescriptorSet0." + name + (is_array ? "[0u]" : "") +
	                           ".write(float4(1.0, 0.5, 0.25, 1.0), uint3(0, 0, 0)); // kyty CS probe\n";
	msl.insert(brace2 + 1, inject);
	std::fprintf(stderr, "[CsProbe] hash=0x%016llx writes %s\n", (unsigned long long)hash, name.c_str());
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

// KYTY_PROBE_HASH is one hash or a comma-separated list (pixel shader probes only).
static bool ProbeHashListed(const char* list, uint64_t hash) {
	for (const char* p = list; *p != '\0';) {
		char*          end = nullptr;
		const uint64_t value = std::strtoull(p, &end, 0);
		if (end == p) return false;
		if (value == hash) return true;
		p = *end == ',' ? end + 1 : end;
	}
	return false;
}

// KYTY_PS_PROBE=nan on a variant that was not emitted with KYTY_MSL_NAN_TRACE (the emitter only knows the guest shader hash): add the
// tracking to the generated text. Every single-line assignment to a float variable of main0 is followed by a check that remembers the
// number of the first variable that became NaN (the variable "v123" has id 123, look it up in the KYTY_MSL_DUMP_DIR dump).
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

static void ApplyPsProbe(std::string& msl, uint64_t hash) {
	const char* mode_env = KYTY_ENV("KYTY_PS_PROBE");
	if (mode_env == nullptr || mode_env[0] == '\0') return;
	if (const char* only = KYTY_ENV("KYTY_PROBE_HASH")) {
		if (!ProbeHashListed(only, hash)) return;
	}
	const std::string mode = mode_env;
	const size_t frag = msl.find("fragment ");
	const size_t color_tag = msl.find("[[color(0)]]");
	if (frag == std::string::npos || color_tag == std::string::npos) return;
	const std::string color = PsIdentBefore(msl, color_tag);

	std::string inject;
	if (mode == "dims") {
		// Shader-side dimensions of the first sampled 2D texture: w/2048 in red, h/2048 in green.
		const size_t t = msl.find("texture2d<float>");
		if (t == std::string::npos) return;
		const size_t line_start = msl.rfind('\n', t) + 1;
		const size_t id = msl.find("[[id(", line_start);
		if (id == std::string::npos) return;
		const std::string tex = "spvDescriptorSet0." + PsIdentBefore(msl, id) + "[0u]";
		inject = "out." + color + " = float4(float(" + tex + ".get_width()) / 2048.0, float(" + tex +
		         ".get_height()) / 2048.0, 0.0, 1.0); ";
	} else if (mode == "red") {
		inject = "out." + color + " = float4(1.0, 0.0, 0.0, 1.0); ";
	} else if (mode == "alpha1") {
		inject = "out." + color + ".w = 1.0; ";
	} else if (mode == "uv") {
		// First user-varying member of main0_in, read as .xy.
		std::string var;
		const size_t in_pos = msl.find("struct main0_in");
		if (in_pos != std::string::npos) {
			const size_t user = msl.find("[[user(", in_pos);
			if (user != std::string::npos) var = PsIdentBefore(msl, msl.rfind(' ', user - 1) == std::string::npos ? user : msl.find_last_not_of(' ', user - 1) + 1);
		}
		if (var.empty()) return;
		inject = "out." + color + " = float4(fract(in." + var + ".x * 4.0), fract(in." + var + ".y * 4.0), 0.0, 1.0); ";
	} else if (mode == "tex") {
		// Textures and samplers live in the descriptor argument buffer (spvDescriptorSet0).
		auto member_expr = [&](const char* type_key) -> std::string {
			const size_t t = msl.find(type_key);
			if (t == std::string::npos) return "";
			const size_t line_start = msl.rfind('\n', t) + 1;
			const size_t line_end = msl.find('\n', t);
			const std::string line = msl.substr(line_start, line_end - line_start);
			const size_t id = line.find("[[id(");
			if (id == std::string::npos) return "";
			const std::string name = PsIdentBefore(line, id);
			return "spvDescriptorSet0." + name + (line.find("array<") != std::string::npos ? "[0]" : "");
		};
		const std::string tex = member_expr("texture2d<float>");
		const std::string samp = member_expr("sampler");
		if (tex.empty() || samp.empty()) return;
		inject = "{ float3 _m = float3(0.0); for (int _i = 0; _i < 4; ++_i) { float2 _uv = float2(float(_i & 1) * 0.5 + 0.25, float(_i >> 1) * 0.5 + 0.25); _m = max(_m, " + tex + ".sample(" + samp + ", _uv).rgb); } out." + color + " = float4(_m, 1.0); } ";
	} else if (mode == "vars") {
		// KYTY_PROBE_VARS="<rt0>;<rt1>", each side one variable (vN) or three scalars (vA,vB,vC): the render targets show those values
		// (magnitude), so one launch reads up to six internal values of a pixel shader; read the result with KYTY_PASS_TRACE_CHANNELS.
		const char* spec_env = KYTY_ENV("KYTY_PROBE_VARS");
		if (spec_env == nullptr) return;
		const auto expr_for = [&](std::string name, bool* wide) -> std::string {
			// "vN:i" shows an integer variable as a number (float(vN)) instead of reinterpreting its bits.
			bool as_int = false;
			if (name.size() > 2 && name.compare(name.size() - 2, 2, ":i") == 0) { as_int = true; name.resize(name.size() - 2); }
			const size_t decl = msl.find(" " + name + " = (");
			if (decl == std::string::npos) return "0.0";
			const size_t ls = msl.rfind('\n', decl) + 1;
			const std::string type = msl.substr(ls, decl - ls);
			std::string t = type;
			t.erase(std::remove(t.begin(), t.end(), '\t'), t.end());
			if (t == "float") return "abs(" + name + ")";
			if (t == "uint") return as_int ? "float(" + name + ")" : "abs(as_type<float>(" + name + "))";
			if (t == "bool") return "(" + name + " ? 1.0 : 0.0)";
			if (t == "uint4") { if (wide) *wide = true; return "abs(as_type<float4>(" + name + ").xyz)"; }
			if (t == "half") return "abs(float(" + name + "))";
			return "0.0";
		};
		std::vector<std::string> sides;
		for (const char* p = spec_env; ; ++p) {
			const char* e = std::strchr(p, ';');
			sides.emplace_back(p, e ? static_cast<size_t>(e - p) : std::strlen(p));
			if (!e) break;
			p = e;
		}
		std::string body;
		for (size_t side = 0; side < sides.size() && side < 2; ++side) {
			std::vector<std::string> names;
			for (size_t at = 0; at <= sides[side].size();) {
				const size_t comma = sides[side].find(',', at);
				names.push_back(sides[side].substr(at, comma == std::string::npos ? std::string::npos : comma - at));
				if (comma == std::string::npos) break;
				at = comma + 1;
			}
			std::string value;
			bool wide = false;
			if (names.size() == 1) {
				const std::string e = expr_for(names[0], &wide);
				value = wide ? "float4(" + e + ", 1.0)" : "float4(" + e + ", " + e + ", " + e + ", 1.0)";
			} else if (names.size() >= 3) {
				value = "float4(" + expr_for(names[0], nullptr) + ", " + expr_for(names[1], nullptr) + ", " + expr_for(names[2], nullptr) + ", 1.0)";
			} else {
				continue;
			}
			body += "out.color" + std::to_string(side) + " = " + value + "; ";
		}
		inject = body;
	} else if (mode == "nan") {
		// With KYTY_MSL_NAN_TRACE=<hash>: the output is the id of the first float instruction whose result was NaN, in three 6-bit parts (id & 63, id >> 6 & 63, id >> 12).
		if (msl.find("kyty_first_nan") == std::string::npos) InstrumentFirstNan(msl, frag);
		if (msl.find("kyty_first_nan") == std::string::npos) return;
		// Every colour output gets the id: the draw may mask colour 0 out and only write another target.
		const std::string id_value = "float4(float(kyty_first_nan & 63u), float((kyty_first_nan >> 6) & 63u), float(kyty_first_nan >> 12), 1.0)";
		for (size_t tag = msl.find("[[color("); tag != std::string::npos; tag = msl.find("[[color(", tag + 1)) {
			inject += "out." + PsIdentBefore(msl, tag) + " = " + id_value + "; ";
		}
	} else if (mode == "cb") {
		// First [[buffer(N)]] argument of the fragment entry, read as four dwords.
		const size_t buf = msl.find("[[buffer(", frag);
		if (buf == std::string::npos) return;
		const std::string name = PsIdentBefore(msl, buf);
		const bool is_ptr = msl.rfind('*', buf) != std::string::npos && msl.rfind('*', buf) > msl.rfind(',', buf);
		const std::string addr = is_ptr ? "(constant uint*)(" + name + ")" : "(constant uint*)(&" + name + ")";
		inject = "{ constant uint* _p = " + addr + "; out." + color +
		         " = float4(_p[0] != 0 ? 1.0 : 0.0, _p[1] != 0 ? 1.0 : 0.0, _p[2] != 0 ? 1.0 : 0.0, 1.0); } ";
	} else {
		return;
	}

	const std::string ret = "return out;";
	size_t pos = frag;
	int injected = 0;
	while ((pos = msl.find(ret, pos)) != std::string::npos) {
		msl.insert(pos, inject);
		pos += inject.size() + ret.size();
		++injected;
	}
	static std::atomic<uint32_t> s_probe_log {0};
	if (s_probe_log.fetch_add(1) < 20) {
		std::fprintf(stderr, "[PsProbe] mode=%s hash=0x%016llx color=%s returns=%d\n", mode.c_str(), (unsigned long long)hash, color.c_str(), injected);
	}
}

// GCN shaders rely on IEEE semantics (inf/NaN propagation, 0*inf, x/0); Metal's default fast-math lets the compiler assume
// they never occur. KYTY_MSL_MATH=fast|relaxed overrides the default (safe).
static void ApplyMslMathMode(MTLCompileOptions* opts) {
	const char* mode = KYTY_ENV("KYTY_MSL_MATH");
	if (@available(macOS 15.0, *)) {
		if (mode && std::string_view(mode) == "fast") {
			opts.mathMode = MTLMathModeFast;
		} else if (mode && std::string_view(mode) == "relaxed") {
			opts.mathMode = MTLMathModeRelaxed;
		} else {
			opts.mathMode = MTLMathModeSafe;
			opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
		}
	} else {
		opts.fastMathEnabled = (mode && std::string_view(mode) == "fast") ? YES : NO;
	}
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
static constexpr const char* kMslCacheSchema = "msl-s2";

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
			ApplyPsProbe(sanitized, hash);
			if (sanitized.find("[[buffer(0)]]") != std::string::npos) {
				RecordArgBuffer0(hash);
			}
			ApplyCsProbe(sanitized, hash);
			ApplyVsProbe(sanitized);
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


// Metal rejects a pipeline when the pixel shader reads a user(locnN) attribute the
// vertex shader does not write. The values are undefined on real hardware too, so
// declare the missing outputs on the vertex side to satisfy the interface check.
static void AddMissingVertexVaryings(std::string& msl) {
	const std::string struct_name = "struct main0_out";
	const auto        struct_pos  = msl.find(struct_name);
	if (struct_pos == std::string::npos) {
		return;
	}
	const auto body_start = msl.find('{', struct_pos);
	const auto body_end   = msl.find("};", body_start);
	if (body_start == std::string::npos || body_end == std::string::npos) {
		return;
	}
	std::string additions;
	for (uint32_t location = 0; location < 16; ++location) {
		const std::string marker = "user(locn" + std::to_string(location) + ")";
		if (msl.find(marker, body_start) != std::string::npos &&
		    msl.find(marker, body_start) < body_end) {
			continue;
		}
		additions += "    float4 kyty_missing" + std::to_string(location) + " [[" + marker +
		             "]];\n";
	}
	if (!additions.empty()) {
		msl.insert(body_end, additions);
	}
}

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
		ApplyPsProbe(result.msl_source, shader_hash);
		ApplyCsProbe(result.msl_source, shader_hash);
		ApplyVsProbe(result.msl_source);
		ApplyVsDbg(result.msl_source, shader_hash);
		if (stage == static_cast<uint32_t>(Libs::Graphics::ShaderType::Vertex)) {
			AddMissingVertexVaryings(result.msl_source);
		}
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
	// KYTY_MSL_PS_RED=<hash>: diagnostic. Every fragment return of that shader writes solid red to
	// color0, so the RT readback shows whether fragments are produced at all.
	std::string msl_source = msl_source_in;
	// KYTY_MSL_CS_RED=<hash>: diagnostic. Every storage-image write of that kernel stores solid red,
	// so the presented frame shows whether the kernel's write reaches the flip texture.
	if (const char* cs_hash = KYTY_ENV("KYTY_MSL_CS_RED");
	    cs_hash != nullptr && std::strtoull(cs_hash, nullptr, 0) == shader_hash &&
	    msl_source.find("kernel void main0(") != std::string::npos) {
		const size_t writes = [&] {
			size_t n = 0;
			std::smatch m;
			std::string image_name = "image_5";
			if (std::regex_search(msl_source, m, std::regex(R"((\w+)\.write\(as_type<float4>)"))) {
				image_name = m[1].str();
			}
			msl_source = std::regex_replace(msl_source, std::regex(R"(\.write\(as_type<float4>\(\w+\))"),
			                                ".write(float4(1.0, 0.0, 0.0, 1.0)",
			                                std::regex_constants::format_first_only);
			// Entry marker: green at (0,0) written unconditionally at kernel entry (runs or not).
			const size_t entry = msl_source.find("kernel void main0(");
			const size_t brace = entry != std::string::npos ? msl_source.find('{', entry) : std::string::npos;
			if (brace != std::string::npos) {
				const bool marker_3d = msl_source.find("texture3d<") != std::string::npos;
				msl_source.insert(brace + 1, "\n\tab." + image_name + ".write(float4(0.0, 1.0, 0.0, 1.0), " +
				                             std::string(marker_3d ? "uint3(gid.x, gid.y, 0u)" : "uint2(gid.x, gid.y)") + ", 0u);\n");
			}
			return n;
		}();
		(void)writes;
		std::fprintf(stderr, "[MslCsRed] hash=0x%016llx\n", (unsigned long long)shader_hash);
	}
	if (const char* red_hash = KYTY_ENV("KYTY_MSL_PS_RED");
	    red_hash != nullptr && std::strtoull(red_hash, nullptr, 0) == shader_hash &&
	    msl_source.find("fragment ") != std::string::npos) {
		size_t at = msl_source.find("fragment ");
		while ((at = msl_source.find("return out;", at)) != std::string::npos) {
			const std::string inject = "out.color0 = float4(1.0, 0.0, 0.0, 1.0); ";
			msl_source.insert(at, inject);
			at += inject.size() + std::strlen("return out;");
		}
		std::fprintf(stderr, "[MslPsRed] hash=0x%016llx\n", (unsigned long long)shader_hash);
	}

#ifdef __OBJC__
	auto cached = GetFunction(shader_hash);
	if (cached != nil) {
		result.function = cached;
		result.success  = true;
		return result;
	}
#endif

	// A native vertex shader that exports nothing but the position (depth-only passes) can still be paired with a
	// pixel shader that reads user(locnN): declare the interpolants so Metal accepts the pipeline (values are
	// undefined on hardware as well).
	if (msl_source.find("vertex ") != std::string::npos && msl_source.find("user(locn") == std::string::npos) {
		const size_t so = msl_source.find("struct KytyStageOut {");
		const size_t end = so != std::string::npos ? msl_source.find("};", so) : std::string::npos;
		if (end != std::string::npos) {
			std::string add;
			for (uint32_t i = 0; i < 8; ++i) add += "\tfloat4 kyty_missing" + std::to_string(i) + " [[user(locn" + std::to_string(i) + ")]];\n";
			msl_source.insert(end, add);
		}
	}
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
		// Pixel shader probes (KYTY_PS_PROBE) rewrite the entry returns of natively emitted MSL as well.
		ApplyPsProbe(result.msl_source, shader_hash);
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
