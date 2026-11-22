/* Tetris for the Wii U GamePad (libwup). Single-player modes and 2-player Wi-Fi versus.
 *
 * Pad: Left/Right move, Down soft drop, Up or ZR hard drop, A or R rotate, B or Y rotate back,
 *      X, L or ZL hold, + pause / forfeit.  Menu: Up/Down, A start, B exit (or tap a row). */
#include "wup.h"
#include "armcpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "tetris.h"
#include "netplay.h"

WUP_APP("Tetris");

#define NET_PORT 28901
#define BG      RGB(14, 16, 28)
#define PANEL   RGB(26, 30, 50)
#define WELL    RGB(20, 23, 40)
#define GRID    RGB(30, 34, 56)
#define WHITE   RGB(235, 238, 250)
#define GREY    RGB(130, 138, 165)
#define GOLD    RGB(255, 205, 70)
#define ACCENT  RGB(70, 120, 255)

/* ---------------------------------------------------------------- input */
static unsigned g_b, g_p;                       /* buttons held, newly pressed */
static int g_tx, g_ty, g_touch, g_tap;          /* touch position, state, new tap */
static void poll(void) {
    unsigned b = wup_buttons(); g_p = b & ~g_b; g_b = b;
    int x = 0, y = 0, t = wup_touch(&x, &y);
    g_tap = t && !g_touch; g_touch = t; if (t) { g_tx = x; g_ty = y; }
}
static uint8_t pad_mask(void) {
    uint8_t m = 0;
    if (g_b & WUP_LEFT)  m |= IN_LEFT;
    if (g_b & WUP_RIGHT) m |= IN_RIGHT;
    if (g_b & WUP_DOWN)  m |= IN_SOFT;
    if (g_b & (WUP_UP | WUP_ZR)) m |= IN_HARD;
    if (g_b & (WUP_A | WUP_R))   m |= IN_CW;
    if (g_b & (WUP_B | WUP_Y))   m |= IN_CCW;
    if (g_b & (WUP_X | WUP_L | WUP_ZL)) m |= IN_HOLD;
    return m;
}

/* 60 Hz pacing: returns how many logic steps are due (catches up to 3, then drops the backlog) */
static uint32_t next_us;
static void frame_start(void) { next_us = wup_us(); }
static int frame_wait(void) {
    while ((int32_t)(wup_us() - next_us) < 0) wup_sleep(1);
    int n = 0;
    while ((int32_t)(wup_us() - next_us) >= 0 && n < 3) { next_us += 16667; n++; }
    if ((int32_t)(wup_us() - next_us) >= 0) next_us = wup_us() + 16667;
    return n;
}

/* ---------------------------------------------------------------- saved bests */
static struct { uint32_t magic, v[MODE_COUNT]; } sv;
#define SV_MAGIC 0x54425631u
static void sv_load(void) {
    memset(&sv, 0, sizeof sv);
    FILE *f = fopen("best.dat", "rb");
    if (f) { if (fread(&sv, sizeof sv, 1, f) != 1 || sv.magic != SV_MAGIC) memset(&sv, 0, sizeof sv); fclose(f); }
    sv.magic = SV_MAGIC;
}
static void sv_save(void) { FILE *f = fopen("best.dat", "wb"); if (f) { fwrite(&sv, sizeof sv, 1, f); fclose(f); } }

static void fmt_time(char *o, size_t n, uint32_t frames) {
    snprintf(o, n, "%u:%02u.%02u", (unsigned)(frames / 3600), (unsigned)(frames / 60 % 60), (unsigned)(frames % 60 * 100 / 60));
}

/* ---------------------------------------------------------------- flicker-free compositor
 * The LCD scans the framebuffer while we draw: clearing a panel and redrawing it on the live buffer shows
 * as flicker and tear lines. Each region is instead drawn into an off-screen buffer and copied in as one
 * quick pass (synced to vblank once per frame), and a region is skipped entirely when nothing in it changed. */
static px_t *g_bb;                              /* back buffer for one region at a time */
static size_t g_bb_px;
static int g_ox, g_oy, g_rx, g_ry, g_rw, g_rh, g_buf;
static int g_sync;                              /* 1 = next flush waits for vblank first */

