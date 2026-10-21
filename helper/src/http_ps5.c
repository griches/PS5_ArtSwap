/*
 * Art Swap helper - HTTPS on the console, through its own libraries.
 *
 * libSceHttp2 first (the payload SDK's http2_get sample is the model), then
 * libSceHttp (HTTP/1.1) if HTTP/2 cannot be set up or a request fails at the
 * transport level; whichever answers is kept for the rest of the run. The
 * libraries are opened with dlopen at run time, not linked, so that one the
 * loader cannot give this process costs the network, not the whole helper.
 * A request whose TLS fails is tried once more with the certificate checks
 * off, and the log says so. Only exported functions of the system software
 * are called.
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
/* the certificate checks, as libSceHttp names them: server, CN, not after, not before, known CA */
#define SSL_CHECKS 0x3du

static struct {
    int (*NetInit)(void);
    int (*NetPoolCreate)(const char *, int, int);
    int (*SslInit)(size_t);
    /* HTTP/2 */
    int (*H2Init)(int, int, size_t, int);
    int (*H2CreateTemplate)(int, const char *, int, int);
    int (*H2CreateRequestWithURL)(int, const char *, const char *, uint64_t);
    int (*H2DeleteRequest)(int);
    int (*H2AddRequestHeader)(int, const char *, const char *, uint32_t);
    int (*H2SendRequest)(int, const void *, size_t);
    int (*H2GetStatusCode)(int, int *);
    int (*H2ReadData)(int, void *, size_t);
    int (*H2SslDisableOption)(int, uint32_t);
    int (*H2SetConnectTimeOut)(int, uint32_t);
    int (*H2SetRecvTimeOut)(int, uint32_t);
    /* HTTP/1.1 */
    int (*H1Init)(int, int, size_t);
    int (*H1CreateTemplate)(int, const char *, int, int);
    int (*H1CreateConnectionWithURL)(int, const char *, int);
    int (*H1CreateRequestWithURL)(int, int, const char *, uint64_t);
    int (*H1AddRequestHeader)(int, const char *, const char *, uint32_t);
    int (*H1SendRequest)(int, const void *, size_t);
    int (*H1GetStatusCode)(int, int *);
    int (*H1ReadData)(int, void *, size_t);
    int (*H1DeleteRequest)(int);
    int (*H1DeleteConnection)(int);
    int (*H1SslDisableOption)(int, uint32_t);
    int (*H1SetConnectTimeOut)(int, uint32_t);
    int (*H1SetRecvTimeOut)(int, uint32_t);
} f;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_net = -1, g_ssl = -1;
static int g_h2 = -1, g_h2_tmpl[2] = {-1, -1}; /* [0] checks certificates, [1] does not */
static int g_h1 = -1, g_h1_tmpl[2] = {-1, -1};
static bool g_h2_tried, g_h1_tried;
static const char *g_backend = "none";
static bool g_prefer_h1;

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

