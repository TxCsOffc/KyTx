// Software binary64 for MSL (included as a string by MslEmitter.cpp). Judge: tools/softf64/check.py (bit-exact vs host binary64).
R"KYTYF64(
// Software IEEE-754 binary64 for Metal (Apple GPUs have no 64-bit float). Valid MSL and C++ (tools/softf64/check.py compiles it on
// the CPU and compares it bit-exactly with the host's binary64). Values are raw bit patterns in ulong; f32 values raw uint bits.
// Round to nearest even, subnormals kept, NaN results are the default quiet NaN. Structure follows Berkeley SoftFloat 3.

inline ulong kyty_f64_pack(uint sign, int exp, ulong sig) { return (ulong(sign) << 63) + (ulong(exp) << 52) + sig; }
inline uint kyty_f64_sign(ulong a) { return uint(a >> 63); }
inline int kyty_f64_exp(ulong a) { return int((a >> 52) & 0x7ffu); }
inline ulong kyty_f64_frac(ulong a) { return a & 0x000fffffffffffffUL; }
inline bool kyty_f64_isnan(ulong a) { return kyty_f64_exp(a) == 0x7ff && kyty_f64_frac(a) != 0UL; }
inline ulong kyty_f64_default_nan() { return 0x7ff8000000000000UL; }
inline uint kyty_clz64(ulong a) { return a == 0UL ? 64u : uint(clz(a)); }

inline ulong kyty_shift_right_jam64(ulong a, uint dist) {
	if (dist == 0u) return a;
	if (dist < 63u) return (a >> dist) | ulong((a << (64u - dist)) != 0UL);
	return ulong(a != 0UL);
}

// sig has its leading bit at 62 (or is smaller), 10 rounding bits; exp is the result exponent field minus one.
inline ulong kyty_f64_round_pack(uint sign, int exp, ulong sig) {
	ulong round_bits = sig & 0x3ffUL;
	if (exp < 0 || exp >= 0x7fd) {
		if (exp < 0) {
			sig = kyty_shift_right_jam64(sig, uint(-exp) > 64u ? 64u : uint(-exp));
			exp = 0;
			round_bits = sig & 0x3ffUL;
		} else if (exp > 0x7fd || sig + 0x200UL >= 0x8000000000000000UL) {
			return kyty_f64_pack(sign, 0x7ff, 0UL);
		}
	}
	sig = (sig + 0x200UL) >> 10;
	if (round_bits == 0x200UL) sig &= ~1UL;
	if (sig == 0UL) exp = 0;
	return kyty_f64_pack(sign, exp, sig);
}

inline ulong kyty_f64_norm_round_pack(uint sign, int exp, ulong sig) {
	const int shift = int(kyty_clz64(sig)) - 1;
	exp -= shift;
	if (shift >= 10 && exp >= 0 && exp < 0x7fd) return kyty_f64_pack(sign, sig != 0UL ? exp : 0, sig << uint(shift - 10));
	return kyty_f64_round_pack(sign, exp, shift >= 0 ? sig << uint(shift) : sig);
}

inline ulong kyty_f64_add_mags(ulong a, ulong b, uint sign) {
	int ea = kyty_f64_exp(a), eb = kyty_f64_exp(b);
	ulong sa = kyty_f64_frac(a), sb = kyty_f64_frac(b);
	const int diff = ea - eb;
	if (diff == 0) {
		if (ea == 0) return a + sb;
		if (ea == 0x7ff) return (sa | sb) != 0UL ? kyty_f64_default_nan() : a;
		return kyty_f64_round_pack(sign, ea, (0x0020000000000000UL + sa + sb) << 9);
	}
	sa <<= 9;
	sb <<= 9;
	int ez = 0;
	if (diff < 0) {
		if (eb == 0x7ff) return sb != 0UL ? kyty_f64_default_nan() : kyty_f64_pack(sign, 0x7ff, 0UL);
		ez = eb;
		sa = ea != 0 ? sa + 0x2000000000000000UL : sa << 1;
		sa = kyty_shift_right_jam64(sa, uint(-diff));
	} else {
		if (ea == 0x7ff) return sa != 0UL ? kyty_f64_default_nan() : a;
		ez = ea;
		sb = eb != 0 ? sb + 0x2000000000000000UL : sb << 1;
		sb = kyty_shift_right_jam64(sb, uint(diff));
	}
	ulong sz = 0x2000000000000000UL + sa + sb;
	if (sz < 0x4000000000000000UL) {
		--ez;
		sz <<= 1;
	}
	return kyty_f64_round_pack(sign, ez, sz);
}

