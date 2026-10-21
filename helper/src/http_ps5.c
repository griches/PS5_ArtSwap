/*
 * Art Swap helper - HTTPS on the console, through its own libSceHttp.
 *
 * What the console showed (helper/tests/tlstest.c, firmware 13.60, 2026-10-21):
 * from a payload, plain HTTP works, but every HTTPS request fails with
 * 0x8095F00C (sceHttpsGetSslError: 0x8095F00A, detail 0x20, "not a known
 * CA"), because a payload does not get the console's list of trusted roots.
 * A certificate callback (sceHttpsSetSslCallback) sees that one failure and
 * may accept it; SteamGridDB's API and its CDN then answer. So the callback
 * here accepts the "unknown CA" result and nothing else: the library still
 * checks the name and the dates, and any other failure is refused.
 * sceHttpsDisableOption crashed the process outright, and HTTP/2 failed the
 * same way, so neither is used.
 *
 * The libraries are opened with dlopen at run time, not linked, so that one
 * the loader cannot give this process costs the network, not the whole
 * helper. Only exported functions of the system software are called.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "artswap.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define USER_AGENT "ArtSwap/" ARTSWAP_VERSION " (PS5 homebrew)"
/* the callback's verify error that means "the root is not in the list this process has" */
#define VERIFY_UNKNOWN_CA 0x20u

static struct {
    int (*NetInit)(void);
    int (*NetPoolCreate)(const char *, int, int);
    int (*SslInit)(size_t);
    int (*Init)(int, int, size_t);
    int (*CreateTemplate)(int, const char *, int, int);
    int (*CreateConnectionWithURL)(int, const char *, int);
    int (*CreateRequestWithURL)(int, int, const char *, uint64_t);
    int (*AddRequestHeader)(int, const char *, const char *, uint32_t);
    int (*SendRequest)(int, const void *, size_t);
    int (*GetStatusCode)(int, int *);
    int (*ReadData)(int, void *, size_t);
    int (*DeleteRequest)(int);
    int (*DeleteConnection)(int);
    int (*SetConnectTimeOut)(int, uint32_t);
    int (*SetRecvTimeOut)(int, uint32_t);
    int (*SetSslCallback)(int, int (*)(int, unsigned int, void *const *, int, void *), void *);
    int (*GetSslError)(int, int *, uint32_t *);
} f;

static int g_template = -1;
static const char *g_backend = "none";
static pthread_mutex_t g_once = PTHREAD_MUTEX_INITIALIZER;
static bool g_told; /* the log says once that the callback is accepting */

static void *open_library(const char *name)
{
    void *handle = dlopen(name, RTLD_LAZY);
    if (handle == NULL) {
        char path[128];
        snprintf(path, sizeof(path), "/system/common/lib/%s", name);
        handle = dlopen(path, RTLD_LAZY);
    }
    log_line("dlopen %s: %s", name, handle ? "ok" : "failed");
    return handle;
}

#define FIND(lib, field, symbol)                                                                  \
    do {                                                                                          \
        *(void **)&f.field = (lib) ? dlsym((lib), symbol) : NULL;                                 \
        if (f.field == NULL)                                                                      \
            log_line("missing %s", symbol);                                                       \
    } while (0)

/* The certificate's verdict: accept only "the root is not one this process knows" */
static int check_certificate(int ssl_ctx, unsigned int verify, void *const *certs, int count, void *user)
{
    (void)ssl_ctx;
    (void)certs;
    (void)user;
    if (verify != 0 && (verify & ~VERIFY_UNKNOWN_CA) != 0) {
        log_line("TLS refused: verify error 0x%08x, %d certificates", verify, count);
        return -1;
    }
    pthread_mutex_lock(&g_once);
    const bool tell = !g_told;
    g_told = true;
    pthread_mutex_unlock(&g_once);
    if (tell)
        log_line("TLS: the root is not in this process's list (0x%08x); name and dates checked, accepted", verify);
    return 0;
}

