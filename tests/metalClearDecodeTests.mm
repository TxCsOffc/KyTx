// Standalone CPU test: tools/test_clear_decode.sh
// DecodeRegisterClear against an independent model of the hardware packing (the per-format rules KytyPS5 pins in its renderer tests, upstream
// b2c9ec76): channels sit in the low bits of CB_COLOR_CLEAR_WORD0, a 64-bit colour continues in WORD1, 32-bit channels are raw words.
#include "graphics/host_gpu/metal/metalClearDecode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace Libs::Graphics::Metal;
static void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

static float Half(uint32_t h) { // IEEE binary16 -> float, written out (no __fp16)
	const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
	float v;
	if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
	else if (exp == 31) v = man ? NAN : INFINITY;
	else v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
	return sign ? -v : v;
}

struct Case { MTLPixelFormat fmt; const char* name; std::array<float, 4> (*model)(uint32_t, uint32_t); };

int main() {
	try {
		auto n8 = [](uint32_t w, int i) { return float((w >> (8 * i)) & 0xff) / 255.0f; };
		auto n16 = [](uint32_t w) { return float(w & 0xffff) / 65535.0f; };
		const Case cases[] = {
		    {MTLPixelFormatR8Unorm, "R8Unorm", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xff) / 255.f, 0, 0, 1}; }},
		    {MTLPixelFormatRG8Unorm, "RG8Unorm", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xff) / 255.f, float((a >> 8) & 0xff) / 255.f, 0, 1}; }},
		    {MTLPixelFormatR8Uint, "R8Uint", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xff), 0, 0, 1}; }},
		    {MTLPixelFormatRG8Uint, "RG8Uint", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xff), float((a >> 8) & 0xff), 0, 1}; }},
		    {MTLPixelFormatR16Uint, "R16Uint", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xffff), 0, 0, 1}; }},
		    {MTLPixelFormatR16Unorm, "R16Unorm", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xffff) / 65535.f, 0, 0, 1}; }},
		    {MTLPixelFormatRG16Unorm, "RG16Unorm", [](uint32_t a, uint32_t) { return std::array<float, 4>{float(a & 0xffff) / 65535.f, float(a >> 16) / 65535.f, 0, 1}; }},
		    {MTLPixelFormatRGBA16Unorm, "RGBA16Unorm", [](uint32_t a, uint32_t b) { return std::array<float, 4>{float(a & 0xffff) / 65535.f, float(a >> 16) / 65535.f, float(b & 0xffff) / 65535.f, float(b >> 16) / 65535.f}; }},
		    {MTLPixelFormatRGBA16Uint, "RGBA16Uint", [](uint32_t a, uint32_t b) { return std::array<float, 4>{float(a & 0xffff), float(a >> 16), float(b & 0xffff), float(b >> 16)}; }},
		    {MTLPixelFormatRGBA16Sint, "RGBA16Sint", [](uint32_t a, uint32_t b) { return std::array<float, 4>{float(int16_t(a & 0xffff)), float(int16_t(a >> 16)), float(int16_t(b & 0xffff)), float(int16_t(b >> 16))}; }},
		    {MTLPixelFormatR16Float, "R16Float", [](uint32_t a, uint32_t) { return std::array<float, 4>{Half(a & 0xffff), 0, 0, 1}; }},
		    {MTLPixelFormatRG16Float, "RG16Float", [](uint32_t a, uint32_t) { return std::array<float, 4>{Half(a & 0xffff), Half(a >> 16), 0, 1}; }},
		    {MTLPixelFormatRGBA16Float, "RGBA16Float", [](uint32_t a, uint32_t b) { return std::array<float, 4>{Half(a & 0xffff), Half(a >> 16), Half(b & 0xffff), Half(b >> 16)}; }},
		    {MTLPixelFormatR32Float, "R32Float", [](uint32_t a, uint32_t) { float f; std::memcpy(&f, &a, 4); return std::array<float, 4>{f, 0, 0, 1}; }},
		    {MTLPixelFormatRG32Float, "RG32Float", [](uint32_t a, uint32_t b) { float f, g; std::memcpy(&f, &a, 4); std::memcpy(&g, &b, 4); return std::array<float, 4>{f, g, 0, 1}; }},
		    {MTLPixelFormatRGBA8Unorm, "RGBA8Unorm", nullptr},
		};
		(void)n8; (void)n16;
		const uint32_t words[] = {0x00000001u, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u, 0x3f800000u, 0x40490fdbu, 0x3c003c00u,
		                          0x7bffu,     0x04000400u, 0xdeadbeefu, 0x12345678u, 0x80008000u, 0xffffffffu, 0x0000abcdu, 0xc0a00000u};
		unsigned compared = 0;
		for (const auto& c: cases) {
			if (c.model == nullptr) continue;
			for (uint32_t a: words) for (uint32_t b: {0u, 0x00007c00u, 0x3f800000u, 0xcafebabeu, 0x80000000u}) {
				std::array<float, 4> got {};
				Check(DecodeRegisterClear(c.fmt, a, b, got), c.name);
				const auto want = c.model(a, b);
				for (int i = 0; i < 4; ++i) {
					const bool nan = std::isnan(want[i]);
					Check(nan ? std::isnan(got[i]) : got[i] == want[i], c.name);
				}
				++compared;
			}
		}
		// All-zero words clear to zero for every format, known or not (the identity shortcut).
		for (auto f: {MTLPixelFormatR32Uint, MTLPixelFormatRGBA32Float, MTLPixelFormatBGRA8Unorm, MTLPixelFormatR8Uint}) {
			std::array<float, 4> z {1, 1, 1, 1};
			Check(DecodeRegisterClear(f, 0, 0, z) && z[0] == 0 && z[1] == 0 && z[2] == 0 && z[3] == 0, "zero words clear to zero");
		}
		// A 32-bit integer clear and other unlisted formats are refused, never rounded through a float.
		std::array<float, 4> unused {};
		for (auto f: {MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, MTLPixelFormatRG32Uint, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Float, MTLPixelFormatRG16Uint})
			Check(!DecodeRegisterClear(f, 0x01000001u, 0x01000001u, unused), "unrepresentable clear is refused");
		std::printf("Clear register decode: %u format/word comparisons against the packing model, zero identity and refusals passed\n", compared);
	} catch (const std::exception& e) { std::fprintf(stderr, "clear decode test failed: %s\n", e.what()); return 1; }
}
