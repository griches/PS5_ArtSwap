/*
 * Art Swap helper - the SteamGridDB API key, and whether PlayStation Store logos are on.
 *
 * Every person registers a key of their own (SteamGridDB's terms do not
 * cover sharing one): from their phone (phone.c), with the console's
 * keyboard, or as /data/artswap/apikey.txt copied over FTP. No key is built
 * into Art Swap.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "artswap.h"

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

bool config_key(char *out, size_t size)
{
    pthread_mutex_lock(&g_lock);
    const bool have = g_user_key[0] != 0;
    if (have)
        copy_str(out, size, g_user_key);
    pthread_mutex_unlock(&g_lock);
    return have;
}

const char *config_key_source(void)
{
    pthread_mutex_lock(&g_lock);
    const bool have = g_user_key[0] != 0;
    pthread_mutex_unlock(&g_lock);
    return have ? "user" : "none";
}

bool config_set_key(const char *key)
{
    char path[512];
    data_path("apikey.txt", path, sizeof(path));
    if (key[0] == 0) {
        /* forget the key */
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

bool config_key_hint(char *out, size_t size)
{
    char key[80];
    if (!config_key(key, sizeof(key)) || size < 5)
        return false;
    const size_t len = strlen(key);
    copy_str(out, size, key + (len > 4 ? len - 4 : 0));
    return true;
}

/* ---- PlayStation Store logos ----
 * Off unless the person turns them on: Sony's public catalog needs no account and is asked as a web
 * browser would ask it (http_get_plain), but it still sees the console's address and which game. */

bool config_store_logos(void)
{
    char path[512];
    return file_exists(data_path("store-logos", path, sizeof(path)));
}

bool config_set_store_logos(bool on)
{
    char path[512];
    data_path("store-logos", path, sizeof(path));
    if (!on)
        return remove(path) == 0 || !file_exists(path);
    return file_write_replace(path, "on\n", 3, NULL);
}

/* ---- the title's own choices ---- */

static bool pref_key_ok(const char *key)
{
    const size_t len = strlen(key);
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)key[i]))
            return false;
    return len > 0 && len < 32;
}

bool config_pref(const char *key, char *value, size_t size)
{
    char path[512];
    buf_t text = {0};
    bool found = false;
    if (pref_key_ok(key) && file_read(data_path("prefs.txt", path, sizeof(path)), &text) && text.len > 0) {
        const size_t len = strlen(key);
        char *save = NULL;
        for (char *line = strtok_r((char *)text.data, "\n", &save); line && !found;
             line = strtok_r(NULL, "\n", &save))
            if (strncmp(line, key, len) == 0 && line[len] == '\t') {
                copy_str(value, size, line + len + 1);
                found = true;
            }
    }
    buf_free(&text);
    return found;
}

bool config_set_pref(const char *key, const char *value)
{
    if (!pref_key_ok(key) || strlen(value) > 63)
        return false;
    for (const char *p = value; *p; p++)
        if ((unsigned char)*p < 0x20)
            return false;
    char path[512];
    buf_t text = {0}, out = {0};
    file_read(data_path("prefs.txt", path, sizeof(path)), &text);
    const size_t len = strlen(key);
    char *save = NULL;
    if (text.len > 0)
        for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            if (!(strncmp(line, key, len) == 0 && line[len] == '\t'))
                buf_printf(&out, "%s\n", line);
    buf_printf(&out, "%s\t%s\n", key, value);
    const bool ok = file_write_replace(path, out.data, out.len, NULL);
    buf_free(&text);
    buf_free(&out);
    return ok;
}
