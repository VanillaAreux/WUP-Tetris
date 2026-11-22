/* Two-player netplay over any datagram transport (UDP on lwIP, loopback in tests).
 * Each side simulates only its own board; peers exchange a STATE snapshot every
 * 4 frames. Garbage is sent as a cumulative counter, so lost packets are harmless. */
#ifndef NETPLAY_H
#define NETPLAY_H
#include "tetris.h"

typedef struct {
    int (*send)(void *ctx, const uint8_t *buf, int len);          /* returns <0 on error */
    int (*recv)(void *ctx, uint8_t *buf, int cap);                /* returns 0 if nothing waiting */
    void *ctx;
} NetTransport;

enum { NP_JOIN = 1, NP_HELLO, NP_READY, NP_START, NP_STATE };   /* packet types (byte 3) */

typedef enum { VS_LOBBY, VS_WAIT_READY, VS_WAIT_START, VS_RUNNING, VS_DONE } VsPhase;
typedef enum { VS_PENDING, VS_WIN, VS_LOSE, VS_DRAW } VsResult;

typedef struct {                 /* what you draw for the opponent */
    bool     valid;
    uint16_t seq;
    uint32_t frame, score;
    uint8_t  status;             /* 0 playing, 1 topped out, 2 reached goal */
    uint8_t  pending, level;
    uint16_t garb_total, lines;
    uint8_t  board[TT_VIS][TT_W];/* includes the active piece */
} OppView;

typedef struct {
    bool host; VsPhase phase; VsResult result;
    NetTransport net;
    ModeCfg cfg; uint32_t seed;
    Game me; OppView opp;
    uint16_t seq, garb_applied, last_sent_total;
    uint8_t  last_status;
    int tick, end_timer, grace;
} Versus;

#define NP_HELLO_LEN 14
#define NP_STATE_LEN (4 + 2 + 4 + 1 + 1 + 2 + 2 + 4 + 1 + (TT_VIS * TT_W) / 2)

void vs_host(Versus *v, const ModeCfg *cfg, uint32_t seed, NetTransport net);
void vs_join(Versus *v, NetTransport net);
void vs_step(Versus *v, uint8_t inputs);   /* call at 60 Hz, once per frame */

#endif
