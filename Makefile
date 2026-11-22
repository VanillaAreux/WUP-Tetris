CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
test: tetris.c netplay.c test.c
	$(CC) $(CFLAGS) -o test tetris.c netplay.c test.c && ./test
play: tetris.c play_term.c
	$(CC) $(CFLAGS) -o play tetris.c play_term.c
clean:
	rm -f test play
