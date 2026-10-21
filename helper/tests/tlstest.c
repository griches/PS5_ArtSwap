/*
 * Art Swap helper - why TLS fails from a payload (a test payload, HTTP/1.1 only).
 *
 * For each address: plain request; then the TLS library's own error
 * (sceHttpsGetSslError); then the same with a certificate callback that logs
 * what the library found and accepts it. Log: /data/artswap/tlstest.log.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/artswap.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static int (*NetInit)(void);
static int (*NetPoolCreate)(const char *, int, int);
static int (*SslInit)(size_t);
static int (*Init)(int, int, size_t);
static int (*CreateTemplate)(int, const char *, int, int);
static int (*CreateConnectionWithURL)(int, const char *, int);
static int (*CreateRequestWithURL)(int, int, const char *, uint64_t);
static int (*SendRequest)(int, const void *, size_t);
static int (*GetStatusCode)(int, int *);
static int (*ReadData)(int, void *, size_t);
static int (*DeleteRequest)(int);
static int (*DeleteConnection)(int);
static int (*GetSslError)(int, int *, uint32_t *);
static int (*SetSslCallback)(int, int (*)(int, unsigned int, void *const *, int, void *), void *);

static int accept_any(int ctx, unsigned int verify, void *const *certs, int count, void *user)
{
    (void)certs;
    (void)user;
    log_line("  callback: ssl ctx %d, verify error 0x%08x, %d certificates", ctx, verify, count);
    return 0;
}

static void get(int tmpl, const char *url)
{
    const int conn = CreateConnectionWithURL(tmpl, url, 0);
    const int req = conn >= 0 ? CreateRequestWithURL(conn, 0, url, 0) : -1;
    const int sent = req >= 0 ? SendRequest(req, NULL, 0) : -1;
    int status = 0;
    if (sent >= 0)
        GetStatusCode(req, &status);
    char body[200] = "";
    int got = sent >= 0 ? ReadData(req, body, sizeof(body) - 1) : 0;
    if (got > 0)
        body[got] = 0;
    for (char *p = body; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    int ssl_err = 0;
    uint32_t detail = 0;
    const int e = (sent < 0 && GetSslError && req >= 0) ? GetSslError(req, &ssl_err, &detail) : 1;
    log_line("%s: conn %d req %d sent 0x%08x status %d | ssl %s 0x%08x detail 0x%08x | %.80s", url, conn, req,
             (unsigned)sent, status, e == 0 ? "error" : "-", (unsigned)ssl_err, detail, body);
    if (req >= 0)
        DeleteRequest(req);
    if (conn >= 0)
        DeleteConnection(conn);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    log_open_named("tlstest.log");
    log_quiet(true);
    void *net = dlopen("libSceNet.sprx", RTLD_LAZY), *ssl = dlopen("libSceSsl.sprx", RTLD_LAZY),
         *http = dlopen("libSceHttp.sprx", RTLD_LAZY);
    *(void **)&NetInit = dlsym(net, "sceNetInit");
    *(void **)&NetPoolCreate = dlsym(net, "sceNetPoolCreate");
    *(void **)&SslInit = dlsym(ssl, "sceSslInit");
    *(void **)&Init = dlsym(http, "sceHttpInit");
    *(void **)&CreateTemplate = dlsym(http, "sceHttpCreateTemplate");
    *(void **)&CreateConnectionWithURL = dlsym(http, "sceHttpCreateConnectionWithURL");
    *(void **)&CreateRequestWithURL = dlsym(http, "sceHttpCreateRequestWithURL");
    *(void **)&SendRequest = dlsym(http, "sceHttpSendRequest");
    *(void **)&GetStatusCode = dlsym(http, "sceHttpGetStatusCode");
    *(void **)&ReadData = dlsym(http, "sceHttpReadData");
    *(void **)&DeleteRequest = dlsym(http, "sceHttpDeleteRequest");
    *(void **)&DeleteConnection = dlsym(http, "sceHttpDeleteConnection");
    *(void **)&GetSslError = dlsym(http, "sceHttpsGetSslError");
    *(void **)&SetSslCallback = dlsym(http, "sceHttpsSetSslCallback");
    log_line("symbols: GetSslError %p SetSslCallback %p", (void *)GetSslError, (void *)SetSslCallback);
    NetInit();
    const int pool = NetPoolCreate("tlstest", 128 * 1024, 0);
    const int sslctx = SslInit(1024 * 1024);
    const int ctx = Init(pool, sslctx, 1024 * 1024);
    const int tmpl = CreateTemplate(ctx, "ArtSwap-tlstest", 2, 1);
    log_line("pool %d ssl %d http %d template %d", pool, sslctx, ctx, tmpl);
    static const char *urls[] = {"http://example.com/", "https://example.com/",
                                 "https://www.steamgriddb.com/api/v2/search/autocomplete/portal",
                                 "https://cdn2.steamgriddb.com/thumb/dce5eb1f56dd8dd4e1b9a1e22c0e1be0.jpg"};
    log_line("--- the system's checks");
    for (int i = 0; i < 4; i++)
        get(tmpl, urls[i]);
    if (SetSslCallback) {
        const int tmpl2 = CreateTemplate(ctx, "ArtSwap-tlstest", 2, 1);
        const int set = SetSslCallback(tmpl2, accept_any, NULL);
        log_line("--- with a certificate callback (template %d, set 0x%08x)", tmpl2, (unsigned)set);
        for (int i = 1; i < 4; i++)
            get(tmpl2, urls[i]);
    }
    log_line("tlstest done");
    return 0;
}
