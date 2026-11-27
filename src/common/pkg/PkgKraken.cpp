#include "common/pkg/PkgKraken.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>
#include <vector>

namespace Common::Pkg {

namespace {

constexpr size_t   kChunkMax     = 0x20000u;
constexpr size_t   kSeedSize     = 8u;
constexpr uint32_t kChunk0SubLit = 0x01u;
constexpr uint32_t kChunk0NewLz  = 0x02u;
constexpr uint32_t kChunk1SubLit = 0x10u;
constexpr uint32_t kChunk1NewLz  = 0x20u;
constexpr uint32_t kChunk1Restart = 0x40u;

constexpr std::array<uint32_t, 12> kCodePrefixOrg = {
    0x0u, 0x0u, 0x2u, 0x6u, 0xEu, 0x1Eu, 0x3Eu, 0x7Eu, 0xFEu, 0x1FEu, 0x2FEu, 0x3FEu,
};

constexpr std::array<uint32_t, 256> kRiceVal = {
    0x80000000u, 0x00000007u, 0x10000006u, 0x00000006u, 0x20000005u, 0x00000105u, 0x10000005u, 0x00000005u,
    0x30000004u, 0x00000204u, 0x10000104u, 0x00000104u, 0x20000004u, 0x00010004u, 0x10000004u, 0x00000004u,
    0x40000003u, 0x00000303u, 0x10000203u, 0x00000203u, 0x20000103u, 0x00010103u, 0x10000103u, 0x00000103u,
    0x30000003u, 0x00020003u, 0x10010003u, 0x00010003u, 0x20000003u, 0x01000003u, 0x10000003u, 0x00000003u,
    0x50000002u, 0x00000402u, 0x10000302u, 0x00000302u, 0x20000202u, 0x00010202u, 0x10000202u, 0x00000202u,
    0x30000102u, 0x00020102u, 0x10010102u, 0x00010102u, 0x20000102u, 0x01000102u, 0x10000102u, 0x00000102u,
    0x40000002u, 0x00030002u, 0x10020002u, 0x00020002u, 0x20010002u, 0x01010002u, 0x10010002u, 0x00010002u,
    0x30000002u, 0x02000002u, 0x11000002u, 0x01000002u, 0x20000002u, 0x00000012u, 0x10000002u, 0x00000002u,
    0x60000001u, 0x00000501u, 0x10000401u, 0x00000401u, 0x20000301u, 0x00010301u, 0x10000301u, 0x00000301u,
    0x30000201u, 0x00020201u, 0x10010201u, 0x00010201u, 0x20000201u, 0x01000201u, 0x10000201u, 0x00000201u,
    0x40000101u, 0x00030101u, 0x10020101u, 0x00020101u, 0x20010101u, 0x01010101u, 0x10010101u, 0x00010101u,
    0x30000101u, 0x02000101u, 0x11000101u, 0x01000101u, 0x20000101u, 0x00000111u, 0x10000101u, 0x00000101u,
    0x50000001u, 0x00040001u, 0x10030001u, 0x00030001u, 0x20020001u, 0x01020001u, 0x10020001u, 0x00020001u,
    0x30010001u, 0x02010001u, 0x11010001u, 0x01010001u, 0x20010001u, 0x00010011u, 0x10010001u, 0x00010001u,
    0x40000001u, 0x03000001u, 0x12000001u, 0x02000001u, 0x21000001u, 0x01000011u, 0x11000001u, 0x01000001u,
    0x30000001u, 0x00000021u, 0x10000011u, 0x00000011u, 0x20000001u, 0x00001001u, 0x10000001u, 0x00000001u,
    0x70000000u, 0x00000600u, 0x10000500u, 0x00000500u, 0x20000400u, 0x00010400u, 0x10000400u, 0x00000400u,
    0x30000300u, 0x00020300u, 0x10010300u, 0x00010300u, 0x20000300u, 0x01000300u, 0x10000300u, 0x00000300u,
    0x40000200u, 0x00030200u, 0x10020200u, 0x00020200u, 0x20010200u, 0x01010200u, 0x10010200u, 0x00010200u,
    0x30000200u, 0x02000200u, 0x11000200u, 0x01000200u, 0x20000200u, 0x00000210u, 0x10000200u, 0x00000200u,
    0x50000100u, 0x00040100u, 0x10030100u, 0x00030100u, 0x20020100u, 0x01020100u, 0x10020100u, 0x00020100u,
    0x30010100u, 0x02010100u, 0x11010100u, 0x01010100u, 0x20010100u, 0x00010110u, 0x10010100u, 0x00010100u,
    0x40000100u, 0x03000100u, 0x12000100u, 0x02000100u, 0x21000100u, 0x01000110u, 0x11000100u, 0x01000100u,
    0x30000100u, 0x00000120u, 0x10000110u, 0x00000110u, 0x20000100u, 0x00001100u, 0x10000100u, 0x00000100u,
    0x60000000u, 0x00050000u, 0x10040000u, 0x00040000u, 0x20030000u, 0x01030000u, 0x10030000u, 0x00030000u,
    0x30020000u, 0x02020000u, 0x11020000u, 0x01020000u, 0x20020000u, 0x00020010u, 0x10020000u, 0x00020000u,
    0x40010000u, 0x03010000u, 0x12010000u, 0x02010000u, 0x21010000u, 0x01010010u, 0x11010000u, 0x01010000u,
    0x30010000u, 0x00010020u, 0x10010010u, 0x00010010u, 0x20010000u, 0x00011000u, 0x10010000u, 0x00010000u,
    0x50000000u, 0x04000000u, 0x13000000u, 0x03000000u, 0x22000000u, 0x02000010u, 0x12000000u, 0x02000000u,
    0x31000000u, 0x01000020u, 0x11000010u, 0x01000010u, 0x21000000u, 0x01001000u, 0x11000000u, 0x01000000u,
    0x40000000u, 0x00000030u, 0x10000020u, 0x00000020u, 0x20000010u, 0x00001010u, 0x10000010u, 0x00000010u,
    0x30000000u, 0x00002000u, 0x10001000u, 0x00001000u, 0x20000000u, 0x00100000u, 0x10000000u, 0x00000000u,
};

constexpr std::array<uint8_t, 256> kRiceLen = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

constexpr uint32_t Shl32(uint32_t v, int64_t n) noexcept {
	return (n >= 0 && n < 32) ? (v << static_cast<unsigned>(n)) : 0u;
}

constexpr uint32_t Shr32(uint32_t v, int64_t n) noexcept {
	return (n >= 0 && n < 32) ? (v >> static_cast<unsigned>(n)) : 0u;
}

constexpr uint32_t Bsr32(uint32_t x) noexcept {
	return (x == 0u) ? 0u : static_cast<uint32_t>(31 - std::countl_zero(x));
}

constexpr uint32_t Bsf32(uint32_t x) noexcept {
	return (x == 0u) ? 0u : static_cast<uint32_t>(std::countr_zero(x));
}

constexpr uint32_t Clz32(uint32_t x) noexcept {
	return (x == 0u) ? 31u : static_cast<uint32_t>(std::countl_zero(x));
}

constexpr uint32_t ByteSwap32(uint32_t x) noexcept {
	return ((x & 0x000000FFu) << 24u) | ((x & 0x0000FF00u) << 8u) |
	       ((x & 0x00FF0000u) >> 8u) | ((x & 0xFF000000u) >> 24u);
}

constexpr uint64_t ByteSwap64(uint64_t x) noexcept {
	return (static_cast<uint64_t>(ByteSwap32(static_cast<uint32_t>(x & 0xFFFFFFFFull))) << 32u) |
	       static_cast<uint64_t>(ByteSwap32(static_cast<uint32_t>(x >> 32u)));
}

uint8_t AtByte(std::span<const uint8_t> b, size_t i) noexcept {
	return (i < b.size()) ? b[i] : 0u;
}

uint32_t ReadLe32Padded(std::span<const uint8_t> b, size_t off) noexcept {
	uint32_t r = 0;
	for (size_t i = 0; i < 4u; ++i) {
		r |= static_cast<uint32_t>(AtByte(b, off + i)) << (8u * i);
	}
	return r;
}

uint64_t ReadLe64Padded(std::span<const uint8_t> b, size_t off) noexcept {
	uint64_t r = 0;
	for (size_t i = 0; i < 8u; ++i) {
		r |= static_cast<uint64_t>(AtByte(b, off + i)) << (8u * i);
	}
	return r;
}

void WriteLe32Padded(std::span<uint8_t> b, size_t off, uint32_t v) noexcept {
	for (size_t i = 0; i < 4u; ++i) {
		if (off + i < b.size()) {
			b[off + i] = static_cast<uint8_t>(v >> (8u * i));
		}
	}
}

void WriteLe64Padded(std::span<uint8_t> b, size_t off, uint64_t v) noexcept {
	for (size_t i = 0; i < 8u; ++i) {
		if (off + i < b.size()) {
			b[off + i] = static_cast<uint8_t>(v >> (8u * i));
		}
	}
}

struct BitReader {
	std::span<const uint8_t> b;
	size_t                   p       = 0;
	size_t                   bound   = 0;
	uint32_t                 bits    = 0;
	int32_t                  bit_pos = 24;
	bool                     bwd     = false;

