#include "graphics/host_gpu/metal/metalIndirect.h"
#include "graphics/host_gpu/metal/metalOrder.h"
#include "graphics/host_gpu/cpWriteTrace.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::Metal {

// The shared output buffers of the argument converters, for KytyDescribeIndirectArguments (read by the GPU watchdog thread).
static std::atomic<void*>    s_compute_args {nullptr};
static std::atomic<void*>    s_mdi_args {nullptr};
static std::atomic<uint32_t> s_mdi_count {0};
// Every compute-indirect prepare writes its sequence number into dword 7 of the snapshot; the caller records what that dispatch was.
static std::atomic<uint32_t> s_compute_seq {0};
struct ComputeIndirectNote {
	uint32_t seq  = 0;
	uint32_t clock = 0; // CpTraceTick() when the dispatch was encoded
	uint64_t hash = 0;
	uint64_t addr = 0;
	char     cover[200] = {};
};
static constexpr uint32_t kComputeNotes = 8192; // the CPU can record this many indirect dispatches ahead of the GPU
static ComputeIndirectNote s_compute_notes[kComputeNotes];

void KytyNoteComputeIndirect(uint64_t hash, uint64_t addr, const char* cover) {
	ComputeIndirectNote& n = s_compute_notes[s_compute_seq.load() % kComputeNotes];
	n.seq  = s_compute_seq.load();
	n.clock = Libs::Graphics::CpTraceTick();
	n.hash = hash;
	n.addr = addr;
	std::snprintf(n.cover, sizeof(n.cover), "%s", cover ? cover : "");
}

id<MTLBuffer> MetalRectListIndirect::Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source,
                                          NSUInteger offset, uint32_t count, uint32_t stride) {
	if (!command || !source || count == 0 || stride < 16 || stride % 4 != 0 || offset % 4 != 0 ||
	    offset > source.length || uint64_t(count - 1) * stride + 16 > source.length - offset) return nil;
	id<MTLDevice> device = command.device;
	if (!m_pipeline) {
		NSString* code = @"#include <metal_stdlib>\nusing namespace metal;\n"
		    "kernel void rect_args(device const uint* src [[buffer(0)]], device uint4* dst [[buffer(1)]],\n"
		    "                      constant uint2& layout [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
		    " if (i >= layout.x) return;\n"
		    " uint p = i * layout.y; uint4 a = uint4(src[p], src[p+1], src[p+2], src[p+3]);\n"
		    " dst[i*2+1] = a; if (a.x == 3u) a.x = 4u; dst[i*2] = a;\n}\n";
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:code options:nil error:&error];
		id<MTLFunction> function = [library newFunctionWithName:@"rect_args"];
		if (function) m_pipeline = [device newComputePipelineStateWithFunction:function error:&error];
		if (!m_pipeline) {
			std::fprintf(stderr, "[RectIndirect] pipeline failed: %s\n", error.localizedDescription.UTF8String);
			return nil;
		}
	}
	const NSUInteger bytes = NSUInteger(count) * RecordBytes;
	if (!m_output || m_output.length < bytes) {
		m_output = [device newBufferWithLength:std::max<NSUInteger>(bytes, 4096) options:MTLResourceStorageModePrivate];
		m_output.label = @"RECTLIST indirect arguments";
	}
	if (!m_output) return nil;
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
	if (!encoder) return nil;
	OrderBegin(encoder);
	[encoder setComputePipelineState:m_pipeline];
	[encoder setBuffer:source offset:offset atIndex:0];
	[encoder setBuffer:m_output offset:0 atIndex:1];
	const uint32_t layout[] = {count, stride / 4};
	[encoder setBytes:layout length:sizeof(layout) atIndex:2];
	[encoder dispatchThreads:MTLSizeMake(count, 1, 1)
	     threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(count, m_pipeline.maxTotalThreadsPerThreadgroup), 1, 1)];
	OrderEnd(encoder);
	[encoder endEncoding];
	return m_output;
}

