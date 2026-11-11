#pragma once
#include <bitset>
#include <cstdint>
#include <vector>
namespace Libs::Graphics::Metal {
// Like Vulkan's metadata clear mask: each attachment view consumes only its
// own pending layers. Binding cascade 1 must not erase rendered cascade 0.
class PendingDepthClears {
public:
	void                  MarkAll() { pending_.set(); }
	std::vector<uint32_t> Take(uint32_t first, uint32_t count) {
		std::vector<uint32_t> result;
		if (first >= pending_.size() || count > pending_.size() - first) return result;
		for (uint32_t layer = first; layer < first + count; ++layer)
			if (pending_.test(layer)) {
				result.push_back(layer);
				pending_.reset(layer);
			}
		return result;
	}

private:
	std::bitset<2048> pending_;
};
} // namespace Libs::Graphics::Metal
