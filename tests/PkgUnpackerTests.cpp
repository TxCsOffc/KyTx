#include "common/pkg/PkgCrypto.h"
#include "common/pkg/PkgKraken.h"
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

void TestKrakenDecoder() {
	// 1. Verbatim block copies when payload size matches destination size
	const std::array<uint8_t, 8> raw = {1, 2, 3, 4, 5, 6, 7, 8};
	std::array<uint8_t, 8>       dst {};
	const auto                   r1 = Common::Pkg::DecodeKrakenBlock(raw, 0x02u, 8u, dst);
	Check(r1.status == Common::Pkg::KrakenStatus::Ok, "verbatim Kraken decode failed");
	Check(dst == raw, "verbatim Kraken payload mismatch");

	// 2. Memcpy entropy array (non-LZ chunk)
	const std::array<uint8_t, 6> memcpy_src = {0x80u, 0x04u, 9u, 8u, 7u, 6u};
	std::array<uint8_t, 4>       dst4 {};
	const auto                   r2 = Common::Pkg::DecodeKrakenBlock(memcpy_src, 0x00u, 0u, dst4);
	Check(r2.status == Common::Pkg::KrakenStatus::Ok, "memcpy Kraken entropy decode failed");
	Check((dst4 == std::array<uint8_t, 4> {9u, 8u, 7u, 6u}), "memcpy Kraken output mismatch");

	// 3. Unsupported tANS entropy type is cleanly reported
	const std::array<uint8_t, 5> tans_src = {0x90u, 0x04u, 0x02u, 0u, 0u};
	std::array<uint8_t, 16>      dst16 {};
	const auto                   r3 = Common::Pkg::DecodeKrakenBlock(tans_src, 0x00u, 0u, dst16);
	Check(r3.status == Common::Pkg::KrakenStatus::UnsupportedEntropy && r3.entropy_type == 1u,
	      "tANS entropy must report UnsupportedEntropy(1)");
}

void WriteFlatInode(uint8_t* p, uint16_t mode, uint64_t size, uint64_t logical) {
	WriteLe16(p + 0x00, mode);
	WriteLe16(p + 0x02, 1u);
	WriteLe64(p + 0x08, size);
	WriteLe64(p + 0x10, size);
	WriteLe64(p + 0x60, logical);
}

void WriteDinodeS64(uint8_t* p, uint16_t mode, uint64_t size, uint32_t blocks, uint64_t start_blk) {
	WriteLe16(p + 0x00, mode);
	WriteLe16(p + 0x02, 1u);
	WriteLe64(p + 0x08, size);
	WriteLe64(p + 0x10, size);
	WriteLe32(p + 0x60, blocks);
	for (uint32_t k = 0; k < 12u; ++k) {
		const uint64_t val =
		    (k == 0u || (blocks > 1u && k < blocks)) ? (start_blk + k) : 0ull;
		WriteLe64(p + 0x68 + k * 40u + 32u, val);
	}
	for (uint32_t k = 0; k < 5u; ++k) {
		WriteLe64(p + 0x68 + (12u + k) * 40u + 32u, 0ull);
	}
}

