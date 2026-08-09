#ifndef FLOWENGINE_ESMINI_MINGW_COMPAT_H
#define FLOWENGINE_ESMINI_MINGW_COMPAT_H

/* esmini 77028d8 selects its legacy Win7 timer branch for every MinGW build,
 * but SE_getSimTimeStep() unconditionally calls the microsecond API from the
 * modern branch.  Inject the missing API without modifying the submodule. */
#if defined(__MINGW32__)
#include <chrono>
#include <thread>

inline long long SE_getSystemTimeMicroseconds() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline long long SE_getSystemTimeMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void SE_sleepMicroseconds(unsigned int usec) {
    std::this_thread::sleep_for(std::chrono::microseconds(usec));
}

inline void SE_sleepMilliseconds(unsigned int msec) {
    std::this_thread::sleep_for(std::chrono::milliseconds(msec));
}
#endif

#endif /* FLOWENGINE_ESMINI_MINGW_COMPAT_H */