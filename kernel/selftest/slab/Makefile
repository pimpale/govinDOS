CC ?= cc
CFLAGS ?= -std=c17 -O2 -g -Wall -Wextra -Werror -pthread

.PHONY: all test sanitize thread-sanitize clean
all: slab_test

slab_test: slab_test.c ../../vendor/slab/slab.h ../../vendor/slab/slab_impl.h
	$(CC) $(CFLAGS) -I../../vendor -o $@ slab_test.c

test: slab_test
	./slab_test

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" slab_test
	ASAN_OPTIONS=detect_leaks=0 ./slab_test

thread-sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -O1 -DSLAB_TEST_PTHREAD -fsanitize=thread -fno-omit-frame-pointer" slab_test
	TSAN_OPTIONS=halt_on_error=1 ./slab_test

clean:
	rm -f slab_test slab_test_san slab_test_tsan
