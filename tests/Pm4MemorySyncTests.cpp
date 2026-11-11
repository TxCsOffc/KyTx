#include "graphics/guest_gpu/command_processor/pm4MemorySync.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using namespace Libs::Graphics::Pm4;
static void Check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main() {
	try {
		std::array<uint32_t,7> p {0x80000010u,0x12345678u,0xabu,0x76543210u,0x123456u,0xffff0007u,0};
		auto a=DecodeAcquireMem(0xc0065800u,p);
		Check(a && a->range.base==0x1234567654321000ull && a->range.size==0xab1234567800ull && a->engine_select &&
		      a->coher_control==0x10 && a->poll_interval==7 && !a->range.full,"GFX10 fields/units/high bits");
		auto legacy=DecodeAcquireMem(0xc0055800u,std::span(p).first(6));
		Check(legacy && legacy->range.base==a->range.base && legacy->encoding==AcquireEncoding::Legacy,"legacy fields");
		Check(!DecodeAcquireMem(0xc0055800u,p) && !DecodeAcquireMem(0xc0065800u,std::span(p).first(6)),"truncated/mismatched packet accepted");
		Check(!DecodeAcquireMem(0xc0065900u,p),"wrong opcode accepted");
		p={0,0xffffffffu,0xff,0,0,1,0};
		Check(DecodeAcquireMem(0xc0065800u,p)->range.full,"hardware full range sentinel");
		p={0,0,0,0,0,0,0};
		Check(!DecodeAcquireMem(0xc0065800u,p)->range.full && DecodeAcquireMem(0xc0061050u,p)->range.full,"hardware/translated zero size conflated");
		p[2]=0x100; Check(!DecodeAcquireMem(0xc0065800u,p),"reserved size bits accepted");
		p={0,0xffffffffu,0xff,0xffffffffu,0xffffffu,0,0};
		Check(!DecodeAcquireMem(0xc0065800u,p),"base+size overflow accepted");
		unsigned checks=0;
		for(uint32_t gcr=0;gcr<0x40000;++gcr) {
			const auto c=DecodeAcquireGcr(gcr);
			const uint32_t rebuilt=c.instruction_inv|(c.l1_range<<2)|(uint32_t(c.metadata_wb)<<4)|(uint32_t(c.metadata_inv)<<5)|
			    (uint32_t(c.scalar_wb)<<6)|(uint32_t(c.scalar_inv)<<7)|(uint32_t(c.vector_inv)<<8)|(uint32_t(c.l1_inv)<<9)|
			    (uint32_t(c.unshared)<<10)|(c.l2_range<<11)|(uint32_t(c.discard)<<13)|(uint32_t(c.l2_inv)<<14)|
			    (uint32_t(c.l2_wb)<<15)|(c.sequence<<16);
			Check(rebuilt==gcr && c.unknown==0,"acquire GCR field loss"); ++checks;
		}
		for(uint32_t gcr=0;gcr<4096;++gcr) {
			const auto c=DecodeReleaseGcr(gcr);
			const uint32_t rebuilt=uint32_t(c.metadata_wb)|(uint32_t(c.metadata_inv)<<1)|(uint32_t(c.vector_inv)<<2)|
			    (uint32_t(c.l1_inv)<<3)|(uint32_t(c.unshared)<<4)|(c.l2_range<<5)|(uint32_t(c.discard)<<7)|
			    (uint32_t(c.l2_inv)<<8)|(uint32_t(c.l2_wb)<<9)|(c.sequence<<10);
			Check(rebuilt==gcr && c.unknown==0,"release GCR field loss"); ++checks;
		}
		Check(DecodeAcquireGcr(1u<<31).unknown==(1u<<31) && DecodeReleaseGcr(1u<<15).unknown==(1u<<15),"unknown bits dropped");
		Check(DecodeAcquireGcr(1u<<8).vector_inv && DecodeReleaseGcr(1u<<8).l2_inv,"acquire/release layout conflated");
		p={0x04000000u|0x2fu|(6u<<8)|(0x302u<<12), (2u<<29)|(1u<<16)|(2u<<24),0x12345678,0x82,0xdeadbeef,0x01020304,0xfffffff};
		auto r=DecodeReleaseMem(0xc0061060u,p);
		Check(r && r->label_address==0x8212345678ull && r->value==0x01020304deadbeefull && r->data_select==2 &&
		      r->destination==1 && r->interrupt_select==2 && r->interrupt_context==0x7ffffff && r->cache.l2_wb && r->cache.l2_inv && r->cache.metadata_inv,
		      "release label/control fields");
		auto native=DecodeReleaseMem(0xc0064902u,p);
		Check(native && !native->translated && native->interrupt_context==0xfffffff && native->label_address==r->label_address &&
		      native->value==r->value && native->gcr_control==r->gcr_control,"native release fields/context bit 27/shader-type");
		p[0]|=(1u<<7)|(1u<<28)|(8u<<8); p[1]|=9u<<20;
		native=DecodeReleaseMem(0xc0064900u,p);
		Check(native && native->wait_dma && native->execute && native->mes_control==9 && native->event_index==14,"native release flags were lost");
		Check(!DecodeReleaseMem(0xc0064800u,p) && !DecodeReleaseMem(0xc0064900u,std::span(p).first(6)),"invalid release accepted");
		std::printf("PM4 memory sync: three acquire and two release encodings, ranges/sentinels/overflow, label/flags; %u GCR combinations passed\n",checks);
	} catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