bool http_init(char *err, size_t errlen)
{
    void *net = open_library("libSceNet.sprx");
    void *ssl = open_library("libSceSsl.sprx");
    void *h2 = open_library("libSceHttp2.sprx");
    void *h1 = open_library("libSceHttp.sprx");
    FIND(net, NetInit, "sceNetInit");
    FIND(net, NetPoolCreate, "sceNetPoolCreate");
    FIND(ssl, SslInit, "sceSslInit");
    FIND(h2, H2Init, "sceHttp2Init");
    FIND(h2, H2CreateTemplate, "sceHttp2CreateTemplate");
    FIND(h2, H2CreateRequestWithURL, "sceHttp2CreateRequestWithURL");
    FIND(h2, H2DeleteRequest, "sceHttp2DeleteRequest");
    FIND(h2, H2AddRequestHeader, "sceHttp2AddRequestHeader");
    FIND(h2, H2SendRequest, "sceHttp2SendRequest");
    FIND(h2, H2GetStatusCode, "sceHttp2GetStatusCode");
    FIND(h2, H2ReadData, "sceHttp2ReadData");
    FIND(h2, H2SslDisableOption, "sceHttp2SslDisableOption");
    FIND(h2, H2SetConnectTimeOut, "sceHttp2SetConnectTimeOut");
    FIND(h2, H2SetRecvTimeOut, "sceHttp2SetRecvTimeOut");
    FIND(h1, H1Init, "sceHttpInit");
    FIND(h1, H1CreateTemplate, "sceHttpCreateTemplate");
    FIND(h1, H1CreateConnectionWithURL, "sceHttpCreateConnectionWithURL");
    FIND(h1, H1CreateRequestWithURL, "sceHttpCreateRequestWithURL");
    FIND(h1, H1AddRequestHeader, "sceHttpAddRequestHeader");
    FIND(h1, H1SendRequest, "sceHttpSendRequest");
    FIND(h1, H1GetStatusCode, "sceHttpGetStatusCode");
    FIND(h1, H1ReadData, "sceHttpReadData");
    FIND(h1, H1DeleteRequest, "sceHttpDeleteRequest");
    FIND(h1, H1DeleteConnection, "sceHttpDeleteConnection");
    FIND(h1, H1SslDisableOption, "sceHttpsDisableOption");
    FIND(h1, H1SetConnectTimeOut, "sceHttpSetConnectTimeOut");
    FIND(h1, H1SetRecvTimeOut, "sceHttpSetRecvTimeOut");

    if (f.NetInit == NULL || f.NetPoolCreate == NULL || f.SslInit == NULL) {
        snprintf(err, errlen, "the console's network libraries did not load");
        return false;
    }
    const int net_init = f.NetInit();
    g_net = f.NetPoolCreate("artswapd", 128 * 1024, 0);
    g_ssl = f.SslInit(1024 * 1024);
    log_line("sceNetInit 0x%08x, pool %d, sceSslInit %d", (unsigned)net_init, g_net, g_ssl);
    if (g_net < 0 || g_ssl < 0) {
        snprintf(err, errlen, "network set-up failed (pool 0x%08x, TLS 0x%08x)", (unsigned)g_net,
                 (unsigned)g_ssl);
        return false;
    }
    return true;
}

const char *http_backend(void)
{
    return g_backend;
}

/* ---- HTTP/2 ---- */

static bool h2_ready(char *err, size_t errlen)
{
    pthread_mutex_lock(&g_lock);
    if (!g_h2_tried) {
        g_h2_tried = true;
        if (f.H2Init && f.H2CreateTemplate && f.H2CreateRequestWithURL && f.H2SendRequest &&
            f.H2GetStatusCode && f.H2ReadData && f.H2DeleteRequest) {
            g_h2 = f.H2Init(g_net, g_ssl, 1024 * 1024, 4);
            for (int i = 0; g_h2 >= 0 && i < 2; i++) {
                g_h2_tmpl[i] = f.H2CreateTemplate(g_h2, USER_AGENT, 3, 1);
                if (g_h2_tmpl[i] < 0)
                    continue;
                if (f.H2SetConnectTimeOut)
                    f.H2SetConnectTimeOut(g_h2_tmpl[i], 10u * 1000000u);
                if (f.H2SetRecvTimeOut)
                    f.H2SetRecvTimeOut(g_h2_tmpl[i], 30u * 1000000u);
                if (i == 1 && f.H2SslDisableOption)
                    log_line("HTTP/2 template without checks: 0x%08x",
                             (unsigned)f.H2SslDisableOption(g_h2_tmpl[i], SSL_CHECKS));
            }
            log_line("sceHttp2Init %d, templates %d %d", g_h2, g_h2_tmpl[0], g_h2_tmpl[1]);
        }
    }
    const bool ok = g_h2_tmpl[0] >= 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        snprintf(err, errlen, "HTTP/2 is not available (0x%08x)", (unsigned)g_h2);
    return ok;
}

