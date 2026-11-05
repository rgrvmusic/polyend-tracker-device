/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* glibc symbols gcc 12's libstdc++ expects but the Android runtime's glibc
 * 2.24 lacks. libstdc++ references them from code linked in but never called. */
#if defined(TRACKER_GLIBC224)

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

/* libstdc++ reads this to choose locked or unlocked locale initialisation
 * (glibc 2.32). Zero means "not known to be single-threaded", which is correct
 * here because the firmware starts threads. */
extern "C" int __libc_single_threaded = 0;

/* std::random_device's backends (glibc 2.25 and 2.36). */
extern "C" int getentropy(void* buffer, size_t length)
{
    unsigned char* p = static_cast<unsigned char*>(buffer);
    while (length > 0)
    {
        const long n = syscall(SYS_getrandom, p, length, 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        length -= static_cast<size_t>(n);
    }
    return 0;
}

extern "C" unsigned int arc4random(void)
{
    unsigned int v = 0;
    getentropy(&v, sizeof v);
    return v;
}

#endif
