#include "graphics/host_gpu/metal/metalAddressSpace.h"
#include "graphics/host_gpu/metal/metalBuffer.h"
#include "graphics/host_gpu/metal/metalContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace Libs::Graphics::Metal {
namespace {
constexpr uint64_t kPageSize = 1ull << 14;
constexpr uint64_t kLeafEntries = 1ull << 10;
constexpr uint64_t kLowerSize = 1ull << 40;
constexpr uint64_t kPackedSize = kLowerSize + LibKernel::Memory::kExtendedMemorySize;
constexpr uint64_t kRootEntries = kPackedSize / (kPageSize * kLeafEntries);
constexpr uint64_t kChunkSize = MetalBufferManager::kBackingChunkSize;

uint64_t PackedAddress(uint64_t address) {
	if (address < kLowerSize) return address;
	if (address >= LibKernel::Memory::kExtendedMemoryBase &&
	    address - LibKernel::Memory::kExtendedMemoryBase < LibKernel::Memory::kExtendedMemorySize) {
		return address - LibKernel::Memory::kExtendedMemoryBase + kLowerSize;
	}
	return UINT64_MAX;
}

class GuestAddressSpace {
public:
	uint64_t Bind() {
		std::vector<LibKernel::Memory::GuestBackingMapping> mappings;
		uint64_t generation = m_generation;
		if (LibKernel::Memory::GetGuestBackingMappings(generation, mappings)) {
			// KYTY_LOG_BDA=1: how often the table is rebuilt and what it costs.
			static const bool s_log = std::getenv("KYTY_LOG_BDA") != nullptr;
			const auto        t0    = std::chrono::steady_clock::now();
			const bool        ok    = Rebuild(mappings);
			if (s_log) {
				static uint64_t s_n = 0, s_us = 0;
				s_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
				if (++s_n % 100 == 0) std::fprintf(stderr, "[MetalBda] %llu rebuilds, %llu us total\n", (unsigned long long)s_n, (unsigned long long)s_us);
			}
			if (!ok) {
				std::fprintf(stderr, "[MetalBda] could not publish guest mapping generation %llu\n", static_cast<unsigned long long>(generation));
				return 0;
			}
			m_generation = generation;
		}
		if (m_root == nil) return 0;
		// The table stores raw GPU addresses, so binding the root alone cannot
		// make the leaves or shared physical backing resident. Encoders retain
		// this immutable version while newer mappings create a new version.
		if (auto encoder = MetalContext::Instance().GetActiveRenderEncoder()) {
			[encoder useResources:m_resources.data() count:m_resources.size() usage:MTLResourceUsageRead
			                stages:MTLRenderStageVertex | MTLRenderStageFragment];
		}
		if (auto encoder = MetalContext::Instance().GetActiveComputeEncoder()) {
			[encoder useResources:m_resources.data() count:m_resources.size() usage:MTLResourceUsageRead];
		}
		return m_root.gpuAddress;
	}

private:
	bool Rebuild(const std::vector<LibKernel::Memory::GuestBackingMapping>& mappings) {
		uint64_t alias = 0, size = 0;
		if (!LibKernel::Memory::GetGuestBackingAlias(&alias, &size)) return false;
		using Leaf = std::array<uint64_t, kLeafEntries>;
		std::map<uint32_t, Leaf> tables;
		std::set<uint64_t> used_chunks;
		for (const auto& mapping: mappings) {
			// Guest physical mappings have 16 KiB granularity. Reject malformed
			// ranges rather than publishing pointers outside the backing store.
			if (((mapping.address | mapping.backing_offset | mapping.size) & (kPageSize - 1)) != 0 ||
			    mapping.backing_offset > size || mapping.size > size - mapping.backing_offset) return false;
			// The chunk buffer and the leaf only change every 64 MiB / 16 MiB: look them up when they do, not for every 16 KiB page
			// (three tree lookups per page made a rebuild cost ~40% of the GPU thread while the game mapped and unmapped memory).
			uint64_t cur_chunk = UINT64_MAX, chunk_gpu = 0;
			uint64_t cur_root  = UINT64_MAX;
			Leaf*    leaf      = nullptr;
			for (uint64_t offset = 0; offset < mapping.size; offset += kPageSize) {
				const auto packed = PackedAddress(mapping.address + offset);
				if (packed == UINT64_MAX) continue;
				const auto physical = mapping.backing_offset + offset;
				const auto chunk = physical / kChunkSize;
				if (chunk != cur_chunk) {
					auto& buffer = m_chunks[chunk];
					if (!buffer) {
						// The same chunk buffers as the guest range bindings (MetalBufferManager), so both see one Metal buffer per physical byte.
						buffer = MetalBufferManager::Instance().GetBackingChunk(chunk);
						if (!buffer) {
							m_chunks.erase(chunk);
							return false;
						}
					}
					used_chunks.insert(chunk);
					cur_chunk = chunk;
					chunk_gpu = buffer->GetGpuAddress();
				}
				const auto root = packed / (kPageSize * kLeafEntries);
				if (root != cur_root) {
					leaf     = &tables[static_cast<uint32_t>(root)];
					cur_root = root;
				}
				const auto page = (packed / kPageSize) & (kLeafEntries - 1);
				(*leaf)[page] = chunk_gpu + physical % kChunkSize;
			}
		}
		auto device = MetalContext::Instance().GetDevice();
		id<MTLBuffer> root = [device newBufferWithLength:kRootEntries * sizeof(uint64_t) options:MTLResourceStorageModeShared];
		id<MTLBuffer> leaves = [device newBufferWithLength:std::max<size_t>(1, tables.size()) * sizeof(Leaf) options:MTLResourceStorageModeShared];
		if (root == nil || leaves == nil) return false;
		std::memset(root.contents, 0, root.length);
		uint64_t leaf_offset = 0;
		for (const auto& [index, leaf]: tables) {
			static_cast<uint64_t*>(root.contents)[index] = leaves.gpuAddress + leaf_offset;
			std::memcpy(static_cast<uint8_t*>(leaves.contents) + leaf_offset, leaf.data(), sizeof(Leaf));
			leaf_offset += sizeof(Leaf);
		}
		m_root = root;
		m_leaves = leaves;
		m_resources = {root, leaves};
		for (auto chunk: used_chunks) m_resources.push_back(m_chunks.at(chunk)->GetHandle());
		if (std::getenv("KYTY_LOG_BDA") != nullptr) {
			std::fprintf(stderr, "[MetalBda] mappings=%zu leaves=%zu backing_chunks=%zu table_bytes=%lu\n",
			             mappings.size(), tables.size(), used_chunks.size(), static_cast<unsigned long>(root.length + leaves.length));
		}
		return true;
	}

	uint64_t m_generation = 0;
	id<MTLBuffer> m_root = nil;
	id<MTLBuffer> m_leaves = nil;
	std::map<uint64_t, std::shared_ptr<MetalBuffer>> m_chunks;
	std::vector<id<MTLResource>> m_resources;
};
}

uint64_t BindGuestAddressSpace() {
	// Graphics/compute encoding is serialized by MetalRenderer::m_mutex.
	static GuestAddressSpace space;
	return space.Bind();
}
}
