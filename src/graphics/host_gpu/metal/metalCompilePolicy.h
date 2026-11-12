#pragma once

#import <Metal/Metal.h>
#include <cstdint>

namespace Libs::Graphics::Metal {

// MTLLanguageVersion values are enum constants, not preprocessor macros.
// Pin the same supported language for production shaders and replay tools.
constexpr MTLLanguageVersion MslLanguageForTarget(uint32_t sdk,uint32_t os) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
	if(sdk>=260000 && os>=260000)return MTLLanguageVersion4_0;
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
	if(sdk>=150000 && os>=150000)return MTLLanguageVersion3_2;
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
	if(sdk>=140000 && os>=140000)return MTLLanguageVersion3_1;
#endif
	return MTLLanguageVersion3_0;
}

inline MTLLanguageVersion NativeMslLanguageVersion() {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
	if(@available(macOS 26.0,*))return MslLanguageForTarget(__MAC_OS_X_VERSION_MAX_ALLOWED,260000);
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
	if(@available(macOS 15.0,*))return MslLanguageForTarget(__MAC_OS_X_VERSION_MAX_ALLOWED,150000);
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
	if(@available(macOS 14.0,*))return MslLanguageForTarget(__MAC_OS_X_VERSION_MAX_ALLOWED,140000);
#endif
	return MTLLanguageVersion3_0;
}

} // namespace Libs::Graphics::Metal
