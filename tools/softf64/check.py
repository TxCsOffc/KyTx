#!/usr/bin/env python3
"""Judge for tools/softf64/kyty_f64.h: compile it as C++ (uint/ulong typedefs, MSL-compatible subset) and compare every
function bit-exactly with the host's IEEE double (round to nearest even, denormals kept) on edge values and random inputs."""
import subprocess, sys, tempfile, re, argparse
ap=argparse.ArgumentParser(); ap.add_argument("--only", default="add,mul,fma,rcp,from_f32,to_f32,from_i32,from_u32,cmp"); ONLY=set(ap.parse_args().only.split(","))
from pathlib import Path
here = Path(__file__).resolve().parent
inc = here.parent.parent / "src/graphics/shader/recompiler/backend/msl/MslSoftF64.inc"
if not inc.exists():
    print("MslSoftF64.inc missing"); print("FAIL"); sys.exit(1)
raw = inc.read_text()
text = raw.split('R"KYTYF64(',1)[1].rsplit(')KYTYF64"',1)[0]
# MSL compatibility: no std::, no includes, no double/float math helpers from libc, no recursion-y C++ features.
bad = [w for w in ("#include", "std::", "double", "long double", "__int128", "memcpy", "union", "goto") if w in text]
if bad:
    print("kyty_f64.h must stay MSL-compatible; forbidden:", ", ".join(bad)); print("FAIL"); sys.exit(1)
