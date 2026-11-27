#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace Libs::Graphics::ShaderRecompiler::Msl {

// Contiguous zero-stream-overhead writer for generated Metal Shading Language source.
class MslTextBuffer {
public:
	void Reserve(size_t bytes) { data_.reserve(bytes); }

	void Clear() noexcept { data_.clear(); }

	[[nodiscard]] std::string_view View() const noexcept { return data_; }

	[[nodiscard]] std::string Take() noexcept { return std::move(data_); }

	void Append(std::string_view sv) { data_.append(sv); }

	void Append(char ch) { data_.push_back(ch); }

	void AppendU32(uint32_t value) {
		char buf[16];
		auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
		(void)ec;
		data_.append(buf, static_cast<size_t>(ptr - buf));
	}

	void AppendU64(uint64_t value) {
		char buf[24];
		auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
		(void)ec;
		data_.append(buf, static_cast<size_t>(ptr - buf));
	}

	void AppendHexU32(uint32_t value) {
		static constexpr char kHex[] = "0123456789abcdef";
		char buf[11];
		buf[0]  = '0';
		buf[1]  = 'x';
		buf[2]  = kHex[(value >> 28u) & 0xfu];
		buf[3]  = kHex[(value >> 24u) & 0xfu];
		buf[4]  = kHex[(value >> 20u) & 0xfu];
		buf[5]  = kHex[(value >> 16u) & 0xfu];
		buf[6]  = kHex[(value >> 12u) & 0xfu];
		buf[7]  = kHex[(value >> 8u) & 0xfu];
		buf[8]  = kHex[(value >> 4u) & 0xfu];
		buf[9]  = kHex[value & 0xfu];
		buf[10] = 'u';
		data_.append(buf, sizeof(buf));
	}

	MslTextBuffer& operator<<(std::string_view sv) {
		data_.append(sv);
		return *this;
	}
	MslTextBuffer& operator<<(const char* s) {
		if (s != nullptr) {
			data_.append(s);
		}
		return *this;
	}
	MslTextBuffer& operator<<(const std::string& s) {
		data_.append(s);
		return *this;
	}
	MslTextBuffer& operator<<(char ch) {
		data_.push_back(ch);
		return *this;
	}
	MslTextBuffer& operator<<(uint32_t value) {
		AppendU32(value);
		return *this;
	}
	MslTextBuffer& operator<<(uint64_t value) {
		AppendU64(value);
		return *this;
	}
	MslTextBuffer& operator<<(size_t value) {
		AppendU64(static_cast<uint64_t>(value));
		return *this;
	}
	MslTextBuffer& operator<<(int value) {
		char buf[16];
		auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
		(void)ec;
		data_.append(buf, static_cast<size_t>(ptr - buf));
		return *this;
	}
	MslTextBuffer& operator<<(bool value) {
		data_.push_back(value ? '1' : '0');
		return *this;
	}

private:
	std::string data_;
};

} // namespace Libs::Graphics::ShaderRecompiler::Msl
