#include "common/pkg/PkgCrypto.h"

#include <algorithm>
#include <cstring>

namespace Common::Pkg {

namespace {

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr uint8_t kAesSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

constexpr uint8_t kAesInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

constexpr uint8_t kAesRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

constexpr uint32_t Ror32(uint32_t x, uint32_t n) noexcept {
	return (x >> n) | (x << (32u - n));
}

constexpr uint32_t ReadBe32(const uint8_t* p) noexcept {
	return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u) |
	       (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
}

constexpr void WriteBe32(uint8_t* p, uint32_t v) noexcept {
	p[0] = static_cast<uint8_t>((v >> 24u) & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 16u) & 0xffu);
	p[2] = static_cast<uint8_t>((v >> 8u) & 0xffu);
	p[3] = static_cast<uint8_t>(v & 0xffu);
}

constexpr uint8_t GfMul(uint8_t a, uint8_t b) noexcept {
	uint8_t res = 0;
	for (int i = 0; i < 8; ++i) {
		if ((b & 1u) != 0u) {
			res ^= a;
		}
		const uint8_t hi = static_cast<uint8_t>(a & 0x80u);
		a                = static_cast<uint8_t>(a << 1u);
		if (hi != 0u) {
			a ^= 0x1bu;
		}
		b = static_cast<uint8_t>(b >> 1u);
	}
	return res;
}

inline uint8_t Xtime(uint8_t x) noexcept {
	return static_cast<uint8_t>((x << 1u) ^ (((x >> 7u) & 1u) * 0x1bu));
}

struct Sha256State {
	uint32_t state[8] = {
	    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
	    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};
	uint64_t bitlen = 0;
	uint8_t  buf[64] {};
	size_t   buf_len = 0;

