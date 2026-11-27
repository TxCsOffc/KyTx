#ifndef KYTY_COMMON_PKG_PKGCRYPTO_H_
#define KYTY_COMMON_PKG_PKGCRYPTO_H_

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Common::Pkg {

inline constexpr std::string_view kDefaultFpkgPasscode = "00000000000000000000000000000000";

struct PfsXtsKeys {
	std::array<uint8_t, 16> tweak_key {};
	std::array<uint8_t, 16> data_key {};
};

class Aes128Context {
public:
	Aes128Context() = default;
	explicit Aes128Context(std::span<const uint8_t, 16> key);

	void SetKey(std::span<const uint8_t, 16> key);
	void EncryptBlock(const uint8_t in[16], uint8_t out[16]) const noexcept;
	void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const noexcept;

private:
	std::array<uint32_t, 44> m_enc_rk {};
	std::array<uint32_t, 44> m_dec_rk {};
};

[[nodiscard]] std::array<uint8_t, 32> Sha256(std::span<const uint8_t> data);
[[nodiscard]] std::array<uint8_t, 32> HmacSha256(std::span<const uint8_t> key,
                                                 std::span<const uint8_t> data);

void Aes128CbcEncrypt(std::span<const uint8_t, 16> key, std::span<const uint8_t, 16> iv,
                      std::span<const uint8_t> input, std::span<uint8_t> output);
void Aes128CbcDecrypt(std::span<const uint8_t, 16> key, std::span<const uint8_t, 16> iv,
                      std::span<const uint8_t> input, std::span<uint8_t> output);

void Aes128XtsEncryptSector(const Aes128Context& data_ctx, const Aes128Context& tweak_ctx,
                            uint64_t sector_index, std::span<uint8_t> sector_buf);
void Aes128XtsDecryptSector(const Aes128Context& data_ctx, const Aes128Context& tweak_ctx,
                            uint64_t sector_index, std::span<uint8_t> sector_buf);

[[nodiscard]] bool ComputePkgDerivedKey(std::string_view content_id, std::string_view passcode,
                                        uint32_t index, std::array<uint8_t, 32>& out_key);
[[nodiscard]] bool VerifyDerivedKeyDigest(std::span<const uint8_t, 32> dk,
                                          std::span<const uint8_t, 32> expected_digest);
[[nodiscard]] bool VerifyPasscodeDigest(std::string_view content_id, std::string_view passcode,
                                        std::span<const uint8_t, 32> expected_digest0);

[[nodiscard]] PfsXtsKeys DerivePfsXtsKeys(std::span<const uint8_t, 32> ekpfs,
                                          std::span<const uint8_t, 16> seed,
                                          bool                         new_crypt = false);

[[nodiscard]] bool EncryptPkgEntryData(std::span<const uint8_t, 32> meta_bytes_be,
                                       std::span<const uint8_t, 32> key_seed,
                                       std::span<const uint8_t>     plaintext,
                                       std::vector<uint8_t>&        out_ciphertext);
[[nodiscard]] bool DecryptPkgEntryData(std::span<const uint8_t, 32> meta_bytes_be,
                                       std::span<const uint8_t, 32> key_seed,
                                       std::span<const uint8_t>     ciphertext,
                                       std::vector<uint8_t>&        out_plaintext);

} // namespace Common::Pkg

#endif /* KYTY_COMMON_PKG_PKGCRYPTO_H_ */
