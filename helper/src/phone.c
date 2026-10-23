/*
 * Art Swap helper - the phone page: a SteamGridDB key, or music, sent from a phone.
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
 * Opened for music (PHONE <code> music), the page instead sends the files
 * chosen on the phone, one PUT each, into /data/artswap/music (music.c), where
 * the title lists them. The code goes with each.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "phone.h"
#include "music.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>

#define PHONE_PORT 9118
#define SESSION_MS (10 * 60 * 1000)
#define MAX_WRONG 10
#define UPLOAD_MAX ((size_t)400 << 20) /* a long FLAC album track fits */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_code[8];
static uint64_t g_until;
static int g_wrong;
static bool g_running;
static int g_port;
static bool g_music; /* the page sends music, not a key */

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

/* Sends the files chosen, one PUT /upload?code=&name= each, the TV listing them as they come */
static void page_music(buf_t *out, const char *code)
{
    buf_printf(out, "<!doctype html><html><head><meta charset=utf-8><title>Art Swap: send music</title>%s</head><body><main>",
               kStyle);
    buf_printf(out, "<div class=brand><span></span>Art Swap on your PS5</div>"
                    "<h1>Send music to your PS5</h1>"
                    "<p>Choose songs on this phone (MP3, FLAC, WAV or AIFF). They go to your PS5, where Art Swap "
                    "makes a clip of one into the music a game's tile plays. Use music that is yours to use.</p>"
                    "<div class=card>");
    if (code && code[0])
        buf_printf(out, "<input type=hidden id=code value='%s'>", code);
    else
        buf_printf(out, "<label for=code>The code on your TV</label>"
                        "<input class=code id=code inputmode=numeric maxlength=4 autocomplete=off required>");
    buf_printf(out, "<label for=files>Your music</label>"
                    "<input id=files type=file multiple accept='audio/*,.mp3,.flac,.wav,.aif,.aiff'>"
                    "<button id=send>Send to my PS5</button><div id=list></div></div>"
                    "<p class=small>The files stay on your PS5, in /data/artswap/music. This page closes when you "
                    "leave the music screen on your TV.</p></main>"
                    "<script>"
                    "const list=document.getElementById('list');"
                    "function row(t){const p=document.createElement('p');p.textContent=t;list.appendChild(p);return p}"
                    "function put(f,code){return new Promise(done=>{const p=row(f.name+': 0%%');"
                    "const x=new XMLHttpRequest();"
                    "x.open('PUT','/upload?code='+encodeURIComponent(code)+'&name='+encodeURIComponent(f.name));"
                    "x.upload.onprogress=e=>{if(e.lengthComputable)p.textContent=f.name+': '+Math.round(e.loaded*100/e.total)+'%%'};"
                    "x.onload=()=>{p.textContent=f.name+(x.status==200?': on your PS5 \u2713':': '+x.responseText);"
                    "p.className=x.status==200?'':'err';done()};"
                    "x.onerror=()=>{p.textContent=f.name+': the PS5 did not answer';p.className='err';done()};"
                    "x.send(f)})}"
                    "document.getElementById('send').onclick=async()=>{"
                    "const code=document.getElementById('code').value.trim();"
                    "for(const f of document.getElementById('files').files)await put(f,code)};"
                    "</script></body></html>");
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
                    "<h1>This page has closed</h1><p>Open it again in Art Swap on your PS5 (the key in its "
                    "settings, or Music's <b>From your phone</b>), and scan the new code.</p></main></body></html>",
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
    const char *reason = status == 200   ? "OK"
                         : status == 400 ? "Bad Request"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 413 ? "Too Large"
                                         : "Error";
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

static void wrong_code(void)
{
    pthread_mutex_lock(&g_lock);
    g_wrong++;
    pthread_mutex_unlock(&g_lock);
    log_line("phone: a wrong code");
}

static void answer(int fd, int status, const char *text)
{
    buf_t body = {0};
    buf_printf(&body, "%s", text);
    respond(fd, status, &body);
    buf_free(&body);
}

/* An upload's body: first what came with the head, then the socket */
typedef struct {
    int fd;
    const char *pending;
    size_t have;
} reader_t;

/* Up to n bytes: 0 at the end, -1 when nothing comes for 20 seconds or the connection fails */
static ssize_t reader_read(reader_t *r, char *out, size_t n)
{
    if (r->have > 0) {
        const size_t take = n < r->have ? n : r->have;
        memcpy(out, r->pending, take);
        r->pending += take;
        r->have -= take;
        return (ssize_t)take;
    }
    struct pollfd wait = {r->fd, POLLIN, 0};
    if (poll(&wait, 1, 20000) <= 0)
        return -1;
    return recv(r->fd, out, n, 0);
}

/* A line of a chunked body (a chunk's size, or the CRLF after its data), without its line break */
static bool reader_line(reader_t *r, char *line, size_t size)
{
    size_t o = 0;
    for (;;) {
        char c;
        if (reader_read(r, &c, 1) != 1)
            return false;
        if (c == '\n')
            break;
        if (c != '\r' && o + 1 < size)
            line[o++] = c;
    }
    line[o] = 0;
    return true;
}

/* n bytes of the body into out; false when they do not all come */
static bool reader_copy(reader_t *r, FILE *out, size_t n, char *buffer, size_t buffer_size)
{
    while (n > 0) {
        const ssize_t got = reader_read(r, buffer, n < buffer_size ? n : buffer_size);
        if (got <= 0 || fwrite(buffer, 1, (size_t)got, out) != (size_t)got)
            return false;
        n -= (size_t)got;
    }
    return true;
}

/* PUT /upload?code=&name=: a music file into the music folder. Its size is announced (Content-Length), or, as
 * some phones' browsers send a file, it comes in chunks, each announcing its own (Transfer-Encoding: chunked). */
static void upload(int fd, const char *code, const char *query, const char *first, size_t have, size_t length,
                   bool chunked)
{
    char sent_code[16], wanted[256], name[200], folder[512], partial[800], path[800];
    form_value(query, "code", sent_code, sizeof(sent_code));
    form_value(query, "name", wanted, sizeof(wanted));
    if (strcmp(sent_code, code) != 0) {
        wrong_code();
        answer(fd, 403, "That is not the code on your TV.");
        return;
    }
    if (!music_upload_name(wanted, name, sizeof(name))) {
        answer(fd, 400, "Art Swap reads MP3, FLAC, WAV and AIFF files.");
        return;
    }
    if (!chunked && (length == 0 || length > UPLOAD_MAX)) {
        answer(fd, length == 0 ? 400 : 413,
               length == 0 ? "The file came empty: try again." : "That file is too big for Art Swap (400 MB at most).");
        return;
    }
    music_folder(folder, sizeof(folder));
    snprintf(partial, sizeof(partial), "%s/.%s.partial", folder, name);
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    FILE *out = fopen(partial, "wb");
    /* on the heap: the console gives a thread a small stack */
    const size_t buffer_size = 65536;
    char *buffer = malloc(buffer_size);
    reader_t reader = {fd, first, have};
    size_t done = 0;
    bool ok = out != NULL && buffer != NULL, too_big = false;
    if (ok && !chunked) {
        ok = reader_copy(&reader, out, length, buffer, buffer_size);
        done = ok ? length : 0;
    }
    while (ok && chunked) {
        char line[64];
        ok = reader_line(&reader, line, sizeof(line));
        const size_t size = ok ? strtoul(line, NULL, 16) : 0;
        if (!ok || size == 0)
            break;
        if (done + size > UPLOAD_MAX) {
            too_big = true;
            ok = false;
            break;
        }
        ok = reader_copy(&reader, out, size, buffer, buffer_size) && reader_line(&reader, line, sizeof(line));
        done += size;
    }
    /* after the last chunk: any trailing headers, then an empty line */
    for (char line[256]; ok && chunked && reader_line(&reader, line, sizeof(line)) && line[0];)
        ;
    if (ok && done == 0)
        ok = false;
    free(buffer);
    if (out != NULL && fclose(out) != 0)
        ok = false;
    if (ok && rename(partial, path) != 0)
        ok = false;
    if (!ok) {
        unlink(partial);
        log_line("phone: %s did not arrive whole (%zu bytes%s)", name, done, chunked ? ", in chunks" : "");
        answer(fd, too_big ? 413 : 500,
               too_big ? "That file is too big for Art Swap (400 MB at most)." : "It did not arrive whole: try again.");
        return;
    }
    log_line("phone: received %s (%zu bytes%s)", name, done, chunked ? ", in chunks" : "");
    answer(fd, 200, "ok");
}

static void serve(int fd)
{
    char request[16384];
    size_t used = 0;
    char *body = NULL;
    size_t content_length = 0;
    bool streamed = false;
    /* the head, then as much body as it announces; an upload's body is streamed to its file instead */
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
            if (strncmp(request, "PUT ", 4) == 0) {
                streamed = true;
                break;
            }
        }
        if (body && (size_t)(request + used - body) >= content_length)
            break;
    }
    request[used] = 0;
    buf_t page = {0};
    char code[8];
    char path[300] = "";
    sscanf(request, "%*s %299s", path);
    const bool post = strncmp(request, "POST ", 5) == 0;
    pthread_mutex_lock(&g_lock);
    const bool music = g_music;
    pthread_mutex_unlock(&g_lock);
    if (!session_open(code, sizeof(code))) {
        page_closed(&page);
        respond(fd, 404, &page);
    } else if (streamed) {
        const char *query = strchr(path, '?');
        const char *encoding = header_value(request, "Transfer-Encoding");
        const bool chunked = encoding != NULL && strstr(encoding, "chunked") != NULL;
        if (music && strncmp(path, "/upload?", 8) == 0 && body)
            upload(fd, code, query + 1, body, (size_t)(request + used - body), content_length, chunked);
        else
            answer(fd, 404, "Nothing to send here.");
    } else if (!post) {
        /* the QR code's address carries the code: /1234 */
        const bool with_code = strcmp(path + 1, code) == 0;
        if (strcmp(path, "/favicon.ico") == 0) {
            respond(fd, 404, &page);
        } else {
            if (music)
                page_music(&page, with_code ? code : NULL);
            else
                page_form(&page, with_code ? code : NULL, NULL);
            respond(fd, 200, &page);
        }
    } else if (music) {
        answer(fd, 404, "Nothing to send here.");
    } else {
        char sent_code[16], key[96];
        form_value(body ? body : "", "code", sent_code, sizeof(sent_code));
        form_value(body ? body : "", "key", key, sizeof(key));
        if (strcmp(sent_code, code) != 0) {
            wrong_code();
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

/* The listening socket on the first free of three ports from g_port; -1 for none */
static int open_listener(void)
{
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        return -1;
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    /* a helper that crashed is stopped, not ended, and keeps its port: the next of three */
    for (int i = 0; i < 3; i++) {
        address.sin_port = htons((uint16_t)(g_port + 10 * i));
        if (bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(server, 4) == 0) {
            g_port += 10 * i;
            return server;
        }
        log_line("phone: port %d: %s", g_port + 10 * i, strerror(errno));
    }
    close(server);
    return -1;
}

static void *listen_thread(void *argument)
{
    const int server = (int)(intptr_t)argument;
    log_line("phone: the phone page is open on port %d", g_port);
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
    log_line("phone: the phone page has closed");
    return NULL;
}

bool phone_start(const char *code, bool music, char *err, size_t errlen)
{
    if (strlen(code) != 4) {
        snprintf(err, errlen, "the code is four digits");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    const bool start = !g_running;
    if (start) {
        g_port = PHONE_PORT;
#ifdef ARTSWAP_HOST
        if (getenv("ARTSWAP_PHONE_PORT"))
            g_port = atoi(getenv("ARTSWAP_PHONE_PORT"));
#endif
    }
    pthread_mutex_unlock(&g_lock);
    int server = -1;
    if (start && (server = open_listener()) < 0) {
        snprintf(err, errlen, "the key page could not open: no port free");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    copy_str(g_code, sizeof(g_code), code);
    g_music = music;
    g_until = now_ms() + SESSION_MS;
    g_wrong = 0;
    if (start)
        g_running = true;
    pthread_mutex_unlock(&g_lock);
    if (start) {
        /* a stack as the helper's other threads have: the console's default is small */
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        const int rc = pthread_create(&thread, &attr, listen_thread, (void *)(intptr_t)server);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            close(server);
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
