#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace Libs::Graphics::Pm4 {

// GFX10 acquire and release GCR words have different layouts. Keep raw controls
// as well as normalized actions; range/sequence bits are not cache-action bits.
struct CacheOps {
	uint32_t instruction_inv=0;
	bool metadata_wb=false, metadata_inv=false;
	bool scalar_wb=false, scalar_inv=false, vector_inv=false, l1_inv=false;
	bool l2_inv=false, l2_wb=false, discard=false;
	uint32_t l1_range=0, l2_range=0, sequence=0, unknown=0;
	bool unshared=false;
	bool Any() const {
		return instruction_inv || metadata_wb || metadata_inv || scalar_wb || scalar_inv ||
		       vector_inv || l1_inv || l2_inv || l2_wb || discard || unknown != 0;
	}
};
inline CacheOps DecodeAcquireGcr(uint32_t gcr) {
	CacheOps c;
	c.instruction_inv=gcr&3u; c.l1_range=(gcr>>2)&3u;
	c.metadata_wb=(gcr&(1u<<4))!=0; c.metadata_inv=(gcr&(1u<<5))!=0;
	c.scalar_wb=(gcr&(1u<<6))!=0; c.scalar_inv=(gcr&(1u<<7))!=0;
	c.vector_inv=(gcr&(1u<<8))!=0; c.l1_inv=(gcr&(1u<<9))!=0;
	c.unshared=(gcr&(1u<<10))!=0; c.l2_range=(gcr>>11)&3u;
	c.discard=(gcr&(1u<<13))!=0; c.l2_inv=(gcr&(1u<<14))!=0;
	c.l2_wb=(gcr&(1u<<15))!=0; c.sequence=(gcr>>16)&3u;
	c.unknown=gcr&~0x3ffffu;
	return c;
}
inline CacheOps DecodeReleaseGcr(uint32_t gcr) {
	CacheOps c;
	c.metadata_wb=(gcr&1u)!=0; c.metadata_inv=(gcr&2u)!=0;
	c.vector_inv=(gcr&4u)!=0; c.l1_inv=(gcr&8u)!=0;
	c.unshared=(gcr&16u)!=0; c.l2_range=(gcr>>5)&3u;
	c.discard=(gcr&128u)!=0; c.l2_inv=(gcr&256u)!=0;
	c.l2_wb=(gcr&512u)!=0; c.sequence=(gcr>>10)&3u;
	c.unknown=gcr&~0xfffu;
	return c;
}
struct SyncRange {
	uint64_t base=0, size=0;
	bool full=false;
};
enum class AcquireEncoding { Legacy, Gfx10, Translated };
struct AcquireMemInfo {
	AcquireEncoding encoding=AcquireEncoding::Legacy;
	SyncRange range;
	CacheOps cache;
	uint32_t coher_control=0, gcr_control=0, poll_interval=0;
	bool engine_select=false;
};

inline std::optional<AcquireMemInfo> DecodeAcquireMem(uint32_t header, std::span<const uint32_t> payload) {
	AcquireMemInfo result;
	switch (header & ~3u) { // Predicate and shader-type bits don't change fields.
	case 0xc0055800u: result.encoding=AcquireEncoding::Legacy; break;
	case 0xc0065800u: result.encoding=AcquireEncoding::Gfx10; break;
	case 0xc0061050u: result.encoding=AcquireEncoding::Translated; break;
	default: return std::nullopt;
	}
	const auto count=result.encoding==AcquireEncoding::Legacy ? 6u : 7u;
	if (payload.size()!=count || (payload[2]&~0xffu)!=0 || (payload[4]&~0xffffffu)!=0) return std::nullopt;
	result.coher_control=payload[0]&0x7fffffffu;
	result.engine_select=(payload[0]>>31)!=0;
	const uint64_t units=payload[1]|(uint64_t(payload[2])<<32);
	result.range.base=(uint64_t(payload[3])|(uint64_t(payload[4])<<32))<<8;
	result.range.size=units<<8;
	// PAL hardware packets use all size bits set; the AGC translated packet
	// represents its size=-1 sentinel as zero (see AgcDcbAcquireMem).
	result.range.full=(result.range.base==0 && units==0xffffffffffull) ||
	    (result.encoding==AcquireEncoding::Translated && units==0);
	if (!result.range.full && result.range.size>UINT64_MAX-result.range.base) return std::nullopt;
	result.poll_interval=payload[5]&0xffffu;
	if (count==7) { result.gcr_control=payload[6]; result.cache=DecodeAcquireGcr(payload[6]); }
	return result;
}

struct ReleaseMemInfo {
	CacheOps cache;
	uint32_t event_type=0, event_index=0, gcr_control=0, cache_policy=0;
	uint32_t destination=0, data_select=0, interrupt_select=0, interrupt_context=0;
	bool translated=false, wait_dma=false, execute=false;
	uint32_t mes_control=0;
	uint64_t label_address=0, value=0;
};
inline std::optional<ReleaseMemInfo> DecodeReleaseMem(uint32_t header, std::span<const uint32_t> payload) {
	const uint32_t opcode=header&~3u;
	if ((opcode!=0xc0061060u && opcode!=0xc0064900u) || payload.size()!=7) return std::nullopt;
	ReleaseMemInfo r;
	r.translated=opcode==0xc0061060u;
	r.event_type=payload[0]&0x3fu; r.event_index=(payload[0]>>8)&(r.translated?7u:15u);
	r.gcr_control=(payload[0]>>12)&0xfffu; r.cache_policy=(payload[0]>>25)&3u;
	r.cache=DecodeReleaseGcr(r.gcr_control);
	r.wait_dma=(payload[0]&(1u<<7))!=0; r.execute=(payload[0]&(1u<<28))!=0;
	r.mes_control=(payload[1]>>20)&15u;
	r.destination=(payload[1]>>16)&3u; r.data_select=(payload[1]>>29)&7u;
	r.interrupt_select=(payload[1]>>24)&7u;
	r.label_address=payload[2]|(uint64_t(payload[3])<<32);
	r.value=payload[4]|(uint64_t(payload[5])<<32);
	// The translated AGC ABI has 27 context bits; hardware has 28.
	r.interrupt_context=payload[6]&(r.translated?0x07ffffffu:0x0fffffffu);
	return r;
}

} // namespace Libs::Graphics::Pm4
