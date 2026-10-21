/*
 * Art Swap helper - the console's processes, or one of them ended (a tool payload).
 *
 * As written: lists every process to /data/artswap/procs.txt. Built with
 * -DKILL_STOPPED_UNDER=<the ELF loader's pid> it also ends, with SIGKILL, every
 * payload.elf under that loader that is stopped: a payload that crashes on
 * the console is stopped, not ended, and keeps its sockets. SIGKILL alone
 * leaves such a process held, so it is attached as a debugger (the authid
 * PS5_DevTools measured on 13.60) and ended with PT_KILL.
 * After PS5_PayloadSDK's samples/ps (John Törnblom, GPL-3.0-or-later).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0))
        return 1;
    size += 16384;
    char *buf = malloc(size);
    if (!buf || sysctl(mib, 4, buf, &size, NULL, 0))
        return 1;
    FILE *out = fopen("/data/artswap/procs.txt", "w");
    if (!out)
        return 1;
    for (char *p = buf; p < buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        if (ki->ki_structsize <= 0)
            break;
        p += ki->ki_structsize;
        fprintf(out, "%6d ppid %6d stat %d start %ld comm %s\n", ki->ki_pid, ki->ki_ppid, ki->ki_stat,
                (long)ki->ki_start.tv_sec, ki->ki_comm);
#ifdef KILL_STOPPED_UNDER
        if (ki->ki_ppid == KILL_STOPPED_UNDER && ki->ki_stat == 4 /* SSTOP */ &&
            strcmp(ki->ki_comm, "payload.elf") == 0) {
            const pid_t pid = ki->ki_pid;
            static int elevated;
            if (!elevated) {
                static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
                fprintf(out, "authid %d caps %d\n", kernel_set_ucred_authid(getpid(), 0x4800000000010003ull),
                        kernel_set_ucred_caps(getpid(), caps));
                elevated = 1;
            }
            const int attach = ptrace(PT_ATTACH, pid, 0, 0);
            int status = 0;
            const int waited = attach == 0 ? waitpid(pid, &status, 0) : -1;
            const int killed = attach == 0 ? ptrace(PT_KILL, pid, 0, 0) : -1;
            const int reaped = attach == 0 ? waitpid(pid, &status, 0) : -1;
            fprintf(out, "KILL %d (stopped): attach %d wait %d status 0x%x kill %d reap %d\n", pid, attach, waited,
                    status, killed, reaped);
        }
#endif
    }
    fclose(out);
    free(buf);
    return 0;
}
