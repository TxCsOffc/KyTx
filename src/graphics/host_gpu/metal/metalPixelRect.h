#pragma once
#include <cstdint>

namespace Libs::Graphics::Metal {
struct PixelRect {
	uint32_t x=0,y=0,width=0,height=0;
	bool operator==(const PixelRect&)const=default;
	bool Contains(const PixelRect& r)const {
		return r.x>=x && r.y>=y && uint64_t(r.x)+r.width<=uint64_t(x)+width && uint64_t(r.y)+r.height<=uint64_t(y)+height;
	}
};
}