id<MTLBuffer> MetalComputeIndirect::Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source,
                                         NSUInteger offset, MTLSize guest_threads,
                                         bool thread_dimensions) {
	if (!command || !source || offset % 4 != 0 || offset > source.length ||
	    source.length - offset < 12 || guest_threads.width == 0 ||
	    guest_threads.height == 0 || guest_threads.depth == 0 ||
	    guest_threads.width > UINT32_MAX || guest_threads.height > UINT32_MAX ||
	    guest_threads.depth > UINT32_MAX) return nil;
	id<MTLDevice> device = command.device;
	if (!m_pipeline) {
		NSString* code = @"#include <metal_stdlib>\nusing namespace metal;\n"
		    "kernel void compute_args(device const uint* src [[buffer(0)]], device uint* dst [[buffer(1)]],\n"
		    "                         constant uint* cfg [[buffer(2)]],\n"
		    "                         uint i [[thread_position_in_grid]]) {\n"
		    " if (i >= 3u) return;\n"
		    " dst[7] = cfg[4];\n"
		    " uint c[3];\n"
		    " for (uint k = 0u; k < 3u; ++k) {\n"
		    "   uint n = src[k]; if (k == i) dst[k+4] = n;\n"
		    "   if (cfg[3]) n = n / cfg[k] + uint(n % cfg[k] != 0u);\n"
		    "   c[k] = n;\n"
		    " }\n"
		    " dst[i] = c[i];\n}\n";
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:code options:nil error:&error];
		id<MTLFunction> function = [library newFunctionWithName:@"compute_args"];
		if (function) m_pipeline = [device newComputePipelineStateWithFunction:function error:&error];
		if (!m_pipeline) {
			std::fprintf(stderr, "[ComputeIndirect] pipeline failed: %s\n", error.localizedDescription.UTF8String);
			return nil;
		}
	}
	if (!m_output) {
		m_output = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
		m_output.label = @"Compute indirect arguments";
		s_compute_args.store((__bridge void*)m_output);
	}
	if (!m_output) return nil;
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
	if (!encoder) return nil;
	OrderBegin(encoder);
	encoder.label = @"Prepare compute indirect arguments";
	[encoder setComputePipelineState:m_pipeline];
	[encoder setBuffer:source offset:offset atIndex:0];
	[encoder setBuffer:m_output offset:0 atIndex:1];
	const uint32_t config[] = {uint32_t(guest_threads.width), uint32_t(guest_threads.height),
	                          uint32_t(guest_threads.depth), uint32_t(thread_dimensions), ++s_compute_seq};
	[encoder setBytes:config length:sizeof(config) atIndex:2];
	[encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(3, 1, 1)];
	OrderEnd(encoder);
	[encoder endEncoding];
	return m_output;
}

