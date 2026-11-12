#include "graphics/host_gpu/metal/metalCompilePolicy.h"
#include <cstdio>

using namespace Libs::Graphics::Metal;
int main() {
	struct Case {uint32_t sdk,os;MTLLanguageVersion expected;};
	const Case cases[]{
	    {130000,130000,MTLLanguageVersion3_0},{130000,270000,MTLLanguageVersion3_0},
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
	    {140000,130000,MTLLanguageVersion3_0},{140000,140000,MTLLanguageVersion3_1},
	    {140000,270000,MTLLanguageVersion3_1},{270000,139999,MTLLanguageVersion3_0},
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
	    {150000,140000,MTLLanguageVersion3_1},{150000,150000,MTLLanguageVersion3_2},
	    {150000,270000,MTLLanguageVersion3_2},{259999,270000,MTLLanguageVersion3_2},
	    {270000,149999,MTLLanguageVersion3_1},{270000,259999,MTLLanguageVersion3_2},
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
	    {260000,260000,MTLLanguageVersion4_0},{260000,270000,MTLLanguageVersion4_0},
	    {270000,260000,MTLLanguageVersion4_0},{270000,270000,MTLLanguageVersion4_0},
#endif
	};
	for(const auto& c:cases)if(MslLanguageForTarget(c.sdk,c.os)!=c.expected) {
		std::fprintf(stderr,"wrong language for SDK=%u OS=%u\n",c.sdk,c.os);return 1;
	}
	MTLLanguageVersion expected=MTLLanguageVersion3_0;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
	if(@available(macOS 14.0,*))expected=MTLLanguageVersion3_1;
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
	if(@available(macOS 15.0,*))expected=MTLLanguageVersion3_2;
#endif
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
	if(@available(macOS 26.0,*))expected=MTLLanguageVersion4_0;
#endif
	if(NativeMslLanguageVersion()!=expected){std::fprintf(stderr,"native language selection failed\n");return 1;}
	std::printf("MSL compile policy: %zu SDK/OS boundaries + runtime selection PASS (language=%lu; CPU only, no Metal device)\n",
	    sizeof(cases)/sizeof(cases[0]),(unsigned long)expected);
}