	void Transform(const uint8_t block[64]) noexcept {
		uint32_t w[64] {};
		for (int i = 0; i < 16; ++i) {
			w[i] = ReadBe32(block + i * 4);
		}
		for (int i = 16; i < 64; ++i) {
			const uint32_t s0 = Ror32(w[i - 15], 7u) ^ Ror32(w[i - 15], 18u) ^ (w[i - 15] >> 3u);
			const uint32_t s1 = Ror32(w[i - 2], 17u) ^ Ror32(w[i - 2], 19u) ^ (w[i - 2] >> 10u);
			w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
		for (int i = 0; i < 64; ++i) {
			const uint32_t S1    = Ror32(e, 6u) ^ Ror32(e, 11u) ^ Ror32(e, 25u);
			const uint32_t ch    = (e & f) ^ ((~e) & g);
			const uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
			const uint32_t S0    = Ror32(a, 2u) ^ Ror32(a, 13u) ^ Ror32(a, 22u);
			const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t temp2 = S0 + maj;
			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}
		state[0] += a;
		state[1] += b;
		state[2] += c;
		state[3] += d;
		state[4] += e;
		state[5] += f;
		state[6] += g;
		state[7] += h;
	}

	void Update(std::span<const uint8_t> data) noexcept {
		const uint8_t* ptr = data.data();
		size_t         rem = data.size();
		bitlen += static_cast<uint64_t>(rem) * 8ull;
		if (buf_len > 0) {
			const size_t take = std::min(rem, size_t(64) - buf_len);
			std::memcpy(buf + buf_len, ptr, take);
			buf_len += take;
			ptr += take;
			rem -= take;
			if (buf_len == 64) {
				Transform(buf);
				buf_len = 0;
			}
		}
		while (rem >= 64) {
			Transform(ptr);
			ptr += 64;
			rem -= 64;
		}
		if (rem > 0) {
			std::memcpy(buf, ptr, rem);
			buf_len = rem;
		}
	}

	std::array<uint8_t, 32> Final() noexcept {
		buf[buf_len++] = 0x80u;
		if (buf_len > 56) {
			std::memset(buf + buf_len, 0, 64 - buf_len);
			Transform(buf);
			buf_len = 0;
		}
		std::memset(buf + buf_len, 0, 56 - buf_len);
		for (int i = 0; i < 8; ++i) {
			buf[56 + i] = static_cast<uint8_t>((bitlen >> ((7 - i) * 8)) & 0xffu);
		}
		Transform(buf);
		std::array<uint8_t, 32> digest {};
		for (int i = 0; i < 8; ++i) {
			WriteBe32(digest.data() + i * 4, state[i]);
		}
		return digest;
	}
};

inline void AddRoundKey(uint8_t state[16], const uint32_t* rk) noexcept {
	for (int col = 0; col < 4; ++col) {
		const uint32_t w = rk[col];
		state[col * 4 + 0] ^= static_cast<uint8_t>((w >> 24u) & 0xffu);
		state[col * 4 + 1] ^= static_cast<uint8_t>((w >> 16u) & 0xffu);
		state[col * 4 + 2] ^= static_cast<uint8_t>((w >> 8u) & 0xffu);
		state[col * 4 + 3] ^= static_cast<uint8_t>(w & 0xffu);
	}
}

inline void GfMultiplyTweak(uint8_t tweak[16]) noexcept {
	uint8_t carry = 0;
	for (int i = 0; i < 16; ++i) {
		const uint8_t next_carry = static_cast<uint8_t>((tweak[i] >> 7u) & 1u);
		tweak[i]                 = static_cast<uint8_t>((tweak[i] << 1u) | carry);
		carry                    = next_carry;
	}
	if (carry != 0u) {
		tweak[0] ^= 0x87u;
	}
}

} // namespace

Aes128Context::Aes128Context(std::span<const uint8_t, 16> key) { SetKey(key); }

void Aes128Context::SetKey(std::span<const uint8_t, 16> key) {
	for (int i = 0; i < 4; ++i) {
		m_enc_rk[i] = ReadBe32(key.data() + i * 4);
	}
	for (int i = 4; i < 44; ++i) {
		uint32_t temp = m_enc_rk[i - 1];
		if ((i % 4) == 0) {
			const uint8_t b0 = kAesSbox[(temp >> 16u) & 0xffu];
			const uint8_t b1 = kAesSbox[(temp >> 8u) & 0xffu];
			const uint8_t b2 = kAesSbox[temp & 0xffu];
			const uint8_t b3 = kAesSbox[(temp >> 24u) & 0xffu];
			temp             = (static_cast<uint32_t>(b0) << 24u) ^ (static_cast<uint32_t>(b1) << 16u) ^
			                   (static_cast<uint32_t>(b2) << 8u) ^ static_cast<uint32_t>(b3) ^
			                   (static_cast<uint32_t>(kAesRcon[i / 4]) << 24u);
		}
		m_enc_rk[i] = m_enc_rk[i - 4] ^ temp;
	}
	for (int i = 0; i < 44; i += 4) {
		const int rev = 40 - i;
		m_dec_rk[i + 0] = m_enc_rk[rev + 0];
		m_dec_rk[i + 1] = m_enc_rk[rev + 1];
		m_dec_rk[i + 2] = m_enc_rk[rev + 2];
		m_dec_rk[i + 3] = m_enc_rk[rev + 3];
	}
}

void Aes128Context::EncryptBlock(const uint8_t in[16], uint8_t out[16]) const noexcept {
	uint8_t s[16];
	std::memcpy(s, in, 16);
	AddRoundKey(s, m_enc_rk.data());

	for (int round = 1; round <= 10; ++round) {
		for (uint8_t& b: s) {
			b = kAesSbox[b];
		}
		// ShiftRows (column-major: index = col * 4 + row)
		uint8_t tmp = s[1];
		s[1] = s[5];
		s[5] = s[9];
		s[9] = s[13];
		s[13] = tmp;

		std::swap(s[2], s[10]);
		std::swap(s[6], s[14]);

		tmp   = s[15];
		s[15] = s[11];
		s[11] = s[7];
		s[7]  = s[3];
		s[3]  = tmp;

		if (round < 10) {
			for (int col = 0; col < 4; ++col) {
				uint8_t*      c  = s + col * 4;
				const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
				const uint8_t u  = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
				c[0]             = static_cast<uint8_t>(a0 ^ u ^ Xtime(static_cast<uint8_t>(a0 ^ a1)));
				c[1]             = static_cast<uint8_t>(a1 ^ u ^ Xtime(static_cast<uint8_t>(a1 ^ a2)));
				c[2]             = static_cast<uint8_t>(a2 ^ u ^ Xtime(static_cast<uint8_t>(a2 ^ a3)));
				c[3]             = static_cast<uint8_t>(a3 ^ u ^ Xtime(static_cast<uint8_t>(a3 ^ a0)));
			}
		}
		AddRoundKey(s, m_enc_rk.data() + round * 4);
	}
	std::memcpy(out, s, 16);
}

void Aes128Context::DecryptBlock(const uint8_t in[16], uint8_t out[16]) const noexcept {
	uint8_t s[16];
	std::memcpy(s, in, 16);
	AddRoundKey(s, m_dec_rk.data());

	for (int round = 1; round <= 10; ++round) {
		// InvShiftRows
		uint8_t tmp = s[13];
		s[13]       = s[9];
		s[9]        = s[5];
		s[5]        = s[1];
		s[1]        = tmp;

		std::swap(s[2], s[10]);
		std::swap(s[6], s[14]);

		tmp   = s[3];
		s[3]  = s[7];
		s[7]  = s[11];
		s[11] = s[15];
		s[15] = tmp;

		for (uint8_t& b: s) {
			b = kAesInvSbox[b];
		}
		AddRoundKey(s, m_dec_rk.data() + round * 4);

		if (round < 10) {
			for (int col = 0; col < 4; ++col) {
				uint8_t*      c  = s + col * 4;
				const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
				c[0] = GfMul(a0, 0x0e) ^ GfMul(a1, 0x0b) ^ GfMul(a2, 0x0d) ^ GfMul(a3, 0x09);
				c[1] = GfMul(a0, 0x09) ^ GfMul(a1, 0x0e) ^ GfMul(a2, 0x0b) ^ GfMul(a3, 0x0d);
				c[2] = GfMul(a0, 0x0d) ^ GfMul(a1, 0x09) ^ GfMul(a2, 0x0e) ^ GfMul(a3, 0x0b);
				c[3] = GfMul(a0, 0x0b) ^ GfMul(a1, 0x0d) ^ GfMul(a2, 0x09) ^ GfMul(a3, 0x0e);
			}
		}
	}
	std::memcpy(out, s, 16);
}

std::array<uint8_t, 32> Sha256(std::span<const uint8_t> data) {
	Sha256State ctx;
	ctx.Update(data);
	return ctx.Final();
}

std::array<uint8_t, 32> HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data) {
	std::array<uint8_t, 64> key_block {};
	if (key.size() > 64) {
		const auto hk = Sha256(key);
		std::memcpy(key_block.data(), hk.data(), hk.size());
	} else {
		std::memcpy(key_block.data(), key.data(), key.size());
	}

	std::array<uint8_t, 64> ipad {};
	std::array<uint8_t, 64> opad {};
	for (size_t i = 0; i < 64; ++i) {
		ipad[i] = static_cast<uint8_t>(key_block[i] ^ 0x36u);
		opad[i] = static_cast<uint8_t>(key_block[i] ^ 0x5cu);
	}

	Sha256State inner;
	inner.Update(ipad);
	inner.Update(data);
	const auto inner_hash = inner.Final();

	Sha256State outer;
	outer.Update(opad);
	outer.Update(inner_hash);
	return outer.Final();
}