inline ulong kyty_f64_sub_mags(ulong a, ulong b, uint sign) {
	int ea = kyty_f64_exp(a), eb = kyty_f64_exp(b);
	ulong sa = kyty_f64_frac(a), sb = kyty_f64_frac(b);
	const int diff = ea - eb;
	if (diff == 0) {
		if (ea == 0x7ff) return kyty_f64_default_nan();
		long d = long(sa) - long(sb);
		if (d == 0L) return 0UL;  // exact cancellation: +0 when rounding to nearest
		if (ea != 0) --ea;
		if (d < 0L) {
			sign ^= 1u;
			d = -d;
		}
		int shift = int(kyty_clz64(ulong(d))) - 11;
		int ez = ea - shift;
		if (ez < 0) {
			shift = ea;
			ez = 0;
		}
		return kyty_f64_pack(sign, ez, ulong(d) << uint(shift));
	}
	sa <<= 10;
	sb <<= 10;
	int ez = 0;
	ulong sz = 0UL;
	if (diff < 0) {
		sign ^= 1u;
		if (eb == 0x7ff) return sb != 0UL ? kyty_f64_default_nan() : kyty_f64_pack(sign, 0x7ff, 0UL);
		sa += ea != 0 ? 0x4000000000000000UL : sa;
		sa = kyty_shift_right_jam64(sa, uint(-diff));
		sb |= 0x4000000000000000UL;
		ez = eb;
		sz = sb - sa;
	} else {
		if (ea == 0x7ff) return sa != 0UL ? kyty_f64_default_nan() : a;
		sb += eb != 0 ? 0x4000000000000000UL : sb;
		sb = kyty_shift_right_jam64(sb, uint(diff));
		sa |= 0x4000000000000000UL;
		ez = ea;
		sz = sa - sb;
	}
	return kyty_f64_norm_round_pack(sign, ez - 1, sz);
}

inline ulong kyty_f64_add(ulong a, ulong b) {
	if (kyty_f64_isnan(a) || kyty_f64_isnan(b)) return kyty_f64_default_nan();
	const uint sa = kyty_f64_sign(a), sb = kyty_f64_sign(b);
	return sa == sb ? kyty_f64_add_mags(a, b, sa) : kyty_f64_sub_mags(a, b, sa);
}

inline void kyty_mul64_to128(ulong a, ulong b, thread ulong& hi, thread ulong& lo) {
	const ulong a32 = a >> 32, a0 = a & 0xffffffffUL, b32 = b >> 32, b0 = b & 0xffffffffUL;
	lo = a0 * b0;
	const ulong mid1 = a32 * b0;
	ulong mid = mid1 + a0 * b32;
	hi = a32 * b32;
	hi += (ulong(mid < mid1) << 32) | (mid >> 32);
	mid <<= 32;
	lo += mid;
	hi += ulong(lo < mid);
}

// Normalizes a nonzero subnormal fraction: returns the exponent and shifts the significand so its leading bit is at 52.
inline int kyty_f64_norm_subnormal(thread ulong& sig) {
	const int shift = int(kyty_clz64(sig)) - 11;
	sig <<= uint(shift);
	return 1 - shift;
}

