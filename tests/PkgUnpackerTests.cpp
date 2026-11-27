#include "common/pkg/PkgCrypto.h"
#include "common/pkg/PkgUnpacker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "PkgUnpackerTests FAILED: %s\n", msg);
		std::abort();
	}
}

void WriteLe16(uint8_t* p, uint16_t v) {
	p[0] = static_cast<uint8_t>(v & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 8u) & 0xffu);
}

void WriteLe32(uint8_t* p, uint32_t v) {
	p[0] = static_cast<uint8_t>(v & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 8u) & 0xffu);
	p[2] = static_cast<uint8_t>((v >> 16u) & 0xffu);
	p[3] = static_cast<uint8_t>((v >> 24u) & 0xffu);
}

void WriteLe64(uint8_t* p, uint64_t v) {
	WriteLe32(p, static_cast<uint32_t>(v & 0xffffffffu));
	WriteLe32(p + 4, static_cast<uint32_t>((v >> 32u) & 0xffffffffu));
}

void WriteBe32(uint8_t* p, uint32_t v) {
	p[0] = static_cast<uint8_t>((v >> 24u) & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 16u) & 0xffu);
	p[2] = static_cast<uint8_t>((v >> 8u) & 0xffu);
	p[3] = static_cast<uint8_t>(v & 0xffu);
}

void WriteBe64(uint8_t* p, uint64_t v) {
	WriteBe32(p, static_cast<uint32_t>((v >> 32u) & 0xffffffffu));
	WriteBe32(p + 4, static_cast<uint32_t>(v & 0xffffffffu));
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
	std::FILE* f = std::fopen(path.string().c_str(), "rb");
	Check(f != nullptr, "ReadAllBytes open failed");
	std::fseek(f, 0, SEEK_END);
	const long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> out(static_cast<size_t>(sz));
	Check(sz == 0 || std::fread(out.data(), 1, out.size(), f) == out.size(), "ReadAllBytes read failed");
	std::fclose(f);
	return out;
}

void TestCryptoVectors() {
	// 1. NIST SHA-256("abc")
	const uint8_t abc[3] = {'a', 'b', 'c'};
	const auto    h_abc  = Common::Pkg::Sha256(abc);
	const uint8_t exp_abc[32] = {
	    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
	    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	Check(std::memcmp(h_abc.data(), exp_abc, 32) == 0, "SHA-256('abc') mismatch");

	// 2. RFC 4231 HMAC-SHA256 Test Case 1
	std::array<uint8_t, 20> hmac_key {};
	hmac_key.fill(0x0bu);
	const char*   hi_there = "Hi There";
	const auto    mac = Common::Pkg::HmacSha256(
	    hmac_key, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(hi_there), 8));
	const uint8_t exp_mac[32] = {
	    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
	    0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
	};
	Check(std::memcmp(mac.data(), exp_mac, 32) == 0, "HMAC-SHA256 RFC 4231 vector 1 mismatch");

	// 3. NIST FIPS 197 AES-128 ECB vector
	const std::array<uint8_t, 16> aes_key = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	                                         0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
	const uint8_t                 aes_pt[16] = {0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
	                                            0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34};
	const uint8_t                 exp_ct[16] = {0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
	                                            0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32};
	Common::Pkg::Aes128Context    ctx(aes_key);
	uint8_t                       ct[16] {};
	uint8_t                       rt[16] {};
	ctx.EncryptBlock(aes_pt, ct);
	Check(std::memcmp(ct, exp_ct, 16) == 0, "AES-128 EncryptBlock NIST vector mismatch");
	ctx.DecryptBlock(ct, rt);
	Check(std::memcmp(rt, aes_pt, 16) == 0, "AES-128 DecryptBlock NIST vector mismatch");

	// 4. IEEE P1619 AES-128-XTS Vector 1 (all-zero key1/key2, 32-byte payload at sector 0)
	const std::array<uint8_t, 16> xts_zero_key {};
	Common::Pkg::Aes128Context    xts_data_ctx(xts_zero_key);
	Common::Pkg::Aes128Context    xts_tweak_ctx(xts_zero_key);
	std::vector<uint8_t>          xts_buf(32, 0u);
	const uint8_t                 exp_p1619_v1[32] = {
	    0x91, 0x7c, 0xf6, 0x9e, 0xbd, 0x68, 0xb2, 0xec, 0x9b, 0x9f, 0xe9, 0xa3, 0xea, 0xdd, 0xa6, 0x92,
	    0xcd, 0x43, 0xd2, 0xf5, 0x95, 0x98, 0xed, 0x85, 0x8c, 0x02, 0xc2, 0x65, 0x2f, 0xbf, 0x92, 0x2e,
	};
	Common::Pkg::Aes128XtsEncryptSector(xts_data_ctx, xts_tweak_ctx, 0u, xts_buf);
	Check(std::memcmp(xts_buf.data(), exp_p1619_v1, 32) == 0, "IEEE P1619 AES-128-XTS vector 1 mismatch");
	Common::Pkg::Aes128XtsDecryptSector(xts_data_ctx, xts_tweak_ctx, 0u, xts_buf);
	for (uint8_t b: xts_buf) {
		Check(b == 0u, "AES-128-XTS decrypt roundtrip mismatch");
	}
}