	static BitReader Forward(std::span<const uint8_t> src, size_t start, size_t end) noexcept {
		BitReader r {src, start, end, 0u, 24, false};
		r.RefillF();
		return r;
	}

	static BitReader Backward(std::span<const uint8_t> src, size_t low, size_t high) noexcept {
		BitReader r {src, high, low, 0u, 24, true};
		r.RefillB();
		return r;
	}

	void Refill() noexcept {
		if (bwd) {
			RefillB();
		} else {
			RefillF();
		}
	}

	void RefillF() noexcept {
		while (bit_pos > 0) {
			const uint32_t byte = (p < bound) ? AtByte(b, p) : 0u;
			bits |= Shl32(byte, bit_pos);
			bit_pos -= 8;
			p += 1u;
		}
	}

	void RefillB() noexcept {
		while (bit_pos > 0) {
			p = p - 1u;
			const uint32_t byte = (p < b.size() && p >= bound) ? b[p] : 0u;
			bits |= Shl32(byte, bit_pos);
			bit_pos -= 8;
		}
	}

	uint32_t ReadBit() noexcept {
		Refill();
		return ReadBitNoRefill();
	}

	uint32_t ReadBitNoRefill() noexcept {
		const uint32_t r = bits >> 31u;
		bits <<= 1u;
		bit_pos += 1;
		return r;
	}

	uint32_t ReadBitsNoRefill(uint32_t n) noexcept {
		const uint32_t r = Shr32(bits, 32 - static_cast<int64_t>(n));
		bits             = Shl32(bits, static_cast<int64_t>(n));
		bit_pos += static_cast<int32_t>(n);
		return r;
	}

	uint32_t ReadBitsNoRefillZero(uint32_t n) noexcept {
		const uint32_t r = Shr32(bits >> 1u, 31 - static_cast<int64_t>(n));
		bits             = Shl32(bits, static_cast<int64_t>(n));
		bit_pos += static_cast<int32_t>(n);
		return r;
	}

	int32_t ReadFluff(int32_t num_symbols) noexcept {
		if (num_symbols == 256) {
			return 0;
		}
		const int32_t  x = std::min(257 - num_symbols, num_symbols) * 2;
		const uint32_t y = Bsr32(static_cast<uint32_t>(x - 1)) + 1u;
		const uint32_t v = Shr32(bits, 32 - static_cast<int64_t>(y));
		const uint32_t z = (1u << y) - static_cast<uint32_t>(x);
		if ((v >> 1u) >= z) {
			bits = Shl32(bits, static_cast<int64_t>(y));
			bit_pos += static_cast<int32_t>(y);
			return static_cast<int32_t>(v - z);
		}
		bits = Shl32(bits, static_cast<int64_t>(y) - 1);
		bit_pos += static_cast<int32_t>(y) - 1;
		return static_cast<int32_t>(v >> 1u);
	}

	uint32_t ReadMoreThan24(uint32_t n) noexcept {
		uint32_t rv = 0u;
		if (n <= 24u) {
			rv = ReadBitsNoRefillZero(n);
		} else {
			const uint32_t hi = ReadBitsNoRefill(24u) << (n - 24u);
			Refill();
			rv = hi + ReadBitsNoRefill(n - 24u);
		}
		Refill();
		return rv;
	}

	uint32_t ReadDistance(uint8_t v_byte) noexcept {
		const uint32_t v  = v_byte;
		uint32_t       rv = 0u;
		if (v < 0xF0u) {
			const uint32_t n = (v >> 4u) + 4u;
			const uint32_t w = std::rotl(bits | 1u, static_cast<int>(n));
			bit_pos += static_cast<int32_t>(n);
			const uint32_t m = (2u << n) - 1u;
			bits             = w & ~m;
			rv               = ((w & m) << 4u) + (v & 0xFu) - 248u;
		} else {
			const uint32_t n = v - 0xF0u + 4u;
			const uint32_t w = std::rotl(bits | 1u, static_cast<int>(n));
			bit_pos += static_cast<int32_t>(n);
			const uint32_t m = (2u << n) - 1u;
			bits             = w & ~m;
			uint32_t r       = 8322816u + ((w & m) << 12u);
			Refill();
			r += (bits >> 20u);
			bit_pos += 12;
			bits <<= 12u;
			rv = r;
		}
		Refill();
		return rv;
	}