static bool h2_get(int tmpl, const char *url, const char *bearer, buf_t *out, int *status, char *err,
                   size_t errlen)
{
    const int req = f.H2CreateRequestWithURL(tmpl, "GET", url, 0);
    if (req < 0) {
        snprintf(err, errlen, "sceHttp2CreateRequestWithURL 0x%08x", (unsigned)req);
        return false;
    }
    bool ok = true;
    if (bearer != NULL && f.H2AddRequestHeader) {
        char value[128];
        snprintf(value, sizeof(value), "Bearer %s", bearer);
        const int added = f.H2AddRequestHeader(req, "Authorization", value, 0);
        if (added < 0) {
            snprintf(err, errlen, "sceHttp2AddRequestHeader 0x%08x", (unsigned)added);
            ok = false;
        }
    }
    int rc = ok ? f.H2SendRequest(req, NULL, 0) : 0;
    if (ok && rc < 0) {
        snprintf(err, errlen, "sceHttp2SendRequest 0x%08x", (unsigned)rc);
        ok = false;
    }
    if (ok && (rc = f.H2GetStatusCode(req, status)) < 0) {
        snprintf(err, errlen, "sceHttp2GetStatusCode 0x%08x", (unsigned)rc);
        ok = false;
    }
    while (ok) {
        if (!buf_reserve(out, 65536)) {
            snprintf(err, errlen, "out of memory");
            ok = false;
            break;
        }
        const int got = f.H2ReadData(req, out->data + out->len, 65536);
        if (got < 0) {
            snprintf(err, errlen, "sceHttp2ReadData 0x%08x", (unsigned)got);
            ok = false;
        } else if (got == 0) {
            break;
        } else {
            out->len += (size_t)got;
        }
    }
    if (out->data)
        out->data[out->len] = 0;
    f.H2DeleteRequest(req);
    return ok;
}

/* ---- HTTP/1.1 ---- */

static bool h1_ready(char *err, size_t errlen)
{
    pthread_mutex_lock(&g_lock);
    if (!g_h1_tried) {
        g_h1_tried = true;
        if (f.H1Init && f.H1CreateTemplate && f.H1CreateConnectionWithURL && f.H1CreateRequestWithURL &&
            f.H1SendRequest && f.H1GetStatusCode && f.H1ReadData && f.H1DeleteRequest &&
            f.H1DeleteConnection) {
            g_h1 = f.H1Init(g_net, g_ssl, 1024 * 1024);
            for (int i = 0; g_h1 >= 0 && i < 2; i++) {
                g_h1_tmpl[i] = f.H1CreateTemplate(g_h1, USER_AGENT, 2, 1); /* HTTP/1.1, system proxy */
                if (g_h1_tmpl[i] < 0)
                    continue;
                if (f.H1SetConnectTimeOut)
                    f.H1SetConnectTimeOut(g_h1_tmpl[i], 10u * 1000000u);
                if (f.H1SetRecvTimeOut)
                    f.H1SetRecvTimeOut(g_h1_tmpl[i], 30u * 1000000u);
                if (i == 1 && f.H1SslDisableOption)
                    log_line("HTTP/1.1 template without checks: 0x%08x",
                             (unsigned)f.H1SslDisableOption(g_h1_tmpl[i], SSL_CHECKS));
            }
            log_line("sceHttpInit %d, templates %d %d", g_h1, g_h1_tmpl[0], g_h1_tmpl[1]);
        }
    }
    const bool ok = g_h1_tmpl[0] >= 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        snprintf(err, errlen, "HTTP/1.1 is not available (0x%08x)", (unsigned)g_h1);
    return ok;
}

