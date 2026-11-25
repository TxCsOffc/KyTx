#pragma once

#include <cstdint>

#ifdef __OBJC__
#import <Metal/Metal.h>
#endif

namespace Libs::Graphics::Metal {
inline constexpr uint32_t GuestAddressLeafBits = 10;
#ifdef __OBJC__
id<MTLBuffer> BindGuestAddressSpaceBuffer(bool mesh_stage);
#endif

// Native Metal page table for guest scalar/flat addresses. Call while the
// consuming encoder is active; all indirect resources are declared resident.
uint64_t BindGuestAddressSpace();
}
