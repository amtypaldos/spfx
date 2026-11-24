# SP-1 FX host build: DSP library, simulator and tests. (Firmware build is separate; see docs.)
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter
LDLIBS  = -lm
BUILD   = build

DSP_SRC = $(wildcard firmware/dsp/*.c) $(wildcard firmware/dsp/effects/*.c)
CTL_SRC = $(wildcard firmware/controls/*.c)
APP_SRC = firmware/app/sync_out.c firmware/app/app_logic.c firmware/drivers/ladder.c firmware/drivers/midi_out.c $(wildcard firmware/storage/*.c) $(wildcard firmware/protocol/*.c)
APP_SRC += firmware/audio/usb_audio_core.c firmware/audio/fxprof_core.c
LIB_SRC = $(DSP_SRC) $(CTL_SRC) $(APP_SRC)
LIB_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC))
TESTS   = $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

all: $(BUILD)/sp1fx_sim $(TESTS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD)/sp1fx.a: $(LIB_OBJ)
	ar rcs $@ $^

$(BUILD)/sp1fx_sim: simulator/sim.c simulator/wav.c $(BUILD)/sp1fx.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $^ -o $@ $(LDLIBS)

$(BUILD)/test_%: tests/test_%.c $(BUILD)/sp1fx.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Itests $^ -o $@ $(LDLIBS)

test: $(TESTS)
	@fail=0; for t in $(TESTS); do echo "== $$t"; $$t || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; else echo "ALL TEST BINARIES PASSED"; fi

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d)
.PHONY: all test clean
