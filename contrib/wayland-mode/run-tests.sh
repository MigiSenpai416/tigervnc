#!/bin/bash
# Build everything and run the full test suite in Docker.
#
#   contrib/wayland-mode/run-tests.sh [build-dir] [artifact-dir]
#
# Environment: DOCKER (default "sudo docker"), PROXY (optional http proxy,
# passed as http_proxy/https_proxy build args, e.g. http://127.0.0.1:8791),
# SKIP_SYSTEMD=1 to skip the privileged systemd test.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
BUILD=${1:-$SRC/../tigervnc-wl-build}
ART=${2:-$SRC/test-artifacts}
DOCKER=${DOCKER:-sudo docker}
PROXY_ARGS=()
if [ -n "${PROXY:-}" ]; then
  PROXY_ARGS=(--network host --build-arg "http_proxy=$PROXY"
              --build-arg "https_proxy=$PROXY")
fi

mkdir -p "$BUILD" "$ART"
chmod 777 "$ART"

echo "=== build environment image"
$DOCKER build "${PROXY_ARGS[@]}" -f "$HERE/Dockerfile.buildenv" \
  -t tigervnc-wl-buildenv:26.04 "$HERE"

echo "=== build (cmake + Xvnc)"
$DOCKER run --rm --user "$(id -u):$(id -g)" -v "$SRC:/src:ro" \
  -v "$BUILD:/build" tigervnc-wl-buildenv:26.04 /src/contrib/wayland-mode/build.sh

echo "=== test image"
$DOCKER build "${PROXY_ARGS[@]}" -f "$HERE/Dockerfile.test" \
  -t tigervnc-wl-test "$BUILD"

RUN=($DOCKER run --rm -v "$HERE/tests:/tests:ro" -v "$ART:/artifacts")
rc=0
"${RUN[@]}" --user vncuser -e HOME=/home/vncuser tigervnc-wl-test \
  python3 /tests/test_x11.py || rc=1
"${RUN[@]}" --user vncuser -e HOME=/home/vncuser tigervnc-wl-test \
  python3 /tests/test_wayland.py || rc=1
"${RUN[@]}" tigervnc-wl-test python3 /tests/test_vncsession.py || rc=1

if [ "${SKIP_SYSTEMD:-0}" != 1 ]; then
  echo "=== systemd image + test (privileged container)"
  $DOCKER build "${PROXY_ARGS[@]}" -f "$HERE/Dockerfile.systemd" \
    -t tigervnc-wl-systemd "$HERE"
  $DOCKER rm -f tvnc-systemd >/dev/null 2>&1 || true
  $DOCKER run -d --name tvnc-systemd --privileged --cgroupns=host \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw -v "$HERE/tests:/tests:ro" \
    -v "$ART:/artifacts" tigervnc-wl-systemd >/dev/null
  sleep 8
  $DOCKER exec tvnc-systemd python3 /tests/test_systemd.py || rc=1
  $DOCKER rm -f tvnc-systemd >/dev/null
fi

python3 "$HERE/tests/summarize.py" "$ART" > "$ART/SUMMARY.md"
cat "$ART/SUMMARY.md"
exit $rc
