U := ../..
include $(U)/build.mk

all: out/tests.exe

out/tests.exe: $(patsubst %.c,out/%.c.o,$(wildcard *.c)) $(LIBS)
	$(LD) $(LDFLAGS) -out:$@ $^
