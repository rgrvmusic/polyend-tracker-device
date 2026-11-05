/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

#include "child.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace
{
void setCloexec(int fd, bool on)
{
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0) return;
    fcntl(fd, F_SETFD, on ? (fl | FD_CLOEXEC) : (fl & ~FD_CLOEXEC));
}

void setBlocking(int fd, bool blocking)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(fd, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
}

// The child inherits environ minus stale TRACKER_* keys, plus these values.
std::vector<std::string> childEnv(const std::string& sd, int audioFd, int midiFd)
{
    static const char* owned[] = {"TRACKER_SD", "TRACKER_AUDIO", "TRACKER_MIDI", "TRACKER_BIND",
                                  "TRACKER_SECCOMP_SKIP", "TRACKER_RESTART", "TMPDIR"};
    std::vector<std::string> env;
    for (char** e = environ; e != nullptr && *e != nullptr; e++)
    {
        const char* eq = strchr(*e, '=');
        if (eq == nullptr) continue;
        const size_t n = (size_t)(eq - *e);
        bool skip = false;
        for (const char* k : owned)
            if (strlen(k) == n && !strncmp(*e, k, n))
            {
                skip = true;
                break;
            }
        if (!skip) env.emplace_back(*e);
    }
    env.push_back("TRACKER_SD=" + sd);
    env.push_back("TRACKER_AUDIO=fd:" + std::to_string(audioFd));
    env.push_back("TRACKER_MIDI=fd:" + std::to_string(midiFd));
    env.push_back("TRACKER_BIND=127.0.0.1");
    // The patched libpthread issues nothing the filter traps; the handler is a
    // safety net. TRACKER_RESTART=0: a re-exec of argv[0] would lose
    // --library-path, so the app restarts the child instead.
    env.push_back("TRACKER_SECCOMP_SKIP=1");
    env.push_back("TRACKER_RESTART=0");
    env.push_back("TMPDIR=" + sd);
    return env;
}
// After a fork only async-signal-safe calls are allowed, so a failed execve
// would be silent. Write the errno to stderr by hand.
template <size_t N>
void mark(const char (&s)[N]) noexcept
{
    const ssize_t w = write(STDERR_FILENO, s, N - 1);
    (void)w;
}

void reportExecFailure(int err) noexcept
{
    static const char kPrefix[] = "child: execve failed, errno=";
    char msg[sizeof kPrefix + 12];
    size_t i = 0;
    while (kPrefix[i] != '\0')
    {
        msg[i] = kPrefix[i];
        i++;
    }
    char digits[10];
    int n = 0;
    do
    {
        digits[n++] = (char)('0' + err % 10);
        err /= 10;
    } while (err > 0 && n < (int)sizeof digits);
    while (n > 0) msg[i++] = digits[--n];
    msg[i++] = '\n';
    const ssize_t w = write(STDERR_FILENO, msg, i);
    (void)w;
}
} // namespace

bool childStart(const std::string& binary, const std::string& sd, const std::string& log,
                Child& out)
{
    out = Child();

    int audio[2], midi[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, audio) != 0) return false;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, midi) != 0)
    {
        close(audio[0]);
        close(audio[1]);
        return false;
    }
    // The firmware's blocking audio writes pace its audio thread, so that end
    // stays blocking and the buffer is kept small to hold latency down. MIDI
    // must not block either side: the firmware polls it.
    int sndbuf = 8192;
    setsockopt(audio[1], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    setBlocking(audio[0], false);
    setBlocking(midi[0], false);
    setBlocking(midi[1], false);
    setCloexec(audio[0], true);
    setCloexec(midi[0], true);
    setCloexec(audio[1], false);
    setCloexec(midi[1], false);

    int logFd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logFd < 0) logFd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    // Built before fork: the child may only run async-signal-safe code. The
    // firmware's PT_INTERP path does not exist on Android, so it is started
    // through the loader beside it, which consumes --library-path.
    const size_t slash = binary.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : binary.substr(0, slash);
    const std::string loader = dir + "/ld-linux-armhf.so.3";
    const std::string bin = binary;
    char libraryPath[] = "--library-path";
    std::vector<std::string> env = childEnv(sd, audio[1], midi[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(loader.c_str()));
    argv.push_back(libraryPath);
    argv.push_back(const_cast<char*>(dir.c_str()));
    argv.push_back(const_cast<char*>(bin.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (std::string& s : env) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(audio[0]);
        close(audio[1]);
        close(midi[0]);
        close(midi[1]);
        if (logFd >= 0) close(logFd);
        return false;
    }
    if (pid == 0)
    {
        if (logFd >= 0)
        {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) close(logFd);
        }
        mark("child: pre-exec\n");
        // Child: its own session.
        if (setsid() < 0) mark("child: setsid failed\n");
        mark("child: exec\n");
        execve(loader.c_str(), argv.data(), envp.data());
        reportExecFailure(errno);
        _exit(127);
    }

    close(audio[1]);
    close(midi[1]);
    if (logFd >= 0) close(logFd);

    out.pid = pid;
    out.audioFd = audio[0];
    out.midiFd = midi[0];
    return true;
}

bool childExited(Child& c)
{
    if (c.pid <= 0) return true;
    int status = 0;
    const pid_t r = waitpid(c.pid, &status, WNOHANG);
    if (r != c.pid) return false;
    c.pid = -1;
    c.status = status;
    return true;
}

void childStop(Child& c)
{
    if (c.pid > 0)
    {
        kill(c.pid, SIGTERM);
        for (int i = 0; i < 100; i++)
        {
            if (childExited(c)) break;
            usleep(20 * 1000);
        }
        if (c.pid > 0)
        {
            kill(c.pid, SIGKILL);
            waitpid(c.pid, nullptr, 0);
            c.pid = -1;
        }
    }
    if (c.audioFd >= 0) close(c.audioFd);
    if (c.midiFd >= 0) close(c.midiFd);
    c.audioFd = -1;
    c.midiFd = -1;
}
