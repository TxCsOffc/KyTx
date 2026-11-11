#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_

#include "common/abi.h"
#include "common/common.h"

#include <chrono>

namespace Libs::LibKernel::SyncOnAddress {

using signal_poll_func_t = void (*)();

// Byte and halfword waits (sceKernelSyncOnAddressWait8/16): the same "sleep while *address == expected" contract on a naturally
// aligned 1- or 2-byte cell. A cell narrower than a futex word is always waited on through the portable registry.
int Wait8(volatile uint8_t* address, uint8_t expected, const uint32_t* timeout_micros,
          signal_poll_func_t signal_poll = nullptr);
int Wait16(volatile uint16_t* address, uint16_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait32(volatile uint32_t* address, uint32_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, const uint32_t* timeout_micros,
           signal_poll_func_t signal_poll = nullptr);
int Wait64(volatile uint64_t* address, uint64_t expected, std::chrono::nanoseconds timeout,
           signal_poll_func_t signal_poll = nullptr);
int Wake(volatile void* address, int32_t count);

} // namespace Libs::LibKernel::SyncOnAddress

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SYNC_ON_ADDRESS_H_ */
