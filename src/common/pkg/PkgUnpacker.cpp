#include "common/pkg/PkgUnpacker.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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
constexpr uint32_t kPfscMagic       = 0x43534650u; // "PFSC" little-endian
constexpr int64_t  kPfsMagic        = 20130315ll;
constexpr uint32_t kXtsSectorSize   = 0x1000u;
constexpr uint16_t kPfsModeSigned   = 0x1u;
constexpr uint16_t kPfsModeEncrypt  = 0x4u;
constexpr uint64_t kPfsNewCryptFlag = 0x2000000000000000ull;

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

} // namespace

PkgUnpackResult InspectPkg(const std::filesystem::path& pkg_path, std::string_view passcode) {
	PkgUnpackResult res {};
	ParsedPkg       parsed;
	if (!ParsePkgFile(pkg_path, passcode, parsed, res.error)) {
		return res;
	}
	std::unique_ptr<PfsVolume> vol;
	if (parsed.info.pfs_image_size > 0 && (!parsed.info.pfs_encrypted || parsed.ekpfs.has_value())) {
		if (OpenInnerAndOuterPfs(parsed, vol, res.error) && vol != nullptr) {
			std::vector<PfsVolume::FileEntry> files;
			if (vol->ListFiles({}, files, res.error)) {
				for (const auto& f: files) {
					parsed.info.pfs_files.push_back(f.rel_path.generic_string());
				}
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
	PkgUnpackResult res {};
	ParsedPkg       parsed;
	if (!ParsePkgFile(pkg_path, options.passcode, parsed, res.error)) {
		return res;
	}
	if (options.list_only) {
		return InspectPkg(pkg_path, options.passcode);
	}

	std::error_code ec;
	std::filesystem::create_directories(output_dir, ec);
	if (ec) {
		res.error = "Failed to create output directory: " + output_dir.string();
		return res;
	}

	std::unique_ptr<PfsVolume>        vol;
	std::vector<PfsVolume::FileEntry> pfs_files;
	if (options.extract_pfs && parsed.info.pfs_image_size > 0) {
		if (!OpenInnerAndOuterPfs(parsed, vol, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}
		if (vol && !vol->ListFiles({}, pfs_files, res.error)) {
			res.info = std::move(parsed.info);
			return res;
		}
		for (const auto& f: pfs_files) {
			parsed.info.pfs_files.push_back(f.rel_path.generic_string());
		}
	}

	uint64_t total_bytes = 0;
	for (const auto& raw: parsed.raw_entries) {
		if (options.extract_sc0 && raw.summary.id >= 0x1000u && raw.summary.size > 0) {
			total_bytes += raw.summary.size;
		}
	}
	for (const auto& f: pfs_files) {
		total_bytes += f.size;
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
		for (const auto& f: pfs_files) {
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
