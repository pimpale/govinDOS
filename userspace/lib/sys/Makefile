U := ../..
include $(U)/build.mk

all: out/sys.a

out/sys.a: $(patsubst %.c,out/%.c.o,$(wildcard *.c))
	$(AR) rcs $@ $^
