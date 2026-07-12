U := ../..
ULIBS := sys c
include $(U)/build.mk

all: out/pcid.exe

out/pcid.exe: $(patsubst %.c,out/%.c.o,$(wildcard *.c)) $(LIBS)
	$(LD) $(LDFLAGS) -out:$@ $^
