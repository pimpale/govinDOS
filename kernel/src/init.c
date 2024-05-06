#include "stdint.h"

#include "vga.h"

void main() {
  vga_fill_screen('u' | VGA_COLOR_BLACK_BG | VGA_COLOR_BLUE_FG);
}

[[noreturn]] 
void init() {
  main();

  asm("hlt");

  while(true);
}

