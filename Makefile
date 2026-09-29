CC      ?= cc
OPT     ?= -O2 -g
CFLAGS  += $(OPT) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Iinclude
LDLIBS  += -pthread

BUILD   ?= build
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=$(BUILD)/obj/%.o)
LIB     := $(BUILD)/libck.a
TESTS   := $(BUILD)/test_queues $(BUILD)/test_sync
BENCH   := $(BUILD)/bench

.PHONY: all test bench asan tsan clean

all: $(LIB) $(TESTS) $(BENCH)

$(BUILD)/obj/%.o: src/%.c $(wildcard include/ck/*.h) | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD)/test_%: tests/test_%.c tests/test.h $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS) $(LDLIBS)

$(BENCH): bench/bench.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD)/obj:
	mkdir -p $@

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

bench: $(BENCH)
	./$(BENCH)

# Sanitizer builds go to their own directories so they never mix objects.
asan:
	$(MAKE) test BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	    LDFLAGS="-fsanitize=address,undefined"

tsan:
	$(MAKE) test BUILD=build-tsan OPT="-O1 -g -fsanitize=thread" LDFLAGS="-fsanitize=thread"

clean:
	rm -rf build build-asan build-tsan