#define gfx_fill(x, y, w, h, c)               gfx_fill((x) - g_ox, (y) - g_oy, w, h, c)
#define gfx_fill_a(x, y, w, h, c, a)          gfx_fill_a((x) - g_ox, (y) - g_oy, w, h, c, a)
#define gfx_rrect(x, y, w, h, r, c, a)        gfx_rrect((x) - g_ox, (y) - g_oy, w, h, r, c, a)
#define gfx_rrect_outline(x, y, w, h, r, t, c) gfx_rrect_outline((x) - g_ox, (y) - g_oy, w, h, r, t, c)
#define gfx_text(f, x, b, s, c)               gfx_text(f, (x) - g_ox, (b) - g_oy, s, c)
#define gfx_text_c(f, x, b, s, c)             gfx_text_c(f, (x) - g_ox, (b) - g_oy, s, c)

static void region_begin(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x; if (y + h > GFX_H) h = GFX_H - y;
    g_rx = x; g_ry = y; g_rw = w; g_rh = h;
    g_buf = g_bb && w > 0 && h > 0 && (size_t)w * h <= g_bb_px;
    if (g_buf) { g_ox = x; g_oy = y; gfx_target(g_bb, w, h); }
    else { g_ox = g_oy = 0; }
}
static void region_end(void) {
    if (!g_buf) { gfx_present(g_rx, g_ry, g_rw, g_rh); return; }
    gfx_target_screen(); g_ox = g_oy = 0; g_buf = 0;
    if (g_sync) { wup_vblank(); g_sync = 0; }
    px_t *fb = gfx_fb();
    for (int j = 0; j < g_rh; j++) {                    /* top to bottom, ahead of the beam; each row flushed at once */
        px_t *d = fb + (g_ry + j) * GFX_W + g_rx;
        gfx_copy(d, g_bb + j * g_rw, g_rw);
        cache_clean(d, (uint32_t)g_rw * 2);
    }
    gfx_present(g_rx, g_ry, 0, 0);                      /* runs the first-frame fade hook; cleans nothing */
}
static void bb_init(void) {
    g_bb_px = 250 * 454;                                /* the largest region: the board */
    g_bb = malloc(g_bb_px * sizeof(px_t));
    if (!g_bb) g_bb_px = 0;
}

/* a region is redrawn only when its signature changes */
enum { S_BOARD, S_HOLD, S_NEXT, S_STATS, S_POPUP, S_OPP, S_COUNT };
static uint32_t g_sig[S_COUNT]; static uint8_t g_sig_ok[S_COUNT];
static uint32_t fnv(uint32_t h, const void *p, size_t n) { const uint8_t *b = p; while (n--) h = (h ^ *b++) * 16777619u; return h; }
static uint32_t fnv_s(uint32_t h, const char *s) { return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "", 1); }
static int dirty(int slot, uint32_t h) {
    if (g_sig_ok[slot] && g_sig[slot] == h) return 0;
    g_sig[slot] = h; g_sig_ok[slot] = 1; return 1;
}
static void sig_reset(void) { memset(g_sig_ok, 0, sizeof g_sig_ok); g_sync = 1; }

/* ---------------------------------------------------------------- drawing */
static px_t PC[9], PCL[9], PCD[9];
static void init_colors(void) {
    static const uint8_t c[9][3] = { {0,0,0}, {0,200,230}, {240,200,0}, {160,70,210}, {70,200,80},
                                     {225,55,65}, {55,90,230}, {240,140,35}, {112,112,124} };
    for (int i = 0; i < 9; i++) {
        PC[i] = RGB(c[i][0], c[i][1], c[i][2]);
        PCL[i] = gfx_mix(PC[i], RGB(255, 255, 255), 90);
        PCD[i] = gfx_mix(PC[i], RGB(0, 0, 0), 120);
    }
}
static void clear(int x, int y, int w, int h) { gfx_fill(x, y, w, h, BG); }
static void cell(int x, int y, int cs, int v) {
    gfx_fill(x, y, cs, cs, PCD[v]);
    gfx_fill(x + 1, y + 1, cs - 2, cs - 2, PC[v]);
    gfx_fill(x + 1, y + 1, cs - 2, 2, PCL[v]);
}
static void mini(int cx, int cy, int cs, int type) {
    int c[4][2]; tt_piece_cells(type, 0, c);
    int x0 = 9, x1 = -9, y0 = 9, y1 = -9;
    for (int i = 0; i < 4; i++) {
        if (c[i][0] < x0) x0 = c[i][0]; if (c[i][0] > x1) x1 = c[i][0];
        if (c[i][1] < y0) y0 = c[i][1]; if (c[i][1] > y1) y1 = c[i][1];
    }
    int ox = cx - (x1 - x0 + 1) * cs / 2, oy = cy - (y1 - y0 + 1) * cs / 2;
    for (int i = 0; i < 4; i++) cell(ox + (c[i][0] - x0) * cs, oy + (c[i][1] - y0) * cs, cs, type + 1);
}
static void panel(int x, int y, int w, int h, const char *title) {
    clear(x - 2, y - 2, w + 4, h + 4);
    gfx_rrect(x, y, w, h, 10, PANEL, 256);
    if (title) gfx_text(&font_small, x + 14, y + 24, title, GREY);
}
static void overlay(int bx, int by, int cs, const char *l1, const char *l2, const font_t *f) {
    if (!l1) return;
    gfx_fill_a(bx, by, cs * TT_W, cs * TT_VIS, RGB(0, 0, 0), 170);
    gfx_text_c(f, bx + cs * TT_W / 2, by + cs * 9, l1, WHITE);
    if (l2) gfx_text_c(&font_body, bx + cs * TT_W / 2, by + cs * 11 + 6, l2, GREY);
}

