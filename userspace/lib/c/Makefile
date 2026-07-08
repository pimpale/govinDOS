U := ../..
include $(U)/build.mk

all: out/c.a

out/c.a: $(patsubst %.c,out/%.c.o,$(wildcard *.c))
	$(AR) rcs $@ $^
