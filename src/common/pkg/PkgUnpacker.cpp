#include "common/pkg/PkgUnpacker.h"

#include "common/pkg/PkgKraken.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Common::Pkg {

namespace {

constexpr uint32_t kCntMagic        = 0x7F434E54u; // "\x7FCNT" big-endian
constexpr uint32_t kFihMagic        = 0x7F464948u; // "\x7FFIH" big-endian
constexpr uint32_t kPfscMagic       = 0x43534650u; // "PFSC" little-endian
constexpr int64_t  kPfsMagic        = 20130315ll;
constexpr uint32_t kXtsSectorSize   = 0x1000u;
constexpr uint16_t kPfsModeSigned   = 0x1u;
constexpr uint16_t kPfsMode64Bit    = 0x2u;
constexpr uint16_t kPfsModeEncrypt  = 0x4u;
constexpr uint64_t kPfsNewCryptFlag = 0x2000000000000000ull;

constexpr std::array<uint8_t, 16> kPlainNoAuthSeed = {
    'P', 'P', 'R', 'P', 'L', 'A', 'I', 'N', '-', 'N', 'O', 'A', 'U', 'T', 'H', '!',
};
constexpr uint64_t kNapsUblockSize      = 0x40000ull;
constexpr uint64_t kNapsOuterBlockSize  = 0x10000ull;
constexpr uint8_t  kNapsKindMount       = 0x40u;
constexpr uint64_t kMaxOuterDirSize     = 8ull << 20;
constexpr uint64_t kMaxNapsSize         = 1ull << 30;
constexpr uint64_t kMaxMetaTail         = 1ull << 30;
constexpr size_t   kPs5InnerInodeSize   = 0xA8u;
constexpr uint32_t kPs5DirentFile       = 2u;
constexpr uint32_t kPs5DirentDir        = 3u;

enum EntryId : uint32_t {
	kEntryDigests        = 0x0001u,
	kEntryKeys           = 0x0010u,
	kEntryImageKey       = 0x0020u,
	kEntryGeneralDigests = 0x0080u,
	kEntryMetas          = 0x0100u,
	kEntryNames          = 0x0200u,
	kEntryScStart        = 0x0400u,
};

uint16_t ReadLe16(const uint8_t* p) noexcept {
	return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u);
}

