#include "graphics/host_gpu/renderer/productionProfile.h"
#include "graphics/host_gpu/renderer/image/detileReplay.h"

#include "common/alignment.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

namespace Libs::Graphics {
namespace {

constexpr uint64_t MaxBytes = 256ull << 20;
constexpr uint32_t MaxInfos = 1024;
constexpr std::array<char, 8> Magic {'K', 'Y', 'D', 'E', 'T', 'I', 'L', 'E'};

template<typename T> void WriteInteger(std::ostream& stream, T value) {
	for (size_t i = 0; i < sizeof(T); ++i) stream.put(static_cast<char>(value >> (i * 8)));
}
template<typename T> T ReadInteger(std::istream& stream) {
	T value = 0;
	for (size_t i = 0; i < sizeof(T); ++i) {
		const auto byte = stream.get();
		if (byte == std::char_traits<char>::eof()) return 0;
		value |= static_cast<T>(static_cast<uint8_t>(byte)) << (i * 8);
	}
	return value;
}

bool Validate(const DetileReplay& replay, std::string& error) {
	if (replay.infos.empty() || replay.infos.size() > MaxInfos ||
	    replay.tiled_capacity == 0 || replay.tiled_capacity > MaxBytes ||
	    replay.linear_capacity == 0 || replay.linear_capacity > MaxBytes ||
	    replay.input.size() > MaxBytes || replay.source_offset > replay.input.size() ||
	    replay.tiled_capacity > replay.input.size() - replay.source_offset ||
	    replay.expected.size() != replay.linear_capacity ||
	    replay.input.size() % 4 != 0 || replay.linear_capacity % 4 != 0) {
		error = "invalid or oversized detile replay buffers";
		return false;
	}
	for (const auto& info: replay.infos) {
		TileBlockLayout block {};
		if (!TileGetBlockLayout(info.family, info.bytes_per_element, block) ||
		    info.width == 0 || info.height == 0 || info.depth == 0 || info.pitch < info.width ||
		    info.linear_offset > replay.linear_capacity || info.linear_size == 0 ||
		    info.linear_size > replay.linear_capacity - info.linear_offset ||
		    info.tiled_offset > replay.tiled_capacity || info.tiled_size == 0 ||
		    info.tiled_size > replay.tiled_capacity - info.tiled_offset) {
			error = "invalid detile replay layout";
			return false;
		}
		const uint64_t row = uint64_t(info.pitch) * info.bytes_per_element;
		const uint64_t slice = info.linear_slice_stride != 0 ? info.linear_slice_stride : row * info.height;
		if (info.width > 524280 || info.height > 524280 || info.depth > 65535 ||
		    row > UINT32_MAX || slice > UINT32_MAX || (block.block_depth == 1 && info.depth != 1) ||
		    (info.depth > 1 && slice < row * info.height) ||
		    uint64_t(info.depth - 1) * slice + uint64_t(info.height - 1) * row +
		        uint64_t(info.width) * info.bytes_per_element > info.linear_size ||
		    ((info.linear_offset | info.tiled_offset | row | slice) &
		     (std::min(info.bytes_per_element, 4u) - 1u)) != 0) {
			error = "invalid detile replay dimensions or strides"; return false;
		}
		if (info.tail) {
			if (info.family == TileBlockFamily::Standard256B || info.depth > block.block_depth ||
			    info.tail_x >= block.block_width || info.width > block.block_width - info.tail_x ||
			    info.tail_y >= block.block_height || info.height > block.block_height - info.tail_y ||
			    info.tiled_size < block.block_size) {
				error = "invalid detile replay mip tail"; return false;
			}
		} else {
			const uint64_t width = info.tiled_width != 0 ? info.tiled_width : info.pitch;
			const uint64_t height = info.tiled_height != 0 ? info.tiled_height : info.height;
			const uint64_t columns = (width + block.block_width - 1) / block.block_width;
			const uint64_t rows = (height + block.block_height - 1) / block.block_height;
			const uint64_t slices = (uint64_t(info.depth) + block.block_depth - 1) / block.block_depth;
			if (width < info.width || height < info.height || columns == 0 || rows == 0 ||
			    columns > info.tiled_size / block.block_size ||
			    rows > info.tiled_size / (columns * block.block_size) ||
			    slices > info.tiled_size / (columns * rows * block.block_size)) {
				error = "detile replay tile grid exceeds source storage"; return false;
			}
		}
	}
	return true;
}

void WriteInfo(std::ostream& stream, const GpuTileInfo& info) {
	WriteInteger(stream, static_cast<uint32_t>(info.family));
	WriteInteger(stream, info.bytes_per_element);
	for (auto value: {info.linear_offset, info.linear_size, info.tiled_offset,
	                  info.tiled_size, info.linear_slice_stride}) WriteInteger(stream, value);
	for (auto value: {info.width, info.height, info.depth, info.pitch, info.tail_x,
	                  info.tail_y, static_cast<uint32_t>(info.tail), info.tiled_width,
	                  info.tiled_height, info.surface_z}) WriteInteger(stream, value);
}
GpuTileInfo ReadInfo(std::istream& stream) {
	GpuTileInfo info;
	info.family = static_cast<TileBlockFamily>(ReadInteger<uint32_t>(stream));
	info.bytes_per_element = ReadInteger<uint32_t>(stream);
	for (auto* field: {&info.linear_offset, &info.linear_size, &info.tiled_offset,
	                   &info.tiled_size, &info.linear_slice_stride})
		*field = ReadInteger<uint64_t>(stream);
	for (auto* field: {&info.width, &info.height, &info.depth, &info.pitch, &info.tail_x,
	                   &info.tail_y}) *field = ReadInteger<uint32_t>(stream);
	info.tail = ReadInteger<uint32_t>(stream) != 0;
	for (auto* field: {&info.tiled_width, &info.tiled_height, &info.surface_z})
		*field = ReadInteger<uint32_t>(stream);
	return info;
}

struct CaptureControl {
	std::filesystem::path directory;
	std::filesystem::path control;
	std::chrono::steady_clock::time_point next_poll {};
	bool armed = false;
	uint32_t count = 0;
	uint64_t total_bytes = 0;
	std::set<uint64_t> addresses;

