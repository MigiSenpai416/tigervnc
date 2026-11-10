#!/bin/bash
# Run the full test suite (X11 + Wayland mode) against a portable
# TigerVNC tarball, on a clean distro runtime image.
#
#   contrib/wayland-mode/test-tarball.sh TARBALL BASE_IMAGE [artifact-dir]
#
# BASE_IMAGE: ubuntu:26.04 or ubuntu:18.04 (the test tool lists below are
# per distro). The image gets only the distro's test tools and the tarball
# extracted to /; nothing is built in it.
# Environment: DOCKER (default "sudo docker"), PROXY (optional http proxy,
# e.g. http://127.0.0.1:8791), SKIP_SYSTEMD=1 to skip the systemd test.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TARBALL=${1:?usage: $0 TARBALL BASE_IMAGE [artifact-dir]}
BASE=${2:?usage: $0 TARBALL BASE_IMAGE [artifact-dir]}
TAG=${BASE##*:}
ART=${3:-$PWD/tarball-test-artifacts-$TAG}
DOCKER=${DOCKER:-sudo docker}
IMAGE=tigervnc-tarball-test:$TAG

COMMON="python3 xdotool x11-utils x11-apps x11-xserver-utils xterm xclip xauth xinit
  weston openssl libpam-modules libpam-pwquality dbus dbus-x11 sudo procps
  psmisc fonts-dejavu-core xfonts-base libgl1 libgl1-mesa-dri strace less
  ca-certificates"
case "$BASE" in
  ubuntu:26.04)
    PKGS="$COMMON python3-pil foot sway labwc wlr-randr wayland-utils wl-clipboard"
    EXTRA=(--build-arg NESTED_WLROOTS_FIX=1) ;;
  ubuntu:18.04)
    # no foot/sway/labwc/wlr-randr/wayland-info/wl-clipboard in 18.04; its
    # python 3.6 is too old for the tests -> python3.8 + Pillow from PyPI
    PKGS="$COMMON python3.8 python3.8-venv"
    EXTRA=(--build-arg TEST_PYTHON=python3.8 --build-arg PIP_PACKAGES=pillow) ;;
  *) echo "unsupported BASE_IMAGE $BASE (ubuntu:26.04 or ubuntu:18.04)" >&2; exit 2 ;;
esac

PROXY_ARGS=()
if [ -n "${PROXY:-}" ]; then
  PROXY_ARGS=(--network host --build-arg "http_proxy=$PROXY"
              --build-arg "https_proxy=$PROXY")
fi

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cp "$TARBALL" "$CTX/tigervnc.tar.gz"
cp -R "$HERE/patches" "$CTX/wlroots-patches"

mkdir -p "$ART"
chmod 777 "$ART"

echo "=== runtime image $IMAGE ($BASE + test tools + $(basename "$TARBALL"))"
$DOCKER build "${PROXY_ARGS[@]}" "${EXTRA[@]}" --build-arg "BASE_IMAGE=$BASE" \
  --build-arg "TEST_PACKAGES=$(echo $PKGS)" \
  -f "$HERE/Dockerfile.tarball-test" -t "$IMAGE" "$CTX"

RUN=($DOCKER run --rm -v "$HERE/tests:/tests:ro" -v "$ART:/artifacts")
rc=0
echo "=== what the image provides"
"${RUN[@]}" "$IMAGE" sh -c '. /etc/os-release; echo "$PRETTY_NAME, $(ldd --version | head -1)";
  ls -l /usr/bin/Xvnc /usr/libexec/Xvnc-wayland /usr/libexec/tigervnc/Xwayland;
  echo "distro Xwayland: $(command -v Xwayland || echo none)";
  /usr/share/tigervnc/check-runtime.sh' | tee "$ART/runtime-image.txt" || rc=1
"${RUN[@]}" --user vncuser -e HOME=/home/vncuser "$IMAGE" \
  python3 /tests/test_x11.py || rc=1
"${RUN[@]}" --user vncuser -e HOME=/home/vncuser "$IMAGE" \
  python3 /tests/test_wayland.py || rc=1
"${RUN[@]}" --user vncuser -e HOME=/home/vncuser "$IMAGE" \
  python3 /tests/test_nested.py || rc=1
"${RUN[@]}" "$IMAGE" python3 /tests/test_vncsession.py || rc=1

if [ "${SKIP_SYSTEMD:-0}" != 1 ]; then
  echo "=== systemd image + test (privileged container)"
  $DOCKER build "${PROXY_ARGS[@]}" --build-arg "TEST_IMAGE=$IMAGE" \
    -f "$HERE/Dockerfile.systemd" -t "tigervnc-tarball-systemd:$TAG" "$HERE"
  $DOCKER rm -f tvnc-tarball-systemd >/dev/null 2>&1 || true
  $DOCKER run -d --name tvnc-tarball-systemd --privileged --cgroupns=host \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw -v "$HERE/tests:/tests:ro" \
    -v "$ART:/artifacts" "tigervnc-tarball-systemd:$TAG" >/dev/null
  sleep 8
  $DOCKER exec tvnc-tarball-systemd python3 /tests/test_systemd.py || rc=1
  $DOCKER rm -f tvnc-tarball-systemd >/dev/null
fi

python3 "$HERE/tests/summarize.py" "$ART" > "$ART/SUMMARY.md" || true
exit $rc
