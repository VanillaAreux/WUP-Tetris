#include "tetris.h"
#include <string.h>

#define LOCK_DELAY 30
#define MAX_RESETS 15

static const int8_t BASE[P_COUNT][4][2] = {
    [P_I] = {{0,1},{1,1},{2,1},{3,1}},
    [P_O] = {{1,0},{2,0},{1,1},{2,1}},
    [P_T] = {{1,0},{0,1},{1,1},{2,1}},
    [P_S] = {{1,0},{2,0},{0,1},{1,1}},
    [P_Z] = {{0,0},{1,0},{1,1},{2,1}},
    [P_J] = {{0,0},{0,1},{1,1},{2,1}},
    [P_L] = {{2,0},{0,1},{1,1},{2,1}},
};

/* SRS kicks, standard y-up convention; negated on use. [cw: rot, ccw: 4+rot] */
static const int8_t KJ[8][5][2] = {
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}, {{0,0},{1,0},{1,-1},{0,2},{1,2}},
    {{0,0},{1,0},{1,1},{0,-2},{1,-2}},    {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},
    {{0,0},{1,0},{1,1},{0,-2},{1,-2}},    {{0,0},{1,0},{1,-1},{0,2},{1,2}},
    {{0,0},{-1,0},{-1,1},{0,-2},{-1,-2}}, {{0,0},{-1,0},{-1,-1},{0,2},{-1,2}},
};
static const int8_t KI[8][5][2] = {
    {{0,0},{-2,0},{1,0},{-2,-1},{1,2}},   {{0,0},{-1,0},{2,0},{-1,2},{2,-1}},
    {{0,0},{2,0},{-1,0},{2,1},{-1,-2}},   {{0,0},{1,0},{-2,0},{1,-2},{-2,1}},
    {{0,0},{-1,0},{2,0},{-1,2},{2,-1}},   {{0,0},{2,0},{-1,0},{2,1},{-1,-2}},
    {{0,0},{1,0},{-2,0},{1,-2},{-2,1}},   {{0,0},{-2,0},{1,0},{-2,-1},{1,2}},
};

/* frames per row at levels 1..20 (guideline curve) */
static const float FPR[20] = {
    60, 47.6f, 37, 28.4f, 21.3f, 15.7f, 11.4f, 8.1f, 5.6f, 3.9f,
    2.6f, 1.7f, 1.1f, 0.66f, 0.42f, 0.26f, 0.16f, 0.09f, 0.05f, 0.03f
};

static const char *MODE_NAMES[MODE_COUNT] = {
    "Marathon", "Sprint 40L", "Ultra 2:00", "Endless", "Cheese Race",
    "Versus Marathon", "Versus Attack"
};
const char *tt_mode_name(GameMode m) { return m < MODE_COUNT ? MODE_NAMES[m] : "?"; }

static uint32_t rnd(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x;
}

void tt_piece_cells(int type, int rot, int out[4][2]) {
    int n = (type == P_I) ? 4 : 3;
    for (int i = 0; i < 4; i++) {
        int x = BASE[type][i][0], y = BASE[type][i][1];
        if (type != P_O)
            for (int r = 0; r < (rot & 3); r++) { int nx = n - 1 - y, ny = x; x = nx; y = ny; }
        out[i][0] = x; out[i][1] = y;
    }
}

static bool blocked(const Game *g, int x, int y) {
    if (x < 0 || x >= TT_W || y < 0 || y >= TT_H) return true;
    return g->board[y][x] != 0;
}

bool tt_collides(const Game *g, Piece p) {
    int c[4][2]; tt_piece_cells(p.type, p.rot, c);
    for (int i = 0; i < 4; i++) if (blocked(g, p.x + c[i][0], p.y + c[i][1])) return true;
    return false;
}

Piece tt_ghost(const Game *g) {
    Piece p = g->cur;
    for (;;) { Piece q = p; q.y++; if (tt_collides(g, q)) break; p = q; }
    return p;
}

int tt_next(const Game *g, int i) { return g->queue[i]; }

