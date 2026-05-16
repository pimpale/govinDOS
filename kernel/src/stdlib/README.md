# Kernel Space Implementation of C Standard Library

Not all functions are included!

Serial must be turned on before approximately any of these functions can be used.
This is because it is the main way to panic. Without it, it'll just crash silently.

If the allocator is not yet set up, functions that need to allocate will panic.