void Aes128CbcEncrypt(std::span<const uint8_t, 16> key, std::span<const uint8_t, 16> iv,
                      std::span<const uint8_t> input, std::span<uint8_t> output) {
	const Aes128Context ctx(key);
	uint8_t             chain[16];
	std::memcpy(chain, iv.data(), 16);
	const size_t blocks = std::min(input.size(), output.size()) / 16u;
	for (size_t b = 0; b < blocks; ++b) {
		uint8_t block[16];
		for (int i = 0; i < 16; ++i) {
			block[i] = static_cast<uint8_t>(input[b * 16 + i] ^ chain[i]);
		}
		ctx.EncryptBlock(block, output.data() + b * 16);
		std::memcpy(chain, output.data() + b * 16, 16);
	}
}

void Aes128CbcDecrypt(std::span<const uint8_t, 16> key, std::span<const uint8_t, 16> iv,
                      std::span<const uint8_t> input, std::span<uint8_t> output) {
	const Aes128Context ctx(key);
	uint8_t             chain[16];
	std::memcpy(chain, iv.data(), 16);
	const size_t blocks = std::min(input.size(), output.size()) / 16u;
	for (size_t b = 0; b < blocks; ++b) {
		uint8_t       next_chain[16];
		const uint8_t* src = input.data() + b * 16;
		std::memcpy(next_chain, src, 16);
		uint8_t plain[16];
		ctx.DecryptBlock(src, plain);
		for (int i = 0; i < 16; ++i) {
			output[b * 16 + i] = static_cast<uint8_t>(plain[i] ^ chain[i]);
		}
		std::memcpy(chain, next_chain, 16);
	}
}