id<MTLBuffer> MetalMultiDrawIndirect::Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source, NSUInteger offset,
                                           id<MTLBuffer> count_buffer, NSUInteger count_offset,
                                           uint32_t max_count, uint32_t stride, bool indexed, uint32_t index_limit) {
	const NSUInteger argument_bytes = indexed ? 20 : 16;
	if (!command || !source || max_count == 0 || stride < argument_bytes || stride % 4 != 0 ||
	    offset % 4 != 0 || offset > source.length ||
	    uint64_t(max_count - 1) * stride + argument_bytes > source.length - offset ||
	    (count_buffer && (count_offset % 4 != 0 || count_offset > count_buffer.length ||
	                      count_buffer.length - count_offset < 4))) return nil;
	id<MTLDevice> device = command.device;
	if (!m_pipeline) {
		NSString* code = @"#include <metal_stdlib>\nusing namespace metal;\n"
		    "kernel void multi_args(device const uint* src [[buffer(0)]], device uint* dst [[buffer(1)]],\n"
		    "                       device const uint* count [[buffer(2)]], constant uint* cfg [[buffer(3)]],\n"
		    "                       uint i [[thread_position_in_grid]]) {\n"
		    " if (i >= cfg[0]) return;\n"
		    " uint n = cfg[2] ? count[0] : cfg[0];\n"
		    " ulong base = ulong(i) * cfg[1]; uint out = i * 16u;\n"
		    " for (uint w = 0u; w < 8u; ++w) {\n"
		    "   uint v = w < (cfg[3] ? 5u : 4u) ? src[base+w] : 0u;\n"
		    "   dst[out+w] = v; dst[out+8u+w] = v;\n"
		    " }\n"
		    " dst[out+13u] = n; dst[out+14u] = cfg[4]; dst[out+15u] = cfg[3];\n"
		    " if (cfg[3]) {\n"
		    "   uint start = dst[out+2u];\n"
		    "   if (start >= cfg[4]) { dst[out] = 0u; dst[out+2u] = 0u; }\n"
		    "   else dst[out] = min(dst[out], cfg[4] - start);\n"
		    " }\n"
		    " if (i >= n) dst[out+1u] = 0u;\n"
		    "}\n";
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:code options:nil error:&error];
		id<MTLFunction> function = [library newFunctionWithName:@"multi_args"];
		if (function) m_pipeline = [device newComputePipelineStateWithFunction:function error:&error];
		if (!m_pipeline) {
			std::fprintf(stderr, "[MdiPrepare] pipeline failed: %s\n", error.localizedDescription.UTF8String);
			return nil;
		}
	}
	const NSUInteger bytes = NSUInteger(max_count) * RecordBytes;
	if (!m_output || m_output.length < bytes) {
		m_output = [device newBufferWithLength:std::max<NSUInteger>(bytes, 4096) options:MTLResourceStorageModeShared];
		m_output.label = @"GPU multi-draw arguments and original snapshot";
		s_mdi_args.store((__bridge void*)m_output);
	}
	s_mdi_count.store(max_count);
	if (!m_output) return nil;
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
	if (!encoder) return nil;
	OrderBegin(encoder);
	encoder.label = @"Prepare multi-draw arguments";
	[encoder setComputePipelineState:m_pipeline];
	[encoder setBuffer:source offset:offset atIndex:0];
	[encoder setBuffer:m_output offset:0 atIndex:1];
	[encoder setBuffer:count_buffer ? count_buffer : source offset:count_buffer ? count_offset : 0 atIndex:2];
	const uint32_t config[] = {max_count, stride / 4u, count_buffer != nil, indexed, index_limit};
	[encoder setBytes:config length:sizeof(config) atIndex:3];
	const NSUInteger group = std::min<NSUInteger>(64, m_pipeline.maxTotalThreadsPerThreadgroup);
	[encoder dispatchThreadgroups:MTLSizeMake((NSUInteger(max_count) + group - 1) / group, 1, 1)
	        threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
	OrderEnd(encoder);
	[encoder endEncoding];
	return m_output;
}

std::string KytyDescribeIndirectArguments() {
	std::string out;
	char line[320];
	if (void* p = s_compute_args.load()) {
		const uint32_t* d = static_cast<const uint32_t*>(((__bridge id<MTLBuffer>)p).contents);
		std::snprintf(line, sizeof(line), "compute-indirect snapshot seq=%u: metal groups=%u,%u,%u original=%u,%u,%u; ", d[7], d[0], d[1], d[2], d[4], d[5], d[6]);
		out += line;
		const ComputeIndirectNote& n = s_compute_notes[d[7] % kComputeNotes];
		if (n.seq == d[7]) {
			std::snprintf(line, sizeof(line), "that dispatch: cs=0x%016llx args=0x%llx covered by: %s; ", (unsigned long long)n.hash, (unsigned long long)n.addr,
			              n.cover[0] ? n.cover : "(no GPU surface)");
			out += line;
		}
	}
	if (void* p = s_mdi_args.load()) {
		id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)p;
		const uint32_t count = std::min<uint32_t>(s_mdi_count.load(), 4u);
		const uint32_t* d = static_cast<const uint32_t*>(buffer.contents);
		for (uint32_t k = 0; k < count && (k + 1) * MetalMultiDrawIndirect::RecordBytes <= buffer.length; ++k) {
			const uint32_t* c = d + k * (MetalMultiDrawIndirect::RecordBytes / 4);
			const uint32_t* r = c + MetalMultiDrawIndirect::OriginalArgumentsOffset / 4;
			std::snprintf(line, sizeof(line), "mdi[%u] metal=%u,%u,%u,%d,%u original=%u,%u,%u,%d,%u gpu_count=%u index_limit=%u indexed=%u; ",
			              k, c[0], c[1], c[2], int32_t(c[3]), c[4], r[0], r[1], r[2], int32_t(r[3]), r[4], r[5], r[6], r[7]);
			out += line;
		}
	}
	return out;
}

} // namespace Libs::Graphics::Metal

