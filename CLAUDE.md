# GovindOS

- Put all kernel tests in `kernel/selftest/` — hosted suites as
  subdirectories with their own Makefile, boot-time in-kernel selftests in
  `kernel/selftest/boot/`. No test code in `kernel/src/` or
  `kernel/archsrc/`; a selftest that would need production internals
  exposed to move there should be deleted or rewritten against public
  kernel APIs instead.