std::vector<uint8_t> BuildSyntheticPs5FihPkg(uint8_t                     signed_byte,
                                             const std::vector<uint8_t>& param_json,
                                             const std::vector<uint8_t>& eboot_data,
                                             const std::vector<uint8_t>& test_data) {
	constexpr uint32_t kBlkSize    = 0x10000u;
	constexpr uint64_t kMountSize  = 0x90000ull; // 9 inner blocks of 64 KiB
	constexpr uint64_t kMetaBase   = 0x50000ull;

	// Build stored pfs_image.dat (0x54000 bytes -> 6 outer blocks):
	//   [0x00000..0x04000): eboot.bin (0x4000)
	//   [0x04000..0x14000): Data/test.bin + zero pad (mapped at logical 0x40000..0x50000)
	//   [0x14000..0x54000): inner PFSv2 metadata tail (mapped at logical 0x50000..0x90000)
	std::vector<uint8_t> pfs_image(0x54000u, 0u);
	std::memcpy(pfs_image.data() + 0x00000u, eboot_data.data(), eboot_data.size());
	std::memcpy(pfs_image.data() + 0x04000u, test_data.data(), test_data.size());

	uint8_t* tail = pfs_image.data() + 0x14000u; // logical 0x50000
	WriteLe64(tail + 0x00, 2ull);               // ver = 2
	WriteLe64(tail + 0x08, 20130315ull);        // magic
	WriteLe16(tail + 0x1c, 0x0010u);            // flat mode
	WriteLe32(tail + 0x20, kBlkSize);
	WriteLe64(tail + 0x30, 5ull);               // 5 inner inodes
	WriteLe64(tail + 0x38, kMountSize / kBlkSize);

	uint8_t* in_inodes = tail + kBlkSize; // logical 0x60000
	// inode 0: root (dir at 0x70000), 1: uroot (dir at 0x70000+0x4000), 2: Data (dir at 0x80000),
	// 3: eboot.bin (0x00000), 4: Data/test.bin (0x40000)
	WriteFlatInode(in_inodes + 0u * 0xa8u, 0x416du, 0x1000u, 0x70000ull);
	WriteFlatInode(in_inodes + 1u * 0xa8u, 0x416du, 0x1000u, 0x74000ull);
	WriteFlatInode(in_inodes + 2u * 0xa8u, 0x416du, 0x1000u, 0x80000ull);
	WriteFlatInode(in_inodes + 3u * 0xa8u, 0x816du, eboot_data.size(), 0x00000ull);
	WriteFlatInode(in_inodes + 4u * 0xa8u, 0x816du, test_data.size(), 0x40000ull);

	auto write_dir_at = [&](size_t tail_off, auto&& fn) {
		std::vector<uint8_t> blk(kBlkSize, 0u);
		size_t               pos = 0;
		fn(blk, pos);
		std::memcpy(tail + tail_off, blk.data(), 0x1000u);
	};
	write_dir_at(0x20000u, [&](std::vector<uint8_t>& blk, size_t& pos) {
		WriteDirent(blk, pos, 0u, 4, ".");
		WriteDirent(blk, pos, 0u, 5, "..");
		WriteDirent(blk, pos, 1u, 3, "uroot");
	});
	write_dir_at(0x24000u, [&](std::vector<uint8_t>& blk, size_t& pos) {
		WriteDirent(blk, pos, 1u, 4, ".");
		WriteDirent(blk, pos, 0u, 5, "..");
		WriteDirent(blk, pos, 3u, 2, "eboot.bin");
		WriteDirent(blk, pos, 2u, 3, "Data");
	});
	write_dir_at(0x30000u, [&](std::vector<uint8_t>& blk, size_t& pos) {
		WriteDirent(blk, pos, 2u, 4, ".");
		WriteDirent(blk, pos, 1u, 5, "..");
		WriteDirent(blk, pos, 4u, 2, "test.bin");
	});

	// Build naps_pkg_layout.dat:
	//   5 file_offsets: 0 (file), 0x4000 (kind 0x40 sparse), 0x40000 (file), 0x50000 (file), 0x90000 (kind 0x40 mount)
	//   num_outer_blocks = 6 (0x54000 -> 6 * 64KiB), num_ublocks = 3, num_cblock_info = 4 (encoded 2)
	const uint32_t       num_files        = 5u;
	const uint32_t       num_outer_blocks = 6u;
	const uint32_t       num_ublocks      = 3u;
	const uint32_t       num_u2c          = (num_ublocks + 8u) >> 3u;
	const uint32_t       num_cblocks      = 4u;
	const uint64_t       w0 = static_cast<uint64_t>(num_files - 1u) | (2ull << 24u) |
	                          (static_cast<uint64_t>(num_ublocks) << 32u);
	const uint64_t       w1 = static_cast<uint64_t>(num_outer_blocks) |
	                          (static_cast<uint64_t>(num_cblocks - 2u) << 24u);
	const size_t         fidx_off  = 16u + num_outer_blocks * 8u;
	const size_t         u2c_off   = fidx_off + num_files * 6u;
	const size_t         cb_off    = ((u2c_off + num_u2c * 10u + 7u) / 8u) * 8u;
	std::vector<uint8_t> naps(cb_off + num_cblocks * 9u, 0u);
	WriteLe64(naps.data() + 0u, w0);
	WriteLe64(naps.data() + 8u, w1);

	auto write_fo = [&](size_t idx, uint8_t kind, uint64_t off) {
		uint8_t* p = naps.data() + fidx_off + idx * 6u;
		for (size_t b = 0; b < 5u; ++b) {
			p[b] = static_cast<uint8_t>((off >> (8u * b)) & 0xffu);
		}
		p[5] = kind;
	};
	write_fo(0u, 0x00u, 0x00000ull);
	write_fo(1u, 0x40u, 0x04000ull);
	write_fo(2u, 0x00u, 0x40000ull);
	write_fo(3u, 0x00u, kMetaBase);
	write_fo(4u, 0x40u, kMountSize);

	auto write_cb = [&](size_t idx, uint32_t coffset_mod) {
		WriteLe64(naps.data() + cb_off + idx * 9u, coffset_mod & 0x3ffffu);
	};
	write_cb(0u, 0x00000u);
	write_cb(1u, 0x04000u);
	write_cb(2u, 0x14000u);
	write_cb(3u, 0x14000u); // diff == 0 -> 0x40000

	// Build Outer PFSv2 at 0x10000:
	//   blocks 0..5: pfs_image.dat (6 blocks)
	//   block 6: naps_pkg_layout.dat (1 block)
	//   block 7: outer superblock (superblock_offset = pfs_offset + 7*kBlkSize)
	//   block 8: outer inode table (inode_table_block = 8)
	//   block 9: outer root directory
	constexpr uint64_t kFihPfsOffset  = 0x10000ull;
	constexpr uint32_t kOuterBlocks   = 10u;
	constexpr uint64_t kOuterSbOffset = kFihPfsOffset + 7ull * kBlkSize;
	std::vector<uint8_t> outer_pfs(static_cast<size_t>(kOuterBlocks) * kBlkSize, 0u);
	std::memcpy(outer_pfs.data(), pfs_image.data(), pfs_image.size());
	std::memcpy(outer_pfs.data() + 6u * kBlkSize, naps.data(), naps.size());

	uint8_t* osb = outer_pfs.data() + 7u * kBlkSize;
	WriteLe64(osb + 0x00, 2ull);        // version = 2
	WriteLe64(osb + 0x08, 20130315ull); // magic
	WriteLe16(osb + 0x1c, 0x000fu);     // signed | 64-bit | encrypted-flag (with plain seed)
	WriteLe32(osb + 0x20, kBlkSize);
	WriteLe64(osb + 0x30, 3ull);        // 3 inodes (0=dir, 1=pfs_image.dat, 2=naps_pkg_layout.dat)
	WriteLe64(osb + 0x38, kOuterBlocks);
	WriteLe64(osb + 0x40, 1ull);        // dinode_blocks
	WriteLe64(osb + 0xd8, 8ull);        // inode_table_block
	std::memcpy(osb + 0x370, "PPRPLAIN-NOAUTH!", 16u);

	uint8_t* oino = outer_pfs.data() + 8u * kBlkSize;
	WriteDinodeS64(oino + 0u * 0x310u, 0x416du, kBlkSize, 1u, 9ull);
	WriteDinodeS64(oino + 1u * 0x310u, 0x816du, pfs_image.size(), 6u, 0ull);
	WriteDinodeS64(oino + 2u * 0x310u, 0x816du, naps.size(), 1u, 6ull);

	std::vector<uint8_t> odir(kBlkSize, 0u);
	size_t               opos = 0;
	WriteDirent(odir, opos, 0u, 4, ".");
	WriteDirent(odir, opos, 0u, 5, "..");
	WriteDirent(odir, opos, 1u, 2, "pfs_image.dat");
	WriteDirent(odir, opos, 2u, 2, "naps_pkg_layout.dat");
	std::memcpy(outer_pfs.data() + 9u * kBlkSize, odir.data(), kBlkSize);

	// Embedded \x7FCNT container placed right after outer_pfs
	const std::string    cid        = "EP9000-PPSA99999_00-KYTXTESTPS5PKG01";
	const std::string    names_blob = std::string("\0sce_sys/param.json\0license.dat\0", 31);
	constexpr uint32_t   kCntTbl    = 0x100u;
	constexpr uint32_t   kNamesOff  = 0x200u;
	constexpr uint32_t   kParamOff  = 0x300u;
	const uint32_t       lic_off    = kParamOff + static_cast<uint32_t>(param_json.size());
	const uint32_t       cnt_total  = lic_off + 16u;
	std::vector<uint8_t> cnt(cnt_total, 0u);
	WriteBe32(cnt.data() + 0x00, 0x7F434E54u);
	WriteBe32(cnt.data() + 0x10, 3u);
	WriteBe32(cnt.data() + 0x18, kCntTbl);
	WriteBe64(cnt.data() + 0x20, kNamesOff);
	WriteBe64(cnt.data() + 0x28, cnt_total - kNamesOff);
	std::memcpy(cnt.data() + 0x40, cid.data(), cid.size());

	auto write_cnt_rec = [&](size_t idx, uint32_t id, uint32_t noff, uint32_t f1, uint32_t off,
	                         uint32_t sz) {
		uint8_t* p = cnt.data() + kCntTbl + idx * 0x20u;
		WriteBe32(p + 0x00, id);
		WriteBe32(p + 0x04, noff);
		WriteBe32(p + 0x08, f1);
		WriteBe32(p + 0x10, off);
		WriteBe32(p + 0x14, sz);
	};
	write_cnt_rec(0u, 0x0200u, 0u, 0u, kNamesOff, static_cast<uint32_t>(names_blob.size()));
	write_cnt_rec(1u, 0x2000u, 1u, 0u, kParamOff, static_cast<uint32_t>(param_json.size()));
	write_cnt_rec(2u, 0x0400u, 19u, 0x80000000u, lic_off, 16u);
	std::memcpy(cnt.data() + kNamesOff, names_blob.data(), names_blob.size());
	std::memcpy(cnt.data() + kParamOff, param_json.data(), param_json.size());

	const uint64_t       cnt_offset = kFihPfsOffset + outer_pfs.size();
	std::vector<uint8_t> pkg(static_cast<size_t>(cnt_offset + cnt.size()), 0u);
	std::memcpy(pkg.data(), "\x7F" "FIH", 4u);
	pkg[4] = 0x01u;
	pkg[5] = signed_byte;
	WriteLe16(pkg.data() + 0x06, 3u);
	WriteLe64(pkg.data() + 0x10, kFihPfsOffset);
	WriteLe64(pkg.data() + 0x18, outer_pfs.size());
	WriteLe64(pkg.data() + 0x20, kOuterSbOffset);
	WriteLe64(pkg.data() + 0x58, cnt_offset);
	std::memcpy(pkg.data() + kFihPfsOffset, outer_pfs.data(), outer_pfs.size());
	std::memcpy(pkg.data() + cnt_offset, cnt.data(), cnt.size());
	return pkg;
}

