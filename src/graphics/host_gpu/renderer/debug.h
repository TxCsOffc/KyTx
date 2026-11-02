#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <string>

namespace Libs::Graphics {

class CommandBuffer;

namespace HW {
class Context;
class UserConfig;
struct RenderTarget;
struct ScanModeControl;
struct ScreenViewport;
} // namespace HW

struct ScissorRect {
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
};

uint32_t                 render_target_mask_slot(uint32_t mask, uint32_t slot);
uint32_t                 render_target_first_bound_slot(const CommandBuffer& buffer);
bool                     graphics_debug_dump_enabled();
void                     uc_print(const char* func, const HW::UserConfig& uc);
void                     uc_check(const HW::UserConfig& uc);
std::string              rt_print(const char* func, const HW::RenderTarget& rt);
bool                     RenderIsColorTileModeLinear(Prospero::TileMode tile_mode);
void                     hw_print(const CommandBuffer& buffer);
void                     hw_check(const CommandBuffer& buffer);
void                     LogDrawPhase(const char* draw_name, const char* phase);
ScissorRect calc_final_scissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc,
                               vk::Extent2D extent, uint32_t viewport_index);

// KYTY_TRACE_CUBES=1: log cube-texture bindings and render targets that land inside them.
extern thread_local uint64_t g_debug_shader_hash;
extern thread_local int      g_debug_rt_follow; // stages left to report after a traced RT
void DebugNoteStage(uint64_t shader_hash);
// Log every stage with its bindings and render targets for the next `stages` stages.
void DebugStartFrameTrace(int stages);
bool DebugFrameTracing();
// Always-on ring of recent draws/dispatches, dumped when a GPU wait stalls.
void DebugRecordWork(const char* kind, uint64_t hash, uint32_t a, uint32_t b, uint32_t c);
void DebugDumpRecentWork(int count, uint64_t gpu_tick);
bool DebugReadIndirectProbe(uint32_t slot, uint32_t out[3]);
bool DebugDirtyWatchHit(uint64_t vaddr, uint64_t size);
// KYTY_BREADCRUMBS=1: the GPU writes the stage count after every draw/dispatch (slow, diagnostic).
uint64_t DebugWorkCount();
// Records (submits, draws, dispatches) newer than the submit of done_tick, up to and including
// the submit of wait_tick. Counts only what the 8192-entry ring still holds.
uint64_t DebugPendingWork(uint64_t done_tick, uint64_t wait_tick);
bool     DebugReadBreadcrumb(uint32_t* value);
void     DebugOnGpuHang();
// _TempData/cmask_no_readback present: skip CMask clear materialization readbacks (diagnostic).
bool DebugCmaskNoReadback();
// _TempData/no_readback present: CPU read faults on GPU-written pages see stale guest memory
// instead of draining the GPU (shadPS4 readbacks=false); CMask clear readbacks are skipped too.
bool DebugNoReadback();
// Guest EOP label writes land when the GPU completes the work before them, not at record time.
// KYTY_EOP_DEFERRED=1 or _TempData/eop_deferred turns it on; _TempData/eop_immediate forces off.
bool DebugEopDeferred();
// KYTY_UMA_READBACK=1 read path; _TempData/uma_off disables it live (buffers stay host-visible).
bool DebugUmaReadbackOff();
// _TempData/skip_shaders (hex hashes, re-read while running): drop dispatches of these shaders.
bool DebugSkipShader(uint64_t shader_hash);
void DebugNoteBuffer(uint64_t address, uint64_t size, bool written, uint64_t shader_hash);
// Latest recorded buffer write overlapping [address, address+size); needs KYTY_TRACK_WRITES=1.
bool DebugLastBufferWriter(uint64_t address, uint64_t size, uint64_t* shader, uint64_t* write_address,
                           uint64_t* write_size);
void DebugNoteCubeBinding(uint64_t address, uint64_t size, uint32_t format, uint32_t width,
                          uint32_t levels, uint32_t base_level, uint32_t level_count, bool storage,
                          bool is_cube, uint32_t layers);
void DebugNoteRenderTarget(uint64_t address, uint64_t size, uint32_t format, uint32_t width,
                           uint32_t height, uint32_t mip, uint32_t base_layer, uint32_t layers);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_
