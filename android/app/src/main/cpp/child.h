/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

// Starting the firmware as a child process: the glibc 2.24 loader is exec'd
// with --library-path, because the firmware's PT_INTERP does not exist on
// Android.
#pragma once

#include <sys/types.h>

#include <string>

struct Child
{
    pid_t pid = -1;
    int audioFd = -1; // app reads S16 stereo 44.1 kHz here
    int midiFd = -1;  // app reads the firmware's MIDI here, writes input here
    int status = 0;   // waitpid status of the last child to go away
    int running() const { return pid > 0; }
};

// Everything the child needs is prepared before fork(): neither setenv nor
// malloc is safe in a forked child of a process that has SDL's threads running.
bool childStart(const std::string& binary, const std::string& sd, const std::string& log,
                Child& out);

// SIGTERM, then SIGKILL if it does not go; reaps it either way.
void childStop(Child& c);

// Non-blocking: true when the child has exited (and has been reaped).
bool childExited(Child& c);
