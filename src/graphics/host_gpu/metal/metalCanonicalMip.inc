// Eager canonical-memory adapter for private 2D mip targets. Ownership returns
// to physical guest bytes at every producer boundary; no frame-age or sampled
// hash can make a private copy authoritative. This intentionally trades copies
// for correctness until a lazy range tracker replaces it.
// Diagnostic scope: split each actual import, including imports reached during
// resource preparation, rather than only the final image-binding loop.
static thread_local bool s_split_canonical_imports=false;
class CanonicalImportDiagnosticScope {
public:
	explicit CanonicalImportDiagnosticScope(bool enabled):previous_(s_split_canonical_imports){s_split_canonical_imports=enabled;}
	~CanonicalImportDiagnosticScope(){s_split_canonical_imports=previous_;}
private:
	bool previous_;
};
static std::map<uint64_t,uint64_t> s_canonical_mip_ranges; // physical [begin,end)
static uint64_t s_canonical_mapping_generation=UINT64_MAX;
static std::vector<PhysicalMapping> s_canonical_mappings;
static bool CanonicalMipSpans(uint64_t addr,uint64_t bytes,std::vector<PhysicalSpan>& spans) {
	std::vector<LibKernel::Memory::GuestBackingMapping> updated;
	if(LibKernel::Memory::GetGuestBackingMappings(s_canonical_mapping_generation,updated)) {
		s_canonical_mappings.clear();for(const auto&m:updated)s_canonical_mappings.push_back({m.address,m.size,m.backing_offset});
	}
	return ResolvePhysicalSpans(s_canonical_mappings,addr,bytes,spans);
}
static bool CanonicalMipOverlap(uint64_t addr,uint64_t size) {
	std::vector<PhysicalSpan> spans;
	if(size==0 || s_canonical_mip_ranges.empty() || !CanonicalMipSpans(addr,size,spans))return false;
	for(const auto&s:spans) {
		auto it=s_canonical_mip_ranges.upper_bound(s.physical);
		if(it!=s_canonical_mip_ranges.begin() && std::prev(it)->second>s.physical)return true;
		if(it!=s_canonical_mip_ranges.end() && it->first-s.physical<s.size)return true;
	}return false;
}
static void RegisterCanonicalMipRange(uint64_t addr,uint64_t size) {
	std::vector<PhysicalSpan> spans;
	if(!CanonicalMipSpans(addr,size,spans))RejectMetalState("canonical mip target has unmapped physical bytes");
	for(const auto&s:spans) {
		uint64_t begin=s.physical,end=begin+s.size;
		auto it=s_canonical_mip_ranges.lower_bound(begin);
		if(it!=s_canonical_mip_ranges.begin() && std::prev(it)->second>=begin)--it;
		while(it!=s_canonical_mip_ranges.end() && it->first<=end) {
			begin=std::min(begin,it->first);end=std::max(end,it->second);it=s_canonical_mip_ranges.erase(it);
		}s_canonical_mip_ranges[begin]=end;
	}
}
static CanonicalMemoryView CanonicalMipMemory(uint64_t addr,uint64_t bytes) {
	std::vector<PhysicalSpan> spans;
	if(!CanonicalMipSpans(addr,bytes,spans))RejectMetalState("canonical mip memory has unmapped bytes");
	CanonicalMemoryView view;
	const uint64_t chunk=MetalBufferManager::kBackingChunkSize;
	if(!view.Initialize(MetalContext::Instance().GetDevice(),spans,bytes,chunk,[=](uint64_t physical,uint64_t count)->std::optional<CanonicalMemoryView::BufferSpan> {
		auto buffer=MetalBufferManager::Instance().GetBackingChunk(physical/chunk);
		if(!buffer || buffer->GetLength()<physical%chunk+count)return std::nullopt;
		return CanonicalMemoryView::BufferSpan{buffer->GetHandle(),buffer->GetOffset()+physical%chunk};
	}))RejectMetalState("canonical mip physical Metal buffer resolution failed");
	return view;
}