void Aes128XtsEncryptSector(const Aes128Context& data_ctx, const Aes128Context& tweak_ctx,
                            uint64_t sector_index, std::span<uint8_t> sector_buf) {
	uint8_t tweak[16] {};
	for (int i = 0; i < 8; ++i) {
		tweak[i] = static_cast<uint8_t>((sector_index >> (i * 8)) & 0xffu);
	}
	uint8_t enc_tweak[16] {};
	tweak_ctx.EncryptBlock(tweak, enc_tweak);

	const size_t blocks = sector_buf.size() / 16u;
	for (size_t b = 0; b < blocks; ++b) {
		uint8_t* p = sector_buf.data() + b * 16;
		uint8_t  tmp[16];
		for (int i = 0; i < 16; ++i) {
			tmp[i] = static_cast<uint8_t>(p[i] ^ enc_tweak[i]);
		}
		data_ctx.EncryptBlock(tmp, tmp);
		for (int i = 0; i < 16; ++i) {
			p[i] = static_cast<uint8_t>(tmp[i] ^ enc_tweak[i]);
		}
		GfMultiplyTweak(enc_tweak);
	}
}

void Aes128XtsDecryptSector(const Aes128Context& data_ctx, const Aes128Context& tweak_ctx,
                            uint64_t sector_index, std::span<uint8_t> sector_buf) {
	uint8_t tweak[16] {};
	for (int i = 0; i < 8; ++i) {
		tweak[i] = static_cast<uint8_t>((sector_index >> (i * 8)) & 0xffu);
	}
	uint8_t enc_tweak[16] {};
	tweak_ctx.EncryptBlock(tweak, enc_tweak);

	const size_t blocks = sector_buf.size() / 16u;
	for (size_t b = 0; b < blocks; ++b) {
		uint8_t* p = sector_buf.data() + b * 16;
		uint8_t  tmp[16];
		for (int i = 0; i < 16; ++i) {
			tmp[i] = static_cast<uint8_t>(p[i] ^ enc_tweak[i]);
		}
		data_ctx.DecryptBlock(tmp, tmp);
		for (int i = 0; i < 16; ++i) {
			p[i] = static_cast<uint8_t>(tmp[i] ^ enc_tweak[i]);
		}
		GfMultiplyTweak(enc_tweak);
	}
}

bool ComputePkgDerivedKey(std::string_view content_id, std::string_view passcode, uint32_t index,
                          std::array<uint8_t, 32>& out_key) {
	if (content_id.size() != 36u || passcode.size() != 32u) {
		return false;
	}
	uint8_t index_be[4] = {
	    static_cast<uint8_t>((index >> 24u) & 0xffu),
	    static_cast<uint8_t>((index >> 16u) & 0xffu),
	    static_cast<uint8_t>((index >> 8u) & 0xffu),
	    static_cast<uint8_t>(index & 0xffu),
	};
	std::array<uint8_t, 48> cid_padded {};
	std::memcpy(cid_padded.data(), content_id.data(), content_id.size());

	const auto h_idx = Sha256(index_be);
	const auto h_cid = Sha256(cid_padded);

	std::array<uint8_t, 96> buf {};
	std::memcpy(buf.data(), h_idx.data(), 32);
	std::memcpy(buf.data() + 32, h_cid.data(), 32);
	std::memcpy(buf.data() + 64, passcode.data(), 32);
	out_key = Sha256(buf);
	return true;
}

