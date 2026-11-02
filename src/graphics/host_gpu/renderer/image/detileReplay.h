#ifndef KYTY_GRAPHICS_DETILE_REPLAY_H_
#define KYTY_GRAPHICS_DETILE_REPLAY_H_

#include "graphics/host_gpu/renderer/image/tiler.h"

#include <filesystem>
#include <string>

namespace Libs::Graphics {

// Logical inputs and reference bytes, independent of Vulkan handles and shader binaries.
struct DetileReplay {
	uint64_t guest_address = 0;
	uint64_t frame = 0;
	uint64_t source_offset = 0;
	uint64_t tiled_capacity = 0;
	uint64_t linear_capacity = 0;
	std::vector<GpuTileInfo> infos;
	std::vector<uint8_t> input;
	std::vector<uint8_t> expected;
};

[[nodiscard]] uint64_t DetileReplayHash(std::span<const uint8_t> bytes) noexcept;
[[nodiscard]] bool WriteDetileReplay(const std::filesystem::path& path,
                                    const DetileReplay& replay, std::string& error);
[[nodiscard]] bool ReadDetileReplay(const std::filesystem::path& path,
                                   DetileReplay& replay, std::string& error);

// One-time diagnostic readback only when explicitly armed through the live control file.
void MaybeCaptureDetile(GraphicContext& graphics, CommandScheduler& scheduler,
                        TileManager::Result source, TileManager::Result output,
                        std::span<const GpuTileInfo> infos, uint64_t guest_address);

} // namespace Libs::Graphics

#endif
