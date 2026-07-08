#include "espfile.h"

#include <efi/loaded_image_protocol.h>
#include <efi/simple_file_system_protocol.h>

#include "stdlib/stdio.h"

void *esp_read_file(efi_handle_t image_handle,
                    struct efi_system_table *system,
                    const efi_char16_t *path, uint64_t *len_out) {
  struct efi_boot_services *bs = system->boot;

  // Our own loaded image -> the device we were loaded from -> its
  // filesystem. handle_protocol is the pre-1.1 form of open_protocol;
  // fine for a boot-time, never-closed use.
  struct efi_guid li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  struct efi_loaded_image_protocol *li = nullptr;
  efi_status_t status = bs->handle_protocol(image_handle, &li_guid,
                                            (void **)&li);
  if (status != EFI_SUCCESS) {
    printf("espfile: no loaded-image protocol (status=%016llX)\n",
           (uint64_t)status);
    return nullptr;
  }

  struct efi_guid sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  struct efi_simple_file_system_protocol *sfs = nullptr;
  status = bs->handle_protocol(li->device_handle, &sfs_guid, (void **)&sfs);
  if (status != EFI_SUCCESS) {
    printf("espfile: boot device has no filesystem (status=%016llX)\n",
           (uint64_t)status);
    return nullptr;
  }

  struct efi_file_protocol *root = nullptr;
  status = sfs->open_volume(sfs, &root);
  if (status != EFI_SUCCESS) {
    printf("espfile: open_volume failed (status=%016llX)\n",
           (uint64_t)status);
    return nullptr;
  }

  struct efi_file_protocol *file = nullptr;
  status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
  if (status != EFI_SUCCESS) {
    root->close(root);
    printf("espfile: open failed (status=%016llX)\n", (uint64_t)status);
    return nullptr;
  }

  // Size via EFI_FILE_INFO. The info struct carries a variable-length
  // name, so ask for the size first (BUFFER_TOO_SMALL fills it in).
  struct efi_guid info_guid = EFI_FILE_INFO_GUID;
  efi_uint_t info_size = 0;
  status = file->get_info(file, &info_guid, &info_size, nullptr);
  if (status != EFI_BUFFER_TOO_SMALL) {
    goto fail;
  }
  struct efi_file_info *info = nullptr;
  status = bs->allocate_pool(EFI_LOADER_DATA, info_size, (void **)&info);
  if (status != EFI_SUCCESS) {
    goto fail;
  }
  status = file->get_info(file, &info_guid, &info_size, info);
  if (status != EFI_SUCCESS) {
    bs->free_pool(info);
    goto fail;
  }
  uint64_t file_size = info->file_size;
  bs->free_pool(info);

  void *buf = nullptr;
  status = bs->allocate_pool(EFI_LOADER_DATA, file_size, &buf);
  if (status != EFI_SUCCESS) {
    goto fail;
  }
  efi_uint_t read_size = file_size;
  status = file->read(file, &read_size, buf);
  if (status != EFI_SUCCESS || read_size != file_size) {
    bs->free_pool(buf);
    goto fail;
  }

  file->close(file);
  root->close(root);
  *len_out = file_size;
  return buf;

fail:
  file->close(file);
  root->close(root);
  printf("espfile: read failed (status=%016llX)\n", (uint64_t)status);
  return nullptr;
}
