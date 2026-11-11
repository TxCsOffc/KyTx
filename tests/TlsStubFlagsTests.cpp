// Execute the actual x86 TLS stubs; tools/test_tls_stub_flags.sh also runs on
// Apple Silicon using the x86_64 translation runtime installed on the host.
#include "loader/jit.h"
#include <xbyak/xbyak.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/mman.h>

static KYTY_MS_ABI uint8_t* Address() { return reinterpret_cast<uint8_t*>(0x123456789abcull); }
static void Check(bool ok,const char* message) { if(!ok) { std::fprintf(stderr,"TLS flags: %s\n",message); std::exit(1); } }

int main() {
  constexpr size_t size=0x4000;
  auto* memory=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
  Check(memory!=MAP_FAILED,"allocate code");
  auto* safe=new(memory) Loader::Jit::SafeCall;
  safe->SetFunc(Address);
  constexpr std::array<unsigned,6> registers{1,2,3,5,6,7};
  struct Case { size_t offset; uint8_t input; uint64_t flags; };
  std::array<Case,12> cases{};
  size_t n=0;
  for(const auto reg:registers) {
    const auto offset=Loader::Jit::TlsRegStub::GetOffset(reg);
    auto* stub=new(memory+offset) Loader::Jit::TlsRegStub;
    stub->SetFunc(memory); stub->SetOutputReg(reg);
    if(std::getenv("KYTX_TLS_OLD_FLAG_CLOBBER")!=nullptr) {
      const uint8_t sub[]{0x48,0x81,0xec,0x80,0,0,0};
      const uint8_t add[]{0x48,0x81,0xc4,0x80,0,0,0,0xc3};
      std::memcpy(stub->code,sub,sizeof(sub)); std::memcpy(stub->code+17,add,sizeof(add));
      std::memcpy(safe->code,sub,sizeof(sub)); std::memcpy(safe->code+0x43,add,sizeof(add));
    }
    for(const auto input:{uint8_t(0x7f),uint8_t(0xff)}) {
      const size_t fixture=0x1000+n*128;
      Xbyak::CodeGenerator code(128,memory+fixture);
      code.push(code.rbx); code.push(code.rbp); code.push(code.r12); code.mov(code.r12,code.rdi);
      code.mov(code.rax,0x5555555555555555ull); code.mov(code.al,input); code.add(code.al,1);
      code.mov(code.r11,reinterpret_cast<uint64_t>(stub)); code.call(code.r11);
      code.mov(code.qword[code.r12],Xbyak::Reg64(reg)); code.mov(code.qword[code.r12+8],code.rax);
      code.pushf(); code.pop(code.rax); code.pop(code.r12); code.pop(code.rbp); code.pop(code.rbx); code.ret();
      cases[n++]={fixture,input,input==0x7f?0x890u:0x055u};
    }
  }
  Check(mprotect(memory,size,PROT_READ|PROT_EXEC)==0,"make code executable");
  using Fixture=uint64_t(KYTY_SYSV_ABI*)(uint64_t*);
  for(const auto& item:cases) {
    std::array<uint64_t,2> values{};
    const auto flags=reinterpret_cast<Fixture>(memory+item.offset)(values.data());
    Check((flags&0x8d5u)==item.flags,"TLS load changed caller arithmetic flags");
    Check(values[0]==0x123456789abcull,"TLS load returned wrong register value");
    Check(values[1]==((0x5555555555555555ull&~0xffull)|uint8_t(item.input+1)),"TLS load clobbered RAX");
  }
  munmap(memory,size);
  std::printf("TLS stubs: %zu executable cases preserved flags, result and RAX\n",cases.size());
}