namespace Libs::Graphics::Metal {
id<MTLBuffer> MetalMeshIndirect::Encode(id<MTLCommandBuffer> command, id<MTLBuffer> source, NSUInteger offset,
                                       uint32_t count, uint32_t stride, bool indexed, uint32_t index_bytes,
                                       uint32_t primitive_size, uint32_t primitive_step, uint32_t primitives_per_group) {
	const uint32_t input_bytes = indexed ? 20u : 16u;
	if (!command || !source || count == 0 || stride < input_bytes || stride % 4 || offset % 4 ||
	    offset > source.length || uint64_t(count - 1) * stride + input_bytes > source.length - offset ||
	    primitive_size == 0 || primitive_step == 0 || primitives_per_group == 0 ||
	    (indexed && index_bytes != 2 && index_bytes != 4)) return nil;
	auto device = command.device;
	if (!m_pipeline) {
		NSString* code = @"#include <metal_stdlib>\nusing namespace metal;\n"
		"kernel void mesh_args(device const uint* src [[buffer(0)]], device uint* dst [[buffer(1)]], constant uint* c [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
		" if (i >= c[0]) return; uint p=i*c[1], q=i*16u; uint n=src[p], instances=src[p+1];\n"
		" uint primitives=n<c[4]?0u:(n-c[4])/c[5]+1u; uint groups=primitives/c[6]+uint(primitives%c[6]!=0u);\n"
		" dst[q]=groups; dst[q+1]=instances; dst[q+2]=1u; dst[q+3]=0u;\n"
		" dst[q+4]=n; dst[q+5]=src[p+(c[2]?3u:2u)]; dst[q+6]=src[p+(c[2]?4u:3u)];\n"
		" dst[q+7]=c[2]?c[3]:0u; dst[q+8]=c[2]?src[p+2]:0u;\n"
		" for(uint k=9;k<16;++k) dst[q+k]=0u;\n}\n";
		NSError* error=nil;
		auto library=[device newLibraryWithSource:code options:nil error:&error];
		auto function=[library newFunctionWithName:@"mesh_args"];
		if (function) m_pipeline=[device newComputePipelineStateWithFunction:function error:&error];
		if (!m_pipeline) { std::fprintf(stderr,"[MeshIndirect] %s\n",error.localizedDescription.UTF8String); return nil; }
	}
	const NSUInteger bytes=NSUInteger(count)*RecordBytes;
	if (!m_output || m_output.length < bytes) m_output=[device newBufferWithLength:std::max<NSUInteger>(bytes,4096) options:MTLResourceStorageModePrivate];
	if (!m_output) return nil;
	auto encoder=[command computeCommandEncoder];if (!encoder) return nil;
	OrderBegin(encoder);[encoder setComputePipelineState:m_pipeline];[encoder setBuffer:source offset:offset atIndex:0];[encoder setBuffer:m_output offset:0 atIndex:1];
	const uint32_t config[]={count,stride/4,uint32_t(indexed),index_bytes,primitive_size,primitive_step,primitives_per_group};
	[encoder setBytes:config length:sizeof(config) atIndex:2];[encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(std::min(count,64u),1,1)];OrderEnd(encoder);[encoder endEncoding];return m_output;
}
}
