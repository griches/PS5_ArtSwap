/*
 * Art Swap helper - the SteamGridDB API key.
 *
 * A key the person gave (Settings in the title, or /data/artswap/apikey.txt
 * copied over FTP) wins; otherwise the key built into the release is used,
 * when the build had one (helper/tools/build.sh reads SGDB_KEY from the
 * project's ignored .env and writes it, masked, into builtin_key.h).
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "artswap.h"
#include "builtin_key.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_user_key[80];

static bool plausible(const char *key)
{
    const size_t len = strlen(key);
    if (len < 16 || len > 64)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)key[i]))
            return false;
    return true;
}

void config_load(void)
{
    char path[512];
    buf_t text = {0};
    pthread_mutex_lock(&g_lock);
    g_user_key[0] = 0;
    if (file_read(data_path("apikey.txt", path, sizeof(path)), &text) && text.len > 0) {
        char key[80];
        copy_str(key, sizeof(key), (const char *)text.data);
        char *newline = strpbrk(key, "\r\n");
        if (newline)
            *newline = 0;
        trim(key);
        if (plausible(key))
            copy_str(g_user_key, sizeof(g_user_key), key);
        else if (key[0])
            log_line("apikey.txt does not hold a key (%zu characters)", strlen(key));
    }
    pthread_mutex_unlock(&g_lock);
    buf_free(&text);
}

static bool builtin_key(char *out, size_t size)
{
    static const uint8_t masked[] = ARTSWAP_BUILTIN_KEY;
    const size_t len = sizeof(masked) - 1; /* the array ends in a 0 byte */
    if (len == 0 || len + 1 > size)
        return false;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)(masked[i] ^ (uint8_t)(0x5a + i * 7));
    out[len] = 0;
    return true;
}

bool config_key(char *out, size_t size)
{
    pthread_mutex_lock(&g_lock);
    const bool user = g_user_key[0] != 0;
    if (user)
        copy_str(out, size, g_user_key);
    pthread_mutex_unlock(&g_lock);
    return user || builtin_key(out, size);
}

const char *config_key_source(void)
{
    char key[80];
    pthread_mutex_lock(&g_lock);
    const bool user = g_user_key[0] != 0;
    pthread_mutex_unlock(&g_lock);
    if (user)
        return "user";
    return builtin_key(key, sizeof(key)) ? "builtin" : "none";
}

bool config_set_key(const char *key)
{
    char path[512];
    data_path("apikey.txt", path, sizeof(path));
    if (key[0] == 0) {
        /* back to the built-in key */
        remove(path);
        config_load();
        return true;
    }
    if (!plausible(key))
        return false;
    char line[96];
    snprintf(line, sizeof(line), "%s\n", key);
    if (!file_write_replace(path, line, strlen(line), NULL))
        return false;
    config_load();
    return true;
}