typedef struct { int bx, by, cs; int hx, hy, hw, hh; int nx, ny, nw, nh, ncs; int sx, sy, sw, sh; int px, py, pw, ph; int vs; int ox, oy, ocs; } Layout;
static const Layout L_SOLO = { 317, 20, 22,  70, 30, 200, 90,  585, 30, 200, 330, 20,  70, 170, 200, 270,  70, 128, 200, 36, 0, 0, 0, 0 };
static const Layout L_VS   = { 250, 20, 22,  20, 30, 200, 90,  490, 30, 110, 330, 16,  20, 170, 210, 270,  20, 128, 200, 36, 1, 650, 70, 16 };

static void draw_board(const Layout *l, const Game *g, const char *o1, const char *o2) {
    int bx = l->bx, by = l->by, cs = l->cs;
    uint32_t h = 2166136261u;
    h = fnv(h, g->board, sizeof g->board); h = fnv(h, &g->cur, sizeof g->cur); h = fnv(h, &g->state, sizeof g->state);
    h = fnv(h, &g->pending, sizeof g->pending); h = fnv_s(h, o1); h = fnv_s(h, o2);
    if (!dirty(S_BOARD, h)) return;
    region_begin(bx - 14, by - 6, cs * TT_W + 28, cs * TT_VIS + 12);
    clear(bx - 14, by - 6, cs * TT_W + 28, cs * TT_VIS + 12);
    gfx_fill(bx, by, cs * TT_W, cs * TT_VIS, WELL);
    for (int x = 1; x < TT_W; x++) gfx_fill(bx + x * cs, by, 1, cs * TT_VIS, GRID);
    for (int y = TT_HIDDEN; y < TT_H; y++) for (int x = 0; x < TT_W; x++)
        if (g->board[y][x]) cell(bx + x * cs, by + (y - TT_HIDDEN) * cs, cs, g->board[y][x]);
    if (g->state == ST_PLAYING) {
        Piece gh = tt_ghost(g); int c[4][2]; tt_piece_cells(gh.type, gh.rot, c);
        for (int i = 0; i < 4; i++) {
            int y = gh.y + c[i][1] - TT_HIDDEN; if (y < 0) continue;
            gfx_fill_a(bx + (gh.x + c[i][0]) * cs, by + y * cs, cs, cs, PC[gh.type + 1], 90);
        }
        tt_piece_cells(g->cur.type, g->cur.rot, c);
        for (int i = 0; i < 4; i++) {
            int y = g->cur.y + c[i][1] - TT_HIDDEN; if (y < 0) continue;
            cell(bx + (g->cur.x + c[i][0]) * cs, by + y * cs, cs, g->cur.type + 1);
        }
    }
    gfx_rrect_outline(bx - 4, by - 4, cs * TT_W + 8, cs * TT_VIS + 8, 4, 2, RGB(70, 78, 120));
    int pend = g->pending > TT_VIS ? TT_VIS : g->pending;
    if (pend) gfx_fill(bx - 12, by + (TT_VIS - pend) * cs, 6, pend * cs, RGB(235, 60, 60));
    overlay(bx, by, cs, o1, o2, &font_title);
    region_end();
}
static void draw_hold(const Layout *l, const Game *g) {
    if (!dirty(S_HOLD, (uint32_t)g->hold + 7u)) return;
    region_begin(l->hx - 2, l->hy - 2, l->hw + 4, l->hh + 4);
    panel(l->hx, l->hy, l->hw, l->hh, "HOLD");
    if (g->hold >= 0) mini(l->hx + l->hw / 2, l->hy + l->hh / 2 + 10, 18, g->hold);
    region_end();
}
static void draw_next(const Layout *l, const Game *g) {
    uint32_t h = 2166136261u; for (int i = 0; i < 5; i++) { int t = tt_next(g, i); h = fnv(h, &t, sizeof t); }
    if (!dirty(S_NEXT, h)) return;
    region_begin(l->nx - 2, l->ny - 2, l->nw + 4, l->nh + 4);
    panel(l->nx, l->ny, l->nw, l->nh, "NEXT");
    for (int i = 0; i < 5; i++) mini(l->nx + l->nw / 2, l->ny + 62 + i * 54, l->ncs - (l->vs ? 2 : 0), tt_next(g, i));
    region_end();
}
static void stat(int x, int y, const char *label, const char *val) {
    gfx_text(&font_small, x, y, label, GREY);
    gfx_text(&font_label, x, y + 26, val, WHITE);
}
static void draw_stats(const Layout *l, const Game *g) {
    char s[32];
    uint32_t h = 2166136261u;
    h = fnv(h, &g->score, sizeof g->score); h = fnv(h, &g->lines, sizeof g->lines); h = fnv(h, &g->level, sizeof g->level);
    h = fnv(h, &g->frames, sizeof g->frames);
    if (!dirty(S_STATS, h)) return;
    region_begin(l->sx - 2, l->sy - 2, l->sw + 4, l->sh + 4);
    panel(l->sx, l->sy, l->sw, l->sh, NULL);
    int x = l->sx + 14, y = l->sy + 26;
    snprintf(s, sizeof s, "%u", (unsigned)g->score); stat(x, y, "SCORE", s);
    if (g->cfg.goal_lines) snprintf(s, sizeof s, "%d / %d", g->lines, g->cfg.goal_lines); else snprintf(s, sizeof s, "%d", g->lines);
    stat(x, y + 56, "LINES", s);
    snprintf(s, sizeof s, "%d", g->level); stat(x, y + 112, "LEVEL", s);
    if (g->cfg.time_limit) fmt_time(s, sizeof s, (uint32_t)g->cfg.time_limit - (g->frames > (uint32_t)g->cfg.time_limit ? (uint32_t)g->cfg.time_limit : g->frames));
    else fmt_time(s, sizeof s, g->frames);
    stat(x, y + 168, g->cfg.time_limit ? "TIME LEFT" : "TIME", s);
    region_end();
}
static void draw_popup(const Layout *l, const Game *g) {
    static const char *N[5] = { "", "SINGLE", "DOUBLE", "TRIPLE", "TETRIS" };
    const ClearInfo *c = &g->last;
    uint32_t h = 2166136261u; int show = c->age < 120 && (c->lines || c->tspin);
    h = fnv(h, &show, sizeof show);
    if (show) { h = fnv(h, &c->lines, sizeof c->lines); h = fnv(h, &c->tspin, sizeof c->tspin); h = fnv(h, &c->mini, sizeof c->mini);
                h = fnv(h, &c->b2b, sizeof c->b2b); h = fnv(h, &c->perfect, sizeof c->perfect); h = fnv(h, &c->combo, sizeof c->combo); }
    if (!dirty(S_POPUP, h)) return;
    region_begin(l->px, l->py, l->pw + 40, l->ph + 40);
    clear(l->px, l->py, l->pw + 40, l->ph + 40);
    if (c->age < 120 && (c->lines || c->tspin)) {
        char s[48];
        snprintf(s, sizeof s, "%s%s%s", c->b2b ? "B2B " : "", c->tspin ? (c->mini ? "MINI T-SPIN " : "T-SPIN ") : "", c->lines ? N[c->lines] : "");
        gfx_text(&font_body, l->px, l->py + 18, s, GOLD);
        if (c->perfect) gfx_text(&font_body, l->px, l->py + 40, "PERFECT CLEAR", GOLD);
        else if (c->combo > 1) { snprintf(s, sizeof s, "%d COMBO", c->combo - 1); gfx_text(&font_body, l->px, l->py + 40, s, WHITE); }
    }
    region_end();
}
static void draw_opp(const Layout *l, const OppView *o, const char *status) {
    int ox = l->ox, oy = l->oy, cs = l->ocs;
    uint32_t h = 2166136261u;
    h = fnv(h, o->board, sizeof o->board); h = fnv(h, &o->valid, sizeof o->valid); h = fnv(h, &o->pending, sizeof o->pending);
    h = fnv(h, &o->score, sizeof o->score); h = fnv(h, &o->lines, sizeof o->lines); h = fnv_s(h, status);
    if (!dirty(S_OPP, h)) return;
    region_begin(ox - 20, oy - 52, cs * TT_W + 40, cs * TT_VIS + 150);
    clear(ox - 20, oy - 52, cs * TT_W + 40, cs * TT_VIS + 150);
    gfx_text_c(&font_small, ox + cs * TT_W / 2, oy - 24, "OPPONENT", GREY);
    gfx_fill(ox, oy, cs * TT_W, cs * TT_VIS, WELL);
    for (int x = 1; x < TT_W; x++) gfx_fill(ox + x * cs, oy, 1, cs * TT_VIS, GRID);
    if (o->valid) {
        for (int y = 0; y < TT_VIS; y++) for (int x = 0; x < TT_W; x++)
            if (o->board[y][x] && o->board[y][x] <= 8) cell(ox + x * cs, oy + y * cs, cs, o->board[y][x]);
        int pend = o->pending > TT_VIS ? TT_VIS : o->pending;
        if (pend) gfx_fill(ox - 10, oy + (TT_VIS - pend) * cs, 5, pend * cs, RGB(235, 60, 60));
    }
    gfx_rrect_outline(ox - 4, oy - 4, cs * TT_W + 8, cs * TT_VIS + 8, 4, 2, RGB(70, 78, 120));
    if (status) { gfx_fill_a(ox, oy, cs * TT_W, cs * TT_VIS, RGB(0, 0, 0), 170); gfx_text_c(&font_label, ox + cs * TT_W / 2, oy + cs * 9, status, WHITE); }
    char s[40]; snprintf(s, sizeof s, "%u pts   %d lines", o->valid ? (unsigned)o->score : 0u, o->valid ? o->lines : 0);
    gfx_text_c(&font_small, ox + cs * TT_W / 2, oy + cs * TT_VIS + 28, s, WHITE);
    region_end();
}
static void draw_static(const Layout *l, const char *title) {
    sig_reset();
    gfx_fill(0, 0, GFX_W, GFX_H, BG);
    if (title) gfx_text(&font_small, l->vs ? 20 : 70, 470, title, GREY);
    gfx_present_all();
}