uint32_t ReadLe32(const uint8_t* p) noexcept {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
	       (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
}

int32_t ReadLeI32(const uint8_t* p) noexcept {
	return static_cast<int32_t>(ReadLe32(p));
}

uint64_t ReadLe64(const uint8_t* p) noexcept {
	return static_cast<uint64_t>(ReadLe32(p)) | (static_cast<uint64_t>(ReadLe32(p + 4)) << 32u);
}

int64_t ReadLeI64(const uint8_t* p) noexcept {
	return static_cast<int64_t>(ReadLe64(p));
}

uint32_t ReadBe32(const uint8_t* p) noexcept {
	return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u) |
	       (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
}

uint64_t ReadBe64(const uint8_t* p) noexcept {
	return (static_cast<uint64_t>(ReadBe32(p)) << 32u) | static_cast<uint64_t>(ReadBe32(p + 4));
}

bool SanitizeRelativePath(std::string_view raw, std::filesystem::path& out_rel) {
	out_rel.clear();
	size_t i = 0;
	while (i < raw.size()) {
		while (i < raw.size() && (raw[i] == '/' || raw[i] == '\\')) {
			++i;
		}
		if (i >= raw.size()) {
			break;
		}
		size_t end = i;
		while (end < raw.size() && raw[end] != '/' && raw[end] != '\\') {
			++end;
		}
		const std::string_view part = raw.substr(i, end - i);
		if (part == "..") {
			return false;
		}
		if (!part.empty() && part != ".") {
			if (part.find('\0') != std::string_view::npos || part.find(':') != std::string_view::npos) {
				return false;
			}
			out_rel /= std::string(part);
		}
		i = end;
	}
	return !out_rel.empty();
}

// Abstract byte-addressable read-only data source supporting arbitrary layering
// (PKG file slice -> optional AES-XTS -> optional PFSC zlib -> chunked/contiguous PFS).
class ByteSource {
public:
	virtual ~ByteSource() = default;
	[[nodiscard]] virtual uint64_t Size() const noexcept = 0;
	virtual bool ReadAt(uint64_t offset, void* out, size_t size) = 0;
};

class FileSliceSource final: public ByteSource {
public:
	FileSliceSource(std::FILE* file, uint64_t base_offset, uint64_t size)
	    : m_file(file), m_base_offset(base_offset), m_size(size) {}

	[[nodiscard]] uint64_t Size() const noexcept override { return m_size; }

	bool ReadAt(uint64_t offset, void* out, size_t size) override {
		if (offset > m_size || size > m_size - offset) {
			return false;
		}
		if (size == 0) {
			return true;
		}
#if defined(_WIN32)
		if (_fseeki64(m_file, static_cast<int64_t>(m_base_offset + offset), SEEK_SET) != 0) {
			return false;
		}
#else
		if (fseeko(m_file, static_cast<off_t>(m_base_offset + offset), SEEK_SET) != 0) {
			return false;
		}
#endif
		return std::fread(out, 1, size, m_file) == size;
	}

private:
	std::FILE* m_file        = nullptr;
	uint64_t   m_base_offset = 0;
	uint64_t   m_size        = 0;
};

class SubSource final: public ByteSource {
public:
	SubSource(std::shared_ptr<ByteSource> inner, uint64_t base_offset, uint64_t size)
	    : m_inner(std::move(inner)), m_base_offset(base_offset), m_size(size) {}

	[[nodiscard]] uint64_t Size() const noexcept override { return m_size; }

	bool ReadAt(uint64_t offset, void* out, size_t size) override {
		if (offset > m_size || size > m_size - offset) {
			return false;
		}
		return m_inner->ReadAt(m_base_offset + offset, out, size);
	}

private:
	std::shared_ptr<ByteSource> m_inner;
	uint64_t                    m_base_offset = 0;
	uint64_t                    m_size        = 0;
};

class ChunkedBlockSource final: public ByteSource {
public:
	ChunkedBlockSource(std::shared_ptr<ByteSource> inner, uint32_t block_size,
	                   std::vector<int32_t> blocks, uint64_t logical_size)
	    : m_inner(std::move(inner)), m_block_size(block_size), m_blocks(std::move(blocks)),
	      m_logical_size(logical_size) {}

	[[nodiscard]] uint64_t Size() const noexcept override { return m_logical_size; }

	bool ReadAt(uint64_t offset, void* out, size_t size) override {
		if (offset > m_logical_size || size > m_logical_size - offset) {
			return false;
		}
		auto*    dst = static_cast<uint8_t*>(out);
		uint64_t pos = offset;
		size_t   rem = size;
		while (rem > 0) {
			const size_t blk_idx = static_cast<size_t>(pos / m_block_size);
			if (blk_idx >= m_blocks.size() || m_blocks[blk_idx] < 0) {
				return false;
			}
			const uint64_t in_blk = pos % m_block_size;
			const size_t   take   = std::min(rem, static_cast<size_t>(m_block_size - in_blk));
			const uint64_t phys   = static_cast<uint64_t>(m_blocks[blk_idx]) * m_block_size + in_blk;
			if (!m_inner->ReadAt(phys, dst, take)) {
				return false;
			}
			dst += take;
			pos += take;
			rem -= take;
		}
		return true;
	}

private:
	std::shared_ptr<ByteSource> m_inner;
	uint32_t                    m_block_size = 0;
	std::vector<int32_t>        m_blocks;
	uint64_t                    m_logical_size = 0;
};

class XtsDecryptSource final: public ByteSource {
public:
	XtsDecryptSource(std::shared_ptr<ByteSource> inner, const PfsXtsKeys& keys,
	                 uint32_t start_sector, uint32_t sector_size = kXtsSectorSize)
	    : m_inner(std::move(inner)), m_data_ctx(keys.data_key), m_tweak_ctx(keys.tweak_key),
	      m_start_sector(start_sector), m_sector_size(sector_size), m_sector_buf(sector_size) {}

	[[nodiscard]] uint64_t Size() const noexcept override { return m_inner->Size(); }

	bool ReadAt(uint64_t offset, void* out, size_t size) override {
		if (offset > m_inner->Size() || size > m_inner->Size() - offset) {
			return false;
		}
		auto*    dst = static_cast<uint8_t*>(out);
		uint64_t pos = offset;
		size_t   rem = size;
		while (rem > 0) {
			const uint64_t sector_idx = pos / m_sector_size;
			const size_t   in_sector  = static_cast<size_t>(pos % m_sector_size);
			const size_t   avail      = static_cast<size_t>(
			    std::min<uint64_t>(m_sector_size, m_inner->Size() - sector_idx * m_sector_size));
			if (m_cached_sector != sector_idx) {
				if (!m_inner->ReadAt(sector_idx * m_sector_size, m_sector_buf.data(), avail)) {
					return false;
				}
				if (sector_idx >= m_start_sector && (avail % 16u) == 0u) {
					Aes128XtsDecryptSector(m_data_ctx, m_tweak_ctx, sector_idx,
					                       std::span<uint8_t>(m_sector_buf.data(), avail));
				}
				m_cached_sector = sector_idx;
			}
			const size_t take = std::min(rem, avail - in_sector);
			std::memcpy(dst, m_sector_buf.data() + in_sector, take);
			dst += take;
			pos += take;
			rem -= take;
		}
		return true;
	}

private:
	std::shared_ptr<ByteSource> m_inner;
	Aes128Context               m_data_ctx;
	Aes128Context               m_tweak_ctx;
	uint32_t                    m_start_sector  = 0;
	uint32_t                    m_sector_size   = kXtsSectorSize;
	std::vector<uint8_t>        m_sector_buf;
	uint64_t                    m_cached_sector = UINT64_MAX;
};

class PfscDecompressSource final: public ByteSource {
public:
	static std::shared_ptr<PfscDecompressSource> Create(std::shared_ptr<ByteSource> inner,
	                                                    std::string&                out_error) {
		if (inner->Size() < 0x30u) {
			out_error = "PFSC image truncated";
			return nullptr;
		}
		uint8_t hdr_bytes[0x30] {};
		if (!inner->ReadAt(0, hdr_bytes, sizeof(hdr_bytes))) {
			out_error = "Failed to read PFSC header";
			return nullptr;
		}
		if (ReadLe32(hdr_bytes + 0x00) != kPfscMagic) {
			out_error = "Invalid PFSC magic";
			return nullptr;
		}
		const uint32_t block_sz      = ReadLe32(hdr_bytes + 0x0c);
		const uint64_t block_sz2     = ReadLe64(hdr_bytes + 0x10);
		const uint64_t block_offsets = ReadLe64(hdr_bytes + 0x18);
		const uint64_t data_length   = ReadLe64(hdr_bytes + 0x28);
		if (block_sz == 0 || block_sz != block_sz2 || data_length == 0 ||
		    block_sz > (16u << 20u)) {
			out_error = "Invalid PFSC header geometry";
			return nullptr;
		}
		const uint64_t num_blocks = (data_length + block_sz - 1u) / block_sz;
		const uint64_t map_bytes  = (num_blocks + 1u) * sizeof(uint64_t);
		if (block_offsets > inner->Size() || map_bytes > inner->Size() - block_offsets) {
			out_error = "PFSC sector table out of bounds";
			return nullptr;
		}
		std::vector<uint8_t> raw_map(static_cast<size_t>(map_bytes));
		if (!inner->ReadAt(block_offsets, raw_map.data(), raw_map.size())) {
			out_error = "Failed to read PFSC sector table";
			return nullptr;
		}
		std::vector<uint64_t> sector_map(static_cast<size_t>(num_blocks + 1u));
		for (size_t i = 0; i < sector_map.size(); ++i) {
			sector_map[i] = ReadLe64(raw_map.data() + i * 8u);
		}
		return std::shared_ptr<PfscDecompressSource>(
		    new PfscDecompressSource(std::move(inner), block_sz, data_length, std::move(sector_map)));
	}

	[[nodiscard]] uint64_t Size() const noexcept override { return m_data_length; }

	bool ReadAt(uint64_t offset, void* out, size_t size) override {
		if (offset > m_data_length || size > m_data_length - offset) {
			return false;
		}
		auto*    dst = static_cast<uint8_t*>(out);
		uint64_t pos = offset;
		size_t   rem = size;
		while (rem > 0) {
			const uint64_t sector_idx = pos / m_block_size;
			if (m_cached_sector != sector_idx) {
				if (!LoadSector(static_cast<size_t>(sector_idx))) {
					return false;
				}
				m_cached_sector = sector_idx;
			}
			const size_t in_blk = static_cast<size_t>(pos % m_block_size);
			const size_t take   = std::min(rem, m_block_size - in_blk);
			std::memcpy(dst, m_sector_buf.data() + in_blk, take);
			dst += take;
			pos += take;
			rem -= take;
		}
		return true;
	}

private:
	PfscDecompressSource(std::shared_ptr<ByteSource> inner, uint32_t block_size,
	                     uint64_t data_length, std::vector<uint64_t> sector_map)
	    : m_inner(std::move(inner)), m_block_size(block_size), m_data_length(data_length),
	      m_sector_map(std::move(sector_map)), m_sector_buf(block_size) {}

	bool LoadSector(size_t idx) {
		if (idx + 1u >= m_sector_map.size()) {
			return false;
		}
		const uint64_t off      = m_sector_map[idx];
		const uint64_t next_off = m_sector_map[idx + 1u];
		if (next_off < off) {
			return false;
		}
		const uint64_t stored_sz = next_off - off;
		if (stored_sz > m_block_size) {
			std::memset(m_sector_buf.data(), 0, m_block_size);
			return true;
		}
		if (stored_sz == m_block_size) {
			return m_inner->ReadAt(off, m_sector_buf.data(), m_block_size);
		}
		if (stored_sz < 2u) {
			std::memset(m_sector_buf.data(), 0, m_block_size);
			return true;
		}
		m_compressed_buf.resize(static_cast<size_t>(stored_sz));
		if (!m_inner->ReadAt(off, m_compressed_buf.data(), m_compressed_buf.size())) {
			return false;
		}
		const int decoded = stbi_zlib_decode_buffer(
		    reinterpret_cast<char*>(m_sector_buf.data()), static_cast<int>(m_block_size),
		    reinterpret_cast<const char*>(m_compressed_buf.data()),
		    static_cast<int>(m_compressed_buf.size()));
		if (decoded < 0) {
			return false;
		}
		if (static_cast<uint32_t>(decoded) < m_block_size) {
			std::memset(m_sector_buf.data() + decoded, 0, m_block_size - static_cast<size_t>(decoded));
		}
		return true;
	}

	std::shared_ptr<ByteSource> m_inner;
	uint32_t                    m_block_size = 0;
	uint64_t                    m_data_length = 0;
	std::vector<uint64_t>       m_sector_map;
	std::vector<uint8_t>        m_sector_buf;
	std::vector<uint8_t>        m_compressed_buf;
	uint64_t                    m_cached_sector = UINT64_MAX;
};

struct PfsHeader {
	int64_t                 version            = 0;
	int64_t                 magic              = 0;
	uint16_t                mode               = 0;
	uint32_t                block_size         = 0;
	int64_t                 dinode_count       = 0;
	int64_t                 ndblock            = 0;
	int64_t                 dinode_block_count = 0;
	std::array<uint8_t, 16> seed {};
};

struct PfsInode {
	uint16_t mode            = 0;
	int64_t  size            = 0;
	int64_t  compressed_size = 0;
	uint32_t blocks          = 0;
	int32_t  db[12]          = {};
	int32_t  ib[5]           = {};
};

struct PfsDirent {
	uint32_t    inode_number = 0;
	int32_t     type         = 0;
	std::string name;
	uint32_t    ent_size     = 0;
};

class PfsVolume {
public:
	static std::unique_ptr<PfsVolume> Open(std::shared_ptr<ByteSource>           src,
	                                       uint64_t                              pfs_flags,
	                                       const std::optional<std::array<uint8_t, 32>>& ekpfs,
	                                       std::string&                          out_error) {
		if (!src || src->Size() < 0x400u) {
			out_error = "PFS image too small";
			return nullptr;
		}
		uint8_t hdr_buf[0x400] {};
		if (!src->ReadAt(0, hdr_buf, sizeof(hdr_buf))) {
			out_error = "Failed to read PFS superblock";
			return nullptr;
		}
		PfsHeader hdr {};
		hdr.version            = ReadLeI64(hdr_buf + 0x00);
		hdr.magic              = ReadLeI64(hdr_buf + 0x08);
		hdr.mode               = ReadLe16(hdr_buf + 0x1c);
		hdr.block_size         = ReadLe32(hdr_buf + 0x20);
		hdr.dinode_count       = ReadLeI64(hdr_buf + 0x30);
		hdr.ndblock            = ReadLeI64(hdr_buf + 0x38);
		hdr.dinode_block_count = ReadLeI64(hdr_buf + 0x40);
		std::memcpy(hdr.seed.data(), hdr_buf + 0x370, 16);

		if (hdr.version != 1 || hdr.magic != kPfsMagic || hdr.block_size < 0x1000u ||
		    hdr.dinode_count <= 0 || hdr.dinode_block_count <= 0 || hdr.dinode_count > (1 << 22)) {
			out_error = "Invalid PFS superblock";
			return nullptr;
		}

		std::shared_ptr<ByteSource> active_src = std::move(src);
		if ((hdr.mode & kPfsModeEncrypt) != 0u) {
			if (!ekpfs.has_value()) {
				out_error = "PFS image is encrypted and no valid EKPFS key is available (wrong passcode or retail PKG)";
				return nullptr;
			}
			const bool       new_crypt    = (pfs_flags & kPfsNewCryptFlag) != 0ull;
			const PfsXtsKeys xts_keys     = DerivePfsXtsKeys(*ekpfs, hdr.seed, new_crypt);
			const uint32_t   start_sector = hdr.block_size / kXtsSectorSize;
			active_src = std::make_shared<XtsDecryptSource>(std::move(active_src), xts_keys, start_sector);
		}

		const bool     is_signed     = (hdr.mode & kPfsModeSigned) != 0u;
		const size_t   dinode_stride = is_signed ? 0x2c8u : 0xa8u;
		const uint32_t per_sector    = hdr.block_size / static_cast<uint32_t>(dinode_stride);
		if (per_sector == 0) {
			out_error = "Invalid PFS block size";
			return nullptr;
		}

		std::vector<PfsInode> inodes;
		inodes.reserve(static_cast<size_t>(hdr.dinode_count));
		std::vector<uint8_t> sector(hdr.block_size);
		for (int64_t blk = 0; blk < hdr.dinode_block_count &&
		                      static_cast<int64_t>(inodes.size()) < hdr.dinode_count;
		     ++blk) {
			const uint64_t pos = static_cast<uint64_t>(hdr.block_size) * static_cast<uint64_t>(blk + 1);
			if (!active_src->ReadAt(pos, sector.data(), sector.size())) {
				out_error = "Failed to read PFS dinode table (wrong key or corrupt image)";
				return nullptr;
			}
			for (uint32_t j = 0;
			     j < per_sector && static_cast<int64_t>(inodes.size()) < hdr.dinode_count; ++j) {
				const uint8_t* p = sector.data() + j * dinode_stride;
				PfsInode       ino {};
				ino.mode            = ReadLe16(p + 0x00);
				ino.size            = ReadLeI64(p + 0x08);
				ino.compressed_size = ReadLeI64(p + 0x10);
				ino.blocks          = ReadLe32(p + 0x60);
				if (is_signed) {
					for (int k = 0; k < 12; ++k) {
						ino.db[k] = ReadLeI32(p + 0x64 + k * 36 + 32);
					}
					for (int k = 0; k < 5; ++k) {
						ino.ib[k] = ReadLeI32(p + 0x64 + 12 * 36 + k * 36 + 32);
					}
				} else {
					for (int k = 0; k < 12; ++k) {
						ino.db[k] = ReadLeI32(p + 0x64 + k * 4);
					}
					for (int k = 0; k < 5; ++k) {
						ino.ib[k] = ReadLeI32(p + 0x64 + 12 * 4 + k * 4);
					}
				}
				inodes.push_back(ino);
			}
		}

		auto vol = std::unique_ptr<PfsVolume>(
		    new PfsVolume(std::move(active_src), hdr, std::move(inodes)));
		if (!vol->FindURoot(out_error)) {
			return nullptr;
		}
		return vol;
	}

	struct FileEntry {
		std::filesystem::path rel_path;
		uint32_t              inode_idx       = 0;
		uint64_t              size            = 0;
		uint64_t              compressed_size = 0;
	};

	bool ListFiles(const std::filesystem::path& prefix, std::vector<FileEntry>& out_files,
	               std::string& out_error) {
		return CollectDir(m_uroot_inode, prefix, out_files, out_error, 0);
	}

	[[nodiscard]] bool HasRootEntry(std::string_view name, uint32_t& out_inode) const {
		std::vector<PfsDirent> entries;
		std::string            err;
		if (!const_cast<PfsVolume*>(this)->ReadDirectory(0u, entries, err)) {
			return false;
		}
		for (const auto& d: entries) {
			if (d.type == 2 && d.name == name) {
				out_inode = d.inode_number;
				return true;
			}
		}
		return false;
	}

	std::shared_ptr<ByteSource> OpenFileSource(uint32_t inode_idx, std::string& out_error) const {
		if (inode_idx >= m_inodes.size()) {
			out_error = "Invalid PFS file inode";
			return nullptr;
		}
		const auto& ino = m_inodes[inode_idx];
		if (ino.size < 0) {
			out_error = "Negative PFS file size";
			return nullptr;
		}
		const uint64_t raw_size = static_cast<uint64_t>(ino.size);
		if (ino.blocks <= 1u || ino.db[1] == -1) {
			if (ino.db[0] < 0) {
				out_error = "Invalid contiguous PFS block index";
				return nullptr;
			}
			const uint64_t offset = static_cast<uint64_t>(ino.db[0]) * m_hdr.block_size;
			return std::make_shared<SubSource>(m_src, offset, raw_size);
		}
		if ((m_hdr.mode & kPfsModeSigned) == 0u) {
			out_error = "Noncontiguous unsigned PFS file not supported";
			return nullptr;
		}

		std::vector<int32_t> blocks(ino.blocks, -1);
		size_t               rem           = ino.blocks;
		size_t               out_pos       = 0;
		const size_t         direct_take   = std::min<size_t>(rem, 12u);
		for (size_t i = 0; i < direct_take; ++i) {
			blocks[out_pos++] = ino.db[i];
		}
		rem -= direct_take;

		const size_t         sigs_per_block = m_hdr.block_size / 36u;
		std::vector<uint8_t> blk_buf(m_hdr.block_size);
		if (rem > 0) {
			if (ino.ib[0] < 0) {
				out_error = "Missing indirect PFS block";
				return nullptr;
			}
			if (!m_src->ReadAt(static_cast<uint64_t>(ino.ib[0]) * m_hdr.block_size, blk_buf.data(),
			                   blk_buf.size())) {
				out_error = "Failed reading indirect PFS block";
				return nullptr;
			}
			const size_t take = std::min(rem, sigs_per_block);
			for (size_t i = 0; i < take; ++i) {
				blocks[out_pos++] = ReadLeI32(blk_buf.data() + i * 36u + 32u);
			}
			rem -= take;
		}
		if (rem > 0) {
			if (ino.ib[1] < 0) {
				out_error = "Missing double-indirect PFS block";
				return nullptr;
			}
			std::vector<uint8_t> l2_buf(m_hdr.block_size);
			if (!m_src->ReadAt(static_cast<uint64_t>(ino.ib[1]) * m_hdr.block_size, l2_buf.data(),
			                   l2_buf.size())) {
				out_error = "Failed reading double-indirect PFS block";
				return nullptr;
			}
			for (size_t j = 0; j < sigs_per_block && rem > 0; ++j) {
				const int32_t child = ReadLeI32(l2_buf.data() + j * 36u + 32u);
				if (child < 0 || !m_src->ReadAt(static_cast<uint64_t>(child) * m_hdr.block_size,
				                                blk_buf.data(), blk_buf.size())) {
					out_error = "Failed reading double-indirect child block";
					return nullptr;
				}
				const size_t take = std::min(rem, sigs_per_block);
				for (size_t i = 0; i < take; ++i) {
					blocks[out_pos++] = ReadLeI32(blk_buf.data() + i * 36u + 32u);
				}
				rem -= take;
			}
		}

		bool contiguous = true;
		for (size_t i = 1; i < blocks.size(); ++i) {
			if (blocks[i - 1] + 1 != blocks[i]) {
				contiguous = false;
				break;
			}
		}
		if (contiguous) {
			return std::make_shared<SubSource>(
			    m_src, static_cast<uint64_t>(blocks[0]) * m_hdr.block_size, raw_size);
		}
		return std::make_shared<ChunkedBlockSource>(m_src, m_hdr.block_size, std::move(blocks),
		                                            raw_size);
	}

	const PfsInode& Inode(uint32_t idx) const { return m_inodes[idx]; }

private:
	PfsVolume(std::shared_ptr<ByteSource> src, PfsHeader hdr, std::vector<PfsInode> inodes)
	    : m_src(std::move(src)), m_hdr(hdr), m_inodes(std::move(inodes)) {}

	bool FindURoot(std::string& out_error) {
		std::vector<PfsDirent> root_entries;
		if (!ReadDirectory(0u, root_entries, out_error)) {
			return false;
		}
		for (const auto& d: root_entries) {
			if (d.type == 3 && d.name == "uroot") {
				m_uroot_inode = d.inode_number;
				return true;
			}
		}
		// Some inner images may put files directly at inode 0 if no uroot subfolder exists.
		m_uroot_inode = 0;
		return true;
	}

	bool ReadDirectory(uint32_t dinode, std::vector<PfsDirent>& out_entries, std::string& out_error) {
		if (dinode >= m_inodes.size()) {
			out_error = "Directory inode out of range";
			return false;
		}
		const auto& ino = m_inodes[dinode];
		if (ino.blocks < 1u || ino.db[0] < 1 || ino.blocks > 1000000u) {
			out_error = "Corrupt directory inode blocks";
			return false;
		}
		std::vector<uint8_t> sec(m_hdr.block_size);
		for (uint32_t b = 0; b < ino.blocks; ++b) {
			const uint64_t blk_num = static_cast<uint64_t>(ino.db[0]) + b;
			if (!m_src->ReadAt(blk_num * m_hdr.block_size, sec.data(), sec.size())) {
				out_error = "Failed reading PFS directory block";
				return false;
			}
			size_t pos = 0;
			while (pos + 16u <= m_hdr.block_size) {
				const uint32_t ino_num  = ReadLe32(sec.data() + pos + 0);
				const int32_t  type     = ReadLeI32(sec.data() + pos + 4);
				const uint32_t name_len = ReadLe32(sec.data() + pos + 8);
				const uint32_t ent_size = ReadLe32(sec.data() + pos + 12);
				if (ent_size == 0) {
					break;
				}
				if (ent_size < 16u || pos + ent_size > m_hdr.block_size || 16u + name_len > ent_size) {
					out_error = "Corrupt PFS dirent record";
					return false;
				}
				PfsDirent d {};
				d.inode_number = ino_num;
				d.type         = type;
				d.ent_size     = ent_size;
				if (name_len > 0) {
					d.name.assign(reinterpret_cast<const char*>(sec.data() + pos + 16), name_len);
				}
				out_entries.push_back(std::move(d));
				pos += ent_size;
			}
		}
		return true;
	}

	bool CollectDir(uint32_t dinode, const std::filesystem::path& prefix,
	                std::vector<FileEntry>& out_files, std::string& out_error, int depth) {
		if (depth > 64) {
			out_error = "PFS directory nesting exceeds limit";
			return false;
		}
		std::vector<PfsDirent> entries;
		if (!ReadDirectory(dinode, entries, out_error)) {
			return false;
		}
		for (const auto& d: entries) {
			if (d.type == 4 || d.type == 5) { // '.' or '..'
				continue;
			}
			std::filesystem::path rel_part;
			if (!SanitizeRelativePath(d.name, rel_part)) {
				out_error = "Unsafe path entry inside PFS: " + d.name;
				return false;
			}
			const auto child_rel = prefix.empty() ? rel_part : (prefix / rel_part);
			if (d.type == 2) {
				if (d.inode_number >= m_inodes.size()) {
					out_error = "Invalid file inode in PFS";
					return false;
				}
				const auto& ino = m_inodes[d.inode_number];
				out_files.push_back({
				    .rel_path        = child_rel,
				    .inode_idx       = d.inode_number,
				    .size            = static_cast<uint64_t>(std::max<int64_t>(0, ino.size)),
				    .compressed_size = static_cast<uint64_t>(std::max<int64_t>(0, ino.compressed_size)),
				});
			} else if (d.type == 3) {
				if (!CollectDir(d.inode_number, child_rel, out_files, out_error, depth + 1)) {
					return false;
				}
			}
		}
		return true;
	}

	std::shared_ptr<ByteSource> m_src;
	PfsHeader                   m_hdr {};
	std::vector<PfsInode>       m_inodes;
	uint32_t                    m_uroot_inode = 0;
};

struct RawMetaEntry {
	PkgEntrySummary         summary;
	std::array<uint8_t, 32> raw_meta_be {};
	uint32_t                name_offset = 0;
	uint32_t                key_index   = 0;
};

struct ParsedPkg {
	PkgInfo                   info;
	std::vector<RawMetaEntry> raw_entries;
	std::array<uint8_t, 32>   key0_digest {};
	bool                      has_key0_digest = false;
	std::optional<std::array<uint8_t, 32>> ekpfs;
	std::FILE*                file       = nullptr;
	uint64_t                  file_size  = 0;

	~ParsedPkg() {
		if (file != nullptr) {
			std::fclose(file);
		}
	}
};

std::string DefaultEntryName(uint32_t id) {
	switch (id) {
		case 0x1000u: return "param.sfo";
		case 0x1001u: return "playgo-chunk.dat";
		case 0x1002u: return "playgo-chunk.sha";
		case 0x1003u: return "playgo-manifest.xml";
		case 0x1004u: return "pronunciation.xml";
		case 0x1005u: return "pronunciation.sig";
		case 0x1006u: return "pic1.png";
		case 0x1007u: return "pubtoolinfo.dat";
		case 0x1008u: return "app/playgo-chunk.dat";
		case 0x1200u: return "icon0.png";
		default: {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "entry_%04x.bin", id);
			return buf;
		}
	}
}

