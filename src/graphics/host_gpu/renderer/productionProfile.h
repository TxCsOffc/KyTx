#pragma once

#include "graphics/host_gpu/vulkanCommon.h"
#include <chrono>
#include <source_location>
#include <optional>

namespace Libs::Graphics {
class CommandScheduler;

uint64_t ProfileClockNs() noexcept;
uint64_t ProfileThreadId() noexcept;
void ProfileArmWindow(uint64_t start) noexcept;
uint64_t ProfileWindowStart() noexcept;
struct ProfileThreadContext {
	CommandScheduler* scheduler = nullptr;
	uint64_t transaction = 0, address = 0;
	uint32_t guest_queue = UINT32_MAX;
	const char* operation = "";
};
ProfileThreadContext& ProfileThread() noexcept;
bool ProfileFineSelected() noexcept;
class ProfileOperationScope {
public:
	explicit ProfileOperationScope(const char* operation);
	~ProfileOperationScope();
private:
	const char* m_previous = nullptr;
};

// Inclusive CPU scopes are reconstructed as interval unions by the analyzer.
class ProfileCpuScope {
public:
	ProfileCpuScope(CommandScheduler& scheduler, const char* kind, uint64_t resource = 0,
	                uint64_t bytes = 0, bool transaction = false,
	                std::source_location site = std::source_location::current());
	~ProfileCpuScope();
	void SetGuestQueue(uint32_t queue);
	void SetIdentity(uint64_t resource, uint64_t bytes = 0) { m_resource = resource; m_bytes = bytes; }
	void NewTransaction(uint64_t address);
private:
	CommandScheduler* m_scheduler = nullptr;
	ProfileThreadContext m_previous {};
	const char* m_kind;
	std::source_location m_site;
	uint64_t m_begin = 0, m_frame = 0, m_resource = 0, m_bytes = 0;
};

// Fine host scopes are sampled; operation context remains available for watched readbacks.
class ProfileDetailScope {
public:
	ProfileDetailScope(const char* kind, uint64_t resource = 0, uint64_t bytes = 0,
	                   std::source_location site = std::source_location::current());
	~ProfileDetailScope();
	[[nodiscard]] bool Active() const { return m_scope.has_value(); }
	void SetIdentity(uint64_t resource, uint64_t bytes = 0) { if (m_scope) m_scope->SetIdentity(resource, bytes); }
	void Finish();
private:
	std::optional<ProfileCpuScope> m_scope;
	const char* m_previous_operation = nullptr;
	bool m_joined = false;
};
void ProfileDetailEvent(const char* kind, uint64_t resource = 0, uint64_t bytes = 0,
                        std::source_location site = std::source_location::current());
uint64_t ProfileFingerprint(const void* data, size_t bytes, uint64_t seed = 0);

void ProfilePipelineBarrier(vk::CommandBuffer command, vk::PipelineStageFlags src,
    vk::PipelineStageFlags dst, vk::DependencyFlags flags, uint32_t memories,
    const vk::MemoryBarrier* memory, uint32_t buffers, const vk::BufferMemoryBarrier* buffer,
    uint32_t images, const vk::ImageMemoryBarrier* image,
    std::source_location site = std::source_location::current());
void ProfilePipelineBarrier(vk::CommandBuffer command, vk::PipelineStageFlags src,
    vk::PipelineStageFlags dst, vk::DependencyFlags flags,
    vk::ArrayProxy<const vk::MemoryBarrier> memory, vk::ArrayProxy<const vk::BufferMemoryBarrier> buffer,
    vk::ArrayProxy<const vk::ImageMemoryBarrier> image,
    std::source_location site = std::source_location::current());
void ProfilePipelineBarrier2(vk::CommandBuffer command, const vk::DependencyInfo& dependency,
    std::source_location site = std::source_location::current());
} // namespace Libs::Graphics
