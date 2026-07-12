U := ../..
ULIBS := sys c
include $(U)/build.mk

all: out/nvmed.exe

out/nvmed.exe: $(patsubst %.c,out/%.c.o,$(wildcard *.c)) $(LIBS)
	$(LD) $(LDFLAGS) -out:$@ $^
