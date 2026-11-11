#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace Libs::Graphics::Metal::HostBudget {

// A completed interval of GPU work must be followed by this much idle time.
// Submission is serialized separately; this is an average duty-cycle budget,
// not a preemption mechanism for a single command buffer.
inline uint64_t IdleNanoseconds(uint64_t busy_ns, uint32_t percent) {
	if (percent == 0 || percent > 90) throw std::invalid_argument("GPU budget must be 1..90 percent");
	const uint64_t idle_percent = 100u - percent;
	const uint64_t quotient = busy_ns / percent;
	const uint64_t remainder = busy_ns % percent;
	const uint64_t tail = (remainder * idle_percent + percent - 1u) / percent;
	if (quotient > (std::numeric_limits<uint64_t>::max() - tail) / idle_percent)
		throw std::overflow_error("GPU idle interval overflow");
	return quotient * idle_percent + tail;
}

} // namespace Libs::Graphics::Metal::HostBudget
