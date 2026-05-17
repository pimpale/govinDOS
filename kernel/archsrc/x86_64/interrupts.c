#include "interrupts.h"

#include <stdint.h>

#include "stdlib/stdio.h"

#define INT_DOUBLE_FAULT 0x00000002
#define INT_GENERAL_PROTECTION 0x0000000D
#define INT_PAGE_FAULT 0x0000000E
#define INT_MACHINE_CHECK 0x00000012
#define INT_SYSCALL 0x00000080

struct [[gnu::packed]] cpu_state {
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t r9;
  uint64_t r8;
  uint64_t rdx;
};

uint64_t interrupt_handler(struct cpu_state cpu, uint64_t vector, uint64_t error, uint64_t rip) {
    printf("vector:%016llX\n", vector);
    printf("error:%016llX\n", error);
    printf("rip:%016llX\n", rip);
    return 0;
}
