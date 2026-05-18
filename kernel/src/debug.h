#ifndef debug_h_INCLUDED
#define debug_h_INCLUDED

[[noreturn]] void fatal(char *message);
void asserts(bool h, char *message);

#endif // debug_h_INCLUDED