bool VerifyDerivedKeyDigest(std::span<const uint8_t, 32> dk,
                            std::span<const uint8_t, 32> expected_digest) {
	const auto h = Sha256(dk);
	uint8_t    diff = 0;
	for (size_t i = 0; i < 32; ++i) {
		diff |= static_cast<uint8_t>((h[i] ^ dk[i]) ^ expected_digest[i]);
	}
	return diff == 0;
}

bool VerifyPasscodeDigest(std::string_view content_id, std::string_view passcode,
                          std::span<const uint8_t, 32> expected_digest0) {
	std::array<uint8_t, 32> dk0 {};
	if (!ComputePkgDerivedKey(content_id, passcode, 0u, dk0)) {
		return false;
	}
	return VerifyDerivedKeyDigest(dk0, expected_digest0);
}

PfsXtsKeys DerivePfsXtsKeys(std::span<const uint8_t, 32> ekpfs, std::span<const uint8_t, 16> seed,
                            bool new_crypt) {
	std::array<uint8_t, 32> base_key {};
	if (new_crypt) {
		base_key = HmacSha256(ekpfs, seed);
	} else {
		std::memcpy(base_key.data(), ekpfs.data(), 32);
	}
	std::array<uint8_t, 20> msg {};
	msg[0] = 1u;
	msg[1] = 0u;
	msg[2] = 0u;
	msg[3] = 0u;
	std::memcpy(msg.data() + 4, seed.data(), 16);

	const auto enc = HmacSha256(base_key, msg);
	PfsXtsKeys keys {};
	std::memcpy(keys.tweak_key.data(), enc.data(), 16);
	std::memcpy(keys.data_key.data(), enc.data() + 16, 16);
	return keys;
}

bool EncryptPkgEntryData(std::span<const uint8_t, 32> meta_bytes_be,
                         std::span<const uint8_t, 32> key_seed,
                         std::span<const uint8_t>     plaintext,
                         std::vector<uint8_t>&        out_ciphertext) {
	if (plaintext.empty() || (plaintext.size() % 16u) != 0u) {
		return false;
	}
	std::array<uint8_t, 64> seed_buf {};
	std::memcpy(seed_buf.data(), meta_bytes_be.data(), 32);
	std::memcpy(seed_buf.data() + 32, key_seed.data(), 32);
	const auto iv_key = Sha256(seed_buf);

	std::array<uint8_t, 16> iv {};
	std::array<uint8_t, 16> key {};
	std::memcpy(iv.data(), iv_key.data(), 16);
	std::memcpy(key.data(), iv_key.data() + 16, 16);

	out_ciphertext.resize(plaintext.size());
	Aes128CbcEncrypt(key, iv, plaintext, out_ciphertext);
	return true;
}

bool DecryptPkgEntryData(std::span<const uint8_t, 32> meta_bytes_be,
                         std::span<const uint8_t, 32> key_seed,
                         std::span<const uint8_t>     ciphertext,
                         std::vector<uint8_t>&        out_plaintext) {
	if (ciphertext.empty() || (ciphertext.size() % 16u) != 0u) {
		return false;
	}
	std::array<uint8_t, 64> seed_buf {};
	std::memcpy(seed_buf.data(), meta_bytes_be.data(), 32);
	std::memcpy(seed_buf.data() + 32, key_seed.data(), 32);
	const auto iv_key = Sha256(seed_buf);

	std::array<uint8_t, 16> iv {};
	std::array<uint8_t, 16> key {};
	std::memcpy(iv.data(), iv_key.data(), 16);
	std::memcpy(key.data(), iv_key.data() + 16, 16);

	out_plaintext.resize(ciphertext.size());
	Aes128CbcDecrypt(key, iv, ciphertext, out_plaintext);
	return true;
}

} // namespace Common::Pkg
