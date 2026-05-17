#ifndef interrupts_h_INCLUDED
#define interrupts_h_INCLUDED

// both defined in asm

// only has to be run once (probably by the BSP)
extern void interrupts_fill_idt(void);

// has to be run on each cpu
extern void interrupts_load_idt(void);


#endif // interrupts_h_INCLUDED
