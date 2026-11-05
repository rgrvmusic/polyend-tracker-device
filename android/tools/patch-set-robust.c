/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* Remove set_robust_list from a glibc 2.24 armhf libpthread.
 *
 * Android's seccomp policy traps unknown syscalls instead of returning ENOSYS,
 * and glibc issues set_robust_list (338) before any constructor runs, so a
 * SIGSYS handler is too late.
 *
 * glibc 2.24 builds for armhf use two forms: crosstool-NG's `movw r7,#338` /
 * `svc #0` and Debian's `mov.w ip,#338` / `bl __libc_do_syscall`. The
 * instruction entering the kernel is replaced by the call's post-condition
 * (`mov r0,#0` or `movs r0,#0; nop`).
 *
 *     patch-set-robust [--check] <libpthread>
 */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SITE_MOVW 0xe3007152u /* movw r7, #338 */
#define SITE_SVC 0xef000000u  /* svc #0 */
#define SITE_MOVR0 0xe3a00000u /* mov r0, #0 */
#define SITE_MOVIP 0x7ca9f44fu /* mov.w ip, #338 */
#define SITE_MOVS0 0xbf002000u /* movs r0, #0; nop */

/* The call is a few instructions after the syscall number; start_thread has
 * one add between them. 32 bytes covers that without reaching another
 * syscall. */
#define REACH 32

/* Thumb-2 bl (blx has 0xc000 in the second halfword).  The first halfword is
 * in the low 16 bits of the little-endian word. */
static int isBl(uint32_t w)
{
    return (w & 0xd000f800u) == 0xd000f000u;
}

typedef struct
{
    size_t movw;
    size_t call;
    uint32_t patched;
} Site;

static uint32_t wordAt(const unsigned char* d, size_t off)
{
    return (uint32_t)d[off] | (uint32_t)d[off + 1] << 8 | (uint32_t)d[off + 2] << 16 |
           (uint32_t)d[off + 3] << 24;
}

static void wordPut(unsigned char* d, size_t off, uint32_t v)
{
    d[off + 0] = (unsigned char)v;
    d[off + 1] = (unsigned char)(v >> 8);
    d[off + 2] = (unsigned char)(v >> 16);
    d[off + 3] = (unsigned char)(v >> 24);
}

static int elf32Arm(const unsigned char* d, size_t n, const char** why)
{
    if (n < sizeof(Elf32_Ehdr) || memcmp(d, ELFMAG, SELFMAG) != 0)
    {
        *why = "not an ELF file";
        return 0;
    }
    if (d[EI_CLASS] != ELFCLASS32)
    {
        *why = "not ELF32";
        return 0;
    }
    if (d[EI_DATA] != ELFDATA2LSB)
    {
        *why = "not little-endian";
        return 0;
    }
    if ((uint16_t)(d[18] | d[19] << 8) != EM_ARM)
    {
        *why = "not ARM";
        return 0;
    }
    return 1;
}

/* Every site; -1 if a syscall number has no call behind it, -2 if there is no
 * site at all, else the count.  Both forms are searched because a build may
 * use either, and neither form occurs in the other's encoding. */
static int findSites(const unsigned char* d, size_t n, Site* out, int max, const char** why)
{
    int count = 0;
    for (size_t off = 0; off + 4 <= n; off += 2)
    {
        const uint32_t head = wordAt(d, off);
        const int arm = head == SITE_MOVW;
        const int thumb = head == SITE_MOVIP;
        if (!arm && !thumb) continue;
        const uint32_t call0 = arm ? SITE_SVC : SITE_MOVS0;
        int found = 0;
        for (size_t o = off + 4; o + 4 <= n && o < off + 4 + REACH; o += 2)
        {
            const uint32_t w = wordAt(d, o);
            if (w == call0 || w == (thumb ? SITE_MOVS0 : SITE_MOVR0) ||
                (thumb && isBl(w)))
            {
                if (count < max)
                {
                    out[count].movw = off;
                    out[count].call = o;
                    out[count].patched = thumb ? SITE_MOVS0 : SITE_MOVR0;
                }
                count++;
                found = 1;
                break;
            }
        }
        if (!found)
        {
            *why = arm ? "a movw r7,#338 has no svc within 32 bytes"
                       : "a mov.w ip,#338 has no bl within 32 bytes";
            return -1;
        }
    }
    if (count == 0)
    {
        *why = "no set_robust_list site";
        return -2;
    }
    return count;
}

