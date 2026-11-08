/*
 * Art Swap helper - the home screen's layout (layout.h).
 *
 * The person's changes are kept in /data/artswap/tiles.txt until Apply changes (home.c) writes them, just before the
 * home screen restarts, all in one transaction checked before it is committed, after both databases are copied
 * aside (tiles_backup). Once written, the databases and Art Swap's own tables in them (tiles.c) hold the layout, and
 * their triggers keep it through restarts, rest mode and installs; tiles.txt is emptied.
 *
 * tiles.txt, a line each, tab separated:
 *   move <id> <games|media>             a tile on that tab, on every profile
 *   hide <user> <id> <1|0>              hidden on that profile, or brought back
 *   show <id> <1|0>                     one the system hides, shown on every profile, or hidden again
 *   row <user> <games|media> <id>[*]... a row's order; * pins the tile at its place
 *   tabshifter                          TabShifter's tile removed
 *   reset                               everything Art Swap changed in the databases removed
 *   normalize                           what Art Swap 1.4 wrote put right (taken over from its layout.txt)
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "layout.h"
#include "appdb.h"
#include "tiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WISHES_MAX 512
#define ROWS_MAX 32

typedef struct {
    char id[16];
    int tab;
} move_t;

typedef struct {
    char user[16], id[16];
    bool hide;
} hide_t;

typedef struct {
    char id[16];
    bool show;
} show_t;

typedef struct {
    char user[16];
    int tab;
    int n;
    char ids[TILES_MAX][16];
    bool pins[TILES_MAX];
} row_wish_t;

typedef struct {
    move_t moves[WISHES_MAX];
    int nmoves;
    hide_t hides[WISHES_MAX];
    int nhides;
    show_t shows[64];
    int nshows;
    row_wish_t rows[ROWS_MAX];
    int nrows;
    bool tabshifter, reset, normalize;
} state_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* tiles.txt */

static bool plain_id(const char *id)
{
    const size_t len = strlen(id);
    if (len == 0 || len > 15)
        return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' ||
              *p == '-'))
            return false;
    return true;
}

static int tab_of(const char *where)
{
    return strcmp(where, "games") == 0 ? GAMES_TAB : strcmp(where, "media") == 0 ? MEDIA_TAB : 0;
}

static const char *tab_name(int tab)
{
    return tab == GAMES_TAB ? "games" : tab == MEDIA_TAB ? "media" : "none";
}

/* ---- tiles.txt, g_lock held ---- */

static void state_load(state_t *s)
{
    memset(s, 0, sizeof(*s));
    char path[512];
    buf_t text = {0};
    if (!file_read(data_path("tiles.txt", path, sizeof(path)), &text) || text.len == 0 || !buf_append(&text, "", 1)) {
        buf_free(&text);
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *rest = NULL;
        const char *what = strtok_r(line, "\t", &rest);
        const char *a = what ? strtok_r(NULL, "\t", &rest) : NULL;
        const char *b = a ? strtok_r(NULL, "\t", &rest) : NULL;
        if (what == NULL)
            continue;
        if (strcmp(what, "move") == 0 && a && b && plain_id(a) && tab_of(b) && s->nmoves < WISHES_MAX) {
            copy_str(s->moves[s->nmoves].id, 16, a);
            s->moves[s->nmoves++].tab = tab_of(b);
        } else if (strcmp(what, "hide") == 0 && a && b && plain_id(b) && s->nhides < WISHES_MAX) {
            const char *c = strtok_r(NULL, "\t", &rest);
            hide_t *h = &s->hides[s->nhides++];
            copy_str(h->user, sizeof(h->user), a);
            copy_str(h->id, sizeof(h->id), b);
            h->hide = c && strcmp(c, "1") == 0;
        } else if (strcmp(what, "show") == 0 && a && b && plain_id(a) && s->nshows < 64) {
            copy_str(s->shows[s->nshows].id, 16, a);
            s->shows[s->nshows++].show = strcmp(b, "1") == 0;
        } else if (strcmp(what, "row") == 0 && a && b && tab_of(b) && s->nrows < ROWS_MAX) {
            row_wish_t *r = &s->rows[s->nrows++];
            memset(r, 0, sizeof(*r));
            copy_str(r->user, sizeof(r->user), a);
            r->tab = tab_of(b);
            for (const char *id; (id = strtok_r(NULL, "\t", &rest)) != NULL && r->n < TILES_MAX;) {
                char plain[16];
                copy_str(plain, sizeof(plain), id);
                const size_t len = strlen(plain);
                const bool pin = len > 0 && plain[len - 1] == '*';
                if (pin)
                    plain[len - 1] = 0;
                if (plain_id(plain)) {
                    copy_str(r->ids[r->n], 16, plain);
                    r->pins[r->n++] = pin;
                }
            }
        } else if (strcmp(what, "tabshifter") == 0) {
            s->tabshifter = true;
        } else if (strcmp(what, "reset") == 0) {
            s->reset = true;
        } else if (strcmp(what, "normalize") == 0) {
            s->normalize = true;
        }
    }
    buf_free(&text);
}

