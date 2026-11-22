#include "netplay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void run_frames(Game *g, int n) { for (int i = 0; i < n; i++) tt_step(g, 0); }
static Game fresh(GameMode m, uint32_t seed) {
    Game g; ModeCfg c; tt_cfg_for_mode(&c, m); tt_init(&g, &c, seed); run_frames(&g, 181); return g;
}

static void test_core(void) {
    /* rotations return to start after four turns, for every piece */
    for (int t = 0; t < P_COUNT; t++) {
        int a[4][2], b[4][2]; tt_piece_cells(t, 0, a); tt_piece_cells(t, 4, b);
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
    /* 7-bag: first 14 pieces are two full permutations */
    Game g = fresh(MODE_MARATHON, 1234);
    CHECK(g.state == ST_PLAYING);
    int seen[7] = {0}; seen[g.cur.type]++;
    for (int i = 0; i < 6; i++) seen[tt_next(&g, i)]++;
    for (int t = 0; t < 7; t++) CHECK(seen[t] == 1);

    /* vertical I into a 4-row well = tetris, 800 points, attack 4 */
    g = fresh(MODE_VS_ATTACK, 5); g.level = 1;
    for (int r = TT_H - 4; r < TT_H; r++) for (int x = 0; x < 9; x++) g.board[r][x] = 1;
    g.board[19][0] = 1;   /* stray block so it is not a perfect clear */
    g.cur = (Piece){ P_I, 1, 7, 16 };
    tt_step(&g, IN_HARD);
    CHECK(g.lines == 4); CHECK(g.score >= 800); CHECK(g.out_total == 4); CHECK(g.b2b);

    /* incoming garbage is cancelled by outgoing attack first */
    g = fresh(MODE_VS_ATTACK, 5); g.level = 1; g.pending = 4;
    for (int r = TT_H - 4; r < TT_H; r++) for (int x = 0; x < 9; x++) g.board[r][x] = 1;
    g.board[19][0] = 1;
    g.cur = (Piece){ P_I, 1, 7, 16 };
    tt_step(&g, IN_HARD);
    CHECK(g.pending == 0); CHECK(g.out_total == 0);

    /* garbage lands after a piece locks with no clear, with one hole per row */
    g = fresh(MODE_VS_ATTACK, 9); tt_receive_garbage(&g, 3);
    tt_step(&g, IN_HARD);
    CHECK(g.pending == 0);
    for (int r = TT_H - 3; r < TT_H; r++) {
        int holes = 0; for (int x = 0; x < TT_W; x++) if (!g.board[r][x]) holes++;
        CHECK(holes >= 1);
    }

    /* T-spin double: T pointing down into a slot with three corners blocked */
    g = fresh(MODE_MARATHON, 3); g.level = 1;
    memset(g.board, 0, sizeof g.board);
    for (int x = 0; x < TT_W; x++) { if (x != 4) g.board[23][x] = 1; if (x < 3 || x > 5) g.board[22][x] = 1; }
    g.board[21][3] = 1;
    g.cur = (Piece){ P_T, 2, 3, 21 };
    g.last_rot = true; g.last_kick = 0;
    tt_step(&g, IN_HARD);
    CHECK(g.last.tspin && !g.last.mini && g.last.lines == 2);
    CHECK(g.score >= 1200);

    /* Sprint finishes at 40 lines, Ultra at 2 minutes */
    g = fresh(MODE_SPRINT, 1); g.lines = 39;
    for (int x = 0; x < 6; x++) g.board[23][x] = 1;
    g.cur = (Piece){ P_I, 0, 6, 21 };
    tt_step(&g, IN_HARD);
    CHECK(g.state == ST_WON);
    g = fresh(MODE_ULTRA, 1); g.frames = 120 * 60 - 1; tt_step(&g, 0);
    CHECK(g.state == ST_WON);

    /* Cheese race starts with 10 garbage rows, and wins when none are left */
    g = fresh(MODE_CHEESE, 1);
    int garb = 0; for (int y = 0; y < TT_H; y++) for (int x = 0; x < TT_W; x++) garb += g.board[y][x] == CELL_GARBAGE;
    CHECK(garb == 90);

    /* hold swaps once per piece; topping out is detected */
    g = fresh(MODE_ENDLESS, 7); int first = g.cur.type;
    tt_step(&g, IN_HOLD); CHECK(g.hold == first);
    int cur2 = g.cur.type; tt_step(&g, 0); tt_step(&g, IN_HOLD); CHECK(g.cur.type == cur2);
    g = fresh(MODE_ENDLESS, 7);
    for (int i = 0; i < 40 && g.state == ST_PLAYING; i++) { tt_step(&g, IN_HARD); tt_step(&g, 0); }
    CHECK(g.state == ST_LOST);
}

/* ---- loopback transport with optional packet loss ---- */
typedef struct { uint8_t d[512][160]; int len[512]; int h, t; } Q;
typedef struct { Q *tx, *rx; int loss; } Port;
static int p_send(void *c, const uint8_t *b, int n) {
    Port *p = c; if (p->loss && rand() % 100 < p->loss) return n;
    Q *q = p->tx; memcpy(q->d[q->t % 512], b, (size_t)n); q->len[q->t % 512] = n; q->t++; return n;
}
static int p_recv(void *c, uint8_t *b, int cap) {
    Port *p = c; Q *q = p->rx; if (q->h == q->t) return 0;
    int n = q->len[q->h % 512]; memcpy(b, q->d[q->h % 512], (size_t)(n < cap ? n : cap)); q->h++; return n;
}

static int chance(int pct) { return rand() % 100 < pct; }
static uint8_t bot(void) {
    uint8_t m = 0;
    if (chance(6)) m |= IN_LEFT;
    if (chance(6)) m |= IN_RIGHT;
    if (chance(4)) m |= IN_CW;
    if (chance(3)) m |= IN_HARD;
    if (chance(10)) m |= IN_SOFT;
    if (chance(1)) m |= IN_HOLD;
    return m;
}

static void test_net(int loss, GameMode mode) {
    static Q ab, ba; memset(&ab, 0, sizeof ab); memset(&ba, 0, sizeof ba);
    Port pa = { &ab, &ba, loss }, pb = { &ba, &ab, loss };
    NetTransport ta = { p_send, p_recv, &pa }, tb = { p_send, p_recv, &pb };
    Versus A, B; ModeCfg cfg; tt_cfg_for_mode(&cfg, mode);
    vs_host(&A, &cfg, 424242, ta); vs_join(&B, tb);

    int f = 0;
    for (; f < 600 && !(A.phase == VS_RUNNING && B.phase == VS_RUNNING); f++) { vs_step(&A, 0); vs_step(&B, 0); }
    CHECK(A.phase == VS_RUNNING && B.phase == VS_RUNNING);
    CHECK(B.cfg.mode == mode && B.seed == 424242);
    CHECK(A.me.queue[0] == B.me.queue[0] && A.me.queue[3] == B.me.queue[3]);   /* same piece order */

    for (int i = 0; i < 200; i++) { vs_step(&A, 0); vs_step(&B, 0); }          /* past the countdown */
    if (mode == MODE_VS_ATTACK) {
        /* cumulative garbage counter survives loss: 7 then 12 */
        A.me.out_total = 7;  for (int i = 0; i < 60; i++) { vs_step(&A, 0); vs_step(&B, 0); }
        CHECK(B.me.pending == 7);
        A.me.out_total = 12; for (int i = 0; i < 60; i++) { vs_step(&A, 0); vs_step(&B, 0); }
        CHECK(B.me.pending == 12 || B.me.pending == 0);  /* 0 only if a piece locked and it landed */
    }

    for (f = 0; f < 60000 && !(A.result != VS_PENDING && B.result != VS_PENDING); f++) {
        vs_step(&A, bot()); vs_step(&B, bot());
    }
    printf("  mode=%s loss=%d%% -> A:%d B:%d after %d frames\n", tt_mode_name(mode), loss, A.result, B.result, f);
    CHECK(A.result != VS_PENDING && B.result != VS_PENDING);
    if (A.result == VS_WIN)  CHECK(B.result == VS_LOSE);
    if (A.result == VS_LOSE) CHECK(B.result == VS_WIN);
}

static void test_race_logic(void) {
    static Q ab, ba; memset(&ab, 0, sizeof ab); memset(&ba, 0, sizeof ba);
    Port pa = { &ab, &ba, 0 }, pb = { &ba, &ab, 0 };
    NetTransport ta = { p_send, p_recv, &pa }, tb = { p_send, p_recv, &pb };
    Versus A, B; ModeCfg cfg; tt_cfg_for_mode(&cfg, MODE_VS_RACE); cfg.goal_lines = 5;
    vs_host(&A, &cfg, 99, ta); vs_join(&B, tb);
    for (int i = 0; i < 400; i++) { vs_step(&A, 0); vs_step(&B, 0); }
    CHECK(B.cfg.goal_lines == 5);
    /* A reaches the goal at frame 3000, B is still playing at frame 3100 -> A wins, B loses */
    A.me.state = ST_WON; A.me.frames = 3000; B.me.frames = 3100;
    for (int i = 0; i < 20; i++) { vs_step(&A, 0); vs_step(&B, 0); }
    CHECK(A.result == VS_WIN); CHECK(B.result == VS_LOSE);
}

int main(void) {
    srand(7);
    test_core();
    test_net(0, MODE_VS_ATTACK);
    test_net(30, MODE_VS_ATTACK);
    test_net(0, MODE_VS_RACE);
    test_net(30, MODE_VS_RACE);
    test_race_logic();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
