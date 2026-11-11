#include "graphics/host_gpu/metal/metalDutyCycle.h"
#include <array>
#include <cstdio>
#include <random>

using Libs::Graphics::Metal::HostBudget::IdleNanoseconds;

int main() {
	unsigned checks = 0;
	const auto check = [&](bool ok) { ++checks; if (!ok) throw std::runtime_error("duty-cycle bound"); };
	check(IdleNanoseconds(9, 90) == 1);
	check(IdleNanoseconds(10, 90) == 2);
	check(IdleNanoseconds(90'000'000, 90) == 10'000'000);
	check(IdleNanoseconds(0, 90) == 0);
	for (unsigned p : {0u, 91u, 100u, UINT32_MAX}) {
		bool rejected = false;
		try { (void)IdleNanoseconds(100, p); } catch (const std::invalid_argument&) { rejected = true; }
		check(rejected);
	}
	std::mt19937_64 random(0x90);
	for (unsigned p : {1u, 10u, 33u, 50u, 89u, 90u}) {
		for (unsigned n = 0; n < 1000; ++n) {
			const uint64_t busy = random() % 10'000'000'000ull;
			const uint64_t idle = IdleNanoseconds(busy, p);
			using Wide = unsigned __int128;
			check(Wide(idle) * p >= Wide(busy) * (100u - p));
			check(idle == 0 || Wide(idle - 1) * p < Wide(busy) * (100u - p));
		}
	}
	const uint64_t last = IdleNanoseconds(UINT64_MAX, 90);
	check(static_cast<unsigned __int128>(last) * 90 >= static_cast<unsigned __int128>(UINT64_MAX) * 10);
	bool overflow = false;
	try { (void)IdleNanoseconds(UINT64_MAX, 1); } catch (const std::overflow_error&) { overflow = true; }
	check(overflow);
	std::printf("GPU duty-cycle arithmetic: %u independent bound/rounding/overflow checks PASS\n", checks);
}
