#ifndef KYTY_COMMON_PKG_PKGKRAKEN_H_
#define KYTY_COMMON_PKG_PKGKRAKEN_H_

#include <cstdint>
#include <span>

namespace Common::Pkg {

enum class KrakenStatus {
	Ok,
	Malformed,
	UnsupportedEntropy,
};

struct KrakenResult {
	KrakenStatus status       = KrakenStatus::Ok;
	uint32_t     entropy_type = 0;
};

// Decodes a header-stripped PS5 NAPS/Kraken (Oodle newLZ) block into `dst`.
// A block is 1 or 2 chunks (up to 128 KiB each); NAPS provides `flags` and
// `first_chunk_comp` (0 when single chunk or uncompressed).
[[nodiscard]] KrakenResult DecodeKrakenBlock(std::span<const uint8_t> payload,
                                             uint32_t                 flags,
                                             uint32_t                 first_chunk_comp,
                                             std::span<uint8_t>       dst);

} // namespace Common::Pkg

#endif /* KYTY_COMMON_PKG_PKGKRAKEN_H_ */
