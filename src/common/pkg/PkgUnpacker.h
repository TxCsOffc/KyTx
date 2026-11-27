#ifndef KYTY_COMMON_PKG_PKGUNPACKER_H_
#define KYTY_COMMON_PKG_PKGUNPACKER_H_

#include "common/pkg/PkgCrypto.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Pkg {

struct PkgEntrySummary {
	uint32_t    id        = 0;
	uint32_t    flags1    = 0;
	uint32_t    flags2    = 0;
	uint32_t    offset    = 0;
	uint32_t    size      = 0;
	std::string name;
	bool        encrypted = false;
};

struct PkgInfo {
	std::string                  content_id;
	uint32_t                     flags            = 0;
	uint32_t                     drm_type         = 0;
	uint32_t                     content_type     = 0;
	uint32_t                     content_flags    = 0;
	uint64_t                     pfs_flags        = 0;
	uint64_t                     pfs_image_offset = 0;
	uint64_t                     pfs_image_size   = 0;
	uint64_t                     package_size     = 0;
	bool                         is_fpkg          = false;
	bool                         pfs_encrypted    = false;
	bool                         pfs_compressed   = false;
	bool                         passcode_valid   = false;
	std::vector<PkgEntrySummary> entries;
	std::vector<std::string>     pfs_files;
};

struct PkgUnpackOptions {
	std::string passcode    = std::string(kDefaultFpkgPasscode);
	bool        extract_sc0 = true;
	bool        extract_pfs = true;
	bool        list_only   = false;
	std::function<void(std::string_view relative_path, uint64_t bytes_written, uint64_t total_bytes)>
	    on_progress;
};

struct PkgUnpackResult {
	bool        success          = false;
	std::string error;
	PkgInfo     info;
	uint64_t    files_extracted  = 0;
	uint64_t    bytes_extracted  = 0;
};

[[nodiscard]] PkgUnpackResult InspectPkg(const std::filesystem::path& pkg_path,
                                         std::string_view passcode = kDefaultFpkgPasscode);

[[nodiscard]] PkgUnpackResult UnpackPkg(const std::filesystem::path& pkg_path,
                                        const std::filesystem::path& output_dir,
                                        const PkgUnpackOptions&      options = {});

} // namespace Common::Pkg

#endif /* KYTY_COMMON_PKG_PKGUNPACKER_H_ */