bool ParsePkgFile(const std::filesystem::path& pkg_path, std::string_view passcode,
                  ParsedPkg& out_pkg, std::string& out_error) {
	std::FILE* file = std::fopen(pkg_path.string().c_str(), "rb");
	if (file == nullptr) {
		out_error = "Failed to open PKG file: " + pkg_path.string();
		return false;
	}
	out_pkg.file = file;

#if defined(_WIN32)
	_fseeki64(file, 0, SEEK_END);
	out_pkg.file_size = static_cast<uint64_t>(_ftelli64(file));
	_fseeki64(file, 0, SEEK_SET);
#else
	fseeko(file, 0, SEEK_END);
	out_pkg.file_size = static_cast<uint64_t>(ftello(file));
	fseeko(file, 0, SEEK_SET);
#endif

	if (out_pkg.file_size < 0x5A0u) {
		out_error = "File too small to be a PS4/PS5 CNT PKG";
		return false;
	}
	std::vector<uint8_t> hdr(0x5A0u);
	if (std::fread(hdr.data(), 1, hdr.size(), file) != hdr.size()) {
		out_error = "Failed reading PKG header";
		return false;
	}
	if (ReadBe32(hdr.data() + 0x00) != kCntMagic) {
		out_error = "Invalid CNT PKG magic (expected \\x7FCNT)";
		return false;
	}

	auto& info             = out_pkg.info;
	info.flags             = ReadBe32(hdr.data() + 0x04);
	const uint32_t entries = ReadBe32(hdr.data() + 0x10);
	const uint32_t tbl_off = ReadBe32(hdr.data() + 0x18);
	const size_t   cid_len = std::min<size_t>(36u, strnlen(reinterpret_cast<const char*>(hdr.data() + 0x40), 0x30u));
	info.content_id.assign(reinterpret_cast<const char*>(hdr.data() + 0x40), cid_len);
	info.drm_type          = ReadBe32(hdr.data() + 0x70);
	info.content_type      = ReadBe32(hdr.data() + 0x74);
	info.content_flags     = ReadBe32(hdr.data() + 0x78);
	info.pfs_flags         = ReadBe64(hdr.data() + 0x408);
	info.pfs_image_offset  = ReadBe64(hdr.data() + 0x410);
	info.pfs_image_size    = ReadBe64(hdr.data() + 0x418);
	info.package_size      = ReadBe64(hdr.data() + 0x430);
	info.is_fpkg           = true;

	const uint64_t table_bytes = static_cast<uint64_t>(entries) * 0x20u;
	if (entries > 65536u || static_cast<uint64_t>(tbl_off) + table_bytes > out_pkg.file_size) {
		out_error = "PKG entry table out of bounds";
		return false;
	}

	FileSliceSource full_src(file, 0, out_pkg.file_size);
	std::vector<uint8_t> table_buf(static_cast<size_t>(table_bytes));
	if (!full_src.ReadAt(tbl_off, table_buf.data(), table_buf.size())) {
		out_error = "Failed reading PKG entry table";
		return false;
	}

	out_pkg.raw_entries.reserve(entries);
	const RawMetaEntry* names_entry = nullptr;
	const RawMetaEntry* keys_entry  = nullptr;

	for (uint32_t i = 0; i < entries; ++i) {
		const uint8_t* p = table_buf.data() + i * 0x20u;
		RawMetaEntry   e {};
		std::memcpy(e.raw_meta_be.data(), p, 32);
		e.summary.id        = ReadBe32(p + 0x00);
		e.name_offset       = ReadBe32(p + 0x04);
		e.summary.flags1    = ReadBe32(p + 0x08);
		e.summary.flags2    = ReadBe32(p + 0x0c);
		e.summary.offset    = ReadBe32(p + 0x10);
		e.summary.size      = ReadBe32(p + 0x14);
		e.summary.encrypted = (e.summary.flags1 & 0x80000000u) != 0u;
		e.key_index         = (e.summary.flags2 & 0xF000u) >> 12u;
		out_pkg.raw_entries.push_back(e);
		if (out_pkg.raw_entries.back().summary.id == kEntryNames) {
			names_entry = &out_pkg.raw_entries.back();
		} else if (out_pkg.raw_entries.back().summary.id == kEntryKeys) {
			keys_entry = &out_pkg.raw_entries.back();
		}
	}

	std::vector<uint8_t> names_buf;
	if (names_entry != nullptr && names_entry->summary.size > 0 &&
	    static_cast<uint64_t>(names_entry->summary.offset) + names_entry->summary.size <=
	        out_pkg.file_size) {
		names_buf.resize(names_entry->summary.size);
		(void)full_src.ReadAt(names_entry->summary.offset, names_buf.data(), names_buf.size());
	}

	for (auto& e: out_pkg.raw_entries) {
		if (!names_buf.empty() && e.name_offset < names_buf.size()) {
			const char* start = reinterpret_cast<const char*>(names_buf.data() + e.name_offset);
			const size_t max_len = names_buf.size() - e.name_offset;
			const size_t len     = strnlen(start, max_len);
			e.summary.name.assign(start, len);
		}
		if (e.summary.name.empty() && e.summary.id >= 0x1000u) {
			e.summary.name = DefaultEntryName(e.summary.id);
		}
		info.entries.push_back(e.summary);
	}

	if (keys_entry != nullptr && keys_entry->summary.size >= 0x40u &&
	    static_cast<uint64_t>(keys_entry->summary.offset) + 0x40u <= out_pkg.file_size) {
		std::vector<uint8_t> keys_buf(keys_entry->summary.size);
		if (full_src.ReadAt(keys_entry->summary.offset, keys_buf.data(), keys_buf.size())) {
			std::memcpy(out_pkg.key0_digest.data(), keys_buf.data() + 0x20u, 32);
			out_pkg.has_key0_digest = true;
			info.passcode_valid =
			    VerifyPasscodeDigest(info.content_id, passcode, out_pkg.key0_digest);
			if (info.passcode_valid) {
				std::array<uint8_t, 32> ekpfs {};
				if (ComputePkgDerivedKey(info.content_id, passcode, 1u, ekpfs)) {
					out_pkg.ekpfs = ekpfs;
				}
			}
		}
	} else {
		// Unprotected/unkeyed test/homebrew package with fallback passcode key
		std::array<uint8_t, 32> ekpfs {};
		if (ComputePkgDerivedKey(info.content_id, passcode, 1u, ekpfs)) {
			out_pkg.ekpfs     = ekpfs;
			info.passcode_valid = true;
		}
	}

	// Probe outer PFS flags if present
	if (info.pfs_image_size >= 0x400u &&
	    info.pfs_image_offset + info.pfs_image_size <= out_pkg.file_size) {
		uint8_t pfs_sb[0x40] {};
		if (full_src.ReadAt(info.pfs_image_offset, pfs_sb, sizeof(pfs_sb)) &&
		    ReadLeI64(pfs_sb + 0x08) == kPfsMagic) {
			const uint16_t mode = ReadLe16(pfs_sb + 0x1c);
			info.pfs_encrypted  = (mode & kPfsModeEncrypt) != 0u;
		}
	}

	return true;
}

bool OpenInnerAndOuterPfs(ParsedPkg& parsed, std::unique_ptr<PfsVolume>& out_vol,
                          std::string& out_error) {
	if (parsed.info.pfs_image_size == 0) {
		return true;
	}
	if (parsed.info.pfs_image_offset + parsed.info.pfs_image_size > parsed.file_size) {
		out_error = "PFS image extent exceeds PKG file size";
		return false;
	}
	auto outer_src = std::make_shared<FileSliceSource>(parsed.file, parsed.info.pfs_image_offset,
	                                                   parsed.info.pfs_image_size);
	auto outer_vol = PfsVolume::Open(outer_src, parsed.info.pfs_flags, parsed.ekpfs, out_error);
	if (!outer_vol) {
		return false;
	}

	uint32_t inner_inode = 0;
	if (outer_vol->HasRootEntry("pfs_image.dat", inner_inode)) {
		std::shared_ptr<ByteSource> inner_src = outer_vol->OpenFileSource(inner_inode, out_error);
		if (!inner_src) {
			return false;
		}
		uint8_t magic[4] {};
		if (inner_src->Size() >= 4u && inner_src->ReadAt(0, magic, sizeof(magic)) &&
		    ReadLe32(magic) == kPfscMagic) {
			parsed.info.pfs_compressed = true;
			inner_src                  = PfscDecompressSource::Create(std::move(inner_src), out_error);
			if (!inner_src) {
				return false;
			}
		}
		out_vol = PfsVolume::Open(std::move(inner_src), parsed.info.pfs_flags, std::nullopt, out_error);
		return out_vol != nullptr;
	}

	out_vol = std::move(outer_vol);
	return true;
}

bool WriteStreamToFile(ByteSource& src, uint64_t size, const std::filesystem::path& dst_path,
                       std::string& out_error) {
	std::error_code ec;
	std::filesystem::create_directories(dst_path.parent_path(), ec);
	if (ec) {
		out_error = "Failed to create directory: " + dst_path.parent_path().string();
		return false;
	}
	std::FILE* out = std::fopen(dst_path.string().c_str(), "wb");
	if (out == nullptr) {
		out_error = "Failed to create output file: " + dst_path.string();
		return false;
	}
	std::vector<uint8_t> buf(65536u);
	uint64_t             pos = 0;
	while (pos < size) {
		const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - pos));
		if (!src.ReadAt(pos, buf.data(), chunk) || std::fwrite(buf.data(), 1, chunk, out) != chunk) {
			std::fclose(out);
			out_error = "Failed writing output file: " + dst_path.string();
			return false;
		}
		pos += chunk;
	}
	std::fclose(out);
	return true;
}

bool EndsWithIgnoreCase(std::string_view s, std::string_view suffix) noexcept {
	if (s.size() < suffix.size()) {
		return false;
	}
	const std::string_view tail = s.substr(s.size() - suffix.size());
	for (size_t i = 0; i < tail.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(tail[i])) !=
		    std::tolower(static_cast<unsigned char>(suffix[i]))) {
			return false;
		}
	}
	return true;
}

std::string TrimSlashes(std::string_view s) {
	while (!s.empty() && (s.front() == '/' || s.front() == '\\')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
		s.remove_suffix(1);
	}
	return std::string(s);
}

bool MatchesSelection(std::string_view rel_path, const PkgUnpackOptions& options) {
	if (options.executables_only) {
		return rel_path == "eboot.bin" || rel_path.starts_with("sce_module/") ||
		       EndsWithIgnoreCase(rel_path, ".prx") || EndsWithIgnoreCase(rel_path, ".sprx") ||
		       EndsWithIgnoreCase(rel_path, ".self") || EndsWithIgnoreCase(rel_path, ".elf");
	}
	if (!options.only_prefixes.empty()) {
		for (const auto& raw_prefix: options.only_prefixes) {
			const std::string p = TrimSlashes(raw_prefix);
			if (p.empty()) {
				continue;
			}
			if (rel_path == p ||
			    (rel_path.size() > p.size() && rel_path.starts_with(p) && rel_path[p.size()] == '/')) {
				return true;
			}
		}
		return false;
	}
	return true;
}

