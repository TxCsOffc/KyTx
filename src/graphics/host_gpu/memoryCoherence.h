#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace Libs::Graphics::Coherence {

using BackingId = uint64_t;
using OwnerId = uint64_t;
inline constexpr OwnerId GuestBytes = 0;

struct Range {
	BackingId backing = 0;
	uint64_t begin = 0, size = 0;
	bool operator==(const Range&) const = default;
};
struct Stamp {
	Range range;
	OwnerId owner = GuestBytes;
	uint64_t version = 0;
	bool operator==(const Stamp&) const = default;
};
struct ReadPlan {
	OwnerId target = GuestBytes;
	std::vector<Stamp> required; // All versions observed, including already-valid bytes.
	std::vector<Stamp> copies;   // Only bytes missing from target; source is stamp.owner.
};
enum class WriteMode { OverwriteRange, PreserveUnwritten };

// Encode-order bookkeeping, independent of Metal resource objects. No bytes are
// initialized here: version 0 means the existing guest allocation, not zeros.
// Adapters must serialize data access, retain sources, encode every copy in the
// plan with proper GPU ordering, then call CompleteRead. A CPU read also needs
// the corresponding GPU completion; marking a replica valid is not completion.
class MemoryCoherence {
	template<class T> struct Span { uint64_t end; T value; };
	struct Writer {
		OwnerId owner = GuestBytes;
		uint64_t version = 0;
		bool operator==(const Writer&) const = default;
	};
	template<class T> using Intervals = std::map<uint64_t, Span<T>>;
	struct Backing { uint64_t size; Intervals<Writer> latest; };
	struct View {
		std::vector<Range> coverage;
		std::map<BackingId, Intervals<uint64_t>> replicas;
	};

public:
	MemoryCoherence() { views_.emplace(GuestBytes, View {}); }
	bool RegisterBacking(BackingId id, uint64_t size) {
		std::lock_guard lock(mutex_);
		if (size == 0 || backings_.contains(id)) return false;
		backings_.emplace(id, Backing {size, {{0, {size, {GuestBytes, 0}}}}});
		views_.at(GuestBytes).replicas[id].emplace(0, Span<uint64_t> {size, 0});
		return true;
	}
	bool RegisterView(OwnerId id, std::vector<Range> coverage) {
		std::lock_guard lock(mutex_);
		if (id == GuestBytes || used_ids_.contains(id) || coverage.empty()) return false;
		for (const auto& range: coverage) if (!ValidRange(range)) return false;
		std::sort(coverage.begin(), coverage.end(), [](auto a, auto b) {
			return a.backing < b.backing || (a.backing == b.backing && a.begin < b.begin);
		});
		std::vector<Range> merged;
		for (const auto& range: coverage) {
			if (!merged.empty() && merged.back().backing == range.backing &&
			    range.begin <= merged.back().begin + merged.back().size) {
				auto& prev = merged.back();
				prev.size = std::max(prev.begin + prev.size, range.begin + range.size) - prev.begin;
			} else merged.push_back(range);
		}
		views_.emplace(id, View {std::move(merged), {}});
		used_ids_.insert(id); // Resource lifetime IDs cannot be recycled by a cache.
		return true;
	}
	// `copy` runs under the tracker lock and must not re-enter this tracker.
	template<class Copier> bool Read(OwnerId target, Range range, Copier&& copy) {
		std::lock_guard lock(mutex_);
		auto plan = PlanReadLocked(target,range);
		if (!plan) return false;
		for (const auto& stamp: plan->copies) if (!copy(stamp,target)) return false;
		PublishRead(*plan);
		return true;
	}
	std::optional<ReadPlan> PlanRead(OwnerId target, Range range) const {
		std::lock_guard lock(mutex_);
		return PlanReadLocked(target, range);
	}
	bool CompleteRead(const ReadPlan& plan) {
		std::lock_guard lock(mutex_);
		if (!views_.contains(plan.target)) return false;
		// Validate the whole transaction before publishing any replica. A write
		// after planning must not make a stale transfer appear current.
		for (const auto& stamp: plan.required) {
			if (!Covers(plan.target, stamp.range) || !views_.contains(stamp.owner) || !Current(stamp)) return false;
		}
		PublishRead(plan);
		return true;
	}
	bool RecordWrite(OwnerId owner, Range range, WriteMode mode) {
		std::lock_guard lock(mutex_);
		if (!Covers(owner, range)) return false;
		if (mode == WriteMode::PreserveUnwritten) {
			const auto plan = PlanReadLocked(owner, range);
			if (!plan || !plan->copies.empty()) return false;
		}
		if (next_version_ == UINT64_MAX) return false;
		const uint64_t version = ++next_version_;
		Assign(backings_.at(range.backing).latest, range.begin, range.begin + range.size, Writer {owner, version});
		Assign(views_.at(owner).replicas[range.backing], range.begin, range.begin + range.size, version);
		return true;
	}
	bool ForgetView(OwnerId id) {
		std::lock_guard lock(mutex_);
		if (id == GuestBytes || !views_.contains(id)) return false;
		// Never evict the only current copy. Once guest bytes have been made
		// current, they can take over authority without a further data copy.
		for (const auto& [backing, state]: backings_) for (const auto& [begin, span]: state.latest) {
			if (span.value.owner == id && !Replica(GuestBytes, {backing,begin,span.end-begin}, span.value.version)) return false;
		}
		for (auto& [backing, state]: backings_) for (auto& [begin, span]: state.latest)
			if (span.value.owner == id) span.value.owner = GuestBytes;
		views_.erase(id);
		return true;
	}
	std::optional<std::vector<Stamp>> Latest(Range range) const {
		std::lock_guard lock(mutex_);
		if (!ValidRange(range)) return std::nullopt;
		return Stamps(range);
	}

private:
	void PublishRead(const ReadPlan& plan) {
		for (const auto& stamp: plan.required)
			Assign(views_.at(plan.target).replicas[stamp.range.backing], stamp.range.begin,
			       stamp.range.begin + stamp.range.size, stamp.version);
	}
	bool ValidRange(Range r) const {
		const auto it = backings_.find(r.backing);
		return it != backings_.end() && r.size != 0 && r.begin <= it->second.size && r.size <= it->second.size-r.begin;
	}
	bool Covers(OwnerId id, Range r) const {
		if (!ValidRange(r)) return false;
		const auto view = views_.find(id);
		if (view == views_.end()) return false;
		if (id == GuestBytes) return true;
		for (const auto& c: view->second.coverage)
			if (c.backing == r.backing && c.begin <= r.begin && r.begin-c.begin <= c.size && r.size <= c.size-(r.begin-c.begin)) return true;
		return false;
	}
	template<class T> static void Split(Intervals<T>& spans, uint64_t at) {
		auto it = spans.upper_bound(at);
		if (it == spans.begin()) return;
		--it;
		if (it->first < at && at < it->second.end) {
			const auto old = it->second;
			it->second.end = at;
			spans.emplace(at, old);
		}
	}
	template<class T> static void Assign(Intervals<T>& spans, uint64_t begin, uint64_t end, T value) {
		Split(spans, begin); Split(spans, end);
		spans.erase(spans.lower_bound(begin), spans.lower_bound(end));
		auto it = spans.emplace(begin, Span<T> {end, value}).first;
		if (it != spans.begin()) {
			auto prev = std::prev(it);
			if (prev->second.end == begin && prev->second.value == value) {
				prev->second.end = end; spans.erase(it); it = prev;
			}
		}
		auto next = std::next(it);
		if (next != spans.end() && it->second.end == next->first && next->second.value == value) {
			it->second.end = next->second.end; spans.erase(next);
		}
	}
	bool Replica(OwnerId owner, Range r, uint64_t version) const {
		const auto view = views_.find(owner);
		if (view == views_.end()) return false;
		const auto found = view->second.replicas.find(r.backing);
		if (found == view->second.replicas.end()) return false;
		const auto& spans = found->second;
		auto it = spans.upper_bound(r.begin);
		if (it == spans.begin()) return false;
		--it;
		uint64_t at = r.begin;
		while (at < r.begin+r.size) {
			if (it == spans.end() || it->first > at || it->second.end <= at || it->second.value != version) return false;
			at = std::min(it->second.end, r.begin+r.size); ++it;
		}
		return true;
	}
	std::vector<Stamp> Stamps(Range r) const {
		std::vector<Stamp> result;
		const auto& spans = backings_.at(r.backing).latest;
		auto it = std::prev(spans.upper_bound(r.begin));
		for (uint64_t at = r.begin; at < r.begin+r.size; ++it) {
			const uint64_t end = std::min(it->second.end, r.begin+r.size);
			result.push_back({{r.backing,at,end-at},it->second.value.owner,it->second.value.version});
			at = end;
		}
		return result;
	}
	bool Current(const Stamp& stamp) const {
		if (!ValidRange(stamp.range)) return false;
		for (const auto& current: Stamps(stamp.range)) if (current.version != stamp.version) return false;
		return true;
	}
	std::optional<ReadPlan> PlanReadLocked(OwnerId target, Range r) const {
		if (!Covers(target,r)) return std::nullopt;
		ReadPlan plan {target,Stamps(r),{}};
		for (const auto& stamp: plan.required)
			if (stamp.owner != target && !Replica(target,stamp.range,stamp.version)) plan.copies.push_back(stamp);
		return plan;
	}
	mutable std::mutex mutex_;
	std::map<BackingId, Backing> backings_;
	std::map<OwnerId, View> views_;
	std::set<OwnerId> used_ids_;
	uint64_t next_version_ = 0;
};

} // namespace Libs::Graphics::Coherence