static bool CanonicalSamePhysical(uint64_t a,uint64_t b,uint64_t size) {
	std::vector<PhysicalSpan> lhs,rhs;
	if(!CanonicalMipSpans(a,size,lhs) || !CanonicalMipSpans(b,size,rhs) || lhs.size()!=rhs.size())return false;
	for(size_t i=0;i<lhs.size();++i)if(lhs[i].offset!=rhs[i].offset || lhs[i].physical!=rhs[i].physical || lhs[i].size!=rhs[i].size)return false;
	return true;
}
static DepthMemoryTransfer ConfigureCanonicalDepthPlane(MetalDepthStencilAttachmentConfig& attachment,
    uint64_t read_addr,uint64_t write_addr,uint32_t first,uint32_t count,bool writable) {
	const auto fmt=attachment.texture.pixelFormat;
	const uint32_t eb=fmt==MTLPixelFormatDepth16Unorm?2:fmt==MTLPixelFormatDepth32Float?4:fmt==MTLPixelFormatStencil8?1:0;
	TileSurfaceLayout layout{};
	if(!eb || !TileGetTiledTextureLayout({RepresentativeFormat(eb),Prospero::TileMode::kDepth,TileSurfaceDimension::Dim2D,
	    uint32_t(attachment.texture.width),uint32_t(attachment.texture.height),1,1,first+count},layout))
		RejectMetalState("canonical depth/stencil layout unavailable");
	if(writable && (!write_addr || !CanonicalSamePhysical(read_addr,write_addr,layout.total_size)))
		RejectMetalState("distinct depth/stencil read/write backing requires per-fragment store coverage");
	const auto read=CanonicalMipMemory(read_addr,layout.total_size);
	// Read-only attachments must not modify a separate write allocation.
	const auto write=writable?CanonicalMipMemory(write_addr,layout.total_size):read;
	DepthMemoryTransfer transfer;
	if(!transfer.Initialize(MetalContext::Instance().GetDevice(),attachment.texture,layout,first,count,read,write))
		RejectMetalState("canonical depth/stencil transfer initialization failed");
	RegisterCanonicalMipRange(read_addr,layout.total_size);
	if(writable)RegisterCanonicalMipRange(write_addr,layout.total_size);
	attachment.before_load=[transfer](id cmd){if(!transfer.Load(cmd))RejectMetalState("canonical depth/stencil load failed");};
	attachment.after_store=[transfer](id cmd){if(!transfer.Store(cmd))RejectMetalState("canonical depth/stencil store failed");};
	return transfer;
}