bool CheckAvailableSpace(const std::filesystem::path& output_dir,
                         uint64_t                     needed_bytes,
                         bool                         check_free_space,
                         std::string&                 out_error) {
	if (!check_free_space || needed_bytes == 0u) {
		return true;
	}
	std::error_code                 ec;
	const std::filesystem::space_info sp = std::filesystem::space(output_dir, ec);
	if (ec || sp.available == static_cast<uintmax_t>(-1)) {
		return true;
	}
	const uint64_t margin = 64ull << 20;
	if (needed_bytes > sp.available || (sp.available - needed_bytes) < margin) {
		const double need_gib  = static_cast<double>(needed_bytes) / static_cast<double>(1ull << 30);
		const double avail_gib = static_cast<double>(sp.available) / static_cast<double>(1ull << 30);
		char         msg[512];
		std::snprintf(msg, sizeof(msg),
		              "Insufficient free disk space in '%s': need %.2f GiB, available %.2f GiB "
		              "(use --only or --executables-only to unpack a subset)",
		              output_dir.string().c_str(), need_gib, avail_gib);
		out_error = msg;
		return false;
	}
	return true;
}

std::string DefaultPs5CntName(uint32_t id) {
	switch (id) {
		case 0x0001u: return "digests.bin";
		case 0x0010u: return "entry_keys.bin";
		case 0x0020u: return "image_key.bin";
		case 0x0080u: return "general_digests.bin";
		case 0x0100u: return "metas.bin";
		case 0x0200u: return "entry_names.bin";
		case 0x0400u: return "license.dat";
		case 0x0401u: return "license.info";
		case 0x0402u: return "nptitle.dat";
		case 0x040au: return "imagedigs.dat";
		case 0x1001u: return "playgo-chunk.dat";
		case 0x1200u: return "icon0.png";
		case 0x1220u: return "pic0.png";
		case 0x1240u: return "snd0.at9";
		case 0x1280u: return "icon0.dds";
		case 0x12a0u: return "pic0.dds";
		case 0x2000u: return "param.json";
		case 0x2010u: return "playgo-hash-table.dat";
		case 0x2011u: return "playgo-ficm.dat";
		default: return "unnamed.bin";
	}
}

struct Ps5FihHeader {
	uint16_t format_version    = 0;
	uint64_t pfs_offset        = 0;
	uint64_t pfs_size          = 0;
	uint64_t superblock_offset = 0;
	uint64_t cnt_offset        = 0;
};

struct Ps5CntExtractItem {
	std::filesystem::path rel_path;
	uint64_t              file_offset = 0;
	uint32_t              size        = 0;
};

struct Ps5InodeLayout {
	size_t size      = 0;
	size_t first_ptr = 0;
	size_t stride    = 0;
	size_t ptr_at    = 0;
	bool   ptr64     = false;

	uint64_t ReadPtr(const uint8_t* base, size_t index) const noexcept {
		const size_t at = index * stride + ptr_at;
		return ptr64 ? ReadLe64(base + at) : ReadLe32(base + at);
	}
};

struct Ps5OuterInode {
	uint64_t                    size = 0;
	std::array<uint64_t, 12>    direct {};
	std::array<uint64_t, 5>     indirect {};
};

class Ps5OuterFile {
public:
	Ps5OuterFile(std::shared_ptr<ByteSource> src,
	             uint64_t                    block_size,
	             std::vector<uint64_t>       blocks,
	             uint64_t                    size)
	    : m_src(std::move(src)), m_block_size(block_size), m_blocks(std::move(blocks)),
	      m_size(size) {}

	[[nodiscard]] uint64_t Size() const noexcept { return m_size; }

	bool ReadAt(uint64_t offset, void* out, size_t len, std::string& out_error) const {
		if (offset > m_size || len > m_size - offset) {
			out_error = "PS5 outer file read past end";
			return false;
		}
		auto*  dst  = static_cast<uint8_t*>(out);
		size_t done = 0;
		while (done < len) {
			const uint64_t pos    = offset + done;
			const size_t   index  = static_cast<size_t>(pos / m_block_size);
			const uint64_t within = pos % m_block_size;
			if (index >= m_blocks.size()) {
				out_error = "PS5 outer file block index out of range";
				return false;
			}
			size_t run = 1;
			while (index + run < m_blocks.size() &&
			       m_blocks[index + run] == m_blocks[index] + run) {
				++run;
			}
			const uint64_t avail = static_cast<uint64_t>(run) * m_block_size - within;
			const size_t   n     = static_cast<size_t>(std::min<uint64_t>(avail, len - done));
			const uint64_t at    = m_blocks[index] * m_block_size + within;
			if (!m_src->ReadAt(at, dst + done, n)) {
				out_error = "Failed reading PS5 outer PFS block";
				return false;
			}
			done += n;
		}
		return true;
	}

	bool ReadAll(std::vector<uint8_t>& out, std::string& out_error) const {
		out.resize(static_cast<size_t>(m_size));
		return m_size == 0u || ReadAt(0u, out.data(), out.size(), out_error);
	}

private:
	std::shared_ptr<ByteSource> m_src;
	uint64_t                    m_block_size = 0;
	std::vector<uint64_t>       m_blocks;
	uint64_t                    m_size = 0;
};

class Ps5OuterPfs {
public:
	static std::unique_ptr<Ps5OuterPfs> Open(std::shared_ptr<ByteSource> src,
	                                         uint64_t                    pfs_offset,
	                                         uint64_t                    pfs_size,
	                                         uint64_t                    superblock_offset,
	                                         std::string&                out_error) {
		uint64_t sb_rel = 0;
		if (superblock_offset >= pfs_offset &&
		    superblock_offset - pfs_offset + 0x400u <= pfs_size) {
			sb_rel = superblock_offset - pfs_offset;
		}
		uint8_t sb_buf[0x400] {};
		if (!src->ReadAt(sb_rel, sb_buf, sizeof(sb_buf))) {
			out_error = "Failed to read PS5 outer PFS superblock";
			return nullptr;
		}
		const uint64_t version           = ReadLe64(sb_buf + 0x00);
		const uint64_t magic             = ReadLe64(sb_buf + 0x08);
		const uint16_t mode              = ReadLe16(sb_buf + 0x1c);
		const uint64_t block_size        = ReadLe32(sb_buf + 0x20);
		const uint64_t dinode_count      = ReadLe64(sb_buf + 0x30);
		const uint64_t dinode_blocks     = std::max<uint64_t>(1u, ReadLe64(sb_buf + 0x40));
		const uint64_t inode_table_block = ReadLe64(sb_buf + 0xd8);
		if (magic != static_cast<uint64_t>(kPfsMagic) || (version != 1u && version != 2u)) {
			out_error = "Invalid PS5 outer PFS superblock magic/version";
			return nullptr;
		}
		if (block_size == 0u || block_size > (1ull << 20) || (block_size % 0x1000u) != 0u) {
			out_error = "Invalid PS5 outer PFS block size";
			return nullptr;
		}
		std::array<uint8_t, 16> seed {};
		std::memcpy(seed.data(), sb_buf + 0x370, 16);
		if ((mode & kPfsModeEncrypt) != 0u && seed != kPlainNoAuthSeed) {
			out_error =
			    "Outer PS5 PFS is encrypted (seed is not PPRPLAIN-NOAUTH!); cannot decrypt without key";
			return nullptr;
		}
		if (dinode_count == 0u || dinode_count > (1ull << 20)) {
			out_error = "Implausible PS5 outer PFS inode count";
			return nullptr;
		}

		const bool          is_signed = (mode & kPfsModeSigned) != 0u;
		const bool          is_64bit  = (mode & kPfsMode64Bit) != 0u;
		const Ps5InodeLayout layout = [&]() noexcept -> Ps5InodeLayout {
			if (is_signed && is_64bit) return {0x310u, 0x68u, 40u, 32u, true};
			if (is_signed && !is_64bit) return {0x2c8u, 0x64u, 36u, 32u, false};
			if (!is_signed && is_64bit) return {0x100u, 0x68u, 8u, 0u, true};
			return {0xa8u, 0x64u, 4u, 0u, false};
		}();

		const size_t per_block = static_cast<size_t>(block_size) / layout.size;
		if (per_block == 0u || dinode_blocks > pfs_size / block_size ||
		    inode_table_block > pfs_size / block_size) {
			out_error = "PS5 outer PFS inode table geometry invalid";
			return nullptr;
		}
		const uint64_t table_len = dinode_blocks * block_size;
		const uint64_t table_at  = inode_table_block * block_size;
		if (table_at + table_len > pfs_size) {
			out_error = "PS5 outer PFS inode table exceeds segment bounds";
			return nullptr;
		}
		std::vector<uint8_t> table(static_cast<size_t>(table_len));
		if (!src->ReadAt(table_at, table.data(), table.size())) {
			out_error = "Failed reading PS5 outer PFS inode table";
			return nullptr;
		}
		std::vector<Ps5OuterInode> inodes;
		inodes.reserve(static_cast<size_t>(dinode_count));
		for (size_t i = 0; i < static_cast<size_t>(dinode_count); ++i) {
			const size_t at = (i / per_block) * static_cast<size_t>(block_size) +
			                  (i % per_block) * layout.size;
			if (at + layout.size > table.size()) {
				out_error = "PS5 outer PFS inode table truncated";
				return nullptr;
			}
			const uint8_t* rec  = table.data() + at;
			const uint8_t* ptrs = rec + layout.first_ptr;
			Ps5OuterInode  ino {};
			ino.size = ReadLe64(rec + 8u);
			for (size_t k = 0; k < 12u; ++k) {
				ino.direct[k] = layout.ReadPtr(ptrs, k);
			}
			for (size_t k = 0; k < 5u; ++k) {
				ino.indirect[k] = layout.ReadPtr(ptrs, 12u + k);
			}
			inodes.push_back(ino);
		}
		return std::unique_ptr<Ps5OuterPfs>(new Ps5OuterPfs(
		    std::move(src), pfs_size, block_size, layout, std::move(inodes)));
	}

	std::unique_ptr<Ps5OuterFile> OpenFile(uint32_t ino_idx, std::string& out_error) const {
		if (ino_idx >= m_inodes.size()) {
			out_error = "PS5 outer inode out of range";
			return nullptr;
		}
		const auto& ino = m_inodes[ino_idx];
		if (ino.size > m_pfs_size) {
			out_error = "PS5 outer inode size exceeds PFS segment";
			return nullptr;
		}
		const uint64_t        count = (ino.size + m_block_size - 1u) / m_block_size;
		std::vector<uint64_t> blocks;
		blocks.reserve(static_cast<size_t>(count));
		for (size_t k = 0; k < 12u && blocks.size() < count; ++k) {
			blocks.push_back(ino.direct[k]);
		}
		const size_t per_blk = static_cast<size_t>(m_block_size) / m_layout.stride;
		size_t       level   = 0;
		while (blocks.size() < count) {
			if (level >= 3u) {
				out_error = "PS5 outer file exceeds triple-indirect limit";
				return nullptr;
			}
			if (!WalkIndirect(ino.indirect[level], level, count, per_blk, blocks, out_error)) {
				return nullptr;
			}
			++level;
		}
		return std::make_unique<Ps5OuterFile>(m_src, m_block_size, std::move(blocks), ino.size);
	}

	bool FindFile(std::string_view name, uint32_t& out_ino, std::string& out_error) const {
		std::vector<uint32_t> stack = {0u};
		std::vector<bool>     seen(m_inodes.size(), false);
		while (!stack.empty()) {
			const uint32_t dir = stack.back();
			stack.pop_back();
			if (dir >= m_inodes.size() || seen[dir]) {
				continue;
			}
			seen[dir] = true;
			auto file = OpenFile(dir, out_error);
			if (!file) {
				return false;
			}
			if (file->Size() > kMaxOuterDirSize) {
				out_error = "PS5 outer directory exceeds max size";
				return false;
			}
			std::vector<uint8_t> bytes;
			if (!file->ReadAll(bytes, out_error)) {
				return false;
			}
			size_t pos = 0;
			while (pos < bytes.size()) {
				const size_t rem = bytes.size() - pos;
				if (rem < 16u) {
					break;
				}
				const uint32_t ino_num  = ReadLe32(bytes.data() + pos + 0u);
				const uint32_t kind     = ReadLe32(bytes.data() + pos + 4u);
				const uint32_t name_len = ReadLe32(bytes.data() + pos + 8u);
				const uint32_t rec_size = ReadLe32(bytes.data() + pos + 12u);
				if (ino_num == 0u && kind == 0u && name_len == 0u && rec_size == 0u) {
					const size_t skip = static_cast<size_t>(m_block_size) -
					                    (pos % static_cast<size_t>(m_block_size));
					pos += std::min(skip, rem);
					continue;
				}
				if (rec_size < 16u || rec_size > 0x1000u || name_len > rec_size - 16u ||
				    rec_size > rem) {
					out_error = "Corrupt PS5 outer directory record";
					return false;
				}
				const char*            nm_ptr = reinterpret_cast<const char*>(bytes.data() + pos + 16u);
				const size_t           nm_len = strnlen(nm_ptr, name_len);
				const std::string_view nm(nm_ptr, nm_len);
				pos += rec_size;
				if (ino_num >= m_inodes.size() || nm == "." || nm == "..") {
					continue;
				}
				if (kind == kPs5DirentDir) {
					stack.push_back(ino_num);
				} else if (kind == kPs5DirentFile && nm == name) {
					out_ino = ino_num;
					return true;
				}
			}
		}
		out_error = "Missing required file in PS5 outer PFS: " + std::string(name);
		return false;
	}

private:
	Ps5OuterPfs(std::shared_ptr<ByteSource> src,
	            uint64_t                    pfs_size,
	            uint64_t                    block_size,
	            Ps5InodeLayout              layout,
	            std::vector<Ps5OuterInode>  inodes)
	    : m_src(std::move(src)), m_pfs_size(pfs_size), m_block_size(block_size),
	      m_layout(layout), m_inodes(std::move(inodes)) {}

