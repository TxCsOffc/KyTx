#ifndef KYTY_GRAPHICS_HOST_GPU_GPU_CAPTURE_APPLE_H_
#define KYTY_GRAPHICS_HOST_GPU_GPU_CAPTURE_APPLE_H_

namespace Libs::Graphics {

#if defined(__APPLE__)
// Called after each present; records frames to a Metal .gputrace when triggered.
void GpuCaptureOnPresent();
#else
inline void GpuCaptureOnPresent() {}
#endif

} // namespace Libs::Graphics

#endif /* KYTY_GRAPHICS_HOST_GPU_GPU_CAPTURE_APPLE_H_ */
