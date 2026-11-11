#ifndef KYTY_UFC5_DETILE_REPLAY_RUNNER_H_
#define KYTY_UFC5_DETILE_REPLAY_RUNNER_H_

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/detileReplay.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <numeric>

namespace Libs::Graphics {

// Uses the production TileManager and newly built shaders, not saved SPIR-V or Vulkan objects.
inline int RunDetileReplayFiles(GraphicContext& graphics, RenderContext& renderer,
                               const std::filesystem::path& path, uint32_t iterations,
                               uint32_t warmup, const std::filesystem::path& csv_path,
                               bool verify_capture = false, bool profile_queries = false,
                               bool correlate_submits = false) {
	std::vector<std::filesystem::path> files;
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec)) {
		std::filesystem::directory_iterator it(path, ec), end;
		for (; !ec && it != end; it.increment(ec)) {
			if (it->path().extension() == ".kdr") files.push_back(it->path());
		}
		std::sort(files.begin(), files.end());
	} else files.push_back(path);
	if (ec || files.empty()) {
		std::fprintf(stderr, "[replay] no capture files: %s\n", path.string().c_str());
		return 1;
	}
	std::ofstream csv;
	std::ofstream correlation;
	if (correlate_submits) {
		correlation.open(csv_path.string() + ".iterations.csv", std::ios::trunc);
		if (!correlation) { std::fprintf(stderr, "[replay] cannot write iteration correlation CSV\n"); return 1; }
		correlation << "capture,iteration,warmup,frame,tick,thread,record_begin_ns,flush_begin_ns,flush_end_ns,"
		               "gpu_start_raw,gpu_end_raw,timestamp_period_ns,timestamp_mask,gpu_ms,wall_ms\n";
	}
	if (!csv_path.empty()) {
		csv.open(csv_path, std::ios::trunc);
		if (!csv) { std::fprintf(stderr, "[replay] cannot write CSV\n"); return 1; }
		csv << "capture,guest_address,tiled_bytes,linear_bytes,layouts,iterations,"
		       "gpu_median_ms,gpu_min_ms,gpu_max_ms,wall_median_ms,reference_matches,output_hash\n";
	}
	const auto properties = graphics.GetPhysicalDeviceProperties();
	const auto queues = graphics.physical_device.getQueueFamilyProperties();
	const auto valid_bits = queues[graphics.queue_family].timestampValidBits;
	if (valid_bits == 0) { std::fprintf(stderr, "[replay] GPU timestamps unavailable\n"); return 1; }
	const uint64_t mask = valid_bits == 64 ? UINT64_MAX : (uint64_t {1} << valid_bits) - 1;
	std::printf("[replay] device: %s; %zu captures; %u warmup + %u measured iterations\n",
	            properties.deviceName.data(), files.size(), warmup, iterations);
	std::printf("[replay] reports isolated detile latency, not game FPS or whole-game residency\n");
	bool matches = true;
	for (const auto& file: files) {
		DetileReplay replay;
		std::string error;
		if (!ReadDetileReplay(file, replay, error)) {
			std::fprintf(stderr, "[replay] %s: %s\n", file.string().c_str(), error.c_str());
			return 1;
		}
		CommandScheduler scheduler(renderer, graphics, "replay");
		HW::Context registers {};
		HW::UserConfig user_config {};
		HW::Shader shaders {};
		scheduler.Begin(registers, user_config, shaders);
		StreamBuffer parameters(graphics, scheduler, MemoryUsage::Stream, 1u << 20);
		Buffer upload(graphics, scheduler, MemoryUsage::Upload, 0,
		              vk::BufferUsageFlagBits::eTransferSrc, replay.input.size());
		Buffer input(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, replay.input.size());
		Buffer download(graphics, scheduler, MemoryUsage::Download, 0,
		                vk::BufferUsageFlagBits::eTransferDst, replay.linear_capacity);
		if (correlate_submits) {
			parameters.ProfileAllocation("replay_parameters_allocation");
			upload.ProfileAllocation("replay_upload_allocation");
			input.ProfileAllocation("replay_input_allocation");
			download.ProfileAllocation("replay_download_allocation");
		}
		std::memcpy(upload.Mapped().data(), replay.input.data(), replay.input.size());
		upload.Flush(0, upload.Size());
		input.CopyFrom(scheduler.Current(), upload, 0, 0, upload.Size(),
		               vk::AccessFlagBits::eHostWrite, {}, vk::AccessFlagBits::eTransferRead,
		               vk::AccessFlagBits::eShaderRead);
		scheduler.FlushAndWait(); // Upload and allocation are outside timed iterations.
		TileManager tiler(graphics, scheduler, parameters);
		vk::QueryPool query;
		vk::QueryPoolCreateInfo query_info {};
		query_info.queryType = vk::QueryType::eTimestamp; query_info.queryCount = 2;
		RequireVulkanSuccess(graphics.device.createQueryPool(&query_info, nullptr, &query),
		                     "create detile replay timestamps");
		std::vector<double> gpu_ms, wall_ms;
		struct Iteration { uint32_t index; uint64_t tick, thread, begin, flush, end; std::array<uint64_t, 2> gpu; double wall; };
		std::vector<Iteration> correlation_rows;
		if (correlate_submits) correlation_rows.reserve(warmup + iterations);
		TileManager::Result result;
		bool query_stress_done = false;
		for (uint32_t i = 0; i < warmup + iterations; ++i) {
			if (correlate_submits) scheduler.SetProfileFrame(i);
			else if (profile_queries) scheduler.SetProfileFrame(i / 256);
			if (profile_queries && scheduler.ProfileActive() && !query_stress_done) {
				// Exercise page exhaustion and a scope split across submission. No extra
				// GPU work or resource writes; only this explicit profiler test adds markers.
				scheduler.FlushAndWait();
				const auto split = scheduler.StartGpuTimer("test_split", scheduler.ProfileFrame());
				for (uint32_t n = 0; n < 300; ++n) {
					const auto marker = scheduler.StartGpuTimer("test_capacity", scheduler.ProfileFrame());
					scheduler.EndGpuTimer(marker);
				}
				scheduler.FlushAndWait();
				scheduler.EndGpuTimer(split); // Stale token must be ignored safely.
				query_stress_done = true;
			}
			const auto start = std::chrono::steady_clock::now();
			const auto begin_ns = correlate_submits ? ProfileClockNs() : 0;
			const auto command = scheduler.Current().Handle();
			command.resetQueryPool(query, 0, 2);
			command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, query, 0);
			result = tiler.Detile(input.Handle(), replay.source_offset, replay.tiled_capacity,
			                      replay.linear_capacity, replay.infos,
			                      correlate_submits ? replay.guest_address : 0);
			scheduler.Current().Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, 1);
			const auto tick = scheduler.CurrentTick();
			const auto flush_ns = correlate_submits ? ProfileClockNs() : 0;
			scheduler.FlushAndWait(); // A query slot is reused only after its submission completes.
			const auto end_ns = correlate_submits ? ProfileClockNs() : 0;
			const double wall = std::chrono::duration<double, std::milli>(
			    std::chrono::steady_clock::now() - start).count();
			std::array<uint64_t, 2> stamps {};
			RequireVulkanSuccess(graphics.device.getQueryPoolResults(
			                         query, 0, 2, sizeof(stamps), stamps.data(), sizeof(uint64_t),
			                         vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait),
			                     "collect detile replay timestamps");
			if (correlate_submits) correlation_rows.push_back(
			    {i, tick, ProfileThreadId(), begin_ns, flush_ns, end_ns, stamps, wall});
			if (i >= warmup) {
				gpu_ms.push_back(((stamps[1] - stamps[0]) & mask) * properties.limits.timestampPeriod / 1e6);
				wall_ms.push_back(wall);
			}
		}
		if (correlation) {
			std::string name = file.filename().string();
			for (size_t pos = 0; (pos = name.find('"', pos)) != std::string::npos; pos += 2) name.insert(pos, 1, '"');
			for (const auto& row: correlation_rows) {
				correlation << '"' << name << "\"," << row.index << ',' << (row.index < warmup) << ','
				            << row.index << ',' << row.tick << ',' << row.thread << ',' << row.begin << ','
				            << row.flush << ',' << row.end << ',' << row.gpu[0] << ',' << row.gpu[1] << ','
				            << properties.limits.timestampPeriod << ',' << mask << ','
				            << ((row.gpu[1]-row.gpu[0]) & mask)*properties.limits.timestampPeriod/1e6 << ','
				            << row.wall << '\n';
			}
		}
		if (verify_capture && replay.linear_capacity >= (8ull << 20)) {
			constexpr uint64_t address = 0x5b000000;
			MaybeCaptureDetile(graphics, scheduler,
			                   {input.Handle(), replay.source_offset, replay.tiled_capacity},
			                   result, replay.infos, address);
			DetileReplay captured;
			const auto directory = std::filesystem::path(std::getenv("KYTY_DETILE_CAPTURE_DIR"));
			if (!ReadDetileReplay(directory / "detile-000000005b000000-f0.kdr", captured, error) ||
			    captured.expected != replay.expected || captured.tiled_capacity != replay.tiled_capacity ||
			    captured.infos.size() != replay.infos.size() ||
			    !std::equal(replay.input.begin() + replay.source_offset, replay.input.end(),
			                captured.input.begin() + captured.source_offset, captured.input.end())) {
				std::fprintf(stderr, "[capture self-test] GPU snapshot round trip failed: %s\n", error.c_str());
				return 1;
			}
			std::printf("[capture self-test] actual GPU input/output snapshot MATCH\n");
		}
		const auto command = scheduler.Current().Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
		before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer = result.buffer; before.offset = result.offset; before.size = result.size;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
		                        vk::PipelineStageFlagBits::eTransfer, {}, {}, before, {});
		command.copyBuffer(result.buffer, download.Handle(), vk::BufferCopy {result.offset, 0, result.size});
		vk::BufferMemoryBarrier host = before;
		host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		host.dstAccessMask = vk::AccessFlagBits::eHostRead;
		host.buffer = download.Handle(); host.offset = 0;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                        vk::PipelineStageFlagBits::eHost, {}, {}, host, {});
		scheduler.FlushAndWait();
		download.Invalidate(0, download.Size());
		const auto bytes = download.Mapped();
		const auto mismatch = std::mismatch(replay.expected.begin(), replay.expected.end(), bytes.begin());
		const bool match = mismatch.first == replay.expected.end();
		matches &= match;
		if (!match) std::fprintf(stderr, "[replay] %s first output mismatch at byte %zu\n",
		                         file.filename().string().c_str(), size_t(mismatch.first - replay.expected.begin()));
		std::sort(gpu_ms.begin(), gpu_ms.end()); std::sort(wall_ms.begin(), wall_ms.end());
		const auto median = [](const std::vector<double>& values) {
			const auto middle = values.size() / 2;
			return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
		};
		const auto hash = DetileReplayHash(bytes);
		std::printf("[replay] %s: GPU %.3f ms (%.3f..%.3f), wall %.3f ms, reference %s\n",
		            file.filename().string().c_str(), median(gpu_ms), gpu_ms.front(), gpu_ms.back(),
		            median(wall_ms), match ? "MATCH" : "MISMATCH");
		if (csv) {
			std::string name = file.filename().string();
			for (size_t pos = 0; (pos = name.find('"', pos)) != std::string::npos; pos += 2) name.insert(pos, 1, '"');
			csv << '"' << name << "\",0x" << std::hex << replay.guest_address << std::dec << ','
			    << replay.tiled_capacity << ',' << replay.linear_capacity << ',' << replay.infos.size() << ','
			    << iterations << ',' << median(gpu_ms) << ',' << gpu_ms.front() << ',' << gpu_ms.back() << ','
			    << median(wall_ms) << ',' << match << ",0x" << std::hex << hash << std::dec << '\n';
		}
		scheduler.Finish();
		graphics.device.destroyQueryPool(query);
	}
	if (csv) { csv.flush(); if (!csv) return 1; }
	if (correlation) { correlation.flush(); if (!correlation) return 1; }
	return matches ? 0 : 2;
}

