#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALENV_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALENV_H_

#include <cstdlib>

// Diagnostic switches (KYTY_*) are fixed for the life of the process, but getenv walks the environment on every call: it showed up at
// ~14% of the command-processor thread, called per draw and per dispatch. KYTY_ENV("NAME") reads a variable once per call site.
#define KYTY_ENV(name)                                                                                                   \
	([]() -> const char* {                                                                                               \
		static const char* const kyty_env_value = std::getenv(name);                                                     \
		return kyty_env_value;                                                                                           \
	}())

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_METAL_METALENV_H_