/* ---------------------------------------------------------------- simple full-screen messages */
static void message(const char *title, const char *l1, const char *l2) {
    gfx_fill(0, 0, GFX_W, GFX_H, BG);
    gfx_text_c(&font_title, GFX_W / 2, 190, title, WHITE);
    if (l1) gfx_text_c(&font_body, GFX_W / 2, 240, l1, GREY);
    if (l2) gfx_text_c(&font_body, GFX_W / 2, 270, l2, GREY);
    gfx_text_c(&font_small, GFX_W / 2, 420, "A  OK", GREY);
    gfx_present_all();
    for (;;) { poll(); if (g_p & (WUP_A | WUP_B)) return; wup_sleep(16); }
}

static uint32_t seed_now(void) { return (wup_us() * 2654435761u) | 1u; }

static const char *countdown_text(const Game *g) {
    static char s[4];
    if (g->state != ST_COUNTDOWN) return NULL;
    snprintf(s, sizeof s, "%d", (g->countdown + 59) / 60);
    return s;
}

/* ---------------------------------------------------------------- single player */
static void record_single(const Game *g) {
    GameMode m = g->cfg.mode; uint32_t *b = &sv.v[m];
    if (m == MODE_SPRINT || m == MODE_CHEESE) { if (g->state == ST_WON && (*b == 0 || g->frames < *b)) *b = g->frames; }
    else if (g->score > *b) *b = g->score;
    sv_save();
}