// A constructed depth/stencil upload pair. KDR v1 saves tile layouts and bytes, not
// the native image format or the rest of the guest submission. D32S8 is explicit
// here; this is not an exact replay of the entire production batch.
inline int RunDetileUploadPair(GraphicContext& graphics, RenderContext& renderer,
                              const std::filesystem::path& depth_path,
                              const std::filesystem::path& stencil_path,
                              uint32_t iterations, uint32_t warmup,
                              const std::filesystem::path& csv_path) {
	std::array<DetileReplay, 2> replays;
	std::string error;
	if (!ReadDetileReplay(depth_path, replays[0], error) ||
	    !ReadDetileReplay(stencil_path, replays[1], error)) {
		std::fprintf(stderr, "[upload pair] %s\n", error.c_str()); return 1;
	}
	for (size_t n = 0; n < replays.size(); ++n) {
		const auto& replay = replays[n];
		if (replay.infos.size() != 1) {
			std::fprintf(stderr, "[upload pair] only single-layout captures supported\n"); return 1;
		}
		const auto& info = replay.infos.front();
		if (info.family != TileBlockFamily::Depth64KB || info.bytes_per_element != (n == 0 ? 4u : 1u) ||
		    info.depth != 1 || info.linear_offset != 0 || info.tail) {
			std::fprintf(stderr, "[upload pair] requires a 4-byte depth / 1-byte stencil 2D pair\n"); return 1;
		}
	}
	const auto& depth = replays[0].infos.front();
	const auto& stencil = replays[1].infos.front();
	if (depth.width != stencil.width || depth.height != stencil.height) {
		std::fprintf(stderr, "[upload pair] mismatched image extents\n"); return 1;
	}
	const auto properties = graphics.GetPhysicalDeviceProperties();
	const auto queues = graphics.physical_device.getQueueFamilyProperties();
	const auto bits = queues[graphics.queue_family].timestampValidBits;
	if (bits == 0) { std::fprintf(stderr, "[upload pair] timestamps unavailable\n"); return 1; }
	const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
	std::ofstream csv;
	if (!csv_path.empty()) {
		csv.open(csv_path, std::ios::trunc);
		if (!csv) { std::fprintf(stderr, "[upload pair] cannot write CSV\n"); return 1; }
		csv << "iteration,format,width,height,depth_address,stencil_address,depth_detile_elapsed_ms,"
		       "depth_upload_elapsed_ms,stencil_detile_elapsed_ms,stencil_upload_elapsed_ms,"
		       "batch_elapsed_ms,cpu_record_ms,flush_wait_ms,wall_ms,depth_image_matches,stencil_image_matches\n";
	}
	CommandScheduler scheduler(renderer, graphics, "upload-pair");
	HW::Context registers{}; HW::UserConfig config{}; HW::Shader shaders{};
	scheduler.Begin(registers, config, shaders);
	StreamBuffer parameters(graphics, scheduler, MemoryUsage::Stream, 1u << 20);
	std::array<std::unique_ptr<Buffer>, 2> inputs, downloads;
	for (size_t n = 0; n < replays.size(); ++n) {
		const auto& replay = replays[n];
		Buffer upload(graphics, scheduler, MemoryUsage::Upload, 0,
		              vk::BufferUsageFlagBits::eTransferSrc, replay.input.size());
		inputs[n] = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
		                                   AllFlags, replay.input.size());
		downloads[n] = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
		                                      vk::BufferUsageFlagBits::eTransferDst, replay.linear_capacity);
		std::memcpy(upload.Mapped().data(), replay.input.data(), replay.input.size());
		upload.Flush(0, upload.Size());
		inputs[n]->CopyFrom(scheduler.Current(), upload, 0, 0, upload.Size(),
		                    vk::AccessFlagBits::eHostWrite, {}, vk::AccessFlagBits::eTransferRead,
		                    vk::AccessFlagBits::eShaderRead);
		scheduler.FlushAndWait(); // Input transfer and allocations excluded from timing.
	}
	ImageInfo image_info{};
	image_info.pixel_format = vk::Format::eD32SfloatS8Uint;
	image_info.guest_format = Prospero::BufferFormat::k32Float;
	image_info.extent = {depth.width, depth.height, 1};
	image_info.pitch = depth.pitch;
	image_info.bytes_per_block = 4;
	image_info.data = {replays[0].guest_address, replays[0].tiled_capacity};
	image_info.stencil = {replays[1].guest_address, replays[1].tiled_capacity};
	Image image(graphics, scheduler, image_info);
	std::array<vk::BufferImageCopy, 2> copies{};
	for (size_t n = 0; n < replays.size(); ++n) {
		const auto& info = replays[n].infos.front();
		copies[n].bufferRowLength = info.pitch;
		copies[n].bufferImageHeight = info.height;
		copies[n].imageSubresource.aspectMask = n == 0 ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eStencil;
		copies[n].imageSubresource.layerCount = 1;
		copies[n].imageExtent = {info.width, info.height, 1};
	}
	TileManager tiler(graphics, scheduler, parameters);
	vk::QueryPool query;
	vk::QueryPoolCreateInfo query_info{};
	query_info.queryType = vk::QueryType::eTimestamp; query_info.queryCount = 5;
	RequireVulkanSuccess(graphics.device.createQueryPool(&query_info, nullptr, &query), "upload pair timestamps");
	struct Sample { std::array<double, 5> gpu; double record, wait, wall; };
	std::vector<Sample> samples;
	std::printf("[upload pair] %s; constructed D32S8 %ux%u, %u warmup + %u measured\n",
	            properties.deviceName.data(), depth.width, depth.height, warmup, iterations);
	std::printf("[upload pair] production Detile + Image::Upload, one submission, no mid-pair host wait\n");
	std::printf("[upload pair] adjacent timestamp differences are elapsed stage brackets, not pure shader/copy execution\n");
	for (uint32_t i = 0; i < warmup + iterations; ++i) {
		const auto start = std::chrono::steady_clock::now();
		auto command = scheduler.Current().Handle();
		command.resetQueryPool(query, 0, 5);
		command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, query, 0);
		for (size_t n = 0; n < replays.size(); ++n) {
			const auto& replay = replays[n];
			const auto linear = tiler.Detile(inputs[n]->Handle(), replay.source_offset,
			                                replay.tiled_capacity, replay.linear_capacity, replay.infos);
			command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, 1 + 2*n);
			auto copy = copies[n]; copy.bufferOffset += linear.offset;
			image.Upload(std::span{&copy, size_t{1}}, linear.buffer, linear.offset, linear.size);
			command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query, 2 + 2*n);
		}
		const auto recorded = std::chrono::steady_clock::now();
		scheduler.FlushAndWait();
		const auto completed = std::chrono::steady_clock::now();
		std::array<uint64_t, 5> stamps{};
		RequireVulkanSuccess(graphics.device.getQueryPoolResults(query, 0, 5, sizeof(stamps),
		                     stamps.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait),
		                     "collect upload pair timestamps");
		if (i >= warmup) {
			Sample sample{};
			for (size_t n = 0; n < 4; ++n)
				sample.gpu[n] = ((stamps[n+1] - stamps[n]) & mask) * properties.limits.timestampPeriod / 1e6;
			sample.gpu[4] = ((stamps[4] - stamps[0]) & mask) * properties.limits.timestampPeriod / 1e6;
			sample.record = std::chrono::duration<double, std::milli>(recorded-start).count();
			sample.wait = std::chrono::duration<double, std::milli>(completed-recorded).count();
			sample.wall = std::chrono::duration<double, std::milli>(completed-start).count();
			samples.push_back(sample);
		}
	}
	// Read BOTH image aspects after the last iteration, outside the measured batch.
	// Padding has no image representation; compare every active texel byte only.
	std::array<bool, 2> matches{true, true};
	for (size_t n = 0; n < replays.size(); ++n) {
		image.Download(std::span{&copies[n], size_t{1}}, downloads[n]->Handle(), 0, downloads[n]->Size());
		auto command = scheduler.Current().Handle();
		vk::BufferMemoryBarrier host{};
		host.srcAccessMask = vk::AccessFlagBits::eTransferWrite; host.dstAccessMask = vk::AccessFlagBits::eHostRead;
		host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.buffer = downloads[n]->Handle(); host.size = downloads[n]->Size();
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {}, host, {});
		scheduler.FlushAndWait();
		downloads[n]->Invalidate(0, downloads[n]->Size());
		const auto bytes = downloads[n]->Mapped();
		const auto& info = replays[n].infos.front();
		for (uint32_t y = 0; y < info.height; ++y) {
			const size_t offset = uint64_t(y) * info.pitch * info.bytes_per_element;
			if (std::memcmp(bytes.data()+offset, replays[n].expected.data()+offset,
			                uint64_t(info.width)*info.bytes_per_element) != 0) {
				std::fprintf(stderr, "[upload pair] aspect %zu mismatch at row %u\n", n, y);
				matches[n] = false; break;
			}
		}
	}
	if (csv) {
		for (size_t i = 0; i < samples.size(); ++i) {
			const auto& s = samples[i];
			csv << i << ",D32S8," << depth.width << ',' << depth.height << ",0x" << std::hex
			    << replays[0].guest_address << ",0x" << replays[1].guest_address << std::dec;
			for (auto ms: s.gpu) csv << ',' << ms;
			csv << ',' << s.record << ',' << s.wait << ',' << s.wall << ',' << matches[0] << ',' << matches[1] << '\n';
		}
		csv.flush(); if (!csv) return 1;
	}
	std::vector<double> totals;
	for (const auto& s: samples) totals.push_back(s.gpu[4]);
	std::sort(totals.begin(), totals.end());
	const size_t middle = totals.size()/2;
	const double median = totals.size()%2 ? totals[middle] : (totals[middle-1]+totals[middle])/2;
	std::printf("[upload pair] batch GPU median %.3f ms (%.3f..%.3f); depth image %s; stencil image %s\n",
	            median, totals.front(), totals.back(), matches[0] ? "MATCH" : "MISMATCH", matches[1] ? "MATCH" : "MISMATCH");
	scheduler.Finish();
	graphics.device.destroyQueryPool(query);
	return matches[0] && matches[1] ? 0 : 2;
}