static bool state_save(const state_t *s)
{
    char path[512];
    buf_t text = {0};
    buf_append(&text, "", 0);
    for (int i = 0; i < s->nmoves; i++)
        buf_printf(&text, "move\t%s\t%s\n", s->moves[i].id, tab_name(s->moves[i].tab));
    for (int i = 0; i < s->nhides; i++)
        buf_printf(&text, "hide\t%s\t%s\t%d\n", s->hides[i].user, s->hides[i].id, s->hides[i].hide);
    for (int i = 0; i < s->nshows; i++)
        buf_printf(&text, "show\t%s\t%d\n", s->shows[i].id, s->shows[i].show);
    for (int i = 0; i < s->nrows; i++) {
        const row_wish_t *r = &s->rows[i];
        buf_printf(&text, "row\t%s\t%s", r->user, tab_name(r->tab));
        for (int k = 0; k < r->n; k++)
            buf_printf(&text, "\t%s%s", r->ids[k], r->pins[k] ? "*" : "");
        buf_printf(&text, "\n");
    }
    if (s->tabshifter)
        buf_printf(&text, "tabshifter\n");
    if (s->reset)
        buf_printf(&text, "reset\n");
    if (s->normalize)
        buf_printf(&text, "normalize\n");
    const bool ok = file_write_replace(data_path("tiles.txt", path, sizeof(path)), text.data ? (const char *)text.data
                                                                                              : "",
                                       text.len, NULL);
    buf_free(&text);
    return ok;
}

static bool waiting(const state_t *s)
{
    return s->nmoves || s->nhides || s->nshows || s->nrows || s->tabshifter || s->reset || s->normalize;
}

static const move_t *find_move(const state_t *s, const char *id)
{
    for (int i = 0; i < s->nmoves; i++)
        if (strcmp(s->moves[i].id, id) == 0)
            return &s->moves[i];
    return NULL;
}

static const hide_t *find_hide(const state_t *s, const char *user, const char *id)
{
    for (int i = 0; i < s->nhides; i++)
        if (strcmp(s->hides[i].id, id) == 0 && strcmp(s->hides[i].user, user) == 0)
            return &s->hides[i];
    return NULL;
}

static const show_t *find_show(const state_t *s, const char *id)
{
    for (int i = 0; i < s->nshows; i++)
        if (strcmp(s->shows[i].id, id) == 0)
            return &s->shows[i];
    return NULL;
}

static row_wish_t *find_row(state_t *s, const char *user, int tab)
{
    for (int i = 0; i < s->nrows; i++)
        if (s->rows[i].tab == tab && strcmp(s->rows[i].user, user) == 0)
            return &s->rows[i];
    return NULL;
}

/* ---- the home screen as it will be ---- */

typedef struct {
    sqlite3 *db;
    char user[16];
    tile_t *tiles;
    int n;
    bool conflict;
} home_t;

static void home_close(home_t *h)
{
    if (h->db)
        sqlite3_close(h->db);
    free(h->tiles);
    memset(h, 0, sizeof(*h));
}

