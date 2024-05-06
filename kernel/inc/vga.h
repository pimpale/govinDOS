#ifndef vga_h_INCLUDED
#define vga_h_INCLUDED

#include <stdint.h>

// Declare VGA color consts 
// Each constant is a 16 bit value
#define VGA_COLOR_BLACK_BG         ((uint16_t)(0x00 << 0x0c))
#define VGA_COLOR_BLUE_BG          ((uint16_t)(0x01 << 0x0c))
#define VGA_COLOR_GREEN_BG         ((uint16_t)(0x02 << 0x0c))
#define VGA_COLOR_CYAN_BG          ((uint16_t)(0x03 << 0x0c))
#define VGA_COLOR_RED_BG           ((uint16_t)(0x04 << 0x0c))
#define VGA_COLOR_MAGENTA_BG       ((uint16_t)(0x05 << 0x0c))
#define VGA_COLOR_BROWN_BG         ((uint16_t)(0x06 << 0x0c))
#define VGA_COLOR_LIGHT_GREY_BG    ((uint16_t)(0x07 << 0x0c))
#define VGA_COLOR_DARK_GREY_BG     ((uint16_t)(0x08 << 0x0c))
#define VGA_COLOR_LIGHT_BLUE_BG    ((uint16_t)(0x09 << 0x0c))
#define VGA_COLOR_LIGHT_GREEN_BG   ((uint16_t)(0x0a << 0x0c))
#define VGA_COLOR_LIGHT_CYAN_BG    ((uint16_t)(0x0b << 0x0c))
#define VGA_COLOR_LIGHT_RED_BG     ((uint16_t)(0x0c << 0x0c))
#define VGA_COLOR_LIGHT_MAGENTA_BG ((uint16_t)(0x0d << 0x0c))
#define VGA_COLOR_LIGHT_BROWN_BG   ((uint16_t)(0x0e << 0x0c))
#define VGA_COLOR_WHITE_BG         ((uint16_t)(0x0f << 0x0c))

#define VGA_COLOR_BLACK_FG           ((uint16_t)(0x00 << 0x08))
#define VGA_COLOR_BLUE_FG            ((uint16_t)(0x01 << 0x08))
#define VGA_COLOR_GREEN_FG           ((uint16_t)(0x02 << 0x08))
#define VGA_COLOR_CYAN_FG            ((uint16_t)(0x03 << 0x08))
#define VGA_COLOR_RED_FG             ((uint16_t)(0x04 << 0x08))
#define VGA_COLOR_MAGENTA_FG         ((uint16_t)(0x05 << 0x08))
#define VGA_COLOR_BROWN_FG           ((uint16_t)(0x06 << 0x08))
#define VGA_COLOR_LIGHT_GREY_FG      ((uint16_t)(0x07 << 0x08))
#define VGA_COLOR_DARK_GREY_FG       ((uint16_t)(0x08 << 0x08))
#define VGA_COLOR_LIGHT_BLUE_FG      ((uint16_t)(0x09 << 0x08))
#define VGA_COLOR_LIGHT_GREEN_FG     ((uint16_t)(0x0a << 0x08))
#define VGA_COLOR_LIGHT_CYAN_FG      ((uint16_t)(0x0b << 0x08))
#define VGA_COLOR_LIGHT_RED_FG       ((uint16_t)(0x0c << 0x08))
#define VGA_COLOR_LIGHT_MAGENTA_FG   ((uint16_t)(0x0d << 0x08))
#define VGA_COLOR_LIGHT_BROWN_FG     ((uint16_t)(0x0e << 0x08))
#define VGA_COLOR_WHITE_FG           ((uint16_t)(0x0f << 0x08))

// Dimensions and properties of buffer
#define VGA_XSIZE 80
#define VGA_YSIZE 25
#define VGA_BUF ((uint16_t*)0xb8000)
#define VGA_BUF_LEN (VGA_XSIZE * VGA_YSIZE * 2)
#define VGA_BUF_END VGA_BUF + VGA_BUF_LEN



void vga_fill_screen(uint16_t v);

#endif // vga_h_INCLUDED