	bool ReadLength(uint32_t& v_out) noexcept {
		uint32_t n = 31u - Bsr32(bits);
		if (n > 12u) {
			return false;
		}
		bit_pos += static_cast<int32_t>(n);
		bits = Shl32(bits, static_cast<int64_t>(n));
		Refill();
		n += 7u;
		bit_pos += static_cast<int32_t>(n);
		v_out = Shr32(bits, 32 - static_cast<int64_t>(n)) - 64u;
		bits  = Shl32(bits, static_cast<int64_t>(n));
		Refill();
		return true;
	}
};

struct BitReader2 {
	size_t  p       = 0;
	size_t  p_end   = 0;
	int32_t bit_pos = 0;
};

constexpr uint32_t Reverse11(uint32_t v) noexcept {
	uint32_t r = 0u;
	for (int i = 0; i < 11; ++i) {
		r = (r << 1u) | (v & 1u);
		v >>= 1u;
	}
	return r;
}

struct HuffLut {
	std::array<uint8_t, 2048 + 16> bits2len {};
	std::array<uint8_t, 2048 + 16> bits2sym {};
};

bool HuffMakeLut(const std::array<uint32_t, 12>& prefix_cur,
                 HuffLut&                        lut,
                 const std::array<uint8_t, 1280>& syms) noexcept {
	size_t currslot = 0;
	for (size_t i = 1; i < 11u; ++i) {
		const size_t start = kCodePrefixOrg[i];
		const size_t count = prefix_cur[i] - kCodePrefixOrg[i];
		if (count != 0u) {
			const size_t stepsize   = size_t {1} << (11u - i);
			const size_t num_to_set = count << (11u - i);
			if (currslot + num_to_set > 2048u) {
				return false;
			}
			std::fill_n(lut.bits2len.begin() + currslot, num_to_set, static_cast<uint8_t>(i));
			size_t p = currslot;
			for (size_t j = 0; j < count; ++j) {
				std::fill_n(lut.bits2sym.begin() + p, stepsize, syms[start + j]);
				p += stepsize;
			}
			currslot += num_to_set;
		}
	}
	const size_t num_to_set = prefix_cur[11] - kCodePrefixOrg[11];
	if (num_to_set != 0u) {
		if (currslot + num_to_set > 2048u) {
			return false;
		}
		const size_t start = kCodePrefixOrg[11];
		std::fill_n(lut.bits2len.begin() + currslot, num_to_set, uint8_t {11});
		std::copy_n(syms.begin() + start, num_to_set, lut.bits2sym.begin() + currslot);
		currslot += num_to_set;
	}
	return currslot == 2048u;
}

int32_t HuffReadCodeLengthsOld(BitReader&                bits,
                               std::array<uint8_t, 1280>& syms,
                               std::array<uint32_t, 12>&  code_prefix) noexcept {
	if (bits.ReadBitNoRefill() != 0u) {
		int32_t        sym         = 0;
		int32_t        num_symbols = 0;
		int32_t        avg         = 32;
		const int32_t  forced      = static_cast<int32_t>(bits.ReadBitsNoRefill(2u));
		const uint32_t thres       = 1u << (31u - (20u >> static_cast<unsigned>(forced)));
		bool           skip        = bits.ReadBit() != 0u;
		for (;;) {
			if (!skip) {
				if ((bits.bits & 0xFF000000u) == 0u) {
					return -1;
				}
				sym += static_cast<int32_t>(bits.ReadBitsNoRefill(2u * (Clz32(bits.bits) + 1u))) - 1;
				if (sym >= 256) {
					break;
				}
			}
			skip = false;
			bits.Refill();
			if ((bits.bits & 0xFF000000u) == 0u) {
				return -1;
			}
			int32_t n = static_cast<int32_t>(bits.ReadBitsNoRefill(2u * (Clz32(bits.bits) + 1u))) - 1;
			if (sym + n > 256 || n < 0) {
				return -1;
			}
			bits.Refill();
			num_symbols += n;
			while (n != 0) {
				--n;
				if (bits.bits < thres) {
					return -1;
				}
				const uint32_t lz  = Clz32(bits.bits);
				const int32_t  v   = static_cast<int32_t>(
                    bits.ReadBitsNoRefill(lz + static_cast<uint32_t>(forced) + 1u));
				const int32_t  vv  = v + (static_cast<int32_t>(lz) - 1) * (1 << forced);
				// Arithmetic shift preserves sign in C++20 two's complement.
				const int32_t  codelen = (-(vv & 1) ^ (vv >> 1)) + ((avg + 2) >> 2);
				if (codelen < 1 || codelen > 11) {
					return -1;
				}
				avg = codelen + ((3 * avg + 2) >> 2);
				bits.Refill();
				const uint32_t cp = code_prefix[static_cast<size_t>(codelen)];
				code_prefix[static_cast<size_t>(codelen)] = cp + 1u;
				if (cp >= syms.size()) {
					return -1;
				}
				syms[cp] = static_cast<uint8_t>(sym);
				sym += 1;
			}
			if (sym == 256) {
				break;
			}
		}
		return (sym == 256 && num_symbols >= 2) ? num_symbols : -1;
	}

	const int32_t num_symbols = static_cast<int32_t>(bits.ReadBitsNoRefill(8u));
	if (num_symbols == 0) {
		return -1;
	}
	if (num_symbols == 1) {
		syms[0] = static_cast<uint8_t>(bits.ReadBitsNoRefill(8u));
	} else {
		const uint32_t codelen_bits = bits.ReadBitsNoRefill(3u);
		if (codelen_bits > 4u) {
			return -1;
		}
		for (int32_t i = 0; i < num_symbols; ++i) {
			bits.Refill();
			const uint32_t sym     = bits.ReadBitsNoRefill(8u);
			const uint32_t codelen = bits.ReadBitsNoRefillZero(codelen_bits) + 1u;
			if (codelen > 11u) {
				return -1;
			}
			const uint32_t cp        = code_prefix[codelen];
			code_prefix[codelen]     = cp + 1u;
			if (cp >= syms.size()) {
				return -1;
			}
			syms[cp] = static_cast<uint8_t>(sym);
		}
	}
	return num_symbols;
}

bool DecodeGolombRiceLengths(std::span<const uint8_t> src,
                             std::span<uint8_t>       dst,
                             size_t                   size,
                             BitReader2&              br) noexcept {
	size_t p       = br.p;
	size_t p_end   = br.p_end;
	size_t dst_pos = 0;
	if (p >= p_end) {
		return false;
	}
	int32_t  count = -br.bit_pos;
	uint32_t v     = AtByte(src, p) & Shr32(255u, std::max(0, br.bit_pos));
	p += 1u;
	for (;;) {
		if (v == 0u) {
			count += 8;
		} else {
			const uint32_t x = kRiceVal[v];
			WriteLe32Padded(dst, dst_pos, static_cast<uint32_t>(count) + (x & 0x0F0F0F0Fu));
			WriteLe32Padded(dst, dst_pos + 4u, (x >> 4u) & 0x0F0F0F0Fu);
			dst_pos += kRiceLen[v];
			if (dst_pos >= size) {
				break;
			}
			count = static_cast<int32_t>(x >> 28u);
		}
		if (p >= p_end) {
			return false;
		}
		v = AtByte(src, p);
		p += 1u;
	}
	if (dst_pos > size) {
		for (size_t i = 0; i < dst_pos - size; ++i) {
			v &= v - 1u;
		}
	}
	int32_t bitpos = 0;
	if ((v & 1u) == 0u) {
		p -= 1u;
		bitpos = 8 - static_cast<int32_t>(Bsf32(v));
	}
	br.p       = p;
	br.bit_pos = bitpos;
	return true;
}

bool DecodeGolombRiceBits(std::span<const uint8_t> src,
                          std::span<uint8_t>       dst,
                          size_t                   size,
                          int32_t                  bitcount,
                          BitReader2&              br) noexcept {
	if (bitcount == 0) {
		return true;
	}
	size_t        dst_pos        = 0;
	size_t        p              = br.p;
	const int32_t bitpos         = br.bit_pos;
	const size_t  bits_required  = static_cast<size_t>(std::max(0, bitpos)) +
	                               static_cast<size_t>(bitcount) * size;
	const size_t  bytes_required = (bits_required + 7u) / 8u;
	if (p > br.p_end || bytes_required > (br.p_end - p)) {
		return false;
	}
	br.p       = p + (bits_required >> 3u);
	br.bit_pos = static_cast<int32_t>(bits_required & 7u);
	const uint64_t bak = ReadLe64Padded(dst, size);
	while (dst_pos < size) {
		const uint64_t cur    = ReadLe64Padded(dst, dst_pos);
		uint64_t       spread = 0;
		if (bitcount == 1) {
			uint64_t bits = static_cast<uint8_t>(
			    Shr32(ByteSwap32(ReadLe32Padded(src, p)), 24 - bitpos));
			p += 1u;
			bits   = (bits | (bits << 28u)) & 0xF0000000Full;
			bits   = (bits | (bits << 14u)) & 0x3000300030003ull;
			bits   = (bits | (bits << 7u)) & 0x0101010101010101ull;
			spread = cur * 2ull + ByteSwap64(bits);
		} else if (bitcount == 2) {
			uint64_t bits = static_cast<uint16_t>(
			    Shr32(ByteSwap32(ReadLe32Padded(src, p)), 16 - bitpos));
			p += 2u;
			bits   = (bits | (bits << 24u)) & 0xFF000000FFull;
			bits   = (bits | (bits << 12u)) & 0xF000F000F000Full;
			bits   = (bits | (bits << 6u)) & 0x0303030303030303ull;
			spread = cur * 4ull + ByteSwap64(bits);
		} else {
			uint64_t bits = Shr32(ByteSwap32(ReadLe32Padded(src, p)), 8 - bitpos) & 0xFFFFFFu;
			p += 3u;
			bits   = (bits | (bits << 20u)) & 0xFFF00000FFFull;
			bits   = (bits | (bits << 10u)) & 0x3F003F003F003Full;
			bits   = (bits | (bits << 5u)) & 0x0707070707070707ull;
			spread = cur * 8ull + ByteSwap64(bits);
		}
		WriteLe64Padded(dst, dst_pos, spread);
		dst_pos += 8u;
	}
	WriteLe64Padded(dst, size, bak);
	return true;
}

int32_t HuffConvertToRanges(std::array<std::pair<int32_t, int32_t>, 128>& range,
                            int32_t                                       num_symbols,
                            int32_t                                       p,
                            std::span<const uint8_t>                      symlen,
                            size_t                                        start,
                            BitReader&                                    bits) noexcept {
	const int32_t num_ranges = p >> 1;
	int32_t       sym_idx    = 0;
	size_t        off        = start;
	if ((p & 1) != 0) {
		bits.Refill();
		const int32_t v = symlen[off];
		off += 1u;
		if (v >= 8) {
			return -1;
		}
		const uint32_t n = static_cast<uint32_t>(v + 1);
		sym_idx          = static_cast<int32_t>(bits.ReadBitsNoRefill(n) + (1u << n) - 1u);
	}
	if (num_ranges < 0 || static_cast<size_t>(num_ranges) >= range.size()) {
		return -1;
	}
	int32_t syms_used = 0;
	for (int32_t i = 0; i < num_ranges; ++i) {
		bits.Refill();
		const uint32_t v = symlen[off];
		if (v >= 9u) {
			return -1;
		}
		const int32_t  num = static_cast<int32_t>(bits.ReadBitsNoRefillZero(v) + (1u << v));
		const uint32_t v2  = symlen[off + 1u];
		if (v2 >= 8u) {
			return -1;
		}
		const int32_t space =
		    static_cast<int32_t>(bits.ReadBitsNoRefill(v2 + 1u) + (1u << (v2 + 1u)) - 1u);
		range[static_cast<size_t>(i)] = {sym_idx, num};
		syms_used += num;
		sym_idx += num + space;
		off += 2u;
	}
	if (sym_idx >= 256 || syms_used >= num_symbols || sym_idx + num_symbols - syms_used > 256) {
		return -1;
	}
	range[static_cast<size_t>(num_ranges)] = {sym_idx, num_symbols - syms_used};
	return num_ranges + 1;
}

int32_t HuffReadCodeLengthsNew(BitReader&                bits,
                               std::array<uint8_t, 1280>& syms,
                               std::array<uint32_t, 12>&  code_prefix) noexcept {
	const int32_t forced_bits = static_cast<int32_t>(bits.ReadBitsNoRefill(2u));
	const int32_t num_symbols = static_cast<int32_t>(bits.ReadBitsNoRefill(8u)) + 1;
	const int32_t fluff       = bits.ReadFluff(num_symbols);
	std::array<uint8_t, 512 + 16> code_len {};
	const int32_t                 adj = (24 - bits.bit_pos + 7) >> 3;
	BitReader2                    br2 {
        static_cast<size_t>(static_cast<int64_t>(bits.p) - static_cast<int64_t>(adj)),
        bits.bound,
        (bits.bit_pos - 24) & 7,
    };
	if (!DecodeGolombRiceLengths(bits.b, code_len, static_cast<size_t>(num_symbols + fluff), br2)) {
		return -1;
	}
	if (!DecodeGolombRiceBits(bits.b, code_len, static_cast<size_t>(num_symbols), forced_bits, br2)) {
		return -1;
	}
	bits.bit_pos = 24;
	bits.p       = br2.p;
	bits.bits    = 0u;
	bits.Refill();
	bits.bits = Shl32(bits.bits, br2.bit_pos);
	bits.bit_pos += br2.bit_pos;

	uint32_t running_sum = 0x1Eu;
	for (int32_t i = 0; i < num_symbols; ++i) {
		int32_t v  = code_len[static_cast<size_t>(i)];
		v          = -(v & 1) ^ (v >> 1);
		const int32_t cl = v + static_cast<int32_t>(running_sum >> 2u) + 1;
		if (cl < 1 || cl > 11) {
			return -1;
		}
		code_len[static_cast<size_t>(i)] = static_cast<uint8_t>(cl);
		running_sum                      = static_cast<uint32_t>(static_cast<int32_t>(running_sum) + v);
	}

	std::array<std::pair<int32_t, int32_t>, 128> ranges {};
	const int32_t n_ranges =
	    HuffConvertToRanges(ranges, num_symbols, fluff, code_len, static_cast<size_t>(num_symbols), bits);
	if (n_ranges <= 0) {
		return -1;
	}
	size_t cp = 0;
	for (int32_t i = 0; i < n_ranges; ++i) {
		auto [sym, nn] = ranges[static_cast<size_t>(i)];
		while (nn != 0) {
			--nn;
			const size_t   clen = code_len[cp++];
			const uint32_t slot = code_prefix[clen];
			code_prefix[clen]   = slot + 1u;
			if (slot >= syms.size()) {
				return -1;
			}
			syms[slot] = static_cast<uint8_t>(sym);
			sym += 1;
		}
	}
	return num_symbols;
}

bool DecodeBytesCore(std::span<const uint8_t> src,
                     std::span<uint8_t>       out,
                     size_t                   out_off,
                     size_t                   out_end,
                     size_t                   src_off,
                     size_t                   src_end0,
                     size_t                   src_mid_org,
                     const HuffLut&           lut) noexcept {
	size_t   src_i          = src_off;
	uint32_t src_bits       = 0;
	int32_t  src_bitpos     = 0;
	size_t   src_mid        = src_mid_org;
	uint32_t src_mid_bits   = 0;
	int32_t  src_mid_bitpos = 0;
	size_t   src_end        = src_end0;
	uint32_t src_end_bits   = 0;
	int32_t  src_end_bitpos = 0;
	size_t   dst            = out_off;

	if (src_i > src_mid || src_mid > src_end || src_end > src.size()) {
		return false;
	}
	while (dst < out_end) {
		const int64_t bp = static_cast<int64_t>(static_cast<uint32_t>(src_bitpos) & 31u);
		if (src_mid - src_i <= 1u) {
			if (src_mid - src_i == 1u) {
				src_bits |= Shl32(src[src_i], bp);
			}
		} else {
			src_bits |= Shl32(static_cast<uint32_t>(src[src_i]) |
			                      (static_cast<uint32_t>(src[src_i + 1u]) << 8u),
			                  bp);
		}
		uint32_t k = src_bits & 0x7FFu;
		int32_t  n = lut.bits2len[k];
		src_bitpos -= n;
		src_bits = Shr32(src_bits, n);
		out[dst++] = lut.bits2sym[k];
		src_i += static_cast<size_t>((7 - src_bitpos) >> 3);
		src_bitpos &= 7;

		if (dst < out_end) {
			const int64_t ebp = static_cast<int64_t>(static_cast<uint32_t>(src_end_bitpos) & 31u);
			const int64_t mbp = static_cast<int64_t>(static_cast<uint32_t>(src_mid_bitpos) & 31u);
			if (src_end - src_mid <= 1u) {
				if (src_end - src_mid == 1u) {
					src_end_bits |= Shl32(src[src_mid], ebp);
					src_mid_bits |= Shl32(src[src_mid], mbp);
				}
			} else {
				const uint32_t vv = static_cast<uint32_t>(src[src_end - 2u]) |
				                    (static_cast<uint32_t>(src[src_end - 1u]) << 8u);
				src_end_bits |= Shl32(((vv >> 8u) | (vv << 8u)) & 0xFFFFu, ebp);
				src_mid_bits |= Shl32(static_cast<uint32_t>(src[src_mid]) |
				                          (static_cast<uint32_t>(src[src_mid + 1u]) << 8u),
				                      mbp);
			}
			k          = src_end_bits & 0x7FFu;
			n          = lut.bits2len[k];
			out[dst++] = lut.bits2sym[k];
			src_end_bitpos -= n;
			src_end_bits      = Shr32(src_end_bits, n);
			const size_t step = static_cast<size_t>((7 - src_end_bitpos) >> 3);
			if (step > src_end) {
				return false;
			}
			src_end -= step;
			src_end_bitpos &= 7;

			if (dst < out_end) {
				k          = src_mid_bits & 0x7FFu;
				n          = lut.bits2len[k];
				out[dst++] = lut.bits2sym[k];
				src_mid_bitpos -= n;
				src_mid_bits = Shr32(src_mid_bits, n);
				src_mid += static_cast<size_t>((7 - src_mid_bitpos) >> 3);
				src_mid_bitpos &= 7;
			}
		}
		if (src_i > src_mid || src_mid > src_end) {
			return false;
		}
	}
	return src_i == src_mid_org && src_end == src_mid;
}

bool DecodeHuff(std::span<const uint8_t> src, std::span<uint8_t> output, uint32_t type_div) {
	auto                       prefix = kCodePrefixOrg;
	std::array<uint8_t, 1280>  syms {};
	BitReader                  bits = BitReader::Forward(src, 0u, src.size());
	int32_t                    num_syms = -1;
	if (bits.ReadBitNoRefill() == 0u) {
		num_syms = HuffReadCodeLengthsOld(bits, syms, prefix);
	} else if (bits.ReadBitNoRefill() == 0u) {
		num_syms = HuffReadCodeLengthsNew(bits, syms, prefix);
	} else {
		return false;
	}
	if (num_syms < 1) {
		return false;
	}
	const int32_t adj = (24 - bits.bit_pos) / 8;
	const int64_t sp0 = static_cast<int64_t>(bits.p) - static_cast<int64_t>(adj);
	if (sp0 < 0) {
		return false;
	}
	size_t sp = static_cast<size_t>(sp0);
	if (num_syms == 1) {
		std::fill(output.begin(), output.end(), syms[0]);
		return true;
	}
	HuffLut lut;
	if (!HuffMakeLut(prefix, lut, syms)) {
		return false;
	}
	HuffLut rev;
	for (uint32_t i = 0; i < 2048u; ++i) {
		const uint32_t r = Reverse11(i);
		rev.bits2len[i]  = lut.bits2len[r];
		rev.bits2sym[i]  = lut.bits2sym[r];
	}
	if (type_div == 1u) {
		if (sp + 3u > src.size()) {
			return false;
		}
		const size_t split_mid = static_cast<size_t>(src[sp]) |
		                         (static_cast<size_t>(src[sp + 1u]) << 8u);
		sp += 2u;
		return DecodeBytesCore(src, output, 0u, output.size(), sp, src.size(), sp + split_mid, rev);
	}
	if (sp + 6u > src.size()) {
		return false;
	}
	const size_t half      = (output.size() + 1u) >> 1u;
	const size_t split_mid = static_cast<size_t>(src[sp]) |
	                         (static_cast<size_t>(src[sp + 1u]) << 8u) |
	                         (static_cast<size_t>(src[sp + 2u]) << 16u);
	sp += 3u;
	if (split_mid > src.size() - sp) {
		return false;
	}
	const size_t src_mid    = sp + split_mid;
	const size_t split_left = static_cast<size_t>(src[sp]) |
	                          (static_cast<size_t>(src[sp + 1u]) << 8u);
	sp += 2u;
	if (src_mid < sp || (src_mid - sp) < split_left + 2u || (src.size() - src_mid) < 3u) {
		return false;
	}
	const size_t split_right = static_cast<size_t>(src[src_mid]) |
	                           (static_cast<size_t>(src[src_mid + 1u]) << 8u);
	if (src.size() - (src_mid + 2u) < split_right + 2u) {
		return false;
	}
	return DecodeBytesCore(src, output, 0u, half, sp, src_mid, sp + split_left, rev) &&
	       DecodeBytesCore(src, output, half, output.size(), src_mid + 2u, src.size(),
	                       src_mid + 2u + split_right, rev);
}

KrakenResult DecodeBytes(std::span<const uint8_t> src,
                         size_t                   output_cap,
                         size_t&                  used_out,
                         std::vector<uint8_t>&    decoded_out) {
	if (src.size() < 2u) {
		return {KrakenStatus::Malformed, 0u};
	}
	const uint32_t chunk_type = (src[0] >> 4u) & 7u;
	if (chunk_type == 0u) {
		size_t size = 0;
		size_t sp   = 0;
		if (src[0] >= 0x80u) {
			size = ((static_cast<size_t>(src[0]) << 8u) | src[1]) & 0xFFFu;
			sp   = 2u;
		} else {
			if (src.size() < 3u) {
				return {KrakenStatus::Malformed, 0u};
			}
			size = (static_cast<size_t>(src[0]) << 16u) |
			       (static_cast<size_t>(src[1]) << 8u) |
			       static_cast<size_t>(src[2]);
			if ((size & ~size_t {0x3FFFFu}) != 0u) {
				return {KrakenStatus::Malformed, 0u};
			}
			sp = 3u;
		}
		if (size > output_cap || (src.size() - sp) < size) {
			return {KrakenStatus::Malformed, 0u};
		}
		decoded_out.assign(src.begin() + static_cast<ptrdiff_t>(sp),
		                   src.begin() + static_cast<ptrdiff_t>(sp + size));
		used_out = sp + size;
		return {KrakenStatus::Ok, 0u};
	}

	size_t src_size = 0;
	size_t dst_size = 0;
	size_t sp       = 0;
	if (src[0] >= 0x80u) {
		if (src.size() < 3u) {
			return {KrakenStatus::Malformed, 0u};
		}
		const uint32_t bits = (static_cast<uint32_t>(src[0]) << 16u) |
		                      (static_cast<uint32_t>(src[1]) << 8u) |
		                      static_cast<uint32_t>(src[2]);
		src_size = bits & 0x3FFu;
		dst_size = src_size + ((bits >> 10u) & 0x3FFu) + 1u;
		sp       = 3u;
	} else {
		if (src.size() < 5u) {
			return {KrakenStatus::Malformed, 0u};
		}
		const uint32_t bits = (static_cast<uint32_t>(src[1]) << 24u) |
		                      (static_cast<uint32_t>(src[2]) << 16u) |
		                      (static_cast<uint32_t>(src[3]) << 8u) |
		                      static_cast<uint32_t>(src[4]);
		src_size = bits & 0x3FFFFu;
		dst_size = (((bits >> 18u) | (static_cast<uint32_t>(src[0]) << 14u)) & 0x3FFFFu) + 1u;
		if (src_size >= dst_size) {
			return {KrakenStatus::Malformed, 0u};
		}
		sp = 5u;
	}

	if ((src.size() - sp) < src_size || dst_size > output_cap) {
		return {KrakenStatus::Malformed, 0u};
	}

	decoded_out.assign(dst_size, 0u);
	if (chunk_type == 2u || chunk_type == 4u) {
		if (!DecodeHuff(src.subspan(sp, src_size), decoded_out, chunk_type >> 1u)) {
			return {KrakenStatus::Malformed, 0u};
		}
	} else {
		return {KrakenStatus::UnsupportedEntropy, chunk_type};
	}
	used_out = sp + src_size;
	return {KrakenStatus::Ok, 0u};
}

struct LzTable {
	std::vector<uint8_t> lit;
	std::vector<uint8_t> cmd;
	std::vector<int32_t> offs;
	std::vector<int32_t> lens;
};

bool UnpackOffsets(std::span<const uint8_t> src,
                   size_t                   bs_begin,
                   size_t                   bs_end,
                   bool                     excess_flag,
                   size_t                   excess_count,
                   std::span<const uint8_t> packed_offs,
                   int32_t                  offs_scaling,
                   std::span<const uint8_t> packed_extra,
                   std::span<const uint8_t> packed_len,
                   std::span<int32_t>       offs_out,
                   std::span<int32_t>       len_out) noexcept {
	BitReader a       = BitReader::Forward(src, bs_begin, bs_end);
	BitReader b       = BitReader::Backward(src, bs_begin, bs_end);
	size_t    u32_len = 0;
	if (!excess_flag) {
		if (b.bits < 0x2000u) {
			return false;
		}
		uint32_t nn = 31u - Bsr32(b.bits);
		b.bit_pos += static_cast<int32_t>(nn);
		b.bits = Shl32(b.bits, static_cast<int64_t>(nn));
		b.RefillB();
		nn += 1u;
		u32_len = Shr32(b.bits, 32 - static_cast<int64_t>(nn)) - 1u;
		b.bit_pos += static_cast<int32_t>(nn);
		b.bits = Shl32(b.bits, static_cast<int64_t>(nn));
		b.RefillB();
	} else {
		u32_len = static_cast<size_t>(std::count(packed_len.begin(), packed_len.end(), uint8_t {255}));
	}
	if (u32_len > 512u) {
		return false;
	}

	if (offs_scaling == 0) {
		size_t i = 0;
		while (i < packed_offs.size()) {
			offs_out[i] = -static_cast<int32_t>(a.ReadDistance(packed_offs[i]));
			++i;
			if (i >= packed_offs.size()) {
				break;
			}
			offs_out[i] = -static_cast<int32_t>(b.ReadDistance(packed_offs[i]));
			++i;
		}
	} else {
		size_t i = 0;
		while (i < packed_offs.size()) {
			const uint32_t cmd = packed_offs[i];
			if ((cmd >> 3u) > 26u) {
				return false;
			}
			const uint32_t off = ((8u + (cmd & 7u)) << (cmd >> 3u)) | a.ReadMoreThan24(cmd >> 3u);
			offs_out[i]        = 8 - static_cast<int32_t>(off);
			++i;
			if (i >= packed_offs.size()) {
				break;
			}
			const uint32_t cmd2 = packed_offs[i];
			if ((cmd2 >> 3u) > 26u) {
				return false;
			}
			const uint32_t off2 = ((8u + (cmd2 & 7u)) << (cmd2 >> 3u)) | b.ReadMoreThan24(cmd2 >> 3u);
			offs_out[i]         = 8 - static_cast<int32_t>(off2);
			++i;
		}
		if (offs_scaling != 1) {
			if (packed_extra.size() != packed_offs.size()) {
				return false;
			}
			for (size_t k = 0; k < offs_out.size(); ++k) {
				const int32_t e   = static_cast<int8_t>(packed_extra[k]);
				const int64_t mul = static_cast<int64_t>(offs_out[k]) * static_cast<int64_t>(offs_scaling);
				offs_out[k]       = static_cast<int32_t>(static_cast<int64_t>(e) - mul);
			}
		}
	}

	std::array<uint32_t, 512> u32s {};
	if (u32_len > 0u) {
		BitReader ra = excess_flag ? BitReader::Forward(src, bs_end, bs_end + excess_count) : a;
		BitReader rb = excess_flag ? BitReader::Backward(src, bs_end, bs_end + excess_count) : b;
		size_t    i  = 0;
		while (i + 1u < u32_len) {
			if (!ra.ReadLength(u32s[i]) || !rb.ReadLength(u32s[i + 1u])) {
				return false;
			}
			i += 2u;
		}
		if (i < u32_len && !ra.ReadLength(u32s[i])) {
			return false;
		}
	}

	size_t u = 0;
	for (size_t i = 0; i < packed_len.size(); ++i) {
		uint32_t v = packed_len[i];
		if (v == 255u) {
			if (u >= u32_len) {
				return false;
			}
			v = u32s[u++] + 255u;
		}
		len_out[i] = static_cast<int32_t>(v + 3u);
	}
	return u == u32_len;
}

KrakenResult ReadLzTable(std::span<const uint8_t> src,
                         size_t&                  sp,
                         size_t                   dst_size,
                         LzTable&                 table_out) {
	size_t src_end      = src.size();
	bool   excess_flag  = false;
	size_t excess_count = 0;
	if (sp < src_end && (src[sp] & 0x80u) != 0u) {
		const uint8_t flag = src[sp];
		if ((flag & 0xC0u) == 0x80u) {
			sp += 1u;
			excess_flag  = true;
			excess_count = flag & 0x3Fu;
			if (excess_count > 0x1Fu) {
				if (sp >= src_end) {
					return {KrakenStatus::Malformed, 0u};
				}
				excess_count += static_cast<size_t>(src[sp]) * 0x20u;
				sp += 1u;
			}
			if (src_end < sp + excess_count) {
				return {KrakenStatus::Malformed, 0u};
			}
			src_end -= excess_count;
		}
	}

	auto region = [&](size_t pos) -> std::span<const uint8_t> {
		return (pos <= src_end) ? src.subspan(pos, src_end - pos) : std::span<const uint8_t> {};
	};

	size_t used = 0;
	if (sp > src_end) {
		return {KrakenStatus::Malformed, 0u};
	}
	if (auto r = DecodeBytes(region(sp), dst_size, used, table_out.lit); r.status != KrakenStatus::Ok) {
		return r;
	}
	sp += used;
	if (sp > src_end) {
		return {KrakenStatus::Malformed, 0u};
	}
	if (auto r = DecodeBytes(region(sp), dst_size, used, table_out.cmd); r.status != KrakenStatus::Ok) {
		return r;
	}
	sp += used;

	if (src_end < sp || (src_end - sp) < 3u) {
		return {KrakenStatus::Malformed, 0u};
	}

	int32_t              offs_scaling = 0;
	std::vector<uint8_t> packed_offs;
	std::vector<uint8_t> packed_extra;
	if ((src[sp] & 0x80u) != 0u) {
		offs_scaling = static_cast<int32_t>(src[sp]) - 127;
		sp += 1u;
		if (sp > src_end) {
			return {KrakenStatus::Malformed, 0u};
		}
		if (auto r = DecodeBytes(region(sp), table_out.cmd.size(), used, packed_offs);
		    r.status != KrakenStatus::Ok) {
			return r;
		}
		sp += used;
		if (offs_scaling != 1) {
			if (sp > src_end) {
				return {KrakenStatus::Malformed, 0u};
			}
			if (auto r = DecodeBytes(region(sp), packed_offs.size(), used, packed_extra);
			    r.status != KrakenStatus::Ok) {
				return r;
			}
			if (packed_extra.size() != packed_offs.size()) {
				return {KrakenStatus::Malformed, 0u};
			}
			sp += used;
		}
	} else {
		if (auto r = DecodeBytes(region(sp), table_out.cmd.size(), used, packed_offs);
		    r.status != KrakenStatus::Ok) {
			return r;
		}
		sp += used;
	}

	if (sp > src_end) {
		return {KrakenStatus::Malformed, 0u};
	}
	std::vector<uint8_t> packed_len;
	if (auto r = DecodeBytes(region(sp), dst_size >> 2u, used, packed_len);
	    r.status != KrakenStatus::Ok) {
		return r;
	}
	sp += used;

	table_out.offs.assign(packed_offs.size(), 0);
	table_out.lens.assign(packed_len.size(), 0);
	if (!UnpackOffsets(src, sp, src_end, excess_flag, excess_count, packed_offs, offs_scaling,
	                   packed_extra, packed_len, table_out.offs, table_out.lens)) {
		return {KrakenStatus::Malformed, 0u};
	}
	return {KrakenStatus::Ok, 0u};
}

void CopyMatch(std::span<uint8_t> dst, size_t from, size_t to, size_t len) noexcept {
	for (size_t c = 0; c < len; ++c) {
		dst[to + c] = dst[from + c];
	}
}

bool ProcessType1(const LzTable& t, std::span<uint8_t> dst, size_t start, size_t dst_end) noexcept {
	size_t cmd_i   = 0;
	size_t len_i   = 0;
	size_t lit_i   = 0;
	size_t offs_i  = 0;
	size_t dst_pos = start;
	std::array<int32_t, 7> recent = {0, 0, 0, -8, -8, -8, 0};

	while (cmd_i < t.cmd.size()) {
		const uint32_t f          = t.cmd[cmd_i++];
		size_t         litlen     = f & 3u;
		const size_t   offs_index = f >> 6u;
		const uint32_t matchlen   = (f >> 2u) & 0xFu;
		if (litlen == 3u) {
			if (len_i >= t.lens.size() || t.lens[len_i] < 0) {
				return false;
			}
			litlen = static_cast<size_t>(t.lens[len_i++]);
		}
		recent[6] = (offs_i < t.offs.size()) ? t.offs[offs_i] : 0;
		if (litlen != 0u) {
			if (lit_i + litlen > t.lit.size() || dst_pos + litlen > dst_end) {
				return false;
			}
			std::copy_n(t.lit.begin() + static_cast<ptrdiff_t>(lit_i), litlen,
			            dst.begin() + static_cast<ptrdiff_t>(dst_pos));
			dst_pos += litlen;
			lit_i += litlen;
		}
		const int32_t offset      = recent[offs_index + 3u];
		recent[offs_index + 3u]   = recent[offs_index + 2u];
		recent[offs_index + 2u]   = recent[offs_index + 1u];
		recent[offs_index + 1u]   = recent[offs_index];
		recent[3]                 = offset;
		offs_i += ((offs_index + 1u) & 4u) >> 2u;

		const int64_t from = static_cast<int64_t>(dst_pos) + static_cast<int64_t>(offset);
		if (from < 0 || static_cast<size_t>(from) >= dst_pos) {
			return false;
		}
		size_t actual = 0;
		if (matchlen != 15u) {
			actual = matchlen + 2u;
		} else {
			if (len_i >= t.lens.size() || t.lens[len_i] < 0) {
				return false;
			}
			actual = 14u + static_cast<size_t>(t.lens[len_i++]);
		}
		if (dst_pos + actual > dst_end || static_cast<size_t>(from) + actual > dst.size()) {
			return false;
		}
		CopyMatch(dst, static_cast<size_t>(from), dst_pos, actual);
		dst_pos += actual;
	}

	if (offs_i != t.offs.size() || len_i != t.lens.size()) {
		return false;
	}
	const size_t final_len = dst_end - dst_pos;
	if (final_len != (t.lit.size() - lit_i)) {
		return false;
	}
	std::copy(t.lit.begin() + static_cast<ptrdiff_t>(lit_i), t.lit.end(),
	          dst.begin() + static_cast<ptrdiff_t>(dst_pos));
	return true;
}

bool ProcessType0(const LzTable& t, std::span<uint8_t> dst, size_t start, size_t dst_end) noexcept {
	size_t                 cmd_i       = 0;
	size_t                 len_i       = 0;
	size_t                 lit_i       = 0;
	size_t                 offs_i      = 0;
	size_t                 dst_pos     = start;
	std::array<int32_t, 7> recent      = {0, 0, 0, -8, -8, -8, 0};
	int32_t                last_offset = -8;

	auto delta_literals = [&](size_t at, std::span<const uint8_t> lit) noexcept {
		for (size_t c = 0; c < lit.size(); ++c) {
			const int64_t back = static_cast<int64_t>(at + c) + static_cast<int64_t>(last_offset);
			const uint8_t prev = (back >= 0 && static_cast<size_t>(back) < dst.size())
			                         ? dst[static_cast<size_t>(back)]
			                         : 0u;
			dst[at + c]        = static_cast<uint8_t>(lit[c] + prev);
		}
	};

	while (cmd_i < t.cmd.size()) {
		const uint32_t f          = t.cmd[cmd_i++];
		size_t         litlen     = f & 3u;
		const size_t   offs_index = f >> 6u;
		const uint32_t matchlen   = (f >> 2u) & 0xFu;
		if (litlen == 3u) {
			if (len_i >= t.lens.size() || t.lens[len_i] < 0) {
				return false;
			}
			litlen = static_cast<size_t>(t.lens[len_i++]);
		}
		recent[6] = (offs_i < t.offs.size()) ? t.offs[offs_i] : 0;
		if (litlen != 0u) {
			if (lit_i + litlen > t.lit.size() || dst_pos + litlen > dst_end) {
				return false;
			}
			delta_literals(dst_pos, std::span<const uint8_t>(t.lit).subspan(lit_i, litlen));
			dst_pos += litlen;
			lit_i += litlen;
		}
		const int32_t offset      = recent[offs_index + 3u];
		recent[offs_index + 3u]   = recent[offs_index + 2u];
		recent[offs_index + 2u]   = recent[offs_index + 1u];
		recent[offs_index + 1u]   = recent[offs_index];
		recent[3]                 = offset;
		last_offset               = offset;
		offs_i += ((offs_index + 1u) & 4u) >> 2u;

		size_t actual = 0;
		if (matchlen != 15u) {
			actual = matchlen + 2u;
		} else {
			if (len_i >= t.lens.size() || t.lens[len_i] < 0) {
				return false;
			}
			actual = 14u + static_cast<size_t>(t.lens[len_i++]);
		}
		if (dst_pos + actual > dst_end) {
			return false;
		}
		const int64_t from = static_cast<int64_t>(dst_pos) + static_cast<int64_t>(offset);
		if (from < 0 || static_cast<size_t>(from) >= dst_pos) {
			return false;
		}
		CopyMatch(dst, static_cast<size_t>(from), dst_pos, actual);
		dst_pos += actual;
	}

	if (offs_i != t.offs.size() || len_i != t.lens.size()) {
		return false;
	}
	const size_t rest_len = t.lit.size() - lit_i;
	if (dst_pos + rest_len != dst_end) {
		return false;
	}
	delta_literals(dst_pos, std::span<const uint8_t>(t.lit).subspan(lit_i, rest_len));
	return true;
}

KrakenResult DecodeChunk(std::span<const uint8_t> src,
                         std::span<uint8_t>       dst,
                         size_t                   dst_start,
                         size_t                   dst_len,
                         bool                     with_seed,
                         uint32_t                 literal_mode) {
	size_t sp = 0;
	if (with_seed) {
		if (src.size() < kSeedSize || dst_len < kSeedSize) {
			return {KrakenStatus::Malformed, 0u};
		}
		std::copy_n(src.begin(), kSeedSize, dst.begin() + static_cast<ptrdiff_t>(dst_start));
		sp = kSeedSize;
	}
	LzTable            table;
	const KrakenResult table_res = ReadLzTable(src, sp, dst_len, table);
	if (table_res.status != KrakenStatus::Ok) {
		return table_res;
	}
	const size_t start = dst_start + (with_seed ? kSeedSize : 0u);
	const size_t end   = dst_start + dst_len;
	const bool   ok    = (literal_mode == 1u) ? ProcessType1(table, dst, start, end)
	                   : (literal_mode == 0u) ? ProcessType0(table, dst, start, end)
	                                          : false;
	return ok ? KrakenResult {KrakenStatus::Ok, 0u} : KrakenResult {KrakenStatus::Malformed, 0u};
}

KrakenResult DecodeSubChunk(std::span<const uint8_t> src,
                            std::span<uint8_t>       dst,
                            size_t                   dst_start,
                            size_t                   dst_len,
                            bool                     lz,
                            bool                     restart,
                            uint32_t                 literal_mode) {
	if (dst_start + dst_len > dst.size()) {
		return {KrakenStatus::Malformed, 0u};
	}
	if (src.size() == dst_len) {
		std::copy_n(src.begin(), dst_len, dst.begin() + static_cast<ptrdiff_t>(dst_start));
		return {KrakenStatus::Ok, 0u};
	}
	if (lz) {
		return DecodeChunk(src, dst, dst_start, dst_len, restart, literal_mode);
	}
	size_t               used = 0;
	std::vector<uint8_t> decoded;
	const KrakenResult   res = DecodeBytes(src, dst_len, used, decoded);
	if (res.status != KrakenStatus::Ok) {
		return res;
	}
	if (used != src.size() || decoded.size() != dst_len) {
		return {KrakenStatus::Malformed, 0u};
	}
	std::copy_n(decoded.begin(), dst_len, dst.begin() + static_cast<ptrdiff_t>(dst_start));
	return {KrakenStatus::Ok, 0u};
}

} // namespace

KrakenResult DecodeKrakenBlock(std::span<const uint8_t> payload,
                               uint32_t                 flags,
                               uint32_t                 first_chunk_comp,
                               std::span<uint8_t>       dst) {
	if (dst.empty()) {
		return payload.empty() ? KrakenResult {KrakenStatus::Ok, 0u}
		                       : KrakenResult {KrakenStatus::Malformed, 0u};
	}
	auto sub_lit = [&](uint32_t bit) noexcept -> uint32_t { return ((flags & bit) != 0u) ? 0u : 1u; };
	const bool one_chunk =
	    (first_chunk_comp == 0u) || (first_chunk_comp >= payload.size()) || (dst.size() <= kChunkMax);
	if (one_chunk) {
		return DecodeSubChunk(payload, dst, 0u, dst.size(), (flags & kChunk0NewLz) != 0u, true,
		                      sub_lit(kChunk0SubLit));
	}
	const size_t chunk0_dst  = std::min(dst.size(), kChunkMax);
	const size_t chunk1_dst  = dst.size() - chunk0_dst;
	const size_t chunk0_comp = first_chunk_comp;
	const size_t chunk1_comp = payload.size() - chunk0_comp;
	if (chunk1_dst == 0u && chunk1_comp != 0u) {
		return {KrakenStatus::Malformed, 0u};
	}
	const KrakenResult r0 = DecodeSubChunk(payload.subspan(0u, chunk0_comp), dst, 0u, chunk0_dst,
	                                       (flags & kChunk0NewLz) != 0u, true, sub_lit(kChunk0SubLit));
	if (r0.status != KrakenStatus::Ok) {
		return r0;
	}
	if (chunk1_dst > 0u) {
		return DecodeSubChunk(payload.subspan(chunk0_comp), dst, chunk0_dst, chunk1_dst,
		                      (flags & kChunk1NewLz) != 0u, (flags & kChunk1Restart) != 0u,
		                      sub_lit(kChunk1SubLit));
	}
	return {KrakenStatus::Ok, 0u};
}

} // namespace Common::Pkg