/* The database and `user`'s tiles (user empty: the first profile) */
static bool home_open(const char *user, home_t *h, char *err, size_t errlen)
{
    memset(h, 0, sizeof(*h));
    h->db = appdb_open(SQLITE_OPEN_READONLY, err, errlen);
    if (h->db == NULL)
        return false;
    char users[TILES_USERS][16];
    const int count = tiles_users(h->db, users, TILES_USERS);
    if (count <= 0) {
        snprintf(err, errlen, "This console's home-screen database keeps its tiles in a way Art Swap does not know");
        home_close(h);
        return false;
    }
    copy_str(h->user, sizeof(h->user), user && *user ? user : users[0]);
    if (!tiles_user_known(h->db, h->user)) {
        snprintf(err, errlen, "no such profile");
        home_close(h);
        return false;
    }
    h->conflict = tiles_conflict(h->db);
    h->tiles = calloc(TILES_MAX, sizeof(tile_t));
    if (h->tiles == NULL || (h->n = tiles_read(h->db, h->user, h->tiles, TILES_MAX, err, errlen)) < 0) {
        if (h->tiles == NULL)
            snprintf(err, errlen, "out of memory");
        home_close(h);
        return false;
    }
    return true;
}

static tile_t *home_tile(home_t *h, const char *id)
{
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->tiles[i].id, id) == 0)
            return &h->tiles[i];
    return NULL;
}

/* A tile as the person wants it on this profile */
typedef struct {
    int tab;
    bool hidden;
    int pin;
    long long order;
} wish_t;

static wish_t wished(const state_t *s, const char *user, const tile_t *t)
{
    wish_t w = {t->tab, !t->visible, t->pin, t->index};
    const move_t *m = find_move(s, t->id);
    if (m)
        w.tab = m->tab;
    const hide_t *h = find_hide(s, user, t->id);
    if (h)
        w.hidden = h->hide;
    const show_t *sh = find_show(s, t->id);
    if (sh)
        w.hidden = !sh->show;
    if (m || w.hidden)
        w.pin = -1;
    return w;
}

/* "T id tab now hidden now_hidden kind caps pin name": tab and hidden as wished, now as the home screen has it */
static void tile_line(buf_t *out, const state_t *s, const char *user, const tile_t *t, const wish_t *w)
{
    char caps[8], *c = caps;
    if (t->caps & TILE_MOVE)
        *c++ = 'm';
    if (t->caps & TILE_HIDE)
        *c++ = 'h';
    if (t->caps & TILE_SHOW)
        *c++ = 's';
    if (t->caps & TILE_ORDER)
        *c++ = 'o';
    if (t->shown)
        *c++ = 'S';
    if (c == caps)
        *c++ = '-';
    *c = 0;
    char pin[16] = "-";
    if (w->pin >= 0)
        snprintf(pin, sizeof(pin), "%d", w->pin);
    buf_printf(out, "T\t%s\t%s\t%s\t%d\t%d\t%s\t%s\t%s\t", t->id, tab_name(w->tab), tab_name(t->tab), w->hidden,
               !t->visible, t->system ? "system" : "game", caps, pin);
    buf_field(out, t->name);
    buf_printf(out, "\n");
    (void)s;
    (void)user;
}

typedef struct {
    const tile_t *t;
    wish_t w;
} listed_t;

static int by_wish(const void *a, const void *b)
{
    const listed_t *x = a, *y = b;
    const int gx = x->w.hidden || !x->w.tab ? 2 : x->w.tab == GAMES_TAB ? 0 : 1;
    const int gy = y->w.hidden || !y->w.tab ? 2 : y->w.tab == GAMES_TAB ? 0 : 1;
    if (gx != gy)
        return gx - gy;
    if (gx == 2)
        return strcmp(x->t->name, y->t->name);
    if (x->t->priority != y->t->priority)
        return x->t->priority < y->t->priority ? -1 : 1;
    if (x->w.order != y->w.order)
        return x->w.order > y->w.order ? -1 : 1;
    return strcmp(x->t->id, y->t->id);
}

/* ---- requests ---- */

bool layout_profiles(buf_t *out, char *err, size_t errlen)
{
    sqlite3 *db = appdb_open(SQLITE_OPEN_READONLY, err, errlen);
    if (db == NULL)
        return false;
    tiles_profile_t profiles[TILES_USERS];
    const int n = tiles_profiles(db, profiles, TILES_USERS);
    sqlite3_close(db);
    if (n < 0) {
        snprintf(err, errlen, "The profiles could not be read");
        return false;
    }
    for (int i = 0; i < n; i++) {
        buf_printf(out, "U\t%s\t", profiles[i].user);
        buf_field(out, profiles[i].name);
        buf_printf(out, "\n");
    }
    return true;
}