	bool WalkIndirect(uint64_t               block,
	                  size_t                 depth,
	                  uint64_t               count,
	                  size_t                 per_blk,
	                  std::vector<uint64_t>& out,
	                  std::string&           out_error) const {
		if (block > m_pfs_size / m_block_size || block * m_block_size + m_block_size > m_pfs_size) {
			out_error = "PS5 outer indirect block pointer out of range";
			return false;
		}
		std::vector<uint8_t> buf(static_cast<size_t>(m_block_size));
		if (!m_src->ReadAt(block * m_block_size, buf.data(), buf.size())) {
			out_error = "Failed reading PS5 outer indirect block";
			return false;
		}
		for (size_t i = 0; i < per_blk && out.size() < count; ++i) {
			const uint64_t ptr = m_layout.ReadPtr(buf.data(), i);
			if (depth == 0u) {
				out.push_back(ptr);
			} else if (!WalkIndirect(ptr, depth - 1u, count, per_blk, out, out_error)) {
				return false;
			}
		}
		return true;
	}

	std::shared_ptr<ByteSource> m_src;
	uint64_t                    m_pfs_size   = 0;
	uint64_t                    m_block_size = 0;
	Ps5InodeLayout              m_layout {};
	std::vector<Ps5OuterInode>  m_inodes;
};

struct NapsCounts {
	uint32_t num_files        = 0;
	uint8_t  compression_type = 0;
	uint32_t num_keys         = 0;
	uint32_t num_shuffle      = 0;
	uint32_t num_ublocks      = 0;
	uint32_t num_outer_blocks = 0;
	uint32_t num_cblock_info  = 0;
};

struct NapsFileOffset {
	uint8_t  kind                = 0;
	uint64_t uncompressed_offset = 0;
};

struct NapsCblock {
	bool     is_run_base        = false;
	uint32_t coffset_mod        = 0;
	uint32_t clen_even_minus1   = 0;
	uint8_t  kde_predictor      = 0;
	uint8_t  shuffle_idx        = 0;
	uint32_t coffset_start_256k = 0;

	[[nodiscard]] uint32_t EvenComp() const noexcept { return clen_even_minus1 + 1u; }
	[[nodiscard]] bool     Kraken() const noexcept { return (kde_predictor & 2u) != 0u; }
	[[nodiscard]] uint32_t KrakenFlags() const noexcept {
		return static_cast<uint32_t>(kde_predictor) | (static_cast<uint32_t>(shuffle_idx) << 4u);
	}
	[[nodiscard]] uint64_t RunOnDisk(const NapsCblock& first) const noexcept {
		return static_cast<uint64_t>(coffset_start_256k / 2u) * kNapsUblockSize +
		       static_cast<uint64_t>(first.coffset_mod);
	}
};

struct NapsUBlock {
	uint64_t logical   = 0;
	uint64_t on_disk   = 0;
	uint32_t comp      = 0;
	uint32_t uncomp    = 0;
	uint32_t even_comp = 0;
	uint32_t flags     = 0;
	bool     kraken    = false;
};

struct Ps5InnerFile {
	std::filesystem::path rel_path;
	uint64_t              logical = 0;
	uint64_t              size    = 0;
};

class Ps5NapsImage {
public:
	static std::unique_ptr<Ps5NapsImage> Open(std::unique_ptr<Ps5OuterFile> image,
	                                          const std::vector<uint8_t>&   naps_blob,
	                                          std::string&                  out_error) {
		if (naps_blob.size() < 16u) {
			out_error = "NAPS layout header truncated";
			return nullptr;
		}
		const uint64_t w0 = ReadLe64(naps_blob.data() + 0u);
		const uint64_t w1 = ReadLe64(naps_blob.data() + 8u);
		NapsCounts     counts {};
		counts.num_files        = static_cast<uint32_t>(w0 & 0xFFFFFFu) + 1u;
		counts.compression_type = static_cast<uint8_t>((w0 >> 24u) & 3u);
		counts.num_keys         = static_cast<uint32_t>((w0 >> 26u) & 3u) + 1u;
		counts.num_shuffle      = static_cast<uint32_t>((w0 >> 28u) & 0xFu);
		counts.num_ublocks      = static_cast<uint32_t>((w0 >> 32u) & 0xFFFFFFu);
		counts.num_outer_blocks = static_cast<uint32_t>(w1 & 0xFFFFFFu);
		counts.num_cblock_info  = static_cast<uint32_t>((w1 >> 24u) & 0xFFFFFFu) + 2u;

		const uint64_t header_skip =
		    16ull + static_cast<uint64_t>(counts.num_outer_blocks) * 8ull +
		    static_cast<uint64_t>(counts.num_shuffle) * 8ull;
		const uint64_t fidx_bytes = static_cast<uint64_t>(counts.num_files) * 6ull;
		if (header_skip > naps_blob.size() || fidx_bytes > naps_blob.size() - header_skip) {
			out_error = "NAPS file-offset table truncated";
			return nullptr;
		}
		std::vector<NapsFileOffset> file_offsets;
		file_offsets.reserve(counts.num_files);
		for (uint32_t i = 0; i < counts.num_files; ++i) {
			const uint8_t* rec = naps_blob.data() + static_cast<size_t>(header_skip) + i * 6u;
			uint64_t       off = 0;
			for (size_t b = 0; b < 5u; ++b) {
				off |= static_cast<uint64_t>(rec[b]) << (8u * b);
			}
			file_offsets.push_back({rec[5], off});
		}

		const uint64_t fidx_end = header_skip + fidx_bytes;
		const uint64_t num_u2c  = static_cast<uint64_t>((counts.num_ublocks + 8u) >> 3u);
		auto           align_up = [](uint64_t v, uint64_t a) noexcept {
            return ((v + a - 1u) / a) * a;
        };
		std::vector<uint64_t> starts;
		starts.push_back(align_up(align_up(fidx_end, 16u) + num_u2c * 10u, 8u));
		const uint64_t s2 = align_up(fidx_end + num_u2c * 10u, 8u);
		if (s2 != starts[0]) {
			starts.push_back(s2);
		}

		std::stable_sort(file_offsets.begin(), file_offsets.end(),
		                 [](const NapsFileOffset& a, const NapsFileOffset& b) {
			                 return a.uncompressed_offset < b.uncompressed_offset;
		                 });
		uint64_t mount = 0;
		for (auto it = file_offsets.rbegin(); it != file_offsets.rend(); ++it) {
			if (it->kind == kNapsKindMount) {
				mount = it->uncompressed_offset;
				break;
			}
		}
		if (mount == 0u) {
			out_error = "NAPS layout has no mount size entry";
			return nullptr;
		}

		const uint64_t image_size = static_cast<uint64_t>(counts.num_outer_blocks) * kNapsOuterBlockSize;
		auto           decode_cblock = [](const uint8_t* raw) noexcept -> NapsCblock {
            const uint64_t lo          = ReadLe64(raw);
            const uint64_t hi          = raw[8];
            const uint32_t coffset_mod = static_cast<uint32_t>(lo & 0x3FFFFu);
            if (((lo >> 18u) & 1u) == 0u) {
                return NapsCblock {
                    .is_run_base      = false,
                    .coffset_mod      = coffset_mod,
                    .clen_even_minus1 = static_cast<uint32_t>((lo >> 38u) & 0x1FFFFu),
                    .kde_predictor    = static_cast<uint8_t>((lo >> 56u) & 7u),
                    .shuffle_idx      = static_cast<uint8_t>((lo >> 59u) & 0xFu),
                };
            }
            return NapsCblock {
                .is_run_base        = true,
                .coffset_mod        = coffset_mod,
                .coffset_start_256k = static_cast<uint32_t>(((lo >> 49u) & 0x7FFFu) | ((hi & 0x1FFu) << 15u)),
            };
		};

		auto next_boundary = [&](uint64_t cur) noexcept -> uint64_t {
			auto it = std::upper_bound(
			    file_offsets.begin(), file_offsets.end(), cur,
			    [](uint64_t val, const NapsFileOffset& f) { return val < f.uncompressed_offset; });
			return (it != file_offsets.end()) ? std::min(it->uncompressed_offset, mount) : mount;
		};
		auto first_at_or_after = [&](uint64_t off) noexcept -> std::optional<NapsFileOffset> {
			auto it = std::lower_bound(
			    file_offsets.begin(), file_offsets.end(), off,
			    [](const NapsFileOffset& f, uint64_t val) { return f.uncompressed_offset < val; });
			if (it == file_offsets.end()) {
				return std::nullopt;
			}
			return *it;
		};

		std::vector<NapsUBlock> valid_blocks;
		int                     valid_walks = 0;
		for (uint64_t st: starts) {
			const uint64_t table_bytes = static_cast<uint64_t>(counts.num_cblock_info) * 9ull;
			if (st > naps_blob.size() || table_bytes > naps_blob.size() - st) {
				continue;
			}
			std::vector<NapsCblock> recs;
			recs.reserve(counts.num_cblock_info);
			for (uint32_t i = 0; i < counts.num_cblock_info; ++i) {
				recs.push_back(decode_cblock(naps_blob.data() + static_cast<size_t>(st) + i * 9u));
			}
			bool structural_ok = true;
			for (size_t i = 0; i < recs.size(); ++i) {
				if (!recs[i].is_run_base) {
					continue;
				}
				if (i + 1u >= recs.size() || recs[i + 1u].is_run_base ||
				    recs[i].RunOnDisk(recs[i + 1u]) >= image_size) {
					structural_ok = false;
					break;
				}
			}
			if (!structural_ok) {
				continue;
			}

			std::vector<NapsUBlock> out;
			out.reserve(counts.num_ublocks + 16u);
			uint64_t on_disk = 0;
			uint64_t uncomp  = 0;
			bool     walk_ok = true;

			auto append_sparse = [&]() {
				while (uncomp < mount) {
					const auto e = first_at_or_after(uncomp);
					if (!e.has_value() || e->kind != kNapsKindMount ||
					    e->uncompressed_offset != uncomp) {
						return true;
					}
					const uint64_t end = next_boundary(uncomp);
					const uint32_t len = static_cast<uint32_t>(std::min(kNapsUblockSize, end - uncomp));
					if (len == 0u) {
						return false;
					}
					out.push_back({.logical = uncomp, .uncomp = len});
					uncomp += len;
				}
				return true;
			};

			size_t i = 0;
			while (i < recs.size()) {
				const auto& rec = recs[i];
				if (rec.is_run_base) {
					if (i + 1u >= recs.size() || recs[i + 1u].is_run_base) {
						walk_ok = false;
						break;
					}
					on_disk = rec.RunOnDisk(recs[i + 1u]);
					++i;
					continue;
				}
				if (!append_sparse()) {
					walk_ok = false;
					break;
				}
				if (uncomp >= mount || i + 1u >= recs.size()) {
					break;
				}
				const uint64_t file_end = next_boundary(uncomp);
				const uint32_t uncomp_len =
				    static_cast<uint32_t>(std::min(kNapsUblockSize, file_end - uncomp));
				if (uncomp_len == 0u) {
					break;
				}
				int64_t diff = static_cast<int64_t>(recs[i + 1u].coffset_mod) -
				               static_cast<int64_t>(rec.coffset_mod);
				if (diff <= 0) {
					diff += static_cast<int64_t>(kNapsUblockSize);
				}
				const uint32_t comp = static_cast<uint32_t>(diff);
				out.push_back({
				    .logical   = uncomp,
				    .on_disk   = on_disk,
				    .comp      = comp,
				    .uncomp    = uncomp_len,
				    .even_comp = rec.EvenComp(),
				    .flags     = rec.KrakenFlags(),
				    .kraken    = rec.Kraken() || comp != uncomp_len,
				});
				on_disk += comp;
				uncomp += uncomp_len;
				if (uncomp >= mount) {
					break;
				}
				++i;
			}
			if (walk_ok && !append_sparse()) {
				walk_ok = false;
			}
			if (walk_ok && uncomp == mount) {
				valid_blocks = std::move(out);
				++valid_walks;
			}
		}

		if (valid_walks == 0) {
			out_error = "No valid NAPS CblockInfo placement covers mount size";
			return nullptr;
		}
		if (valid_walks > 1) {
			out_error = "Ambiguous NAPS CblockInfo table placements";
			return nullptr;
		}

		uint64_t stored_cnt = 0;
		uint64_t kraken_cnt = 0;
		uint64_t sparse_cnt = 0;
		for (const auto& b: valid_blocks) {
			if (b.comp == 0u) {
				++sparse_cnt;
			} else if (b.kraken) {
				++kraken_cnt;
			} else {
				++stored_cnt;
			}
			if (b.on_disk > image->Size() || b.comp > image->Size() - b.on_disk) {
				out_error = "NAPS block extends past pfs_image.dat";
				return nullptr;
			}
		}

		uint64_t meta_base = 0;
		for (const auto& f: file_offsets) {
			if (f.uncompressed_offset > 0u && f.uncompressed_offset < mount) {
				meta_base = std::max(meta_base, f.uncompressed_offset);
			}
		}

		return std::unique_ptr<Ps5NapsImage>(new Ps5NapsImage(
		    std::move(image), std::move(valid_blocks), mount, meta_base,
		    stored_cnt, kraken_cnt, sparse_cnt));
	}

	[[nodiscard]] uint64_t MountSize() const noexcept { return m_mount; }
	[[nodiscard]] uint64_t StoredBlocks() const noexcept { return m_stored_blocks; }
	[[nodiscard]] uint64_t KrakenBlocks() const noexcept { return m_kraken_blocks; }
	[[nodiscard]] uint64_t SparseBlocks() const noexcept { return m_sparse_blocks; }

