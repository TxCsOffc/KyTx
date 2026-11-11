#pragma once

// CB_COLOR_CLEAR_WORD0/1 -> clear colour, per render target format. A CMASK or DCC fast clear leaves the pixels untouched and the hardware reads
// cleared blocks as this colour, so a format this table does not know turns the clear into nothing and the target keeps its old contents (UFC 6
// fast-clears an R8_UINT region map every frame this way: KytyPS5 b2c9ec76). Integer formats hold their raw channel bits, so a value is
// returned only when a float represents it exactly (<= 24 bits); 32-bit integer clears are refused rather than rounded.

#import <Metal/Metal.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Libs::Graphics::Metal {

inline float DecodeSmallFloat(uint32_t v, uint32_t exp_bits_unused, uint32_t mant_bits) {
	(void)exp_bits_unused;
	const uint32_t e = (v >> mant_bits) & 0x1fu;
	const uint32_t m = v & ((1u << mant_bits) - 1u);
	const float scale = static_cast<float>(1u << mant_bits);
	if (e == 0u) return (static_cast<float>(m) / scale) * std::ldexp(1.0f, -14);
	if (e == 31u) return 65504.0f;
	return (1.0f + static_cast<float>(m) / scale) * std::ldexp(1.0f, static_cast<int>(e) - 15);
}

inline float HalfToFloat(uint32_t h) {
	const uint16_t bits = static_cast<uint16_t>(h);
	__fp16 half;
	std::memcpy(&half, &bits, sizeof(half));
	return static_cast<float>(half);
}

inline bool DecodeRegisterClear(MTLPixelFormat fmt, uint32_t w0, uint32_t w1, std::array<float, 4>& out) {
	if (w0 == 0 && w1 == 0) {
		out = {0.f, 0.f, 0.f, 0.f};
		return true;
	}
	const auto byte  = [](uint32_t w, uint32_t i) { return static_cast<float>((w >> (8u * i)) & 0xffu) / 255.0f; };
	const auto unorm16 = [](uint32_t w) { return static_cast<float>(w & 0xffffu) / 65535.0f; };
	const auto u16 = [](uint32_t w) { return static_cast<float>(w & 0xffffu); };
	const auto s16 = [](uint32_t w) { return static_cast<float>(static_cast<int16_t>(w & 0xffffu)); };
	switch (fmt) {
		case MTLPixelFormatR8Unorm: out = {byte(w0, 0), 0.f, 0.f, 1.f}; return true;
		case MTLPixelFormatRG8Unorm: out = {byte(w0, 0), byte(w0, 1), 0.f, 1.f}; return true;
		case MTLPixelFormatR8Uint: out = {static_cast<float>(w0 & 0xffu), 0.f, 0.f, 1.f}; return true;
		case MTLPixelFormatRG8Uint: out = {static_cast<float>(w0 & 0xffu), static_cast<float>((w0 >> 8) & 0xffu), 0.f, 1.f}; return true;
		case MTLPixelFormatRGBA8Unorm:
		case MTLPixelFormatRGBA8Unorm_sRGB: out = {byte(w0, 0), byte(w0, 1), byte(w0, 2), byte(w0, 3)}; return true;
		case MTLPixelFormatBGRA8Unorm:
		case MTLPixelFormatBGRA8Unorm_sRGB: out = {byte(w0, 2), byte(w0, 1), byte(w0, 0), byte(w0, 3)}; return true;
		case MTLPixelFormatRGB10A2Unorm:
			out = {static_cast<float>(w0 & 0x3ffu) / 1023.f, static_cast<float>((w0 >> 10) & 0x3ffu) / 1023.f,
			       static_cast<float>((w0 >> 20) & 0x3ffu) / 1023.f, static_cast<float>(w0 >> 30) / 3.f};
			return true;
		case MTLPixelFormatRG11B10Float:
			out = {DecodeSmallFloat(w0 & 0x7ffu, 5, 6), DecodeSmallFloat((w0 >> 11) & 0x7ffu, 5, 6),
			       DecodeSmallFloat((w0 >> 22) & 0x3ffu, 5, 5), 1.f};
			return true;
		case MTLPixelFormatR16Float: out = {HalfToFloat(w0 & 0xffffu), 0.f, 0.f, 1.f}; return true;
		case MTLPixelFormatRG16Float: out = {HalfToFloat(w0 & 0xffffu), HalfToFloat(w0 >> 16), 0.f, 1.f}; return true;
		case MTLPixelFormatRGBA16Float:
			out = {HalfToFloat(w0 & 0xffffu), HalfToFloat(w0 >> 16), HalfToFloat(w1 & 0xffffu), HalfToFloat(w1 >> 16)};
			return true;
		case MTLPixelFormatR16Unorm: out = {unorm16(w0), 0.f, 0.f, 1.f}; return true;
		case MTLPixelFormatRG16Unorm: out = {unorm16(w0), unorm16(w0 >> 16), 0.f, 1.f}; return true;
		case MTLPixelFormatRGBA16Unorm: out = {unorm16(w0), unorm16(w0 >> 16), unorm16(w1), unorm16(w1 >> 16)}; return true;
		case MTLPixelFormatR16Uint: out = {u16(w0), 0.f, 0.f, 1.f}; return true;
		case MTLPixelFormatRGBA16Uint: out = {u16(w0), u16(w0 >> 16), u16(w1), u16(w1 >> 16)}; return true;
		case MTLPixelFormatRGBA16Sint: out = {s16(w0), s16(w0 >> 16), s16(w1), s16(w1 >> 16)}; return true;
		// Each clear register holds one raw 32-bit channel.
		case MTLPixelFormatR32Float: out = {std::bit_cast<float>(w0), 0.f, 0.f, 1.f}; return true;
		case MTLPixelFormatRG32Float: out = {std::bit_cast<float>(w0), std::bit_cast<float>(w1), 0.f, 1.f}; return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::Metal