/* returns 1 to play again, 0 for the menu */
static int play_single(GameMode m) {
    ModeCfg c; tt_cfg_for_mode(&c, m);
    Game g; tt_init(&g, &c, seed_now());
    const Layout *l = &L_SOLO;
    draw_static(l, tt_mode_name(m));
    int paused = 0, recorded = 0;
    frame_start();
    for (;;) {
        poll();
        if (g.state == ST_WON || g.state == ST_LOST) {
            if (!recorded) { record_single(&g); recorded = 1; }
            if (g_p & WUP_A) return 1;
            if (g_p & WUP_B) return 0;
        } else if (g_p & WUP_PLUS) { paused = !paused; frame_start(); }
        if (paused && (g_p & WUP_B)) return 0;
        if (paused && (g_p & WUP_A)) { paused = 0; frame_start(); }

        int steps = 1;
        if (paused || g.state == ST_WON || g.state == ST_LOST) wup_sleep(16); else steps = frame_wait();
        if (!paused) for (int i = 0; i < steps; i++) tt_step(&g, pad_mask());

        const char *o1 = NULL, *o2 = NULL; char t[40];
        if (paused) { o1 = "PAUSED"; o2 = "A resume    B quit"; }
        else if (g.state == ST_WON) {
            o1 = g.cfg.time_limit ? "TIME!" : "FINISHED!";
            if (m == MODE_SPRINT || m == MODE_CHEESE) { char tm[16]; fmt_time(tm, sizeof tm, g.frames); snprintf(t, sizeof t, "%s   A again  B menu", tm); o2 = t; }
            else { snprintf(t, sizeof t, "%u pts   A again  B menu", (unsigned)g.score); o2 = t; }
        } else if (g.state == ST_LOST) { o1 = "GAME OVER"; snprintf(t, sizeof t, "%u pts   A again  B menu", (unsigned)g.score); o2 = t; }
        else o1 = countdown_text(&g);
        g_sync = 1; draw_board(l, &g, o1, o2);
        draw_hold(l, &g); draw_next(l, &g); draw_stats(l, &g); draw_popup(l, &g);
    }
}

