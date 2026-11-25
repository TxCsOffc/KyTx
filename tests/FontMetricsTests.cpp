#include "libs/libFont.cpp"

namespace Loader {
#if KYTY_GUEST_CPU_EMULATED
namespace GuestCpu {
uint64_t GetHostFunctionStub(uint64_t address, const char*) { return address; }
} // namespace GuestCpu
#endif
void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {}
namespace Timer {
double GetTimeMs() { return 0.0; }
} // namespace Timer
} // namespace Loader

int main() {
	using namespace Libs::Font;
	FontState font {};
	if (!ensure_stb_font(&font)) {
		std::fprintf(stderr, "The bundled fallback font could not be loaded.\n");
		return 1;
	}
	FontGlyphMetrics small {}, large {}, space {}, bitmap {};
	FontSetScalePixel(&font, 100.0f, 100.0f);
	FontGetCharGlyphMetrics(&font, 'M', &small);
	FontSetScalePixel(&font, 10000.0f, 7500.0f);
	FontGetCharGlyphMetrics(&font, 'M', &large);
	FontGetCharGlyphMetrics(&font, ' ', &space);
	FontGetRenderCharGlyphMetrics(&font, 'M', &bitmap);
	if (small.width <= 0 || small.height <= 0 ||
	    std::abs(large.width / small.width - 100.0f) > 0.001f ||
	    std::abs(large.horizontal.advance / small.horizontal.advance - 100.0f) > 0.001f ||
	    std::abs(large.height / small.height - 75.0f) > 0.001f ||
	    space.width != 0 || space.horizontal.advance <= 0 ||
	    bitmap.height <= 0 || bitmap.height > FONT_BITMAP_MAX_DIM) {
		std::fprintf(stderr, "Incorrect layout or bitmap metrics at large independent scales.\n");
		return 1;
	}
	return 0;
}
