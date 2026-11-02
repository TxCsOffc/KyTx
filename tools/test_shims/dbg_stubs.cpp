// Standalone-test replacements for the Common::Dbg* handlers (abort instead of the emulator's exit path).
#include "common/assert.h"
#include <cstdio>
#include <cstdlib>
namespace Common {
int DbgExitHandler(char const* file, int line, std::string_view text) { std::fprintf(stderr, "DbgExit %s:%d %.*s\n", file, line, (int)text.size(), text.data()); std::abort(); }
int DbgExitHandler(char const* file, int line, fmt::text_style, std::string_view text) { return DbgExitHandler(file, line, text); }
int DbgExitIfHandler(char const* expr, char const* file, int line) { std::fprintf(stderr, "DbgExitIf %s %s:%d\n", expr, file, line); std::abort(); }
int DbgNotImplementedHandler(char const* expr, char const* file, int line) { std::fprintf(stderr, "NotImplemented %s %s:%d\n", expr, file, line); std::abort(); }
void DbgExit(int) { std::abort(); }
} // namespace Common
