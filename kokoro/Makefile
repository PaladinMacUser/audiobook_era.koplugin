# Kokoro-family runtime: host build and gates.
#
#   make test      build and run the text-half golden gate
#   make clean
#
# The blobs and fixtures come from the tools (see kokoro/README.md):
#   py.sh tools/export_kokoro_blobs.py --text ... --dec ... --out model/blobs
#   py.sh tools/dump_stage_bin.py --goldens goldens/paradee-8m --out test/fixtures/paradee-8m
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Iinclude
LDLIBS ?= -lm

SRC = src/kok_meta.c src/kok_fft.c src/kok_kernels.c src/kok_istft.c src/kok_rng.c src/kok_source.c src/kok_text.c src/kok_dec.c
# the on-device G2P is the plugin's espeak-free misaki lexicon port, reused
# rather than duplicated (3 MB of const tables live in sanotts/)
G2P = ../sanotts/nano_lex_g2p.c ../sanotts/nano_lex_tables.c
# kok_text_to_ids lives in the G2P module; the engine sources come with it
ENGINE_SRC = $(SRC) src/kok_g2p.c $(G2P)
BUILD = build

all: $(BUILD)/text_golden_test $(BUILD)/kok_tts

$(BUILD)/kok_tts: src/kok_tts.c $(SRC) include/*.h ../sanotts/nano_lex_g2p.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I../sanotts -o $@ src/kok_tts.c $(ENGINE_SRC) $(LDLIBS)

$(BUILD)/text_golden_test: test/text_golden_test.c $(SRC) include/*.h ../sanotts/nano_lex_g2p.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/text_golden_test.c $(SRC) $(LDLIBS)

test: $(BUILD)/text_golden_test
	./$(BUILD)/text_golden_test

clean:
	rm -rf $(BUILD)

.PHONY: all test clean