void tt_cfg_for_mode(ModeCfg *c, GameMode m) {
    memset(c, 0, sizeof *c);
    c->mode = m; c->start_level = 1;
    switch (m) {
    case MODE_MARATHON: case MODE_VS_RACE: c->goal_lines = 150; c->level_up = true; break;
    case MODE_SPRINT:   c->goal_lines = 40; break;
    case MODE_ULTRA:    c->time_limit = 120 * 60; c->level_up = true; break;
    case MODE_ENDLESS:  c->level_up = true; break;
    case MODE_CHEESE:   c->cheese_rows = 10; break;
    case MODE_VS_ATTACK: c->level_up = false; c->garbage_on = true; c->start_level = 3; break;
    default: break;
    }
}

static void refill(Game *g) {
    int bag[7] = {0,1,2,3,4,5,6};
    for (int i = 6; i > 0; i--) { int j = rnd(&g->rng) % (i + 1); int t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
    for (int i = 0; i < 7; i++) g->queue[g->qlen++] = bag[i];
}

static int pop_piece(Game *g) {
    int t = g->queue[0];
    memmove(g->queue, g->queue + 1, sizeof(int) * (--g->qlen));
    if (g->qlen < 7) refill(g);
    return t;
}

static void spawn(Game *g, int type) {
    g->cur = (Piece){ type, 0, 3, 1 };
    g->lock_timer = 0; g->lock_resets = 0; g->grav_acc = 0; g->last_rot = false;
    if (tt_collides(g, g->cur)) { g->state = ST_LOST; return; }
    Piece d = g->cur; d.y++;
    if (!tt_collides(g, d)) g->cur = d;
    g->lowest_y = g->cur.y;
}

static void spawn_next(Game *g) { spawn(g, pop_piece(g)); g->hold_used = false; }

void tt_init(Game *g, const ModeCfg *c, uint32_t seed) {
    memset(g, 0, sizeof *g);
    g->cfg = *c;
    g->level = c->start_level > 0 ? c->start_level : 1;
    g->rng = seed ? seed : 1;
    g->grng = (seed * 2654435761u) ^ 0xA5A5A5A5u; if (!g->grng) g->grng = 7;
    g->countdown = 180; g->hold = -1;
    g->das = 10; g->arr = 2;
    g->state = ST_COUNTDOWN;
    refill(g);
    for (int i = 0; i < c->cheese_rows && i < TT_VIS - 4; i++) {
        int hole = rnd(&g->grng) % TT_W;
        for (int x = 0; x < TT_W; x++) g->board[TT_H - 1 - i][x] = (x == hole) ? 0 : CELL_GARBAGE;
    }
    g->last.age = 1000;
}

static void after_move(Game *g) {
    if (g->cur.y > g->lowest_y) { g->lowest_y = g->cur.y; g->lock_resets = 0; g->lock_timer = 0; }
    else {
        Piece d = g->cur; d.y++;
        if (tt_collides(g, d) && g->lock_resets < MAX_RESETS) { g->lock_timer = 0; g->lock_resets++; }
    }
}

static bool try_move(Game *g, int dx, int dy) {
    Piece p = g->cur; p.x += dx; p.y += dy;
    if (tt_collides(g, p)) return false;
    g->cur = p; g->last_rot = false; after_move(g);
    return true;
}

static bool try_rotate(Game *g, int dir) {
    if (g->cur.type == P_O) return false;
    int to = (g->cur.rot + dir + 4) & 3;
    int idx = dir > 0 ? g->cur.rot : 4 + g->cur.rot;
    const int8_t (*K)[2] = (g->cur.type == P_I) ? KI[idx] : KJ[idx];
    for (int k = 0; k < 5; k++) {
        Piece p = { g->cur.type, to, g->cur.x + K[k][0], g->cur.y - K[k][1] };
        if (!tt_collides(g, p)) {
            g->cur = p; after_move(g);
            g->last_rot = true; g->last_kick = k;   /* after_move must not clear it */
            return true;
        }
    }
    return false;
}

/* returns false if the stack overflowed */
static bool add_garbage(Game *g, int n) {
    for (int r = 0; r < n; r++) for (int x = 0; x < TT_W; x++) if (g->board[r][x]) return false;
    memmove(g->board[0], g->board[n], (size_t)(TT_H - n) * TT_W);
    int hole = rnd(&g->grng) % TT_W;
    for (int r = TT_H - n; r < TT_H; r++)
        for (int x = 0; x < TT_W; x++) g->board[r][x] = (x == hole) ? 0 : CELL_GARBAGE;
    return true;
}

void tt_receive_garbage(Game *g, int n) {
    if (!g->cfg.garbage_on || n <= 0) return;
    g->pending += n; if (g->pending > 40) g->pending = 40;
}

static bool cheese_done(const Game *g) {
    for (int y = 0; y < TT_H; y++) for (int x = 0; x < TT_W; x++)
        if (g->board[y][x] == CELL_GARBAGE) return false;
    return true;
}

static void lock_piece(Game *g) {
    int c[4][2]; tt_piece_cells(g->cur.type, g->cur.rot, c);

    bool tspin = false, mini = false;
    if (g->cur.type == P_T && g->last_rot) {
        static const int cx[4] = {0,2,0,2}, cy[4] = {0,0,2,2};
        int occ[4], n = 0;
        for (int i = 0; i < 4; i++) { occ[i] = blocked(g, g->cur.x + cx[i], g->cur.y + cy[i]); n += occ[i]; }
        if (n >= 3) {
            int f;
            switch (g->cur.rot) {
            case 0:  f = occ[0] + occ[1]; break;
            case 1:  f = occ[1] + occ[3]; break;
            case 2:  f = occ[2] + occ[3]; break;
            default: f = occ[0] + occ[2]; break;
            }
            tspin = true; mini = (f < 2 && g->last_kick != 4);
        }
    }

    bool all_hidden = true;
    for (int i = 0; i < 4; i++) {
        int x = g->cur.x + c[i][0], y = g->cur.y + c[i][1];
        g->board[y][x] = (uint8_t)(g->cur.type + 1);
        if (y >= TT_HIDDEN) all_hidden = false;
    }
    g->pieces++;
    if (all_hidden) { g->state = ST_LOST; return; }

    int cleared = 0;
    for (int r = TT_H - 1; r >= 0; r--) {
        bool full = true;
        for (int x = 0; x < TT_W; x++) if (!g->board[r][x]) { full = false; break; }
        if (!full) continue;
        memmove(g->board[1], g->board[0], (size_t)r * TT_W);
        memset(g->board[0], 0, TT_W);
        cleared++; r++;
    }
    bool perfect = false;
    if (cleared) {
        perfect = true;
        for (int y = 0; y < TT_H && perfect; y++) for (int x = 0; x < TT_W; x++) if (g->board[y][x]) { perfect = false; break; }
    }

    static const int S_LINE[5] = {0,100,300,500,800}, S_TS[5] = {400,800,1200,1600,1600}, S_MINI[5] = {100,200,400,400,400};
    static const int A_LINE[5] = {0,0,1,2,4}, A_TS[5] = {0,2,4,6,6}, A_MINI[5] = {0,0,1,2,2};
    static const int A_COMBO[9] = {0,0,1,1,2,2,3,3,4};

    bool elig = cleared > 0 && (cleared == 4 || tspin);
    bool bonus_b2b = elig && g->b2b;
    int sc = (tspin ? (mini ? S_MINI : S_TS) : S_LINE)[cleared] * g->level;
    if (bonus_b2b) sc = sc * 3 / 2;
    int atk = 0;
    if (cleared) {
        sc += 50 * g->combo * g->level;
        if (perfect) sc += 800 * g->level;
        atk = (tspin ? (mini ? A_MINI : A_TS) : A_LINE)[cleared];
        if (bonus_b2b) atk += 1;
        atk += A_COMBO[g->combo > 8 ? 8 : g->combo];
        if (perfect) atk = 10;
    }
    g->score += (uint32_t)sc;
    if (cleared || tspin) {
        g->last = (ClearInfo){ cleared, tspin, mini, bonus_b2b, perfect, g->combo + (cleared ? 1 : 0), atk, sc, 0 };
    }

    if (cleared) {
        g->combo++; g->b2b = elig; g->lines += cleared;
        if (g->cfg.level_up) {
            int lv = g->cfg.start_level + g->lines / 10;
            g->level = lv > 20 ? 20 : lv;
        }
        if (g->cfg.garbage_on) {
            int cancel = atk < g->pending ? atk : g->pending;
            g->pending -= cancel; atk -= cancel;
            g->out_total = (uint16_t)(g->out_total + atk);
        }
    } else {
        g->combo = 0;
        if (g->pending > 0 && g->cfg.garbage_on) {
            int n = g->pending > 8 ? 8 : g->pending;
            g->pending -= n;
            if (!add_garbage(g, n)) { g->state = ST_LOST; return; }
        }
    }

    if (g->cfg.goal_lines && g->lines >= g->cfg.goal_lines) { g->state = ST_WON; return; }
    if (g->cfg.cheese_rows && cheese_done(g)) { g->state = ST_WON; return; }
    spawn_next(g);
}

void tt_step(Game *g, uint8_t in) {
    uint8_t pressed = in & (uint8_t)~g->prev_in;
    g->prev_in = in;
    if (g->last.age < 1000) g->last.age++;

    if (g->state == ST_COUNTDOWN) {
        if (--g->countdown <= 0) { g->state = ST_PLAYING; spawn_next(g); }
        return;
    }
    if (g->state != ST_PLAYING) return;

    g->frames++;
    if (g->cfg.time_limit && g->frames >= (uint32_t)g->cfg.time_limit) { g->state = ST_WON; return; }

    if ((pressed & IN_HOLD) && !g->hold_used) {
        int t = g->cur.type;
        if (g->hold < 0) { g->hold = t; spawn_next(g); }
        else { int h = g->hold; g->hold = t; spawn(g, h); }
        g->hold_used = true;
        if (g->state != ST_PLAYING) return;
    }

    if (pressed & IN_CW)  try_rotate(g, 1);
    if (pressed & IN_CCW) try_rotate(g, -1);

    int dir = ((in & IN_LEFT) && !(in & IN_RIGHT)) ? -1 : ((in & IN_RIGHT) && !(in & IN_LEFT)) ? 1 : 0;
    if (dir != g->das_dir) {
        g->das_dir = dir; g->das_timer = 0;
        if (dir) try_move(g, dir, 0);
    } else if (dir) {
        g->das_timer++;
        if (g->das_timer >= g->das) {
            if (g->arr == 0) { while (try_move(g, dir, 0)) {} }
            else if ((g->das_timer - g->das) % g->arr == 0) try_move(g, dir, 0);
        }
    }

    if (pressed & IN_HARD) {
        int n = 0;
        while (try_move(g, 0, 1)) n++;
        g->score += (uint32_t)(2 * n);
        lock_piece(g);
        return;
    }

    float fpr = FPR[(g->level < 1 ? 1 : g->level > 20 ? 20 : g->level) - 1];
    float gr = 1.0f / fpr;
    bool soft = (in & IN_SOFT) != 0;
    if (soft) { gr *= 20.0f; if (gr < 0.5f) gr = 0.5f; }
    g->grav_acc += gr;
    for (int i = 0; i < 24 && g->grav_acc >= 1.0f; i++) {
        g->grav_acc -= 1.0f;
        if (try_move(g, 0, 1)) { if (soft) g->score++; }
        else { g->grav_acc = 0; break; }
    }

    Piece d = g->cur; d.y++;
    if (tt_collides(g, d)) { if (++g->lock_timer >= LOCK_DELAY) lock_piece(g); }
    else g->lock_timer = 0;
}