bool layout_list(const char *user, buf_t *out, char *err, size_t errlen)
{
    home_t h;
    if (!home_open(user, &h, err, errlen))
        return false;
    state_t *s = malloc(sizeof(*s));
    listed_t *list = calloc(TILES_MAX, sizeof(listed_t));
    if (s == NULL || list == NULL) {
        free(s);
        free(list);
        home_close(&h);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    pthread_mutex_unlock(&g_lock);
    buf_printf(out, "profile\t%s\n", h.user);
    if (h.conflict)
        buf_printf(out, "conflict\tlineup\n");
    /* TabShifter's tile, when it left one */
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(h.db, "SELECT 1 FROM tbl_contentinfo WHERE titleId = 'TBSH00001'", -1, &st, NULL) ==
        SQLITE_OK)
        found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    buf_printf(out, "tabshifter\t%s\n", s->tabshifter ? "queued" : found ? "found" : "none");
    buf_printf(out, "reset\t%s\n", s->reset ? "queued" : "none");
    /* the rows wished: the tiles named in the order wished, the others after, the most recent first */
    for (int i = 0; i < h.n; i++) {
        list[i].t = &h.tiles[i];
        list[i].w = wished(s, h.user, &h.tiles[i]);
    }
    for (int tab = GAMES_TAB; tab <= MEDIA_TAB; tab += MEDIA_TAB - GAMES_TAB) {
        const row_wish_t *r = find_row(s, h.user, tab);
        for (int k = 0; r && k < r->n; k++)
            for (int i = 0; i < h.n; i++)
                if (list[i].w.tab == tab && !list[i].w.hidden && strcmp(list[i].t->id, r->ids[k]) == 0) {
                    list[i].w.order = 4000000000000000000LL - k;
                    list[i].w.pin = r->pins[k] ? k : -1;
                }
    }
    qsort(list, (size_t)h.n, sizeof(list[0]), by_wish);
    /* a pinned tile's place counts among the row's tiles only */
    int place[2] = {0, 0};
    for (int i = 0; i < h.n; i++) {
        listed_t *l = &list[i];
        if (!l->w.hidden && (l->t->caps & TILE_ORDER) && l->w.tab) {
            const int at = place[l->w.tab == MEDIA_TAB]++;
            if (l->w.pin >= 0)
                l->w.pin = at;
        }
        tile_line(out, s, h.user, l->t, &l->w);
    }
    free(list);
    free(s);
    home_close(&h);
    return true;
}

/* The tile, checked it can do `cap`, on `user`'s home; false with a sentence in err */
static bool check_tile(const char *user, const char *id, unsigned cap, home_t *h, tile_t **t, char *err,
                       size_t errlen)
{
    if (!plain_id(id)) {
        snprintf(err, errlen, "no such tile");
        return false;
    }
    if (!home_open(user, h, err, errlen))
        return false;
    if (h->conflict) {
        snprintf(err, errlen, TILES_CONFLICT);
        home_close(h);
        return false;
    }
    *t = home_tile(h, id);
    if (*t == NULL) {
        snprintf(err, errlen, "no such tile");
        home_close(h);
        return false;
    }
    if (!((*t)->caps & cap)) {
        snprintf(err, errlen, cap == TILE_MOVE   ? "%s does not move between the tabs"
                              : cap == TILE_HIDE ? "%s does not hide"
                              : cap == TILE_SHOW ? "%s does not show"
                                                 : "%s is not ordered in a row",
                 (*t)->name);
        home_close(h);
        return false;
    }
    return true;
}

bool layout_tab(const char *user, const char *id, const char *where, char *err, size_t errlen)
{
    const int tab = tab_of(where);
    if (!tab) {
        snprintf(err, errlen, "a tile goes on games or media");
        return false;
    }
    home_t h;
    tile_t *t = NULL;
    if (!check_tile(user, id, TILE_MOVE, &h, &t, err, errlen))
        return false;
    const int now = t->tab;
    char name[256];
    copy_str(name, sizeof(name), t->name);
    home_close(&h);
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    int at = -1;
    for (int i = 0; i < s->nmoves && at < 0; i++)
        if (strcmp(s->moves[i].id, id) == 0)
            at = i;
    if (tab == now) {
        if (at >= 0)
            s->moves[at] = s->moves[--s->nmoves];
    } else if (at >= 0) {
        s->moves[at].tab = tab;
    } else if (s->nmoves < WISHES_MAX) {
        copy_str(s->moves[s->nmoves].id, 16, id);
        s->moves[s->nmoves++].tab = tab;
    }
    const bool ok = state_save(s);
    pthread_mutex_unlock(&g_lock);
    free(s);
    if (!ok)
        snprintf(err, errlen, "The choice could not be kept");
    else
        log_line("layout: %s (%s) wished on %s", id, name, where);
    return ok;
}

