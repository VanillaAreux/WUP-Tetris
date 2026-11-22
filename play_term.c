/* Desktop test player (ANSI terminal). Single-player modes only.
 *   ./play [marathon|sprint|ultra|endless|cheese]
 * Keys: arrows or a/d move, s/down soft drop, space hard drop,
 *       x/up rotate cw, z rotate ccw, c hold, q quit.
 * Terminals report no key releases, so held keys are emulated with a short window. */
#define _POSIX_C_SOURCE 200809L
#include "tetris.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>

static struct termios old_tio;
static void restore(void) { tcsetattr(0, TCSANOW, &old_tio); printf("\x1b[?25h\x1b[0m\n"); }

static const int COL[9] = {0, 51, 226, 129, 46, 196, 21, 208, 244};   /* 256-colour: I O T S Z J L, garbage */

static void cell(char **o, int v, bool ghost) {
    if (v) *o += sprintf(*o, "\x1b[48;5;%dm  \x1b[0m", COL[v]);
    else if (ghost) *o += sprintf(*o, "\x1b[90m[]\x1b[0m");
    else *o += sprintf(*o, "\x1b[48;5;234m  \x1b[0m");
}

int main(int argc, char **argv) {
    GameMode m = MODE_MARATHON;
    if (argc > 1) {
        for (int i = 0; i < MODE_VS_RACE; i++) {
            static const char *k[] = {"marathon", "sprint", "ultra", "endless", "cheese"};
            if (!strcmp(argv[1], k[i])) m = (GameMode)i;
        }
    }
    ModeCfg cfg; tt_cfg_for_mode(&cfg, m);
    Game g; tt_init(&g, &cfg, (uint32_t)time(NULL));

    tcgetattr(0, &old_tio); atexit(restore);
    struct termios t = old_tio; t.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &t);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    printf("\x1b[2J\x1b[?25l");

    int hold_until[7] = {0};     /* held-key emulation for L R soft */
    uint8_t pulse = 0; int f = 0;
    for (;; f++) {
        unsigned char ch;
        while (read(0, &ch, 1) == 1) {
            if (ch == 27) { unsigned char s[2]; if (read(0, &s[0], 1) == 1 && read(0, &s[1], 1) == 1 && s[0] == '[')
                ch = s[1] == 'A' ? 'x' : s[1] == 'B' ? 's' : s[1] == 'C' ? 'd' : s[1] == 'D' ? 'a' : 0; }
            switch (ch) {
            case 'a': hold_until[0] = f + 7; break;
            case 'd': hold_until[1] = f + 7; break;
            case 's': hold_until[2] = f + 5; break;
            case ' ': pulse |= IN_HARD; break;
            case 'x': pulse |= IN_CW; break;
            case 'z': pulse |= IN_CCW; break;
            case 'c': pulse |= IN_HOLD; break;
            case 'q': return 0;
            }
        }
        uint8_t in = pulse; pulse = 0;
        if (f < hold_until[0]) in |= IN_LEFT;
        if (f < hold_until[1]) in |= IN_RIGHT;
        if (f < hold_until[2]) in |= IN_SOFT;
        tt_step(&g, in);

        char buf[8192], *o = buf;
        o += sprintf(o, "\x1b[H %s   %s\n\n", tt_mode_name(m),
                     g.state == ST_WON ? "FINISHED!" : g.state == ST_LOST ? "GAME OVER" : g.state == ST_COUNTDOWN ? "get ready" : "");
        Piece gh = tt_ghost(&g);
        int gc[4][2], cc[4][2];
        tt_piece_cells(gh.type, gh.rot, gc); tt_piece_cells(g.cur.type, g.cur.rot, cc);
        for (int y = TT_HIDDEN; y < TT_H; y++) {
            o += sprintf(o, " \x1b[37m|\x1b[0m");
            for (int x = 0; x < TT_W; x++) {
                int v = g.board[y][x]; bool gho = false;
                if (g.state == ST_PLAYING) for (int i = 0; i < 4; i++) {
                    if (gh.x + gc[i][0] == x && gh.y + gc[i][1] == y) gho = true;
                    if (g.cur.x + cc[i][0] == x && g.cur.y + cc[i][1] == y) v = g.cur.type + 1;
                }
                cell(&o, v, gho);
            }
            o += sprintf(o, "\x1b[37m|\x1b[0m");
            int r = y - TT_HIDDEN;
            static const char *N = "IOTSZJL";
            if (r == 0) o += sprintf(o, "  Score %-8u", g.score);
            if (r == 1) o += sprintf(o, "  Lines %-4d%s", g.lines, g.cfg.goal_lines ? "" : "     ");
            if (r == 2) o += sprintf(o, "  Level %-3d   ", g.level);
            if (r == 3) o += sprintf(o, "  Time  %d:%05.2f ", (int)(g.frames / 3600), (g.frames % 3600) / 60.0);
            if (r == 5) o += sprintf(o, "  Hold  %c      ", g.hold >= 0 ? N[g.hold] : '-');
            if (r == 7) { o += sprintf(o, "  Next  "); for (int i = 0; i < 5; i++) *o++ = N[tt_next(&g, i)]; o += sprintf(o, "  "); }
            if (r == 9 && g.last.age < 90 && (g.last.lines || g.last.tspin))
                o += sprintf(o, "  %s%s%d  +%-6d   ", g.last.tspin ? "T-SPIN " : "", g.last.b2b ? "B2B " : "", g.last.lines, g.last.score);
            else if (r == 9) o += sprintf(o, "                        ");
            *o++ = '\n';
        }
        o += sprintf(o, " \x1b[37m+--------------------+\x1b[0m\n");
        fwrite(buf, 1, (size_t)(o - buf), stdout); fflush(stdout);

        struct timespec ts = {0, 16666667}; nanosleep(&ts, NULL);
        if (g.state == ST_WON || g.state == ST_LOST) { sleep(1); printf("\nScore %u, %d lines, %u frames. Press q.\n", g.score, g.lines, g.frames);
            for (;;) { unsigned char c; if (read(0, &c, 1) == 1 && c == 'q') return 0; nanosleep(&(struct timespec){0, 50000000}, NULL); } }
    }
}
