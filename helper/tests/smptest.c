/*
 * Art Swap - ShadowMountPlus refresh test payload. Send to the ELF loader:
 * its output (what ShadowMountPlus says of one title, and with
 * -DSMPTEST_REFRESH the refresh's steps) comes back over the same connection.
 *
 *   make -C helper smptest SMPTEST_ID=PPSA99305 [SMPTEST_REFRESH=1]
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/smp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SMPTEST_ROOT
#include <ps5/kernel.h>
#endif

/* Every file under dir with its size and a hash of its bytes, and each folder that cannot be read */
static void snapshot(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        printf("UNREADABLE %s (%s)\n", dir, strerror(errno));
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (lstat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 16)
                snapshot(path, depth + 1);
            continue;
        }
        uint64_t hash = 0xcbf29ce484222325ull;
        const int fd = open(path, O_RDONLY);
        static uint8_t piece[1 << 16];
        ssize_t got;
        while (fd >= 0 && (got = read(fd, piece, sizeof(piece))) > 0)
            for (ssize_t i = 0; i < got; i++)
                hash = (hash ^ piece[i]) * 0x100000001b3ull;
        if (fd >= 0)
            close(fd);
        printf("file %s %lld %016llx%s\n", path, (long long)st.st_size, (unsigned long long)hash,
               fd < 0 ? " UNREADABLE" : "");
    }
    closedir(d);
}

#ifndef SMPTEST_ID
#define SMPTEST_ID "PPSA99305"
#endif

pthread_mutex_t g_apply_lock = PTHREAD_MUTEX_INITIALIZER;

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    log_open_named("smptest.log");
    app_t app;
    memset(&app, 0, sizeof(app));
    copy_str(app.id, sizeof(app.id), SMPTEST_ID);
    copy_str(app.name, sizeof(app.name), SMPTEST_ID);
    snprintf(app.files[0].path, sizeof(app.files[0].path), "/data/homebrew/%s/sce_sys/icon0.png", SMPTEST_ID);
    copy_str(app.files[0].slot, sizeof(app.files[0].slot), "icon0");
    app.nfiles = 1;
#ifdef SMPTEST_SNAPSHOT
    printf("uid %d euid %d\n", (int)getuid(), (int)geteuid());
#ifdef SMPTEST_ROOT
    {
        const pid_t pid = getpid();
        uint8_t caps[16];
        memset(caps, 0xff, sizeof(caps));
        printf("set uid %d ruid %d svuid %d rgid %d svgid %d caps %d authid 0x%llx\n", kernel_set_ucred_uid(pid, 0),
               kernel_set_ucred_ruid(pid, 0), kernel_set_ucred_svuid(pid, 0), kernel_set_ucred_rgid(pid, 0),
               kernel_set_ucred_svgid(pid, 0), kernel_set_ucred_caps(pid, caps),
               (unsigned long long)kernel_get_ucred_authid(pid));
        printf("now uid %d euid %d\n", (int)getuid(), (int)geteuid());
        struct stat st;
        if (stat(SMPTEST_SNAPSHOT, &st) == 0) {
            printf("owner uid %d gid %d mode %o\n", (int)st.st_uid, (int)st.st_gid, (unsigned)st.st_mode);
            /* the owner's own uid, the way the game wrote it */
            printf("as owner: uid %d ruid %d gid %d\n", kernel_set_ucred_uid(pid, st.st_uid),
                   kernel_set_ucred_ruid(pid, st.st_uid), kernel_set_ucred_rgid(pid, st.st_gid));
            setegid(st.st_gid);
            printf("seteuid %d -> uid %d euid %d\n", seteuid(st.st_uid), (int)getuid(), (int)geteuid());
        }
    }
#endif
    snapshot(SMPTEST_SNAPSHOT, 0);
    fflush(stdout);
#endif
    smp_info_t info;
    smp_info(&app, &info);
    printf("answers %d version %s found %d installed %d source %s path %s can_refresh %d\nwhy: %s\n", info.answers,
           info.version, info.found, info.installed, info.source_type, info.path, info.can_refresh, info.why);
#ifdef SMPTEST_REFRESH
    char err[300] = "";
    if (!smp_refresh_start(&app, 0, err, sizeof(err))) {
        printf("refresh did not start: %s\n", err);
        return 1;
    }
    char last[400] = "";
    for (int i = 0; i < 400 && smp_refresh_running(); i++) {
        buf_t line = {0};
        smp_refresh_line(&line);
        if (line.data && strcmp((char *)line.data, last) != 0) {
            copy_str(last, sizeof(last), (char *)line.data);
            printf("%s", last);
            fflush(stdout);
        }
        buf_free(&line);
        usleep(500000);
    }
    buf_t line = {0};
    smp_refresh_line(&line);
    printf("final %s", line.data ? (char *)line.data : "?\n");
    buf_free(&line);
#endif
    return 0;
}
