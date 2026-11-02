#ifndef EMULATOR_SRC_COMMON_PLATFORM_SYSAPPLEAUTORELEASE_H_
#define EMULATOR_SRC_COMMON_PLATFORM_SYSAPPLEAUTORELEASE_H_

#ifdef __APPLE__
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void* pool);

struct ScopedAutoreleasePool {
	void* pool = nullptr;
	ScopedAutoreleasePool(): pool(objc_autoreleasePoolPush()) {}
	~ScopedAutoreleasePool() {
		if (pool != nullptr) {
			objc_autoreleasePoolPop(pool);
		}
	}
	ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
	ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;
};
#else
struct ScopedAutoreleasePool {};
#endif

#endif // EMULATOR_SRC_COMMON_PLATFORM_SYSAPPLEAUTORELEASE_H_