// Small independent CPU-reference fixtures exercise the disk round trip and production GPU path.
inline bool MakeDetileReplayFixtures(const std::filesystem::path& directory, std::string& error) {
	std::error_code ec;
	std::filesystem::create_directories(directory, ec);
	if (ec) { error = ec.message(); return false; }
	uint32_t index = 0;
	for (const auto family: {TileBlockFamily::RenderTarget64KB, TileBlockFamily::Depth64KB}) {
		for (const uint32_t bpe: {4u, 8u}) {
			TileBlockLayout block;
			if (!TileGetBlockLayout(family, bpe, block)) { error = "missing fixture tile layout"; return false; }
			DetileReplay replay;
			replay.source_offset = 20;
			const uint32_t columns = index == 0 ? 17 : 2;
			const uint32_t rows = index == 0 ? 9 : 2;
			replay.tiled_capacity = block.block_size * columns * rows;
			GpuTileInfo info;
			info.family = family; info.bytes_per_element = bpe;
			info.width = block.block_width * (columns - 1) + 3;
			info.height = block.block_height * (rows - 1) + 5;
			info.pitch = block.block_width * columns; info.tiled_width = info.pitch;
			info.tiled_height = block.block_height * rows; info.surface_z = 3;
			info.tiled_size = replay.tiled_capacity;
			info.linear_size = uint64_t(info.pitch) * info.height * bpe;
			replay.linear_capacity = info.linear_size;
			replay.infos.push_back(info);
			replay.input.resize(replay.source_offset + replay.tiled_capacity);
			replay.expected.resize(replay.linear_capacity, 0);
			for (size_t i = 0; i < replay.input.size(); ++i)
				replay.input[i] = static_cast<uint8_t>((i * 41u) ^ (i >> 8u) ^ (i >> 16u));
			for (uint32_t y = 0; y < info.height; ++y) for (uint32_t x = 0; x < info.width; ++x) {
				const auto bx = x / block.block_width, by = y / block.block_height;
				uint32_t local = 0, block_xor = 0;
				if (!TileGetBlockOffset(block, x % block.block_width, y % block.block_height, 0, local) ||
				    !TileGetBlockXor(block, bx, by, info.surface_z, block_xor)) {
					error = "fixture CPU address calculation failed"; return false;
				}
				const uint64_t source = replay.source_offset + (by * columns + bx) * block.block_size + (local ^ block_xor);
				const uint64_t destination = (uint64_t(y) * info.pitch + x) * bpe;
				std::memcpy(replay.expected.data() + destination, replay.input.data() + source, bpe);
			}
			const auto path = directory / ("synthetic-detile-" + std::to_string(++index) + ".kdr");
			if (!WriteDetileReplay(path, replay, error)) return false;
		}
	}
	return true;
}

} // namespace Libs::Graphics

#endif