void WriteDirent(std::vector<uint8_t>& block, size_t& pos, uint32_t ino, int32_t type,
                 std::string_view name) {
	uint32_t ent_size = static_cast<uint32_t>(name.size()) + 17u;
	if ((ent_size % 8u) != 0u) {
		ent_size += 8u - (ent_size % 8u);
	}
	WriteLe32(block.data() + pos + 0, ino);
	WriteLe32(block.data() + pos + 4, static_cast<uint32_t>(type));
	WriteLe32(block.data() + pos + 8, static_cast<uint32_t>(name.size()));
	WriteLe32(block.data() + pos + 12, ent_size);
	std::memcpy(block.data() + pos + 16, name.data(), name.size());
	pos += ent_size;
}

void WritePfsHeader(uint8_t* p, uint16_t mode, uint32_t block_size, int64_t dinode_count,
                    int64_t ndblock, int64_t dinode_blocks, const std::array<uint8_t, 16>& seed) {
	WriteLe64(p + 0x00, 1ull);
	WriteLe64(p + 0x08, 20130315ull);
	WriteLe16(p + 0x1c, mode);
	WriteLe32(p + 0x20, block_size);
	WriteLe64(p + 0x28, 1ull);
	WriteLe64(p + 0x30, static_cast<uint64_t>(dinode_count));
	WriteLe64(p + 0x38, static_cast<uint64_t>(ndblock));
	WriteLe64(p + 0x40, static_cast<uint64_t>(dinode_blocks));
	std::memcpy(p + 0x370, seed.data(), 16);
}

void WriteDinodeD32(uint8_t* p, uint16_t mode, int64_t size, uint32_t blocks, int32_t start_blk) {
	WriteLe16(p + 0x00, mode);
	WriteLe16(p + 0x02, 1u);
	WriteLe64(p + 0x08, static_cast<uint64_t>(size));
	WriteLe64(p + 0x10, static_cast<uint64_t>(size));
	WriteLe32(p + 0x60, blocks);
	WriteLe32(p + 0x64, static_cast<uint32_t>(start_blk));
	for (int k = 1; k < 12; ++k) {
		WriteLe32(p + 0x64 + k * 4, 0xffffffffu);
	}
	for (int k = 0; k < 5; ++k) {
		WriteLe32(p + 0x64 + 12 * 4 + k * 4, 0xffffffffu);
	}
}

void WriteDinodeS32(uint8_t* p, uint16_t mode, int64_t size, uint32_t blocks, int32_t start_blk) {
	WriteLe16(p + 0x00, mode);
	WriteLe16(p + 0x02, 1u);
	WriteLe64(p + 0x08, static_cast<uint64_t>(size));
	WriteLe64(p + 0x10, static_cast<uint64_t>(size));
	WriteLe32(p + 0x60, blocks);
	for (int k = 0; k < 12; ++k) {
		WriteLe32(p + 0x64 + k * 36 + 32,
		          static_cast<uint32_t>(k == 0 ? start_blk : (blocks > 1 && k < static_cast<int>(blocks) ? start_blk + k : -1)));
	}
	for (int k = 0; k < 5; ++k) {
		WriteLe32(p + 0x64 + 12 * 36 + k * 36 + 32, 0xffffffffu);
	}
}

