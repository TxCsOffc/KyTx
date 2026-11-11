#pragma once

// Metal adapter of MemoryCoherence for private tiled 2D textures (one mip, uncompressed or block elements).
//
// A private texture holds texels in linear order, guest memory holds them tiled. MemoryCoherence (host_gpu/memoryCoherence.h) says
// which bytes of a physical backing are newest where; this class performs the copies it plans:
//   * ReadSurface       a GPU consumer is about to read the texture: bytes written by the CPU, by raw buffer writers or by another
//                       surface reach guest memory first, then the texture is imported from guest memory (tile import kernel).
//   * WriteSurface      a GPU pass wrote the texture: its footprint now belongs to it; guest memory keeps the older bytes until
//                       somebody needs them.
//   * MaterializeRange  a raw buffer reader, a release/acquire writeback or the CPU needs the bytes of a physical range: surfaces that
//                       own newer bytes are exported (tile export kernel) and only the owned sub-ranges are copied to guest memory.
//   * RecordGuestWrite  the CPU, a DMA or a raw buffer shader wrote bytes of the backing.
// Everything is encoded into the caller's command buffer in GPU order; nothing waits. A CPU read of materialized bytes still needs the
// completion of that command buffer. Callers serialize access (the renderer mutex), the tracker has its own lock.

#include "graphics/host_gpu/memoryCoherence.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/metal/metalSurfaceFootprint.h"
#include "graphics/host_gpu/metal/metalTiling.h"

#import <Metal/Metal.h>

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace Libs::Graphics::Metal {

class SurfaceCoherence {
public:
	using BackingId = Coherence::BackingId;
	using OwnerId   = Coherence::OwnerId;
	using Range     = Coherence::Range;

	// The Metal buffer holding a physical range and the offset of its first byte (the range must lie in one buffer).
	struct GuestSpan {
		id<MTLBuffer> buffer = nil;
		NSUInteger    offset = 0;
	};
	using GuestResolver = std::function<std::optional<GuestSpan>(Range)>;

	explicit SurfaceCoherence(id<MTLDevice> device, GuestResolver resolver): device_(device), resolver_(std::move(resolver)) {}

	bool RegisterBacking(BackingId backing, uint64_t size) { return tracker_.RegisterBacking(backing, size); }

	// `texture` is an uint element texture of the surface's element size; `base` is the byte offset of texel (0,0)'s block in the backing.
	bool RegisterSurface(OwnerId owner, BackingId backing, uint64_t base, id<MTLTexture> texture, const TileTables& tables, uint32_t width,
	                     uint32_t height) {
		std::vector<FootprintRange> footprint;
		if (texture == nil || !TiledSurfaceFootprint(tables, width, height, footprint)) return false;
		Surface s;
		s.backing = backing;
		s.base    = base;
		s.texture = texture;
		s.tables  = tables;
		s.width   = width;
		s.height  = height;
		for (const auto& r: footprint) s.footprint.push_back({backing, base + r.begin, r.size});
		const uint64_t extent = uint64_t(tables.columns) * tables.rows * tables.block.block_size;
		s.exported = [device_ newBufferWithLength:extent options:MTLResourceStorageModeShared];
		if (s.exported == nil || !tracker_.RegisterView(owner, s.footprint)) return false;
		surfaces_.emplace(owner, std::move(s));
		return true;
	}

	// Bring the texture up to date with the newest bytes of its footprint. A texture that already holds them costs no GPU work.
	bool ReadSurface(OwnerId owner, id<MTLCommandBuffer> cmd) {
		auto it = surfaces_.find(owner);
		if (it == surfaces_.end()) return false;
		Surface& s = it->second;
		std::vector<Coherence::ReadPlan> plans;
		bool stale = false;
		for (const auto& range: s.footprint) {
			auto plan = tracker_.PlanRead(owner, range);
			if (!plan) return false;
			stale = stale || !plan->copies.empty();
			plans.push_back(std::move(*plan));
		}
		if (!stale) return true;
		// Guest memory first holds every byte of the footprint, then the import reads it.
		for (size_t i = 0; i < s.footprint.size(); ++i) if (!plans[i].copies.empty() && !MaterializeRange(s.footprint[i], cmd)) return false;
		const auto whole = resolver_({s.backing, s.base, uint64_t(s.tables.columns) * s.tables.rows * s.tables.block.block_size});
		if (!whole || !EncodeTileImport(device_, cmd, whole->buffer, whole->offset, s.texture, s.tables, s.width, s.height)) return false;
		s.exported_current = false;
		for (const auto& range: s.footprint) {
			auto plan = tracker_.PlanRead(owner, range);
			if (!plan || !tracker_.CompleteRead(*plan)) return false;
		}
		return true;
	}

	// A GPU pass wrote the texture. Without proof that every footprint byte was written the old bytes stay valid (PreserveUnwritten
	// requires the texture to be current, see ReadSurface); OverwriteRange is for passes known to cover the whole footprint.
	bool WriteSurface(OwnerId owner, Coherence::WriteMode mode) {
		auto it = surfaces_.find(owner);
		if (it == surfaces_.end()) return false;
		for (const auto& range: it->second.footprint) if (!tracker_.RecordWrite(owner, range, mode)) return false;
		it->second.exported_current = false;
		return true;
	}

	// Make guest memory current for `range`: exports each owning surface once per change and copies only the bytes it owns.
	bool MaterializeRange(Range range, id<MTLCommandBuffer> cmd) {
		const auto dest = resolver_(range);
		if (!dest) return false;
		auto copy = [&](const Coherence::Stamp& from, Coherence::OwnerId target) {
			if (target != Coherence::GuestBytes || from.owner == Coherence::GuestBytes) return false;
			const auto found = surfaces_.find(from.owner);
			if (found == surfaces_.end()) return false;
			Surface& s = found->second;
			if (!s.exported_current) {
				if (!EncodeTileExport(device_, cmd, s.texture, s.exported, 0, s.tables, s.width, s.height)) return false;
				s.exported_current = true;
			}
			id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
			OrderBegin(blit);
			[blit copyFromBuffer:s.exported sourceOffset:from.range.begin - s.base toBuffer:dest->buffer
			   destinationOffset:dest->offset + (from.range.begin - range.begin) size:from.range.size];
			OrderEnd(blit);
			[blit endEncoding];
			return true;
		};
		return tracker_.Read(Coherence::GuestBytes, range, copy);
	}

	bool RecordGuestWrite(Range range, Coherence::WriteMode mode) { return tracker_.RecordWrite(Coherence::GuestBytes, range, mode); }

	// Refuses (false) while the surface owns the only current copy of some bytes; MaterializeRange first.
	bool ForgetSurface(OwnerId owner) {
		if (!tracker_.ForgetView(owner)) return false;
		surfaces_.erase(owner);
		return true;
	}

	std::optional<std::vector<Coherence::Stamp>> Latest(Range range) const { return tracker_.Latest(range); }
	const std::vector<Range>& Footprint(OwnerId owner) const { return surfaces_.at(owner).footprint; }

private:
	struct Surface {
		BackingId           backing = 0;
		uint64_t            base = 0;
		id<MTLTexture>      texture = nil;
		id<MTLBuffer>       exported = nil; // tiled image of the whole texture, filled by the export kernel
		TileTables          tables;
		uint32_t            width = 0, height = 0;
		bool                exported_current = false;
		std::vector<Range>  footprint;
	};
	id<MTLDevice>                device_;
	GuestResolver                resolver_;
	Coherence::MemoryCoherence   tracker_;
	std::map<OwnerId, Surface>   surfaces_;
};

} // namespace Libs::Graphics::Metal