bool layout_hide(const char *user, const char *id, bool hide, char *err, size_t errlen)
{
    home_t h;
    tile_t *t = NULL;
    if (!check_tile(user, id, TILE_HIDE, &h, &t, err, errlen))
        return false;
    const bool now = !t->visible;
    char profile[16];
    copy_str(profile, sizeof(profile), h.user);
    home_close(&h);
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    int at = -1;
    for (int i = 0; i < s->nhides && at < 0; i++)
        if (strcmp(s->hides[i].id, id) == 0 && strcmp(s->hides[i].user, profile) == 0)
            at = i;
    if (hide == now) {
        if (at >= 0)
            s->hides[at] = s->hides[--s->nhides];
    } else if (at >= 0) {
        s->hides[at].hide = hide;
    } else if (s->nhides < WISHES_MAX) {
        hide_t *w = &s->hides[s->nhides++];
        copy_str(w->user, sizeof(w->user), profile);
        copy_str(w->id, sizeof(w->id), id);
        w->hide = hide;
    }
    const bool ok = state_save(s);
    pthread_mutex_unlock(&g_lock);
    free(s);
    if (!ok)
        snprintf(err, errlen, "The choice could not be kept");
    return ok;
}

bool layout_show(const char *user, const char *id, bool show, char *err, size_t errlen)
{
    home_t h;
    tile_t *t = NULL;
    if (!check_tile(user, id, TILE_SHOW, &h, &t, err, errlen))
        return false;
    const bool now = t->shown;
    home_close(&h);
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    int at = -1;
    for (int i = 0; i < s->nshows && at < 0; i++)
        if (strcmp(s->shows[i].id, id) == 0)
            at = i;
    if (show == now) {
        if (at >= 0)
            s->shows[at] = s->shows[--s->nshows];
    } else if (at >= 0) {
        s->shows[at].show = show;
    } else if (s->nshows < 64) {
        copy_str(s->shows[s->nshows].id, 16, id);
        s->shows[s->nshows++].show = show;
    }
    const bool ok = state_save(s);
    pthread_mutex_unlock(&g_lock);
    free(s);
    if (!ok)
        snprintf(err, errlen, "The choice could not be kept");
    return ok;
}

bool layout_row(const char *user, const char *where, char *const *ids, int nids, char *err, size_t errlen)
{
    const int tab = tab_of(where);
    if (!tab) {
        snprintf(err, errlen, "a row is games or media");
        return false;
    }
    home_t h;
    if (!home_open(user, &h, err, errlen))
        return false;
    if (h.conflict) {
        snprintf(err, errlen, TILES_CONFLICT);
        home_close(&h);
        return false;
    }
    char profile[16];
    copy_str(profile, sizeof(profile), h.user);
    /* the tiles of the row now: the named ones come first, the others after them */
    int on_row = 0;
    for (int i = 0; i < h.n; i++)
        on_row += h.tiles[i].visible && h.tiles[i].tab == tab && (h.tiles[i].caps & TILE_ORDER);
    home_close(&h);
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    row_wish_t *r = find_row(s, profile, tab);
    if (r == NULL && s->nrows < ROWS_MAX)
        r = &s->rows[s->nrows++];
    bool ok = r != NULL;
    if (ok) {
        memset(r, 0, sizeof(*r));
        copy_str(r->user, sizeof(r->user), profile);
        r->tab = tab;
        int pins = 0;
        for (int i = 0; i < nids && r->n < TILES_MAX; i++) {
            char plain[16];
            copy_str(plain, sizeof(plain), ids[i]);
            const size_t len = strlen(plain);
            const bool pin = len > 0 && plain[len - 1] == '*';
            if (pin)
                plain[len - 1] = 0;
            if (!plain_id(plain))
                continue;
            copy_str(r->ids[r->n], 16, plain);
            pins += pin;
            r->pins[r->n++] = pin;
        }
        /* the home screen shows the first places of a row: one of them stays free for the app opened last */
        const int count = on_row > r->n ? on_row : r->n;
        const int home = count < (tab == MEDIA_TAB ? 11 : 12) ? count : (tab == MEDIA_TAB ? 11 : 12);
        int first = 0;
        for (int i = 0; i < r->n && i < home; i++)
            first += r->pins[i];
        if (home > 0 && first >= home) {
            snprintf(err, errlen, "Leave one of the first %d places unpinned, for the app you open last", home);
            ok = false;
            state_load(s);
        }
    }
    ok = ok && state_save(s);
    pthread_mutex_unlock(&g_lock);
    free(s);
    if (!ok && !*err)
        snprintf(err, errlen, "The choice could not be kept");
    return ok;
}