static unsigned char* readFile(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    unsigned char* d = malloc((size_t)size + 4);
    if (d == NULL)
    {
        fclose(f);
        return NULL;
    }
    if (fread(d, 1, (size_t)size, f) != (size_t)size)
    {
        free(d);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return d;
}

static int writeFile(const char* path, const unsigned char* d, size_t len)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.patch.%d", path, (int)getpid()) >= (int)sizeof tmp)
    {
        return -1;
    }
    FILE* f = fopen(tmp, "wb");
    if (f == NULL) return -1;
    int ok = fwrite(d, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok || chmod(tmp, st.st_mode) != 0 || rename(tmp, path) != 0)
    {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    int check = 0;
    const char* path;
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
    {
        check = 1;
        path = argv[2];
    }
    else if (argc == 2) path = argv[1];
    else
    {
        fprintf(stderr, "usage: %s [--check] <libpthread>\n", argv[0]);
        return 2;
    }

    size_t len;
    unsigned char* d = readFile(path, &len);
    if (d == NULL)
    {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 1;
    }

    const char* why = "";
    if (!elf32Arm(d, len, &why))
    {
        fprintf(stderr, "%s: %s -- nothing has been written.\n", path, why);
        free(d);
        return 1;
    }

    enum { kMaxSites = 16 };
    Site sites[kMaxSites];
    const int count = findSites(d, len, sites, kMaxSites, &why);
    if (count < 0)
    {
        fprintf(stderr, "%s: %s -- nothing has been written.\n", path, why);
        free(d);
        return 1;
    }

    int already = 0, patched = 0;
    for (int i = 0; i < count && i < kMaxSites; i++)
    {
        if (wordAt(d, sites[i].call) == sites[i].patched)
        {
            already++;
            continue;
        }
        if (check)
        {
            fprintf(stderr, "%s: site %#zx still calls set_robust_list\n", path,
                    sites[i].movw);
            free(d);
            return 1;
        }
        wordPut(d, sites[i].call, sites[i].patched);
        patched++;
        printf("  %#zx: call -> return 0  (number at %#zx)\n", sites[i].call, sites[i].movw);
    }

    if (check)
    {
        printf("  all %d set_robust_list sites are skipped\n", already);
        free(d);
        return 0;
    }

    if (patched == 0)
    {
        printf("  %s needs nothing\n", path);
        free(d);
        return 0;
    }

    if (writeFile(path, d, len) != 0)
    {
        fprintf(stderr, "%s: cannot write the patch: %s\n", path, strerror(errno));
        free(d);
        return 1;
    }
    free(d);

    /* The two halves must agree before the file is trusted. */
    d = readFile(path, &len);
    if (d == NULL)
    {
        fprintf(stderr, "%s: cannot re-read the patched file: %s\n", path, strerror(errno));
        return 1;
    }
    Site again[kMaxSites];
    const int count2 = findSites(d, len, again, kMaxSites, &why);
    if (count2 != count)
    {
        fprintf(stderr, "%s: the patched file does not re-read as the same shape: %s\n",
                path, why);
        free(d);
        return 1;
    }
    for (int i = 0; i < count2 && i < kMaxSites; i++)
    {
        if (wordAt(d, again[i].call) != again[i].patched)
        {
            fprintf(stderr, "%s: site %#zx did not take the patch\n", path, again[i].movw);
            free(d);
            return 1;
        }
    }
    free(d);
    printf("  wrote %s (%d patched, %d already)\n", path, patched, already);
    return 0;
}