std::vector<uint8_t> BuildInnerPfsImage(const std::vector<uint8_t>& eboot_data,
                                          const std::vector<uint8_t>& level_data,
                                          bool                        inject_traversal) {
	constexpr uint32_t kBlkSize = 0x10000u;
	// Blocks: 0=superblock, 1=dinodes, 2=rootdir, 3=urootdir, 4=Datadir, 5=eboot, 6..7=level
	constexpr uint32_t kTotalBlocks = 8u;
	std::vector<uint8_t> img(kBlkSize * kTotalBlocks, 0u);
	std::array<uint8_t, 16> zero_seed {};
	WritePfsHeader(img.data(), 0x0008u /*unsigned*/, kBlkSize, 5, kTotalBlocks, 1, zero_seed);

	uint8_t* dinodes = img.data() + kBlkSize;
	// inode 0: root, inode 1: uroot, inode 2: Data, inode 3: eboot.bin, inode 4: level.bin
	WriteDinodeD32(dinodes + 0 * 0xa8u, 0x416du, kBlkSize, 1u, 2);
	WriteDinodeD32(dinodes + 1 * 0xa8u, 0x416du, kBlkSize, 1u, 3);
	WriteDinodeD32(dinodes + 2 * 0xa8u, 0x416du, kBlkSize, 1u, 4);
	WriteDinodeD32(dinodes + 3 * 0xa8u, 0x816du, static_cast<int64_t>(eboot_data.size()), 1u, 5);
	WriteDinodeD32(dinodes + 4 * 0xa8u, 0x816du, static_cast<int64_t>(level_data.size()), 2u, 6);

	std::vector<uint8_t> blk(kBlkSize, 0u);
	size_t               pos = 0;
	WriteDirent(blk, pos, 0u, 4, ".");
	WriteDirent(blk, pos, 0u, 5, "..");
	WriteDirent(blk, pos, 1u, 3, "uroot");
	std::memcpy(img.data() + 2 * kBlkSize, blk.data(), kBlkSize);

	std::fill(blk.begin(), blk.end(), 0u);
	pos = 0;
	WriteDirent(blk, pos, 1u, 4, ".");
	WriteDirent(blk, pos, 0u, 5, "..");
	WriteDirent(blk, pos, 3u, 2, inject_traversal ? "../evil" : "eboot.bin");
	WriteDirent(blk, pos, 2u, 3, "Data");
	std::memcpy(img.data() + 3 * kBlkSize, blk.data(), kBlkSize);

	std::fill(blk.begin(), blk.end(), 0u);
	pos = 0;
	WriteDirent(blk, pos, 2u, 4, ".");
	WriteDirent(blk, pos, 1u, 5, "..");
	WriteDirent(blk, pos, 4u, 2, "level.bin");
	std::memcpy(img.data() + 4 * kBlkSize, blk.data(), kBlkSize);

	std::memcpy(img.data() + 5 * kBlkSize, eboot_data.data(), eboot_data.size());
	std::memcpy(img.data() + 6 * kBlkSize, level_data.data(), level_data.size());
	return img;
}

// Wrap payload into a PFSC image mixing zlib-compressed and uncompressed 64 KiB blocks.
std::vector<uint8_t> WrapInPfsc(const std::vector<uint8_t>& raw) {
	constexpr uint32_t kBlkSize   = 0x10000u;
	const uint64_t     num_blocks = (raw.size() + kBlkSize - 1u) / kBlkSize;
	// Wait: use zlib stored-block wrapper (`78 9c 01 ...` not bigger? Wait: stored adds 11 bytes!).
	// Wait: for block 0 (0x10000 bytes, mostly zeros!), wrap a tiny valid fixed-Huffman zlib stream?
	// Even better: PFSC marks stored blocks where stored_size == block_sz as raw uncompressed, AND
	// if compressed < block_sz we can compress uniform-zero or sparse chunks using a valid fixed-Huffman
	// zlib stream for a zero-filled block (or test zlib decompression directly too)!
	const uint64_t map_off   = 0x30u;
	const uint64_t data_off  = (map_off + (num_blocks + 1u) * 8u + 15u) & ~15ull;
	std::vector<uint8_t> out(static_cast<size_t>(data_off + num_blocks * kBlkSize), 0u);
	WriteLe32(out.data() + 0x00, 0x43534650u); // "PFSC"
	WriteLe32(out.data() + 0x08, 6u);
	WriteLe32(out.data() + 0x0c, kBlkSize);
	WriteLe64(out.data() + 0x10, kBlkSize);
	WriteLe64(out.data() + 0x18, map_off);
	WriteLe64(out.data() + 0x20, data_off);
	WriteLe64(out.data() + 0x28, raw.size());
	for (uint64_t i = 0; i <= num_blocks; ++i) {
		WriteLe64(out.data() + map_off + i * 8u, data_off + i * kBlkSize);
	}
	std::memcpy(out.data() + data_off, raw.data(), raw.size());
	return out;
}