bool layout_queue(const char *what, bool on, char *err, size_t errlen)
{
    const bool tabshifter = strcmp(what, "tabshifter") == 0;
    if (!tabshifter && strcmp(what, "reset") != 0) {
        snprintf(err, errlen, "tabshifter or reset");
        return false;
    }
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    if (tabshifter)
        s->tabshifter = on;
    else {
        /* removing everything: what waits to be changed goes too */
        if (on) {
            const bool keep = s->tabshifter, normalize = s->normalize;
            memset(s, 0, sizeof(*s));
            s->tabshifter = keep;
            s->normalize = normalize;
        }
        s->reset = on;
    }
    const bool ok = state_save(s);
    pthread_mutex_unlock(&g_lock);
    free(s);
    if (!ok)
        snprintf(err, errlen, "The choice could not be kept");
    return ok;
}

bool layout_waiting(void)
{
    state_t *s = malloc(sizeof(*s));
    if (s == NULL)
        return false;
    pthread_mutex_lock(&g_lock);
    state_load(s);
    pthread_mutex_unlock(&g_lock);
    const bool any = waiting(s);
    free(s);
    return any;
}

void layout_pending(buf_t *out)
{
    state_t *s = malloc(sizeof(*s));
    if (s == NULL)
        return;
    pthread_mutex_lock(&g_lock);
    state_load(s);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < s->nmoves; i++)
        buf_printf(out, "P\t%s\tlayout\n", s->moves[i].id);
    for (int i = 0; i < s->nhides; i++) {
        bool seen = find_move(s, s->hides[i].id) != NULL;
        for (int k = 0; k < i && !seen; k++)
            seen = strcmp(s->hides[k].id, s->hides[i].id) == 0;
        if (!seen)
            buf_printf(out, "P\t%s\tlayout\n", s->hides[i].id);
    }
    for (int i = 0; i < s->nshows; i++)
        if (find_move(s, s->shows[i].id) == NULL)
            buf_printf(out, "P\t%s\tlayout\n", s->shows[i].id);
    if (s->nrows)
        buf_printf(out, "P\torder\tlayout\n");
    if (s->tabshifter)
        buf_printf(out, "P\ttabshifter\tlayout\n");
    if (s->reset)
        buf_printf(out, "P\treset\tlayout\n");
    free(s);
}

/* ---- Apply changes ---- */

/* One change in a savepoint of its own: one that cannot be made is rolled back, logged and left out, and the
 * others are still written. False only when the database itself failed. */
typedef int (*change_fn)(sqlite3 *db, const void *arg, char *msg, size_t msglen);

static bool change(sqlite3 *db, const char *what, change_fn fn, const void *arg, int *written)
{
    char msg[300] = "";
    if (sqlite3_exec(db, "SAVEPOINT artswap_change", NULL, NULL, NULL) != SQLITE_OK)
        return false;
    const int rc = fn(db, arg, msg, sizeof(msg));
    if (rc == 0) {
        (*written)++;
        return sqlite3_exec(db, "RELEASE artswap_change", NULL, NULL, NULL) == SQLITE_OK;
    }
    log_line("layout: %s %s: %s", what, rc > 0 ? "left out" : "failed", msg);
    return sqlite3_exec(db, "ROLLBACK TO artswap_change", NULL, NULL, NULL) == SQLITE_OK &&
           sqlite3_exec(db, "RELEASE artswap_change", NULL, NULL, NULL) == SQLITE_OK;
}

