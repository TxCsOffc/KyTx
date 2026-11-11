#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>

namespace Libs::Graphics::Metal {
// Unlike a debug assertion, this contract is enforced in release builds too.
// An unsupported operation must never become a successful draw with fake data.
[[noreturn]] inline void RejectMetalState(const std::string& detail) {
	const std::string message = "Unsupported Metal emulation: " + detail;
	std::fprintf(stderr, "%s\n", message.c_str());
	throw std::runtime_error(message);
}
} // namespace Libs::Graphics::Metal