inline ulong kyty_f64_mul(ulong a, ulong b) {
	if (kyty_f64_isnan(a) || kyty_f64_isnan(b)) return kyty_f64_default_nan();
	const uint sign = kyty_f64_sign(a) ^ kyty_f64_sign(b);
	int ea = kyty_f64_exp(a), eb = kyty_f64_exp(b);
	ulong sa = kyty_f64_frac(a), sb = kyty_f64_frac(b);
	if (ea == 0x7ff || eb == 0x7ff) {
		const ulong other = ea == 0x7ff ? (ulong(eb) | sb) : (ulong(ea) | sa);
		return other == 0UL ? kyty_f64_default_nan() : kyty_f64_pack(sign, 0x7ff, 0UL);
	}
	if (ea == 0) {
		if (sa == 0UL) return kyty_f64_pack(sign, 0, 0UL);
		ea = kyty_f64_norm_subnormal(sa);
	}
	if (eb == 0) {
		if (sb == 0UL) return kyty_f64_pack(sign, 0, 0UL);
		eb = kyty_f64_norm_subnormal(sb);
	}
	int ez = ea + eb - 0x3ff;
	sa = (sa | 0x0010000000000000UL) << 10;
	sb = (sb | 0x0010000000000000UL) << 11;
	ulong hi = 0UL, lo = 0UL;
	kyty_mul64_to128(sa, sb, hi, lo);
	ulong sz = hi | ulong(lo != 0UL);
	if (sz < 0x4000000000000000UL) {
		--ez;
		sz <<= 1;
	}
	return kyty_f64_round_pack(sign, ez, sz);
}

// 128-bit helpers for fma (hi:lo).
inline uint kyty_clz128(ulong hi, ulong lo) { return hi != 0UL ? kyty_clz64(hi) : 64u + kyty_clz64(lo); }
inline void kyty_shl128(thread ulong& hi, thread ulong& lo, uint n) {
	if (n == 0u) return;
	if (n >= 128u) { hi = 0UL; lo = 0UL; return; }
	if (n >= 64u) { hi = lo << (n - 64u); lo = 0UL; return; }
	hi = (hi << n) | (lo >> (64u - n));
	lo <<= n;
}
inline void kyty_shr128_jam(thread ulong& hi, thread ulong& lo, uint n) {
	if (n == 0u) return;
	if (n >= 128u) { const ulong any = ulong((hi | lo) != 0UL); hi = 0UL; lo = any; return; }
	ulong lost = 0UL;
	if (n >= 64u) {
		lost = lo | (n > 64u ? (hi << (128u - n)) : 0UL);
		lo = n == 64u ? hi : hi >> (n - 64u);
		hi = 0UL;
	} else {
		lost = lo << (64u - n);
		lo = (lo >> n) | (hi << (64u - n));
		hi >>= n;
	}
	lo |= ulong(lost != 0UL);
}

// The value as (significand with the hidden bit, exponent of its least significant bit); a must be finite and nonzero.
inline ulong kyty_f64_unpack(ulong a, thread int& e_lsb) {
	const int e = kyty_f64_exp(a);
	e_lsb = (e == 0 ? 1 : e) - 1075;
	return kyty_f64_frac(a) | (e != 0 ? 0x0010000000000000UL : 0UL);
}

