#include "vga.h"

void vga_fill_screen(uint16_t v) {
    for(uint32_t i = 0; i < VGA_BUF_LEN; i++) {
        VGA_BUF[i] = v;
    }
}