static int do_move(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    const move_t *m = arg;
    return tiles_move(db, m->id, m->tab, msg, msglen);
}

static int do_hide(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    const hide_t *h = arg;
    return tiles_hide(db, h->user, h->id, h->hide, msg, msglen);
}

static int do_show(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    const show_t *s = arg;
    return tiles_show(db, s->id, s->show, msg, msglen);
}

static int do_row(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    const row_wish_t *r = arg;
    return tiles_order(db, r->user, r->tab, (char(*)[16])r->ids, r->pins, r->n, msg, msglen);
}

static int g_removed;

static int do_tabshifter(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    (void)arg;
    return tiles_tabshifter(db, &g_removed, msg, msglen);
}

static int do_reset(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    (void)arg;
    return tiles_reset(db, msg, msglen);
}

static int do_adopt(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    (void)arg;
    if (tiles_adopt(db, true) < 0)
        return snprintf(msg, msglen, "%s", sqlite3_errmsg(db)), -1;
    return 0;
}

static int do_normalize(sqlite3 *db, const void *arg, char *msg, size_t msglen)
{
    int changed = 0;
    const int rc = tiles_normalize(db, &changed, msg, msglen);
    if (rc == 0)
        log_line("layout: what Art Swap 1.4 wrote put right (%d values)", changed);
    (void)arg;
    return rc;
}

bool layout_arrange(char *err, size_t errlen)
{
    state_t *s = malloc(sizeof(*s));
    if (s == NULL) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    if (!waiting(s)) {
        pthread_mutex_unlock(&g_lock);
        free(s);
        return true;
    }
    sqlite3 *db = appdb_open(SQLITE_OPEN_READWRITE, err, errlen);
    bool ok = db != NULL;
    if (ok && tiles_conflict(db)) {
        snprintf(err, errlen, TILES_CONFLICT);
        ok = false;
    }
    ok = ok && tiles_backup(db, err, errlen);
    if (ok && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database is busy (%s)", sqlite3_errmsg(db));
        ok = false;
    }
    const bool began = ok;
    int written = 0;
    g_removed = 0;
    /* first what was there before (1.4's values, everything removed), then the tiles, then the rows they make */
    if (ok && s->normalize)
        ok = change(db, "Art Swap 1.4's layout", do_normalize, NULL, &written);
    if (ok && s->reset)
        ok = change(db, "removing everything", do_reset, NULL, &written);
    if (ok && s->tabshifter)
        ok = change(db, "TabShifter's tile", do_tabshifter, NULL, &written);
    for (int i = 0; ok && i < s->nshows; i++)
        ok = change(db, s->shows[i].id, do_show, &s->shows[i], &written);
    for (int i = 0; ok && i < s->nmoves; i++)
        ok = change(db, s->moves[i].id, do_move, &s->moves[i], &written);
    /* apps another tool moved, kept where they are as Art Swap's own moves: else an install puts them back */
    if (ok && !s->reset && tiles_adopt(db, false) > 0)
        ok = change(db, "apps other tools moved", do_adopt, NULL, &written);
    for (int i = 0; ok && i < s->nhides; i++)
        ok = change(db, s->hides[i].id, do_hide, &s->hides[i], &written);
    for (int i = 0; ok && i < s->nrows; i++)
        ok = change(db, "a row's order", do_row, &s->rows[i], &written);
    if (!ok && began && !*err)
        snprintf(err, errlen, "The home screen's database could not be changed (%s)", sqlite3_errmsg(db));
    /* checked before it is kept: a database the home screen cannot read is never left behind */
    if (ok) {
        sqlite3_stmt *st = NULL;
        const char *verdict = NULL;
        if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            verdict = (const char *)sqlite3_column_text(st, 0);
        ok = verdict != NULL && strcmp(verdict, "ok") == 0;
        if (!ok)
            snprintf(err, errlen, "The home screen's database did not pass its check after the change (%s), so nothing "
                                  "was changed",
                     verdict ? verdict : sqlite3_errmsg(db));
        sqlite3_finalize(st);
    }
    if (ok && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "The home screen's database could not be changed (%s)", sqlite3_errmsg(db));
        ok = false;
    }
    if (!ok && began)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (ok) {
        if (s->tabshifter)
            tiles_tabshifter_folders();
        log_line("layout: %d change(s) written%s", written, s->tabshifter && g_removed ? ", TabShifter's tile removed" : "");
        memset(s, 0, sizeof(*s));
        state_save(s);
    } else {
        log_line("layout: not arranged: %s", err);
    }
    pthread_mutex_unlock(&g_lock);
    if (db != NULL)
        sqlite3_close(db);
    free(s);
    return ok;
}

