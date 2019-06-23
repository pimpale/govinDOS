# GovinDOS Executable Format (GEF)

```C
struct ExecutableHeader {
  uint32_t gef_magic; /* Must be 0xDEADCAFE */
  uint32_t section_count; /* greater than 0 */
}

struct ExecutableHeaderSection {
  uint32_t file_offset /* Offset in file from header end */
  uint32_t mem_offset; /* Offset in memory from header end */
  uint32_t executable; /* Should the code in which this is placed be executable? */
  uint32_t modifiable; /* Should the code in which this is placed be modifiable */
}
```

Each executable header shall be followed by section_count number of executable header sections.


