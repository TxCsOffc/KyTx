#include "graphics/host_gpu/memoryCoherence.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>

using namespace Libs::Graphics::Coherence;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() {
	try {
		MemoryCoherence tracker;
		Check(tracker.RegisterBacking(7,128), "register backing");
		Check(!tracker.RegisterBacking(7,128), "duplicate backing accepted");
		Check(tracker.RegisterView(1,{{7,0,128}}) && tracker.RegisterView(2,{{7,0,128}}), "register views");
		Check(!tracker.RecordWrite(1,{7,0,128},WriteMode::PreserveUnwritten), "partial write accepted without preserved contents");
		auto initial=tracker.PlanRead(1,{7,0,128});
		Check(initial && initial->copies.size()==1 && initial->copies[0].owner==GuestBytes && initial->copies[0].version==0,
		      "initial bytes not sourced from guest allocation");
		Check(tracker.CompleteRead(*initial), "initial read completion");
		Check(tracker.RecordWrite(1,{7,0,128},WriteMode::PreserveUnwritten), "preserved partial writer rejected");
		Check(tracker.RecordWrite(GuestBytes,{7,17,3},WriteMode::OverwriteRange), "CPU partial overwrite");
		auto raw=tracker.PlanRead(GuestBytes,{7,0,128});
		Check(raw && raw->copies.size()==2 && raw->copies[0].range==Range{7,0,17} && raw->copies[1].range==Range{7,20,108},
		      "GPU writeback clobbers a newer CPU subrange");
		Check(!tracker.ForgetView(1), "dirty writer evicted");
		Check(tracker.CompleteRead(*raw) && tracker.ForgetView(1), "materialized writer cannot be retired");
		Check(!tracker.RegisterView(1,{{7,0,128}}), "retired resource ID reused");
		auto stale=tracker.PlanRead(2,{7,0,8});
		Check(stale.has_value(), "plan stale read");
		Check(tracker.RecordWrite(GuestBytes,{7,2,1},WriteMode::OverwriteRange), "CPU overlap update");
		Check(!tracker.CompleteRead(*stale), "stale read was published");
		auto disjoint=tracker.PlanRead(2,{7,0,8});
		Check(tracker.RecordWrite(GuestBytes,{7,80,1},WriteMode::OverwriteRange), "disjoint update");
		Check(tracker.CompleteRead(*disjoint), "disjoint write invalidated unrelated planned read");
		Check(!tracker.PlanRead(2,{7,UINT64_MAX,8}) && !tracker.PlanRead(2,{7,0,0}) && !tracker.PlanRead(2,{99,0,8}), "bad range accepted");
		Check(tracker.RegisterView(3,{{7,8,8},{7,16,8}}) && tracker.PlanRead(3,{7,8,16}).has_value(), "adjacent coverage not merged");
		Check(!tracker.PlanRead(3,{7,7,16}), "view read outside footprint");

		// Dense byte arrays are the independent oracle; they do not use an
		// interval map or version logic. Every transfer copies actual bytes.
		MemoryCoherence fuzz;
		constexpr size_t owners=5, backings=3, bytes=127;
		using Data=std::array<std::array<uint8_t,bytes>,backings>;
		std::array<Data,owners> data {};
		Data truth {};
		std::mt19937 rng(0x4d454d);
		for (size_t b=0;b<backings;++b) {
			Check(fuzz.RegisterBacking(b,bytes), "fuzz backing");
			for (size_t i=0;i<bytes;++i) data[0][b][i]=truth[b][i]=uint8_t(rng());
		}
		for (size_t owner=1;owner<owners;++owner) {
			Check(fuzz.RegisterView(owner,{{0,0,bytes},{1,0,bytes},{2,0,bytes}}), "fuzz view");
			for (auto& buffer:data[owner]) buffer.fill(0xcd);
		}
		auto copy=[&](const Stamp& from,OwnerId to) {
			std::memcpy(data[to][from.range.backing].data()+from.range.begin,
			            data[from.owner][from.range.backing].data()+from.range.begin,from.range.size);
			return true;
		};
		unsigned reads=0,writes=0;
		for (unsigned step=0;step<20000;++step) {
			const size_t b=rng()%backings, owner=rng()%owners, begin=rng()%bytes, count=1+rng()%(bytes-begin);
			const Range range{b,begin,count};
			switch (rng()%3) {
			case 0:
				Check(fuzz.Read(owner,range,copy), "random read failed");
				Check(std::memcmp(data[owner][b].data()+begin,truth[b].data()+begin,count)==0, "random read returned stale bytes");
				++reads; break;
			case 1:
				for(size_t i=begin;i<begin+count;++i) data[owner][b][i]=truth[b][i]=uint8_t(rng());
				Check(fuzz.RecordWrite(owner,range,WriteMode::OverwriteRange), "random full write failed");
				++writes; break;
			case 2:
				// A shader writes just part of a view but claims a conservative
				// full footprint; untouched values must have been imported first.
				Check(fuzz.Read(owner,{b,0,bytes},copy), "RMW prefill failed");
				for(size_t i=begin;i<begin+count;++i) data[owner][b][i]=truth[b][i]=uint8_t(rng());
				Check(fuzz.RecordWrite(owner,{b,0,bytes},WriteMode::PreserveUnwritten), "random partial write failed");
				++writes; break;
			}
		}
		for (size_t owner=0;owner<owners;++owner) for (size_t b=0;b<backings;++b) {
			Check(fuzz.Read(owner,{b,0,bytes},copy), "final materialization");
			Check(data[owner][b]==truth[b], "final full backing mismatch");
		}
		Check(fuzz.RecordWrite(1,{0,0,bytes},WriteMode::OverwriteRange), "failure setup");
		Check(!fuzz.Read(2,{0,0,bytes},[](auto,auto){return false;}), "copy failure was ignored");
		Check(!fuzz.PlanRead(2,{0,0,bytes})->copies.empty(), "failed copy published a replica");
		std::printf("MemoryCoherence: range/alias preservation, stale plans, eviction, ID lifetime, bounds; 20000 byte-oracle operations (%u reads, %u writes) passed\n",reads,writes);
	} catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
