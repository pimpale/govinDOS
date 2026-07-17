#!/bin/sh
# Route abuild's apk operations away from the Arch host root. Calls carrying an
# explicit --root are target dependency operations; unrooted add/info/del calls
# operate on the disposable host package database initialized by buildrepo.

set -eu

: "${GDOS_HOST_BUILD_ROOT:?}"
: "${GDOS_APK_KEYS_DIR:?}"

has_root=false
for arg do
	case "$arg" in
		--root|--root=*) has_root=true ;;
	esac
done

if "$has_root"; then
	if [ "${1:-}" = add ]; then
		exec apk --keys-dir "$GDOS_APK_KEYS_DIR" "$@" --usermode
	fi
	exec apk "$@"
fi

case "${1:-}" in
add)
	exec apk --root "$GDOS_HOST_BUILD_ROOT" --arch x86_64 \
		--keys-dir "$GDOS_APK_KEYS_DIR" "$@" --usermode
	;;
info|del)
	exec apk --root "$GDOS_HOST_BUILD_ROOT" --arch x86_64 "$@"
	;;
*)
	exec apk "$@"
	;;
esac
