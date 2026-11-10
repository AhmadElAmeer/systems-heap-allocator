CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L
SAN     = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

BUILD = build
HEAP_SRC = src/heap.c

.PHONY: all test test-san bench traces clean

all: $(BUILD)/test_heap $(BUILD)/trace_runner

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_heap: tests/test_heap.c $(HEAP_SRC) include/heap.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_heap.c $(HEAP_SRC) -o $@

$(BUILD)/test_heap_san: tests/test_heap.c $(HEAP_SRC) include/heap.h | $(BUILD)
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra $(SAN) tests/test_heap.c $(HEAP_SRC) -o $@

$(BUILD)/trace_runner: bench/trace_runner.c $(HEAP_SRC) include/heap.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) bench/trace_runner.c $(HEAP_SRC) -o $@

test: $(BUILD)/test_heap
	./$(BUILD)/test_heap

# Same tests under AddressSanitizer + UndefinedBehaviorSanitizer.
test-san: $(BUILD)/test_heap_san
	./$(BUILD)/test_heap_san

bench: $(BUILD)/trace_runner
	./$(BUILD)/trace_runner -s 1048576 traces/*.trace

traces:
	python3 scripts/gen_traces.py traces

clean:
	rm -rf $(BUILD)
