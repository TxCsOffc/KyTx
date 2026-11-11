// Write generations of guest physical pages (metalGuestWrites.h): when a cached copy of guest bytes is still exact.
#include "guestWritesKernelShim.h"
#include "graphics/host_gpu/metal/metalGuestWrites.inc"
#include "graphics/host_gpu/metal/metalShaderGuestWrites.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace GW = Libs::Graphics::Metal::GuestWrites;
using GuestWritesShim::ReadOnly;

static void Check(bool value, const char* message) {
	if (!value) throw std::runtime_error(message);
}
static unsigned s_checks = 0;
#define EXPECT(cond, msg) (++s_checks, Check((cond), (msg)))

int main() {
	try {
		constexpr uint64_t P = 0x4000;
		// Two guest aliases of one 4-page physical run, and an unrelated mapping.
		GuestWritesShim::g_mappings = {{0x100000000ull, 4 * P, 0x40000}, {0x200000000ull, 4 * P, 0x40000}, {0x300000000ull, 2 * P, 0x90000}};
		GuestWritesShim::g_generation++;
		const uint64_t A = 0x100000000ull, B = 0x200000000ull, C = 0x300000000ull;

		// Never watched: never current, whatever the generation.
		EXPECT(!GW::Current(A, 4 * P, GW::Begin()), "an unwatched range is current");

		GW::Watch(A + 100, 2 * P);                 // pages 0..2 of A (the range crosses into a third page)
		const uint64_t g = GW::Begin();
		EXPECT(GW::Current(A, 3 * P, g), "a watched, unwritten range is not current");
		EXPECT(GW::Current(B, 3 * P, g), "the alias of a watched range is not current");
		EXPECT(ReadOnly(A) && ReadOnly(A + 2 * P) && ReadOnly(B) && ReadOnly(B + 2 * P), "watched pages are not write-protected through every alias");
		EXPECT(!ReadOnly(A + 3 * P) && !ReadOnly(C), "pages outside the watch were protected");
		EXPECT(!GW::Current(A, 4 * P, g), "a range with an unwatched page is current");

		// A GPU write to another physical page leaves the copy current; one to its pages does not.
		GW::NoteWrite(C, P);
		EXPECT(GW::Current(A, 3 * P, g), "an unrelated GPU write made the copy stale");
		GW::NoteWrite(B + P + 8, 4);               // through the alias
		EXPECT(!GW::Current(A, 3 * P, g), "a GPU write through an alias left the copy current");
		EXPECT(GW::Current(A, P, g) && GW::Current(A + 2 * P, P, g), "a GPU write made unrelated pages stale");

		// A copy made after the write is current again.
		const uint64_t g2 = GW::Begin();
		EXPECT(GW::Current(A, 3 * P, g2), "a copy made after the write is stale");

		// CPU write fault through the second alias: handled, every alias unprotected, copy stale.
		EXPECT(GW::HandleCpuWriteFault(B + 2 * P + 17), "a fault on a watched alias was not handled");
		EXPECT(!ReadOnly(A + 2 * P) && !ReadOnly(B + 2 * P), "a faulted page stayed protected on an alias");
		EXPECT(ReadOnly(A) && ReadOnly(B + P), "a fault unprotected other pages");
		EXPECT(!GW::Current(A, 3 * P, g2), "a CPU write left the copy current");
		EXPECT(!GW::HandleCpuWriteFault(B + 2 * P), "a second fault on an unwatched page was claimed");
		EXPECT(!GW::HandleCpuWriteFault(C), "a fault on a never-watched page was claimed");
		GW::Watch(A, 3 * P);
		const uint64_t g3 = GW::Begin();
		EXPECT(GW::Current(B, 3 * P, g3) && ReadOnly(B + 2 * P), "re-watching did not protect the faulted page again");

		// Physical writes (canonical exports) by backing offset.
		const GW::PhysicalRange exported[] = {{0x40000 + P + 4, 16}};
		GW::NotePhysicalWrite(exported);
		EXPECT(!GW::Current(A, 3 * P, g3) && GW::Current(A, P, g3), "a physical write was not attributed to its page only");

		// A writer with an unknown target makes every older copy stale.
		const uint64_t g4 = GW::Begin();
		GW::Watch(A, 3 * P);
		const uint64_t g5 = GW::Begin();
		GW::NoteUnknownWrite();
		EXPECT(!GW::Current(A, P, g5) && !GW::Current(A, P, g4), "an unknown write left a copy current");
		EXPECT(GW::Current(A, P, GW::Begin()), "a copy made after an unknown write is stale");

		// A shader may need the address table only for loads while also writing a bounded V# buffer.
		// Exercise the renderer's actual notification policy against physical aliases.
		Libs::Graphics::ShaderRecompiler::IR::CompiledShaderInfo shader;
		Libs::Graphics::ShaderRecompiler::IR::ResourceSnapshot resources;
		shader.info.uses_dma = true;
		shader.info.buffers.resize(1);
		resources.buffers.resize(1);
		resources.buffers[0].dwords = {uint32_t(C),uint32_t(C>>32)|(16u<<16),uint32_t(P/16)};
		GW::Watch(C,2*P);
		const uint64_t read_generation = GW::Begin();
		GW::NoteShaderWrites(shader,resources);
		EXPECT(GW::Current(A,P,read_generation) && GW::Current(C,2*P,read_generation),"address-table loads invalidated cached copies");
		shader.info.buffers[0].written = true;
		GW::NoteShaderWrites(shader,resources);
		EXPECT(GW::Current(A,P,read_generation) && !GW::Current(C,P,read_generation) && GW::Current(C+P,P,read_generation),"bounded store with address-table loads did not invalidate exactly its V# span");
		shader.info.buffers[0].written = false;
		shader.info.buffers[0].atomic = true;
		const uint64_t atomic_generation = GW::Begin();
		GW::NoteShaderWrites(shader,resources);
		EXPECT(GW::Current(A,P,atomic_generation) && !GW::Current(C,P,atomic_generation),"atomic-only V# writer was not recorded");
		shader.has_address_writes = true;
		const uint64_t address_generation = GW::Begin();
		GW::NoteShaderWrites(shader,resources);
		EXPECT(!GW::Current(A,P,address_generation) && !GW::Current(C+P,P,address_generation),"unknown address store left unrelated cached bytes current");

		// An announced host write (file read) unprotects, makes stale, and keeps the pages unwatched for the cooldown.
		GW::InvalidateHostWrite(A + P, 10);
		EXPECT(!ReadOnly(A + P) && !ReadOnly(B + P), "an announced host write left the page protected");
		GW::Watch(A, 3 * P);
		const uint64_t g6 = GW::Begin();
		EXPECT(!ReadOnly(A + P) && !GW::Current(A, 3 * P, g6), "a page in its cooldown was watched again");
		EXPECT(GW::Current(A, P, g6), "the cooldown spread to other pages");
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		GW::Watch(A, 3 * P);
		const uint64_t g7 = GW::Begin();
		EXPECT(ReadOnly(A + P) && GW::Current(A, 3 * P, g7), "the page was not watched again after its cooldown");

		// A mapping change can add aliases nobody protected: every copy is stale and the old watches are dropped.
		GuestWritesShim::g_mappings.push_back({0x400000000ull, P, 0x40000});
		GuestWritesShim::g_generation++;
		EXPECT(!GW::Current(A, 3 * P, g7), "a copy stayed current across a mapping change");
		EXPECT(!ReadOnly(A) && !ReadOnly(B), "watches were kept across a mapping change");
		GW::Watch(A, P);
		EXPECT(ReadOnly(0x400000000ull) && GW::Current(A, P, GW::Begin()), "the new alias was not protected by the next watch");

		std::printf("Guest write generations: %u checks PASS\n", s_checks);
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s (after %u checks)\n", e.what(), s_checks);
		return 1;
	}
}
