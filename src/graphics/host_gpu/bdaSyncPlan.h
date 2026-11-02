#pragma once

#include "graphics/host_gpu/memoryTracker.h"
#include <array>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

enum class BdaSyncAction { Reference, Compare, Candidate };
struct BdaSyncCounters {
	uint64_t reference_visits = 0, candidate_visits = 0, dirty_bytes = 0;
	uint64_t regions_considered = 0, regions_skipped = 0;
	uint64_t upload_calls = 0, upload_bytes = 0, uncovered_upload_bytes = 0;
	uint64_t full_passes = 0, unchanged_passes = 0;
	bool compared = false, matched = false, raced = false;
};

// Dirty-page/epoch selection adapted from GTA e5dd76e7. Scratch belongs to the
// serialized buffer-cache owner. Guest bytes and GPU synchronization are untouched.
struct BdaSyncPlan {
	using Epochs = std::array<uint64_t, 3>;
	Epochs epochs {};
	RangeSet dirty_spans;
	std::vector<std::pair<uint64_t, uint64_t>> regions;
	uint64_t regions_considered = 0, regions_skipped = 0, dirty_bytes = 0;
	bool full = false, unchanged = false;
};

class BdaSyncHistory {
public:
	BdaSyncPlan Build(MemoryTracker& tracker, const RangeSet& mapped,
	                  uint64_t registered, uint64_t mapping) const {
		BdaSyncPlan plan;
		plan.epochs = {RegionManager::CpuDirtyEpoch(), registered, mapping};
		plan.unchanged = plan.epochs == m_epochs;
		if (plan.unchanged) return plan;
		plan.full = plan.epochs[1] != m_epochs[1] || plan.epochs[2] != m_epochs[2];
		uint64_t last_region = UINT64_MAX;
		mapped.ForEach([&](uint64_t begin, uint64_t end) {
			for (auto index = begin / TRACKER_REGION_SIZE; index * TRACKER_REGION_SIZE < end; ++index) {
				if (index == last_region) continue;
				last_region = index;
				++plan.regions_considered;
				const auto epoch = tracker.RegionCpuDirtyEpoch(index);
				const auto previous = m_regions.find(index);
				if (!plan.full && epoch != 0 && previous != m_regions.end() && previous->second == epoch) {
					++plan.regions_skipped; continue;
				}
				// Read before snapshot/upload, never afterward. A concurrent write must
				// advance beyond the remembered epoch and be visited on the next pass.
				plan.regions.emplace_back(index, epoch);
				mapped.ForEachInRange(index * TRACKER_REGION_SIZE, TRACKER_REGION_SIZE, [&](uint64_t start, uint64_t finish) {
					tracker.ForEachCpuDirtySpan(start, finish - start, [&](uint64_t address, uint64_t bytes) {
						plan.dirty_spans.Add(address, bytes);
					});
				});
			}
		});
		plan.dirty_spans.ForEach([&](uint64_t begin, uint64_t end) { plan.dirty_bytes += end - begin; });
		return plan;
	}
	void Commit(const BdaSyncPlan& plan) {
		if (plan.full) m_regions.clear();
		for (const auto& [index, epoch]: plan.regions) m_regions[index] = epoch;
		m_epochs = plan.epochs;
	}
	void Reset() { m_epochs.fill(UINT64_MAX); m_regions.clear(); }
private:
	BdaSyncPlan::Epochs m_epochs {UINT64_MAX, UINT64_MAX, UINT64_MAX};
	std::unordered_map<uint64_t, uint64_t> m_regions;
};

} // namespace Libs::Graphics