inline ulong kyty_f64_fma(ulong a, ulong b, ulong c) {
	if (kyty_f64_isnan(a) || kyty_f64_isnan(b) || kyty_f64_isnan(c)) return kyty_f64_default_nan();
	const uint sp = kyty_f64_sign(a) ^ kyty_f64_sign(b), sc = kyty_f64_sign(c);
	const bool a_inf = kyty_f64_exp(a) == 0x7ff, b_inf = kyty_f64_exp(b) == 0x7ff, c_inf = kyty_f64_exp(c) == 0x7ff;
	const bool a_zero = (a << 1) == 0UL, b_zero = (b << 1) == 0UL;
	if (a_inf || b_inf) {
		if (a_zero || b_zero) return kyty_f64_default_nan();
		if (c_inf && sc != sp) return kyty_f64_default_nan();
		return kyty_f64_pack(sp, 0x7ff, 0UL);
	}
	if (c_inf) return c;
	if (a_zero || b_zero) return kyty_f64_add(kyty_f64_pack(sp, 0, 0UL), c);  // exact zero product
	int ea = 0, eb = 0, ec = 0;
	const ulong ma = kyty_f64_unpack(a, ea), mb = kyty_f64_unpack(b, eb);
	ulong ph = 0UL, pl = 0UL;
	kyty_mul64_to128(ma, mb, ph, pl);
	int e = ea + eb;  // exponent of the product's least significant bit
	if ((c << 1) == 0UL) {
		// Product only: normalize to the leading bit at 62 with sticky, then round.
		const uint msb = 127u - kyty_clz128(ph, pl);
		if (msb > 62u) { kyty_shr128_jam(ph, pl, msb - 62u); e += int(msb - 62u); }
		else { kyty_shl128(ph, pl, 62u - msb); e -= int(62u - msb); }
		return kyty_f64_round_pack(sp, e + 1084, pl);
	}
	ulong ch = 0UL, cl = kyty_f64_unpack(c, ec);
	// Both with their leading bit at 125 (room for the carry of an addition).
	uint msb = 127u - kyty_clz128(ph, pl);
	kyty_shl128(ph, pl, 125u - msb);
	e -= int(125u - msb);
	msb = 127u - kyty_clz128(ch, cl);
	kyty_shl128(ch, cl, 125u - msb);
	ec -= int(125u - msb);
	if (e > ec) { kyty_shr128_jam(ch, cl, uint(e - ec > 128 ? 128 : e - ec)); }
	else if (ec > e) { kyty_shr128_jam(ph, pl, uint(ec - e > 128 ? 128 : ec - e)); e = ec; }
	uint sign = sp;
	ulong sh = 0UL, sl = 0UL;
	if (sp == sc) {
		sl = pl + cl;
		sh = ph + ch + ulong(sl < pl);
	} else {
		const bool p_ge = ph > ch || (ph == ch && pl >= cl);
		if (p_ge) { sl = pl - cl; sh = ph - ch - ulong(pl < cl); }
		else { sl = cl - pl; sh = ch - ph - ulong(cl < pl); sign = sc; }
		if ((sh | sl) == 0UL) return 0UL;  // exact cancellation: +0
	}
	msb = 127u - kyty_clz128(sh, sl);
	if (msb > 62u) { kyty_shr128_jam(sh, sl, msb - 62u); e += int(msb - 62u); }
	else { kyty_shl128(sh, sl, 62u - msb); e -= int(62u - msb); }
	return kyty_f64_round_pack(sign, e + 1084, sl);
}

inline ulong kyty_f64_from_f32(uint f) {
	const uint sign = f >> 31, e = (f >> 23) & 0xffu;
	ulong m = ulong(f & 0x007fffffu);
	if (e == 0xffu) return m != 0UL ? (kyty_f64_pack(sign, 0x7ff, 0UL) | 0x0008000000000000UL | (m << 29)) : kyty_f64_pack(sign, 0x7ff, 0UL);
	if (e == 0u) {
		if (m == 0UL) return kyty_f64_pack(sign, 0, 0UL);
		const int shift = int(kyty_clz64(m)) - 40;  // leading bit to 23
		m = (m << uint(shift)) & 0x007fffffUL;
		return kyty_f64_pack(sign, 0x380 + 1 - shift, m << 29);
	}
	return kyty_f64_pack(sign, int(e) + 0x380, m << 29);
}

inline uint kyty_f64_to_f32(ulong a) {
	const uint sign = kyty_f64_sign(a);
	const int e = kyty_f64_exp(a);
	const ulong frac = kyty_f64_frac(a);
	if (e == 0x7ff) return (sign << 31) | 0x7f800000u | (frac != 0UL ? 0x00400000u | uint(frac >> 29) : 0u);
	// 30-bit significand with sticky: leading bit at 29 for normals, as in SoftFloat f64_to_f32.
	ulong sig = kyty_shift_right_jam64(frac, 22u);
	if ((ulong(e) | sig) == 0UL) return sign << 31;
	int ez = e - 0x381;
	uint s = uint(sig) | 0x40000000u;
	// round_pack for f32: s has its leading bit at 30, 7 rounding bits
	uint round_bits = s & 0x7fu;
	if (ez < 0 || ez >= 0xfd) {
		if (ez < 0) {
			const uint dist = uint(-ez);
			s = dist < 31u ? (s >> dist) | uint((s << (32u - dist)) != 0u) : uint(s != 0u);
			ez = 0;
			round_bits = s & 0x7fu;
		} else if (ez > 0xfd || s + 0x40u >= 0x80000000u) {
			return (sign << 31) | 0x7f800000u;
		}
	}
	s = (s + 0x40u) >> 7;
	if (round_bits == 0x40u) s &= ~1u;
	if (s == 0u) ez = 0;
	return (sign << 31) + (uint(ez) << 23) + s;
}

