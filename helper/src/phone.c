/*
 * Art Swap helper - the key page, for adding a SteamGridDB key from a phone.
 *
 * Typing a 32-character key on the console's keyboard is slow, so the title
 * offers a page instead: while its "From your phone" sheet is open (PHONE
 * <code>), the helper serves one small page on port 9118 of the console's
 * network address. The page asks for the key, and the four-digit code the TV
 * shows (the QR code carries it). A key with the right code is checked with
 * SteamGridDB, then kept as the person's own (config_set_key). Nothing listens
 * on 9118 at any other time; the page closes after ten minutes, when the
 * sheet closes, or after ten wrong codes.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "phone.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PHONE_PORT 9118
#define SESSION_MS (10 * 60 * 1000)
#define MAX_WRONG 10

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_code[8];
static uint64_t g_until;
static int g_wrong;
static bool g_running;
static int g_port;

/* ---- the console's address ---- */

const char *phone_lan_address(char *out, size_t size)
{
    out[0] = 0;
#ifdef ARTSWAP_HOST
    if (getenv("ARTSWAP_LAN")) {
        copy_str(out, size, getenv("ARTSWAP_LAN"));
        return out;
    }
#endif
    /* connect() on a UDP socket sends nothing, but picks the address a packet out would leave from */
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return out;
    struct sockaddr_in far;
    memset(&far, 0, sizeof(far));
    far.sin_family = AF_INET;
    far.sin_port = htons(53);
    far.sin_addr.s_addr = htonl(0x08080808); /* any address off the local network does */
    struct sockaddr_in near;
    socklen_t len = sizeof(near);
    if (connect(fd, (struct sockaddr *)&far, sizeof(far)) == 0 &&
        getsockname(fd, (struct sockaddr *)&near, &len) == 0 && near.sin_addr.s_addr != 0) {
        const uint32_t a = ntohl(near.sin_addr.s_addr);
        snprintf(out, size, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    }
    close(fd);
    return out;
}

/* ---- the pages ---- */

static const char kStyle[] =
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{margin:0;font:17px/1.45 -apple-system,system-ui,'Segoe UI',Roboto,sans-serif;color:#eef1ff;"
    "background:radial-gradient(120% 80% at 80% 0%,#3a2a8c 0%,#140f33 45%,#07060f 100%) fixed;min-height:100vh}"
    "main{max-width:440px;margin:0 auto;padding:28px 20px 40px}"
    ".brand{font-weight:700;letter-spacing:.02em;font-size:15px;opacity:.8}"
    ".brand span{display:inline-block;width:10px;height:10px;border-radius:50%;background:#9f8cff;margin-right:8px}"
    "h1{font-size:28px;line-height:1.15;margin:18px 0 10px;font-weight:700}"
    "p{margin:0 0 14px;color:#c9cdf0}"
    "ol{padding:0;margin:20px 0;list-style:none;counter-reset:s}"
    "ol li{counter-increment:s;position:relative;padding:0 0 14px 44px;color:#dfe2ff}"
    "ol li:before{content:counter(s);position:absolute;left:0;top:-1px;width:30px;height:30px;border-radius:50%;"
    "background:#9f8cff;color:#120d2b;font-weight:700;display:flex;align-items:center;justify-content:center}"
    "a{color:#b8abff}"
    ".card{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.14);border-radius:20px;padding:20px;"
    "backdrop-filter:blur(12px)}"
    "label{display:block;font-size:13px;letter-spacing:.12em;text-transform:uppercase;color:#a9aed6;margin:4px 0 8px}"
    "input{width:100%;box-sizing:border-box;font:16px ui-monospace,Menlo,monospace;padding:14px;border-radius:12px;"
    "border:1px solid rgba(255,255,255,.25);background:rgba(0,0,0,.35);color:#fff;margin-bottom:16px}"
    "input.code{font-size:24px;letter-spacing:.4em;text-align:center}"
    "button{width:100%;font:600 18px system-ui,sans-serif;padding:15px;border:0;border-radius:999px;"
    "background:#fff;color:#120d2b}"
    ".ok{font-size:56px;margin:10px 0 0}.err{color:#ff9b9b}.small{font-size:14px;color:#9aa0c8;margin-top:18px}"
    "</style>";

static void page_form(buf_t *out, const char *code, const char *message)
{
    buf_printf(out, "<!doctype html><html><head><meta charset=utf-8><title>Art Swap: your key</title>%s</head><body><main>",
               kStyle);
    buf_printf(out, "<div class=brand><span></span>Art Swap on your PS5</div>"
                    "<h1>Add your SteamGridDB API key</h1>"
                    "<p>Art Swap finds new icons and backgrounds on SteamGridDB. Its API needs a key of your own: "
                    "it is free.</p>"
                    "<ol><li>Sign in (or make a free account) at "
                    "<a href='https://www.steamgriddb.com/profile/preferences/api' target=_blank>steamgriddb.com</a>.</li>"
                    "<li>Open <b>Preferences</b>, then <b>API</b>, and copy your key.</li>"
                    "<li>Paste it below and press Save. Your PS5 shows when it is in.</li></ol>");
    if (message && message[0])
        buf_printf(out, "<p class=err>%s</p>", message);
    buf_printf(out, "<form class=card method=post action='/'>");
    if (code && code[0])
        buf_printf(out, "<input type=hidden name=code value='%s'>", code);
    else
        buf_printf(out, "<label for=code>The code on your TV</label>"
                        "<input class=code id=code name=code inputmode=numeric maxlength=4 autocomplete=off required>");
    buf_printf(out, "<label for=key>Your API key</label>"
                    "<input id=key name=key autocomplete=off autocapitalize=off spellcheck=false required "
                    "placeholder='32 letters and numbers'>"
                    "<button>Save to my PS5</button></form>"
                    "<p class=small>The key stays on your PS5 (in /data/artswap). This page closes when you leave "
                    "the key screen on your TV.</p></main></body></html>");
}

static void page_done(buf_t *out)
{
    buf_printf(out, "<!doctype html><html><head><meta charset=utf-8><title>Art Swap: key saved</title>%s</head><body><main>"
                    "<div class=brand><span></span>Art Swap on your PS5</div><div class=ok>&#10003;</div>"
                    "<h1>Your key is saved</h1><p>SteamGridDB accepted it, and Art Swap on your PS5 is ready. "
                    "You can close this page.</p></main></body></html>",
               kStyle);
}

static void page_closed(buf_t *out)
{
    buf_printf(out, "<!doctype html><html><head><meta charset=utf-8><title>Art Swap</title>%s</head><body><main>"
                    "<div class=brand><span></span>Art Swap on your PS5</div>"
                    "<h1>This page has closed</h1><p>Open <b>Add your key from your phone</b> in Art Swap's settings "
                    "on your PS5 again, and scan the new code.</p></main></body></html>",
               kStyle);
}

/* ---- HTTP ---- */

/* The header's value in a request head (the name matched without regard to case) */
static const char *header_value(const char *head, const char *name)
{
    const size_t len = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        const char *p = line + 2;
        if (p[0] == '\r')
            break;
        size_t i = 0;
        while (i < len && p[i] && (p[i] | 0x20) == (name[i] | 0x20))
            i++;
        if (i == len && p[len] == ':')
            return p + len + 1;
    }
    return NULL;
}

static void respond(int fd, int status, const buf_t *body)
{
    const char *reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : "Error";
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, reason, body->len);
    send(fd, head, strlen(head), 0);
    if (body->len)
        send(fd, body->data, body->len, 0);
}

