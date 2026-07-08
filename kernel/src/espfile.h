#ifndef espfile_h_INCLUDED
#define espfile_h_INCLUDED

#include <stdint.h>

#include <efi/efi.h>
#include <efi/types.h>

// Read a whole file from the volume the kernel image was loaded from (the
// ESP). Boot services only — must be called before exit_boot_services.
//
// The returned buffer is EFI_LOADER_DATA: the allocator's non-conventional
// pass marks it unusable, so it survives exit_boot_services and is never
// handed out by the buddy — the same deal as the kernel image itself.
// The pages are gone for good; callers that only need the contents
// transiently accept that (init.exe is small, and the ESP read happens
// once per boot).
//
// `path` is a UTF-16 absolute path with backslash separators (u"\\boot\\...").
// Returns the buffer and writes its size to *len_out, or nullptr with the
// failing EFI status logged.
void *esp_read_file(efi_handle_t image_handle,
                    struct efi_system_table *system,
                    const efi_char16_t *path, uint64_t *len_out);

#endif // espfile_h_INCLUDED
