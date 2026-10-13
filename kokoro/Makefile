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

SRC = src/kok_meta.c src/kok_kernels.c src/kok_text.c
BUILD = build

all: $(BUILD)/text_golden_test

$(BUILD)/text_golden_test: test/text_golden_test.c $(SRC) include/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/text_golden_test.c $(SRC) $(LDLIBS)

test: $(BUILD)/text_golden_test
	./$(BUILD)/text_golden_test

clean:
	rm -rf $(BUILD)

.PHONY: all test clean