	bool ReadAt(uint64_t logical, void* out, size_t size, std::string& out_error) const {
		auto*  dst  = static_cast<uint8_t*>(out);
		size_t done = 0;
		while (done < size) {
			const uint64_t pos = logical + done;
			const size_t   idx = FindBlock(pos);
			if (idx == SIZE_MAX) {
				out_error = "No NAPS block covers logical offset";
				return false;
			}
			if (!EnsureBlockLoaded(idx, out_error)) {
				return false;
			}
			const auto&  blk    = m_blocks[idx];
			const size_t within = static_cast<size_t>(pos - blk.logical);
			const size_t n      = std::min(static_cast<size_t>(blk.uncomp) - within, size - done);
			std::memcpy(dst + done, m_decoded_buf.data() + within, n);
			done += n;
		}
		return true;
	}

	bool StreamToFile(uint64_t                                      logical,
	                  uint64_t                                      size,
	                  const std::filesystem::path&                  dst_path,
	                  std::string&                                  out_error,
	                  const std::function<void(uint64_t delta)>&    on_chunk = {}) const {
		std::error_code ec;
		std::filesystem::create_directories(dst_path.parent_path(), ec);
		if (ec) {
			out_error = "Failed to create directory: " + dst_path.parent_path().string();
			return false;
		}
		std::FILE* out = std::fopen(dst_path.string().c_str(), "wb");
		if (out == nullptr) {
			out_error = "Failed to create output file: " + dst_path.string();
			return false;
		}

		// Small files reuse the single-block cache; large files batch-decode in parallel.
		if (size <= (4ull << 20)) {
			uint64_t       pos = logical;
			const uint64_t end = logical + size;
			while (pos < end) {
				const size_t idx = FindBlock(pos);
				if (idx == SIZE_MAX || !EnsureBlockLoaded(idx, out_error)) {
					std::fclose(out);
					if (out_error.empty()) {
						out_error = "No NAPS block covers logical offset";
					}
					return false;
				}
				const auto&  blk    = m_blocks[idx];
				const size_t within = static_cast<size_t>(pos - blk.logical);
				const size_t n =
				    static_cast<size_t>(std::min<uint64_t>(blk.uncomp - within, end - pos));
				if (std::fwrite(m_decoded_buf.data() + within, 1, n, out) != n) {
					std::fclose(out);
					out_error = "Failed writing output file: " + dst_path.string();
					return false;
				}
				pos += n;
			}
			std::fclose(out);
			return true;
		}

		std::vector<char> io_buf(1u << 20);
		std::setvbuf(out, io_buf.data(), _IOFBF, io_buf.size());

		struct BatchItem {
			size_t               blk_idx      = 0;
			size_t               slice_off    = 0;
			size_t               slice_len    = 0;
			bool                 needs_decode = false;
			std::vector<uint8_t> comp_buf;
			std::vector<uint8_t> dec_buf;
		};

		constexpr size_t       kBatchBlocks    = 64u;
		constexpr uint64_t     kReportInterval = 512ull << 20;
		std::vector<BatchItem> batch(kBatchBlocks);

		uint64_t       pos         = logical;
		const uint64_t end         = logical + size;
		uint64_t       chunk_accum = 0;
		size_t         hint_idx    = FindBlock(pos);

		auto format_kraken_error = [&](const NapsUBlock& blk, const KrakenResult& kres) {
			char msg[256];
			if (kres.status == KrakenStatus::UnsupportedEntropy) {
				std::snprintf(msg, sizeof(msg),
				              "Unsupported Kraken entropy type %u at logical 0x%llx",
				              kres.entropy_type, static_cast<unsigned long long>(blk.logical));
			} else {
				std::snprintf(msg, sizeof(msg),
				              "Malformed Kraken block at logical 0x%llx (comp=%u uncomp=%u flags=0x%x)",
				              static_cast<unsigned long long>(blk.logical), blk.comp, blk.uncomp,
				              blk.flags);
			}
			return std::string(msg);
		};

		while (pos < end) {
			size_t batch_count  = 0;
			size_t decode_count = 0;

			// Phase A: sequential I/O into reusable batch buffers.
			while (pos < end && batch_count < kBatchBlocks) {
				size_t idx = hint_idx;
				if (idx >= m_blocks.size() || pos < m_blocks[idx].logical ||
				    pos >= m_blocks[idx].logical + m_blocks[idx].uncomp) {
					idx = FindBlock(pos);
				}
				if (idx == SIZE_MAX) {
					std::fclose(out);
					out_error = "No NAPS block covers logical offset";
					return false;
				}
				const auto&  blk    = m_blocks[idx];
				const size_t within = static_cast<size_t>(pos - blk.logical);
				const size_t n =
				    static_cast<size_t>(std::min<uint64_t>(blk.uncomp - within, end - pos));

				auto& item     = batch[batch_count++];
				item.blk_idx   = idx;
				item.slice_off = within;
				item.slice_len = n;

				if (blk.comp == 0u) {
					item.dec_buf.resize(blk.uncomp);
					std::fill(item.dec_buf.begin(), item.dec_buf.end(), uint8_t {0});
					item.needs_decode = false;
				} else if (blk.comp == blk.uncomp) {
					item.dec_buf.resize(blk.uncomp);
					if (!m_image->ReadAt(blk.on_disk, item.dec_buf.data(), blk.comp, out_error)) {
						std::fclose(out);
						return false;
					}
					item.needs_decode = false;
				} else {
					item.comp_buf.resize(blk.comp);
					item.dec_buf.resize(blk.uncomp);
					if (!m_image->ReadAt(blk.on_disk, item.comp_buf.data(), blk.comp, out_error)) {
						std::fclose(out);
						return false;
					}
					item.needs_decode = true;
					++decode_count;
				}

				pos += n;
				hint_idx = idx + 1u;
			}

			// Phase B: parallel Kraken CPU decoding across workers.
			if (decode_count > 0u) {
				const size_t hw = std::max<size_t>(1u, std::thread::hardware_concurrency());
				const size_t num_workers =
				    std::min<size_t>(8u, std::min(decode_count, hw));
				if (num_workers <= 1u) {
					for (size_t i = 0; i < batch_count; ++i) {
						auto& item = batch[i];
						if (!item.needs_decode) {
							continue;
						}
						const auto&        blk  = m_blocks[item.blk_idx];
						const KrakenResult kres = DecodeKrakenBlock(item.comp_buf, blk.flags,
						                                            blk.even_comp, item.dec_buf);
						if (kres.status != KrakenStatus::Ok) {
							std::fclose(out);
							out_error = format_kraken_error(blk, kres);
							return false;
						}
					}
				} else {
					std::atomic<size_t> next_idx {0};
					std::atomic<size_t> failed_idx {SIZE_MAX};
					KrakenResult        failed_res {};

					auto worker = [&]() {
						for (;;) {
							if (failed_idx.load(std::memory_order_relaxed) != SIZE_MAX) {
								return;
							}
							const size_t i = next_idx.fetch_add(1u, std::memory_order_relaxed);
							if (i >= batch_count) {
								return;
							}
							auto& item = batch[i];
							if (!item.needs_decode) {
								continue;
							}
							const auto&        blk  = m_blocks[item.blk_idx];
							const KrakenResult kres = DecodeKrakenBlock(
							    item.comp_buf, blk.flags, blk.even_comp, item.dec_buf);
							if (kres.status != KrakenStatus::Ok) {
								size_t expected = SIZE_MAX;
								if (failed_idx.compare_exchange_strong(expected, i)) {
									failed_res = kres;
								}
								return;
							}
						}
					};

					std::vector<std::thread> workers;
					workers.reserve(num_workers);
					for (size_t w = 0; w < num_workers; ++w) {
						workers.emplace_back(worker);
					}
					for (auto& t: workers) {
						t.join();
					}
					const size_t err_i = failed_idx.load(std::memory_order_acquire);
					if (err_i != SIZE_MAX) {
						std::fclose(out);
						out_error = format_kraken_error(m_blocks[batch[err_i].blk_idx], failed_res);
						return false;
					}
				}
			}

			// Phase C: sequential disk write.
			for (size_t i = 0; i < batch_count; ++i) {
				const auto& item = batch[i];
				if (std::fwrite(item.dec_buf.data() + item.slice_off, 1, item.slice_len, out) !=
				    item.slice_len) {
					std::fclose(out);
					out_error = "Failed writing output file: " + dst_path.string();
					return false;
				}
				chunk_accum += item.slice_len;
				if (on_chunk && chunk_accum >= kReportInterval) {
					on_chunk(chunk_accum);
					chunk_accum = 0;
				}
			}
		}

		std::fclose(out);
		return true;
	}

	bool CollectFiles(std::vector<Ps5InnerFile>& out_files, std::string& out_error) const {
		auto first_it = std::lower_bound(
		    m_blocks.begin(), m_blocks.end(), m_meta_base,
		    [](const NapsUBlock& b, uint64_t base) { return (b.logical + b.uncomp) <= base; });
		if (first_it == m_blocks.end()) {
			out_error = "No NAPS blocks cover PS5 inner metadata tail";
			return false;
		}
		const uint64_t tail_base = first_it->logical;
		const uint64_t tail_len  = m_mount - tail_base;
		if (tail_len == 0u || tail_len > kMaxMetaTail) {
			out_error = "PS5 inner metadata tail size out of bounds";
			return false;
		}
		std::vector<uint8_t> tail(static_cast<size_t>(tail_len));
		if (!ReadAt(tail_base, tail.data(), tail.size(), out_error)) {
			return false;
		}

		size_t sb_off = SIZE_MAX;
		for (size_t off = 0; off + 0x40u <= tail.size(); off += 0x10000u) {
			const uint64_t ver   = ReadLe64(tail.data() + off + 0x00);
			const uint64_t magic = ReadLe64(tail.data() + off + 0x08);
			const uint64_t bsz   = ReadLe32(tail.data() + off + 0x20);
			const uint64_t dcnt  = ReadLe64(tail.data() + off + 0x30);
			const uint64_t ndblk = ReadLe64(tail.data() + off + 0x38);
			if (ver == 2u && magic == static_cast<uint64_t>(kPfsMagic) && dcnt > 0u &&
			    ndblk * bsz == m_mount) {
				sb_off = off;
				break;
			}
		}
		if (sb_off == SIZE_MAX) {
			out_error = "Could not locate PS5 inner PFSv2 superblock matching NAPS mount size";
			return false;
		}

		const uint16_t mode       = ReadLe16(tail.data() + sb_off + 0x1c);
		const uint32_t block_size = ReadLe32(tail.data() + sb_off + 0x20);
		const uint64_t count      = ReadLe64(tail.data() + sb_off + 0x30);
		if (block_size == 0u || (block_size % 0x1000u) != 0u || count == 0u || count > 1000000u) {
			out_error = "Invalid PS5 inner PFSv2 superblock geometry";
			return false;
		}
		if ((mode & 0x13u) != 0x10u) {
			out_error = "Unsupported PS5 inner PFSv2 mode (expected flat 0x10 layout)";
			return false;
		}

		struct FlatInode {
			uint16_t mode    = 0;
			uint64_t size    = 0;
			uint64_t logical = 0;
		};
		const size_t           per_block = block_size / kPs5InnerInodeSize;
		const size_t           table_off = sb_off + block_size;
		std::vector<FlatInode> inodes;
		inodes.reserve(static_cast<size_t>(count));
		for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
			const size_t at = table_off + (i / per_block) * block_size +
			                  (i % per_block) * kPs5InnerInodeSize;
			if (at + kPs5InnerInodeSize > tail.size()) {
				out_error = "PS5 inner inode table truncated";
				return false;
			}
			const uint8_t* rec = tail.data() + at;
			inodes.push_back({
			    .mode    = ReadLe16(rec + 0x00),
			    .size    = ReadLe64(rec + 0x08),
			    .logical = ReadLe64(rec + 0x60),
			});
		}

		std::vector<bool> seen(inodes.size(), false);
		auto walk_dir = [&](auto& self, uint32_t ino_idx, const std::filesystem::path& cur_rel,
		                    bool under_uroot, int depth) -> bool {
			if (depth > 64) {
				out_error = "PS5 inner directory nesting exceeds limit";
				return false;
			}
			if (ino_idx >= inodes.size()) {
				out_error = "PS5 inner inode out of range";
				return false;
			}
			if (seen[ino_idx]) {
				return true;
			}
			seen[ino_idx]    = true;
			const auto& dir  = inodes[ino_idx];
			const bool is_dr = (dir.mode & 0xF000u) == 0x4000u;
			if (!is_dr || dir.size == 0u || dir.size > kMaxOuterDirSize) {
				out_error = "Unreadable PS5 inner directory inode";
				return false;
			}
			std::vector<uint8_t>   dir_storage;
			std::span<const uint8_t> dbytes;
			if (dir.logical >= tail_base && dir.logical - tail_base + dir.size <= tail.size()) {
				dbytes = std::span<const uint8_t>(
				    tail.data() + static_cast<size_t>(dir.logical - tail_base),
				    static_cast<size_t>(dir.size));
			} else {
				dir_storage.resize(static_cast<size_t>(dir.size));
				if (!ReadAt(dir.logical, dir_storage.data(), dir_storage.size(), out_error)) {
					return false;
				}
				dbytes = dir_storage;
			}

			size_t pos = 0;
			while (pos + 16u <= dbytes.size()) {
				const uint32_t c_ino    = ReadLe32(dbytes.data() + pos + 0u);
				const uint32_t kind     = ReadLe32(dbytes.data() + pos + 4u);
				const uint32_t name_len = ReadLe32(dbytes.data() + pos + 8u);
				const uint32_t rec_size = ReadLe32(dbytes.data() + pos + 12u);
				if (c_ino == 0u && kind == 0u && name_len == 0u && rec_size == 0u) {
					const size_t skip = block_size - (pos % block_size);
					pos += std::min(skip, dbytes.size() - pos);
					continue;
				}
				if (rec_size < 16u || rec_size > 0x1000u || name_len > rec_size - 16u ||
				    pos + rec_size > dbytes.size()) {
					out_error = "Corrupt PS5 inner directory record";
					return false;
				}
				const char*            nm_ptr = reinterpret_cast<const char*>(dbytes.data() + pos + 16u);
				const size_t           nm_len = strnlen(nm_ptr, name_len);
				const std::string_view nm(nm_ptr, nm_len);
				pos += rec_size;
				if (nm == "." || nm == "..") {
					continue;
				}
				if (nm.empty() || nm.find('/') != std::string_view::npos ||
				    nm.find('\\') != std::string_view::npos ||
				    nm.find(':') != std::string_view::npos) {
					out_error = "Unsafe filename inside PS5 inner PFS: " + std::string(nm);
					return false;
				}
				if (c_ino >= inodes.size()) {
					out_error = "PS5 inner child inode out of range";
					return false;
				}
				const bool is_uroot = cur_rel.empty() && nm == "uroot";
				std::filesystem::path next_rel;
				if (!is_uroot) {
					std::filesystem::path part;
					if (!SanitizeRelativePath(nm, part)) {
						out_error = "Unsafe path component inside PS5 inner PFS: " + std::string(nm);
						return false;
					}
					next_rel = cur_rel.empty() ? part : (cur_rel / part);
				}
				if (kind == kPs5DirentDir) {
					if (!self(self, c_ino, next_rel, under_uroot || is_uroot, depth + 1)) {
						return false;
					}
				} else if (kind == kPs5DirentFile && under_uroot) {
					const auto& f = inodes[c_ino];
					if (f.logical > m_mount || f.size > m_mount - f.logical) {
						out_error = "PS5 inner file runs past image mount size";
						return false;
					}
					out_files.push_back({
					    .rel_path = next_rel,
					    .logical  = f.logical,
					    .size     = f.size,
					});
				}
			}
			return true;
		};