/* ---------------------------------------------------------------- networking glue */
typedef struct { int fd; struct sockaddr_in peer; int has_peer, host; uint32_t my_ip, bcast; } Net;
static int net_send(void *ctx, const uint8_t *b, int n) {
    Net *s = ctx; struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(NET_PORT);
    if (s->has_peer) return (int)sendto(s->fd, b, (size_t)n, 0, (struct sockaddr *)&s->peer, sizeof s->peer);
    a.sin_addr.s_addr = 0xffffffffu; sendto(s->fd, b, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
    if (s->bcast && s->bcast != 0xffffffffu) { a.sin_addr.s_addr = s->bcast; sendto(s->fd, b, (size_t)n, 0, (struct sockaddr *)&a, sizeof a); }
    return n;
}
static int net_recv(void *ctx, uint8_t *b, int cap) {
    Net *s = ctx;
    for (;;) {
        struct sockaddr_in f; socklen_t fl = sizeof f;
        int n = (int)recvfrom(s->fd, b, (size_t)cap, 0, (struct sockaddr *)&f, &fl);
        if (n <= 0) return 0;
        if (f.sin_addr.s_addr == s->my_ip) continue;                  /* our own broadcast */
        if (n < 4 || b[0] != 'T' || b[1] != 'T') continue;
        if (!s->has_peer) {                                           /* host locks onto a JOIN, client onto a HELLO */
            if (b[3] != (s->host ? NP_JOIN : NP_HELLO)) continue;
            s->peer = f; s->has_peer = 1;
        } else if (f.sin_addr.s_addr != s->peer.sin_addr.s_addr) continue;
        return n;
    }
}
static int net_open(Net *s, int host) {
    memset(s, 0, sizeof *s); s->host = host;
    if (wup_net_init() < 0) return -1;
    uint32_t ip, mask, gw, dns; wup_net_info(&ip, &mask, &gw, &dns);
    s->my_ip = ip; s->bcast = ip | ~mask;
    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) return -2;
    int one = 1; setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(NET_PORT); a.sin_addr.s_addr = 0;
    if (bind(s->fd, (struct sockaddr *)&a, sizeof a)) { close(s->fd); return -3; }
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

/* ---------------------------------------------------------------- versus */
static void lobby_screen(int host, GameMode m, int tick) {
    char s[64];
    gfx_fill(0, 0, GFX_W, GFX_H, BG);
    gfx_text_c(&font_title, GFX_W / 2, 170, host ? "Hosting" : "Looking for a game", WHITE);
    if (host) {
        gfx_text_c(&font_body, GFX_W / 2, 220, tt_mode_name(m), GOLD);
        snprintf(s, sizeof s, "Your address: %s", wup_net_ip_str());
        gfx_text_c(&font_body, GFX_W / 2, 258, s, GREY);
        gfx_text_c(&font_body, GFX_W / 2, 288, "Ask your friend to choose Join a Game", GREY);
    } else gfx_text_c(&font_body, GFX_W / 2, 230, "Searching your Wi-Fi network", GREY);
    snprintf(s, sizeof s, "%.*s", (tick / 20) % 4, "...");
    gfx_text_c(&font_title, GFX_W / 2, 350, s, ACCENT);
    gfx_text_c(&font_small, GFX_W / 2, 440, "B  Cancel", GREY);
    gfx_present_all();
}

static void play_versus(int host, GameMode m) {
    Net net;
    int r = net_open(&net, host);
    if (r == -1) { message("Not connected", "Join a Wi-Fi network in the Homebrew Menu", "(Settings > Network) and try again."); return; }
    if (r < 0)   { message("Network error", "Could not open the game port.", NULL); return; }
    NetTransport tr = { net_send, net_recv, &net };
    static Versus v;
    if (host) { ModeCfg c; tt_cfg_for_mode(&c, m); vs_host(&v, &c, seed_now(), tr); } else vs_join(&v, tr);

    int tick = 0;
    while (v.phase != VS_RUNNING) {                               /* lobby */
        poll();
        if (g_p & WUP_B) { close(net.fd); return; }
        vs_step(&v, 0);
        if ((tick++ % 20) == 0) lobby_screen(host, m, tick);
        wup_sleep(16);
    }
    const Layout *l = &L_VS;
    draw_static(l, tt_mode_name(v.cfg.mode));
    int leave_prompt = 0, quiet = 0; uint16_t last_seq = 0;
    int result_recorded = 0, left = 0;
    frame_start();
    for (;;) {
        poll();
        if (v.phase == VS_DONE) { if (g_p & (WUP_A | WUP_B)) break; }
        else if (v.result == VS_PENDING) {
            if (g_p & WUP_PLUS) leave_prompt = !leave_prompt;
            if (leave_prompt && (g_p & WUP_B)) leave_prompt = 0;
            else if (leave_prompt && (g_p & WUP_A)) {              /* forfeit: the peer is told by our final STATE */
                if (v.me.state == ST_PLAYING || v.me.state == ST_COUNTDOWN) v.me.state = ST_LOST;
                leave_prompt = 0; left = 1;
            }
        }
        int steps = frame_wait();
        for (int i = 0; i < steps; i++) vs_step(&v, leave_prompt ? 0 : pad_mask());

        if (v.opp.valid && v.opp.seq != last_seq) { last_seq = v.opp.seq; quiet = 0; } else if (v.phase == VS_RUNNING) quiet++;
        if (quiet > 600 && v.result == VS_PENDING) { close(net.fd); message("Connection lost", "The other player stopped responding.", NULL); return; }

        const char *o1 = NULL, *o2 = NULL;
        if (v.result == VS_WIN)       { o1 = "YOU WIN!";  o2 = v.phase == VS_DONE ? "A / B  menu" : NULL; }
        else if (v.result == VS_LOSE) { o1 = left ? "FORFEIT" : "YOU LOSE"; o2 = v.phase == VS_DONE ? "A / B  menu" : NULL; }
        else if (v.result == VS_DRAW) { o1 = "DRAW";      o2 = v.phase == VS_DONE ? "A / B  menu" : NULL; }
        else if (leave_prompt)        { o1 = "FORFEIT?";  o2 = "A yes    B no"; }
        else o1 = countdown_text(&v.me);
        if (v.result != VS_PENDING && !result_recorded) {
            result_recorded = 1;
            if (v.result == VS_WIN) { sv.v[v.cfg.mode]++; sv_save(); }
        }
        const char *os = NULL;
        if (v.opp.valid) os = v.opp.status == 1 ? "TOPPED OUT" : v.opp.status == 2 ? "FINISHED" : NULL;
        else os = "connecting";
        g_sync = 1; draw_board(l, &v.me, o1, o2);
        draw_hold(l, &v.me); draw_next(l, &v.me); draw_stats(l, &v.me); draw_popup(l, &v.me);
        draw_opp(l, &v.opp, os);
    }
    close(net.fd);
}

/* ---------------------------------------------------------------- menu */
enum { M_MARATHON, M_SPRINT, M_ULTRA, M_ENDLESS, M_CHEESE, M_HOST_RACE, M_HOST_ATTACK, M_JOIN, M_COUNT };
static const char *M_LABEL[M_COUNT] = {
    "Marathon", "Sprint  40 lines", "Ultra  2 minutes", "Endless", "Cheese Race",
    "Versus Marathon  (host)", "Versus Attack  (host)", "Join a Game"
};
static const GameMode M_MODE[M_COUNT] = { MODE_MARATHON, MODE_SPRINT, MODE_ULTRA, MODE_ENDLESS, MODE_CHEESE, MODE_VS_RACE, MODE_VS_ATTACK, MODE_COUNT };

#define ROW_Y0 96
#define ROW_H  38
static void menu_draw(int sel) {
    gfx_fill(0, 0, GFX_W, GFX_H, BG);
    gfx_text(&font_big, 150, 70, "TETRIS", WHITE);
    for (int i = 0; i < M_COUNT; i++) {
        int y = ROW_Y0 + i * ROW_H;
        if (i == sel) gfx_rrect(130, y, 594, ROW_H - 4, 10, ACCENT, 256);
        else gfx_rrect(130, y, 594, ROW_H - 4, 10, PANEL, 256);
        gfx_text(&font_label, 152, y + 26, M_LABEL[i], WHITE);
        char s[32] = ""; GameMode gm = M_MODE[i];
        if (gm == MODE_COUNT) snprintf(s, sizeof s, "Wi-Fi");
        else if (gm == MODE_VS_RACE || gm == MODE_VS_ATTACK) { if (sv.v[gm]) snprintf(s, sizeof s, "%u wins", (unsigned)sv.v[gm]); }
        else if (sv.v[gm]) {
            if (gm == MODE_SPRINT || gm == MODE_CHEESE) { char t[16]; fmt_time(t, sizeof t, sv.v[gm]); snprintf(s, sizeof s, "best %s", t); }
            else snprintf(s, sizeof s, "best %u", (unsigned)sv.v[gm]);
        }
        if (s[0]) gfx_text(&font_body, 704 - gfx_text_w(&font_body, s), y + 25, s, i == sel ? WHITE : GREY);
    }
    gfx_text_c(&font_small, GFX_W / 2, 436, "Left/Right move   Down soft   Up hard drop   A rotate   B rotate back   X hold   + pause", GREY);
    gfx_text_c(&font_small, GFX_W / 2, 460, "Up/Down choose   A start   B exit", GREY);
    gfx_present_all();
}

static int menu(int *sel) {
    int dirty = 1;
    for (;;) {
        poll();
        if (g_p & WUP_UP)   { *sel = (*sel + M_COUNT - 1) % M_COUNT; dirty = 1; }
        if (g_p & WUP_DOWN) { *sel = (*sel + 1) % M_COUNT; dirty = 1; }
        if (g_tap && g_ty >= ROW_Y0 && g_ty < ROW_Y0 + M_COUNT * ROW_H && g_tx > 130 && g_tx < 724) {
            int i = (g_ty - ROW_Y0) / ROW_H;
            if (i == *sel) return i;
            *sel = i; dirty = 1;
        }
        if (g_p & WUP_A) return *sel;
        if (g_p & WUP_B) return -1;
        if (dirty) { menu_draw(*sel); dirty = 0; }
        wup_sleep(16);
    }
}

int main(void) {
    wup_screen();
    init_colors(); sv_load(); bb_init();
    g_b = wup_buttons();                     /* a button still held from the menu is not a new press */
    int sel = 0;
    for (;;) {
        int c = menu(&sel);
        if (c < 0) break;
        if (c == M_JOIN) play_versus(0, MODE_COUNT);
        else if (c == M_HOST_RACE) play_versus(1, MODE_VS_RACE);
        else if (c == M_HOST_ATTACK) play_versus(1, MODE_VS_ATTACK);
        else while (play_single(M_MODE[c])) {}
    }
    return 0;
}
