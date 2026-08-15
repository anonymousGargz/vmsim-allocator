CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Iinclude
LDFLAGS ?=
BUILD   := build

VM_SRC    := src/vmsim.c src/trace.c src/main_vmsim.c
ALLOC_SRC := src/alloc.c src/main_allocbench.c
TEST_SRC  := src/vmsim.c src/trace.c tests/test_vmsim.c

.PHONY: all clean test asan check bench valgrind

all: $(BUILD)/vmsim $(BUILD)/allocbench $(BUILD)/test_vmsim

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/vmsim: $(VM_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

$(BUILD)/allocbench: $(ALLOC_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_vmsim: $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# Correctness gate: known-answer policy tests + allocator torture test.
test: all
	./$(BUILD)/test_vmsim
	./$(BUILD)/allocbench --test --no-bench --ops 200000

# ASan/UBSan needs no root and catches most of what Valgrind would.
# The allocator is deliberately built WITHOUT ASan's malloc interception issues
# because it never calls malloc for user blocks — only mmap.
asan: | $(BUILD)
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wall -Wextra -Iinclude $(TEST_SRC) -o $(BUILD)/test_vmsim_asan -lm
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wall -Wextra -Iinclude $(ALLOC_SRC) -o $(BUILD)/allocbench_asan
	./$(BUILD)/test_vmsim_asan
	./$(BUILD)/allocbench_asan --test --no-bench --ops 100000

# Valgrind, if it is on PATH (see README for a no-sudo local install).
valgrind: all
	@command -v valgrind >/dev/null 2>&1 || \
	  { echo "valgrind not found — see README for a --prefix=\$$HOME build"; exit 1; }
	valgrind --leak-check=full --error-exitcode=1 ./$(BUILD)/test_vmsim
	valgrind --leak-check=full --error-exitcode=1 \
	         ./$(BUILD)/allocbench --test --no-bench --ops 20000

check: test asan

bench: all
	./$(BUILD)/vmsim --workload ws --pages 1024 --ws 128 --frames 64
	./$(BUILD)/vmsim --sweep-frames 8 --workload ws --pages 1024 --ws 128
	./$(BUILD)/allocbench --ops 200000

clean:
	rm -rf $(BUILD)
