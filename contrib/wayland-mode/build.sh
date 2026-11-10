#!/bin/bash
# Build the TigerVNC fork (X11 Xvnc + Wayland mode) inside the build
# environment image (Dockerfile.buildenv).
#
#   SRC   source tree (default /src)
#   BUILD out-of-tree build directory (default /build)
#   STAGE install root (default $BUILD/stage)
#
# Steps follow contrib/packages/deb/ubuntu-resolute/debian/rules:
#   1. cmake build of common/, unix/ (incl. unix/xvnc-wayland) and vncserver
#   2. Xvnc: system Xorg source + unix/xserver21.patch, autotools build
#   3. "make install" of both into $STAGE
set -euo pipefail

SRC=${SRC:-/src}
BUILD=${BUILD:-/build}
STAGE=${STAGE:-$BUILD/stage}
JOBS=${JOBS:-$(nproc)}
XORG_SOURCE_ARCHIVE=${XORG_SOURCE_ARCHIVE:-/usr/src/xorg-server.tar.xz}

mkdir -p "$BUILD"
cd "$BUILD"

# wlroots patches for the test image (Dockerfile.test builds with $BUILD as
# context)
rm -rf "$BUILD/wlroots-patches"
cp -R "$SRC/contrib/wayland-mode/patches" "$BUILD/wlroots-patches"

echo "=== [1/3] CMake build"
cmake -G "Unix Makefiles" "$SRC" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_STATIC=OFF \
  -DENABLE_NLS=ON \
  -DENABLE_H264=ON \
  -DENABLE_GNUTLS=ON \
  -DENABLE_NETTLE=ON \
  -DENABLE_PAM=ON \
  -DENABLE_SYSTEMD=ON \
  -DENABLE_PWQUALITY=ON \
  -DENABLE_WAYLAND=ON \
  -DENABLE_WAYLAND_SERVER=ON \
  -DBUILD_VIEWER=OFF \
  -DCMAKE_INSTALL_PREFIX:PATH=/usr \
  -DCMAKE_INSTALL_SYSCONFDIR:PATH=/etc \
  -DCMAKE_INSTALL_UNITDIR:PATH=/usr/lib/systemd/system
make -j"$JOBS"

echo "=== [2/3] Xvnc (Xorg $(basename "$XORG_SOURCE_ARCHIVE"))"
if [ ! -f "$BUILD/xorg-source-stamp" ]; then
  rm -rf "$BUILD/unix/xserver"
  mkdir -p "$BUILD/unix"
  cp -R "$SRC/unix/xserver" "$BUILD/unix/"
  tar -C "$BUILD/unix/xserver" -axf "$XORG_SOURCE_ARCHIVE" --strip-components=1
  (cd "$BUILD/unix/xserver" && patch -p1 < "$SRC/unix/xserver21.patch")
  touch "$BUILD/xorg-source-stamp"
else
  # keep the TigerVNC part of the X server in sync with the source tree
  cp -R "$SRC/unix/xserver/hw/vnc/." "$BUILD/unix/xserver/hw/vnc/"
fi

if [ ! -f "$BUILD/xorg-config-stamp" ]; then
  (cd "$BUILD/unix/xserver"
   export PIXMANINCDIR=/usr/include/pixman-1
   autoreconf -fi
   ./configure --prefix=/usr \
     --disable-silent-rules --disable-static --without-dtrace \
     --disable-strict-compilation --disable-debug --disable-unit-tests \
     --with-xkb-path=/usr/share/X11/xkb --with-xkb-output=/var/lib/xkb \
     --with-default-xkb-rules=evdev --disable-devel-docs \
     --enable-mitshm --enable-xres --disable-tslib --enable-dbe \
     --disable-xf86bigfont --disable-dpms --disable-config-hal \
     --disable-config-udev --disable-xorg --disable-xquartz --disable-xwin \
     --disable-xfake --disable-install-setuid \
     --with-default-font-path="/usr/share/fonts/X11/misc,/usr/share/fonts/X11/100dpi/:unscaled,/usr/share/fonts/X11/75dpi/:unscaled,/usr/share/fonts/X11/Type1,built-ins" \
     --enable-composite --enable-record --enable-xv --enable-screensaver \
     --enable-xdmcp --enable-xdm-auth-1 --enable-glx \
     --disable-dri --enable-dri2 --enable-dri3 --enable-xinerama \
     --enable-xf86vidmode --enable-xace --disable-dmx --disable-xvfb \
     --disable-xnest --disable-kdrive --disable-xephyr \
     --with-sha1=libgcrypt --enable-xcsecurity --disable-docs \
     --disable-selective-werror)
  touch "$BUILD/xorg-config-stamp"
fi
make -j"$JOBS" -C "$BUILD/unix/xserver" \
  TIGERVNC_SRCDIR="$SRC" TIGERVNC_BUILDDIR="$BUILD"

echo "=== [3/3] Install into $STAGE"
rm -rf "$STAGE"
make install DESTDIR="$STAGE"
make -C "$BUILD/unix/xserver/hw/vnc" install DESTDIR="$STAGE" \
  TIGERVNC_SRCDIR="$SRC" TIGERVNC_BUILDDIR="$BUILD"

echo "=== Done"
ls -l "$STAGE/usr/bin/Xvnc" "$STAGE"/usr/libexec/Xvnc-wayland
