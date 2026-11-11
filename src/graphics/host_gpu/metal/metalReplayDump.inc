// Opt-in exact dispatch snapshots for offline native/SPIR-V differential replay.
// Included inside the renderer namespace. No shader/resource values are changed.
static bool ReplayDumpWanted(uint64_t guest, uint64_t native) {
	static const char* dir     = KYTY_ENV("KYTY_REPLAY_DUMP_DIR");
	static const char* filter  = KYTY_ENV("KYTY_REPLAY_DUMP_HASH");
	static const char* trigger = KYTY_ENV("KYTY_REPLAY_DUMP_TRIGGER");
	if (!dir || !filter || !trigger) return false;
	static uint64_t target = UINT64_MAX, last_flip = UINT64_MAX;
	static uint32_t remaining = 0;
	static const char* count_env = KYTY_ENV("KYTY_REPLAY_DUMP_COUNT");
	static const uint32_t count = count_env ? std::max(1u,std::min(32u,uint32_t(std::strtoul(count_env,nullptr,10)))) : 1u;
	const uint64_t  flip   = TraceWindow::g_flip.load(std::memory_order_relaxed);
	if (access(trigger, F_OK) == 0) {
		unlink(trigger);
		target = flip + 1;
		remaining = count;
		last_flip = UINT64_MAX;
		std::fprintf(stderr, "[ReplayDump] armed flip=%llu\n", (unsigned long long)target);
	}
	if (count == 1 ? flip != target : (!remaining || flip < target || flip == last_flip)) return false;
	for (const char* p = filter; *p;) {
		char*          end = nullptr;
		const uint64_t h   = std::strtoull(p, &end, 16);
		if (end == p) break;
		if (h == guest || h == native || (h <= UINT32_MAX && h == (guest >> 32u))) {
			if (count > 1) { --remaining; last_flip = flip; }
			return true;
		}
		p = *end == ',' ? end + 1 : end;
	}
	return false;
}
static bool ReplayWrite(NSString* dir, NSString* name, const void* data, size_t bytes) {
	return [[NSData dataWithBytes:data
	                       length:bytes] writeToFile:[dir stringByAppendingPathComponent:name]
	                                      atomically:YES];
}
static bool ReplayJson(NSString* dir, NSString* name, id object) {
	NSError* error = nil;
	NSData*  data  = [NSJSONSerialization dataWithJSONObject:object
	                                                 options:NSJSONWritingPrettyPrinted
	                                                   error:&error];
	return data && [data writeToFile:[dir stringByAppendingPathComponent:name] atomically:YES];
}
static bool ReplayDumpPhase(NSString* dir, NSString* phase,
                            const std::array<MetalBufferAllocation, 64>& buffers,
                            const std::array<id<MTLTexture>, 64>& images, const uint32_t* sizes, id<MTLBuffer> gds = nil) {
	// Commit the producer and all previous writes before touching shared bytes.
	(void)MetalContext::Instance().GetCurrentCommandBuffer();
	MetalContext::Instance().CommitCommandBuffer(true);
	NSMutableArray* bs       = [NSMutableArray new];
	NSMutableArray* ts       = [NSMutableArray new];
	bool            complete = true;
	struct Range {
		id<MTLBuffer> buffer;
		uint64_t      first, last;
	};
	std::map<uintptr_t, Range> ranges;
	for (size_t i = 0; i < buffers.size(); ++i) {
		const auto& a = buffers[i];
		if (!a.buffer) continue;
		const uint64_t  length = std::min<uint64_t>(sizes[i], a.buffer.length - a.offset);
		const uintptr_t key    = (uintptr_t)(__bridge void*)a.buffer;
		auto            it     = ranges.find(key);
		if (it == ranges.end())
			ranges.emplace(key, Range {a.buffer, a.offset, a.offset + length});
		else {
			it->second.first = std::min<uint64_t>(it->second.first, a.offset);
			it->second.last  = std::max<uint64_t>(it->second.last, a.offset + length);
		}
	}
	for (size_t i = 0; i < buffers.size(); ++i) {
		const auto& a = buffers[i];
		if (!a.buffer) continue;
		const uintptr_t key = (uintptr_t)(__bridge void*)a.buffer;
		const auto&     r   = ranges.at(key);
		[bs addObject:@{
			@"slot" : @(i),
			@"path" :
			    [NSString stringWithFormat:@"%@-buffer-%llx.bin", phase, (unsigned long long)key],
			@"offset" : @(a.offset - r.first),
			@"length" : @(sizes[i]),
			@"backing" : @(key)
		}];
	}
	for (const auto& [key, r]: ranges) {
		const uint64_t bytes = r.last - r.first;
		NSString*      name =
		    [NSString stringWithFormat:@"%@-buffer-%llx.bin", phase, (unsigned long long)key];
		id<MTLBuffer> copy =
		    [MetalContext::Instance().GetDevice() newBufferWithLength:std::max<uint64_t>(bytes, 4)
		                                                      options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer>      cmd  = [MetalContext::Instance().GetCommandQueue() commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
		if (!copy || !blit) {
			complete = false;
			continue;
		}
		if (bytes)
			[blit copyFromBuffer:r.buffer
			         sourceOffset:r.first
			             toBuffer:copy
			    destinationOffset:0
			                 size:bytes];
		[blit endEncoding];
		KytyBudgetedCommit(cmd);
		[cmd waitUntilCompleted];
		if (cmd.status != MTLCommandBufferStatusCompleted ||
		    !ReplayWrite(dir, name, copy.contents, bytes))
			complete = false;
	}
	for (size_t i = 0; i < images.size(); ++i) {
		id<MTLTexture> t = images[i];
		if (!t) continue;
		const auto f     = t.pixelFormat;
		uint32_t   block = 1, bpp = BytesPerPixel(f);
		if (IsCompressedFormat(f)) {
			block = 4;
			bpp   = (f == MTLPixelFormatBC1_RGBA || f == MTLPixelFormatBC1_RGBA_sRGB ||
			         f == MTLPixelFormatBC4_RUnorm || f == MTLPixelFormatBC4_RSnorm)
			            ? 8
			            : 16;
		}
		NSMutableArray*      parts = [NSMutableArray new];
		NSMutableDictionary* image = [@{
			@"slot" : @(i),
			@"object" : @((uintptr_t)(__bridge void*)t),
			@"format" : @(f),
			@"type" : @(t.textureType),
			@"width" : @(t.width),
			@"height" : @(t.height),
			@"depth" : @(t.depth),
			@"layers" : @(t.arrayLength),
			@"mips" : @(t.mipmapLevelCount),
			@"swizzle" :
			    @[ @(t.swizzle.red), @(t.swizzle.green), @(t.swizzle.blue), @(t.swizzle.alpha) ],
			@"parts" : parts
		} mutableCopy];
		if (t.parentTexture) image[@"parent"] = @((uintptr_t)(__bridge void*)t.parentTexture);
		if (t.buffer) {
			image[@"buffer"]        = @((uintptr_t)(__bridge void*)t.buffer);
			image[@"buffer_offset"] = @(t.bufferOffset);
			image[@"buffer_row"]    = @(t.bufferBytesPerRow);
		}
		[ts addObject:image];
		if (!bpp || t.sampleCount != 1 || f == MTLPixelFormatDepth32Float_Stencil8 ||
		    t.textureType == MTLTextureTypeTextureBuffer) {
			complete        = false;
			image[@"error"] = @"unsupported exact readback layout";
			continue;
		}
		const NSUInteger slices =
		    t.textureType == MTLTextureTypeCube        ? 6
			: t.textureType == MTLTextureTypeCubeArray ? t.arrayLength * 6
			: t.textureType == MTLTextureType2DArray || t.textureType == MTLTextureType1DArray
		        ? t.arrayLength
		        : 1;
		for (NSUInteger mip = 0; mip < t.mipmapLevelCount; ++mip)
			for (NSUInteger slice = 0; slice < slices; ++slice) {
				@autoreleasepool {
					const NSUInteger     width  = std::max<NSUInteger>(t.width >> mip, 1),
					                     height = std::max<NSUInteger>(t.height >> mip, 1),
					                     depth  = t.textureType == MTLTextureType3D
										              ? std::max<NSUInteger>(t.depth >> mip, 1)
										              : 1;
					const NSUInteger     row    = ((width + block - 1) / block) * bpp,
					                     rows   = (height + block - 1) / block,
					                     pitch  = (row + 255) & ~NSUInteger(255),
					                     bytes  = pitch * rows * depth;
					id<MTLBuffer>        copy   = [MetalContext::Instance().GetDevice()
					    newBufferWithLength:bytes
					                options:MTLResourceStorageModeShared];
					id<MTLCommandBuffer> cmd =
					    [MetalContext::Instance().GetCommandQueue() commandBuffer];
					id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
					if (!copy || !blit) {
						complete = false;
						continue;
					}
					[blit copyFromTexture:t
					                 sourceSlice:slice
					                 sourceLevel:mip
					                sourceOrigin:MTLOriginMake(0, 0, 0)
					                  sourceSize:MTLSizeMake(width, height, depth)
					                    toBuffer:copy
					           destinationOffset:0
					      destinationBytesPerRow:pitch
					    destinationBytesPerImage:pitch * rows];
					[blit endEncoding];
					KytyBudgetedCommit(cmd);
					[cmd waitUntilCompleted];
					if (cmd.status != MTLCommandBufferStatusCompleted) {
						complete        = false;
						image[@"error"] = cmd.error.localizedDescription ?: @"GPU copy failed";
						continue;
					}
					std::vector<uint8_t> raw(row * rows * depth);
					for (NSUInteger z = 0; z < depth; ++z)
						for (NSUInteger y = 0; y < rows; ++y)
							std::memcpy(raw.data() + (z * rows + y) * row,
							            (const uint8_t*)copy.contents + (z * rows + y) * pitch,
							            row);
					NSString* name =
					    [NSString stringWithFormat:@"%@-image-%zu-m%lu-s%lu.bin", phase, i,
					                               (unsigned long)mip, (unsigned long)slice];
					if (!ReplayWrite(dir, name, raw.data(), raw.size())) complete = false;
					[parts addObject:@{
						@"path" : name,
						@"mip" : @(mip),
						@"slice" : @(slice),
						@"width" : @(width),
						@"height" : @(height),
						@"depth" : @(depth),
						@"row" : @(row),
						@"image_bytes" : @(row * rows)
					}];
				}
			}
	}
	// The producer fence above completed all GDS writes. This shared buffer is
	// snapshotted in full, including counters used to terminate work queues.
	NSString* gds_path = gds ? [phase stringByAppendingString:@"-gds.bin"] : @"";
	if (gds && (!gds.contents || !ReplayWrite(dir,gds_path,gds.contents,gds.length))) complete = false;
	return ReplayJson(
	           dir, [phase stringByAppendingString:@".json"],
	           @{@"complete" : @(complete),
			     @"gds" : gds_path,
			     @"buffers" : bs,
			     @"images" : ts}) &&
	       complete;
}

// ---- GDS tripwire (diagnostic stop, opt-in) ----
// KYTY_GDS_TRIP=<hash>:<dword>:<max>  hash = guest or native hash of a kernel that reads a GDS work count.
// KYTY_GDS_TRIP_DIR=<dir>             where the capture goes.
// KYTY_GDS_TRIP_WATCH=<hash>[,...]    kernels whose last buffer bindings are captured with it (the count's producers).
// Before every dispatch of the kernel the renderer drains the GPU and reads GDS[dword]. Up to <max> the dispatch is encoded
// unchanged (a continuing run skips nothing). Above it the kernel's exact inputs (replay "before" dump, GDS included), the
// current bytes of the watched kernels' last bindings and a trip.json are written, the GPU budget is closed cleanly and the
// process exits with 77 without encoding the kernel: the runaway input never reaches the GPU.
struct GdsTripConfig {
	bool                  on    = false;
	uint64_t              hash  = 0;
	uint32_t              dword = 0;
	uint32_t              max   = 0;
	std::string           dir;
	std::vector<uint64_t> watch;
};
static const GdsTripConfig& GdsTrip() {
	static const GdsTripConfig config = [] {
		GdsTripConfig c;
		const char* spec = KYTY_ENV("KYTY_GDS_TRIP");
		const char* dir  = KYTY_ENV("KYTY_GDS_TRIP_DIR");
		if (spec == nullptr || dir == nullptr || dir[0] == '\0') return c;
		char* end = nullptr;
		c.hash = std::strtoull(spec, &end, 16);
		if (end == spec || *end != ':') return c;
		const char* p = end + 1;
		c.dword = static_cast<uint32_t>(std::strtoul(p, &end, 0));
		if (end == p || *end != ':' || c.dword >= 16384u) return c;
		p = end + 1;
		c.max = static_cast<uint32_t>(std::strtoul(p, &end, 0));
		if (end == p || *end != '\0') return c;
		c.dir = dir;
		if (const char* list = KYTY_ENV("KYTY_GDS_TRIP_WATCH")) {
			for (const char* q = list; *q != '\0';) {
				const uint64_t h = std::strtoull(q, &end, 16);
				if (end == q) break;
				c.watch.push_back(h);
				q = *end == ',' ? end + 1 : end;
			}
		}
		c.on = true;
		std::fprintf(stderr, "[GdsTrip] armed hash=%016llx GDS[%u] max=%u watch=%zu dir=%s\n", (unsigned long long)c.hash, c.dword, c.max,
		             c.watch.size(), c.dir.c_str());
		return c;
	}();
	return config;
}
struct GdsTripBinding {
	size_t   slot = 0;
	uint64_t addr = 0, size = 0;
};
struct GdsTripRecord {
	uint64_t                    guest = 0, flip = 0, serial = 0, indirect = 0;
	uint32_t                    groups[3] {};
	std::vector<GdsTripBinding> buffers;
};
static std::mutex                         s_gds_trip_mutex;
static std::map<uint64_t, GdsTripRecord>  s_gds_trip_records; // native hash -> its last dispatch
static uint64_t                           s_gds_trip_serial = 0;
static bool GdsTripWatched(uint64_t guest, uint64_t native) {
	const auto& c = GdsTrip();
	if (!c.on) return false;
	for (uint64_t h: c.watch)
		if (h == guest || h == native) return true;
	return false;
}
static void GdsTripBeginWatch(uint64_t guest, uint64_t native, uint32_t gx, uint32_t gy, uint32_t gz, uint64_t indirect) {
	std::lock_guard lock(s_gds_trip_mutex);
	auto& r    = s_gds_trip_records[native];
	r.guest    = guest;
	r.flip     = TraceWindow::g_flip.load(std::memory_order_relaxed);
	r.serial   = ++s_gds_trip_serial;
	r.indirect = indirect;
	r.groups[0] = gx;
	r.groups[1] = gy;
	r.groups[2] = gz;
	r.buffers.clear();
}
static void GdsTripNoteBuffer(uint64_t native, size_t slot, uint64_t addr, uint64_t size) {
	std::lock_guard lock(s_gds_trip_mutex);
	auto it = s_gds_trip_records.find(native);
	if (it != s_gds_trip_records.end()) it->second.buffers.push_back({slot, addr, size});
}
// Drains the GPU and reads the count. True when the dispatch must not be encoded.
static bool GdsTripCheck(uint64_t guest, uint64_t native, uint32_t& value) {
	const auto& c = GdsTrip();
	if (!c.on || (c.hash != guest && c.hash != native)) return false;
	(void)MetalContext::Instance().GetCurrentCommandBuffer();
	MetalContext::Instance().CommitCommandBuffer(true);
	value = static_cast<const uint32_t*>([GdsBuffer() contents])[c.dword];
	static uint64_t checks = 0;
	if (value > c.max || ++checks % 64 == 1) {
		std::fprintf(stderr, "[GdsTrip] check #%llu flip=%llu GDS[%u]=%u max=%u\n", (unsigned long long)checks,
		             (unsigned long long)TraceWindow::g_flip.load(std::memory_order_relaxed), c.dword, value, c.max);
	}
	return value > c.max;
}
static NSString* GdsTripDir(uint64_t guest, uint64_t native) {
	NSString* dir = [NSString stringWithFormat:@"%s/trip-%016llx-%016llx-f%llu", GdsTrip().dir.c_str(), (unsigned long long)guest,
	                                           (unsigned long long)native, (unsigned long long)TraceWindow::g_flip.load()];
	[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	return dir;
}
// Current bytes of every watched kernel's last bindings (later kernels may have reused a range: the json keeps the order).
[[noreturn]] static void GdsTripFinish(NSString* dir, uint64_t guest, uint64_t native, uint32_t value, bool inputs_complete) {
	NSMutableArray* kernels = [NSMutableArray new];
	std::lock_guard lock(s_gds_trip_mutex);
	for (const auto& [hash, r]: s_gds_trip_records) {
		NSMutableArray* bufs = [NSMutableArray new];
		for (const auto& b: r.buffers) {
			NSString* name = [NSString stringWithFormat:@"watch-%016llx-b%zu-%llx.bin", (unsigned long long)hash, b.slot, (unsigned long long)b.addr];
			bool ok = false;
			const uint64_t bytes = std::min<uint64_t>(b.size, 256ull << 20);
			auto a = MetalBufferManager::Instance().GetBufferForGuestRange(b.addr, bytes);
			if (a.buffer != nil && a.offset + bytes <= a.buffer.length) {
				id<MTLBuffer> copy = [MetalContext::Instance().GetDevice() newBufferWithLength:std::max<uint64_t>(bytes, 4)
				                                                                       options:MTLResourceStorageModeShared];
				id<MTLCommandBuffer>      cmd  = [MetalContext::Instance().GetCommandQueue() commandBuffer];
				id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
				if (copy && blit) {
					[blit copyFromBuffer:a.buffer sourceOffset:a.offset toBuffer:copy destinationOffset:0 size:bytes];
					[blit endEncoding];
					KytyBudgetedCommit(cmd);
					[cmd waitUntilCompleted];
					ok = cmd.status == MTLCommandBufferStatusCompleted && ReplayWrite(dir, name, copy.contents, bytes);
				}
			}
			[bufs addObject:@{@"slot": @(b.slot), @"addr": [NSString stringWithFormat:@"0x%llx", (unsigned long long)b.addr],
			                  @"size": @(b.size), @"file": name, @"ok": @(ok)}];
		}
		[kernels addObject:@{@"native": [NSString stringWithFormat:@"%016llx", (unsigned long long)hash],
		                     @"guest": [NSString stringWithFormat:@"%016llx", (unsigned long long)r.guest], @"flip": @(r.flip), @"serial": @(r.serial),
		                     @"groups": @[@(r.groups[0]), @(r.groups[1]), @(r.groups[2])],
		                     @"indirect": [NSString stringWithFormat:@"0x%llx", (unsigned long long)r.indirect], @"buffers": bufs}];
	}
	ReplayWrite(dir, @"gds.bin", [GdsBuffer() contents], 64 * 1024);
	ReplayJson(dir, @"trip.json", @{@"guest": [NSString stringWithFormat:@"%016llx", (unsigned long long)guest],
	                                 @"native": [NSString stringWithFormat:@"%016llx", (unsigned long long)native],
	                                 @"gds_dword": @(GdsTrip().dword), @"value": @(value), @"max": @(GdsTrip().max),
	                                 @"serial": @(s_gds_trip_serial + 1), @"inputs_complete": @(inputs_complete), @"watched": kernels});
	std::fprintf(stderr, "[GdsTrip] TRIPPED GDS[%u]=%u > %u: capture %s; exiting 77 without encoding %016llx\n", GdsTrip().dword, value,
	             GdsTrip().max, dir.UTF8String, (unsigned long long)native);
	std::fflush(stderr);
	HostBudget::Finish();
	std::_Exit(77);
}

// ---- Encode-vs-GPU input race watch (diagnostic, opt-in) ----
// KYTY_RACE_WATCH=<hash>[,...] (guest or native): for each dispatch of these kernels, every read-only buffer of at most
// KYTY_RACE_MAX_BYTES (default 8 MiB) is copied on the CPU when the dispatch is encoded and by a blit ordered right before it on
// the GPU. The completion handler compares them: a difference means the guest changed the bytes between submission and GPU
// execution (expected for ranges an earlier kernel of the same frame writes; a race for CPU-written ranges).
// KYTY_RACE_STOP=1: the first difference below 0x8240000000 (outside the GPU arena) closes the GPU budget and exits 78.
static const std::vector<uint64_t>& RaceWatchList() {
	static const std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		if (const char* s = KYTY_ENV("KYTY_RACE_WATCH")) {
			char* end = nullptr;
			for (const char* q = s; *q != '\0';) {
				const uint64_t h = std::strtoull(q, &end, 16);
				if (end == q) break;
				v.push_back(h);
				q = *end == ',' ? end + 1 : end;
			}
			std::fprintf(stderr, "[RaceWatch] armed kernels=%zu\n", v.size());
		}
		return v;
	}();
	return list;
}
static bool RaceWatched(uint64_t guest, uint64_t native) {
	for (uint64_t h: RaceWatchList())
		if (h == guest || h == native) return true;
	return false;
}
static std::atomic<uint64_t> s_race_checks {0}, s_race_diffs {0}, s_race_logged {0};
static std::atomic<bool>     s_race_stop {false};
static void RaceWatchBuffer(uint64_t native, size_t slot, uint64_t addr, uint64_t size) {
	static const uint64_t max_bytes = KYTY_ENV("KYTY_RACE_MAX_BYTES") ? std::strtoull(KYTY_ENV("KYTY_RACE_MAX_BYTES"), nullptr, 0) : (8ull << 20);
	static const bool     stop      = KYTY_ENV("KYTY_RACE_STOP") != nullptr;
	if (addr == 0 || size == 0 || size > max_bytes) return;
	uint64_t backing = 0;
	if (!LibKernel::Memory::TryGetGuestBackingOffset(addr, size, &backing)) return;
	auto a = MetalBufferManager::Instance().GetBufferForGuestRange(addr, size);
	if (a.buffer == nil || a.offset + size > a.buffer.length) return;
	auto encoded = std::make_shared<std::vector<uint8_t>>(size);
	std::memcpy(encoded->data(), reinterpret_cast<const void*>(addr), size);
	id<MTLBuffer> staging = [MetalContext::Instance().GetDevice() newBufferWithLength:size options:MTLResourceStorageModeShared];
	if (staging == nil) return;
	GdsBlit(a.buffer, a.offset, staging, 0, size);
	id<MTLCommandBuffer> cmd = MetalContext::Instance().GetCurrentCommandBuffer();
	if (cmd == nil) return;
	const uint64_t flip = TraceWindow::g_flip.load(std::memory_order_relaxed);
	[cmd addCompletedHandler:^(id<MTLCommandBuffer> c) {
		if (c.status != MTLCommandBufferStatusCompleted) return;
		const uint64_t n = s_race_checks.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto* gpu = static_cast<const uint32_t*>(staging.contents);
		const auto* enc = reinterpret_cast<const uint32_t*>(encoded->data());
		uint64_t diff = 0, first = UINT64_MAX;
		for (uint64_t i = 0; i < size / 4; ++i)
			if (gpu[i] != enc[i]) {
				if (first == UINT64_MAX) first = i;
				++diff;
			}
		if (diff == 0) {
			if (n % 20000 == 0) std::fprintf(stderr, "[RaceWatch] checks=%llu diffs=%llu\n", (unsigned long long)n, (unsigned long long)s_race_diffs.load());
			return;
		}
		s_race_diffs.fetch_add(1, std::memory_order_relaxed);
		const bool cpu_range = addr < 0x8240000000ull;
		if (cpu_range || s_race_logged.fetch_add(1, std::memory_order_relaxed) < 100) {
			std::fprintf(stderr, "[RaceWatch] DIFF cs=0x%016llx b%zu addr=0x%llx size=%llu encode_flip=%llu now_flip=%llu dwords=%llu first=+0x%llx enc=%08x gpu=%08x%s\n",
			             (unsigned long long)native, slot, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)flip,
			             (unsigned long long)TraceWindow::g_flip.load(std::memory_order_relaxed), (unsigned long long)diff,
			             (unsigned long long)(first * 4), enc[first], gpu[first], cpu_range ? " CPU-RANGE" : "");
		}
		// The GPU thread stops at the next watched dispatch: a completion handler must not take the submission gate.
		if (stop && cpu_range) s_race_stop.store(true, std::memory_order_release);
	}];
}
// GPU thread, before a watched dispatch is encoded: drain, close the budget and exit after a CPU-range difference.
static void RaceWatchMaybeStop() {
	if (!s_race_stop.load(std::memory_order_acquire)) return;
	(void)MetalContext::Instance().GetCurrentCommandBuffer();
	MetalContext::Instance().CommitCommandBuffer(true);
	std::fprintf(stderr, "[RaceWatch] stopping on the first CPU-range difference (exit 78); checks=%llu diffs=%llu\n",
	             (unsigned long long)s_race_checks.load(), (unsigned long long)s_race_diffs.load());
	std::fflush(stderr);
	HostBudget::Finish();
	std::_Exit(78);
}
