# Hosted self-tests for kernel components. Each subdirectory is one suite
# with its own Makefile providing test/sanitize/thread-sanitize/clean; these
# targets fan out to every suite. Never built into the kernel image.

SUITES := $(patsubst %/,%,$(dir $(wildcard */Makefile)))

.PHONY: all test sanitize thread-sanitize clean $(SUITES)

all test sanitize thread-sanitize clean:
	@for suite in $(SUITES); do \
		$(MAKE) -C $$suite $@ || exit 1; \
	done
