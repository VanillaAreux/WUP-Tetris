/* wup-tetris: platform-independent Tetris core (no libc beyond string.h).
 * Fixed 60 Hz step. Guideline-style: SRS, 7-bag, hold, ghost, lock delay,
 * T-spins, back-to-back, combos, garbage. */
#ifndef TETRIS_H
#define TETRIS_H
#include <stdint.h>
#include <stdbool.h>

#define TT_W        10
#define TT_H        24          /* rows 0-3 are the hidden spawn zone */
#define TT_HIDDEN   4
#define TT_VIS      (TT_H - TT_HIDDEN)
#define TT_QUEUE_MAX 14

enum { P_I, P_O, P_T, P_S, P_Z, P_J, P_L, P_COUNT };
#define CELL_GARBAGE 8          /* board cells: 0 empty, 1..7 piece+1, 8 garbage */

enum {
    IN_LEFT = 1 << 0, IN_RIGHT = 1 << 1, IN_SOFT = 1 << 2, IN_HARD = 1 << 3,
    IN_CW   = 1 << 4, IN_CCW   = 1 << 5, IN_HOLD = 1 << 6
};

typedef enum {
    MODE_MARATHON,   /* 150 lines, level up every 10 */
    MODE_SPRINT,     /* 40 lines, fastest time */
    MODE_ULTRA,      /* 2 minutes, highest score */
    MODE_ENDLESS,    /* no goal, survive */
    MODE_CHEESE,     /* dig through 10 garbage rows */
    MODE_VS_RACE,    /* multiplayer marathon: first to the goal, no garbage */
    MODE_VS_ATTACK,  /* multiplayer: clears send garbage, last one standing */
    MODE_COUNT
} GameMode;

typedef enum { ST_COUNTDOWN, ST_PLAYING, ST_WON, ST_LOST } GameState;

typedef struct {
    GameMode mode;
    int  goal_lines;    /* 0 = none */
    int  time_limit;    /* frames, 0 = none */
    int  start_level;
    bool level_up;
    bool garbage_on;
    int  cheese_rows;
} ModeCfg;

typedef struct { int type, rot, x, y; } Piece;

typedef struct {            /* last line clear, for UI popups */
    int  lines;
    bool tspin, mini, b2b, perfect;
    int  combo, attack, score, age;
} ClearInfo;

typedef struct {
    ModeCfg   cfg;
    GameState state;
    uint8_t   board[TT_H][TT_W];
    Piece     cur;
    int       hold;  bool hold_used;
    int       queue[TT_QUEUE_MAX]; int qlen;
    uint32_t  rng, grng;
    int       countdown;
    uint32_t  score, frames;
    int       lines, level, combo, pieces;
    bool      b2b;
    float     grav_acc;
    int       lock_timer, lock_resets, lowest_y;
    int       das_dir, das_timer, das, arr;   /* frames; tweakable */
    uint8_t   prev_in;
    bool      last_rot; int last_kick;
    int       pending;        /* incoming garbage not yet on the board */
    uint16_t  out_total;      /* cumulative garbage this player has sent */
    ClearInfo last;
} Game;

void  tt_cfg_for_mode(ModeCfg *c, GameMode m);
void  tt_init(Game *g, const ModeCfg *c, uint32_t seed);
void  tt_step(Game *g, uint8_t inputs);        /* call at 60 Hz */
void  tt_receive_garbage(Game *g, int n);
bool  tt_collides(const Game *g, Piece p);
Piece tt_ghost(const Game *g);
void  tt_piece_cells(int type, int rot, int out[4][2]);
int   tt_next(const Game *g, int i);           /* preview, i in 0..4 */
const char *tt_mode_name(GameMode m);

#endif
