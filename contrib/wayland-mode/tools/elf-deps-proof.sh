#!/bin/bash
# Dependency proof for a portable TigerVNC tarball: readelf over every ELF
# file in it. Lists NEEDED, RPATH/RUNPATH, the interpreter and the highest
# GLIBC symbol version, and fails if any NEEDED library is outside the
# allowed system set (glibc parts, libGL/Mesa, libpam, libsystemd).
#
#   contrib/wayland-mode/tools/elf-deps-proof.sh TARBALL > proof.txt
set -euo pipefail
TARBALL=${1:?usage: $0 TARBALL}
ALLOWED='^(libc|libm|libdl|libpthread|librt|ld-linux-x86-64)\.so\.[0-9]+$|^(libGL\.so\.1|libgbm\.so\.1|libpam\.so\.0|libsystemd\.so\.0)$'
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
tar -xzf "$TARBALL" -C "$tmp"
echo "Tarball: $(basename "$TARBALL")  sha256 $(sha256sum "$TARBALL" | cut -d' ' -f1)"
echo "readelf: $(readelf --version | head -1)"
echo "Allowed system libraries: glibc (libc, libm, libdl, libpthread, librt, ld-linux),"
echo "  libGL.so.1 + libgbm.so.1 (Mesa/GLVND), libpam.so.0, libsystemd.so.0"
echo
bad=0 n=0
while IFS= read -r f; do
  head -c4 "$f" | grep -q ELF || continue
  n=$((n+1)); rel=${f#$tmp/*/}
  needed=$(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
  rpath=$(readelf -d "$f" | grep -E '\((RPATH|RUNPATH)\)' || true)
  interp=$(readelf -l "$f" | sed -n 's/.*\[Requesting program interpreter: \(.*\)\]/\1/p')
  glibc=$(readelf -V "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
  echo "== /$rel"
  echo "   interpreter: ${interp:-none}   max symbol version: ${glibc:-none}   RPATH/RUNPATH: ${rpath:-none}"
  for l in $needed; do
    if [[ $l =~ $ALLOWED ]]; then echo "   NEEDED $l"; else echo "   NEEDED $l   <-- NOT ALLOWED"; bad=1; fi
  done
  [[ -z $rpath ]] || bad=1
done < <(find "$tmp" -type f | sort)
echo
if [[ $bad == 0 ]]; then echo "RESULT: OK, $n ELF files, every NEEDED library is in the allowed system set, no RPATH/RUNPATH"
else echo "RESULT: FAIL"; exit 1; fi