harness = r'''
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
#define thread
typedef uint32_t uint; typedef uint64_t ulong;
static inline uint clz(uint x){return x?__builtin_clz(x):32;}
static inline ulong clz(ulong x){return x?__builtin_clzll(x):64;}
#define inline inline
static inline float as_float(uint u){float f;std::memcpy(&f,&u,4);return f;}
static inline uint as_uint(float f){uint u;std::memcpy(&u,&f,4);return u;}
template<class T, class F> static inline T as_type(F v){T t;static_assert(sizeof(T)==sizeof(F));std::memcpy(&t,&v,sizeof(T));return t;}
#include "kyty_f64.h"

static double D(ulong u){double d;std::memcpy(&d,&u,8);return d;}
static ulong U(double d){ulong u;std::memcpy(&u,&d,8);return u;}
static bool same(ulong a, ulong b){ // NaN: any NaN accepted for a NaN result
  bool an=((a>>52)&0x7ff)==0x7ff && (a&0xfffffffffffffull); bool bn=((b>>52)&0x7ff)==0x7ff && (b&0xfffffffffffffull);
  return an||bn ? (an&&bn) : a==b; }
static int fails=0; static long checked=0;
#define REPORT(name, ...) do{ if(fails<12) std::printf(name " " __VA_ARGS__); ++fails; }while(0)
int main(){
  std::mt19937_64 rng(20261112);
  std::vector<ulong> edge={0,0x8000000000000000ull,1,0x8000000000000001ull,0x000fffffffffffffull,0x0010000000000000ull,
    0x3ff0000000000000ull,0xbff0000000000000ull,0x7fefffffffffffffull,0xffefffffffffffffull,0x7ff0000000000000ull,
    0xfff0000000000000ull,0x7ff8000000000000ull,0x7ff0000000000001ull,0x3fe0000000000000ull,0x4000000000000000ull,
    0x3cb0000000000000ull,0x3ca0000000000000ull,0x4340000000000000ull,0x0000000000000002ull,0x001fffffffffffffull,
    0x3ff0000000000001ull,0x3fefffffffffffffull,0x47efffffe0000000ull,0x36a0000000000000ull,0x380fffffffffffffull};
  auto rnd=[&]()->ulong{ switch(rng()%5){ case 0: return edge[rng()%edge.size()];
      case 1: return rng() & 0x800fffffffffffffull; // denormals
      case 2: return (rng() & 0x800fffffffffffffull) | (ulong(1000+rng()%48)<<52); // near 1.0
      default: return rng(); } };
  for(long i=0;i<3000000;++i){
    ulong a=rnd(), b=rnd(), c=rnd();
    ulong r;
#if KYTY_TEST_ADD
    r=kyty_f64_add(a,b); if(!same(r,U(D(a)+D(b)))) REPORT("add","%016llx %016llx -> %016llx want %016llx\n",(unsigned long long)a,(unsigned long long)b,(unsigned long long)r,(unsigned long long)U(D(a)+D(b)));
#endif
#if KYTY_TEST_MUL
    r=kyty_f64_mul(a,b); if(!same(r,U(D(a)*D(b)))) REPORT("mul","%016llx %016llx -> %016llx want %016llx\n",(unsigned long long)a,(unsigned long long)b,(unsigned long long)r,(unsigned long long)U(D(a)*D(b)));
#endif
#if KYTY_TEST_FMA
    r=kyty_f64_fma(a,b,c); if(!same(r,U(std::fma(D(a),D(b),D(c))))) REPORT("fma","%016llx %016llx %016llx -> %016llx want %016llx\n",(unsigned long long)a,(unsigned long long)b,(unsigned long long)c,(unsigned long long)r,(unsigned long long)U(std::fma(D(a),D(b),D(c))));
#endif
#if KYTY_TEST_RCP
    r=kyty_f64_rcp(a); if(!same(r,U(1.0/D(a)))) REPORT("rcp","%016llx -> %016llx want %016llx\n",(unsigned long long)a,(unsigned long long)r,(unsigned long long)U(1.0/D(a)));
#endif
    uint f=uint(rng()); if(i%7==0) f=uint(edge[rng()%edge.size()]>>32);
#if KYTY_TEST_FROM_F32
    r=kyty_f64_from_f32(f); if(!same(r,U(double(as_float(f))))) REPORT("from_f32","%08x -> %016llx\n",f,(unsigned long long)r);
#endif
#if KYTY_TEST_TO_F32
    uint n=kyty_f64_to_f32(a); float want=float(D(a)); uint wn=as_uint(want);
    bool nan_ok=std::isnan(want) && std::isnan(as_float(n)); if(!nan_ok && n!=wn) REPORT("to_f32","%016llx -> %08x want %08x\n",(unsigned long long)a,n,wn);
#endif
#if KYTY_TEST_FROM_I32
    int si=int(uint(rng())); r=kyty_f64_from_i32(uint(si)); if(r!=U(double(si))) REPORT("from_i32","%d\n",si);
#endif
#if KYTY_TEST_FROM_U32
    uint ui=uint(rng()); r=kyty_f64_from_u32(ui); if(r!=U(double(ui))) REPORT("from_u32","%u\n",ui);
#endif
    double x=D(a), y=D(b);
#if KYTY_TEST_CMP
    if(kyty_f64_eq(a,b)!=(x==y)) REPORT("eq","%016llx %016llx\n",(unsigned long long)a,(unsigned long long)b);
#endif
#if KYTY_TEST_CMP
    if(kyty_f64_lt(a,b)!=(x<y)) REPORT("lt","%016llx %016llx\n",(unsigned long long)a,(unsigned long long)b);
#endif
#if KYTY_TEST_CMP
    if(kyty_f64_le(a,b)!=(x<=y)) REPORT("le","%016llx %016llx\n",(unsigned long long)a,(unsigned long long)b);
#endif
#if KYTY_TEST_CMP
    if(kyty_f64_gt(a,b)!=(x>y)) REPORT("gt","%016llx %016llx\n",(unsigned long long)a,(unsigned long long)b);
#endif
#if KYTY_TEST_CMP
    if(kyty_f64_ge(a,b)!=(x>=y)) REPORT("ge","%016llx %016llx\n",(unsigned long long)a,(unsigned long long)b);
#endif
    checked++;
  }
  std::printf("checked %ld inputs, %d mismatches\n",checked,fails);
  return fails?1:0;
}
'''
with tempfile.TemporaryDirectory() as t:
    defs="".join(f"#define KYTY_TEST_{n.upper()} 1\n" for n in ONLY)
    (Path(t)/"kyty_f64.h").write_text(text)
    (Path(t)/"h.cpp").write_text(defs+harness)
    exe = Path(t)/"h"
    c = subprocess.run(["xcrun","clang++","-std=c++20","-O2","-ffp-contract=off","-I",str(t),str(Path(t)/"h.cpp"),"-o",str(exe)],capture_output=True,text=True)
    if c.returncode:
        print("COMPILE ERROR\n"+"\n".join(c.stderr.splitlines()[:30])); print("FAIL"); sys.exit(1)
    r = subprocess.run([str(exe)],capture_output=True,text=True,timeout=600)
    print(r.stdout[-3000:])
    print("PASS" if r.returncode==0 else "FAIL"); sys.exit(r.returncode)
