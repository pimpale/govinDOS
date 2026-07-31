# Self-tests for kernel components. Each subdirectory with its own Makefile
# is one hosted suite providing test/sanitize/thread-sanitize/clean; these
# targets fan out to every suite, and hosted suites are never built into the
# kernel image. The exception is boot/ (no Makefile): boot-time in-kernel
# selftests that kernel/Makefile compiles into the image.

SUITES := $(patsubst %/,%,$(dir $(wildcard */Makefile)))

.PHONY: all test sanitize thread-sanitize clean $(SUITES)

all test sanitize thread-sanitize clean:
	@for suite in $(SUITES); do \
		$(MAKE) -C $$suite $@ || exit 1; \
	done
