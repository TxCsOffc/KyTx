// Direct RDNA (native IR) -> MSL emitter.
//
// This backend deliberately does not go through SPIR-V/SPIRV-Cross. The IR already
// distinguishes typed memory, image, export and resource operations, so the MSL is
// emitted from the same semantic layer that the SPIR-V backend uses.
//
// Layout of the argument buffer follows IR::NativeBinding exactly; the renderer fills
// one 64-bit slot per resource id (see BindMetalShaderResources).

#include "graphics/shader/recompiler/backend/msl/MslEmitter.h"

#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/shader.h"
#include <unordered_set>
#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static bool LdsSyncEnabled() {
	static const bool s_on = std::getenv("KYTY_LDS_SYNC") != nullptr;
	return s_on;
}

// KYTY_MSL_GDS=1: DS instructions with the GDS bit use the device GDS buffer (ab.gds, 64 KiB, bound for compute) instead of the
// workgroup LDS array. Without it they keep the old (wrong) LDS lowering and ds_append/ds_consume on GDS stay unsupported.
static bool MslGdsEnabled() {
	static const bool s_on = [] {
		const char* value = std::getenv("KYTY_MSL_GDS");
		return value != nullptr && value[0] == '1';
	}();
	return s_on;
}

// Dwords of the Metal GDS buffer (GdsBuffer() in metalRenderer.mm is 64 KiB).
static constexpr uint32_t kMslGdsDwords = 16384u;
static constexpr uint32_t kMslLoopTripGdsBase = 16376u; // KYTY_LOOP_TRIP_GDS counters, the last 8 GDS dwords

namespace Libs::Graphics::ShaderRecompiler::Msl {
namespace {

using IR::Inst;
using IR::Value;
using IR::ValueOpcode;

std::string U32Literal(uint32_t value) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "0x%08xu", value);
	return buffer;
}

// Runaway-loop guard (KYTY_LOOP_CAP=<n> overrides): structured code returns once one entry into a loop has run this many
// iterations. The old guard, one counter per invocation that grew on every block, cut legitimate loops short: the BRDF LUT pass
// (PS 0xf77586..., 4096 samples of a ~12-step radical inverse, ~57k steps in all) returned with an empty output and every lit
// surface read a zero LUT, and the forward light loops (one bit per light, ~17k lines per light) returned after a few lights. One
// counter per invocation cannot fit both: 65536 kept the LUT but let the light shaders run past the GPU timeout. The SPIR-V path
// has no guard by default.
uint32_t MslLoopCap() {
	static const uint32_t cap = [] {
		const char* val = std::getenv("KYTY_LOOP_CAP");
		return val != nullptr ? static_cast<uint32_t>(std::strtoul(val, nullptr, 0)) : 8192u;
	}();
	return cap;
}

// The switch(pc) dispatcher counts block transitions, not loop iterations: a few blocks per iteration.
uint32_t MslDispatcherCap() {
	const uint64_t cap = static_cast<uint64_t>(MslLoopCap()) * 16u;
	return static_cast<uint32_t>(std::min<uint64_t>(cap, 0xffffffffu));
}

std::string F32Literal(uint32_t bits) {
	char buffer[48];
	std::snprintf(buffer, sizeof(buffer), "as_type<float>(0x%08xu)", bits);
	return buffer;
}

const char* MslType(IR::Type type) {
	switch (type) {
		case IR::Type::U1: return "bool";
		case IR::Type::U8:
		case IR::Type::U16:
		case IR::Type::U32: return "uint";
		case IR::Type::F16: return "half";
		case IR::Type::F32: return "float";
		case IR::Type::U64: return "ulong";
		case IR::Type::F64: return "double";
		case IR::Type::U32x2: return "uint2";
		case IR::Type::U32x3: return "uint3";
		case IR::Type::U32x4: return "uint4";
		case IR::Type::F32x2: return "float2";
		default: return nullptr;
	}
}

bool IsResourceType(IR::Type type) {
	switch (type) {
		case IR::Type::SrtResource:
		case IR::Type::BufferResource:
		case IR::Type::AddressResource:
		case IR::Type::ImageResource:
		case IR::Type::SamplerResource:
		case IR::Type::ImageAddress: return true;
		default: return false;
	}
}

// Flat argument-buffer id space used by the direct MSL backend. Slot numbers are
// resource indexes (not descriptor positions), so the renderer can fill them
// directly from its per-resource arrays. See BindMetalShaderResources.
constexpr uint32_t kMslBufferBase  = 0u;    // buffers[64]
constexpr uint32_t kMslImageBase   = 64u;   // image_<resource>[64]
constexpr uint32_t kMslSamplerBase = 128u;  // samplers[32]
constexpr uint32_t kMslSrtBinding  = 160u;
constexpr uint32_t kMslGdsBinding  = 161u;
constexpr uint32_t kMslBdaBinding  = 162u;
constexpr uint32_t kMslFaultBinding = 163u;
constexpr uint32_t kMslShaderDataBinding = 164u;

struct IoField {
	std::string name;
	std::string type;
	std::string attribute;
};

struct Emitter {
	const IR::Program& program;
	ShaderStageInputInfo input_info;
	std::vector<std::string> unsupported;

	std::ostringstream body;
	std::unordered_map<const Inst*, std::string> names;
	uint32_t next_name = 0;
	uint32_t next_phi_edge = 0;
	uint32_t next_loop = 0; // structured loops emitted so far: each gets its own runaway guard counter

	// Wave64 pair lowering (see Wave64Pair in MslEmitter.h): every instruction is emitted once per emulated half of the wave, with
	// the results of half 1 named "<name>_h"; half_ is the half being emitted.
	bool     pair_ = false;
	bool     nan_trace_ = false; // KYTY_MSL_NAN_TRACE=<pixel shader hash>[,<hash>...]: remember the first float result that is NaN
	uint32_t half_ = 0;

	// Stage interface: one struct per direction, plus lookup tables used by
	// GetAttribute/SetAttribute/GetBuiltin.
	std::vector<IoField> out_fields;
	std::vector<IoField> in_fields;
	std::unordered_map<uint32_t, std::string> out_param_field;
	std::unordered_map<uint32_t, std::string> in_param_field;
	std::unordered_map<uint32_t, std::string> mrt_field;
	// 1 = uint4 attachment, 2 = int4 attachment, 0 = float4.
	std::unordered_map<uint32_t, uint32_t> mrt_integer;
	std::string position_field;
	uint32_t    clip_distance_count = 0;
	std::string depth_field;
	std::string frag_coord_field;
	std::string front_facing_field;
	std::string bary_field;
	std::string prim_id_field;
	uint32_t pixel_location_count = 0;

	explicit Emitter(const IR::Program& program_, ShaderStageInputInfo input_info_)
	    : program(program_), input_info(std::move(input_info_)) {}

	[[nodiscard]] bool HasOutInterface() const {
		return !out_fields.empty();
	}
	[[nodiscard]] bool HasInInterface() const {
		return !in_fields.empty();
	}

	static std::string LocationField(uint32_t location) {
		return "param" + std::to_string(location);
	}

	void BuildInterface() {
		const bool vertex = program.stage == ShaderType::Vertex;
		const bool pixel  = program.stage == ShaderType::Pixel;

		if (vertex) {
			for (const auto& output: program.info.outputs) {
				switch (output.kind) {
					case IR::StageOutputKind::Position:
						if (output.index == 0 && position_field.empty()) {
							position_field = "position";
							out_fields.push_back({position_field, "float4", "[[position]]"});
						}
						break;
					case IR::StageOutputKind::Parameter:
						out_param_field[output.index] = LocationField(output.index);
						out_fields.push_back(
						    {LocationField(output.index), "float4",
						     "[[user(locn" + std::to_string(output.index) + ")]]"});
						break;
					case IR::StageOutputKind::ClipDistance:
						// User clip planes from the auxiliary position exports (PA_CL_VS_OUT_CNTL.CLIP_DIST_ENA), as gl_ClipDistance in
						// the SPIR-V backend. KYTY_MSL_NO_CLIP_DIST=1 drops them again.
						if (std::getenv("KYTY_MSL_NO_CLIP_DIST") == nullptr) clip_distance_count = std::max(clip_distance_count, output.index + 1u);
						break;
					case IR::StageOutputKind::PointSize:
					case IR::StageOutputKind::CullDistance:
					case IR::StageOutputKind::Layer:
					case IR::StageOutputKind::ViewportIndex:
						// Accepted and dropped: no cull planes, layered rendering or point size yet.
						// Writes to these outputs are ignored (see EmitSetAttribute).
						break;
					default:
						unsupported.push_back("vertex output kind " +
						                      std::to_string(static_cast<uint32_t>(output.kind)));
						break;
				}
			}
			if (clip_distance_count != 0u) {
				out_fields.push_back({"clip_dist", "float", "[[clip_distance]] [" + std::to_string(clip_distance_count) + "]"});
				static std::atomic<uint32_t> s_clip_shaders {0};
				std::fprintf(stderr, "[MSL-clip] vertex shader #%u writes %u clip distance(s) (PA_CL_VS_OUT_CNTL=0x%08x)\n", s_clip_shaders.fetch_add(1) + 1u,
				             clip_distance_count, input_info.vertex != nullptr ? input_info.vertex->pa_cl_vs_out_cntl : 0u);
			}
			return;
		}

		if (pixel) {
			// PS inputs go through the guest's SPI_PS_INPUT_CNTL OFFSET (same mapping the SPIR-V
			// path uses): attr N reads the VS export at ShaderPixelParameterLocation(N).
			std::vector<uint32_t> active_params;
			for (const auto& input: program.info.inputs) {
				if (input.kind == IR::StageInputKind::Parameter) {
					active_params.push_back(input.location);
				}
			}
			std::unordered_set<uint32_t> emitted_locations;
			for (const auto& input: program.info.inputs) {
				switch (input.kind) {
					case IR::StageInputKind::Parameter: {
						const uint32_t mapped = input_info.pixel != nullptr
						    ? Libs::Graphics::ShaderPixelParameterLocation(*input_info.pixel, active_params, input.location)
						    : input.location;
						in_param_field[input.location] = LocationField(mapped);
						if (emitted_locations.insert(mapped).second) {
							const bool flat = input.force_flat || (input_info.pixel != nullptr &&
							    ShaderPixelParameterIsFlat(*input_info.pixel, input.location));
							// Packed integer flags also travel through float parameters. Interpolating them
							// changes their bits (subnormal encodings can become zero), so P0/flat reads
							// must take the provoking vertex's value directly.
							const char* interpolation = flat ? ", flat" :
							    input_info.pixel != nullptr && input_info.pixel->ps_no_perspective
							        ? ", center_no_perspective" : "";
							in_fields.push_back(
							    {LocationField(mapped), "float4",
							     "[[user(locn" + std::to_string(mapped) + ")" + interpolation + "]]"});
						}
						break;
					}
					case IR::StageInputKind::FragCoord:
						frag_coord_field = "frag_coord";
						in_fields.push_back({frag_coord_field, "float4", "[[position]]"});
						break;
					case IR::StageInputKind::FrontFacing:
						front_facing_field = "front_facing";
						in_fields.push_back({front_facing_field, "bool", "[[front_facing]]"});
						break;
					case IR::StageInputKind::PrimitiveId:
						prim_id_field = "prim_id";
						in_fields.push_back({prim_id_field, "uint", "[[primitive_id]]"});
						break;
					case IR::StageInputKind::BaryCoordSmooth:
					case IR::StageInputKind::BaryCoordSmoothCentroid:
					case IR::StageInputKind::BaryCoordNoPerspective:
						if (bary_field.empty()) {
							bary_field = "bary";
							in_fields.push_back({bary_field, "float3", "[[barycentric_coord]]"});
						}
						break;
					case IR::StageInputKind::Layer:
					case IR::StageInputKind::SampleId: unsupported.push_back("pixel input builtin"); break;
					default: break;
				}
			}
			for (const auto& output: program.info.outputs) {
				switch (output.kind) {
					case IR::StageOutputKind::Mrt: {
						// Metal requires the fragment output type to match the attachment.
						uint32_t integer = 0;
						if (input_info.pixel != nullptr &&
						    output.index < std::size(input_info.pixel->target_integer)) {
							integer = input_info.pixel->target_integer[output.index];
						}
						mrt_integer[output.index] = integer;
						mrt_field[output.index] = "color" + std::to_string(output.index);
						const char* type = integer == 1 ? "uint4" : integer == 2 ? "int4" : "float4";
						const bool secondary = output.index == 1 && input_info.pixel != nullptr &&
						    (input_info.pixel->dual_source_blending || input_info.pixel->alpha_blend_source_remap);
						out_fields.push_back({mrt_field[output.index], type,
						                      secondary ? "[[color(0), index(1)]]"
						                                : "[[color(" + std::to_string(output.index) + ")]]"});
						break;
					}
					case IR::StageOutputKind::Depth:
						depth_field = "depth";
						out_fields.push_back({depth_field, "float", "[[depth(any)]]"});
						break;
					default: break;
				}
			}
			pixel_location_count = static_cast<uint32_t>(in_fields.size());
		}
	}

	bool IsSupported(IR::Type type) const {
		return IsResourceType(type) || MslType(type) != nullptr;
	}

	// Immediates in the IR are typed strictly; arguments such as builtin kinds and
	// attribute indexes are normally U32 but may be narrowed. Never abort the emulator
	// on an unexpected immediate type - report it instead.
	static std::optional<uint32_t> ImmU32(Value value) {
		if (!value.IsImmediate()) {
			return std::nullopt;
		}
		switch (value.GetType()) {
			case IR::Type::U1: return value.U1() ? 1u : 0u;
			case IR::Type::U8: return value.U8();
			case IR::Type::U16: return value.U16();
			case IR::Type::U32: return value.U32();
			case IR::Type::U64: return static_cast<uint32_t>(value.U64());
			default: return std::nullopt;
		}
	}

	std::string Name(const Inst& inst) { return HalfName(inst, half_); }

	std::string HalfName(const Inst& inst, uint32_t half) {
		std::string name;
		auto        it = names.find(&inst);
		if (it != names.end()) {
			name = it->second;
		} else {
			name = "v" + std::to_string(next_name++);
			names.emplace(&inst, name);
		}
		return pair_ && half == 1 ? name + "_h" : name;
	}

	// The expression of a value as seen by one emulated half of the wave.
	std::string ExprHalf(Value value, uint32_t half) {
		const auto saved = half_;
		half_            = half;
		auto text        = Expr(value);
		half_            = saved;
		return text;
	}

	// The guest lane id (0..63 when pair-packed, else the host simdgroup lane) of the half being emitted.
	[[nodiscard]] std::string LaneIdExpr() const { return pair_ && half_ == 1 ? "(lane + 32u)" : "lane"; }

	std::string Expr(Value value) {
		if (value.IsEmpty()) {
			return "0u";
		}
		if (value.TryInstruction() == nullptr) {
			const auto type = value.GetType();
			switch (type) {
				case IR::Type::U1: return value.U1() ? "true" : "false";
				case IR::Type::U8: return U32Literal(value.U8());
				case IR::Type::U16: return U32Literal(value.U16());
				case IR::Type::U32: return U32Literal(value.U32());
				case IR::Type::F16:
					return "as_type<half>(ushort(" + U32Literal(value.F16Bits() & 0xffffu) + "))";
				case IR::Type::F32:
					return F32Literal(std::bit_cast<uint32_t>(value.F32Value()));
				case IR::Type::U64: return std::to_string(value.U64()) + "ul";
				default: break;
			}
			if (!IsResourceType(type)) {
				unsupported.push_back("register or composite operand");
			}
			return "0u";
		}
		const auto* inst = value.TryInstruction();
		return Name(*inst);
	}

	void Unsupported(const Inst& inst, const char* detail) {
		if (unsupported.size() < 256) {
			unsupported.push_back(std::string(IR::ValueOpcodeName(inst.GetOpcode())) + " (" + detail + ")");
		}
	}

	static const char* ComponentName(uint32_t component) {
		switch (component & 3u) {
			case 0: return "x";
			case 1: return "y";
			case 2: return "z";
			default: return "w";
		}
	}

	// Resolves a guest buffer resource index to the argument-buffer slot the renderer
	// fills with the Metal GPU address of that guest range.
	std::optional<std::string> BufferPtr(uint32_t resource) const {
		const auto* binding = IR::FindBinding(program.bindings, IR::DescriptorBindingKind::Buffers);
		if (binding == nullptr ||
		    std::find(binding->resources.begin(), binding->resources.end(), resource) ==
		        binding->resources.end()) {
			return std::nullopt;
		}
		return "ab.buffers[" + std::to_string(resource) + "]";
	}

	// Descriptor formats whose raw dwords are stored/loaded unchanged (32-bit channels, any channel count).
	static bool IsRaw32BitFormat(Prospero::BufferFormat f) {
		using F = Prospero::BufferFormat;
		return f == F::k32UInt || f == F::k32SInt || f == F::k32Float || f == F::k32_32UInt || f == F::k32_32SInt ||
		       f == F::k32_32Float || f == F::k32_32_32UInt || f == F::k32_32_32SInt || f == F::k32_32_32Float ||
		       f == F::k32_32_32_32UInt || f == F::k32_32_32_32SInt || f == F::k32_32_32_32Float;
	}

	[[nodiscard]] bool BufferAddressable(uint32_t resource) const {
		if (resource >= program.info.buffers.size()) {
			return false;
		}
		const uint32_t packed = program.info.buffers[resource].packed_stride;
		const uint32_t stride = packed & 0x3fffu;
		// The RDNA2 stride swizzle (bit 14) is not lowered yet.
		return stride == 0 || (packed & (1u << 14u)) == 0;
	}

	const IR::MemoryInfo& Mem(const Inst& inst) const {
		return program.memory_info.at(inst.Flags<IR::MemoryFlags>().index);
	}

	// ---- Image support -------------------------------------------------
	static bool IsImageBinding(IR::DescriptorBindingKind kind) {
		const auto value = static_cast<uint32_t>(kind);
		return value >= IR::FirstImageBinding &&
		       value < static_cast<uint32_t>(IR::DescriptorBindingKind::Samplers);
	}

	struct ImageSlot {
		uint32_t                  id   = 0;
		IR::DescriptorBindingKind kind = IR::DescriptorBindingKind::Buffers;
	};

	// Assigns one unique argument-buffer id per image resource. The IR may contain
	// several descriptor entries for the same binding kind, so a per-kind cursor is
	// kept instead of using the position inside a single entry.
	std::unordered_map<uint32_t, ImageSlot> image_slots_;

	void BuildImageSlots() {
		image_slots_.clear();
		for (const auto& binding: program.bindings.descriptors) {
			if (!IsImageBinding(binding.kind)) {
				continue;
			}
			for (const uint32_t resource: binding.resources) {
				image_slots_.emplace(resource,
				                     ImageSlot {kMslImageBase + resource, binding.kind});
			}
		}
	}

	std::optional<ImageSlot> ImageSlotOf(uint32_t resource) const {
		const auto it = image_slots_.find(resource);
		if (it == image_slots_.end()) {
			return std::nullopt;
		}
		return it->second;
	}

	std::optional<uint32_t> SamplerSlotOf(uint32_t resource) const {
		const auto* binding = IR::FindBinding(program.bindings, IR::DescriptorBindingKind::Samplers);
		if (binding == nullptr ||
		    std::find(binding->resources.begin(), binding->resources.end(), resource) ==
		        binding->resources.end()) {
			return std::nullopt;
		}
		return resource;
	}

	bool ImageSupported(const IR::ImageResource& image) const {
		return !image.r128 && image.conversion_format == Prospero::BufferFormat::kInvalid &&
		       !image.atomic;
	}

	static const char* ImageScalar(const IR::ImageResource& image) {
		switch (image.numeric_class) {
			case Prospero::TextureNumericClass::Uint: return "uint";
			case Prospero::TextureNumericClass::Sint: return "int";
			default: return "float";
		}
	}

	static std::string TextureType(const IR::ImageResource& image, bool storage) {
		const char* scalar = ImageScalar(image);
		std::string base;
		switch (image.dimension) {
			case Decoder::ImageDimension::Dim1D: base = "texture1d"; break;
			case Decoder::ImageDimension::Dim1DArray: base = "texture1d_array"; break;
			// Cube images are sampled face by face (the RDNA address already carries face + in-face coordinates), so the host binds
			// them as 6-slice 2D arrays.
			case Decoder::ImageDimension::Dim2D: base = image.cube ? "texture2d_array" : "texture2d"; break;
			case Decoder::ImageDimension::Dim3D: base = "texture3d"; break;
			case Decoder::ImageDimension::Dim2DArray:
				base = "texture2d_array";
				break;
			case Decoder::ImageDimension::Dim2DMsaa: base = "texture2d_ms"; break;
			case Decoder::ImageDimension::Dim2DMsaaArray: base = "texture2d_ms_array"; break;
			default: base = "texture2d"; break;
		}
		return base + "<" + scalar + (storage ? ", access::read_write" : "") + ">";
	}

	struct DimInfo {
		uint32_t spatial    = 2;
		uint32_t components = 2;
		bool     msaa       = false;
	};

	static DimInfo DimensionInfo(Decoder::ImageDimension dimension) {
		switch (dimension) {
			case Decoder::ImageDimension::Dim1D: return {1, 1, false};
			case Decoder::ImageDimension::Dim1DArray: return {1, 2, false};
			case Decoder::ImageDimension::Dim2D: return {2, 2, false};
			case Decoder::ImageDimension::Dim3D: return {3, 3, false};
			case Decoder::ImageDimension::Dim2DArray: return {2, 3, false};
			case Decoder::ImageDimension::Dim2DMsaa: return {2, 2, true};
			case Decoder::ImageDimension::Dim2DMsaaArray: return {2, 3, true};
			default: return {2, 2, false};
		}
	}

	struct SampleLayout {
		int offset = -1;
		int bias   = -1;
		int dref   = -1;
		int grad_x = -1;
		int grad_y = -1;
		int coord  = 0;
		int lod    = -1;
	};