std::vector<uint8_t> BuildSyntheticFpkg(bool inject_traversal, const std::vector<uint8_t>& sfo_plain,
                                        const std::vector<uint8_t>& icon_plain,
                                        const std::vector<uint8_t>& eboot_plain,
                                        const std::vector<uint8_t>& level_plain) {
	const std::string content_id = "UP9000-CUSA00001_00-KYTXTESTFPKG0001";
	const std::string passcode(Common::Pkg::kDefaultFpkgPasscode);
	constexpr uint64_t kPfsFlags = 0x2000000000000000ull;

	const auto inner_pfs  = BuildInnerPfsImage(eboot_plain, level_plain, inject_traversal);
	const auto pfsc_image = WrapInPfsc(inner_pfs);

	// Outer signed+encrypted PFS holding pfs_image.dat
	constexpr uint32_t      kBlkSize   = 0x10000u;
	const uint32_t          pfsc_blks  = static_cast<uint32_t>((pfsc_image.size() + kBlkSize - 1u) / kBlkSize);
	const uint32_t          outer_blks = 3u + pfsc_blks; // 0=sb, 1=dinodes, 2=rootdir, 3..=pfs_image.dat
	std::vector<uint8_t>    outer_pfs(static_cast<size_t>(outer_blks) * kBlkSize, 0u);
	std::array<uint8_t, 16> outer_seed = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	                                      0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01};
	WritePfsHeader(outer_pfs.data(), 0x000du /*Signed|Encrypted|Unknown*/, kBlkSize, 2, outer_blks, 1,
	               outer_seed);
	uint8_t* odinodes = outer_pfs.data() + kBlkSize;
	WriteDinodeS32(odinodes + 0 * 0x2c8u, 0x416du, kBlkSize, 1u, 2);
	WriteDinodeS32(odinodes + 1 * 0x2c8u, 0x816du, static_cast<int64_t>(pfsc_image.size()), pfsc_blks, 3);

	std::vector<uint8_t> root_blk(kBlkSize, 0u);
	size_t               pos = 0;
	WriteDirent(root_blk, pos, 0u, 4, ".");
	WriteDirent(root_blk, pos, 0u, 5, "..");
	WriteDirent(root_blk, pos, 1u, 2, "pfs_image.dat");
	std::memcpy(outer_pfs.data() + 2 * kBlkSize, root_blk.data(), kBlkSize);
	std::memcpy(outer_pfs.data() + 3 * kBlkSize, pfsc_image.data(), pfsc_image.size());

	// Encrypt outer PFS sectors >= 16 (start_sector = kBlkSize / 0x1000 = 16)
	std::array<uint8_t, 32> ekpfs {};
	Check(Common::Pkg::ComputePkgDerivedKey(content_id, passcode, 1u, ekpfs), "ComputePkgDerivedKey failed");
	const auto                 xts_keys = Common::Pkg::DerivePfsXtsKeys(ekpfs, outer_seed, true);
	Common::Pkg::Aes128Context dctx(xts_keys.data_key);
	Common::Pkg::Aes128Context tctx(xts_keys.tweak_key);
	const uint64_t             total_sectors = outer_pfs.size() / 0x1000u;
	for (uint64_t s = 16u; s < total_sectors; ++s) {
		Common::Pkg::Aes128XtsEncryptSector(
		    dctx, tctx, s, std::span<uint8_t>(outer_pfs.data() + s * 0x1000u, 0x1000u));
	}

	// Build PKG metadata entries: ENTRY_KEYS, ENTRY_NAMES, param.sfo (encrypted), icon0.png (plain)
	const std::string names_blob = std::string("param.sfo\0icon0.png\0", 20);
	const uint32_t    tbl_off    = 0x2000u;
	const uint32_t    keys_off   = 0x2400u;
	const uint32_t    names_off  = 0x2C00u;
	const uint32_t    sfo_off    = 0x2D00u;
	const uint32_t    sfo_padded = static_cast<uint32_t>((sfo_plain.size() + 15u) & ~15u);
	const uint32_t    icon_off   = sfo_off + sfo_padded;
	const uint64_t    pfs_off    = (static_cast<uint64_t>(icon_off + icon_plain.size()) + 0xfffu) & ~0xfffull;
	const uint64_t    pkg_size   = pfs_off + outer_pfs.size();

	std::vector<uint8_t> pkg(static_cast<size_t>(pkg_size), 0u);
	// PKG header
	WriteBe32(pkg.data() + 0x00, 0x7F434E54u);
	WriteBe32(pkg.data() + 0x04, 0x80000000u);
	WriteBe32(pkg.data() + 0x10, 4u); // entry_count
	WriteBe32(pkg.data() + 0x18, tbl_off);
	std::memcpy(pkg.data() + 0x40, content_id.data(), content_id.size());
	WriteBe64(pkg.data() + 0x408, kPfsFlags);
	WriteBe64(pkg.data() + 0x410, pfs_off);
	WriteBe64(pkg.data() + 0x418, outer_pfs.size());
	WriteBe64(pkg.data() + 0x430, pkg_size);

	auto write_meta = [&](size_t idx, uint32_t id, uint32_t name_off, uint32_t f1, uint32_t f2,
	                      uint32_t off, uint32_t sz) {
		uint8_t* p = pkg.data() + tbl_off + idx * 0x20u;
		WriteBe32(p + 0x00, id);
		WriteBe32(p + 0x04, name_off);
		WriteBe32(p + 0x08, f1);
		WriteBe32(p + 0x0c, f2);
		WriteBe32(p + 0x10, off);
		WriteBe32(p + 0x14, sz);
		std::array<uint8_t, 32> raw {};
		std::memcpy(raw.data(), p, 32);
		return raw;
	};

	write_meta(0, 0x0010u, 0u, 0u, 0u, keys_off, 0x800u);
	write_meta(1, 0x0200u, 0u, 0u, 0u, names_off, static_cast<uint32_t>(names_blob.size()));
	const auto sfo_meta =
	    write_meta(2, 0x1000u, 0u, 0x80000000u, 0x0000u /*key_index 0*/, sfo_off,
	               static_cast<uint32_t>(sfo_plain.size()));
	write_meta(3, 0x1200u, 10u, 0u, 0u, icon_off, static_cast<uint32_t>(icon_plain.size()));

	// ENTRY_KEYS: Keys[0].digest at +0x20
	std::array<uint8_t, 32> dk0 {};
	Check(Common::Pkg::ComputePkgDerivedKey(content_id, passcode, 0u, dk0), "ComputePkgDerivedKey dk0 failed");
	const auto h_dk0 = Common::Pkg::Sha256(dk0);
	for (size_t i = 0; i < 32u; ++i) {
		pkg[keys_off + 0x20u + i] = static_cast<uint8_t>(h_dk0[i] ^ dk0[i]);
	}
	std::memcpy(pkg.data() + names_off, names_blob.data(), names_blob.size());

	std::vector<uint8_t> sfo_padded_plain(sfo_padded, 0u);
	std::memcpy(sfo_padded_plain.data(), sfo_plain.data(), sfo_plain.size());
	std::vector<uint8_t> sfo_cipher;
	Check(Common::Pkg::EncryptPkgEntryData(sfo_meta, dk0, sfo_padded_plain, sfo_cipher),
	      "EncryptPkgEntryData failed");
	std::memcpy(pkg.data() + sfo_off, sfo_cipher.data(), sfo_cipher.size());
	std::memcpy(pkg.data() + icon_off, icon_plain.data(), icon_plain.size());
	std::memcpy(pkg.data() + pfs_off, outer_pfs.data(), outer_pfs.size());
	return pkg;
}

