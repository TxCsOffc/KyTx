// Standalone-test replacement for common/profiler.h: the profiler macros compile to nothing (no Tracy dependency).
#pragma once
#define KYTY_PROFILER_BLOCK(...)
#define KYTY_PROFILER_FUNCTION(...)
#define KYTY_PROFILER_THREAD(name)