	CaptureControl() {
		if (const auto* value = std::getenv("KYTY_DETILE_CAPTURE_DIR")) directory = value;
		if (const auto* value = std::getenv("KYTY_DETILE_CAPTURE_CONTROL_FILE")) control = value;
	}
	bool Wants(uint64_t address, uint64_t input_size, uint64_t output_size) {
		if (directory.empty() || control.empty() || address == 0) return false;
		const auto now = std::chrono::steady_clock::now();
		if (now >= next_poll) {
			next_poll = now + std::chrono::milliseconds {500};
			std::ifstream stream(control);
			std::string value;
			stream >> value;
			const bool enabled = value == "on" || value == "1";
			if (enabled && !armed) {
				addresses.clear(); count = 0; total_bytes = 0;
				std::printf("[detile capture] armed: %s\n", directory.string().c_str());
			}
			armed = enabled;
		}
		return armed && count < 8 && output_size >= (8ull << 20) && input_size <= MaxBytes &&
		       output_size <= MaxBytes && !addresses.contains(address) &&
		       input_size + output_size <= (512ull << 20) - total_bytes;
	}
};

} // namespace

uint64_t DetileReplayHash(std::span<const uint8_t> bytes) noexcept {
	uint64_t hash = 14695981039346656037ull;
	for (auto byte: bytes) { hash ^= byte; hash *= 1099511628211ull; }
	return hash;
}

bool WriteDetileReplay(const std::filesystem::path& path, const DetileReplay& replay,
                       std::string& error) {
	if (!Validate(replay, error)) return false;
	const auto temporary = std::filesystem::path(path.string() + ".tmp");
	std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
	if (!stream) { error = "cannot create replay file"; return false; }
	stream.write(Magic.data(), Magic.size());
	WriteInteger(stream, uint32_t {1});
	WriteInteger(stream, static_cast<uint32_t>(replay.infos.size()));
	for (auto value: {replay.guest_address, replay.frame, replay.source_offset,
	                  replay.tiled_capacity, replay.linear_capacity,
	                  static_cast<uint64_t>(replay.input.size()), DetileReplayHash(replay.input),
	                  DetileReplayHash(replay.expected)}) WriteInteger(stream, value);
	for (const auto& info: replay.infos) WriteInfo(stream, info);
	stream.write(reinterpret_cast<const char*>(replay.input.data()), replay.input.size());
	stream.write(reinterpret_cast<const char*>(replay.expected.data()), replay.expected.size());
	stream.close();
	if (!stream) { error = "failed writing replay file"; return false; }
	std::error_code ec;
	std::filesystem::rename(temporary, path, ec);
	if (ec) { error = "cannot publish replay file: " + ec.message(); return false; }
	return true;
}

bool ReadDetileReplay(const std::filesystem::path& path, DetileReplay& replay,
                      std::string& error) {
	std::ifstream stream(path, std::ios::binary);
	std::array<char, 8> magic {};
	stream.read(magic.data(), magic.size());
	const auto version = ReadInteger<uint32_t>(stream);
	const auto count = ReadInteger<uint32_t>(stream);
	if (!stream || magic != Magic || version != 1 || count == 0 || count > MaxInfos) {
		error = "not a supported detile replay file"; return false;
	}
	DetileReplay saved;
	for (auto* field: {&saved.guest_address, &saved.frame, &saved.source_offset,
	                   &saved.tiled_capacity, &saved.linear_capacity})
		*field = ReadInteger<uint64_t>(stream);
	const auto input_size = ReadInteger<uint64_t>(stream);
	const auto input_hash = ReadInteger<uint64_t>(stream);
	const auto expected_hash = ReadInteger<uint64_t>(stream);
	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	if (!stream || ec || input_size > MaxBytes || saved.linear_capacity > MaxBytes ||
	    size != 80ull + 88ull * count + input_size + saved.linear_capacity) {
		error = "truncated or oversized detile replay file"; return false;
	}
	for (uint32_t i = 0; i < count; ++i) saved.infos.push_back(ReadInfo(stream));
	saved.input.resize(input_size);
	saved.expected.resize(saved.linear_capacity);
	stream.read(reinterpret_cast<char*>(saved.input.data()), saved.input.size());
	stream.read(reinterpret_cast<char*>(saved.expected.data()), saved.expected.size());
	if (!stream || DetileReplayHash(saved.input) != input_hash ||
	    DetileReplayHash(saved.expected) != expected_hash) {
		error = "detile replay data checksum mismatch"; return false;
	}
	if (!Validate(saved, error)) return false;
	replay = std::move(saved);
	return true;
}

void MaybeCaptureDetile(GraphicContext& graphics, CommandScheduler& scheduler,
                        TileManager::Result source, TileManager::Result output,
                        std::span<const GpuTileInfo> infos, uint64_t guest_address) {
	static CaptureControl control;
	const auto alignment = std::max<uint64_t>(
	    graphics.GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment, 4);
	const auto descriptor_offset = Common::AlignDown(source.offset, alignment);
	const auto base = source.offset - descriptor_offset;
	const auto input_size = Common::AlignUp(base + source.size, 4);
	if (!control.Wants(guest_address, input_size, output.size)) return;

	// Copy actual GPU contents; guest RAM may lag behind the producer. Do not run deferred
	// cache retirement callbacks while UploadImage still borrows its source resource.
	Buffer download(graphics, scheduler, MemoryUsage::Download, 0,
	                vk::BufferUsageFlagBits::eTransferDst, input_size + output.size);
	scheduler.EndRendering();
	const auto command = scheduler.Current().Handle();
	std::array<vk::BufferMemoryBarrier, 2> before {};
	for (auto& barrier: before) {
		barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	before[0].buffer = source.buffer; before[0].offset = descriptor_offset; before[0].size = input_size;
	before[1].buffer = output.buffer; before[1].offset = output.offset; before[1].size = output.size;
	ProfilePipelineBarrier(command, vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, {}, before, {});
	const vk::BufferCopy input_copy {descriptor_offset, 0, input_size};
	const vk::BufferCopy output_copy {output.offset, input_size, output.size};
	command.copyBuffer(source.buffer, download.Handle(), input_copy);
	command.copyBuffer(output.buffer, download.Handle(), output_copy);
	vk::BufferMemoryBarrier host {};
	host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	host.dstAccessMask = vk::AccessFlagBits::eHostRead;
	host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = download.Handle(); host.size = download.Size();
	ProfilePipelineBarrier(command, vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eHost, {}, {}, host, {});
	scheduler.FlushAndWait();
	download.Invalidate(0, download.Size());
	DetileReplay replay;
	replay.guest_address = guest_address;
	replay.frame = scheduler.GpuFrameHint();
	replay.source_offset = base;
	replay.tiled_capacity = source.size;
	replay.linear_capacity = output.size;
	replay.infos.assign(infos.begin(), infos.end());
	const auto bytes = download.Mapped();
	replay.input.assign(bytes.begin(), bytes.begin() + input_size);
	replay.expected.assign(bytes.begin() + input_size, bytes.end());
	std::error_code ec;
	std::filesystem::create_directories(control.directory, ec);
	char name[96];
	std::snprintf(name, sizeof(name), "detile-%016llx-f%llu.kdr",
	              static_cast<unsigned long long>(guest_address),
	              static_cast<unsigned long long>(replay.frame));
	std::string error;
	if (ec || !WriteDetileReplay(control.directory / name, replay, error)) {
		std::fprintf(stderr, "[detile capture] failed: %s %s\n", ec.message().c_str(), error.c_str());
		control.armed = false;
		control.directory.clear(); // Do not repeatedly stall the running game on a disk error.
		return;
	}
	control.addresses.insert(guest_address);
	++control.count;
	control.total_bytes += input_size + output.size;
	std::printf("[detile capture] saved %s (%llu bytes, %zu layouts)\n", name,
	            static_cast<unsigned long long>(input_size + output.size), infos.size());
}

} // namespace Libs::Graphics
