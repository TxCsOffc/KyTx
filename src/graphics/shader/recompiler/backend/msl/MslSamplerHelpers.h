#pragma once

#include <ostream>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::Msl {

// Use the image's real texels and the S# address modes. Filtering the 0/1
// comparisons, including between mip levels, is different from filtering depth.
inline void EmitComparisonSamplerHelpers(std::ostream& out) {
	out << R"(
inline float kyty_compare_depth(float d, float ref, uint func) {
    return float(func == 0u ? false : func == 1u ? ref < d : func == 2u ? ref == d : func == 3u ? ref <= d :
                 func == 4u ? ref > d : func == 5u ? ref != d : func == 6u ? ref >= d : true);
}
inline float4 kyty_gather_cmp(float4 g, float ref, uint func) {
    return float4(kyty_compare_depth(g.x, ref, func), kyty_compare_depth(g.y, ref, func),
                  kyty_compare_depth(g.z, ref, func), kyty_compare_depth(g.w, ref, func));
}
inline float kyty_sampler_bias(uint4 state) {
    return float(int((state.z & 0x3fffu) ^ 0x2000u) - 0x2000) / 256.0f;
}
// SPIRV-Cross uses this handle cast when a sampled image is consumed by Dref.
// Keep the physical resource and its swizzle; select Metal's depth operation.
template<typename T> inline depth2d<T> kyty_depth_cast(texture2d<T> t) {
    return reinterpret_cast<thread const depth2d<T>&>(t);
}
template<typename T> inline depth2d_array<T> kyty_depth_cast(texture2d_array<T> t) {
    return reinterpret_cast<thread const depth2d_array<T>&>(t);
}
)";
	for (unsigned kind = 0; kind < 5; ++kind) {
		const unsigned dimensions = kind < 2 ? 1 : kind < 4 ? 2 : 3;
		const bool array = kind == 1 || kind == 3;
		const std::string vector = dimensions == 1 ? "float" : "float" + std::to_string(dimensions);
		const std::string texture = "texture" + std::to_string(dimensions) + "d" + (array ? "_array" : "") + "<float>";
		const std::string slice_parameter = array ? ", uint slice" : "";
		const std::string slice_argument = array ? ", slice" : "";
		const std::string level_argument = dimensions == 1 ? "" : ", level(float(mip))";
		std::string extent = "float(t.get_width())";
		if (dimensions > 1) extent = vector + "(t.get_width(mip), t.get_height(mip)" +
		                                 (dimensions == 3 ? ", t.get_depth(mip))" : ")");
		out << "inline float kyty_pcf_level(" << texture << " t, sampler p, " << vector << " uv" << slice_parameter
		    << ", float ref, uint func, bool linear, bool normalized, " << vector << " offset, uint mip) {\n";
		out << "    const " << vector << " extent = " << extent << ";\n";
		out << "    uv += normalized ? offset / extent : offset;\n";
		out << "    if (!linear) return kyty_compare_depth(t.sample(p, uv" << slice_argument << level_argument << ").x, ref, func);\n";
		out << "    const " << vector << " pos = (normalized ? uv * extent : uv) - 0.5f;\n";
		out << "    const " << vector << " base = floor(pos), f = pos - base;\n    float sum = 0.0f;\n";
		for (unsigned tap = 0; tap < (1u << dimensions); ++tap) {
			std::string offset = dimensions == 1 ? std::to_string(tap) + ".0f" : vector + "(";
			std::string weight;
			for (unsigned axis = 0; axis < dimensions; ++axis) {
				const bool upper = (tap & (1u << axis)) != 0;
				if (dimensions > 1) offset += (axis ? ", " : "") + std::to_string(upper ? 1 : 0) + ".0f";
				const auto fraction = dimensions == 1 ? "f" : "f." + std::string(1, "xyz"[axis]);
				weight += (axis ? " * " : "") + (upper ? fraction : "(1.0f - " + fraction + ")");
			}
			if (dimensions > 1) offset += ")";
			out << "    { const " << vector << " center = base + " << offset << " + 0.5f;\n"
			    << "      sum += kyty_compare_depth(t.sample(p, normalized ? center / extent : center" << slice_argument
			    << level_argument << ").x, ref, func) * " << weight << "; }\n";
		}
		out << "    return sum;\n}\n";
		out << "inline float4 kyty_sample_depth_cmp(" << texture << " t, sampler p, " << vector << " uv" << slice_parameter
		    << ", float ref, uint4 state, float lod, " << vector << " offset) {\n"
		       "    const uint mip_filter = (state.z >> 26u) & 3u;\n"
		       "    const bool normalized = (state.x & 0x8000u) == 0u;\n"
		       "    const uint func = (state.x >> 12u) & 7u;\n"
		       "    const float lo = float(state.y & 0xfffu) / 256.0f, hi = float((state.y >> 12u) & 0xfffu) / 256.0f;\n"
		       "    const float lambda = mip_filter == 0u || !normalized ? 0.0f : clamp(lod + kyty_sampler_bias(state), lo, hi);\n"
		       "    const bool linear = ((state.z >> (lambda <= 0.0f ? 20u : 22u)) & 1u) != 0u;\n";
		if (dimensions == 1) out << "    const float selected = 0.0f;\n";
		else out << "    const float selected = clamp(lambda, 0.0f, float(t.get_num_mip_levels() - 1u));\n";
		// Vulkan's preferred nearest-mip tie rule selects the lower mip.
		out << "    const uint first = uint(mip_filter == 2u ? floor(selected) : ceil(selected + 0.5f) - 1.0f);\n"
		    << "    const float a = kyty_pcf_level(t, p, uv" << slice_argument << ", ref, func, linear, normalized, offset, first);\n";
		if (dimensions != 1) {
			out << "    if (mip_filter == 2u && selected > float(first)) {\n"
			    << "        const float b = kyty_pcf_level(t, p, uv" << slice_argument
			    << ", ref, func, linear, normalized, offset, first + 1u);\n"
			       "        return float4(mix(a, b, selected - float(first)));\n    }\n";
		}
		out << "    return float4(a);\n}\n";
		const std::string generic_texture = texture.substr(0,texture.find('<')) + "<T>";
		out << "template<typename T> inline vec<T,4> kyty_colour_offset_level(" << generic_texture << " t, sampler p, " << vector
		    << " uv" << slice_parameter << ", bool linear, bool normalized, " << vector << " offset, uint mip, uint reduction) {\n"
		    << "    const " << vector << " extent = " << extent << ";\n"
		    << "    uv += normalized ? offset / extent : offset;\n"
		    << "    if (!linear || !is_floating_point<T>::value) return t.sample(p, uv" << slice_argument << level_argument << ");\n"
		    << "    const " << vector << " pos = (normalized ? uv * extent : uv) - 0.5f;\n"
		    << "    const " << vector << " base = floor(pos), f = pos - base;\n"
		       "    float4 sum(0.0f), lo(INFINITY), hi(-INFINITY);\n";
		for (unsigned tap=0;tap<(1u<<dimensions);++tap) {
			std::string delta=dimensions==1?std::to_string(tap)+".0f":vector+"(",weight;
			for (unsigned axis=0;axis<dimensions;++axis) {
				const bool upper=(tap&(1u<<axis))!=0;
				if (dimensions>1) delta+=(axis?", ":"")+std::to_string(upper?1:0)+".0f";
				const auto fraction=dimensions==1?"f":"f."+std::string(1,"xyz"[axis]);
				weight+=(axis?" * ":"")+(upper?fraction:"(1.0f - "+fraction+")");
			}
			if (dimensions>1) delta+=")";
			out << "    { const " << vector << " center = base + " << delta << " + 0.5f;\n"
			    << "      const float weight = " << weight << ";\n"
			    << "      if (weight > 0.0f) { const float4 v = float4(t.sample(p, normalized ? center / extent : center" << slice_argument
			    << level_argument << ")); sum += v * weight; lo = min(lo,v); hi = max(hi,v); } }\n";
		}
		out << "    return vec<T,4>(reduction == 1u ? lo : reduction == 2u ? hi : sum);\n}\n"
		    << "template<typename T> inline vec<T,4> kyty_sample_colour_offset(" << generic_texture << " t, sampler p, " << vector
		    << " uv" << slice_parameter << ", uint4 state, float lod, " << vector << " offset) {\n"
		       "    const uint mip_filter = (state.z >> 26u) & 3u, reduction = (state.x >> 29u) & 3u;\n"
		       "    const bool normalized = (state.x & 0x8000u) == 0u;\n"
		       "    const float lo = float(state.y & 0xfffu) / 256.0f, hi = float((state.y >> 12u) & 0xfffu) / 256.0f;\n"
		       "    const float lambda = mip_filter == 0u || !normalized ? 0.0f : clamp(lod + kyty_sampler_bias(state),lo,hi);\n"
		       "    const bool linear = ((state.z >> (lambda <= 0.0f ? 20u : 22u)) & 1u) != 0u;\n";
		if (dimensions==1) out << "    const float selected = 0.0f;\n";
		else out << "    const float selected = clamp(lambda,0.0f,float(t.get_num_mip_levels()-1u));\n";
		out << "    const uint first = uint(mip_filter == 2u ? floor(selected) : ceil(selected + 0.5f) - 1.0f);\n"
		    << "    const auto a = kyty_colour_offset_level(t,p,uv" << slice_argument << ",linear,normalized,offset,first,reduction);\n";
		if (dimensions!=1) out << "    if (mip_filter == 2u && selected > float(first)) {\n"
		    << "        const auto b = kyty_colour_offset_level(t,p,uv" << slice_argument << ",linear,normalized,offset,first+1u,reduction);\n"
		       "        return reduction == 1u ? min(a,b) : reduction == 2u ? max(a,b) : vec<T,4>(mix(float4(a),float4(b),selected-float(first)));\n    }\n";
		out << "    return a;\n}\n";
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Msl
