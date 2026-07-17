# External ports

Each child directory is an abuild recipe for third-party software. Keep only
the `APKBUILD`, patches, and other packaging inputs here. Upstream source must
be declared in `source=`, checksum-verified by abuild, and unpacked into its
generated build directory; do not vendor it into this tree.

Port packages are published to a separate signed APK repository so image and
sysroot construction can enable or omit third-party software independently of
the first-party `packages/` repository.

Run `make -C ports` to schedule the port APKBUILDs with `buildrepo` and publish
`out/repository/ports/x86_64-govindos/APKINDEX.tar.gz`.