inline ulong kyty_f64_from_u32(uint u) {
	if (u == 0u) return 0UL;
	const int shift = int(kyty_clz64(ulong(u))) - 11;
	return kyty_f64_pack(0u, 0x432 - shift, ulong(u) << uint(shift));
}

inline ulong kyty_f64_from_i32(uint bits) {
	const int v = int(bits);
	if (v == 0) return 0UL;
	const uint sign = bits >> 31;
	const ulong mag = sign != 0u ? ulong(0UL - ulong(long(v))) : ulong(v);
	const int shift = int(kyty_clz64(mag)) - 11;
	return kyty_f64_pack(sign, 0x432 - shift, mag << uint(shift));
}

// 1/a correctly rounded (v_rcp_f64 is computed exactly here): 2^126 / m by restoring division, m the significand with its leading
// bit at 63, remainder as sticky.
inline ulong kyty_f64_rcp(ulong a) {
	if (kyty_f64_isnan(a)) return kyty_f64_default_nan();
	const uint sign = kyty_f64_sign(a);
	if (kyty_f64_exp(a) == 0x7ff) return kyty_f64_pack(sign, 0, 0UL);
	if ((a << 1) == 0UL) return kyty_f64_pack(sign, 0x7ff, 0UL);
	int ex = 0;
	ulong m = kyty_f64_unpack(a, ex);
	const uint lead = 63u - kyty_clz64(m);
	m <<= 63u - lead;
	ex -= int(63u - lead);
	// q = floor(2^126 / m), r = 2^126 mod m: numerator bits 126..0 = one 1 followed by zeros.
	ulong q = 0UL, r = 0UL;
	for (int bit = 126; bit >= 0; --bit) {
		const bool top = (r >> 63) != 0UL;
		r = (r << 1) | ulong(bit == 126);
		q <<= 1;
		if (top || r >= m) { r -= m; q |= 1UL; }
	}
	int e = -126 - ex;
	if ((q >> 63) != 0UL) { q = (q >> 1) | (q & 1UL); e += 1; }
	q |= ulong(r != 0UL);
	return kyty_f64_round_pack(sign, e + 1084, q);
}

inline bool kyty_f64_eq(ulong a, ulong b) {
	if (kyty_f64_isnan(a) || kyty_f64_isnan(b)) return false;
	return a == b || ((a | b) << 1) == 0UL;
}
inline bool kyty_f64_lt(ulong a, ulong b) {
	if (kyty_f64_isnan(a) || kyty_f64_isnan(b)) return false;
	const uint sa = kyty_f64_sign(a), sb = kyty_f64_sign(b);
	if (sa != sb) return sa != 0u && ((a | b) << 1) != 0UL;
	return a != b && ((sa != 0u) != (a < b));
}
inline bool kyty_f64_le(ulong a, ulong b) {
	if (kyty_f64_isnan(a) || kyty_f64_isnan(b)) return false;
	const uint sa = kyty_f64_sign(a), sb = kyty_f64_sign(b);
	if (sa != sb) return sa != 0u || ((a | b) << 1) == 0UL;
	return a == b || ((sa != 0u) != (a < b));
}
inline bool kyty_f64_gt(ulong a, ulong b) { return kyty_f64_lt(b, a); }
inline bool kyty_f64_ge(ulong a, ulong b) { return kyty_f64_le(b, a); }
// end of kyty_f64.h
)KYTYF64"