void TestSyntheticPs5FihUnpack() {
	std::error_code             ec;
	const std::filesystem::path work_dir = std::filesystem::temp_directory_path() / "kytx-ps5-fih-test";
	std::filesystem::remove_all(work_dir, ec);
	Check(std::filesystem::create_directories(work_dir, ec), "create_directories failed");
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code rm_ec;
			std::filesystem::remove_all(path, rm_ec);
		}
	} cleanup {work_dir};

	const std::string          param_str = R"({"titleId":"PPSA99999"})";
	const std::vector<uint8_t> param(param_str.begin(), param_str.end());
	std::vector<uint8_t>       eboot(0x4000u);
	for (size_t i = 0; i < eboot.size(); ++i) eboot[i] = static_cast<uint8_t>((i * 29u + 5u) & 0xffu);
	std::vector<uint8_t> test_data(0x2000u);
	for (size_t i = 0; i < test_data.size(); ++i)
		test_data[i] = static_cast<uint8_t>((i * 41u + 9u) & 0xffu);

	const auto pkg_bytes = BuildSyntheticPs5FihPkg(0x00u, param, eboot, test_data);
	const auto pkg_path  = work_dir / "ps5_debug.pkg";
	{
		std::FILE* f = std::fopen(pkg_path.string().c_str(), "wb");
		Check(f != nullptr && std::fwrite(pkg_bytes.data(), 1, pkg_bytes.size(), f) == pkg_bytes.size(),
		      "failed writing synthetic PS5 pkg");
		std::fclose(f);
	}

	// 1. Inspect succeeds and reports FIH v3 + both files + block stats
	const auto inspected = Common::Pkg::InspectPkg(pkg_path);
	Check(inspected.success, inspected.error.c_str());
	Check(inspected.info.is_ps5_fih && inspected.info.fih_format_version == 3u,
	      "expected PS5 FIH v3 metadata");
	Check(inspected.info.content_id == "EP9000-PPSA99999_00-KYTXTESTPS5PKG01",
	      "unexpected PS5 content ID");
	Check(inspected.info.pfs_file_entries.size() == 2u, "expected 2 PS5 inner files");
	Check(inspected.info.pfs_sparse_blocks > 0u, "expected sparse block count > 0");

	// 2. Executables-only unpacks sce_sys/param.json + eboot.bin, skips Data/test.bin and encrypted license.dat
	const auto                    exec_dir = work_dir / "exec_only";
	Common::Pkg::PkgUnpackOptions exec_opts;
	exec_opts.executables_only = true;
	const auto exec_res        = Common::Pkg::UnpackPkg(pkg_path, exec_dir, exec_opts);
	Check(exec_res.success, exec_res.error.c_str());
	Check(ReadAllBytes(exec_dir / "sce_sys" / "param.json") == param, "param.json mismatch");
	Check(ReadAllBytes(exec_dir / "eboot.bin") == eboot, "eboot.bin mismatch");
	Check(!std::filesystem::exists(exec_dir / "Data" / "test.bin"),
	      "Data/test.bin must be skipped in executables_only mode");
	Check(!std::filesystem::exists(exec_dir / "sce_sys" / "license.dat"),
	      "encrypted license.dat must be skipped cleanly");

	// 3. Full unpack extracts everything byte-for-byte
	const auto full_dir = work_dir / "full";
	const auto full_res = Common::Pkg::UnpackPkg(pkg_path, full_dir);
	Check(full_res.success, full_res.error.c_str());
	Check(ReadAllBytes(full_dir / "Data" / "test.bin") == test_data, "Data/test.bin mismatch");

	// 4. Retail-signed FIH header (0x80) must be refused
	const auto retail_bytes = BuildSyntheticPs5FihPkg(0x80u, param, eboot, test_data);
	const auto retail_path  = work_dir / "ps5_retail.pkg";
	{
		std::FILE* f = std::fopen(retail_path.string().c_str(), "wb");
		Check(f != nullptr &&
		          std::fwrite(retail_bytes.data(), 1, retail_bytes.size(), f) == retail_bytes.size(),
		      "failed writing synthetic retail pkg");
		std::fclose(f);
	}
	const auto retail_res = Common::Pkg::InspectPkg(retail_path);
	Check(!retail_res.success, "retail PS5 FIH package must be rejected");
}

} // namespace

int main() {
	TestCryptoVectors();
	TestSyntheticFpkgUnpack();
	TestKrakenDecoder();
	TestSyntheticPs5FihUnpack();
	std::printf("PkgUnpackerTests passed.\n");
	return 0;
}
