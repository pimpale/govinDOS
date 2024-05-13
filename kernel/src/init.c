#include "stdint.h"

#include "vga.h"
#include "efi/efi.h"

efi_status_t efi_main(
	efi_handle_t handle,
	struct efi_system_table *system)
{
  while(true) {
      system->out->output_string(system->out, L"hello");
  }

  return EFI_SUCCESS;
}