	static SampleLayout LayoutOf(const IR::MemoryInfo& mem) {
		const auto info = DimensionInfo(mem.image_dimension);
		SampleLayout layout;
		int cursor = 0;
		const auto has = [&](uint32_t flag) { return (mem.image_sample_flags & flag) != 0u; };
		if (has(Decoder::ImageSampleFlagOffset)) layout.offset = cursor++;
		if (has(Decoder::ImageSampleFlagBias)) layout.bias = cursor++;
		if (has(Decoder::ImageSampleFlagCompare)) layout.dref = cursor++;
		if (has(Decoder::ImageSampleFlagDerivative)) {
			layout.grad_x = cursor;
			cursor += static_cast<int>(info.spatial);
			layout.grad_y = cursor;
			cursor += static_cast<int>(info.spatial);
		}
		layout.coord = cursor;
		cursor += static_cast<int>(info.components);
		if (has(Decoder::ImageSampleFlagLod)) layout.lod = cursor++;
		return layout;
	}

	std::string AddressU32(const Inst& address, const IR::MemoryInfo& mem, uint32_t component) {
		const auto layout = Decoder::ImageAddressComponentLayout(mem.image_sample_flags, component);
		const uint32_t packed = layout.bit_offset / 32u;
		if (packed >= address.NumArgs()) {
			return "0u";
		}
		std::string value = Expr(address.Arg(packed));
		if (layout.bit_width == 16u) {
			if ((layout.bit_offset & 31u) != 0u) {
				value = "(" + value + " >> 16)";
			}
			value = "(" + value + " & 0xffffu)";
		}
		return value;
	}

	std::string AddressF32(const Inst& address, const IR::MemoryInfo& mem, uint32_t component) {
		const auto layout = Decoder::ImageAddressComponentLayout(mem.image_sample_flags, component);
		const std::string raw = AddressU32(address, mem, component);
		if (layout.bit_width == 16u) {
			return "float(as_type<half>((ushort)(" + raw + ")))";
		}
		return "as_type<float>(" + raw + ")";
	}

	std::string SampleArrayIndex(const Inst& address, const IR::MemoryInfo& mem,
	                             uint32_t component, const std::string& texture) {
		// Sample/gather addresses carry a floating-point layer, unlike image load/store.
		// Metal takes an integer slice: round to nearest even, then clamp before conversion.
		return "uint(clamp(rint(" + AddressF32(address, mem, component) + "), 0.0f, float(" +
		       texture + ".get_array_size() - 1u)))";
	}

	std::string CoordF32(const Inst& address, const IR::MemoryInfo& mem, uint32_t first,
	                     uint32_t components, bool cube) {
		std::string x = AddressF32(address, mem, first);
		if (components <= 1u) {
			return "float(" + x + ")";
		}
		std::string y = mem.image_address_components > first + 1u
		                    ? AddressF32(address, mem, first + 1u) : "0.0f";
		if (cube) {
			x = "(" + x + " - 1.0f)";
			y = "(" + y + " - 1.0f)";
		}
		if (components == 2u) {
			return "float2(" + x + ", " + y + ")";
		}
		std::string z = mem.image_address_components > first + 2u
		                    ? AddressF32(address, mem, first + 2u) : "0.0f";
		if (cube) {
			// Guest packs cube layers with padding every 8; the host uses a dense index.
			const std::string guest = "uint(" + z + ")";
			z = "float(" + guest + " - ((" + guest + " >> 3) << 1))";
		}
		return "float3(" + x + ", " + y + ", " + z + ")";
	}

	std::string CoordU32(const Inst& address, const IR::MemoryInfo& mem, uint32_t components) {
		std::string x = AddressU32(address, mem, 0);
		if (components <= 1u) {
			return "uint(" + x + ")";
		}
		std::string y = mem.image_address_components > 1u ? AddressU32(address, mem, 1) : "0u";
		if (components == 2u) {
			return "uint2(" + x + ", " + y + ")";
		}
		std::string z = mem.image_address_components > 2u ? AddressU32(address, mem, 2) : "0u";
		return "uint3(" + x + ", " + y + ", " + z + ")";
	}

	static bool ImageIsFloatClass(const IR::ImageResource& image) {
		return image.numeric_class != Prospero::TextureNumericClass::Uint &&
		       image.numeric_class != Prospero::TextureNumericClass::Sint;
	}

	// D16 image data is packed two 16-bit components per dword (the IR contract; the SPIR-V backend packs and
	// unpacks the same way): loads return packed words, stores receive them.
	std::string TexelBits(const IR::ImageResource& image, const std::string& expr,
	                      const IR::MemoryInfo* mem = nullptr) {
		std::string bits;
		switch (image.numeric_class) {
			case Prospero::TextureNumericClass::Uint: bits = expr; break;
			default: bits = "as_type<uint4>(" + expr + ")"; break;
		}
		if (mem != nullptr && mem->data_bits == 16u) {
			return "kyty_d16_pack(" + bits + ", " + std::to_string(mem->dmask != 0u ? mem->dmask : 1u) + "u, " +
			       (ImageIsFloatClass(image) ? "true" : "false") + ")";
		}
		return bits;
	}

	std::string TexelValue(const IR::ImageResource& image, const std::string& bits_in,
	                       const IR::MemoryInfo* mem = nullptr) {
		std::string bits = bits_in;
		if (mem != nullptr && mem->data_bits == 16u) {
			bits = "kyty_d16_unpack(" + bits_in + ", " + std::to_string(mem->dmask != 0u ? mem->dmask : 1u) + "u, " +
			       (ImageIsFloatClass(image) ? "true" : "false") + ")";
		}
		switch (image.numeric_class) {
			case Prospero::TextureNumericClass::Uint: return bits;
			case Prospero::TextureNumericClass::Sint: return "as_type<int4>(" + bits + ")";
			default: return "as_type<float4>(" + bits + ")";
		}
	}

	// GDS access that goes to the device GDS buffer (compute only, where the binding exists). The address is the guest
	// address plus the static offset, without the M0 base, as in the SPIR-V path (only ds_append/consume use M0).
	// KYTY_LOOP_TRIP_GDS=1: a compute shader with a GDS binding counts its runaway-loop guard trips in GDS dwords
	// kMslLoopTripGdsBase + loop id (ids past 7 share the last one); the GPU watchdog prints them. Diagnostic only.
	bool LoopTripGds() const {
		static const bool on = std::getenv("KYTY_LOOP_TRIP_GDS") != nullptr;
		if (!on) return false;
		for (const auto& binding: program.bindings.descriptors) {
			if (binding.kind == IR::DescriptorBindingKind::Gds) return true;
		}
		return false;
	}

	bool GdsMem(const IR::MemoryInfo& mem) const {
		return mem.kind == IR::ResourceKind::Gds && program.stage == ShaderType::Compute && MslGdsEnabled();
	}

	// Byte address of an LDS access: the guest address plus the static offset.
	std::string LdsByte(const Inst& inst, const IR::MemoryInfo& mem) {
		std::string expr = Expr(inst.Arg(0));
		if (mem.offset != 0) {
			expr = "(" + expr + " + " + U32Literal(mem.offset) + ")";
		}
		return expr;
	}

	// New LDS dword for a read-modify-write atomic given the old value and operand. Inc/Dec follow the SPIR-V
	// AtomicIncrement/AtomicDecrement wrap rules (operand is the limit).
	static std::string SharedAtomicNew(IR::ValueOpcode op, const std::string& old, const std::string& v) {
		switch (op) {
			case IR::ValueOpcode::SharedAtomicSwap32: return v;
			case IR::ValueOpcode::SharedAtomicIAdd32: return "(" + old + " + " + v + ")";
			case IR::ValueOpcode::SharedAtomicISub32: return "(" + old + " - " + v + ")";
			case IR::ValueOpcode::SharedAtomicAnd32: return "(" + old + " & " + v + ")";
			case IR::ValueOpcode::SharedAtomicOr32: return "(" + old + " | " + v + ")";
			case IR::ValueOpcode::SharedAtomicXor32: return "(" + old + " ^ " + v + ")";
			case IR::ValueOpcode::SharedAtomicUMin32: return "min(" + old + ", " + v + ")";
			case IR::ValueOpcode::SharedAtomicUMax32: return "max(" + old + ", " + v + ")";
			case IR::ValueOpcode::SharedAtomicSMin32: return "as_type<uint>(min((int)" + old + ", (int)" + v + "))";
			case IR::ValueOpcode::SharedAtomicSMax32: return "as_type<uint>(max((int)" + old + ", (int)" + v + "))";
			case IR::ValueOpcode::SharedAtomicInc32: return "(" + old + " >= " + v + " ? 0u : " + old + " + 1u)";
			case IR::ValueOpcode::SharedAtomicDec32:
				return "((" + old + " == 0u || " + old + " > " + v + ") ? " + v + " : " + old + " - 1u)";
			default: return old;
		}
	}

	// Byte address of an unformatted storage-buffer access. Mirrors
	// CalculateBufferAddress: index*stride + offset + immediate + soffset.
	std::string BufferByte(const Inst& inst, const IR::MemoryInfo& mem, uint32_t packed,
	                       uint32_t extra = 0) {
		const uint32_t stride = packed & 0x3fffu;
		std::string index = Expr(inst.Arg(1));
		if ((packed & (1u << 20u)) != 0u && program.stage != ShaderType::Vertex) {
			index = "(" + index + " + (" + LaneIdExpr() + " & 63u))";
		}
		std::string expr;
		if (stride != 0) {
			expr = "(" + index + " * " + U32Literal(stride) + ") + ";
		}
		expr += Expr(inst.Arg(2));
		if ((mem.offset + extra) != 0) {
			expr += " + " + U32Literal(mem.offset + extra);
		}
		expr += " + " + Expr(inst.Arg(3));
		return "(" + expr + ")";
	}

	std::string BufferDword(const Inst& inst, const IR::MemoryInfo& mem, uint32_t packed) {
		return "(" + BufferByte(inst, mem, packed) + " >> 2)";
	}

	static std::string SignExtendBits(const std::string& raw, uint32_t bits) {
		if (bits == 0u || bits >= 32u) {
			return raw;
		}
		const std::string shift = std::to_string(32u - bits) + "u";
		return "as_type<uint>((int)(" + raw + " << " + shift + ") >> " + shift + ")";
	}

	std::string NormalizeFormatComponent(const Format::BufferFormatInfo& info, uint32_t component,
	                                     const std::string& raw) {
		const uint32_t bits = info.component_bits[component];
		switch (info.type) {
			case Format::ComponentType::Uint:
			case Format::ComponentType::Sint: return raw;
			case Format::ComponentType::Uscaled:
				return "as_type<uint>(float(" + raw + "))";
			case Format::ComponentType::Sscaled:
				return "as_type<uint>(float(int(" + SignExtendBits(raw, bits) + ")))";
			case Format::ComponentType::Unorm: {
				const float max_value = static_cast<float>((1u << bits) - 1u);
				return "as_type<uint>(float(" + raw + ") / " + std::to_string(max_value) + "f)";
			}
			case Format::ComponentType::Snorm: {
				const float max_value = static_cast<float>((1u << (bits - 1u)) - 1u);
				return "as_type<uint>(max(float(int(" + SignExtendBits(raw, bits) + ")) / " +
				       std::to_string(max_value) + "f, -1.0f))";
			}
			case Format::ComponentType::Float:
				if (bits == 16u) {
					return "as_type<uint>(float(as_type<half>(ushort(" + raw + "))))";
				}
				return raw;
			default: return raw;
		}
	}

	// One formatted buffer component as U32 bits, following the SPIR-V backend's
	// FormattedLoad: descriptor swizzle selects the source channel, then the raw
	// subword is extracted and normalized.
	std::string FormattedComponentExpr(const Inst& inst, const IR::MemoryInfo& mem,
	                                   const Format::BufferFormatInfo& info,
	                                   uint32_t output_component, const std::string& ptr,
	                                   uint32_t packed) {
		const auto source = mem.typed
		                        ? (output_component < info.component_count
		                               ? Format::FormattedSource {Format::FormattedSourceKind::Memory,
		                                                          output_component}
		                               : Format::FormattedSource {})
		                        : Format::ResolveFormattedSource(
		                              info, GetDstSel(
		                                        program.info.buffers[mem.resource].descriptor_swizzle,
		                                        output_component));
		if (source.kind != Format::FormattedSourceKind::Memory) {
			return U32Literal(Format::FormattedConstantBits(info, source.kind));
		}
		const uint32_t component = source.component;
		const uint32_t bits      = info.component_bits[component];
		const uint32_t extra     = Format::GetFormatComponentByteOffset(info, component);
		const std::string byte   = BufferByte(inst, mem, packed, extra);
		const std::string dword = "(" + byte + " >> 2)";
		std::string raw;
		if (info.packed_bitfield) {
			const uint32_t mask = (1u << bits) - 1u;
			raw = "((" + ptr + "[" + dword + "] >> " +
			      std::to_string(info.component_bit_offset[component]) + "u) & " +
			      U32Literal(mask) + ")";
		} else if (bits == 32u) {
			raw = ptr + "[" + dword + "]";
		} else {
			const uint32_t mask = (1u << bits) - 1u;
			raw = "((" + ptr + "[" + dword + "] >> ((" + byte + " & 3u) * 8u)) & " +
			      U32Literal(mask) + ")";
		}
		const auto limit = "sizes[" + std::to_string(mem.resource) + "]";
		const auto bytes = std::to_string(info.packed_bitfield ? 4u : bits / 8u) + "u";
		return "(" + Expr(inst.Arg(inst.NumArgs() - 1)) + " && " + byte + " <= " + limit + " && " + bytes +
		    " <= " + limit + " - " + byte + " ? " + NormalizeFormatComponent(info, component, raw) + " : 0u)";
	}

	bool EmitFormattedStore(const Inst& inst, const IR::MemoryInfo& mem, const std::string& ptr,
	                        uint32_t packed) {
		const auto format = mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
		                              : program.info.buffers[mem.resource].descriptor_format;
		const auto info = Format::GetFormatInfo(format);
		if (info.type == Format::ComponentType::Unknown || info.component_count == 0 || info.byte_size == 0) return false;
		for (uint32_t c = 0; c < info.component_count; ++c) {
			const auto bits = info.component_bits[c];
			if (bits == 0 || bits > 32 || (info.type == Format::ComponentType::Float && bits != 16 && bits != 32)) return false;
			if (!info.packed_bitfield && bits != 8 && bits != 16 && bits != 32) return false;
		}
		const uint32_t count = inst.GetOpcode() == ValueOpcode::StoreBufferU32 ? 1u
		                       : inst.GetOpcode() == ValueOpcode::StoreBufferU32x2 ? 2u
		                       : inst.GetOpcode() == ValueOpcode::StoreBufferU32x3 ? 3u : 4u;
		const auto value = Expr(inst.Arg(inst.NumArgs() - 2));
		const auto limit = "sizes[" + std::to_string(mem.resource) + "]";
		body << "\t\t{\n\t\t\tconst uint kyty_byte = " << BufferByte(inst, mem, packed) << ";\n"
		     << "\t\t\tif (" << Expr(inst.Arg(inst.NumArgs() - 1)) << " && kyty_byte <= " << limit
		     << " && " << info.byte_size << "u <= " << limit << " - kyty_byte) {\n"
		     << "\t\t\t\tdevice uchar* kyty_dst = ((device uchar*)" << ptr << ") + kyty_byte;\n";
		if (info.packed_bitfield) body << "\t\t\t\tuint kyty_packed = 0u;\n";
		for (uint32_t c = 0; c < info.component_count; ++c) {
			const uint32_t bits = info.component_bits[c];
			const uint32_t mask = bits == 32 ? UINT32_MAX : (1u << bits) - 1u;
			std::string component = c >= count ? "0u" : count == 1 ? value : value + "." + ComponentName(c);
			const auto f = "as_type<float>(" + component + ")";
			if (info.type == Format::ComponentType::Unorm) {
				component = "uint(rint(clamp(" + f + ", 0.0f, 1.0f) * " + std::to_string(mask) + ".0f))";
			} else if (info.type == Format::ComponentType::Snorm) {
				component = "as_type<uint>(int(rint(clamp(" + f + ", -1.0f, 1.0f) * " +
				            std::to_string((1u << (bits - 1u)) - 1u) + ".0f)))";
			} else if (info.type == Format::ComponentType::Float && bits == 16) {
				component = "uint(as_type<ushort>(half(" + f + ")))";
			} else if (info.type == Format::ComponentType::Uscaled) {
				component = "uint(clamp(" + f + ", 0.0f, " + std::to_string(mask) + ".0f))";
			} else if (info.type == Format::ComponentType::Sscaled) {
				component = "as_type<uint>(int(clamp(" + f + ", -" + std::to_string(1u << (bits - 1u)) +
				            ".0f, " + std::to_string((1u << (bits - 1u)) - 1u) + ".0f)))";
			}
			body << "\t\t\t\tconst uint kyty_c" << c << " = (" << component << ") & " << U32Literal(mask) << ";\n";
			if (info.packed_bitfield) {
				body << "\t\t\t\tkyty_packed |= kyty_c" << c << " << " << info.component_bit_offset[c] << "u;\n";
			} else {
				const uint32_t offset = Format::GetFormatComponentByteOffset(info, c);
				for (uint32_t b = 0; b < bits / 8u; ++b) {
					body << "\t\t\t\tkyty_dst[" << offset + b << "] = uchar(kyty_c" << c << " >> " << b * 8u << "u);\n";
				}
			}
		}
		if (info.packed_bitfield) {
			for (uint32_t b = 0; b < info.byte_size; ++b) body << "\t\t\t\tkyty_dst[" << b << "] = uchar(kyty_packed >> " << b * 8u << "u);\n";
		}
		body << "\t\t\t}\n\t\t}\n";
		return true;
	}

	[[nodiscard]] uint32_t LdsDwords() const {
		if (program.stage != ShaderType::Compute) return 2048u;
		const uint32_t declared = input_info.compute ? input_info.compute->lds_size_dwords : 0u;
		return declared != 0 ? std::min(declared, 8192u) : 8192u;
	}

	[[nodiscard]] bool UsesLds() const {
		for (const auto& info: program.memory_info) {
			if (info.kind == IR::ResourceKind::Lds) {
				return true;
			}
		}
		return false;
	}