bool http_init(char *err, size_t errlen)
{
    void *net = open_library("libSceNet.sprx");
    void *ssl = open_library("libSceSsl.sprx");
    void *http = open_library("libSceHttp.sprx");
    FIND(net, NetInit, "sceNetInit");
    FIND(net, NetPoolCreate, "sceNetPoolCreate");
    FIND(ssl, SslInit, "sceSslInit");
    FIND(http, Init, "sceHttpInit");
    FIND(http, CreateTemplate, "sceHttpCreateTemplate");
    FIND(http, CreateConnectionWithURL, "sceHttpCreateConnectionWithURL");
    FIND(http, CreateRequestWithURL, "sceHttpCreateRequestWithURL");
    FIND(http, AddRequestHeader, "sceHttpAddRequestHeader");
    FIND(http, SendRequest, "sceHttpSendRequest");
    FIND(http, GetStatusCode, "sceHttpGetStatusCode");
    FIND(http, ReadData, "sceHttpReadData");
    FIND(http, DeleteRequest, "sceHttpDeleteRequest");
    FIND(http, DeleteConnection, "sceHttpDeleteConnection");
    FIND(http, SetConnectTimeOut, "sceHttpSetConnectTimeOut");
    FIND(http, SetRecvTimeOut, "sceHttpSetRecvTimeOut");
    FIND(http, SetSslCallback, "sceHttpsSetSslCallback");
    FIND(http, GetSslError, "sceHttpsGetSslError");
    if (!f.NetInit || !f.NetPoolCreate || !f.SslInit || !f.Init || !f.CreateTemplate || !f.CreateConnectionWithURL ||
        !f.CreateRequestWithURL || !f.SendRequest || !f.GetStatusCode || !f.ReadData || !f.DeleteRequest ||
        !f.DeleteConnection || !f.SetSslCallback) {
        snprintf(err, errlen, "the console's network libraries did not load");
        return false;
    }
    const int net_init = f.NetInit();
    const int pool = f.NetPoolCreate("artswapd", 128 * 1024, 0);
    const int ssl_ctx = f.SslInit(1024 * 1024);
    const int http_ctx = pool >= 0 && ssl_ctx >= 0 ? f.Init(pool, ssl_ctx, 1024 * 1024) : -1;
    g_template = http_ctx >= 0 ? f.CreateTemplate(http_ctx, USER_AGENT, 2 /* HTTP/1.1 */, 1 /* system proxy */) : -1;
    const int callback = g_template >= 0 ? f.SetSslCallback(g_template, check_certificate, NULL) : -1;
    if (g_template >= 0 && f.SetConnectTimeOut)
        f.SetConnectTimeOut(g_template, 10u * 1000000u);
    if (g_template >= 0 && f.SetRecvTimeOut)
        f.SetRecvTimeOut(g_template, 30u * 1000000u);
    log_line("sceNetInit 0x%08x, pool %d, TLS %d, HTTP %d, template %d, certificate callback 0x%08x",
             (unsigned)net_init, pool, ssl_ctx, http_ctx, g_template, (unsigned)callback);
    if (g_template < 0 || callback < 0) {
        snprintf(err, errlen, "network set-up failed (pool 0x%08x, TLS 0x%08x, HTTP 0x%08x, template 0x%08x)",
                 (unsigned)pool, (unsigned)ssl_ctx, (unsigned)http_ctx, (unsigned)g_template);
        return false;
    }
    g_backend = "http1";
    return true;
}

const char *http_backend(void)
{
    return g_backend;
}

bool http_get(const char *url, const char *bearer, buf_t *out, int *status, char *err, size_t errlen)
{
    const uint64_t start = now_ms();
    out->len = 0;
    *status = 0;
    const int conn = f.CreateConnectionWithURL(g_template, url, 1);
    if (conn < 0) {
        snprintf(err, errlen, "sceHttpCreateConnectionWithURL 0x%08x", (unsigned)conn);
        log_line("GET %s: %s", url, err);
        return false;
    }
    const int req = f.CreateRequestWithURL(conn, 0 /* GET */, url, 0);
    bool ok = req >= 0;
    if (!ok)
        snprintf(err, errlen, "sceHttpCreateRequestWithURL 0x%08x", (unsigned)req);
    if (ok && bearer != NULL && f.AddRequestHeader) {
        char value[128];
        snprintf(value, sizeof(value), "Bearer %s", bearer);
        const int added = f.AddRequestHeader(req, "Authorization", value, 0 /* replace */);
        if (added < 0) {
            snprintf(err, errlen, "sceHttpAddRequestHeader 0x%08x", (unsigned)added);
            ok = false;
        }
    }
    int rc = ok ? f.SendRequest(req, NULL, 0) : 0;
    if (ok && rc < 0) {
        int ssl_error = 0;
        uint32_t detail = 0;
        if (f.GetSslError && f.GetSslError(req, &ssl_error, &detail) == 0)
            snprintf(err, errlen, "the connection failed (0x%08x, TLS 0x%08x/0x%x)", (unsigned)rc,
                     (unsigned)ssl_error, detail);
        else
            snprintf(err, errlen, "the connection failed (0x%08x)", (unsigned)rc);
        ok = false;
    }
    if (ok && (rc = f.GetStatusCode(req, status)) < 0) {
        snprintf(err, errlen, "sceHttpGetStatusCode 0x%08x", (unsigned)rc);
        ok = false;
    }
    while (ok) {
        if (!buf_reserve(out, 65536)) {
            snprintf(err, errlen, "out of memory");
            ok = false;
            break;
        }
        const int got = f.ReadData(req, out->data + out->len, 65536);
        if (got < 0) {
            snprintf(err, errlen, "sceHttpReadData 0x%08x", (unsigned)got);
            ok = false;
        } else if (got == 0) {
            break;
        } else {
            out->len += (size_t)got;
        }
    }
    if (out->data)
        out->data[out->len] = 0;
    if (req >= 0)
        f.DeleteRequest(req);
    f.DeleteConnection(conn);
    log_line("GET %s -> %s %d, %zu bytes, %llu ms%s%s", url, ok ? "status" : "failed", *status, out->len,
             (unsigned long long)(now_ms() - start), ok ? "" : ": ", ok ? "" : err);
    return ok;
}