		if (!walk_dir(walk_dir, 0u, {}, false, 0)) {
			return false;
		}
		if (out_files.empty()) {
			out_error = "PS5 inner PFS tree has no files under uroot";
			return false;
		}
		return true;
	}

private:
	Ps5NapsImage(std::unique_ptr<Ps5OuterFile> image,
	             std::vector<NapsUBlock>       blocks,
	             uint64_t                      mount,
	             uint64_t                      meta_base,
	             uint64_t                      stored_blocks,
	             uint64_t                      kraken_blocks,
	             uint64_t                      sparse_blocks)
	    : m_image(std::move(image)), m_blocks(std::move(blocks)), m_mount(mount),
	      m_meta_base(meta_base), m_stored_blocks(stored_blocks),
	      m_kraken_blocks(kraken_blocks), m_sparse_blocks(sparse_blocks) {}

	[[nodiscard]] size_t FindBlock(uint64_t logical) const noexcept {
		auto it = std::upper_bound(
		    m_blocks.begin(), m_blocks.end(), logical,
		    [](uint64_t pos, const NapsUBlock& b) { return pos < (b.logical + b.uncomp); });
		if (it == m_blocks.end() || it->logical > logical) {
			return SIZE_MAX;
		}
		return static_cast<size_t>(it - m_blocks.begin());
	}

	bool EnsureBlockLoaded(size_t idx, std::string& out_error) const {
		if (m_cached_idx == idx) {
			return true;
		}
		m_cached_idx = SIZE_MAX;
		const auto& blk = m_blocks[idx];
		m_decoded_buf.resize(blk.uncomp);
		if (blk.comp == 0u) {
			std::fill(m_decoded_buf.begin(), m_decoded_buf.end(), uint8_t {0});
		} else if (blk.comp == blk.uncomp) {
			if (!m_image->ReadAt(blk.on_disk, m_decoded_buf.data(), blk.comp, out_error)) {
				return false;
			}
		} else {
			m_comp_buf.resize(blk.comp);
			if (!m_image->ReadAt(blk.on_disk, m_comp_buf.data(), blk.comp, out_error)) {
				return false;
			}
			const KrakenResult kres =
			    DecodeKrakenBlock(m_comp_buf, blk.flags, blk.even_comp, m_decoded_buf);
			if (kres.status != KrakenStatus::Ok) {
				char msg[256];
				if (kres.status == KrakenStatus::UnsupportedEntropy) {
					std::snprintf(msg, sizeof(msg),
					              "Unsupported Kraken entropy type %u at logical 0x%llx",
					              kres.entropy_type, static_cast<unsigned long long>(blk.logical));
				} else {
					std::snprintf(msg, sizeof(msg),
					              "Malformed Kraken block at logical 0x%llx (comp=%u uncomp=%u flags=0x%x)",
					              static_cast<unsigned long long>(blk.logical), blk.comp, blk.uncomp,
					              blk.flags);
				}
				out_error = msg;
				return false;
			}
		}
		m_cached_idx = idx;
		return true;
	}

	std::unique_ptr<Ps5OuterFile> m_image;
	std::vector<NapsUBlock>       m_blocks;
	uint64_t                      m_mount         = 0;
	uint64_t                      m_meta_base     = 0;
	uint64_t                      m_stored_blocks = 0;
	uint64_t                      m_kraken_blocks = 0;
	uint64_t                      m_sparse_blocks = 0;

	mutable size_t               m_cached_idx = SIZE_MAX;
	mutable std::vector<uint8_t> m_decoded_buf;
	mutable std::vector<uint8_t> m_comp_buf;
};

struct ParsedPs5Fih {
	PkgInfo                        info;
	Ps5FihHeader                   fih {};
	std::vector<Ps5CntExtractItem> cnt_files;
	std::FILE*                     file      = nullptr;
	uint64_t                       file_size = 0;

	~ParsedPs5Fih() {
		if (file != nullptr) {
			std::fclose(file);
		}
	}
};

bool ParsePs5FihPkg(const std::filesystem::path& pkg_path,
                    ParsedPs5Fih&                out_pkg,
                    std::string&                 out_error) {
	std::FILE* file = std::fopen(pkg_path.string().c_str(), "rb");
	if (file == nullptr) {
		out_error = "Failed to open PKG file: " + pkg_path.string();
		return false;
	}
	out_pkg.file = file;
#if defined(_WIN32)
	_fseeki64(file, 0, SEEK_END);
	out_pkg.file_size = static_cast<uint64_t>(_ftelli64(file));
	_fseeki64(file, 0, SEEK_SET);
#else
	fseeko(file, 0, SEEK_END);
	out_pkg.file_size = static_cast<uint64_t>(ftello(file));
	fseeko(file, 0, SEEK_SET);
#endif
	if (out_pkg.file_size < 0x100u) {
		out_error = "File too small to be a PS5 FIH package";
		return false;
	}
	uint8_t hdr[0x100] {};
	if (std::fread(hdr, 1, sizeof(hdr), file) != sizeof(hdr)) {
		out_error = "Failed reading PS5 FIH header";
		return false;
	}
	if (ReadBe32(hdr) != kFihMagic) {
		out_error = "Invalid PS5 FIH magic";
		return false;
	}
	if (hdr[5] != 0u) {
		char msg[160];
		std::snprintf(msg, sizeof(msg),
		              "Retail-signed PS5 FIH package (signed byte 0x%02x); only debug/fake-signed PS5 packages are readable",
		              hdr[5]);
		out_error = msg;
		return false;
	}

	out_pkg.fih.format_version    = ReadLe16(hdr + 0x06);
	out_pkg.fih.pfs_offset        = ReadLe64(hdr + 0x10);
	out_pkg.fih.pfs_size          = ReadLe64(hdr + 0x18);
	out_pkg.fih.superblock_offset = ReadLe64(hdr + 0x20);
	out_pkg.fih.cnt_offset        = ReadLe64(hdr + 0x58);

	if (out_pkg.fih.pfs_offset == 0u || out_pkg.fih.pfs_size > out_pkg.file_size ||
	    out_pkg.fih.pfs_offset > out_pkg.file_size - out_pkg.fih.pfs_size) {
		out_error = "PS5 FIH PFS segment out of package bounds";
		return false;
	}
	if (out_pkg.fih.cnt_offset >= out_pkg.file_size ||
	    out_pkg.file_size - out_pkg.fih.cnt_offset < 0x80u) {
		out_error = "PS5 FIH embedded CNT offset out of bounds";
		return false;
	}

	out_pkg.info.is_ps5_fih         = true;
	out_pkg.info.is_fpkg            = false;
	out_pkg.info.fih_format_version = out_pkg.fih.format_version;
	out_pkg.info.pfs_image_offset   = out_pkg.fih.pfs_offset;
	out_pkg.info.pfs_image_size     = out_pkg.fih.pfs_size;
	out_pkg.info.package_size       = out_pkg.file_size;
	out_pkg.info.passcode_valid     = true;

	FileSliceSource full_src(file, 0u, out_pkg.file_size);
	uint8_t         cnt_hdr[0x80] {};
	if (!full_src.ReadAt(out_pkg.fih.cnt_offset, cnt_hdr, sizeof(cnt_hdr)) ||
	    ReadBe32(cnt_hdr) != kCntMagic) {
		out_error = "Missing embedded \\x7FCNT container in PS5 FIH package";
		return false;
	}

	out_pkg.info.flags        = ReadBe32(cnt_hdr + 0x04);
	const uint32_t entries    = ReadBe32(cnt_hdr + 0x10);
	const uint32_t tbl_off    = ReadBe32(cnt_hdr + 0x18);
	const uint64_t body_off   = ReadBe64(cnt_hdr + 0x20);
	const uint64_t body_sz    = ReadBe64(cnt_hdr + 0x28);
	const size_t   cid_len    = strnlen(reinterpret_cast<const char*>(cnt_hdr + 0x40), 0x24u);
	out_pkg.info.content_id.assign(reinterpret_cast<const char*>(cnt_hdr + 0x40), cid_len);
	out_pkg.info.drm_type     = ReadBe32(cnt_hdr + 0x70);
	out_pkg.info.content_type = ReadBe32(cnt_hdr + 0x74);
	out_pkg.info.content_flags = ReadBe32(cnt_hdr + 0x78);

	if (entries == 0u || entries > 4096u) {
		out_error = "PS5 embedded CNT entry count out of range";
		return false;
	}
	const uint64_t cnt_size = body_off + body_sz;
	if (cnt_size < body_off ||
	    out_pkg.fih.cnt_offset + tbl_off + static_cast<uint64_t>(entries) * 0x20u >
	        out_pkg.file_size) {
		out_error = "PS5 embedded CNT entry table out of bounds";
		return false;
	}

	std::vector<uint8_t> table_buf(static_cast<size_t>(entries) * 0x20u);
	if (!full_src.ReadAt(out_pkg.fih.cnt_offset + tbl_off, table_buf.data(), table_buf.size())) {
		out_error = "Failed reading PS5 embedded CNT entry table";
		return false;
	}

	struct CntEntryRaw {
		uint32_t id        = 0;
		uint32_t name_off  = 0;
		uint32_t flags1    = 0;
		uint32_t flags2    = 0;
		uint32_t offset    = 0;
		uint32_t size      = 0;
		bool     encrypted = false;
	};
	std::vector<CntEntryRaw> raw_entries;
	raw_entries.reserve(entries);
	const CntEntryRaw* names_entry = nullptr;
	for (uint32_t i = 0; i < entries; ++i) {
		const uint8_t* p = table_buf.data() + i * 0x20u;
		CntEntryRaw    r {};
		r.id        = ReadBe32(p + 0x00);
		r.name_off  = ReadBe32(p + 0x04);
		r.flags1    = ReadBe32(p + 0x08);
		r.flags2    = ReadBe32(p + 0x0c);
		r.offset    = ReadBe32(p + 0x10);
		r.size      = ReadBe32(p + 0x14);
		r.encrypted = (r.flags1 & 0x80000000u) != 0u;
		raw_entries.push_back(r);
		if (r.id == kEntryNames && !r.encrypted) {
			names_entry = &raw_entries.back();
		}
	}

	std::vector<uint8_t> names_buf;
	if (names_entry != nullptr && names_entry->size > 0u &&
	    static_cast<uint64_t>(names_entry->offset) + names_entry->size <= cnt_size &&
	    out_pkg.fih.cnt_offset + names_entry->offset + names_entry->size <= out_pkg.file_size) {
		names_buf.resize(names_entry->size);
		(void)full_src.ReadAt(out_pkg.fih.cnt_offset + names_entry->offset, names_buf.data(),
		                      names_buf.size());
	}

	for (const auto& r: raw_entries) {
		std::string name;
		if (!names_buf.empty() && r.name_off != 0u && r.name_off < names_buf.size()) {
			const char*  start = reinterpret_cast<const char*>(names_buf.data() + r.name_off);
			const size_t len   = strnlen(start, names_buf.size() - r.name_off);
			name.assign(start, len);
		}
		if (name.empty()) {
			name = DefaultPs5CntName(r.id);
		}
		while (!name.empty() && (name.front() == '/' || name.front() == '\\')) {
			name.erase(name.begin());
		}
		if (name.starts_with("sce_sys/")) {
			name.erase(0, 8);
		}

		out_pkg.info.entries.push_back({
		    .id        = r.id,
		    .flags1    = r.flags1,
		    .flags2    = r.flags2,
		    .offset    = r.offset,
		    .size      = r.size,
		    .name      = name,
		    .encrypted = r.encrypted,
		});

		if (r.size == 0u || r.encrypted || name.empty() ||
		    (name == "unnamed.bin" && r.id != 0x040au)) {
			continue;
		}
		std::filesystem::path rel_path;
		if (!SanitizeRelativePath(name, rel_path)) {
			out_error = "Unsafe CNT entry path in PS5 FIH package: " + name;
			return false;
		}
		if (static_cast<uint64_t>(r.offset) + r.size > cnt_size ||
		    out_pkg.fih.cnt_offset + r.offset + r.size > out_pkg.file_size) {
			out_error = "PS5 CNT entry extends past container bounds: " + name;
			return false;
		}
		out_pkg.cnt_files.push_back({
		    .rel_path    = std::move(rel_path),
		    .file_offset = out_pkg.fih.cnt_offset + r.offset,
		    .size        = r.size,
		});
	}
	return true;
}

