#pragma once

#include <cstdint>

namespace Libs::Graphics::Metal {
// Native Metal page table for guest scalar/flat addresses. Call while the
// consuming encoder is active; all indirect resources are declared resident.
uint64_t BindGuestAddressSpace();
}
