#include "netplay.h"
#include <string.h>

static uint8_t *hdr(uint8_t *b, int type) { b[0] = 'T'; b[1] = 'T'; b[2] = 1; b[3] = (uint8_t)type; return b + 4; }
static uint8_t *w16(uint8_t *p, uint16_t v) { *p++ = (uint8_t)(v >> 8); *p++ = (uint8_t)v; return p; }
static uint8_t *w32(uint8_t *p, uint32_t v) { p = w16(p, (uint16_t)(v >> 16)); return w16(p, (uint16_t)v); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t r32(const uint8_t *p) { return ((uint32_t)r16(p) << 16) | r16(p + 2); }

static void send_simple(Versus *v, int type) {
    uint8_t b[4]; hdr(b, type); v->net.send(v->net.ctx, b, 4);
}

static void send_hello(Versus *v) {
    uint8_t b[NP_HELLO_LEN]; uint8_t *p = hdr(b, NP_HELLO);
    *p++ = (uint8_t)v->cfg.mode;
    p = w16(p, (uint16_t)v->cfg.goal_lines);
    *p++ = (uint8_t)v->cfg.start_level;
    *p++ = (uint8_t)((v->cfg.level_up ? 1 : 0) | (v->cfg.garbage_on ? 2 : 0));
    *p++ = (uint8_t)v->cfg.cheese_rows;
    w32(p, v->seed);
    v->net.send(v->net.ctx, b, NP_HELLO_LEN);
}

static uint8_t status_of(const Game *g) { return g->state == ST_LOST ? 1 : g->state == ST_WON ? 2 : 0; }

static void send_state(Versus *v) {
    const Game *g = &v->me;
    uint8_t b[NP_STATE_LEN]; uint8_t *p = hdr(b, NP_STATE);
    p = w16(p, ++v->seq);
    p = w32(p, g->frames);
    *p++ = status_of(g);
    *p++ = (uint8_t)(g->pending > 255 ? 255 : g->pending);
    p = w16(p, g->out_total);
    p = w16(p, (uint16_t)g->lines);
    p = w32(p, g->score);
    *p++ = (uint8_t)g->level;

    uint8_t cells[TT_VIS][TT_W];
    for (int y = 0; y < TT_VIS; y++) memcpy(cells[y], g->board[y + TT_HIDDEN], TT_W);
    if (g->state == ST_PLAYING) {
        int c[4][2]; tt_piece_cells(g->cur.type, g->cur.rot, c);
        for (int i = 0; i < 4; i++) {
            int x = g->cur.x + c[i][0], y = g->cur.y + c[i][1] - TT_HIDDEN;
            if (x >= 0 && x < TT_W && y >= 0 && y < TT_VIS) cells[y][x] = (uint8_t)(g->cur.type + 1);
        }
    }
    const uint8_t *flat = &cells[0][0];
    for (int i = 0; i < TT_VIS * TT_W; i += 2) *p++ = (uint8_t)((flat[i] << 4) | (flat[i + 1] & 15));
    v->net.send(v->net.ctx, b, NP_STATE_LEN);
    v->last_sent_total = g->out_total; v->last_status = status_of(g);
}

static void on_state(Versus *v, const uint8_t *p) {
    OppView *o = &v->opp;
    uint16_t seq = r16(p);
    if (o->valid && (int16_t)(seq - o->seq) <= 0) return;      /* stale or duplicate */
    o->valid = true; o->seq = seq;
    o->frame = r32(p + 2); o->status = p[6]; o->pending = p[7];
    o->garb_total = r16(p + 8); o->lines = r16(p + 10);
    o->score = r32(p + 12); o->level = p[16];
    uint8_t *flat = &o->board[0][0]; const uint8_t *q = p + 17;
    for (int i = 0; i < TT_VIS * TT_W; i += 2, q++) { flat[i] = *q >> 4; flat[i + 1] = *q & 15; }
}

static void on_packet(Versus *v, const uint8_t *b, int n) {
    if (n < 4 || b[0] != 'T' || b[1] != 'T' || b[2] != 1) return;
    const uint8_t *p = b + 4;
    switch (b[3]) {
    case NP_JOIN:
        if (v->host && (v->phase == VS_LOBBY || v->phase == VS_WAIT_READY)) { v->phase = VS_WAIT_READY; send_hello(v); }
        break;
    case NP_HELLO:
        if (!v->host && n >= NP_HELLO_LEN && (v->phase == VS_LOBBY || v->phase == VS_WAIT_START)) {
            if (v->phase == VS_LOBBY) {
                if (p[0] >= MODE_COUNT) return;
                tt_cfg_for_mode(&v->cfg, (GameMode)p[0]);
                v->cfg.goal_lines = r16(p + 1); v->cfg.start_level = p[3];
                v->cfg.level_up = p[4] & 1; v->cfg.garbage_on = (p[4] & 2) != 0; v->cfg.cheese_rows = p[5];
                v->seed = r32(p + 6);
                tt_init(&v->me, &v->cfg, v->seed);
                v->me.grng ^= 0x5555AAAAu;
                v->phase = VS_WAIT_START;
            }
            send_simple(v, NP_READY);
        }
        break;
    case NP_READY:
        if (v->host && (v->phase == VS_WAIT_READY || v->phase == VS_RUNNING)) {
            v->phase = VS_RUNNING; send_simple(v, NP_START);
        }
        break;
    case NP_START:
        if (!v->host && v->phase == VS_WAIT_START) v->phase = VS_RUNNING;
        break;
    case NP_STATE:
        if (n < NP_STATE_LEN) return;
        if (!v->host && v->phase == VS_WAIT_START) v->phase = VS_RUNNING;   /* START was lost */
        if (v->phase == VS_RUNNING || v->phase == VS_DONE) on_state(v, p);
        break;
    }
}

void vs_host(Versus *v, const ModeCfg *cfg, uint32_t seed, NetTransport net) {
    memset(v, 0, sizeof *v);
    v->host = true; v->net = net; v->cfg = *cfg; v->seed = seed ? seed : 1;
    tt_init(&v->me, &v->cfg, v->seed);
    v->phase = VS_LOBBY;
}

void vs_join(Versus *v, NetTransport net) {
    memset(v, 0, sizeof *v);
    v->host = false; v->net = net; v->phase = VS_LOBBY;
}

static VsResult eval(Versus *v) {
    const Game *g = &v->me; const OppView *o = &v->opp;
    if (!o->valid) return VS_PENDING;
    bool meL = g->state == ST_LOST, oL = o->status == 1;
    bool meW = g->state == ST_WON,  oW = o->status == 2;
    if (meL && oL) {
        if (v->cfg.mode == MODE_VS_ATTACK) return VS_DRAW;
        return g->lines > o->lines ? VS_WIN : g->lines < o->lines ? VS_LOSE : VS_DRAW;
    }
    if (meL) return VS_LOSE;
    if (oL)  return VS_WIN;
    if (v->cfg.mode == MODE_VS_ATTACK) return VS_PENDING;
    /* race: lower finishing frame wins; wait out in-flight packets before declaring a win */
    if (meW && oW) return g->frames < o->frame ? VS_WIN : g->frames > o->frame ? VS_LOSE : VS_DRAW;
    if (oW) return g->frames > o->frame ? VS_LOSE : VS_PENDING;
    if (meW) { if (o->frame > g->frames || ++v->grace > 90) return VS_WIN; }
    return VS_PENDING;
}

static void run(Versus *v, uint8_t in) {
    if (v->result == VS_PENDING) {
        tt_step(&v->me, in);
        if (v->opp.valid && v->cfg.garbage_on) {
            uint16_t d = (uint16_t)(v->opp.garb_total - v->garb_applied);
            if (d && d < 1000) { tt_receive_garbage(&v->me, d); v->garb_applied = v->opp.garb_total; }
        }
        v->result = eval(v);
        if (v->result != VS_PENDING) v->end_timer = 120;   /* keep broadcasting so the peer sees our final state */
    }
    if (v->tick % 4 == 0 || v->me.out_total != v->last_sent_total || status_of(&v->me) != v->last_status)
        send_state(v);
    if (v->result != VS_PENDING && --v->end_timer <= 0) v->phase = VS_DONE;
}

void vs_step(Versus *v, uint8_t in) {
    uint8_t buf[256]; int n;
    while ((n = v->net.recv(v->net.ctx, buf, sizeof buf)) > 0) on_packet(v, buf, n);
    v->tick++;
    switch (v->phase) {
    case VS_LOBBY:      if (!v->host && v->tick % 30 == 1) send_simple(v, NP_JOIN); break;
    case VS_WAIT_READY: if (v->tick % 30 == 1) send_hello(v); break;
    case VS_WAIT_START: if (v->tick % 30 == 1) send_simple(v, NP_READY); break;
    case VS_RUNNING:    run(v, in); break;
    case VS_DONE:       break;
    }
}