// Copies made and reused, for the [GuestWrites] report.
static std::atomic<uint64_t> s_canonical_image_imports{0},s_canonical_image_reuses{0},s_canonical_target_imports{0},s_canonical_target_skips{0};
// Write generation at which a canonical render target's level/slice last equalled its guest bytes (0: unknown).
static char s_canonical_target_sync_key;
static uint64_t CanonicalTargetSynced(id<MTLTexture> texture,uint32_t mip,uint32_t slice) {
	NSMutableDictionary* map=objc_getAssociatedObject(texture,&s_canonical_target_sync_key);
	NSNumber* value=map[@((uint64_t(mip)<<32)|slice)];
	return value?value.unsignedLongLongValue:0;
}
static void SetCanonicalTargetSynced(id<MTLTexture> texture,uint32_t mip,uint32_t slice,uint64_t generation) {
	NSMutableDictionary* map=objc_getAssociatedObject(texture,&s_canonical_target_sync_key);
	if(!map) {map=[NSMutableDictionary dictionary];objc_setAssociatedObject(texture,&s_canonical_target_sync_key,map,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
	map[@((uint64_t(mip)<<32)|slice)]=@(generation);
}
static void ConfigureCanonicalMipTarget(MetalColorAttachmentConfig& color,uint64_t addr,Prospero::TileMode tile,
                                        uint32_t levels,uint32_t physical_slice) {
	const uint32_t eb=BytesPerPixel(color.texture.pixelFormat);
	TileSurfaceLayout layout{};
	if(tile==Prospero::TileMode::kLinear || !TileGetTiledTextureLayout({RepresentativeFormat(eb),tile,TileSurfaceDimension::Dim2D,
	    uint32_t(color.texture.width),uint32_t(color.texture.height),1,levels,physical_slice+1},layout))
		RejectMetalState("unsupported canonical mip render-target layout");
	const auto guest=CanonicalMipMemory(addr,layout.total_size);
	id<MTLTexture> raw=ReinterpretFormat(color.texture,TileElementFormat(eb));
	if(guest.buffer==nil || raw.pixelFormat!=TileElementFormat(eb))
		RejectMetalState("canonical mip render target cannot expose its exact bytes");
	RegisterCanonicalMipRange(addr,layout.total_size);
	id<MTLDevice> device=MetalContext::Instance().GetDevice();
	const uint32_t mip=color.level;
	id<MTLTexture> target=color.texture;
	auto loaded=std::make_shared<bool>(false);
	color.before_load=[device,raw,guest,layout,mip,physical_slice,addr,target,loaded](id command) {
		*loaded=true;
		// The texture already holds these guest bytes: nothing wrote the surface since it was last loaded or stored.
		const uint64_t synced=CanonicalTargetSynced(target,mip,physical_slice);
		if(synced!=0 && GuestWrites::Current(addr,layout.total_size,synced)) {++s_canonical_target_skips;return;}
		++s_canonical_target_imports;
		GuestWrites::Watch(addr,layout.total_size);
		SetCanonicalTargetSynced(target,mip,physical_slice,GuestWrites::Begin());
		if(!guest.Gather(command) || !EncodeTileTextureImport(device,command,guest.buffer,guest.offset,raw,layout,mip,physical_slice,mip)) {
			const auto& f=layout.mips[mip];TileTables t{};const bool ok=BuildTileTables(layout.texture.block,f.padded_width,f.padded_height,t);
			std::fprintf(stderr,"[CanonicalRenderReject] addr=%llx raw=%lux%lu fmt=%lu type=%lu mips=%lu arrays=%lu mip=%u slice=%u layout=%ux%u levels=%u layers=%u bytes=%llu mip_size=%ux%u padded=%ux%u off=%llu table=%d extent=%llu buffer=%lu offset=%llu\n",
			    (unsigned long long)addr,(unsigned long)raw.width,(unsigned long)raw.height,(unsigned long)raw.pixelFormat,(unsigned long)raw.textureType,(unsigned long)raw.mipmapLevelCount,(unsigned long)raw.arrayLength,mip,physical_slice,
			    layout.description.width,layout.description.height,layout.description.levels,layout.description.layers,(unsigned long long)layout.total_size,
			    f.width,f.height,f.padded_width,f.padded_height,(unsigned long long)f.offset,ok,(unsigned long long)t.columns*t.rows*t.block.block_size,(unsigned long)guest.buffer.length,(unsigned long long)guest.offset);
			RejectMetalState("canonical mip render load failed");
		}
	};
	color.after_store_regions=[device,raw,guest,layout,mip,physical_slice,addr,target,loaded](id command,std::span<const PixelRect> regions) {
		bool full=*loaded;
		const uint32_t mw=std::max(uint32_t(target.width>>mip),1u),mh=std::max(uint32_t(target.height>>mip),1u);
		for(const auto rect:regions) {
			std::vector<FootprintRange> footprint;
			if(!TiledMipRectFootprint(layout,mip,physical_slice,rect,footprint) ||
			    !EncodeTileTextureRectExport(device,command,raw,guest.buffer,guest.offset,layout,rect,mip,physical_slice,mip) || !guest.Scatter(command,footprint))
				RejectMetalState("canonical mip render region store failed");
			full=full || (rect.x==0 && rect.y==0 && rect.width>=mw && rect.height>=mh);
		}
		// Loaded (or cleared) and every written rectangle exported: the level equals its guest bytes. A pass that did not load
		// leaves the rest of the level undefined, so it is not a copy of anything.
		if(full)GuestWrites::Watch(addr,layout.total_size);
		SetCanonicalTargetSynced(target,mip,physical_slice,full?GuestWrites::Begin():0);
		*loaded=false;
	};
}

struct CanonicalMipBinding {
	id<MTLTexture> image=nil;
	std::function<void(id)> writeback;
	PhysicalBackingSnapshot physical;
};
#include "metalCanonicalVolume.inc"
static CanonicalMipBinding CanonicalMipImage(const ShaderTextureResource& tex,uint32_t w,uint32_t h,uint64_t addr,bool r128) {
	if(tex.Type()==Prospero::ImageType::kColor3D)return CanonicalVolumeImage(tex,w,h,addr,r128);
	if(addr==0 || s_canonical_mip_ranges.empty() || tex.TileMode()==Prospero::TileMode::kLinear ||
	    (tex.Type()!=Prospero::ImageType::kColor2D && tex.Type()!=Prospero::ImageType::kColor2DArray))return {};
	const bool array=tex.Type()==Prospero::ImageType::kColor2DArray;
	const uint32_t first_layer=tex.BaseArray5(),physical_layers=array?uint32_t(tex.Depth())+1u:first_layer+1u;
	if(first_layer>=physical_layers)RejectMetalState("canonical image invalid array range");
	const uint32_t layers=array?physical_layers-first_layer:1u;
	GuestMipViewPlan plan;
	TileSurfaceLayout layout{};
	if(!PlanGuestMipView(tex,w,h,r128,plan) || !TileGetTiledTextureLayout({tex.Format(),tex.TileMode(),TileSurfaceDimension::Dim2D,
	    w,h,1,plan.physical_levels,physical_layers},layout))return {};
	// This must run before both sampled and storage bindings: otherwise a compute
	// writer can create a detached image at a remapped VA before graphics sees it.
	const bool current_memory=!CanonicalMipOverlap(addr,layout.total_size);
	if(current_memory && !CanonicalTargetWasRemapped(addr))return {};
	const MTLPixelFormat fmt=MetalFormatFromBufferFormat(tex.Format());
	const bool bc=Prospero::BlockCompressedBytesPerBlock(tex.Format())!=0;
	if(fmt==MTLPixelFormatInvalid)RejectMetalState("canonical image has unknown Metal format");
	if(bc && !MetalContext::Instance().GetDevice().supportsBCTextureCompression)RejectMetalState("canonical BC image requires device BC support");

	// Cache within a single draw/dispatch only. Reimport on the next command,
	// including CPU and raw-buffer updates, rather than assuming an old image wins.
	using Key=std::tuple<uint64_t,uint32_t,uint32_t,std::array<uint32_t,8>,bool>;
	struct Entry {CanonicalMipBinding binding;uint64_t epoch=0,flip=0;std::vector<PhysicalSpan> physical;
		std::shared_ptr<uint64_t> generation=std::make_shared<uint64_t>(0);};
	static std::map<Key,Entry> cache;
	std::array<uint32_t,8> words;std::copy_n(tex.fields,8,words.begin());
	const Key key{addr,w,h,words,r128};
	const uint64_t flip=TraceWindow::g_flip.load();
	for(auto it=cache.begin();it!=cache.end();) {if(flip>it->second.flip+120)it=cache.erase(it);else ++it;}
	auto& entry=cache[key];
	std::vector<PhysicalSpan> physical;
	if(!CanonicalMipSpans(addr,layout.total_size,physical))RejectMetalState("canonical image mapping disappeared");
	// Reuse the copy while no write reached its pages since it was made (or written back); the epoch keeps one copy within a
	// draw/dispatch as before.
	if(entry.binding.image && entry.physical==physical &&
	    (entry.epoch==s_image_read_epoch || (*entry.generation!=0 && GuestWrites::Current(addr,layout.total_size,*entry.generation)))) {
		if(entry.epoch!=s_image_read_epoch)++s_canonical_image_reuses;
		entry.epoch=s_image_read_epoch;entry.flip=flip;
		return entry.binding;
	}
	++s_canonical_image_imports;
	const auto guest=CanonicalMipMemory(addr,layout.total_size);
	if(current_memory) {
		RegisterCanonicalMipRange(addr,layout.total_size);
		std::fprintf(stderr,"[RemappedImage] addr=%llx type=%u format=%u size=%ux%u bytes=%llu\n",
		    (unsigned long long)addr,unsigned(tex.Type()),unsigned(tex.Format()),w,h,(unsigned long long)layout.total_size);
	}
	id<MTLDevice> device=MetalContext::Instance().GetDevice();
	id<MTLTexture> image=entry.binding.image;
	if(image==nil) {
		MTLTextureDescriptor* d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
		    width:std::max(w>>plan.first_level,1u) height:std::max(h>>plan.first_level,1u) mipmapped:plan.texture_levels>1];
		d.mipmapLevelCount=plan.texture_levels;d.storageMode=MTLStorageModePrivate;
		if(array){d.textureType=MTLTextureType2DArray;d.arrayLength=layers;}
		d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView|(bc?0:MTLTextureUsageShaderWrite);
		image=KytyNewTexture(device,d,__LINE__);
	}
	id<MTLTexture> raw=bc?image:ReinterpretFormat(image,TileElementFormat(layout.texture.block.bytes_per_element));
	if(image==nil || (!bc && raw.pixelFormat!=TileElementFormat(layout.texture.block.bytes_per_element)))RejectMetalState("canonical mip raw image view failed");
	const bool reopen=MetalContext::Instance().GetActiveRenderEncoder()!=nil && s_last_pass_valid;
	MetalDrawExecutor::Instance().EndRenderPass();
	id<MTLCommandBuffer> command=MetalContext::Instance().GetCurrentCommandBuffer();
	if(s_split_canonical_imports) {
		std::fprintf(stderr,"[CanonicalImport] drain prior work addr=%llx tile=%u %ux%u\n",
		    (unsigned long long)addr,unsigned(tex.TileMode()),w,h);
		std::fflush(stderr);
		MetalContext::Instance().CommitCommandBuffer(true);
		command=MetalContext::Instance().GetCurrentCommandBuffer();
		char label[192];std::snprintf(label,sizeof(label),"canonical import addr=0x%llx tile=%u %ux%u fmt=%u",
		    (unsigned long long)addr,unsigned(tex.TileMode()),w,h,unsigned(tex.Format()));
		KytyFaultOpSet(label);command.label=[NSString stringWithUTF8String:label];
		std::fprintf(stderr,"[CanonicalImport] begin %s\n",label);std::fflush(stderr);
	}
	GuestWrites::Watch(addr,layout.total_size);
	*entry.generation=GuestWrites::Begin();
	if(!guest.Gather(command))RejectMetalState("canonical mip gather failed");
	for(uint32_t layer=0;layer<layers;++layer)for(uint32_t m=0;m<plan.texture_levels;++m)
		if(!(bc?EncodeTileBcImport(device,command,guest.buffer,guest.offset,raw,layout,plan.first_level+m,first_layer+layer,m,layer):
		    EncodeTileTextureImport(device,command,guest.buffer,guest.offset,raw,layout,plan.first_level+m,first_layer+layer,m,layer))) {
			const auto& failed=layout.mips[plan.first_level+m];
			TileTables table{};const bool table_ok=BuildTileTables(layout.texture.block,failed.padded_width,failed.padded_height,table);
			std::fprintf(stderr,"[CanonicalImportReject] addr=%llx format=%u type=%u tile=%u %ux%u phys_levels=%u first=%u raw=%lux%lu levels=%lu type=%lu fmt=%lu usage=%lu arrays=%lu src_mip=%u dst_mip=%u src_layer=%u dst_layer=%u bytes=%llu buf_off=%llu buf_bytes=%lu mip=%ux%u padded=%ux%u offset=%llu slice=%llu table=%d extent=%llu\n",
			    (unsigned long long)addr,unsigned(tex.Format()),unsigned(tex.Type()),unsigned(tex.TileMode()),w,h,plan.physical_levels,plan.first_level,
			    (unsigned long)raw.width,(unsigned long)raw.height,(unsigned long)raw.mipmapLevelCount,(unsigned long)raw.textureType,(unsigned long)raw.pixelFormat,(unsigned long)raw.usage,(unsigned long)raw.arrayLength,
			    plan.first_level+m,m,first_layer+layer,layer,(unsigned long long)layout.total_size,(unsigned long long)guest.offset,(unsigned long)guest.buffer.length,
			    failed.width,failed.height,failed.padded_width,failed.padded_height,(unsigned long long)failed.offset,(unsigned long long)layout.block_slice_size,table_ok,(unsigned long long)table.columns*table.rows*table.block.block_size);
			RejectMetalState("canonical mip image import failed");
		}
	if(s_split_canonical_imports) {
		MetalContext::Instance().CommitCommandBuffer(true);
		std::fprintf(stderr,"[CanonicalImport] completed addr=%llx tile=%u %ux%u\n",
		    (unsigned long long)addr,unsigned(tex.TileMode()),w,h);std::fflush(stderr);
	}
	if(reopen) {
		MetalRenderPassDesc pass=s_last_pass_desc;
		for(uint32_t i=0;i<pass.color_attachment_count;++i)pass.color_attachments[i].load_action=MTLLoadActionLoad;
		if(pass.has_depth)pass.depth_attachment.load_action=MTLLoadActionLoad;
		if(pass.has_stencil)pass.stencil_attachment.load_action=MTLLoadActionLoad;
		if(!MetalDrawExecutor::Instance().BeginRenderPass(pass))RejectMetalState("canonical mip render reopen failed");
	}
	const uint32_t base=tex.BaseLevel(),last=tex.LastLevel();
	entry.binding.image=image;
	entry.binding.physical={layout.total_size,physical};
	entry.binding.writeback=[device,raw,guest,layout,plan,base,last,first_layer,layers,bc,generation=entry.generation](id command) {
		for(uint32_t layer=0;layer<layers;++layer)for(uint32_t m=base;m<=last;++m) {
			const uint32_t target=plan.first_level?0:m;
			std::vector<FootprintRange> footprint;
			if(!TiledMipFootprint(layout,m,first_layer+layer,footprint) ||
			    !(bc?EncodeTileBcExport(device,command,raw,guest.buffer,guest.offset,layout,m,first_layer+layer,target,layer):
			    EncodeTileTextureExport(device,command,raw,guest.buffer,guest.offset,layout,m,first_layer+layer,target,layer)) || !guest.Scatter(command,footprint))
				RejectMetalState("canonical mip storage image export failed");
		}
		// The written levels went back to guest memory and the others were imported from it: the image equals its bytes.
		*generation=GuestWrites::Begin();
	};
	entry.epoch=s_image_read_epoch;entry.flip=flip;entry.physical=std::move(physical);
	static uint32_t logged=0;
	if(logged++<32)std::fprintf(stderr,"[CanonicalMip] addr=%llx %ux%u format=%u physical=%u active=%u..%u bytes=%llu\n",
	    (unsigned long long)addr,w,h,unsigned(tex.Format()),plan.physical_levels,base,last,(unsigned long long)layout.total_size);
	return entry.binding;
}
