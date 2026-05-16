#include "panic.h"

#include "serial.h"

[[noreturn]]
void panic() {
  // Signal QEMU's isa-debug-exit device to terminate with a non-zero status.
  // Exit code observed by the host shell is (value << 1) | 1, so 0x10 -> 0x21.
  outl(0xf4, 0x10);
  while (true) {
  }
}
