# Host test suite. No hardware, no Zephyr, ~1 second.
#
# The deleted proximity engine's single greatest asset was that it compiled and
# tested on a laptop in about a second (v3 §10). This keeps that property for
# the three places the expensive bugs live: the schedule recurrence logic, the
# §9 integrity gate, and the §4.4 verdict state machine.
#
#   make        build and run
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
           -I../src -include test_config.h
SRC     = ../src/schedule/event.c \
          ../src/schedule/schedule_blob.c \
          ../src/schedule/schedule.c \
          ../src/integrity/classify.c \
          ../src/integrity/integrity.c \
          ../src/enforcement/profiles.c \
          ../src/proximity/proximity.c \
          ../src/proximity/cs_fuse.c
TESTS   = test_main.c

all: run

impulse_tests: $(SRC) $(TESTS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(TESTS)

run: impulse_tests
	./impulse_tests

clean:
	rm -f impulse_tests

.PHONY: all run clean
