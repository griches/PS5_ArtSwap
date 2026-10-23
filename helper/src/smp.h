/*
 * Art Swap helper - the home-screen refresh, through ShadowMountPlus's API.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "scan.h"

/* What ShadowMountPlus knows of a title, and whether Art Swap may refresh its tile */
typedef struct {
    bool answers;        /* its API answered */
    char version[40];
    bool found;          /* it lists the title */
    bool installed;      /* the title is on the home screen */
    char source_type[12];
    char path[400];      /* the title's source folder */
    bool can_refresh;
    char why[480];       /* when it cannot: a sentence for a person */
} smp_info_t;

void smp_info(const app_t *app, smp_info_t *out);

/* The titles ShadowMountPlus installed from a folder it can reach, the ones a refresh can show: up to
 * max ids, how many. None when its API does not answer within a few seconds. */
int smp_folder_titles(char (*ids)[16], int max);

/* Starts the refresh of one title in the background: ShadowMountPlus uninstalls
 * the title (its folder, saved data and download data stay) and a scan installs
 * it again, with the art now in its folder. While the console is busy with a game (Art Swap
 * itself, until it closes) ShadowMountPlus refuses; the refresh keeps asking
 * for wait_s seconds. False, with a sentence in err, when it cannot start. */
bool smp_refresh_start(const app_t *app, int wait_s, char *err, size_t errlen);

/* The same refresh, run in the calling thread to its end (Apply changes, after Art Swap has closed): true
 * when the title is back with its new name and art; message says how it went either way */
bool smp_refresh_run(const app_t *app, int wait_s, char *message, size_t size);

/* "refresh <phase> <id> <unseen> <message>": phase none, copying, waiting, removing, adding, checking,
 * done or failed. A finished refresh is kept in /data/artswap/refresh.txt, unseen until smp_refresh_seen():
 * the title has closed while it ran, and shows how it went when it next starts. */
void smp_refresh_line(buf_t *out);
void smp_refresh_seen(void);
bool smp_refresh_running(void);

/* Puts back download data an earlier refresh moved aside and never moved back (the helper ended in between).
 * Run when the helper starts. */
void smp_recover(void);