/* The value of name in an application/x-www-form-urlencoded body */
static void form_value(const char *body, const char *name, char *out, size_t size)
{
    out[0] = 0;
    const size_t name_len = strlen(name);
    for (const char *p = body; p && *p;) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < size) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = {p[1], p[2], 0};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = 0;
            trim(out);
            return;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
}

static bool session_open(char *code, size_t size)
{
    pthread_mutex_lock(&g_lock);
    const bool open = g_code[0] && now_ms() < g_until && g_wrong < MAX_WRONG;
    if (open)
        copy_str(code, size, g_code);
    pthread_mutex_unlock(&g_lock);
    return open;
}

static void serve(int fd)
{
    char request[16384];
    size_t used = 0;
    char *body = NULL;
    size_t content_length = 0;
    /* the head, then as much body as it announces */
    while (used + 1 < sizeof(request)) {
        struct pollfd wait = {fd, POLLIN, 0};
        if (poll(&wait, 1, 5000) <= 0)
            break;
        const ssize_t got = recv(fd, request + used, sizeof(request) - 1 - used, 0);
        if (got <= 0)
            break;
        used += (size_t)got;
        request[used] = 0;
        if (body == NULL && (body = strstr(request, "\r\n\r\n")) != NULL) {
            body += 4;
            const char *length = header_value(request, "Content-Length");
            content_length = length ? strtoul(length, NULL, 10) : 0;
        }
        if (body && (size_t)(request + used - body) >= content_length)
            break;
    }
    request[used] = 0;
    buf_t page = {0};
    char code[8];
    char path[64] = "";
    sscanf(request, "%*s %63s", path);
    const bool post = strncmp(request, "POST ", 5) == 0;
    if (!session_open(code, sizeof(code))) {
        page_closed(&page);
        respond(fd, 404, &page);
    } else if (!post) {
        /* the QR code's address carries the code: /1234 */
        const bool with_code = strcmp(path + 1, code) == 0;
        if (strcmp(path, "/favicon.ico") == 0) {
            respond(fd, 404, &page);
        } else {
            page_form(&page, with_code ? code : NULL, NULL);
            respond(fd, 200, &page);
        }
    } else {
        char sent_code[16], key[96];
        form_value(body ? body : "", "code", sent_code, sizeof(sent_code));
        form_value(body ? body : "", "key", key, sizeof(key));
        if (strcmp(sent_code, code) != 0) {
            pthread_mutex_lock(&g_lock);
            g_wrong++;
            pthread_mutex_unlock(&g_lock);
            log_line("phone: a wrong code");
            page_form(&page, NULL, "That is not the code on your TV.");
            respond(fd, 403, &page);
        } else {
            char err[300] = "";
            if (!sgdb_check_key(key, err, sizeof(err))) {
                char message[400];
                snprintf(message, sizeof(message), "SteamGridDB did not accept that key (%s). Copy it again from "
                                                   "Preferences, API, and paste it here.",
                         err);
                page_form(&page, code, message);
                respond(fd, 200, &page);
            } else if (!config_set_key(key)) {
                page_form(&page, code, "Your PS5 could not keep the key. Try again.");
                respond(fd, 200, &page);
            } else {
                log_line("phone: key saved");
                page_done(&page);
                respond(fd, 200, &page);
                phone_stop();
            }
        }
    }
    buf_free(&page);
}

