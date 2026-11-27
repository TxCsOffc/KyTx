#include "common/pkg/PkgUnpacker.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

void PrintHelp(const char* argv0) {
	::printf("KyTx FPKG / PKG Unpacker (PS4/PS5 CNT + PS5 FIH/NAPS/Kraken + PFS/PFSC)\n");
	::printf("Usage:\n");
	::printf("  %s --list <file.pkg> [--passcode <32chars>]\n", argv0);
	::printf("  %s <file.pkg> --output <dir> [--passcode <32chars>] [--no-sc0] [--no-pfs] "
	         "[--executables-only] [--only <path>]\n\n",
	         argv0);
	::printf("Options:\n");
	::printf("  --list, -l           Inspect PKG metadata and list contents without extracting.\n");
	::printf("  --output, -o <dir>   Output directory. Default: ./<content_id>\n");
	::printf("  --passcode, -p <str> 32-char FPKG passcode (default: 32 zeroes).\n");
	::printf("  --executables-only   Extract sce_sys/ + executables/modules only.\n");
	::printf("  --only <path>        Extract only PFS files under this path (repeatable).\n");
	::printf("  --no-sc0             Skip sce_sys/ metadata extraction.\n");
	::printf("  --no-pfs             Skip PFS/PFSC game data extraction.\n");
	::printf("  --help, -h           Show this help message.\n");
}

} // namespace

int main(int argc, char* argv[]) {
	std::filesystem::path       pkg_path;
	std::filesystem::path       output_dir;
	Common::Pkg::PkgUnpackOptions options;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			PrintHelp(argv[0]);
			return 0;
		}
		if (arg == "--list" || arg == "-l") {
			options.list_only = true;
		} else if (arg == "--no-sc0") {
			options.extract_sc0 = false;
		} else if (arg == "--no-pfs") {
			options.extract_pfs = false;
		} else if (arg == "--executables-only") {
			options.executables_only = true;
		} else if (arg == "--only" && i + 1 < argc) {
			options.only_prefixes.push_back(argv[++i]);
		} else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
			output_dir = argv[++i];
		} else if ((arg == "--passcode" || arg == "-p") && i + 1 < argc) {
			options.passcode = argv[++i];
		} else if (!arg.starts_with('-') && pkg_path.empty()) {
			pkg_path = arg;
		} else {
			::fprintf(stderr, "Unknown or incomplete argument: %s\n", arg.c_str());
			PrintHelp(argv[0]);
			return 1;
		}
	}

	if (pkg_path.empty()) {
		PrintHelp(argv[0]);
		return 1;
	}

	if (options.list_only) {
		const auto res = Common::Pkg::InspectPkg(pkg_path, options.passcode);
		if (!res.success) {
			::fprintf(stderr, "PKG inspect failed: %s\n", res.error.c_str());
			return 1;
		}
		if (res.info.is_ps5_fih) {
			::printf("Format:         PS5 FIH v%u\n", res.info.fih_format_version);
		} else {
			::printf("Format:         PS4/PS5 CNT\n");
		}
		::printf("Content-ID:     %s\n", res.info.content_id.c_str());
		::printf("PFS encrypted:  %s\n", res.info.pfs_encrypted ? "yes" : "no");
		::printf("Compressed:     %s\n", res.info.pfs_compressed ? "yes" : "no");
		::printf("Passcode valid: %s\n", res.info.passcode_valid ? "yes" : "no");
		if (res.info.is_ps5_fih) {
			const double mount_gib =
			    static_cast<double>(res.info.pfs_logical_size) / static_cast<double>(1ull << 30);
			::printf("Inner logical:  %.2f GiB (stored=%llu, kraken=%llu, sparse=%llu)\n",
			         mount_gib, static_cast<unsigned long long>(res.info.pfs_stored_blocks),
			         static_cast<unsigned long long>(res.info.pfs_kraken_blocks),
			         static_cast<unsigned long long>(res.info.pfs_sparse_blocks));
		}
		::printf("SC entries (%zu):\n", res.info.entries.size());
		for (const auto& e: res.info.entries) {
			::printf("  id=0x%04x size=%10u enc=%s %s\n", e.id, e.size,
			         e.encrypted ? "yes" : "no ", e.name.c_str());
		}
		uint64_t total_file_bytes = 0;
		::printf("PFS files (%zu):\n", res.info.pfs_file_entries.size());
		for (const auto& f: res.info.pfs_file_entries) {
			total_file_bytes += f.size;
			::printf("  %14llu  %s\n", static_cast<unsigned long long>(f.size), f.path.c_str());
		}
		::printf("Total PFS size: %.2f GiB (%llu bytes)\n",
		         static_cast<double>(total_file_bytes) / static_cast<double>(1ull << 30),
		         static_cast<unsigned long long>(total_file_bytes));
		return 0;
	}

	if (output_dir.empty()) {
		const auto inspected = Common::Pkg::InspectPkg(pkg_path, options.passcode);
		if (inspected.success && !inspected.info.content_id.empty()) {
			output_dir = inspected.info.content_id;
		} else {
			output_dir = pkg_path.stem();
		}
	}

	options.on_progress = [](std::string_view rel, uint64_t done, uint64_t total) {
		::printf("[FPKG] %s (%llu / %llu bytes)\n", std::string(rel).c_str(),
		         static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
	};

	const auto res = Common::Pkg::UnpackPkg(pkg_path, output_dir, options);
	if (!res.success) {
		::fprintf(stderr, "FPKG unpack failed: %s\n", res.error.c_str());
		return 1;
	}
	::printf("Unpacked %llu files (%llu bytes) from %s -> %s\n",
	         static_cast<unsigned long long>(res.files_extracted),
	         static_cast<unsigned long long>(res.bytes_extracted),
	         res.info.content_id.c_str(), output_dir.string().c_str());
	return 0;
}
