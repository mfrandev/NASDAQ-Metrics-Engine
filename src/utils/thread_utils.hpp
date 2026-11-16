#ifndef THREAD_UTILS_H_
#define THREAD_UTILS_H_

#ifdef __linux__
#include <pthread.h>
#include <assert.h>
#endif

bool pin_to_core(int core) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    int success = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    assert(success == 0);
    return success == 0;
#else
    // Thread pinning is only supported on Linux
    (void)core;
    return false;
#endif
}

#endif // THREAD_UTILS_H_