static bool h1_get(int tmpl, const char *url, const char *bearer, buf_t *out, int *status, char *err,
                   size_t errlen)
{
    const int conn = f.H1CreateConnectionWithURL(tmpl, url, 1);
    if (conn < 0) {
        snprintf(err, errlen, "sceHttpCreateConnectionWithURL 0x%08x", (unsigned)conn);
        return false;
    }
    const int req = f.H1CreateRequestWithURL(conn, 0 /* GET */, url, 0);
    if (req < 0) {
        snprintf(err, errlen, "sceHttpCreateRequestWithURL 0x%08x", (unsigned)req);
        f.H1DeleteConnection(conn);
        return false;
    }
    bool ok = true;
    if (bearer != NULL && f.H1AddRequestHeader) {
        char value[128];
        snprintf(value, sizeof(value), "Bearer %s", bearer);
        const int added = f.H1AddRequestHeader(req, "Authorization", value, 0);
        if (added < 0) {
            snprintf(err, errlen, "sceHttpAddRequestHeader 0x%08x", (unsigned)added);
            ok = false;
        }
    }
    int rc = ok ? f.H1SendRequest(req, NULL, 0) : 0;
    if (ok && rc < 0) {
        snprintf(err, errlen, "sceHttpSendRequest 0x%08x", (unsigned)rc);
        ok = false;
    }
    if (ok && (rc = f.H1GetStatusCode(req, status)) < 0) {
        snprintf(err, errlen, "sceHttpGetStatusCode 0x%08x", (unsigned)rc);
        ok = false;
    }
    while (ok) {
        if (!buf_reserve(out, 65536)) {
            snprintf(err, errlen, "out of memory");
            ok = false;
            break;
        }
        const int got = f.H1ReadData(req, out->data + out->len, 65536);
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
    f.H1DeleteRequest(req);
    f.H1DeleteConnection(conn);
    return ok;
}

/* ---- either ---- */

static bool try_backend(bool h1, const char *url, const char *bearer, buf_t *out, int *status, char *err,
                        size_t errlen)
{
    if (!(h1 ? h1_ready(err, errlen) : h2_ready(err, errlen)))
        return false;
    const int *tmpl = h1 ? g_h1_tmpl : g_h2_tmpl;
    for (int checks_off = 0; checks_off < 2; checks_off++) {
        if (tmpl[checks_off] < 0)
            break;
        out->len = 0;
        *status = 0;
        const bool ok = h1 ? h1_get(tmpl[checks_off], url, bearer, out, status, err, errlen)
                           : h2_get(tmpl[checks_off], url, bearer, out, status, err, errlen);
        if (ok) {
            if (checks_off)
                log_line("%s answered only without certificate checks", h1 ? "HTTP/1.1" : "HTTP/2");
            return true;
        }
        log_line("%s%s: %s", h1 ? "HTTP/1.1" : "HTTP/2", checks_off ? " (checks off)" : "", err);
    }
    return false;
}

bool http_get(const char *url, const char *bearer, buf_t *out, int *status, char *err, size_t errlen)
{
    const uint64_t start = now_ms();
    const bool first_h1 = g_prefer_h1;
    char first_err[256] = "";
    bool ok = try_backend(first_h1, url, bearer, out, status, first_err, sizeof(first_err));
    if (ok) {
        g_backend = first_h1 ? "http1" : "http2";
    } else {
        ok = try_backend(!first_h1, url, bearer, out, status, err, errlen);
        if (ok) {
            g_prefer_h1 = !first_h1;
            g_backend = g_prefer_h1 ? "http1" : "http2";
            log_line("using %s from now on", g_backend);
        } else {
            char second[256];
            copy_str(second, sizeof(second), err);
            snprintf(err, errlen, "%s; %s", first_err, second);
        }
    }
    log_line("GET %s -> %s %d, %zu bytes, %llu ms", url, ok ? "status" : "failed", *status, out->len,
             (unsigned long long)(now_ms() - start));
    return ok;
}
