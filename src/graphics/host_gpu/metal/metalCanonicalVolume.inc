// Every tiled volume uses physical memory from its first producer. Reusing a
// VA, changing an SRD, CPU writes and raw GPU writes all reimport current bytes.
static CanonicalMipBinding CanonicalVolumeImage(const ShaderTextureResource& tex,uint32_t w,uint32_t h,uint64_t addr,bool r128) {
	const uint32_t depth=uint32_t(tex.Depth())+1u;
	if(!addr || tex.BaseArray5()!=0)RejectMetalState("invalid canonical 3D image descriptor");
	if(Prospero::BlockCompressedBytesPerBlock(tex.Format()))RejectMetalState("compressed 3D physical transfer is not implemented");
	GuestMipViewPlan plan;TileSurfaceLayout layout{};
	if(!PlanGuestMipView(tex,w,h,r128,plan,depth) || !PlanVolumeLayout({tex.Format(),tex.TileMode(),TileSurfaceDimension::Dim3D,
	    w,h,depth,plan.physical_levels,1},layout))RejectMetalState("canonical 3D physical layout unavailable");
	const auto format=MetalFormatFromBufferFormat(tex.Format());
	if(format==MTLPixelFormatInvalid)RejectMetalState("canonical 3D Metal format unavailable");
	struct Entry {CanonicalMipBinding binding;uint64_t epoch=0,flip=0;std::vector<PhysicalSpan> physical;};
	static std::map<VolumeImageKey,Entry> cache;
	const auto key=MakeVolumeImageKey(addr,w,h,tex,r128);
	const uint64_t flip=TraceWindow::g_flip.load();
	for(auto it=cache.begin();it!=cache.end();) {if(flip>it->second.flip+120)it=cache.erase(it);else ++it;}
	auto& entry=cache[key];
	std::vector<PhysicalSpan> physical;
	if(!CanonicalMipSpans(addr,layout.total_size,physical))RejectMetalState("canonical 3D image has unmapped physical bytes");
	if(entry.binding.image && entry.epoch==s_image_read_epoch && entry.physical==physical)return entry.binding;
	id<MTLDevice> device=MetalContext::Instance().GetDevice();
	id<MTLTexture> image=entry.binding.image;
	if(!image) {
		MTLTextureDescriptor* d=[MTLTextureDescriptor new];d.textureType=MTLTextureType3D;d.pixelFormat=format;
		d.width=std::max(w>>plan.first_level,1u);d.height=std::max(h>>plan.first_level,1u);d.depth=std::max(depth>>plan.first_level,1u);
		d.mipmapLevelCount=plan.texture_levels;d.storageMode=MTLStorageModePrivate;
		d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
		image=KytyNewTexture(device,d,__LINE__);
	}
	const auto memory=CanonicalMipMemory(addr,layout.total_size);
	VolumeMemoryTransfer transfer;
	if(!transfer.Initialize(device,ReinterpretFormat(image,TileElementFormat(layout.texture.block.bytes_per_element)),layout,plan,
	    tex.BaseLevel(),tex.LastLevel(),memory))RejectMetalState("canonical 3D transfer initialization failed");
	RegisterCanonicalMipRange(addr,layout.total_size);
	const bool reopen=MetalContext::Instance().GetActiveRenderEncoder()!=nil && s_last_pass_valid;
	MetalDrawExecutor::Instance().EndRenderPass();
	if(!transfer.Load(MetalContext::Instance().GetCurrentCommandBuffer()))RejectMetalState("canonical 3D load failed");
	if(reopen) {
		MetalRenderPassDesc pass=s_last_pass_desc;
		for(uint32_t i=0;i<pass.color_attachment_count;++i)pass.color_attachments[i].load_action=MTLLoadActionLoad;
		if(pass.has_depth)pass.depth_attachment.load_action=MTLLoadActionLoad;
		if(pass.has_stencil)pass.stencil_attachment.load_action=MTLLoadActionLoad;
		if(!MetalDrawExecutor::Instance().BeginRenderPass(pass))RejectMetalState("canonical 3D render reopen failed");
	}
	entry.binding.image=image;entry.binding.physical={layout.total_size,physical};
	entry.binding.writeback=[transfer](id command){if(!transfer.Store(command))RejectMetalState("canonical 3D store failed");};
	entry.epoch=s_image_read_epoch;entry.flip=flip;entry.physical=std::move(physical);
	static uint32_t logged=0;
	if(logged++<48)std::fprintf(stderr,"[CanonicalVolume] addr=%llx %ux%ux%u format=%u physical=%u active=%u..%u bytes=%llu\n",
	    (unsigned long long)addr,w,h,depth,unsigned(tex.Format()),plan.physical_levels,tex.BaseLevel(),tex.LastLevel(),(unsigned long long)layout.total_size);
	return entry.binding;
}