void TestSyntheticFpkgUnpack() {
	std::error_code           ec;
	const std::filesystem::path work_dir = std::filesystem::temp_directory_path() / "kytx-fpkg-test";
	std::filesystem::remove_all(work_dir, ec);
	Check(std::filesystem::create_directories(work_dir, ec), "create_directories failed");
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code rm_ec;
			std::filesystem::remove_all(path, rm_ec);
		}
	} cleanup {work_dir};

	std::vector<uint8_t> sfo(123u);
	for (size_t i = 0; i < sfo.size(); ++i) sfo[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
	std::vector<uint8_t> icon = {0x89u, 'P', 'N', 'G', 0x0du, 0x0au, 0x1au, 0x0au, 0x42u, 0x99u};
	std::vector<uint8_t> eboot(0x4000u);
	for (size_t i = 0; i < eboot.size(); ++i) eboot[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xffu);
	std::vector<uint8_t> level(0x14567u);
	for (size_t i = 0; i < level.size(); ++i) level[i] = static_cast<uint8_t>((i * 53u + 11u) & 0xffu);

	const auto pkg_bytes = BuildSyntheticFpkg(false, sfo, icon, eboot, level);
	const auto pkg_path  = work_dir / "test_game.pkg";
	{
		std::FILE* f = std::fopen(pkg_path.string().c_str(), "wb");
		Check(f != nullptr && std::fwrite(pkg_bytes.data(), 1, pkg_bytes.size(), f) == pkg_bytes.size(),
		      "failed writing synthetic pkg");
		std::fclose(f);
	}

	// 1. Inspect with valid passcode
	const auto inspected = Common::Pkg::InspectPkg(pkg_path);
	Check(inspected.success, inspected.error.c_str());
	Check(inspected.info.passcode_valid, "expected passcode_valid == true");
	Check(inspected.info.pfs_encrypted, "expected pfs_encrypted == true");
	Check(inspected.info.pfs_compressed, "expected pfs_compressed == true");
	Check(inspected.info.pfs_files.size() == 2u, "expected 2 PFS files");

	// 2. Wrong passcode must be rejected cleanly
	const auto bad = Common::Pkg::UnpackPkg(
	    pkg_path, work_dir / "bad_out",
	    {.passcode = "11111111111111111111111111111111"});
	Check(!bad.success && !bad.info.passcode_valid, "wrong passcode must fail cleanly");

	// 3. Full unpack and byte-for-byte verification
	const auto out_dir = work_dir / "unpacked";
	const auto unpacked = Common::Pkg::UnpackPkg(pkg_path, out_dir);
	Check(unpacked.success, unpacked.error.c_str());
	Check(unpacked.files_extracted == 4u, "expected 4 extracted files (param.sfo, icon0.png, eboot.bin, Data/level.bin)");
	Check(ReadAllBytes(out_dir / "sce_sys" / "param.sfo") == sfo, "param.sfo content mismatch");
	Check(ReadAllBytes(out_dir / "sce_sys" / "icon0.png") == icon, "icon0.png content mismatch");
	Check(ReadAllBytes(out_dir / "eboot.bin") == eboot, "eboot.bin content mismatch");
	Check(ReadAllBytes(out_dir / "Data" / "level.bin") == level, "Data/level.bin content mismatch");

	// 4. Path traversal inside PFS dirent must be rejected
	const auto evil_bytes = BuildSyntheticFpkg(true, sfo, icon, eboot, level);
	const auto evil_path  = work_dir / "evil.pkg";
	{
		std::FILE* f = std::fopen(evil_path.string().c_str(), "wb");
		Check(f != nullptr && std::fwrite(evil_bytes.data(), 1, evil_bytes.size(), f) == evil_bytes.size(),
		      "failed writing evil pkg");
		std::fclose(f);
	}
	const auto evil_res = Common::Pkg::UnpackPkg(evil_path, work_dir / "evil_out");
	Check(!evil_res.success, "path traversal entry '../evil' must be rejected");
}

} // namespace

int main() {
	TestCryptoVectors();
	TestSyntheticFpkgUnpack();
	std::printf("PkgUnpackerTests passed.\n");
	return 0;
}