	std::string GuestAddress(const Inst& inst, const IR::MemoryInfo& mem) {
		const bool scalar = mem.kind == IR::ResourceKind::ScalarAddress;
		const auto low = "(" + Expr(inst.Arg(1)) + (scalar ? " & ~3u)" : ")");
		std::string address;
		if (mem.address_is_full) {
			address = "((ulong(" + Expr(inst.Arg(2)) + ") << 32u) | ulong(" + low + "))";
		} else {
			const auto* handle = inst.Arg(0).Resolve().TryInstruction();
			if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetAddressResource || handle->NumArgs() != 2) {
				Unsupported(inst, "missing guest address base");
				return "0ul";
			}
			const auto base_low = "(" + Expr(handle->Arg(0)) + (scalar ? " & ~3u)" : ")");
			address = "(((ulong(" + Expr(handle->Arg(1)) + ") << 32u) | ulong(" + base_low + ")) + ulong(" + low + "))";
		}
		const int32_t offset = static_cast<int32_t>(scalar ? mem.offset & ~3u : mem.offset);
		return "(" + address + " + ulong(" + std::to_string(offset) + "l))";
	}

	[[nodiscard]] bool HasDescriptor(IR::DescriptorBindingKind kind) const {
		for (const auto& binding: program.bindings.descriptors) {
			if (binding.kind == kind) {
				return true;
			}
		}
		return false;
	}

	void EmitBuiltin(const Inst& inst) {
		const auto kind_value = ImmU32(inst.Arg(0));
		const auto comp_value = ImmU32(inst.Arg(1));
		if (!kind_value || !comp_value) {
			Unsupported(inst, "non-immediate builtin index");
			EmitAssignment(inst, "0u");
			return;
		}
		const auto kind = static_cast<IR::StageInputKind>(*kind_value);
		const auto comp = *comp_value;
		switch (kind) {
			case IR::StageInputKind::VertexIndex: EmitAssignment(inst, "vid"); return;
			case IR::StageInputKind::InstanceIndex: EmitAssignment(inst, "iid"); return;
			case IR::StageInputKind::InvocationId: EmitAssignment(inst, "0u"); return;
			case IR::StageInputKind::PrimitiveId:
				if (!prim_id_field.empty()) {
					EmitAssignment(inst, "in." + prim_id_field);
				} else {
					EmitAssignment(inst, "prim_id_arg");
				}
				return;
			case IR::StageInputKind::FragCoord:
				if (!frag_coord_field.empty()) {
					EmitAssignment(inst, std::string("as_type<uint>(in.") + frag_coord_field + "." +
					                        ComponentName(comp) + ")");
				} else {
					unsupported.push_back("frag coord without pixel stage input");
				}
				return;
			case IR::StageInputKind::FrontFacing:
				if (!front_facing_field.empty()) {
					EmitAssignment(inst, "(in." + front_facing_field + " ? 0x3f800000u : 0u)");
				} else {
					unsupported.push_back("front facing without pixel stage input");
				}
				return;
			case IR::StageInputKind::BaryCoordSmooth:
			case IR::StageInputKind::BaryCoordSmoothCentroid:
			case IR::StageInputKind::BaryCoordNoPerspective:
				if (!bary_field.empty()) {
					EmitAssignment(inst, std::string("as_type<uint>(in.") + bary_field + "." +
					                        ComponentName(comp) + ")");
				} else {
					unsupported.push_back("barycentric builtin without interface");
				}
				return;
			case IR::StageInputKind::WorkgroupId:
				EmitAssignment(inst, std::string("tgid.") + ComponentName(comp));
				return;
			case IR::StageInputKind::LocalInvocationId:
				EmitAssignment(inst, pair_ ? PairLocalId(comp) : std::string("tid.") + ComponentName(comp));
				return;
			case IR::StageInputKind::LocalInvocationIndex: EmitAssignment(inst, pair_ ? PairLinearId() : "liid"); return;
			case IR::StageInputKind::GlobalInvocationId:
				EmitAssignment(inst, pair_ ? "(tgid." + std::string(ComponentName(comp)) + " * " + std::to_string(WorkgroupSize(comp)) + "u + " +
				                                 PairLocalId(comp) + ")"
				                           : std::string("gid.") + ComponentName(comp));
				return;
			default:
				Unsupported(inst, "builtin kind");
				EmitAssignment(inst, "0u");
				return;
		}
	}

	// Guest workgroup size along an axis, and the guest thread ids of the half being emitted: a pair-packed kernel runs one-dimensional
	// host groups of half the guest threads, so host thread (simdgroup s, lane l) stands for guest threads 64*s + l and 64*s + 32 + l.
	[[nodiscard]] uint32_t WorkgroupSize(uint32_t axis) const {
		const auto* cs = ShaderWorkgroupInput(program.stage, input_info);
		return cs == nullptr ? 1u : std::max(cs->threads_num[axis < 3u ? axis : 2u], 1u);
	}

	[[nodiscard]] std::string PairLinearId() const { return half_ == 0 ? "kyty_gl0" : "kyty_gl1"; }

	[[nodiscard]] std::string PairLocalId(uint32_t component) const {
		const uint32_t axis    = component < 3u ? component : 2u;
		uint32_t       divisor = 1u;
		for (uint32_t i = 0; i < axis; ++i) divisor *= WorkgroupSize(i);
		return "((" + PairLinearId() + " / " + std::to_string(divisor) + "u) % " + std::to_string(WorkgroupSize(axis)) + "u)";
	}

	void EmitGetAttribute(const Inst& inst) {
		const auto attr_value = ImmU32(inst.Arg(0));
		const auto chan_value = ImmU32(inst.Arg(1));
		if (!attr_value || !chan_value) {
			Unsupported(inst, "non-immediate attribute index");
			EmitAssignment(inst, "0u");
			return;
		}
		const auto attr = *attr_value;
		const auto chan = *chan_value;
		if (program.stage != ShaderType::Pixel) {
			Unsupported(inst, "vertex-stage attribute fetch");
			EmitAssignment(inst, "0u");
			return;
		}
		const auto it = in_param_field.find(attr);
		if (it == in_param_field.end()) {
			unsupported.push_back("pixel parameter input location " + std::to_string(attr));
			EmitAssignment(inst, "0u");
			return;
		}
		EmitAssignment(inst, std::string("as_type<uint>(in.") + it->second + "." +
		                        ComponentName(chan) + ")");
	}

	void EmitSetAttribute(const Inst& inst) {
		const auto& exp = program.export_info.at(inst.Flags<IR::ExportFlags>().index);
		// An export with the valid-mask bit makes EXEC the pixel's coverage: a pixel out of it is dropped, colour and depth (the SPIR-V path
		// kills it the same way). Without this, every pixel a shader rejects wrote zeros to all its targets and its depth: the body under a kit,
		// which the game masks out, punched black holes into the shirt and shorts. Null exports carry the bit too, so this comes first.
		static const bool s_no_kill = std::getenv("KYTY_MSL_NO_KILL") != nullptr;
		if (program.stage == ShaderType::Pixel && exp.vm && !s_no_kill) {
			body << "\t\tif (!(" << Expr(inst.Arg(1)) << ")) discard_fragment();\n";
		}
		if (exp.kind == IR::ExportTargetKind::Null || exp.en == 0u) {
			return;
		}
		const std::string value = Expr(inst.Arg(0));
		const std::string exec  = Expr(inst.Arg(1));

		std::string field;
		switch (exp.kind) {
			case IR::ExportTargetKind::Position:
				if (exp.index != 0) {
					// Auxiliary position exports carry point size, layer, clip and cull distances: only the clip distances are kept.
					if (clip_distance_count != 0u && input_info.vertex != nullptr && !exp.compr) {
						for (uint32_t component = 0; component < 4; ++component) {
							if ((exp.en & (1u << component)) == 0u) continue;
							const auto decoded = IR::DecodePositionExportComponent(input_info.vertex->pa_cl_vs_out_cntl, exp.index, component);
							if (decoded.clip_distance == UINT32_MAX || decoded.clip_distance >= clip_distance_count) continue;
							body << "\t\tif (" << exec << ") {\n\t\t\tout.clip_dist[" << decoded.clip_distance << "] = as_type<float>(" << value
							     << "." << ComponentName(component) << ");\n\t\t}\n";
						}
					}
					return;
				}
				field = position_field;
				break;
			case IR::ExportTargetKind::Parameter: {
				const auto it = out_param_field.find(exp.index);
				if (it != out_param_field.end()) {
					field = it->second;
				}
				break;
			}
			case IR::ExportTargetKind::Mrt: {
				const auto it = mrt_field.find(exp.index);
				if (it != mrt_field.end()) {
					field = it->second;
				}
				break;
			}
			case IR::ExportTargetKind::Primitive:
				// Tessellation/mesh primitive attribute; no export slot in VS/PS.
				return;
			case IR::ExportTargetKind::MrtZ:
				if ((exp.en & 1u) != 0u && !depth_field.empty()) {
					body << "\t\tif (" << exec << ") {\n\t\t\tout." << depth_field
					     << " = as_type<float>(" << value << ".x);\n\t\t}\n";
					return;
				}
				return;
			default: {
				char reason[64];
				std::snprintf(reason, sizeof(reason), "export kind=%u index=%u en=%u",
				              static_cast<uint32_t>(exp.kind), exp.index, exp.en);
				Unsupported(inst, reason);
				return;
			}
		}
		if (field.empty()) {
			Unsupported(inst, "export has no interface field");
			return;
		}

		uint32_t integer = 0;
		if (exp.kind == IR::ExportTargetKind::Mrt) {
			const auto it = mrt_integer.find(exp.index);
			if (it != mrt_integer.end()) {
				integer = it->second;
			}
		}

		body << "\t\tif (" << exec << ") {\n";
		for (uint32_t component = 0; component < 4; ++component) {
			const bool component_written =
			    (exp.en & (1u << component)) != 0u &&
			    !(exp.compr && (exp.en & (3u << ((component / 2u) * 2u))) == 0u);
			if (!component_written) {
				// Unexported colour components read as (0, 0, 0, 1), as in the SPIR-V backend.
				if (exp.kind == IR::ExportTargetKind::Mrt && component == 3u) {
					body << "\t\t\tout." << field << "." << ComponentName(component) << " = "
					     << (integer == 0u ? "1.0f" : "1") << ";\n";
				}
				continue;
			}
			const std::string bits = value + "." + ComponentName(component);
			body << "\t\t\tout." << field << "." << ComponentName(component) << " = ";
			if (exp.compr) {
				// Compressed export: two 16-bit components per source dword (R,G in the first, B,A in the second).
				const uint32_t pair = component / 2u;
				const uint32_t lane = component % 2u;
				const std::string word = value + "." + ComponentName(pair);
				if (integer == 1u) {
					body << "((" << word << " >> " << lane * 16u << "u) & 0xffffu)";
				} else if (integer == 2u) {
					body << "as_type<int>((" << word << " >> " << lane * 16u << "u) & 0xffffu)";
				} else {
					const bool unorm16 = exp.kind == IR::ExportTargetKind::Mrt && input_info.pixel != nullptr &&
					                     exp.index < std::size(input_info.pixel->target_output_mode) &&
					                     input_info.pixel->target_output_mode[exp.index] == 5u;
					if (unorm16) {
						body << "unpack_unorm2x16_to_float(" << word << ")[" << lane << "]";
					} else {
						body << "float(as_type<half2>(" << word << ")[" << lane << "])";
					}
				}
				body << ";\n";
				continue;
			}
			if (integer == 1u) {
				body << bits;
			} else if (integer == 2u) {
				body << "as_type<int>(" << bits << ")";
			} else {
				body << "as_type<float>(" << bits << ")";
			}
			body << ";\n";
		}
		body << "\t\t}\n";
	}

	static uint32_t GatherComponent(uint32_t dmask) {
		switch (dmask) {
			case 0x2u: return 1u;
			case 0x4u: return 2u;
			case 0x8u: return 3u;
			default: return 0u;
		}
	}

	// DPP source lane of the lane `g` (a guest lane id when pair-packed, else the host simdgroup lane), and whether this move has a
	// source for it (a row shift leaves the lanes at the row edge without one). RDNA DPP16 only: quad_perm, row_shl/shr/ror,
	// row_mirror/half_mirror, row_share and row_xmask; the wave-wide controls of GFX9 do not exist there.
	struct DppLane {
		std::string target;
		std::string valid;
	};

	static DppLane DppSourceLane(const IR::DppMoveFlags& flags, const std::string& g) {
		const uint32_t    control = flags.control;
		const std::string row     = "(" + g + " & ~15u)";
		const std::string col     = "(" + g + " & 15u)";
		if (flags.dpp8 || control <= 0xffu) {
			const uint32_t    bits = flags.dpp8 ? 3u : 2u;
			const std::string m    = std::to_string((1u << bits) - 1u) + "u";
			return {"((" + g + " & ~" + m + ") | ((" + U32Literal(control) + " >> ((" + g + " & " + m + ") * " +
			            std::to_string(bits) + "u)) & " + m + "))",
			        "true"};
		}
		if (control >= 0x101u && control <= 0x10fu) {
			const uint32_t amount = control & 0xfu;
			return {"(" + row + " | (" + col + " + " + std::to_string(amount) + "u))",
			        "(" + col + " < " + std::to_string(16u - amount) + "u)"};
		}
		if (control >= 0x111u && control <= 0x11fu) {
			const uint32_t amount = control & 0xfu;
			return {"(" + row + " | (" + col + " - " + std::to_string(amount) + "u))",
			        "(" + col + " >= " + std::to_string(amount) + "u)"};
		}
		if (control >= 0x121u && control <= 0x12fu) {
			const std::string a = std::to_string(control & 0xfu) + "u";
			return {"(" + row + " | (" + col + " >= " + a + " ? " + col + " - " + a + " : " + col + " + " +
			            std::to_string(16u - (control & 0xfu)) + "u))",
			        "true"};
		}
		if (control == 0x140u) return {"(" + row + " | (15u - " + col + "))", "true"};
		if (control == 0x141u) return {"((" + g + " & ~7u) | (7u - (" + g + " & 7u)))", "true"};
		if (control >= 0x150u && control <= 0x15fu) return {"(" + row + " | " + std::to_string(control & 0xfu) + "u)", "true"};
		if (control >= 0x160u && control <= 0x16fu) return {"(" + g + " ^ " + std::to_string(control & 0xfu) + "u)", "true"};
		return {g, "true"};
	}

	// ds_swizzle_b32 source lane within the 32-lane group of `lane` (the host simdgroup lane, which is also the lane within its half).
	static std::string SwizzleSourceLane(uint32_t control) {
		if ((control & 0xc000u) == 0xc000u) {
			const uint32_t mask   = control & 0x1fu;
			const uint32_t rotate = (control >> 5u) & 0x1fu;
			const uint32_t delta  = (control & 0x400u) != 0u ? ((32u - rotate) & 0x1fu) : rotate;
			return "((lane & " + std::to_string(mask) + "u) | (((lane + " + std::to_string(delta) + "u) & 31u) & " +
			       std::to_string((~mask) & 31u) + "u))";
		}
		if ((control & 0x8000u) != 0u) {
			return "((lane & ~3u) | ((" + U32Literal(control) + " >> ((lane & 3u) * 2u)) & 3u))";
		}
		return "(((lane & " + std::to_string(control & 0x1fu) + "u) | " + std::to_string((control >> 5u) & 0x1fu) + "u) ^ " +
		       std::to_string((control >> 10u) & 0x1fu) + "u)";
	}

	// Whole-wave operations of a pair-packed program. They see both emulated halves of the wave, so a ballot has all 64 bits, a
	// readlane or DPP move can read a lane of the other half, and a scan reaches lane 63. Returns false for the operations that
	// need no change (they work on one half).
	bool EmitWavePair(const Inst& inst) {
		const auto half_name = [&](uint32_t half) { return HalfName(inst, half); };
		const auto arg       = [&](size_t index, uint32_t half) { return ExprHalf(inst.Arg(index), half); };
		switch (inst.GetOpcode()) {
			case IR::ValueOpcode::LaneId: EmitAssignment(inst, LaneIdExpr()); return true;
			case IR::ValueOpcode::Ballot:
				if (half_ == 0) {
					body << "\t\t" << half_name(0) << " = kyty_ballot2(" << arg(0, 0) << ", " << arg(0, 1) << ");\n"
					     << "\t\t" << half_name(1) << " = " << half_name(0) << ";\n";
				}
				return true;
			case IR::ValueOpcode::ReadFirstLane:
				if (half_ == 0) {
					body << "\t\t{\n"
					     << "\t\t\tconst uint kyty_rl = kyty_first64(kyty_ballot2(" << arg(1, 0) << ", " << arg(1, 1) << "));\n"
					     << "\t\t\t" << half_name(0) << " = kyty_shuffle64(" << arg(0, 0) << ", " << arg(0, 1) << ", kyty_rl);\n"
					     << "\t\t\t" << half_name(1) << " = " << half_name(0) << ";\n\t\t}\n";
				}
				return true;
			case IR::ValueOpcode::ReadLane:
				EmitAssignment(inst, "kyty_shuffle64(" + arg(0, 0) + ", " + arg(0, 1) + ", " + Expr(inst.Arg(1)) + " & 63u)");
				return true;
			case IR::ValueOpcode::WriteLane:
				EmitAssignment(inst, "(" + LaneIdExpr() + " == " + Expr(inst.Arg(2)) + " ? " + Expr(inst.Arg(1)) + " : " +
				                         Expr(inst.Arg(0)) + ")");
				return true;
			case IR::ValueOpcode::DppMoveU32: {
				const auto flags = inst.Flags<IR::DppMoveFlags>();
				const auto src   = DppSourceLane(flags, LaneIdExpr());
				body << "\t\t{\n"
				     << "\t\t\tconst uint kyty_dt = " << src.target << " & 63u;\n"
				     << "\t\t\tuint kyty_dv = kyty_shuffle64(" << arg(0, 0) << ", " << arg(0, 1) << ", kyty_dt);\n";
				if (!flags.fetch_inactive) {
					// The source lane has to be live (in EXEC, the predicate) for the move to deliver its value.
					body << "\t\t\tconst uint4 kyty_db = kyty_ballot2(" << arg(1, 0) << ", " << arg(1, 1) << ");\n"
					     << "\t\t\tconst bool kyty_da = ((((kyty_dt & 32u) != 0u ? kyty_db.y : kyty_db.x) >> (kyty_dt & 31u)) & 1u) != 0u;\n"
					     << "\t\t\tkyty_dv = (" << src.valid << " && kyty_da) ? kyty_dv : 0u;\n";
				}
				body << "\t\t\t" << Name(inst) << " = kyty_dv;\n\t\t}\n";
				return true;
			}
			case IR::ValueOpcode::DppUpdateU32: {
				const auto        flags = inst.Flags<IR::DppMoveFlags>();
				const std::string g     = LaneIdExpr();
				std::string       write = "(" + Expr(inst.Arg(2)) + " && ((" + std::to_string(flags.bank_mask) + "u >> ((" + g +
				                    " >> 2u) & 3u)) & 1u) != 0u && ((" + std::to_string(flags.row_mask) + "u >> ((" + g +
				                    " >> 4u) & 3u)) & 1u) != 0u";
				if (!flags.bound_control) write += " && " + DppSourceLane(flags, g).valid;
				write += ")";
				EmitAssignment(inst, "(" + write + " ? " + Expr(inst.Arg(0)) + " : " + Expr(inst.Arg(1)) + ")");
				return true;
			}
			default: return false;
		}
	}

	void EmitWave(const Inst& inst) {
		// Vertex functions support ballot/shuffle, but not the lane-index
		// parameter used by the other wave operations below.
		if (program.stage == ShaderType::Vertex &&
		    inst.GetOpcode() != ValueOpcode::Ballot &&
		    inst.GetOpcode() != ValueOpcode::ReadLane &&
		    inst.GetOpcode() != ValueOpcode::ReadFirstLane) {
			Unsupported(inst, "wave operation in a vertex shader");
			if (inst.GetType() != IR::Type::Void) {
				EmitAssignment(inst, "0u");
			}
			return;
		}
		if (pair_ && EmitWavePair(inst)) return;
		switch (inst.GetOpcode()) {
			case IR::ValueOpcode::LaneId:
				EmitAssignment(inst, "lane");
				return;
			case IR::ValueOpcode::Ballot:
				EmitAssignment(inst, "kyty_ballot(" + Expr(inst.Arg(0)) + ")");
				return;
			case IR::ValueOpcode::ReadLane:
				EmitAssignment(inst, "simd_shuffle(" + Expr(inst.Arg(0)) + ", (ushort)(" +
				                        Expr(inst.Arg(1)) + " & 31u))");
				return;
			case IR::ValueOpcode::WriteLane:
				EmitAssignment(inst, "(lane == " + Expr(inst.Arg(2)) + " ? " + Expr(inst.Arg(1)) +
				                        " : " + Expr(inst.Arg(0)) + ")");
				return;
			case IR::ValueOpcode::ReadFirstLane:
				EmitAssignment(inst, "simd_shuffle(" + Expr(inst.Arg(0)) +
				                        ", kyty_first_lane(kyty_ballot(" + Expr(inst.Arg(1)) + ")))");
				return;
			case IR::ValueOpcode::DppMoveU32: {
				const auto flags = inst.Flags<IR::DppMoveFlags>();
				EmitAssignment(inst, "simd_shuffle(" + Expr(inst.Arg(0)) + ", (ushort)((" +
				                        DppSourceLane(flags, "lane").target + ") & 31u))");
				return;
			}
			case IR::ValueOpcode::DppUpdateU32:
				EmitAssignment(inst, "(" + Expr(inst.Arg(2)) + " ? " + Expr(inst.Arg(0)) + " : " +
				                        Expr(inst.Arg(1)) + ")");
				return;
			case IR::ValueOpcode::Permlane16U32: {
				const auto flags = inst.Flags<IR::PermlaneFlags>();
				body << "\t\t{\n"
				     << "\t\t\tconst uint kyty_row = lane & ~15u;\n"
				     << "\t\t\tconst uint kyty_row_value = "
				     << (flags.x16 ? "(kyty_row ^ 16u)" : "kyty_row") << ";\n"
				     << "\t\t\tconst uint kyty_shift = (lane & 7u) * 4u;\n"
				     << "\t\t\tconst uint kyty_selected = ((lane & 15u) >= 8u) ? "
				     << Expr(inst.Arg(2)) << " : " << Expr(inst.Arg(1)) << ";\n"
				     << "\t\t\tconst uint kyty_target = kyty_row_value | ((kyty_selected >> "
				        "kyty_shift) & 15u);\n"
				     << "\t\t\t" << Name(inst) << " = simd_shuffle(" << Expr(inst.Arg(0))
				     << ", (ushort)(kyty_target & 31u));\n";
				if (!flags.fetch_inactive) {
					body << "\t\t\tif (!simd_shuffle(uint(" << Expr(inst.Arg(3))
					     << "), (ushort)(kyty_target & 31u))) " << Name(inst) << " = 0u;\n";
				}
				body << "\t\t}\n";
				return;
			}
			case IR::ValueOpcode::BpermuteU32: {
				const std::string target = "((lane & ~31u) | ((" + Expr(inst.Arg(1)) +
				                           " >> 2) & 31u))";
				EmitAssignment(inst, "(" + Expr(inst.Arg(2)) + " ? simd_shuffle(" +
				                        Expr(inst.Arg(0)) + ", (ushort)(" + target +
				                        " & 31u)) : 0u)");
				return;
			}
			case IR::ValueOpcode::SwizzleU32: {
				// ds_swizzle_b32: reads a lane of the same 32-lane group; a source lane that is not live gives zero.
				const auto control = ImmU32(inst.Arg(1));
				body << "\t\t{\n"
				     << "\t\t\tconst uint kyty_sw = " << SwizzleSourceLane(control ? *control : 0u) << " & 31u;\n"
				     << "\t\t\t" << Name(inst) << " = simd_shuffle(uint(" << Expr(inst.Arg(2)) << "), (ushort)kyty_sw) != 0u ? simd_shuffle("
				     << Expr(inst.Arg(0)) << ", (ushort)kyty_sw) : 0u;\n\t\t}\n";
				return;
			}
			default: Unsupported(inst, "wave operation"); return;
		}
	}

	void EmitImage(const Inst& inst) {
		const auto op     = inst.GetOpcode();
		const auto info   = IR::ImageOpcodeInfoOf(op);
		const auto& mem   = Mem(inst);
		const auto& image = program.info.images.at(mem.resource);
		const auto slot   = ImageSlotOf(mem.resource);
		const auto fail   = [&](const char* reason) {
			Unsupported(inst, reason);
			if (inst.GetType() != IR::Type::Void) {
				EmitAssignment(inst, "uint4(0u)");
			}
		};
		if (!slot) {
			fail("image resource is not bound");
			return;
		}
		// NoIndirectImage (UINT32_MAX) is the common direct case; only an image whose
		// indirect_root points at itself needs the runtime SRT mapping.
		if (!ImageSupported(image) || image.indirect_root == mem.resource) {
			char reason[128];
			std::snprintf(reason, sizeof(reason),
			              "image: r128=%d format=%u convert=%u depth=%d atomic=%d indirect=%u/%u",
			              image.r128 ? 1 : 0, static_cast<uint32_t>(image.numeric_class),
			              static_cast<uint32_t>(image.conversion_format),
			              image.depth_compare ? 1 : 0, image.atomic ? 1 : 0,
			              image.indirect_root, mem.resource);
			fail(reason);
			return;
		}
		const bool storage = static_cast<uint32_t>(slot->kind) >= IR::FirstStorageImageBinding;
		const std::string tex = "ab.image_" + std::to_string(mem.resource);
		const auto dim = DimensionInfo(image.dimension);
		const Inst* address = inst.Arg(info.needs_sampler ? 2 : 1).TryInstruction();
		if (address == nullptr) {
			fail("image address is not a MakeImageAddress instruction");
			return;
		}

		switch (op) {
			case IR::ValueOpcode::ImageSampleRaw: {
				const auto layout = LayoutOf(mem);
				if (image.cube) {
					// Experimental (KYTY_MSL_CUBE=1): the kernels that sample cubes (reflection filtering) run for seconds per frame
					// with the state-machine codegen and the cube faces are not uploaded yet, so they stay skipped by default.
					// Graphics stages sample cubes by default (a skipped pixel shader drops its draw); compute kernels stay off.
					if (program.stage == ShaderType::Compute && std::getenv("KYTY_MSL_CUBE") == nullptr) {
						fail("cube sample in compute (enable with KYTY_MSL_CUBE=1)");
						return;
					}
					if (dim.components < 2u || dim.components > 3u || dim.spatial != 2u || (mem.image_sample_flags & Decoder::ImageSampleFlagDerivative) != 0u ||
					    layout.dref >= 0) {
						char why[160];
						std::snprintf(why, sizeof(why), "cube sample: components=%u spatial=%u derivative=%d dref=%d dim=%u", dim.components,
						              dim.spatial, (mem.image_sample_flags & Decoder::ImageSampleFlagDerivative) != 0u ? 1 : 0, layout.dref,
						              static_cast<uint32_t>(image.dimension));
						fail(why);
						return;
					}
					const auto cube_sampler = SamplerSlotOf(mem.sampler);
					if (!cube_sampler) {
						fail("sampler resource is not bound");
						return;
					}
					const std::string sampler = "ab.samplers[" + std::to_string(*cube_sampler) + "]";
					const std::string c3 = "(" + CoordF32(*address, mem, static_cast<uint32_t>(layout.coord), 3u, true) + ")";
					const bool lod_flag_c = (mem.image_sample_flags & Decoder::ImageSampleFlagLod) != 0u;
					const bool level_zero_c = (mem.image_sample_flags & Decoder::ImageSampleFlagLevelZero) != 0u;
					const std::string face = "uint(" + c3 + ".z)";
					std::string call;
					if (lod_flag_c || level_zero_c || program.stage != ShaderType::Pixel) {
						const std::string lod = (lod_flag_c && layout.lod >= 0)
						                            ? AddressF32(*address, mem, static_cast<uint32_t>(layout.lod)) : "0.0f";
						call = tex + ".sample(" + sampler + ", " + c3 + ".xy, " + face + ", level(" + lod + "))";
					} else if (layout.bias >= 0) {
						call = tex + ".sample(" + sampler + ", " + c3 + ".xy, " + face + ", bias(" +
						       AddressF32(*address, mem, static_cast<uint32_t>(layout.bias)) + "))";
					} else {
						call = tex + ".sample(" + sampler + ", " + c3 + ".xy, " + face + ")";
					}
					EmitAssignment(inst, TexelBits(image, call, &mem));
					return;
				}
				const auto sampler_slot = SamplerSlotOf(mem.sampler);
				if (!sampler_slot) {
					fail("sampler resource is not bound");
					return;
				}
				const std::string sampler = "ab.samplers[" + std::to_string(*sampler_slot) + "]";
				const uint32_t spatial = dim.spatial;
				const bool arrayed     = dim.components > dim.spatial;
				const std::string coord =
				    CoordF32(*address, mem, static_cast<uint32_t>(layout.coord), spatial, false);
				const std::string array_arg =
				    arrayed ? (", " + SampleArrayIndex(*address, mem,
				                                       static_cast<uint32_t>(layout.coord) + spatial, tex))
				            : std::string {};
				const bool derivative =
				    (mem.image_sample_flags & Decoder::ImageSampleFlagDerivative) != 0u;
				const bool lod_flag = (mem.image_sample_flags & Decoder::ImageSampleFlagLod) != 0u;
				const bool level_zero =
				    (mem.image_sample_flags & Decoder::ImageSampleFlagLevelZero) != 0u;
				const bool explicit_lod =
				    derivative || lod_flag || level_zero || program.stage != ShaderType::Pixel;
				std::string call;
				if (derivative) {
					if ((spatial != 2u && spatial != 3u) || dim.msaa) {
						fail("derivatives of a 1D or multisampled image");
						return;
					}
					const std::string dx =
					    CoordF32(*address, mem, static_cast<uint32_t>(layout.grad_x), spatial, false);
					const std::string dy =
					    CoordF32(*address, mem, static_cast<uint32_t>(layout.grad_y), spatial, false);
					const std::string gradient = spatial == 3u ? "gradient3d" : "gradient2d";
					call = tex + ".sample(" + sampler + ", " + coord + array_arg + ", " + gradient +
					       "(" + dx + ", " + dy + "))";
				} else if (explicit_lod) {
					const std::string lod = (lod_flag && layout.lod >= 0)
					                            ? AddressF32(*address, mem,
					                                         static_cast<uint32_t>(layout.lod))
					                            : "0.0f";
					call = tex + ".sample(" + sampler + ", " + coord + array_arg + ", level(" +
					       lod + "))";
				} else if (layout.bias >= 0) {
					call = tex + ".sample(" + sampler + ", " + coord + array_arg + ", bias(" +
					       AddressF32(*address, mem, static_cast<uint32_t>(layout.bias)) + "))";
				} else {
					call = tex + ".sample(" + sampler + ", " + coord + array_arg + ")";
				}
				if (layout.dref >= 0) {
					// Depth-compare sample: the depth is sampled as a float and compared with the reference (texel >= reference,
					// no percentage-closer filtering). KYTY_MSL_CMP_FUNC=1: the sampler's own compare function instead (sizes[64 + slot]):
					// right for the shadow samplers (GREATER) but um190/um194 lost the red room's light (some dref samplers carry
					// NEVER/ALWAYS), so it stays opt-in.
					// KYTY_MSL_CMP_FUNC=ref: reference <func> texel (the Vulkan reference's order), as the mirrored function of texel-first.
					static const char* s_cmp_func = std::getenv("KYTY_MSL_CMP_FUNC");
					const std::string slot_func = "sizes[" + std::to_string(64u + *sampler_slot) + "]";
					const std::string func = s_cmp_func == nullptr ? std::string("6u")
					                         : std::strcmp(s_cmp_func, "ref") == 0 ? "((0x73516240u >> (" + slot_func + " * 4u)) & 0xfu)"
					                                                               : slot_func;
					call = "kyty_sample_cmp(" + call + ", " + AddressF32(*address, mem, static_cast<uint32_t>(layout.dref)) + ", " + func + ")";
				}
				EmitAssignment(inst, TexelBits(image, call, &mem));
				return;
			}
			case IR::ValueOpcode::ImageGatherRaw: {
				const auto sampler_slot = SamplerSlotOf(mem.sampler);
				if (!sampler_slot) {
					fail("gather sampler is not bound");
					return;
				}
				if (image.cube || dim.spatial != 2u || dim.msaa) {
					fail("gather requires a 2D image or 2D array");
					return;
				}
				const auto layout      = LayoutOf(mem);
				const uint32_t spatial = dim.spatial;
				const bool arrayed     = dim.components > dim.spatial;
				std::string coord =
				    CoordF32(*address, mem, static_cast<uint32_t>(layout.coord), spatial, false);
				if (layout.offset >= 0 && spatial == 2u) {
					// Packed texel offset (6-bit signed x in bits 0..5, y in bits 8..13), applied to the coordinate
					// because Metal's gather offset must be a compile-time constant.
					const std::string packed = AddressU32(*address, mem, static_cast<uint32_t>(layout.offset));
					coord = "(" + coord + " + float2(float(((int)(" + packed + " << 26u)) >> 26), float(((int)((" + packed +
					        " >> 8u) << 26u)) >> 26)) / float2(" + tex + ".get_width(), " + tex + ".get_height()))";
				}
				const std::string array_arg =
				    arrayed ? (", " + SampleArrayIndex(*address, mem,
				                                       static_cast<uint32_t>(layout.coord) + spatial, tex))
				            : std::string {};
				const std::string component(1, "xyzw"[GatherComponent(mem.dmask)]);
				std::string call = tex + ".gather(ab.samplers[" +
				                   std::to_string(*sampler_slot) + "], " + coord + array_arg +
				                   ", int2(0), component::" + component + ")";
				if ((mem.image_sample_flags & Decoder::ImageSampleFlagCompare) != 0u && layout.dref >= 0) {
					// Depth-compare gather: compare the reference with each gathered texel (LESS_EQUAL).
					static const char* s_cmp_func_g = std::getenv("KYTY_MSL_CMP_FUNC");
					const std::string slot_func_g = "sizes[" + std::to_string(64u + *sampler_slot) + "]";
					const std::string func_g = s_cmp_func_g == nullptr ? std::string("6u")
					                           : std::strcmp(s_cmp_func_g, "ref") == 0 ? "((0x73516240u >> (" + slot_func_g + " * 4u)) & 0xfu)"
					                                                                   : slot_func_g;
					call = "kyty_gather_cmp(" + call + ", " + AddressF32(*address, mem, static_cast<uint32_t>(layout.dref)) + ", " + func_g + ")";
				}
				EmitAssignment(inst, TexelBits(image, call, &mem));
				return;
			}
			case IR::ValueOpcode::ImageRead: {
				const uint32_t spatial = dim.spatial;
				const bool arrayed     = dim.components > dim.spatial;
				const std::string coord = CoordU32(*address, mem, spatial);
				const std::string array_arg =
				    arrayed ? (", uint(" + AddressU32(*address, mem, spatial) + ")") : std::string {};
				if (dim.msaa) {
					const std::string sample =
					    AddressU32(*address, mem, static_cast<uint32_t>(dim.components));
					EmitAssignment(inst, TexelBits(image, tex + ".read(" + coord + array_arg + ", " +
					                                          sample + ")", &mem));
					return;
				}
				const std::string lod =
				    image.mip_mode == IR::ImageMipMode::Dynamic
				        ? AddressU32(*address, mem, static_cast<uint32_t>(dim.components)) : "0u";
				EmitAssignment(inst, TexelBits(image,
				                               tex + ".read(" + coord + array_arg + ", " + lod + ")", &mem));
				return;
			}
			case IR::ValueOpcode::ImageWrite: {
				if (!storage) {
					fail("write to a sampled image");
					return;
				}
				const uint32_t spatial = dim.spatial;
				const bool arrayed     = dim.components > dim.spatial;
				const std::string coord = CoordU32(*address, mem, spatial);
				const std::string array_arg =
				    arrayed ? (", uint(" + AddressU32(*address, mem, spatial) + ")") : std::string {};
				const std::string lod =
				    image.mip_mode == IR::ImageMipMode::Dynamic
				        ? AddressU32(*address, mem, static_cast<uint32_t>(dim.components)) : "0u";
				const std::string value = TexelValue(image, Expr(inst.Arg(2)), &mem);
				body << "\t\tif (" << Expr(inst.Arg(3)) << ") {\n\t\t\t" << tex << ".write(" << value
				     << ", " << coord << array_arg << ", " << lod << ");\n\t\t}\n";
				return;
			}
			case IR::ValueOpcode::ImageQueryDimensions: {
				// image_get_resinfo: (width, height, depth/layers, mip count) at the requested level.
				if (dim.msaa || image.cube) {
					fail("resinfo of a multisampled or cube image");
					return;
				}
				const std::string lod = "min(" + AddressU32(*address, mem, 0) + ", (uint)" + tex + ".get_num_mip_levels() - 1u)";
				std::string h = "1u", d = "1u";
				if (dim.spatial >= 2u) h = "(uint)" + tex + ".get_height(" + lod + ")";
				if (image.dimension == Decoder::ImageDimension::Dim3D) {
					d = "(uint)" + tex + ".get_depth(" + lod + ")";
				} else if (dim.components > dim.spatial) {
					d = "(uint)" + tex + ".get_array_size()";
				}
				EmitAssignment(inst, "uint4((uint)" + tex + ".get_width(" + lod + "), " + h + ", " + d + ", (uint)" +
				                         tex + ".get_num_mip_levels())");
				return;
			}
			default: fail("image operation"); return;
		}
	}

	// Terminators name their successors by block_info id, which is not the block's position in
	// program.blocks (SPIR-V resolves it the same way). The dispatcher's pc is a position, so map
	// the id to its index here; an unknown id maps past the end and is skipped.
	[[nodiscard]] uint32_t BlockIndex(uint32_t id) const {
		for (size_t index = 0; index < program.block_info.size(); ++index) {
			if (program.block_info[index].id == id) {
				return static_cast<uint32_t>(index);
			}
		}
		return static_cast<uint32_t>(program.blocks.size());
	}

	// Copies phi operands that belong to the edge `from -> target`, for both emulated halves of a pair-packed wave.
	void EmitPhiCopies(const IR::Block* from, uint32_t target_id) {
		if (!pair_) {
			EmitPhiCopiesHalf(from, target_id);
			return;
		}
		for (half_ = 0; half_ < 2; ++half_) EmitPhiCopiesHalf(from, target_id);
		half_ = 0;
	}

	void EmitPhiCopiesHalf(const IR::Block* from, uint32_t target_id) {
		const uint32_t target = BlockIndex(target_id);
		if (target >= program.blocks.size()) {
			return;
		}
		const auto edge = next_phi_edge++;
		std::vector<std::pair<std::string, std::string>> copies;
		for (const auto& inst: program.blocks[target]->Instructions()) {
			if (inst.GetOpcode() != IR::ValueOpcode::Phi || MslType(inst.GetType()) == nullptr) {
				continue;
			}
			for (size_t index = 0; index < inst.NumArgs(); ++index) {
				if (inst.PhiBlock(index) == from) {
					// Phi assignments on an edge are simultaneous. A loop may swap
					// two values, so save every RHS before overwriting any result.
					const auto temp = "kyty_phi_" + std::to_string(edge) + "_" + Name(inst);
					body << "\t\t\tconst " << MslType(inst.GetType()) << " " << temp << " = " << Expr(inst.Arg(index)) << ";\n";
					copies.emplace_back(Name(inst), temp);
					break;
				}
			}
		}
		for (const auto& [name, temp]: copies) body << "\t\t\t" << name << " = " << temp << ";\n";
	}

	// Consume the frontend's structured CFG directly. Unsupported control-flow
	// shapes retain the dispatcher; no goto or host API translation is involved.
	bool EmitStructuredRegion(uint32_t index, uint32_t stop,
	                          std::vector<std::pair<uint32_t, uint32_t>>& loops,
	                          std::vector<bool>& seen, uint32_t depth = 0,
	                          uint32_t enter_loop = UINT32_MAX) {
		if (depth > 256) return false;
		while (index != stop) {
			if (index >= program.blocks.size()) return false;
			for (size_t i = 0; i < loops.size(); ++i) {
				if (index == loops[i].first && index != enter_loop) {
					if (i + 1 != loops.size()) return false;
					body << "\t\tcontinue;\n";
					return true;
				}
				if (index == loops[i].second) {
					if (i + 1 != loops.size()) return false;
					body << "\t\tbreak;\n";
					return true;
				}
			}
			const auto& term = program.block_info[index].terminator;
			if (term.loop_header && index != enter_loop) {
				const auto merge = BlockIndex(term.merge_block);
				if (merge >= program.blocks.size() || merge == index) return false;
				loops.emplace_back(index, merge);
				// The guard counts the iterations of this entry into the loop (reset when an enclosing loop enters it again).
				const uint32_t loop_id = next_loop++;
				body << "\tuint kyty_loop" << loop_id << " = 0u;\n";
				body << "\twhile (true) {\n";
				// A compute thread that ran out of iterations leaves the loop rather than the kernel: a `return` here left the
				// rest of the threadgroup waiting at the next threadgroup_barrier for good (a GPU hang the watchdog ends).
				if (program.stage == ShaderType::Compute && LoopTripGds()) {
					body << "\t\tif (++kyty_loop" << loop_id << " > " << MslLoopCap() << "u) { atomic_fetch_add_explicit(&ab.gds["
					     << kMslLoopTripGdsBase + std::min(loop_id, 7u) << "u], 1u, memory_order_relaxed); break; }\n";
				} else if (program.stage == ShaderType::Compute) {
					body << "\t\tif (++kyty_loop" << loop_id << " > " << MslLoopCap() << "u) { break; }\n";
				} else {
					body << "\t\tif (++kyty_loop" << loop_id << " > " << MslLoopCap() << "u) { return" << (HasOutInterface() ? " out" : "") << "; }\n";
				}
				if (!EmitStructuredRegion(index, UINT32_MAX, loops, seen, depth + 1, index)) return false;
				body << "\t}\n";
				loops.pop_back();
				index = merge;
				continue;
			}
			if (seen[index]) return false;
			seen[index] = true;
			enter_loop = UINT32_MAX;
			const auto* block = program.blocks[index];
			for (const auto& inst: block->Instructions()) {
				if (inst.GetOpcode() != ValueOpcode::Phi && !IsResourceType(inst.GetType())) EmitWaveInstruction(inst);
			}
			if (term.kind == CFG::TerminatorKind::Return) {
				body << "\t\treturn" << (HasOutInterface() ? " out" : "") << ";\n";
				return true;
			}
			if (term.kind == CFG::TerminatorKind::Branch) {
				EmitPhiCopies(block, term.true_block);
				index = BlockIndex(term.true_block);
				continue;
			}
			if (term.kind != CFG::TerminatorKind::ConditionalBranch) return false;
			const uint32_t merge = term.merge_block == UINT32_MAX ? UINT32_MAX : BlockIndex(term.merge_block);
			if (merge == index || (merge != UINT32_MAX && merge >= program.blocks.size())) return false;
			body << "\t\tif (" << Expr(program.block_info[index].condition) << ") {\n";
			EmitPhiCopies(block, term.true_block);
			if (!EmitStructuredRegion(BlockIndex(term.true_block), merge, loops, seen, depth + 1)) return false;
			body << "\t\t} else {\n";
			EmitPhiCopies(block, term.false_block);
			if (!EmitStructuredRegion(BlockIndex(term.false_block), merge, loops, seen, depth + 1)) return false;
			body << "\t\t}\n";
			if (merge == UINT32_MAX) return true;
			index = merge;
		}
		return true;
	}

	// Emits a pre-declared assignment `vN = <expr>;` for the instruction result.
	void EmitAssignment(const Inst& inst, const std::string& expr) {
		const char* type_name = MslType(inst.GetType());
		if (type_name == nullptr) {
			if (!IsResourceType(inst.GetType())) {
				Unsupported(inst, "result type");
			}
			return;
		}
		body << "\t\t" << Name(inst) << " = " << expr << ";\n";
		// A bit cast is no float result: phi values and integers pass through as_type<float> here and would read as NaN.
		const bool nan_cast_only = expr.rfind("as_type<float>(", 0) == 0;
		if (nan_trace_ && program.stage == ShaderType::Compute && std::strcmp(type_name, "float") == 0 && nan_cast_only) {
			// Not traced.
		} else if (nan_trace_ && program.stage == ShaderType::Compute && std::strcmp(type_name, "float") == 0) {
			// Compute: the smallest (id * 2 + half) over all threads goes into the debug buffer (buffer 28) with an atomic minimum; the host prints it
			// after the traced dispatch ([NanTrace]). Ids are the numbers of the [NanId] lines printed here at compile time.
			const std::string name  = Name(inst);
			const bool        h1    = name.size() > 2 && name.compare(name.size() - 2, 2, "_h") == 0;
			const std::string digits = name.substr(1, name.size() - 1 - (h1 ? 2 : 0));
			body << "\t\tif (isnan(" << name << ")) atomic_fetch_min_explicit(kyty_nan_dbg, " << digits << "u * 2u + " << (h1 ? 1 : 0) << "u, memory_order_relaxed);\n";
			std::fprintf(stderr, "[NanId] 0x%016llx %s = %s\n", static_cast<unsigned long long>(program.shader_hash), name.c_str(), expr.c_str());
		} else if (nan_trace_ && !pair_ && std::strcmp(type_name, "float") == 0) {
			const std::string name = Name(inst);
			body << "\t\tif (kyty_first_nan == 0u && isnan(" << name << ")) kyty_first_nan = " << name.substr(1) << "u;\n";
			std::fprintf(stderr, "[NanId] 0x%016llx %s = %s\n", static_cast<unsigned long long>(program.shader_hash), name.c_str(), expr.c_str());
		}
	}

	std::string Binary(const Inst& inst, const char* op) {
		return "(" + Expr(inst.Arg(0)) + " " + op + " " + Expr(inst.Arg(1)) + ")";
	}

	std::string Call(const char* fn, const Inst& inst, size_t arg_count) {
		std::string result = std::string(fn) + "(";
		for (size_t i = 0; i < arg_count; ++i) {
			if (i != 0) result += ", ";
			result += Expr(inst.Arg(i));
		}
		result += ")";
		return result;
	}

	// KYTY_MSL_RDNA_FLOAT=<mask> (default 7): 1 = flush denormal inputs of sqrt/rcp/rsq/exp2/log2, 2 = RDNA float->int conversions,
	// 4 = med3 NaN rule (the SPIR-V path's semantics, see the kyty_ftz/kyty_f2u/kyty_f2i/kyty_med3f helpers). A/B switch for comparisons.
	static uint32_t RdnaFloatMask() {
		static const uint32_t mask = [] {
			const char* value = std::getenv("KYTY_MSL_RDNA_FLOAT");
			return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 7u;
		}();
		return mask;
	}
	std::string Ftz(const std::string& expr) { return (RdnaFloatMask() & 1u) != 0 ? "kyty_ftz(" + expr + ")" : expr; }

	void EmitAlu(const Inst& inst);

	// One IR instruction for the whole wave: once per emulated half when pair-packed (the barrier once, it is a workgroup-wide
	// operation; the whole-wave operations emit both halves' results while handling half 0).
	void EmitWaveInstruction(const Inst& inst) {
		if (!pair_ || inst.GetOpcode() == IR::ValueOpcode::Barrier) {
			EmitInstruction(inst);
			return;
		}
		for (half_ = 0; half_ < 2; ++half_) EmitInstruction(inst);
		half_ = 0;
	}

	void EmitInstruction(const Inst& inst) {
		switch (inst.GetOpcode()) {
			case IR::ValueOpcode::Identity:
				EmitAssignment(inst, Expr(inst.Arg(0)));
				return;
			case IR::ValueOpcode::GetUserData:
				// Push data is bound at buffer(1); user SGPRs model that block.
				EmitAssignment(inst, "pc_data.dwords[" +
				                        std::to_string(IR::RegIndex(inst.Arg(0).ScalarRegister())) + "u]");
				return;
			case IR::ValueOpcode::GetBuiltin: EmitBuiltin(inst); return;
			case IR::ValueOpcode::GetAttribute: EmitGetAttribute(inst); return;
			case IR::ValueOpcode::SetAttribute: EmitSetAttribute(inst); return;
			case IR::ValueOpcode::ReadConst:
				if (HasDescriptor(IR::DescriptorBindingKind::FlattenedSrt)) {
					// An immediate index is a flat slot assigned by the SRT plan and always lies inside the uploaded table (FC 27 glass
					// reads slot 315: the old clamp to 255 read another constant, zero exposure and transmittance = black glass).
					// A computed index keeps a bound against stray reads. KYTY_MSL_SRT_CLAMP255=1|ps|vs|<hash,...>: the old clamp for
					// every shader, the pixel shaders, the other stages, or the listed shader hashes (A/B and bisection only).
					static const std::string s_clamp255 = std::getenv("KYTY_MSL_SRT_CLAMP255") != nullptr ? std::getenv("KYTY_MSL_SRT_CLAMP255") : "";
					const bool clamp255 = !s_clamp255.empty() &&
					    (s_clamp255 == "1" || (s_clamp255 == "ps" && program.stage == ShaderType::Pixel) ||
					     (s_clamp255 == "vs" && program.stage != ShaderType::Pixel) ||
					     s_clamp255.find(fmt::format("{:016x}", program.shader_hash)) != std::string::npos);
					if (clamp255) {
						EmitAssignment(inst, "ab.flattened_srt[min(" + Expr(inst.Arg(1)) + ", 255u)]");
					} else if (inst.Arg(1).IsImmediate()) {
						EmitAssignment(inst, "ab.flattened_srt[" + Expr(inst.Arg(1)) + "]");
					} else {
						EmitAssignment(inst, "ab.flattened_srt[min(" + Expr(inst.Arg(1)) + ", 4095u)]");
					}
				} else {
					Unsupported(inst, "SRT read without flattened SRT binding");
					EmitAssignment(inst, "0u");
				}
				return;
			case IR::ValueOpcode::CompositeConstructU32x2:
				EmitAssignment(inst, "uint2(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
				return;
			case IR::ValueOpcode::CompositeConstructU32x3:
				EmitAssignment(inst, "uint3(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ", " +
				                        Expr(inst.Arg(2)) + ")");
				return;
			case IR::ValueOpcode::CompositeConstructU32x4:
				EmitAssignment(inst, "uint4(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ", " +
				                        Expr(inst.Arg(2)) + ", " + Expr(inst.Arg(3)) + ")");
				return;
			case IR::ValueOpcode::CompositeConstructF32x2:
				EmitAssignment(inst, "float2(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
				return;
			case IR::ValueOpcode::CompositeConstructU64:
				EmitAssignment(inst, "(ulong(" + Expr(inst.Arg(0)) + ") | (ulong(" +
				                        Expr(inst.Arg(1)) + ") << 32))");
				return;
			case IR::ValueOpcode::CompositeExtractU32x2:
			case IR::ValueOpcode::CompositeExtractU32x3:
			case IR::ValueOpcode::CompositeExtractU32x4: {
				const auto component = ImmU32(inst.Arg(1));
				if (!component) {
					Unsupported(inst, "non-immediate component");
					EmitAssignment(inst, "0u");
					return;
				}
				EmitAssignment(inst, Expr(inst.Arg(0)) + "." + ComponentName(*component));
				return;
			}
			case IR::ValueOpcode::LaneId:
			case IR::ValueOpcode::Ballot:
			case IR::ValueOpcode::ReadLane:
			case IR::ValueOpcode::WriteLane:
			case IR::ValueOpcode::ReadFirstLane:
			case IR::ValueOpcode::DppMoveU32:
			case IR::ValueOpcode::DppUpdateU32:
			case IR::ValueOpcode::BpermuteU32:
			case IR::ValueOpcode::Permlane16U32:
			case IR::ValueOpcode::SwizzleU32:
				EmitWave(inst);
				return;
			case IR::ValueOpcode::ImageSampleRaw:
			case IR::ValueOpcode::ImageGatherRaw:
			case IR::ValueOpcode::ImageRead:
			case IR::ValueOpcode::ImageWrite:
			case IR::ValueOpcode::ImageQueryDimensions:
			case IR::ValueOpcode::ImageQueryLod:
				EmitImage(inst);
				return;
			case IR::ValueOpcode::CompositeExtractU64: {
				const auto component = ImmU32(inst.Arg(1));
				if (!component) {
					Unsupported(inst, "non-immediate component");
					EmitAssignment(inst, "0u");
					return;
				}
				EmitAssignment(inst, "uint(" + Expr(inst.Arg(0)) + " >> " +
				                        std::to_string(*component * 32u) + "u)");
				return;
			}
			case IR::ValueOpcode::Reference:
			case IR::ValueOpcode::ReferenceU32:
				// Translation placeholders, no runtime value.
				return;
			case IR::ValueOpcode::ConditionRef: {
				// A guest EXECZ/VCCZ branch tests the whole wave mask, so it is one decision for both emulated halves: ballot the
				// per-lane bit (set where the lane's condition holds) over the two halves, like the SPIR-V backend does.
				const auto kind = inst.Flags<CFG::BranchCondition>();
				if (pair_ && kind != CFG::BranchCondition::ScalarInstruction) {
					if (half_ == 0) {
						const bool zero = kind == CFG::BranchCondition::ExecZero || kind == CFG::BranchCondition::VccZero ||
						                  kind == CFG::BranchCondition::SccZero;
						body << "\t\t{\n\t\t\tconst uint4 kyty_cb = kyty_ballot2(" << ExprHalf(inst.Arg(0), 0) << ", "
						     << ExprHalf(inst.Arg(0), 1) << ");\n"
						     << "\t\t\t" << HalfName(inst, 0) << " = "
						     << (zero ? "((kyty_cb.x & kyty_cb.y) == 0xffffffffu)" : "((kyty_cb.x | kyty_cb.y) != 0u)") << ";\n"
						     << "\t\t\t" << HalfName(inst, 1) << " = " << HalfName(inst, 0) << ";\n\t\t}\n";
					}
					return;
				}
				EmitAssignment(inst, Expr(inst.Arg(0)));
				return;
			}
			case IR::ValueOpcode::BitFieldUExtract:
				EmitAssignment(inst, "(" + Expr(inst.Arg(2)) + " == 0u ? 0u : ((" +
				                        Expr(inst.Arg(0)) + " >> " + Expr(inst.Arg(1)) +
				                        ") & (0xffffffffu >> (32u - " + Expr(inst.Arg(2)) + "))))");
				return;
			case IR::ValueOpcode::BitFieldSExtract:
				EmitAssignment(inst, "(" + Expr(inst.Arg(2)) + " == 0u ? 0u : as_type<uint>((int)(" +
				                        Expr(inst.Arg(0)) + " << (32u - " + Expr(inst.Arg(1)) +
				                        " - " + Expr(inst.Arg(2)) + ")) >> (32u - " +
				                        Expr(inst.Arg(2)) + ")))");
				return;
			case IR::ValueOpcode::BitFieldInsert: {
				const std::string base   = Expr(inst.Arg(0));
				const std::string insert = Expr(inst.Arg(1));
				const std::string offset = Expr(inst.Arg(2));
				const std::string count  = Expr(inst.Arg(3));
				body << "\t\t{\n\t\t\tconst uint kyty_mask = " << count
				     << " == 0u ? 0u : ((0xffffffffu >> (32u - " << count << ")) << " << offset
				     << ");\n\t\t\t" << Name(inst) << " = (" << base << " & ~kyty_mask) | (("
				     << insert << " << " << offset << ") & kyty_mask);\n\t\t}\n";
				return;
			}
			case IR::ValueOpcode::IAddCarry32: {
				const std::string a = Expr(inst.Arg(0));
				const std::string b = Expr(inst.Arg(1));
				body << "\t\t{\n\t\t\tconst uint kyty_sum = " << a << " + " << b
				     << ";\n\t\t\t" << Name(inst) << " = uint2(kyty_sum, kyty_sum < " << a
				     << " ? 1u : 0u);\n\t\t}\n";
				return;
			}
			// Naming follows the IR/SPIR-V backend: ConvertF16F32 narrows an F32 to F16, ConvertF32F16 widens an
			// F16 to F32. (They were swapped here, which turned float colours into half-bit garbage.)
			case IR::ValueOpcode::ConvertF16F32:
				EmitAssignment(inst, "half(" + Expr(inst.Arg(0)) + ")");
				return;
			case IR::ValueOpcode::ConvertF32F16: {
				// Operands may carry the overlapping U16 type (raw half bits) instead of F16.
				const bool half_input = inst.Arg(0).GetType() == IR::Type::F16;
				EmitAssignment(inst, half_input ? "float(" + Expr(inst.Arg(0)) + ")"
				                                : "float(as_type<half>((ushort)(" +
				                                      Expr(inst.Arg(0)) + ")))");
				return;
			}
			// Convert<Dst><Src> naming (as in the translator): BitCastF16U16 reinterprets raw 16 bits as a half,
			// BitCastU16F16 returns a half's raw bits. These two were swapped, so a numeric uint->half conversion
			// replaced the reinterpretation.
			case IR::ValueOpcode::BitCastF16U16: {
				const bool half_input = inst.Arg(0).GetType() == IR::Type::F16;
				EmitAssignment(inst, half_input
				                        ? Expr(inst.Arg(0))
				                        : "as_type<half>((ushort)(" + Expr(inst.Arg(0)) + "))");
				return;
			}
			case IR::ValueOpcode::BitCastU16F16: {
				const bool half_input = inst.Arg(0).GetType() == IR::Type::F16;
				EmitAssignment(inst, half_input
				                        ? "uint(as_type<ushort>(" + Expr(inst.Arg(0)) + "))"
				                        : "(uint)(" + Expr(inst.Arg(0)) + " & 0xffffu)");
				return;
			}
			case IR::ValueOpcode::PackHalf2x16:
				EmitAssignment(inst, "as_type<uint>(half2(" + Expr(inst.Arg(0)) + ".x, " +
				                        Expr(inst.Arg(0)) + ".y))");
				return;
			case IR::ValueOpcode::PackFloat2x16Rtz:
				// Round toward zero (kyty_f16_rtz), not half(): see the preamble.
				EmitAssignment(inst, "(kyty_f16_rtz(" + Expr(inst.Arg(0)) + ") | (kyty_f16_rtz(" +
				                        Expr(inst.Arg(1)) + ") << 16))");
				return;
			case IR::ValueOpcode::PackUnorm2x16:
				EmitAssignment(inst, "pack_float_to_unorm2x16(float2(" + Expr(inst.Arg(0)) +
				                        ".x, " + Expr(inst.Arg(0)) + ".y))");
				return;
			case IR::ValueOpcode::PackSnorm2x16:
				EmitAssignment(inst, "pack_float_to_snorm2x16(float2(" + Expr(inst.Arg(0)) +
				                        ".x, " + Expr(inst.Arg(0)) + ".y))");
				return;
			case IR::ValueOpcode::ReadConstBuffer: {
				const auto& mem = Mem(inst);
				if (mem.planning_only) return;
				const auto ptr  = BufferPtr(mem.resource);
				if (!ptr) {
					Unsupported(inst, "SRT buffer resource is not bound");
					EmitAssignment(inst, "0u");
					return;
				}
				// Robust access like LoadBuffer: an out-of-range dword reads 0 instead of touching memory
				// past the bound buffer (the index is data-dependent, e.g. a 16-bit table lookup).
				const std::string idx = "((" + Expr(inst.Arg(1)) + " >> 2) + " + U32Literal(mem.offset >> 2) + ")";
				EmitAssignment(inst, "(" + idx + " < (sizes[" + std::to_string(mem.resource) + "] >> 2) ? " + *ptr +
				                        "[" + idx + "] : 0u)");
				return;
			}
			case IR::ValueOpcode::LoadAddressU8:
			case IR::ValueOpcode::LoadAddressU16:
			case IR::ValueOpcode::LoadAddressU32: {
				// ResourceTracking evaluates descriptor-chain reads on the CPU and
				// replaces their shader consumers with materialized argument slots.
				if (Mem(inst).planning_only) return;
				const auto& mem = Mem(inst);
				if (!HasDescriptor(IR::DescriptorBindingKind::BdaPagetable) ||
				    (mem.kind != IR::ResourceKind::ScalarAddress && mem.kind != IR::ResourceKind::Flat &&
				     mem.kind != IR::ResourceKind::Global)) {
					Unsupported(inst, "guest address space is not bound");
					return;
				}
				const uint32_t bytes = inst.GetOpcode() == ValueOpcode::LoadAddressU8 ? 1u
				                       : inst.GetOpcode() == ValueOpcode::LoadAddressU16 ? 2u : 4u;
				const auto load = "kyty_load_guest(ab.bda_root, " + GuestAddress(inst, mem) + ", " + std::to_string(bytes) + "u)";
				EmitAssignment(inst, mem.kind == IR::ResourceKind::ScalarAddress ? load
				    : "(" + Expr(inst.Arg(3)) + " ? " + load + " : 0u)");
				return;
			}
			case IR::ValueOpcode::LoadBufferU8:
			case IR::ValueOpcode::LoadBufferU16: {
				const auto& mem = Mem(inst);
				const auto ptr = BufferPtr(mem.resource);
				if (!ptr || mem.kind != IR::ResourceKind::Buffer || !BufferAddressable(mem.resource) || mem.formatted) {
					Unsupported(inst, "subword buffer load layout");
					EmitAssignment(inst, "0u");
					return;
				}
				const uint32_t bytes = inst.GetOpcode() == IR::ValueOpcode::LoadBufferU8 ? 1u : 2u;
				const auto offset = BufferByte(inst, mem, program.info.buffers[mem.resource].packed_stride);
				const auto limit = "sizes[" + std::to_string(mem.resource) + "]";
				const std::string pointer = "((device const uchar*)" + *ptr + ")";
				// Byte reads also handle unaligned addresses and a final halfword at
				// the buffer boundary, without loading a dword beyond the descriptor.
				std::string value = "uint(" + pointer + "[" + offset + "])";
				if (bytes == 2) value = "(" + value + " | (uint(" + pointer + "[" + offset + " + 1u]) << 8u))";
				EmitAssignment(inst, "(" + offset + " <= " + limit + " && " + U32Literal(bytes) +
				                        " <= " + limit + " - " + offset + " ? " + value + " : 0u)");
				return;
			}
			case IR::ValueOpcode::LoadBufferU32:
			case IR::ValueOpcode::LoadBufferU32x2:
			case IR::ValueOpcode::LoadBufferU32x3:
			case IR::ValueOpcode::LoadBufferU32x4: {
				const auto& mem = Mem(inst);
				const auto  ptr = BufferPtr(mem.resource);
				if (!ptr || mem.kind != IR::ResourceKind::Buffer ||
				    !BufferAddressable(mem.resource)) {
					Unsupported(inst, "swizzled buffer load");
					EmitAssignment(inst, "0");
					return;
				}
				const uint32_t packed = program.info.buffers[mem.resource].packed_stride;
				if (mem.formatted) {
					const auto format =
					    mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
					              : program.info.buffers[mem.resource].descriptor_format;
					const auto format_info = Format::GetFormatInfo(format);
					if (format_info.type == Format::ComponentType::Unknown) {
						Unsupported(inst, "unknown buffer format");
						EmitAssignment(inst, "0");
						return;
					}
					const uint32_t count =
					    inst.GetOpcode() == IR::ValueOpcode::LoadBufferU32     ? 1u
					    : inst.GetOpcode() == IR::ValueOpcode::LoadBufferU32x2 ? 2u
					    : inst.GetOpcode() == IR::ValueOpcode::LoadBufferU32x3 ? 3u
					                                                           : 4u;
					std::string values[4];
					for (uint32_t index = 0; index < 4; ++index) {
						values[index] =
						    index < count
						        ? FormattedComponentExpr(inst, mem, format_info, index, *ptr, packed)
						        : "0u";
					}
					std::string result;
					switch (count) {
						case 1u: result = values[0]; break;
						case 2u: result = "uint2(" + values[0] + ", " + values[1] + ")"; break;
						case 3u:
							result = "uint3(" + values[0] + ", " + values[1] + ", " + values[2] + ")";
							break;
						default:
							result = "uint4(" + values[0] + ", " + values[1] + ", " + values[2] +
							         ", " + values[3] + ")";
							break;
					}
					EmitAssignment(inst, result);
					return;
				}
				const std::string d = BufferDword(inst, mem, packed);
				const std::string n = "(sizes[" + std::to_string(mem.resource) + "] >> 2)";
				const uint32_t count = inst.GetOpcode() == ValueOpcode::LoadBufferU32 ? 1u
				    : inst.GetOpcode() == ValueOpcode::LoadBufferU32x2 ? 2u
				    : inst.GetOpcode() == ValueOpcode::LoadBufferU32x3 ? 3u : 4u;
				std::string result = count == 1 ? "" : "uint" + std::to_string(count) + "(";
				for (uint32_t i = 0; i < count; ++i) {
					if (i != 0) result += ", ";
					const auto component = std::to_string(i) + "u";
					result += "(" + Expr(inst.Arg(inst.NumArgs() - 1)) + " && " + d + " < " + n + " && " + component + " < " + n + " - " + d +
					          " ? " + *ptr + "[" + d + " + " + component + "] : 0u)";
				}
				if (count > 1) result += ")";
				EmitAssignment(inst, result);
				return;
			}
			case IR::ValueOpcode::BufferAtomicSwap32:
			case IR::ValueOpcode::BufferAtomicCmpSwap32:
			case IR::ValueOpcode::BufferAtomicIAdd32:
			case IR::ValueOpcode::BufferAtomicISub32:
			case IR::ValueOpcode::BufferAtomicAnd32:
			case IR::ValueOpcode::BufferAtomicOr32:
			case IR::ValueOpcode::BufferAtomicXor32:
			case IR::ValueOpcode::BufferAtomicUMin32:
			case IR::ValueOpcode::BufferAtomicUMax32:
			case IR::ValueOpcode::BufferAtomicSMin32:
			case IR::ValueOpcode::BufferAtomicSMax32: {
				// 32-bit device atomics on a storage buffer: the result is the old value. Lanes whose
				// condition is false or whose index is past the buffer end contribute 0 and do not write.
				// Operands follow the SPIR-V EmitAtomicOperation convention: value at NumArgs-2 (CmpSwap:
				// desired at NumArgs-3, comparator at NumArgs-2), condition last.
				const auto& mem = Mem(inst);
				const auto  ptr = BufferPtr(mem.resource);
				if (!ptr || mem.kind != IR::ResourceKind::Buffer || !BufferAddressable(mem.resource) ||
				    mem.formatted) {
					Unsupported(inst, "buffer atomic");
					return;
				}
				const auto op = inst.GetOpcode();
				const std::string d = BufferDword(inst, mem, program.info.buffers[mem.resource].packed_stride);
				const std::string cond = Expr(inst.Arg(inst.NumArgs() - 1));
				const std::string n    = "(sizes[" + std::to_string(mem.resource) + "] >> 2)";
				const bool signed_op = op == IR::ValueOpcode::BufferAtomicSMin32 || op == IR::ValueOpcode::BufferAtomicSMax32;
				const std::string elem = "&" + *ptr + "[" + d + "]";
				body << "\t\t" << Name(inst) << " = 0u;\n";
				body << "\t\tif (" << cond << " && " << d << " < " << n << ") {\n";
				if (op == IR::ValueOpcode::BufferAtomicCmpSwap32) {
					const std::string desired    = Expr(inst.Arg(inst.NumArgs() - 3));
					const std::string comparator = Expr(inst.Arg(inst.NumArgs() - 2));
					body << "\t\t\t{\n\t\t\t\tuint kyty_exp = " << comparator << ";\n"
					     << "\t\t\t\tatomic_compare_exchange_weak_explicit((device atomic_uint*)" << elem
					     << ", &kyty_exp, " << desired << ", memory_order_relaxed, memory_order_relaxed);\n"
					     << "\t\t\t\t" << Name(inst) << " = kyty_exp;\n\t\t\t}\n";
				} else {
					const std::string value = Expr(inst.Arg(inst.NumArgs() - 2));
					const char* fn = "atomic_fetch_add_explicit";
					switch (op) {
						case IR::ValueOpcode::BufferAtomicSwap32: fn = "atomic_exchange_explicit"; break;
						case IR::ValueOpcode::BufferAtomicISub32: fn = "atomic_fetch_sub_explicit"; break;
						case IR::ValueOpcode::BufferAtomicAnd32: fn = "atomic_fetch_and_explicit"; break;
						case IR::ValueOpcode::BufferAtomicOr32: fn = "atomic_fetch_or_explicit"; break;
						case IR::ValueOpcode::BufferAtomicXor32: fn = "atomic_fetch_xor_explicit"; break;
						case IR::ValueOpcode::BufferAtomicUMin32:
						case IR::ValueOpcode::BufferAtomicSMin32: fn = "atomic_fetch_min_explicit"; break;
						case IR::ValueOpcode::BufferAtomicUMax32:
						case IR::ValueOpcode::BufferAtomicSMax32: fn = "atomic_fetch_max_explicit"; break;
						default: break;
					}
					const std::string ptr_type = signed_op ? "device atomic_int*" : "device atomic_uint*";
					const std::string arg = signed_op ? "(int)" + value : value;
					body << "\t\t\t" << Name(inst) << " = " << (signed_op ? "as_type<uint>(" : "(") << fn << "((" << ptr_type
					     << ")" << elem << ", " << arg << ", memory_order_relaxed));\n";
				}
				body << "\t\t}\n";
				return;
			}
			case IR::ValueOpcode::StoreBufferU32:
			case IR::ValueOpcode::StoreBufferU32x2:
			case IR::ValueOpcode::StoreBufferU32x3:
			case IR::ValueOpcode::StoreBufferU32x4: {
				const auto& mem = Mem(inst);
				const auto  ptr = BufferPtr(mem.resource);
				if (mem.formatted && ptr && mem.kind == IR::ResourceKind::Buffer && BufferAddressable(mem.resource) &&
				    EmitFormattedStore(inst, mem, *ptr, program.info.buffers[mem.resource].packed_stride)) return;
				// Untyped formatted store with a 32-bit descriptor format (UInt/SInt/Float): no conversion, so
				// it is a plain dword store (same as the SPIR-V path). Other formatted stores stay unsupported.
				const auto store_op = inst.GetOpcode();
				// A descriptor without a data format (data_format 0) converts nothing either: an untyped dword store goes to memory as it is. Kernel
				// b8b4a7c7, skipped for this, ran 15 times a frame with no effect.
				const bool no_data_format = mem.resource < program.info.buffers.size() && mem.data_format == 0 && mem.number_format == 0 &&
				    static_cast<uint32_t>(program.info.buffers[mem.resource].descriptor_format) == 0u;
				const bool raw_formatted_dword = mem.formatted && !mem.typed &&
				    (store_op == IR::ValueOpcode::StoreBufferU32 || store_op == IR::ValueOpcode::StoreBufferU32x2 ||
				     store_op == IR::ValueOpcode::StoreBufferU32x3 || store_op == IR::ValueOpcode::StoreBufferU32x4) &&
				    mem.resource < program.info.buffers.size() &&
				    (IsRaw32BitFormat(program.info.buffers[mem.resource].descriptor_format) || no_data_format);
				const bool half4_store = mem.formatted && !mem.typed && store_op == IR::ValueOpcode::StoreBufferU32x4 &&
				    mem.resource < program.info.buffers.size() &&
				    program.info.buffers[mem.resource].descriptor_format == Prospero::BufferFormat::k16_16_16_16Float;
				if (!ptr || mem.kind != IR::ResourceKind::Buffer ||
				    !BufferAddressable(mem.resource) || (mem.formatted && !raw_formatted_dword && !half4_store)) {
					const std::string reason = "formatted/swizzled buffer store (data_format=" + std::to_string(mem.data_format) +
					                           " number_format=" + std::to_string(mem.number_format) + " typed=" + std::to_string(mem.typed) +
					                           " desc_format=" + std::to_string(mem.resource < program.info.buffers.size() ?
					                               static_cast<uint32_t>(program.info.buffers[mem.resource].descriptor_format) : 9999u) + ")";
					Unsupported(inst, reason.c_str());
					return;
				}
				const std::string d = BufferDword(inst, mem, program.info.buffers[mem.resource].packed_stride);
				const std::string n = "(sizes[" + std::to_string(mem.resource) + "] >> 2)";
				const std::string value = Expr(inst.Arg(inst.NumArgs() - 2));
				const uint32_t count = half4_store ? 2u : store_op == ValueOpcode::StoreBufferU32 ? 1u
				    : store_op == ValueOpcode::StoreBufferU32x2 ? 2u : store_op == ValueOpcode::StoreBufferU32x3 ? 3u : 4u;
				body << "\t\tif (" << Expr(inst.Arg(inst.NumArgs() - 1)) << ") {\n";
				for (uint32_t i = 0; i < count; ++i) {
					std::string component = value;
					if (half4_store) {
						component = "as_type<uint>(half2(as_type<float>(" + value + "." + ComponentName(i * 2) +
						    "), as_type<float>(" + value + "." + ComponentName(i * 2 + 1) + ")))";
					} else if (count > 1) component += "." + std::string(ComponentName(i));
					body << "\t\t\tif (" << d << " < " << n << " && " << i << "u < " << n << " - " << d << ") "
					     << *ptr << "[" << d << " + " << i << "u] = " << component << ";\n";
				}
				body << "\t\t}\n";
				return;
			}
			case IR::ValueOpcode::SharedAtomicSwap32:
			case IR::ValueOpcode::SharedAtomicIAdd32:
			case IR::ValueOpcode::SharedAtomicISub32:
			case IR::ValueOpcode::SharedAtomicAnd32:
			case IR::ValueOpcode::SharedAtomicOr32:
			case IR::ValueOpcode::SharedAtomicXor32:
			case IR::ValueOpcode::SharedAtomicUMin32:
			case IR::ValueOpcode::SharedAtomicUMax32:
			case IR::ValueOpcode::SharedAtomicSMin32:
			case IR::ValueOpcode::SharedAtomicSMax32:
			case IR::ValueOpcode::SharedAtomicInc32:
			case IR::ValueOpcode::SharedAtomicDec32: {
				// LDS atomic: the result is the old dword. Lanes with a false condition or an index past the LDS
				// array contribute 0 and do not write (same guard as the SPIR-V EmitAtomicAccess path).
				const auto& mem   = Mem(inst);
				const auto  op    = inst.GetOpcode();
				const bool  compute = program.stage == ShaderType::Compute;
				const bool  gds   = GdsMem(mem);
				const std::string limit = std::to_string(gds ? kMslGdsDwords : LdsDwords()) + "u";
				const std::string d     = "(" + LdsByte(inst, mem) + " >> 2)";
				const std::string value = Expr(inst.Arg(inst.NumArgs() - 2));
				const std::string cond  = Expr(inst.Arg(inst.NumArgs() - 1));
				const std::string space = gds ? "device" : "threadgroup";
				const std::string slot  = gds ? "ab.gds[" + d + "]" : "kyty_lds[" + d + "]";
				body << "\t\t" << Name(inst) << " = 0u;\n";
				body << "\t\tif (" << cond << " && " << d << " < " << limit << ") {\n";
				if (!compute) {
					// Non-kernel stages use a per-thread array: plain read-modify-write, no atomics.
					body << "\t\t\t{\n\t\t\t\tconst uint kyty_old = kyty_lds[" << d << "];\n"
					     << "\t\t\t\tkyty_lds[" << d << "] = " << SharedAtomicNew(op, "kyty_old", value) << ";\n"
					     << "\t\t\t\t" << Name(inst) << " = kyty_old;\n\t\t\t}\n";
				} else if (op == IR::ValueOpcode::SharedAtomicInc32 || op == IR::ValueOpcode::SharedAtomicDec32) {
					// No native MSL atomic for the wrapping inc/dec: CAS loop on the threadgroup dword.
					body << "\t\t\t{\n\t\t\t\t" << space << " atomic_uint* kyty_p = (" << space << " atomic_uint*)&" << slot << ";\n"
					     << "\t\t\t\tuint kyty_old = atomic_load_explicit(kyty_p, memory_order_relaxed);\n"
					     << "\t\t\t\twhile (!atomic_compare_exchange_weak_explicit(kyty_p, &kyty_old, "
					     << SharedAtomicNew(op, "kyty_old", value) << ", memory_order_relaxed, memory_order_relaxed)) {}\n"
					     << "\t\t\t\t" << Name(inst) << " = kyty_old;\n\t\t\t}\n";
				} else {
					std::string call;
					const char* fn = "atomic_fetch_add_explicit";
					std::string arg = value;
					std::string ptr_type = space + " atomic_uint*";
					switch (op) {
						case IR::ValueOpcode::SharedAtomicSwap32: fn = "atomic_exchange_explicit"; break;
						case IR::ValueOpcode::SharedAtomicIAdd32: fn = "atomic_fetch_add_explicit"; break;
						case IR::ValueOpcode::SharedAtomicISub32: fn = "atomic_fetch_sub_explicit"; break;
						case IR::ValueOpcode::SharedAtomicAnd32: fn = "atomic_fetch_and_explicit"; break;
						case IR::ValueOpcode::SharedAtomicOr32: fn = "atomic_fetch_or_explicit"; break;
						case IR::ValueOpcode::SharedAtomicXor32: fn = "atomic_fetch_xor_explicit"; break;
						case IR::ValueOpcode::SharedAtomicUMin32: fn = "atomic_fetch_min_explicit"; break;
						case IR::ValueOpcode::SharedAtomicUMax32: fn = "atomic_fetch_max_explicit"; break;
						case IR::ValueOpcode::SharedAtomicSMin32:
							fn = "atomic_fetch_min_explicit"; ptr_type = space + " atomic_int*";
							arg = "(int)" + value;
							break;
						default:
							fn = "atomic_fetch_max_explicit"; ptr_type = space + " atomic_int*";
							arg = "(int)" + value;
							break;
					}
					call = std::string(fn) + "((" + ptr_type + ")&" + slot + ", " + arg + ", memory_order_relaxed)";
					const bool signed_result = ptr_type == space + " atomic_int*";
					body << "\t\t\t" << Name(inst) << " = " << (signed_result ? "as_type<uint>(" : "(")
					     << call << ");\n";
				}
				body << "\t\t}\n";
				return;
			}
			case IR::ValueOpcode::DataAppend:
			case IR::ValueOpcode::DataConsume: {
				// ds_append / ds_consume: one counter update per wave. The lowest active lane adds (or subtracts) the number of
				// active lanes to the counter dword and every lane receives the old value (as in the SPIR-V lowering).
				if (pair_ && half_ == 1) return; // half 0 emits the whole wave's update and both results
				const auto& mem = Mem(inst);
				const bool append = inst.GetOpcode() == IR::ValueOpcode::DataAppend;
				const bool gds = mem.kind == IR::ResourceKind::Gds;
				// Global GDS remains experimental. Local LDS counters do not need a
				// device buffer and must not be disabled by the unrelated GDS gate.
				const bool s_gds_on = MslGdsEnabled();
				if ((gds && !s_gds_on) || program.stage != ShaderType::Compute || (!gds && mem.kind != IR::ResourceKind::Lds)) {
					Unsupported(inst, gds && !s_gds_on ? "GDS counter disabled (KYTY_MSL_GDS=1 opts in)" :
					                                  "DataAppend/Consume outside compute GDS/LDS");
					body << "\t\t" << Name(inst) << " = 0u;\n";
					return;
				}
				const std::string off = U32Literal(mem.offset & 0xfffcu);
				body << "\t\t{\n";
				if (gds) {
					body << "\t\t\tconst uint kyty_m0 = " << Expr(inst.Arg(0)) << ";\n"
					     << "\t\t\tconst uint kyty_base = kyty_m0 >> 16u;\n"
					     << "\t\t\tconst uint kyty_size = kyty_m0 & 0xffffu;\n"
					     << "\t\t\tconst uint kyty_idx = (kyty_base + " << off << ") >> 2;\n"
					     << "\t\t\tconst bool kyty_ok = (" << off << " + 3u) < kyty_size && (kyty_base + kyty_size) <= 0xc000u;\n";
				} else {
					body << "\t\t\tconst uint kyty_idx = " << off << " >> 2;\n"
					     << "\t\t\tconst bool kyty_ok = kyty_idx < " << LdsDwords() << "u;\n";
				}
				const std::string ballot_expr = pair_ ? "kyty_ballot2(" + ExprHalf(inst.Arg(1), 0) + ", " + ExprHalf(inst.Arg(1), 1) + ")"
				                                      : "kyty_ballot(" + Expr(inst.Arg(1)) + ")";
				body << "\t\t\tconst uint4 kyty_b = " << ballot_expr << ";\n"
				     << "\t\t\tconst uint kyty_cnt = popcount(kyty_b.x) + popcount(kyty_b.y);\n"
				     << "\t\t\tconst uint kyty_first = kyty_b.x != 0u ? ctz(kyty_b.x) : 32u + ctz(kyty_b.y);\n"
				     << "\t\t\tuint kyty_old = 0u;\n"
				     << "\t\t\tif ((lane & 31u) == (kyty_first & 31u) && kyty_cnt != 0u && kyty_ok) {\n"
				     << "\t\t\t\tkyty_old = atomic_fetch_" << (append ? "add" : "sub") << "_explicit("
				     << (gds ? "&ab.gds[kyty_idx]" : "(threadgroup atomic_uint*)&kyty_lds[kyty_idx]")
				     << ", kyty_cnt, memory_order_relaxed);\n"
				     << "\t\t\t}\n"
				     << "\t\t\t" << Name(inst) << " = simd_shuffle(kyty_old, kyty_first & 31u);\n";
				if (pair_) body << "\t\t\t" << HalfName(inst, 1) << " = " << HalfName(inst, 0) << ";\n";
				body << "\t\t}\n";
				return;
			}
			case IR::ValueOpcode::LoadSharedU8:
			case IR::ValueOpcode::LoadSharedU16:
			case IR::ValueOpcode::LoadSharedU32:
			case IR::ValueOpcode::LoadSharedU32x2:
			case IR::ValueOpcode::LoadSharedU32x3:
			case IR::ValueOpcode::LoadSharedU32x4: {
				const auto& mem  = Mem(inst);
				const auto  byte = LdsByte(inst, mem);
				const auto  d    = "(" + byte + " >> 2)";
				const auto count = IR::SharedComponentCount(inst.GetOpcode());
				const bool gds   = GdsMem(mem);
				const auto n = std::to_string(gds ? kMslGdsDwords : LdsDwords()) + "u";
				std::string result = count == 1 ? "" : "uint" + std::to_string(count) + "(";
				for (uint32_t i=0;i<count;++i) {
					if (i) result += ", ";
					std::string value = gds ? "atomic_load_explicit(&ab.gds[" + d + " + " + std::to_string(i) + "u], memory_order_relaxed)"
					                        : "kyty_lds[" + d + " + " + std::to_string(i) + "u]";
					if (inst.GetOpcode()==ValueOpcode::LoadSharedU8) value="(("+value+" >> (("+byte+" & 3u)*8u)) & 0xffu)";
					if (inst.GetOpcode()==ValueOpcode::LoadSharedU16) value="(("+value+" >> (("+byte+" & 2u)*8u)) & 0xffffu)";
					result += "(" + Expr(inst.Arg(inst.NumArgs()-1)) + " && " + d + " < " + n + " && " + std::to_string(i) +
					    "u < " + n + " - " + d + " ? " + value + " : 0u)";
				}
				if (count>1) result+=")";
				// KYTY_LDS_SYNC=1 (experiment): a wave64 workgroup is two 32-wide host simdgroups that do not run in lockstep, so LDS
				// handed from one half to the other needs a barrier the RDNA code (one wave, no s_barrier) never had.
				if (LdsSyncEnabled() && program.stage == ShaderType::Compute) body << "\t\tthreadgroup_barrier(mem_flags::mem_threadgroup);\n";
				EmitAssignment(inst,result);
				return;
			}
			case IR::ValueOpcode::WriteSharedU8:
			case IR::ValueOpcode::WriteSharedU16:
			case IR::ValueOpcode::WriteSharedU32:
			case IR::ValueOpcode::WriteSharedU32x2:
			case IR::ValueOpcode::WriteSharedU32x3:
			case IR::ValueOpcode::WriteSharedU32x4: {
				const auto& mem   = Mem(inst);
				const auto  byte  = LdsByte(inst, mem);
				const auto  d     = "(" + byte + " >> 2)";
				const auto  count = IR::SharedComponentCount(inst.GetOpcode());
				if (GdsMem(mem)) {
					// Sub-dword GDS writes clear and then set their bits (two atomics; GDS bytes are not written concurrently).
					const auto op = inst.GetOpcode();
					body << "\t\tif (" << Expr(inst.Arg(inst.NumArgs()-1)) << " && " << d << " < " << kMslGdsDwords << "u) {\n";
					if (op == IR::ValueOpcode::WriteSharedU8 || op == IR::ValueOpcode::WriteSharedU16) {
						const char* mask = op == IR::ValueOpcode::WriteSharedU8 ? "0xffu" : "0xffffu";
						body << "\t\t\tconst uint kyty_shift = (" << byte << " & " << (op == IR::ValueOpcode::WriteSharedU8 ? "3u" : "2u")
						     << ") * 8u;\n"
						     << "\t\t\tatomic_fetch_and_explicit(&ab.gds[" << d << "], ~(" << mask << " << kyty_shift), memory_order_relaxed);\n"
						     << "\t\t\tatomic_fetch_or_explicit(&ab.gds[" << d << "], (" << Expr(inst.Arg(1)) << " & " << mask
						     << ") << kyty_shift, memory_order_relaxed);\n";
					} else {
						for (uint32_t i = 0; i < count; ++i) {
							body << "\t\t\tif (" << i << "u < " << kMslGdsDwords << "u - " << d << ") atomic_store_explicit(&ab.gds[" << d
							     << " + " << i << "u], " << Expr(inst.Arg(1 + i)) << ", memory_order_relaxed);\n";
						}
					}
					body << "\t\t}\n";
					return;
				}
				body << "\t\tif (" << Expr(inst.Arg(inst.NumArgs()-1)) << " && " << d << " < " << LdsDwords() << "u) {\n";
				if (inst.GetOpcode() == IR::ValueOpcode::WriteSharedU8) {
					body << "\t\t\tconst uint kyty_shift = (" << byte << " & 3u) * 8u;\n"
					     << "\t\t\tkyty_lds[" << d << "] = (kyty_lds[" << d
					     << "] & ~(0xffu << kyty_shift)) | ((" << Expr(inst.Arg(1))
					     << " & 0xffu) << kyty_shift);\n";
				} else if (inst.GetOpcode() == IR::ValueOpcode::WriteSharedU16) {
					body << "\t\t\tconst uint kyty_shift = (" << byte << " & 2u) * 8u;\n"
					     << "\t\t\tkyty_lds[" << d << "] = (kyty_lds[" << d
					     << "] & ~(0xffffu << kyty_shift)) | ((" << Expr(inst.Arg(1))
					     << " & 0xffffu) << kyty_shift);\n";
				} else {
					for (uint32_t i = 0; i < count; ++i) {
						body << "\t\t\tif (" << i << "u < " << LdsDwords() << "u - " << d << ") kyty_lds[" << d << " + " << i << "u] = " << Expr(inst.Arg(1 + i))
						     << ";\n";
					}
				}
				body << "\t\t}\n";
				if (LdsSyncEnabled() && program.stage == ShaderType::Compute) body << "\t\tthreadgroup_barrier(mem_flags::mem_threadgroup);\n";
				return;
			}
			default: EmitAlu(inst); return;
		}
	}

	std::string Emit();
};

void Emitter::EmitAlu(const Inst& inst) {
	switch (inst.GetOpcode()) {
		case IR::ValueOpcode::IAdd32: EmitAssignment(inst, Binary(inst, "+")); return;
		case IR::ValueOpcode::ISub32: EmitAssignment(inst, Binary(inst, "-")); return;
		case IR::ValueOpcode::IMul32: EmitAssignment(inst, Binary(inst, "*")); return;
		case IR::ValueOpcode::UDiv32: EmitAssignment(inst, Binary(inst, "/")); return;
		case IR::ValueOpcode::BitwiseAnd32: EmitAssignment(inst, Binary(inst, "&")); return;
		case IR::ValueOpcode::BitwiseOr32: EmitAssignment(inst, Binary(inst, "|")); return;
		case IR::ValueOpcode::BitwiseXor32: EmitAssignment(inst, Binary(inst, "^")); return;
		case IR::ValueOpcode::ShiftLeftLogical32: EmitAssignment(inst, Binary(inst, "<<")); return;
		case IR::ValueOpcode::ShiftLeftLogical64:
			EmitAssignment(inst, "(" + Expr(inst.Arg(0)) + " << (" + Expr(inst.Arg(1)) + " & 63u))"); return;
		case IR::ValueOpcode::ShiftRightLogical64:
			EmitAssignment(inst, "(" + Expr(inst.Arg(0)) + " >> (" + Expr(inst.Arg(1)) + " & 63u))"); return;
		case IR::ValueOpcode::ShiftRightArithmetic64:
			EmitAssignment(inst, "ulong(long(" + Expr(inst.Arg(0)) + ") >> (" + Expr(inst.Arg(1)) + " & 63u))"); return;
		case IR::ValueOpcode::ShiftRightLogical32: EmitAssignment(inst, Binary(inst, ">>")); return;
		case IR::ValueOpcode::ShiftRightArithmetic32:
			EmitAssignment(inst, "(uint)((int)" + Expr(inst.Arg(0)) + " >> (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::BitwiseNot32: EmitAssignment(inst, "(~" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::IAbs32:
			EmitAssignment(inst, "abs((int)" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::SMin32:
			EmitAssignment(inst, "min((int)" + Expr(inst.Arg(0)) + ", (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::UMin32:
			EmitAssignment(inst, "min(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::SMax32:
			EmitAssignment(inst, "max((int)" + Expr(inst.Arg(0)) + ", (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::UMax32:
			EmitAssignment(inst, "max(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::SelectU32:
			EmitAssignment(inst, "(" + Expr(inst.Arg(0)) + " ? " + Expr(inst.Arg(1)) + " : " +
			                        Expr(inst.Arg(2)) + ")");
			return;
		case IR::ValueOpcode::SelectF32:
			EmitAssignment(inst, "(" + Expr(inst.Arg(0)) + " ? " + Expr(inst.Arg(1)) + " : " +
			                        Expr(inst.Arg(2)) + ")");
			return;
		case IR::ValueOpcode::SelectU1:
			EmitAssignment(inst, "(" + Expr(inst.Arg(0)) + " ? " + Expr(inst.Arg(1)) + " : " +
			                        Expr(inst.Arg(2)) + ")");
			return;
		case IR::ValueOpcode::LogicalOr: EmitAssignment(inst, Binary(inst, "||")); return;
		case IR::ValueOpcode::LogicalAnd: EmitAssignment(inst, Binary(inst, "&&")); return;
		case IR::ValueOpcode::LogicalXor: EmitAssignment(inst, Binary(inst, "!=")); return;
		case IR::ValueOpcode::LogicalNot: EmitAssignment(inst, "(!" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::IEqual32: EmitAssignment(inst, Binary(inst, "==")); return;
		case IR::ValueOpcode::INotEqual32: EmitAssignment(inst, Binary(inst, "!=")); return;
		case IR::ValueOpcode::ULessThan32: EmitAssignment(inst, Binary(inst, "<")); return;
		case IR::ValueOpcode::ULessThanEqual32: EmitAssignment(inst, Binary(inst, "<=")); return;
		case IR::ValueOpcode::UGreaterThan32: EmitAssignment(inst, Binary(inst, ">")); return;
		case IR::ValueOpcode::UGreaterThanEqual32: EmitAssignment(inst, Binary(inst, ">=")); return;
		case IR::ValueOpcode::SLessThan32:
			EmitAssignment(inst, "((int)" + Expr(inst.Arg(0)) + " < (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::SLessThanEqual32:
			EmitAssignment(inst, "((int)" + Expr(inst.Arg(0)) + " <= (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::SGreaterThan32:
			EmitAssignment(inst, "((int)" + Expr(inst.Arg(0)) + " > (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::SGreaterThanEqual32:
			EmitAssignment(inst, "((int)" + Expr(inst.Arg(0)) + " >= (int)" + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPAdd32: EmitAssignment(inst, Binary(inst, "+")); return;
		case IR::ValueOpcode::FPSub32: EmitAssignment(inst, Binary(inst, "-")); return;
		case IR::ValueOpcode::FPMul32: EmitAssignment(inst, Binary(inst, "*")); return;
		case IR::ValueOpcode::FPFma32:
			EmitAssignment(inst, "fma(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ", " +
			                        Expr(inst.Arg(2)) + ")");
			return;
		case IR::ValueOpcode::FPMin32:
			EmitAssignment(inst, "fmin(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPMax32:
			EmitAssignment(inst, "fmax(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPMinTri32:
			EmitAssignment(inst, "fmin(" + Expr(inst.Arg(0)) + ", fmin(" + Expr(inst.Arg(1)) +
			                        ", " + Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::FPMaxTri32:
			EmitAssignment(inst, "fmax(" + Expr(inst.Arg(0)) + ", fmax(" + Expr(inst.Arg(1)) +
			                        ", " + Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::FPMedTri32:
			// Same rule as the SPIR-V path (EmitFPMedTri32): a NaN operand gives the minimum of the three, not the median.
			if ((RdnaFloatMask() & 4u) == 0) {
				EmitAssignment(inst, "max(min(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + "), min(max(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) +
				                        "), " + Expr(inst.Arg(2)) + "))");
				return;
			}
			EmitAssignment(inst, "kyty_med3f(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ", " + Expr(inst.Arg(2)) + ")");
			return;
		case IR::ValueOpcode::UMinTri32:
			EmitAssignment(inst, "min(" + Expr(inst.Arg(0)) + ", min(" + Expr(inst.Arg(1)) + ", " +
			                        Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::UMaxTri32:
			EmitAssignment(inst, "max(" + Expr(inst.Arg(0)) + ", max(" + Expr(inst.Arg(1)) + ", " +
			                        Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::UMedTri32:
			EmitAssignment(inst, "max(min(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) +
			                        "), min(max(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) +
			                        "), " + Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::SMinTri32:
			EmitAssignment(inst, "min((int)" + Expr(inst.Arg(0)) + ", min((int)" + Expr(inst.Arg(1)) +
			                        ", (int)" + Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::SMaxTri32:
			EmitAssignment(inst, "max((int)" + Expr(inst.Arg(0)) + ", max((int)" + Expr(inst.Arg(1)) +
			                        ", (int)" + Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::SMedTri32:
			EmitAssignment(inst, "max(min((int)" + Expr(inst.Arg(0)) + ", (int)" + Expr(inst.Arg(1)) +
			                        "), min(max((int)" + Expr(inst.Arg(0)) + ", (int)" +
			                        Expr(inst.Arg(1)) + "), (int)" + Expr(inst.Arg(2)) + "))");
			return;
		case IR::ValueOpcode::FPUnordNotEqual32:
			EmitAssignment(inst, "(isnan(" + Expr(inst.Arg(0)) + ") || isnan(" + Expr(inst.Arg(1)) +
			                        ") || " + Expr(inst.Arg(0)) + " != " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPUnordEqual32:
			EmitAssignment(inst, "(" + Expr(inst.Arg(0)) + " == " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPUnordLessThan32:
			EmitAssignment(inst, "(isnan(" + Expr(inst.Arg(0)) + ") || isnan(" + Expr(inst.Arg(1)) +
			                        ") || " + Expr(inst.Arg(0)) + " < " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPUnordGreaterThan32:
			EmitAssignment(inst, "(isnan(" + Expr(inst.Arg(0)) + ") || isnan(" + Expr(inst.Arg(1)) +
			                        ") || " + Expr(inst.Arg(0)) + " > " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPUnordLessThanEqual32:
			EmitAssignment(inst, "(isnan(" + Expr(inst.Arg(0)) + ") || isnan(" + Expr(inst.Arg(1)) +
			                        ") || " + Expr(inst.Arg(0)) + " <= " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::FPUnordGreaterThanEqual32:
			EmitAssignment(inst, "(isnan(" + Expr(inst.Arg(0)) + ") || isnan(" + Expr(inst.Arg(1)) +
			                        ") || " + Expr(inst.Arg(0)) + " >= " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::Barrier:
			if (program.stage == ShaderType::Compute) {
				body << "\t\tthreadgroup_barrier(mem_flags::mem_threadgroup | "
				        "mem_flags::mem_device);\n";
			} else {
				Unsupported(inst, "barrier outside compute");
			}
			return;
		case IR::ValueOpcode::FPOrdEqual32: EmitAssignment(inst, Binary(inst, "==")); return;
		case IR::ValueOpcode::FPOrdNotEqual32: EmitAssignment(inst, Binary(inst, "!=")); return;
		case IR::ValueOpcode::FPOrdLessThan32: EmitAssignment(inst, Binary(inst, "<")); return;
		case IR::ValueOpcode::FPOrdGreaterThan32: EmitAssignment(inst, Binary(inst, ">")); return;
		case IR::ValueOpcode::FPOrdLessThanEqual32: EmitAssignment(inst, Binary(inst, "<=")); return;
		case IR::ValueOpcode::FPOrdGreaterThanEqual32: EmitAssignment(inst, Binary(inst, ">=")); return;
		case IR::ValueOpcode::FPIsNan32:
			EmitAssignment(inst, "isnan(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::FPCmpClass32:
			EmitAssignment(inst, "kyty_class_f32(as_type<uint>(" + Expr(inst.Arg(0)) + "), " + Expr(inst.Arg(1)) + ")"); return;
		// RDNA flushes a denormal input of the transcendental unit to a signed zero (EmitFlushF32DenormToSignedZero on the SPIR-V path):
		// rcp/rsq of it is +-inf there, a huge finite number without the flush, and log2/exp2/sqrt differ the same way.
		case IR::ValueOpcode::FPSqrt: EmitAssignment(inst, "sqrt(" + Ftz(Expr(inst.Arg(0))) + ")"); return;
		case IR::ValueOpcode::FPRecip32: EmitAssignment(inst, "(1.0f / " + Ftz(Expr(inst.Arg(0))) + ")"); return;
		case IR::ValueOpcode::FPRecipIFlag32: EmitAssignment(inst, "(1.0 / " + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPRecipSqrt32:
			EmitAssignment(inst, "rsqrt(" + Ftz(Expr(inst.Arg(0))) + ")");
			return;
		case IR::ValueOpcode::FPFloor32: EmitAssignment(inst, "floor(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPCeil32: EmitAssignment(inst, "ceil(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPTrunc32: EmitAssignment(inst, "trunc(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPFract32: EmitAssignment(inst, "fract(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPRoundEven32:
			EmitAssignment(inst, "rint(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::FPSin: EmitAssignment(inst, "sin(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPCos: EmitAssignment(inst, "cos(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPExp2: EmitAssignment(inst, "exp2(" + Ftz(Expr(inst.Arg(0))) + ")"); return;
		case IR::ValueOpcode::FPLog2: EmitAssignment(inst, "log2(" + Ftz(Expr(inst.Arg(0))) + ")"); return;
		case IR::ValueOpcode::FPAbs32: EmitAssignment(inst, "fabs(" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPNeg32: EmitAssignment(inst, "(-" + Expr(inst.Arg(0)) + ")"); return;
		case IR::ValueOpcode::FPSaturate32:
			EmitAssignment(inst, "saturate(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::ConvertU32F32:
			// Numeric float -> uint conversion with RDNA's rules (NaN and negatives give 0, large values saturate), as the SPIR-V path
			// (EmitF32ToU32). A plain C cast is undefined in MSL for exactly those inputs.
			EmitAssignment(inst, (RdnaFloatMask() & 2u) != 0 ? "kyty_f2u(" + Expr(inst.Arg(0)) + ")" : "(uint)(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::BitCastU32F32:
			EmitAssignment(inst, "as_type<uint>(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::BitCastF32U32:
			EmitAssignment(inst, "as_type<float>(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::ConvertF32U32:
			EmitAssignment(inst, "float(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::ConvertF32S32:
			EmitAssignment(inst, "float((int)" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::ConvertS32F32:
			EmitAssignment(inst, (RdnaFloatMask() & 2u) != 0 ? "kyty_f2i(" + Expr(inst.Arg(0)) + ")" : "as_type<uint>((int)" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::ConvertU32U16:
		case IR::ValueOpcode::ConvertU16U32:
			EmitAssignment(inst, "(uint)(ushort)(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::ConvertU8U32:
		case IR::ValueOpcode::ConvertU32U8:
			EmitAssignment(inst, "(uint)(uchar)(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::BitCount32:
			EmitAssignment(inst, "popcount(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::BitCount64:
			// popcount(ulong) returns ulong in MSL; the IR result is U32.
			EmitAssignment(inst, "(uint)popcount(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::BitReverse32:
			EmitAssignment(inst, "reverse_bits(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::FindILsb32:
			EmitAssignment(inst, "ctz(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::FindUMsb32:
			EmitAssignment(inst, "clz(" + Expr(inst.Arg(0)) + ")");
			return;
		case IR::ValueOpcode::GetInterpolationParameter: {
			// Interpolation modes are not distinguished yet; read the varying directly.
			const auto attr = ImmU32(inst.Arg(0));
			const auto chan = ImmU32(inst.Arg(1));
			if (program.stage != ShaderType::Pixel || !attr || !chan) {
				Unsupported(inst, "interpolation parameter");
				EmitAssignment(inst, "0u");
				return;
			}
			const auto it = in_param_field.find(*attr);
			if (it == in_param_field.end()) {
				Unsupported(inst, "interpolation parameter location");
				EmitAssignment(inst, "0u");
				return;
			}
			EmitAssignment(inst, "as_type<uint>(in." + it->second + "." +
			                        ComponentName(*chan) + ")");
			return;
		}
		case IR::ValueOpcode::WqmU64: {
			const auto value = Expr(inst.Arg(0));
			body << "\t\t{\n\t\t\tulong kyty_m = " << value << ";\n"
			     << "\t\t\tkyty_m |= kyty_m >> 1;\n\t\t\tkyty_m |= kyty_m >> 2;\n"
			     << "\t\t\tkyty_m &= 0x1111111111111111ul;\n\t\t\tkyty_m *= 0xFul;\n\t\t\t"
			     << Name(inst) << " = kyty_m;\n\t\t}\n";
			return;
		}
		case IR::ValueOpcode::LaneId:
			if (program.stage == ShaderType::Vertex) {
				Unsupported(inst, "lane id in a vertex shader");
				EmitAssignment(inst, "0u");
			} else {
				EmitAssignment(inst, "lane");
			}
			return;
		case IR::ValueOpcode::UMulHi:
			EmitAssignment(inst, "mulhi(" + Expr(inst.Arg(0)) + ", " + Expr(inst.Arg(1)) + ")");
			return;
		case IR::ValueOpcode::SMulHi:
			EmitAssignment(inst, "as_type<uint>(mulhi((int)" + Expr(inst.Arg(0)) + ", (int)" +
			                        Expr(inst.Arg(1)) + "))");
			return;
		case IR::ValueOpcode::UndefU1: EmitAssignment(inst, "false"); return;
		case IR::ValueOpcode::UndefU32: EmitAssignment(inst, "0u"); return;
		case IR::ValueOpcode::UndefU64: EmitAssignment(inst, "0ul"); return;
		default: break;
	}

	// Result-producing instruction with no MSL lowering yet: record and zero the value so
	// Unsupported() reaches EmitProgram without corrupting the emitted source structure.
	if (inst.GetType() != IR::Type::Void && !IsResourceType(inst.GetType())) {
		Unsupported(inst, "no MSL lowering");
		EmitAssignment(inst, "0");
		return;
	}
	if (IR::HasSideEffects(inst.GetOpcode())) {
		Unsupported(inst, "side effect not lowered");
	}
}

std::string Emitter::Emit() {
	pair_ = Wave64Pair(program.stage, program.wave_size, input_info, program.shader_hash);
	if (const char* trace = std::getenv("KYTY_MSL_NAN_TRACE"); trace != nullptr && (program.stage == ShaderType::Pixel || program.stage == ShaderType::Compute)) {
		// A comma-separated list of pixel or compute shader hashes (the guest shader hash, not the translated variant hash that traces print).
		for (const char* p = trace; *p != '\0';) {
			char*          end = nullptr;
			const uint64_t hash = std::strtoull(p, &end, 0);
			if (end == p) break;
			nan_trace_ = nan_trace_ || hash == program.shader_hash;
			p = *end == ',' ? end + 1 : end;
		}
	}
	BuildImageSlots();
	std::ostringstream out;
	out << "// generated by Kyty RDNA->MSL backend\n";
	for (const auto& [resource, slot]: image_slots_) {
		out << "// image_" << resource << " id=" << slot.id
		    << " kind=" << static_cast<uint32_t>(slot.kind) << "\n";
	}
	out << "#include <metal_stdlib>\n";
	out << "#include <simd/simd.h>\n";
	out << "using namespace metal;\n\n";
	if (HasDescriptor(IR::DescriptorBindingKind::BdaPagetable)) {
		out << R"(
// Two-level native Metal page table: 16 KiB pages, 1024 entries per leaf.
inline device const uchar* kyty_guest_pointer(device const ulong* root, ulong address) {
	if (root == nullptr) return nullptr;
	ulong packed;
	if (address < 0x10000000000ul) packed = address;
	else if (address >= 0x80000000000ul && address < 0x88000000000ul)
		packed = address - 0x80000000000ul + 0x10000000000ul;
	else return nullptr;
	const ulong leaf = root[packed >> 24u];
	if (leaf == 0ul) return nullptr;
	device const ulong* pages = reinterpret_cast<device const ulong*>(leaf);
	const ulong page = pages[(packed >> 14u) & 1023ul];
	return page == 0ul ? nullptr : reinterpret_cast<device const uchar*>(page + (address & 16383ul));
}
inline uint kyty_load_guest(device const ulong* root, ulong address, uint bytes) {
	device const uchar* p = kyty_guest_pointer(root, address);
	if (bytes == 4u && (address & 3ul) == 0ul)
		return p == nullptr ? 0u : *reinterpret_cast<device const uint*>(p);
	uint value = 0u;
	for (uint byte = 0u; byte < bytes; ++byte) {
		p = kyty_guest_pointer(root, address + byte);
		if (p != nullptr) value |= uint(*p) << (byte * 8u);
	}
	return value;
}

)";
	}

	// Wave-wide ballot lowered to the simdgroup builtin, matching the SPIR-V path's
	// uint4 representation (low 32 lanes in .x, high lanes in .y).
	out << "inline uint4 kyty_ballot(bool v) {\n";
	out << "\tsimd_vote vote = simd_ballot(v);\n";
	out << "\treturn uint4(as_type<uint2>((simd_vote::vote_t)vote), 0u, 0u);\n";
	out << "}\n\n";
	// Pair-packed wave64: the ballot of the two emulated halves (low 32 lanes in .x, high lanes in .y), the first set lane of it, and a
	// read of lane `lane64` of a wave whose lanes 0..31 are `lo` and 32..63 are `hi` on this simdgroup.
	out << "inline uint4 kyty_ballot2(bool lo, bool hi) {\n";
	out << "\tsimd_vote a = simd_ballot(lo);\n\tsimd_vote b = simd_ballot(hi);\n";
	out << "\treturn uint4(as_type<uint2>((simd_vote::vote_t)a).x, as_type<uint2>((simd_vote::vote_t)b).x, 0u, 0u);\n";
	out << "}\n\n";
	out << "inline uint kyty_first64(uint4 b) {\n";
	out << "\treturn b.x != 0u ? ctz(b.x) : (b.y != 0u ? 32u + ctz(b.y) : 0u);\n";
	out << "}\n\n";
	out << "inline uint kyty_shuffle64(uint lo, uint hi, uint lane64) {\n";
	out << "\tuint a = simd_shuffle(lo, (ushort)(lane64 & 31u));\n\tuint b = simd_shuffle(hi, (ushort)(lane64 & 31u));\n";
	out << "\treturn (lane64 & 32u) != 0u ? b : a;\n";
	out << "}\n\n";
	out << "inline ushort kyty_first_lane(uint4 ballot) {\n";
	out << "\treturn ballot.x != 0u ? (ushort)ctz(ballot.x) : (ballot.y != 0u ? (ushort)min(ctz(ballot.y) + 32u, 31u) : 0u);\n";
	out << "}\n\n";

	out << R"(
inline bool kyty_class_f32(uint bits, uint mask) {
    uint magnitude = bits & 0x7fffffffu;
    bool negative = (bits >> 31) != 0u;
    uint category;
    if (magnitude > 0x7f800000u) category = (bits & 0x00400000u) != 0u ? 1u : 0u;
    else if (magnitude == 0x7f800000u) category = negative ? 2u : 9u;
    else if (magnitude >= 0x00800000u) category = negative ? 3u : 8u;
    else if (magnitude != 0u) category = negative ? 4u : 7u;
    else category = negative ? 5u : 6u;
    return (mask & (1u << category)) != 0u;
}

// v_cvt_pkrtz_f16_f32 rounds toward zero: a finite value past the half range becomes the largest finite half (65504), not inf. half()
// rounds to nearest, so every HDR colour export above 65504 (16-bit exports are packed this way) turned into inf and later passes made
// NaN of it (inf * 0). Same bit construction as the SPIR-V path (EmitF32ToF16RtzBits).
inline uint kyty_f16_rtz(float f) {
    uint bits = as_type<uint>(f);
    uint sign = (bits >> 16) & 0x8000u;
    uint exponent = (bits >> 23) & 0xffu;
    uint mantissa = bits & 0x007fffffu;
    if (exponent == 255u) return sign | 0x7c00u | (mantissa != 0u ? 0x0200u | (mantissa >> 13) : 0u);
    if (exponent >= 143u) return sign | 0x7bffu;
    if (exponent > 112u) return sign | ((exponent - 112u) << 10) | (mantissa >> 13);
    if (exponent < 103u) return sign;
    return sign | ((mantissa | 0x00800000u) >> (126u - exponent));
}

)";
	out << R"(// RDNA semantics the generic MSL builtins do not give (mirrors the SPIR-V path: EmitFlushF32DenormToSignedZero, EmitF32ToU32, EmitFPMedTri32).
inline float kyty_ftz(float v) {
	uint b = as_type<uint>(v);
	return (b & 0x7f800000u) == 0u ? as_type<float>(b & 0x80000000u) : v;
}
inline uint kyty_f2u(float v) {
	if (isnan(v) || v <= 0.0f) return 0u;
	if (v >= 4294967296.0f) return 0xffffffffu;
	return (uint)trunc(v);
}
inline uint kyty_f2i(float v) {
	if (isnan(v)) return 0u;
	if (v <= -2147483648.0f) return 0x80000000u;
	if (v >= 2147483648.0f) return 0x7fffffffu;
	return as_type<uint>((int)trunc(v));
}
inline float kyty_med3f(float a, float b, float c) {
	float minimum = fmin(fmin(a, b), c);
	float median  = fmax(fmin(a, b), fmin(fmax(a, b), c));
	return (isnan(a) || isnan(b) || isnan(c)) ? minimum : median;
}
)";
	// func: a DEPTH_COMPARE_FUNC applied as texel <func> reference (6 = GEQUAL, the default; with KYTY_MSL_CMP_FUNC the sampler's own,
	// FC 27's shadow samplers are GREATER, S# 0x4092/0x4012; reference-first lit the red room grey and blew out frames, um190).
	out << "inline float4 kyty_sample_cmp(float4 s, float ref, uint func) {\n";
	out << "\tconst float d = s.x;\n";
	out << "\tconst bool pass = func == 0u ? false : func == 1u ? d < ref : func == 2u ? d == ref : func == 3u ? d <= ref :\n";
	out << "\t                  func == 4u ? d > ref : func == 5u ? d != ref : func == 6u ? d >= ref : true;\n";
	out << "\treturn float4(pass ? 1.0f : 0.0f);\n}\n\n";
	out << "inline float4 kyty_gather_cmp(float4 g, float ref, uint func) {\n";
	out << "\treturn float4(kyty_sample_cmp(float4(g.x), ref, func).x, kyty_sample_cmp(float4(g.y), ref, func).x,\n";
	out << "\t              kyty_sample_cmp(float4(g.z), ref, func).x, kyty_sample_cmp(float4(g.w), ref, func).x);\n}\n\n";
	// D16 image data: two 16-bit components per dword. `mask` is the dmask (enabled texel components in order).
	out << "inline uint4 kyty_d16_pack(uint4 t, uint mask, bool is_float) {\n";
	out << "\tuint v[4] = {0u, 0u, 0u, 0u};\n\tuint n = 0u;\n";
	out << "\tfor (uint k = 0u; k < 4u; ++k) {\n";
	out << "\t\tif (((mask >> k) & 1u) != 0u) {\n";
	out << "\t\t\tv[n++] = is_float ? (uint)as_type<ushort>(half(as_type<float>(t[k]))) : (t[k] & 0xffffu);\n";
	out << "\t\t}\n\t}\n";
	out << "\treturn uint4(v[0] | (v[1] << 16), v[2] | (v[3] << 16), 0u, 0u);\n}\n\n";
	out << "inline uint4 kyty_d16_unpack(uint4 d, uint mask, bool is_float) {\n";
	out << "\tuint4 r = uint4(0u);\n\tuint n = 0u;\n";
	out << "\tfor (uint k = 0u; k < 4u; ++k) {\n";
	out << "\t\tif (((mask >> k) & 1u) != 0u) {\n";
	out << "\t\t\tuint raw = (d[n >> 1] >> ((n & 1u) * 16u)) & 0xffffu;\n";
	out << "\t\t\tr[k] = is_float ? as_type<uint>(float(as_type<half>((ushort)raw))) : raw;\n";
	out << "\t\t\t++n;\n\t\t}\n\t}\n";
	out << "\treturn r;\n}\n\n";

	out << "struct PushData {\n\tuint dwords[32];\n};\n\n";

	// The MSL argument buffer uses a flat, non-overlapping id space; the historical
	// NativeBinding layout overlaps as soon as a descriptor group has more than one
	// element (Buffers at 0..N-1 vs images starting at 1).
	out << "struct KytyArgumentBuffer {\n";
	if (program.bindings.descriptors.empty()) {
		// A struct with no members cannot be bound as an argument buffer.
		out << "\tdevice uint *kyty_padding [[id(0)]];\n";
	}
	{
		// (first id, number of ids the member occupies, declaration)
		std::vector<std::tuple<uint32_t, uint32_t, std::string>> members;
		for (const auto& binding: program.bindings.descriptors) {
			switch (binding.kind) {
				case IR::DescriptorBindingKind::Buffers:
					// Buffers are read/write storage, never const-qualified.
					members.emplace_back(kMslBufferBase, 64u,
					                     "device uint *buffers [[id(" +
					                         std::to_string(kMslBufferBase) + ")]][64];");
					break;
				case IR::DescriptorBindingKind::Samplers:
					members.emplace_back(kMslSamplerBase, 32u,
					                     "sampler samplers [[id(" +
					                         std::to_string(kMslSamplerBase) + ")]][32];");
					break;
				case IR::DescriptorBindingKind::Gds:
					members.emplace_back(kMslGdsBinding, 1u,
					                     "device atomic_uint *gds [[id(" +
					                         std::to_string(kMslGdsBinding) + ")]];");
					break;
				case IR::DescriptorBindingKind::BdaPagetable:
					members.emplace_back(kMslBdaBinding, 1u,
					                     "device const ulong *bda_root [[id(" + std::to_string(kMslBdaBinding) + ")]];");
					break;
				case IR::DescriptorBindingKind::FlattenedSrt:
					members.emplace_back(kMslSrtBinding, 1u,
					                     "device const uint *flattened_srt [[id(" +
					                         std::to_string(kMslSrtBinding) + ")]];");
					break;
				default:
					if (IsImageBinding(binding.kind)) {
						const bool storage = static_cast<uint32_t>(binding.kind) >=
						                     IR::FirstStorageImageBinding;
						for (const uint32_t resource: binding.resources) {
							if (resource >= program.info.images.size()) {
								continue;
							}
							const auto slot = ImageSlotOf(resource);
							if (!slot) {
								continue;
							}
							const auto& image = program.info.images[resource];
							members.emplace_back(
							    slot->id, 1u, TextureType(image, storage) + " image_" +
							                  std::to_string(resource) + " [[id(" +
							                  std::to_string(slot->id) + ")]];");
						}
					}
					break;
			}
		}
		std::sort(members.begin(), members.end(),
		          [](const auto& lhs, const auto& rhs) { return std::get<0>(lhs) < std::get<0>(rhs); });
		// Metal lays the members of an argument buffer out one after another in declaration order and ignores gaps
		// between the [[id]] values, while the host writes handle N at byte offset N * 8. Filling every gap with an
		// array of dummy pointers keeps the member index equal to its id.
		uint32_t next_id = 0;
		for (const auto& [id, count, decl]: members) {
			if (id > next_id) {
				out << "\tdevice uint *kyty_gap_" << next_id << " [[id(" << next_id << ")]][" << (id - next_id) << "];\n";
			}
			out << "\t" << decl << "\n";
			next_id = id + count;
		}
	}
	out << "};\n\n";

	BuildInterface();

	const char* stage_qualifier = program.stage == ShaderType::Vertex    ? "vertex "
	                              : program.stage == ShaderType::Pixel   ? "fragment "
	                              : program.stage == ShaderType::Compute ? "kernel "
	                                                                     : "";
	const bool has_out = HasOutInterface();
	const bool has_in  = HasInInterface();

	if (has_out) {
		out << "struct KytyStageOut {\n";
		for (const auto& field: out_fields) {
			out << "\t" << field.type << " " << field.name << " " << field.attribute << ";\n";
		}
		out << "};\n\n";
	}
	if (has_in) {
		out << "struct KytyStageIn {\n";
		for (const auto& field: in_fields) {
			out << "\t" << field.type << " " << field.name << " " << field.attribute << ";\n";
		}
		out << "};\n\n";
	}

	// Vertex shaders are emitted as an inline implementation plus a thin main0: RECTLIST draws give only three
	// vertices and the hardware derives the fourth corner (v1 + v2 - v0), which the wrapper reproduces for vid 3.
	const bool vs_wrap = program.stage == ShaderType::Vertex && has_out && !has_in;
	if (vs_wrap) {
		out << "inline KytyStageOut kyty_vs_impl(constant KytyArgumentBuffer& ab, constant PushData& pc_data, "
		       "constant uint* sizes, uint vid, uint iid) {\n";
	} else {
		out << stage_qualifier << (has_out ? "KytyStageOut " : "void ") << "main0(";
		if (has_in) {
			out << "KytyStageIn in [[stage_in]], ";
		}
		out << "constant KytyArgumentBuffer& ab [[buffer(0)]], "
		    << "constant PushData& pc_data [[buffer(1)]], constant uint* sizes [[buffer(25)]]";
		if (program.stage != ShaderType::Vertex) {
			// Vertex functions reject thread_index_in_simdgroup; lane id is unavailable there.
			out << ", uint lane [[thread_index_in_simdgroup]]";
		}
		if (program.stage == ShaderType::Vertex) {
			out << ", uint vid [[vertex_id]], uint iid [[instance_id]]";
		} else if (program.stage == ShaderType::Pixel) {
			out << ", uint prim_id_arg [[primitive_id]]";
		} else if (program.stage == ShaderType::Compute) {
			out << ", uint3 gid [[thread_position_in_grid]], "
			    << "uint3 tid [[thread_position_in_threadgroup]], "
			    << "uint3 tgid [[threadgroup_position_in_grid]], "
			    << "uint liid [[thread_index_in_threadgroup]]";
			if (nan_trace_) out << ", device atomic_uint* kyty_nan_dbg [[buffer(28)]]";
		}
		out << ") {\n";
	}
	if (has_out) {
		out << "\tKytyStageOut out = {};\n";
	}
	out << "\t(void)ab; (void)pc_data; (void)sizes;\n";
	if (UsesLds()) {
		if (program.stage == ShaderType::Compute) {
			out << "\tthreadgroup uint kyty_lds[" << LdsDwords() << "];\n";
		} else {
			// Non-kernel stages cannot use the threadgroup address space; per-thread
			// scratch is an approximation that keeps the shader compiling.
			out << "\tuint kyty_lds[2048];\n";
		}
	}

	if (nan_trace_ && program.stage != ShaderType::Compute) out << "\tuint kyty_first_nan = 0u;\n";
	if (pair_) {
		// Host thread (simdgroup s, lane l) of a one-dimensional group stands for the guest threads 64*s + l and 64*s + 32 + l.
		out << "\tconst uint kyty_gl0 = ((liid >> 5u) << 6u) | (lane & 31u);\n";
		out << "\tconst uint kyty_gl1 = kyty_gl0 + 32u;\n";
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: block->Instructions()) {
			const char* type_name = MslType(inst.GetType());
			if (type_name != nullptr) {
				out << "\t" << type_name << " " << Name(inst) << " = (" << type_name << ")0;\n";
				if (pair_) out << "\t" << type_name << " " << HalfName(inst, 1) << " = (" << type_name << ")0;\n";
			}
		}
	}

	body.str("");
	body.clear();
	const auto unsupported_before_flow = unsupported.size();
	std::vector<bool> seen(program.blocks.size(), false);
	std::vector<std::pair<uint32_t, uint32_t>> loops;
	const bool structured = !program.dispatcher_fallback && std::getenv("KYTY_MSL_DISPATCHER") == nullptr &&
	    EmitStructuredRegion(0, UINT32_MAX, loops, seen);
	if (!structured) {
		body.str("");
		body.clear();
		unsupported.resize(unsupported_before_flow);
	}
	if (std::getenv("KYTY_LOG_MSL_FLOW") != nullptr) {
		std::fprintf(stderr, "[MslFlow] hash=0x%016llx blocks=%zu structured=%d\n",
		             static_cast<unsigned long long>(program.shader_hash), program.blocks.size(), structured);
	}
	// A dispatcher remains available for irreducible or unsupported regions and A/B checks.
	static const bool use_goto = false;
	if (!structured) for (size_t index = 0; index < program.blocks.size(); ++index) {
		const auto* block = program.blocks[index];
		if (use_goto) {
			body << "L" << index << ": {\n";
			if (program.block_info[index].terminator.loop_header) {
				// Bound runaway loops so one bad guest loop cannot wedge the whole device queue.
				body << "\t\tif (++kyty_iterations > " << MslLoopCap() << "u) { return" << (has_out ? " out" : "") << "; }\n";
			}
		} else {
			body << "\tcase " << index << ": {\n";
		}
		for (const auto& inst: block->Instructions()) {
			if (inst.GetOpcode() == IR::ValueOpcode::Phi || IsResourceType(inst.GetType())) {
				continue;
			}
			EmitWaveInstruction(inst);
		}

		const auto& term = program.block_info[index].terminator;
		switch (term.kind) {
			case CFG::TerminatorKind::Branch:
				EmitPhiCopies(block, term.true_block);
				if (use_goto) {
					body << "\t\tgoto L" << BlockIndex(term.true_block) << ";\n";
				} else {
					body << "\t\tpc = " << BlockIndex(term.true_block) << "u; break;\n";
				}
				break;
			case CFG::TerminatorKind::ConditionalBranch:
				body << "\t\tif (" << Expr(program.block_info[index].condition) << ") {\n";
				EmitPhiCopies(block, term.true_block);
				if (use_goto) {
					body << "\t\t\tgoto L" << BlockIndex(term.true_block) << ";\n\t\t} else {\n";
					EmitPhiCopies(block, term.false_block);
					body << "\t\t\tgoto L" << BlockIndex(term.false_block) << ";\n\t\t}\n";
				} else {
					body << "\t\t\tpc = " << BlockIndex(term.true_block) << "u;\n\t\t} else {\n";
					EmitPhiCopies(block, term.false_block);
					body << "\t\t\tpc = " << BlockIndex(term.false_block) << "u;\n\t\t}\n\t\tbreak;\n";
				}
				break;
			case CFG::TerminatorKind::IndirectBranch: {
				// A jump table (s_setpc_b64 on a selector, e.g. the blend-mode switch of the mask layering kernel): choose the target the way the
				// SPIR-V dispatcher does. A selector without an entry leaves the shader. Without this the block returned at once and every block
				// behind the table was unreachable, so such kernels did nothing (the body hide masks never got their cutouts).
				const auto& info    = program.block_info[index];
				const bool  by_code = term.indirect_selector_code != UINT32_MAX;
				const auto& values  = by_code ? term.indirect_selector_values : term.indirect_target_pcs;
				const auto& targets = by_code ? term.indirect_selector_targets : term.indirect_targets;
				const size_t count  = std::min(values.size(), targets.size());
				if (info.indirect_target.IsEmpty() || count == 0) {
					body << "\t\treturn" << (has_out ? " out" : "") << ";\n";
					break;
				}
				body << "\t\t{\n\t\t\tconst uint kyty_sel = as_type<uint>(" << Expr(info.indirect_target) << ");\n";
				for (size_t entry = 0; entry < count; ++entry) {
					body << (entry == 0 ? "\t\t\tif (" : "\t\t\t} else if (") << "kyty_sel == " << values[entry] << "u) {\n";
					EmitPhiCopies(block, targets[entry]);
					body << "\t\t\t\tpc = " << BlockIndex(targets[entry]) << "u;\n";
				}
				body << "\t\t\t} else {\n\t\t\t\tpc = 0xffffffffu;\n\t\t\t}\n\t\t}\n\t\tbreak;\n";
				break;
			}
			default: body << "\t\treturn" << (has_out ? " out" : "") << ";\n"; break;
		}
		body << "\t}\n";
	}

	// Apple GPUs kill the queue when a shader stops making progress. Bound the
	// dispatcher iterations per invocation so a mistranslated guest loop cannot wedge
	// the whole device (mirrors KYTY_LOOP_CAP in the SPIR-V backend).
	if (structured) {
		out << "\tuint kyty_iterations = 0u;\n";
		out << body.str();
		out << "}\n";
	} else if (use_goto) {
		out << "\tuint kyty_iterations = 0u;\n";
		out << body.str();
		out << "\treturn" << (has_out ? " out" : "") << ";\n";
		out << "}\n";
	} else {
	out << "\tuint pc = 0u;\n";
	out << "\tuint kyty_iterations = 0u;\n";
	out << "\twhile (true) {\n";
	out << "\t\tif (++kyty_iterations > " << MslDispatcherCap() << "u) { return" << (has_out ? " out" : "")
	    << "; }\n";
	out << "\t\tswitch (pc) {\n";
	out << body.str();
	out << "\t\tdefault: return" << (has_out ? " out" : "") << ";\n";
	out << "\t\t}\n\t}\n";
	out << "}\n";
	}
	if (vs_wrap) {
		// Parameters can carry packed integers as float bits. Identical corner
		// components must survive without FP arithmetic (which flushes small
		// bit patterns to zero or changes a signed zero/NaN payload).
		out << "\nuint kyty_rect_corner_index(float4 p0, float4 p1, float4 p2) {\n";
		out << "\tconst bool c1 = (p1.x == p0.x && p1.y == p2.y) || (p1.y == p0.y && p1.x == p2.x);\n";
		out << "\tconst bool c2 = (p2.x == p1.x && p2.y == p0.y) || (p2.y == p1.y && p2.x == p0.x);\n";
		out << "\treturn c1 ? 1u : (c2 ? 2u : 0u);\n}\n";
		out << "\nfloat4 kyty_rect_corner(float4 a, float4 b, float4 c) {\n";
		out << "\tuint4 ua = as_type<uint4>(a), ub = as_type<uint4>(b), uc = as_type<uint4>(c);\n";
		out << "\treturn as_type<float4>(select(as_type<uint4>(b + c - a), ub, (ua == ub) & (ub == uc)));\n}\n";
		out << "\nvertex KytyStageOut main0(constant KytyArgumentBuffer& ab [[buffer(0)]], "
		    << "constant PushData& pc_data [[buffer(1)]], constant uint* sizes [[buffer(25)]], "
		    << "device const uint* rect_args [[buffer(26)]], "
		    << "uint vid [[vertex_id]], uint iid [[instance_id]]) {\n";
		// Indexed RECTLIST: the renderer draws (i0,i1,i2,i1,i2,SENT) and passes the source vertex ids in sizes[59..61]
		// and the sentinel in sizes[62]; sizes[63] == 2 selects this form.
		out << "\tif (sizes[63] == 2u && vid == sizes[62]) {\n";
		out << "\t\tKytyStageOut a = kyty_vs_impl(ab, pc_data, sizes, sizes[59], iid);\n";
		out << "\t\tKytyStageOut b = kyty_vs_impl(ab, pc_data, sizes, sizes[60], iid);\n";
		out << "\t\tKytyStageOut c = kyty_vs_impl(ab, pc_data, sizes, sizes[61], iid);\n";
		out << "\t\tKytyStageOut r = b;\n";
		for (const auto& field: out_fields) {
			if (field.type == "float4") {
				out << "\t\tr." << field.name << " = kyty_rect_corner(a." << field.name << ", b." << field.name << ", c."
				    << field.name << ");\n";
			}
		}
		out << "\t\treturn r;\n\t}\n";
		// Indirect mode reads the GPU snapshot, including vertexStart. The fourth
		// vertex must never access a fourth record in the three-record guest V#.
		// The right-angle corner of the three guest vertices is not always vertex 0: it is the one sharing x with one neighbour and y
		// with the other (the Vulkan RectListEmitter finds it the same way). It leads the strip, so vertex 3 - the corner opposite to
		// it - closes the rectangle; with a fixed vertex 0 the strip covered only half of every rectangle whose corner came second or
		// third (full-screen passes left half of their target stale).
		out << "\tuint rect_base = 0u; bool rect_mode = sizes[63] == 1u;\n";
		out << "\tif (sizes[63] == 3u) { rect_base = rect_args[2]; rect_mode = rect_args[0] == 3u; }\n";
		out << "\tif (rect_mode && vid >= rect_base && vid < rect_base + 4u) {\n";
		out << "\t\tKytyStageOut v0 = kyty_vs_impl(ab, pc_data, sizes, rect_base, iid);\n";
		out << "\t\tKytyStageOut v1 = kyty_vs_impl(ab, pc_data, sizes, rect_base + 1u, iid);\n";
		out << "\t\tKytyStageOut v2 = kyty_vs_impl(ab, pc_data, sizes, rect_base + 2u, iid);\n";
		if (!position_field.empty()) {
			out << "\t\tconst uint k = kyty_rect_corner_index(v0." << position_field << ", v1." << position_field << ", v2." << position_field
			    << ");\n";
		} else {
			out << "\t\tconst uint k = 0u;\n";
		}
		out << "\t\tKytyStageOut a = k == 1u ? v1 : (k == 2u ? v2 : v0);\n";
		out << "\t\tKytyStageOut b = k == 1u ? v2 : (k == 2u ? v0 : v1);\n";
		out << "\t\tKytyStageOut c = k == 1u ? v0 : (k == 2u ? v1 : v2);\n";
		out << "\t\tconst uint slot = vid - rect_base;\n";
		out << "\t\tif (slot == 0u) return a;\n\t\tif (slot == 1u) return b;\n\t\tif (slot == 2u) return c;\n";
		out << "\t\tKytyStageOut r = b;\n";
		for (const auto& field: out_fields) {
			if (field.type == "float4") {
				out << "\t\tr." << field.name << " = kyty_rect_corner(a." << field.name << ", b." << field.name << ", c."
				    << field.name << ");\n";
			}
		}
		out << "\t\treturn r;\n\t}\n";
		out << "\treturn kyty_vs_impl(ab, pc_data, sizes, vid, iid);\n}\n";
	}

	EmitResult result;
	result.source      = out.str();
	result.unsupported = unsupported;
	result.success     = unsupported.empty();
	return result.source;
}

} // namespace

bool Wave64Pair(ShaderType stage, uint32_t wave_size, ShaderStageInputInfo input_info, uint64_t shader_hash) {
	static const bool s_on = [] {
		// On by default (KYTY_MSL_WAVE64=0 turns it off). RDNA compute kernels assume one 64-lane wave (LDS handed between lanes with no
		// barrier, cross-lane operations over 64 lanes); a 64-thread Metal group is two 32-lane simdgroups that do not run in lockstep, so
		// without the pair lowering every reduction / lighting kernel races and the menu is noise and streaks. An earlier note recorded
		// crashes in the loading phase with it on; that did not reproduce in the ~25 launches of 2026-10-27..11-01 (all survived 150-420 s).
		const char* value = std::getenv("KYTY_MSL_WAVE64");
		return value == nullptr || value[0] != '0';
	}();
	if (!s_on || stage != ShaderType::Compute || wave_size != 64u) return false;
	static const std::pair<uint32_t, uint32_t> s_range = [] {
		uint32_t lo = 0, hi = 256;
		if (const char* value = std::getenv("KYTY_MSL_WAVE64_RANGE")) std::sscanf(value, "%u,%u", &lo, &hi);
		return std::pair<uint32_t, uint32_t> {lo, hi};
	}();
	if (shader_hash != 0) {
		const uint32_t bucket = static_cast<uint32_t>((shader_hash >> 4) & 255u);
		if (bucket < s_range.first || bucket >= s_range.second) return false;
		// KYTY_MSL_WAVE64_SKIP=<hash>[,<hash>...]: these kernels keep the plain 32-lane lowering (per-kernel A/B).
		static const std::vector<uint64_t> s_skip = [] {
			std::vector<uint64_t> v;
			for (const char* p = std::getenv("KYTY_MSL_WAVE64_SKIP"); p != nullptr && *p != '\0';) {
				char* end = nullptr;
				v.push_back(std::strtoull(p, &end, 0));
				if (end == p) break;
				p = *end == ',' ? end + 1 : end;
			}
			return v;
		}();
		if (std::find(s_skip.begin(), s_skip.end(), shader_hash) != s_skip.end()) return false;
	}
	const auto* cs = ShaderWorkgroupInput(stage, input_info);
	if (cs == nullptr || cs->host_subgroup_size != 32u) return false;
	const uint32_t total = std::max(cs->threads_num[0], 1u) * std::max(cs->threads_num[1], 1u) * std::max(cs->threads_num[2], 1u);
	// Whole waves only: a partial wave would need the lanes that never launched masked out of EXEC (the frontend does that for the
	// unpaired lowering, not for this one).
	return total >= 64u && total % 64u == 0u;
}

namespace {
std::mutex                                          s_pair_mutex;
std::unordered_map<uint64_t, std::pair<bool, uint32_t>> s_pair_by_hash;
} // namespace

bool Wave64PairCompiled(uint64_t shader_hash, bool* pair, uint32_t* threads) {
	std::lock_guard<std::mutex> lock(s_pair_mutex);
	const auto it = s_pair_by_hash.find(shader_hash);
	if (it == s_pair_by_hash.end()) return false;
	if (pair != nullptr) *pair = it->second.first;
	if (threads != nullptr) *threads = it->second.second;
	return true;
}

EmitResult EmitProgram(const IR::Program& program, ShaderStageInputInfo input_info) {
	Emitter emitter(program, std::move(input_info));
	EmitResult result;
	result.source      = emitter.Emit();
	result.wave64_pair = emitter.pair_;
	if (program.stage == ShaderType::Compute) {
		const auto* cs = ShaderWorkgroupInput(program.stage, emitter.input_info);
		std::lock_guard<std::mutex> lock(s_pair_mutex);
		s_pair_by_hash[program.shader_hash] = {emitter.pair_, cs == nullptr ? 0u : std::max(cs->threads_num[0], 1u) * std::max(cs->threads_num[1], 1u) * std::max(cs->threads_num[2], 1u)};
	}
	result.unsupported = emitter.unsupported;
	result.success     = emitter.unsupported.empty();
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler::Msl
