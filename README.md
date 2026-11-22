# wup-tetris

Tetris core + multiplayer for the Wii U GamePad homebrew SDK (libwup).
Plain C11, no dependencies beyond `string.h`. Tested headless (`make test`) and in a desktop terminal (`make play`).

## Modes
| Mode | Rules |
|---|---|
| Marathon | 150 lines, level up every 10 |
| Sprint | 40 lines, best time |
| Ultra | 2 minutes, best score |
| Endless | survive, level up every 10 |
| Cheese Race | clear 10 garbage rows |
| **Versus Marathon** | both players race to 150 lines, same piece order, no garbage; fewer frames wins |
| **Versus Attack** | same pieces; clears send garbage (cancels incoming first); last one standing |

Guideline features: SRS + kicks, 7-bag, hold, ghost, lock delay with 15 resets, DAS/ARR (`g.das`, `g.arr`), T-spins (+mini), back-to-back, combos, perfect clear.

## Files
- `tetris.h/.c` - game core. `tt_init`, then call `tt_step(&g, input_mask)` 60x/second. Read `g.board`, `g.cur`, `tt_ghost`, `tt_next`, `g.hold`, `g.last` (clear popup) to draw.
- `netplay.h/.c` - 2-player session over any datagram transport. Fill a `NetTransport {send, recv, ctx}` (UDP on lwIP), then `vs_host()` or `vs_join()`, and call `vs_step(&v, inputs)` each frame. Draw `v.me` and `v.opp` (opponent board snapshot). `v.result` becomes `VS_WIN/LOSE/DRAW`.
- `test.c` - rotation, bag, scoring, T-spin, garbage, handshake, 30% packet loss, both versus modes.
- `play_term.c` - desktop single-player front end.

## Netplay design
Each side simulates only its own board. Every 4 frames a 121-byte STATE packet carries frame, score, lines, status, a nibble-packed board, and a **cumulative** garbage-sent counter, so a dropped packet never loses garbage. Handshake: JOIN -> HELLO(mode, seed) -> READY -> START, all retransmitted every 30 frames.

## GamePad app (`app/`)
Built against libwup: direct RGB565 drawing (`gfx.h`), `wup_buttons()` and `wup_touch()`, UDP sockets over the menu's Wi-Fi, `best.dat` in the app's folder for saved bests. 60 Hz fixed timestep with catch-up.

Controls: Left/Right move, Down soft drop, Up or ZR hard drop, A or R rotate, B or Y rotate back, X/L/ZL hold, + pause (single) or forfeit prompt (versus). Menu: Up/Down, A start, B exit, or tap a row.

Versus: one player picks *Versus Marathon (host)* or *Versus Attack (host)*, the other picks *Join a Game*. Both pads must be on the same Wi-Fi network (join it in the Homebrew Menu first). The joiner finds the host by LAN broadcast on UDP port 28901, so no addresses to type. A lost connection ends the match after 10 s.

## Build and run
```
# from a checkout of ravensclawx/wup-homebrew (needs clang, ld.lld, python3+Pillow, meson, ninja)
python3 libwup/tools/wupbuild.py /path/to/wup-tetris/app        # -> build/apps/tetris.wup
python3 libwup/tools/wuplink.py build/apps/tetris.wup           # press Y in the Homebrew Menu first
python3 libwup/tools/wuplog.py                                  # printf/log output
```
Or copy `tetris.wup` to `/apps/tetris/tetris.wup` over the menu's FTP File Sharing.

Desktop: `make test` (core + netplay tests), `make play` (terminal player, single-player modes).

## Not verified
Compiles and links with libwup, and the core and netplay logic are tested on a desktop. It has not been run on a real GamePad or in wup-emu (that needs your own pad's firmware dump), so frame rate, button feel, and Wi-Fi broadcast discovery are untested on hardware.