std::unique_ptr<Ps5NapsImage> OpenPs5InnerImage(ParsedPs5Fih& parsed, std::string& out_error) {
	auto outer_src = std::make_shared<FileSliceSource>(parsed.file, parsed.fih.pfs_offset,
	                                                   parsed.fih.pfs_size);
	auto outer = Ps5OuterPfs::Open(outer_src, parsed.fih.pfs_offset, parsed.fih.pfs_size,
	                               parsed.fih.superblock_offset, out_error);
	if (!outer) {
		return nullptr;
	}
	uint32_t image_ino = 0;
	uint32_t naps_ino  = 0;
	if (!outer->FindFile("pfs_image.dat", image_ino, out_error) ||
	    !outer->FindFile("naps_pkg_layout.dat", naps_ino, out_error)) {
		return nullptr;
	}
	auto naps_file = outer->OpenFile(naps_ino, out_error);
	if (!naps_file) {
		return nullptr;
	}
	if (naps_file->Size() > kMaxNapsSize) {
		out_error = "naps_pkg_layout.dat exceeds maximum supported size";
		return nullptr;
	}
	std::vector<uint8_t> naps_blob;
	if (!naps_file->ReadAll(naps_blob, out_error)) {
		return nullptr;
	}
	auto image_file = outer->OpenFile(image_ino, out_error);
	if (!image_file) {
		return nullptr;
	}
	auto inner = Ps5NapsImage::Open(std::move(image_file), naps_blob, out_error);
	if (!inner) {
		return nullptr;
	}
	parsed.info.pfs_logical_size  = inner->MountSize();
	parsed.info.pfs_stored_blocks = inner->StoredBlocks();
	parsed.info.pfs_kraken_blocks = inner->KrakenBlocks();
	parsed.info.pfs_sparse_blocks = inner->SparseBlocks();
	parsed.info.pfs_compressed    = (inner->KrakenBlocks() > 0u);
	return inner;
}

bool PeekPkgMagic(const std::filesystem::path& pkg_path, uint32_t& out_magic, std::string& out_error) {
	std::FILE* file = std::fopen(pkg_path.string().c_str(), "rb");
	if (file == nullptr) {
		out_error = "Failed to open PKG file: " + pkg_path.string();
		return false;
	}
	uint8_t magic[4] {};
	const size_t n = std::fread(magic, 1, sizeof(magic), file);
	std::fclose(file);
	if (n < sizeof(magic)) {
		out_error = "File too small to be a PS4/PS5 PKG";
		return false;
	}
	out_magic = ReadBe32(magic);
	return true;
}

} // namespace

PkgUnpackResult InspectPkg(const std::filesystem::path& pkg_path, std::string_view passcode) {
	PkgUnpackResult res {};
	uint32_t        magic = 0;
	if (!PeekPkgMagic(pkg_path, magic, res.error)) {
		return res;
	}

	if (magic == kFihMagic) {
		ParsedPs5Fih parsed;
		if (!ParsePs5FihPkg(pkg_path, parsed, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}
		auto inner = OpenPs5InnerImage(parsed, res.error);
		if (!inner) {
			res.info = std::move(parsed.info);
			return res;
		}
		std::vector<Ps5InnerFile> files;
		if (!inner->CollectFiles(files, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}
		for (const auto& f: files) {
			const std::string p = f.rel_path.generic_string();
			parsed.info.pfs_files.push_back(p);
			parsed.info.pfs_file_entries.push_back({.path = p, .size = f.size});
		}
		res.info    = std::move(parsed.info);
		res.success = true;
		return res;
	}

	ParsedPkg parsed;
	if (!ParsePkgFile(pkg_path, passcode, parsed, res.error)) {
		return res;
	}
	std::unique_ptr<PfsVolume> vol;
	if (parsed.info.pfs_image_size > 0 && (!parsed.info.pfs_encrypted || parsed.ekpfs.has_value())) {
		if (OpenInnerAndOuterPfs(parsed, vol, res.error) && vol != nullptr) {
			std::vector<PfsVolume::FileEntry> files;
			if (vol->ListFiles({}, files, res.error)) {
				uint64_t logical_sum = 0;
				for (const auto& f: files) {
					const std::string p = f.rel_path.generic_string();
					parsed.info.pfs_files.push_back(p);
					parsed.info.pfs_file_entries.push_back({.path = p, .size = f.size});
					logical_sum += f.size;
				}
				parsed.info.pfs_logical_size = logical_sum;
			}
		} else if (!res.error.empty()) {
			res.info = std::move(parsed.info);
			return res;
		}
	}
	res.info    = std::move(parsed.info);
	res.success = true;
	return res;
}

PkgUnpackResult UnpackPkg(const std::filesystem::path& pkg_path,
                          const std::filesystem::path& output_dir,
                          const PkgUnpackOptions&      options) {
	if (options.list_only) {
		return InspectPkg(pkg_path, options.passcode);
	}

	PkgUnpackResult res {};
	uint32_t        magic = 0;
	if (!PeekPkgMagic(pkg_path, magic, res.error)) {
		return res;
	}

	if (magic == kFihMagic) {
		ParsedPs5Fih parsed;
		if (!ParsePs5FihPkg(pkg_path, parsed, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}

		std::unique_ptr<Ps5NapsImage> inner;
		std::vector<Ps5InnerFile>     all_files;
		std::vector<Ps5InnerFile>     selected_files;
		if (options.extract_pfs) {
			inner = OpenPs5InnerImage(parsed, res.error);
			if (!inner || !inner->CollectFiles(all_files, res.error)) {
				res.info = std::move(parsed.info);
				return res;
			}
			for (const auto& f: all_files) {
				const std::string p = f.rel_path.generic_string();
				parsed.info.pfs_files.push_back(p);
				parsed.info.pfs_file_entries.push_back({.path = p, .size = f.size});
				if (MatchesSelection(p, options)) {
					selected_files.push_back(f);
				}
			}
			if (!options.only_prefixes.empty() && selected_files.empty()) {
				res.error = "No PFS files matched --only filter";
				res.info  = std::move(parsed.info);
				return res;
			}
		}

		std::error_code ec;
		std::filesystem::create_directories(output_dir, ec);
		if (ec) {
			res.error = "Failed to create output directory: " + output_dir.string();
			res.info  = std::move(parsed.info);
			return res;
		}

		uint64_t total_bytes = 0;
		if (options.extract_sc0) {
			for (const auto& c: parsed.cnt_files) {
				total_bytes += c.size;
			}
		}
		for (const auto& f: selected_files) {
			total_bytes += f.size;
		}
		if (!CheckAvailableSpace(output_dir, total_bytes, options.check_free_space, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}

		if (options.extract_sc0) {
			FileSliceSource full_src(parsed.file, 0u, parsed.file_size);
			for (const auto& c: parsed.cnt_files) {
				const auto dst_path = output_dir / "sce_sys" / c.rel_path;
				std::filesystem::create_directories(dst_path.parent_path(), ec);
				std::vector<uint8_t> data(c.size);
				if (!full_src.ReadAt(c.file_offset, data.data(), data.size())) {
					res.error = "Failed reading PS5 CNT entry: " + c.rel_path.generic_string();
					res.info  = std::move(parsed.info);
					return res;
				}
				std::FILE* out = std::fopen(dst_path.string().c_str(), "wb");
				if (out == nullptr ||
				    (!data.empty() && std::fwrite(data.data(), 1, data.size(), out) != data.size())) {
					if (out != nullptr) {
						std::fclose(out);
					}
					res.error = "Failed writing SC0 file: " + dst_path.string();
					res.info  = std::move(parsed.info);
					return res;
				}
				std::fclose(out);
				res.files_extracted++;
				res.bytes_extracted += data.size();
				if (options.on_progress) {
					options.on_progress(dst_path.lexically_relative(output_dir).generic_string(),
					                    res.bytes_extracted, total_bytes);
				}
			}
		}

		if (options.extract_pfs && inner != nullptr) {
			for (const auto& f: selected_files) {
				const auto dst_path          = output_dir / f.rel_path;
				uint64_t   reported_for_file = 0;
				auto       on_chunk          = [&](uint64_t delta) {
                    reported_for_file += delta;
                    if (options.on_progress) {
                        options.on_progress(f.rel_path.generic_string(),
                                            res.bytes_extracted + reported_for_file,
                                            total_bytes);
                    }
				};
				if (!inner->StreamToFile(f.logical, f.size, dst_path, res.error, on_chunk)) {
					res.info = std::move(parsed.info);
					return res;
				}
				res.files_extracted++;
				res.bytes_extracted += f.size;
				if (options.on_progress && reported_for_file < f.size) {
					options.on_progress(f.rel_path.generic_string(), res.bytes_extracted,
					                    total_bytes);
				}
			}
		}

		res.info    = std::move(parsed.info);
		res.success = true;
		return res;
	}

	ParsedPkg parsed;
	if (!ParsePkgFile(pkg_path, options.passcode, parsed, res.error)) {
		return res;
	}

	std::error_code ec;
	std::filesystem::create_directories(output_dir, ec);
	if (ec) {
		res.error = "Failed to create output directory: " + output_dir.string();
		return res;
	}

	std::unique_ptr<PfsVolume>        vol;
	std::vector<PfsVolume::FileEntry> pfs_files;
	std::vector<PfsVolume::FileEntry> selected_pfs_files;
	if (options.extract_pfs && parsed.info.pfs_image_size > 0) {
		if (!OpenInnerAndOuterPfs(parsed, vol, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}
		if (vol && !vol->ListFiles({}, pfs_files, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}
		uint64_t logical_sum = 0;
		for (const auto& f: pfs_files) {
			const std::string p = f.rel_path.generic_string();
			parsed.info.pfs_files.push_back(p);
			parsed.info.pfs_file_entries.push_back({.path = p, .size = f.size});
			logical_sum += f.size;
			if (MatchesSelection(p, options)) {
				selected_pfs_files.push_back(f);
			}
		}
		parsed.info.pfs_logical_size = logical_sum;
		if (!options.only_prefixes.empty() && selected_pfs_files.empty()) {
			res.error = "No PFS files matched --only filter";
			res.info  = std::move(parsed.info);
			return res;
		}
	}

	uint64_t total_bytes = 0;
	for (const auto& raw: parsed.raw_entries) {
		if (options.extract_sc0 && raw.summary.id >= 0x1000u && raw.summary.size > 0) {
			total_bytes += raw.summary.size;
		}
	}
	for (const auto& f: selected_pfs_files) {
		total_bytes += f.size;
	}
	if (!CheckAvailableSpace(output_dir, total_bytes, options.check_free_space, res.error)) {
		res.info = std::move(parsed.info);
		return res;
	}

	if (options.extract_sc0) {
		FileSliceSource full_src(parsed.file, 0, parsed.file_size);
		for (const auto& raw: parsed.raw_entries) {
			if (raw.summary.id < 0x1000u || raw.summary.size == 0) {
				continue;
			}
			std::filesystem::path rel_name;
			if (!SanitizeRelativePath(raw.summary.name, rel_name)) {
				res.error = "Unsafe SC0 entry path in PKG: " + raw.summary.name;
				res.info  = std::move(parsed.info);
				return res;
			}
			const auto rel_str = rel_name.generic_string();
			const std::filesystem::path dst_path =
			    rel_str.starts_with("sce_sys/") ? (output_dir / rel_name)
			                                    : (output_dir / "sce_sys" / rel_name);

			const size_t read_size =
			    raw.summary.encrypted ? ((raw.summary.size + 15u) & ~15u) : raw.summary.size;
			if (static_cast<uint64_t>(raw.summary.offset) + read_size > parsed.file_size) {
				res.error = "PKG SC0 entry out of bounds: " + raw.summary.name;
				res.info  = std::move(parsed.info);
				return res;
			}
			std::vector<uint8_t> data(read_size);
			if (!full_src.ReadAt(raw.summary.offset, data.data(), data.size())) {
				res.error = "Failed reading PKG SC0 entry: " + raw.summary.name;
				res.info  = std::move(parsed.info);
				return res;
			}
			if (raw.summary.encrypted) {
				std::array<uint8_t, 32> key_seed {};
				if (!ComputePkgDerivedKey(parsed.info.content_id, options.passcode, raw.key_index,
				                          key_seed)) {
					res.error = "Cannot derive PKG entry key (invalid passcode/content_id)";
					res.info  = std::move(parsed.info);
					return res;
				}
				std::vector<uint8_t> plain;
				if (!DecryptPkgEntryData(raw.raw_meta_be, key_seed, data, plain)) {
					res.error = "Failed decrypting PKG SC0 entry: " + raw.summary.name;
					res.info  = std::move(parsed.info);
					return res;
				}
				plain.resize(raw.summary.size);
				data = std::move(plain);
			}
			std::filesystem::create_directories(dst_path.parent_path(), ec);
			std::FILE* out = std::fopen(dst_path.string().c_str(), "wb");
			if (out == nullptr || std::fwrite(data.data(), 1, data.size(), out) != data.size()) {
				if (out != nullptr) {
					std::fclose(out);
				}
				res.error = "Failed writing SC0 file: " + dst_path.string();
				res.info  = std::move(parsed.info);
				return res;
			}
			std::fclose(out);
			res.files_extracted++;
			res.bytes_extracted += data.size();
			if (options.on_progress) {
				options.on_progress(dst_path.lexically_relative(output_dir).generic_string(),
				                    res.bytes_extracted, total_bytes);
			}
		}
	}

	if (options.extract_pfs && vol != nullptr) {
		for (const auto& f: selected_pfs_files) {
			auto file_src = vol->OpenFileSource(f.inode_idx, res.error);
			if (!file_src) {
				res.info = std::move(parsed.info);
				return res;
			}
			const auto dst_path = output_dir / f.rel_path;
			if (!WriteStreamToFile(*file_src, f.size, dst_path, res.error)) {
				res.info = std::move(parsed.info);
				return res;
			}
			res.files_extracted++;
			res.bytes_extracted += f.size;
			if (options.on_progress) {
				options.on_progress(f.rel_path.generic_string(), res.bytes_extracted, total_bytes);
			}
		}
	}

	res.info    = std::move(parsed.info);
	res.success = true;
	return res;
}

} // namespace Common::Pkg