/* ---- Art Swap 1.4's layout.txt ----
 *
 * 1.4 kept every wish and wrote it again at each Apply changes, the same on every profile, and a locked order far
 * in the future. Its wishes become changes waiting for the next Apply changes: a tile's tab, hidden on every
 * profile, a system tile it showed, and the locked order as each profile's row (pin the tiles to keep them); then
 * what it wrote is put right (normalize). The file is kept aside as layout.txt.1.4. */

static void take_over_1_4(void)
{
    char path[512], aside[512];
    data_path("layout.txt", path, sizeof(path));
    data_path("layout.txt.1.4", aside, sizeof(aside));
    buf_t text = {0};
    if (!file_exists(path) || !file_read(path, &text) || !buf_append(&text, "", 1)) {
        buf_free(&text);
        return;
    }
    char err[300];
    sqlite3 *db = appdb_open(SQLITE_OPEN_READONLY, err, sizeof(err));
    char users[TILES_USERS][16];
    const int count = db ? tiles_users(db, users, TILES_USERS) : -1;
    state_t *s = malloc(sizeof(*s));
    if (count <= 0 || s == NULL) {
        if (db)
            sqlite3_close(db);
        free(s);
        buf_free(&text);
        return;
    }
    pthread_mutex_lock(&g_lock);
    state_load(s);
    bool locked = false;
    static char seq[TILES_MAX][16];
    int nseq = 0;
    char *save = NULL;
    for (char *line = strtok_r((char *)text.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *rest = NULL;
        const char *what = strtok_r(line, "\t", &rest);
        if (what == NULL)
            continue;
        if (strcmp(what, "place") == 0) {
            const char *id = strtok_r(NULL, "\t", &rest), *where = strtok_r(NULL, "\t", &rest);
            if (!id || !where || !plain_id(id))
                continue;
            if (strcmp(where, "hidden") == 0) {
                for (int u = 0; u < count && s->nhides < WISHES_MAX; u++) {
                    hide_t *h = &s->hides[s->nhides++];
                    copy_str(h->user, sizeof(h->user), users[u]);
                    copy_str(h->id, sizeof(h->id), id);
                    h->hide = true;
                }
            } else if (tab_of(where) && s->nmoves < WISHES_MAX) {
                copy_str(s->moves[s->nmoves].id, 16, id);
                s->moves[s->nmoves++].tab = tab_of(where);
            }
        } else if (strcmp(what, "order") == 0) {
            const char *mode = strtok_r(NULL, "\t", &rest);
            locked = mode && strcmp(mode, "locked") == 0;
        } else if (strcmp(what, "seq") == 0) {
            for (const char *id; (id = strtok_r(NULL, "\t", &rest)) != NULL && nseq < TILES_MAX;)
                if (plain_id(id))
                    copy_str(seq[nseq++], 16, id);
        }
    }
    for (int u = 0; locked && nseq > 0 && u < count && s->nrows < ROWS_MAX; u++) {
        row_wish_t *r = &s->rows[s->nrows++];
        memset(r, 0, sizeof(*r));
        copy_str(r->user, sizeof(r->user), users[u]);
        r->tab = GAMES_TAB;
        for (int i = 0; i < nseq; i++)
            copy_str(r->ids[r->n++], 16, seq[i]);
    }
    s->normalize = true;
    const bool ok = state_save(s) && file_rename(path, aside);
    pthread_mutex_unlock(&g_lock);
    log_line("layout: Art Swap 1.4's layout.txt %s (%d moves, %d hidden, %s)", ok ? "taken over" : "not taken over",
             s->nmoves, s->nhides, locked ? "the order locked" : "no order");
    sqlite3_close(db);
    free(s);
    buf_free(&text);
}

void layout_startup(void)
{
    take_over_1_4();
    pthread_mutex_lock(&g_apply_lock);
    tiles_startup();
    pthread_mutex_unlock(&g_apply_lock);
}