static void *listen_thread(void *argument)
{
    (void)argument;
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)g_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 4) != 0) {
        log_line("phone: port %d: %s", g_port, strerror(errno));
        if (server >= 0)
            close(server);
        pthread_mutex_lock(&g_lock);
        g_running = false;
        g_code[0] = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    log_line("phone: the key page is open on port %d", g_port);
    char code[8];
    while (session_open(code, sizeof(code))) {
        struct pollfd wait = {server, POLLIN, 0};
        if (poll(&wait, 1, 500) <= 0)
            continue;
        const int client = accept(server, NULL, NULL);
        if (client < 0)
            continue;
        serve(client);
        close(client);
    }
    close(server);
    pthread_mutex_lock(&g_lock);
    g_running = false;
    pthread_mutex_unlock(&g_lock);
    log_line("phone: the key page has closed");
    return NULL;
}

bool phone_start(const char *code, char *err, size_t errlen)
{
    if (strlen(code) != 4) {
        snprintf(err, errlen, "the code is four digits");
        return false;
    }
    g_port = PHONE_PORT;
#ifdef ARTSWAP_HOST
    if (getenv("ARTSWAP_PHONE_PORT"))
        g_port = atoi(getenv("ARTSWAP_PHONE_PORT"));
#endif
    pthread_mutex_lock(&g_lock);
    copy_str(g_code, sizeof(g_code), code);
    g_until = now_ms() + SESSION_MS;
    g_wrong = 0;
    const bool start = !g_running;
    g_running = true;
    pthread_mutex_unlock(&g_lock);
    if (start) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, listen_thread, NULL) != 0) {
            pthread_mutex_lock(&g_lock);
            g_running = false;
            pthread_mutex_unlock(&g_lock);
            snprintf(err, errlen, "the key page could not start");
            return false;
        }
        pthread_detach(thread);
    }
    return true;
}

void phone_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_code[0] = 0;
    pthread_mutex_unlock(&g_lock);
}

int phone_port(void)
{
    return g_port ? g_port : PHONE_PORT;
}
