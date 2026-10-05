CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g
ASAN_OPTIONS=detect_leaks=0
all: reference_test test_c04
reference_test: reference_core.c reference_test.c
	cat reference_core.c reference_test.c > .combined.c
	$(CC) $(CFLAGS) -DMB_REFERENCE_TEST .combined.c -o $@
test_c04: test_c04_vectors.c ../../transport/bridge_codec.c ../../core/bridge_core.c
	$(CC) $(CFLAGS) test_c04_vectors.c ../../transport/bridge_codec.c ../../core/bridge_core.c -o $@
run: all
	ASAN_OPTIONS=$(ASAN_OPTIONS) ./reference_test
	ASAN_OPTIONS=$(ASAN_OPTIONS) ./test_c04
clean:
	rm -f reference_test test_c04 .combined.c
