# TigerVNC fork: Wayland mode for Xvnc

This fork adds a **Wayland mode** to `Xvnc`. It is still a persistent
virtual display that you control over VNC, like upstream `Xvnc`, but the
display server is a headless wlroots compositor instead of an X server.
X11 applications run in it through Xwayland.

```
Xvnc :1 ...                 # X11 server, exactly as upstream (default)
Xvnc --x11 :1 ...           # same thing, explicitly
Xvnc --wayland :1 ... [-- session command]   # Wayland compositor
```

X11 is the default everywhere: in `Xvnc`, `vncserver` and `vncsession`.
Without `--wayland` or `mode=wayland`, nothing changes.

## Design

```
                  +---------------------------+
 Xvnc (X.Org) --> | main(): scan argv          |   no flag / --x11: dix_main()
                  |  --wayland: execv(...)     |   (unchanged X server)
                  +-------------+-------------+
                                | argv minus the flag, argv[0] kept
                                v
        ${libexecdir}/Xvnc-wayland   (separate program, links wlroots)
        +-------------------------------------------------------------+
        | xvnc-wayland.cxx   main loop, options, listeners, session   |
        | WaylandServerDesktop  rfb::SDesktop: framebuffer, input,    |
        |                       clipboard, SetDesktopSize             |
        | compositor.c       wlroots: headless backend + pixman,      |
        |                    scene graph, xdg-shell, layer-shell,     |
        |                    Xwayland, seat, selections               |
        +-------------------------------------------------------------+
                     shared: common/rfb, rdr, network, core
```

* **One user-facing command.** `Xvnc` is the entry point. Mode selection
  happens first thing in `main()` in `unix/xserver/hw/vnc/xvnc.c`, before
  the X server sees the arguments. `--x11` is removed and the X server
  starts normally. `--wayland` is removed and the process `execv()`s
  `Xvnc-wayland` with the remaining arguments, keeping `argv[0]`.
  `--x11 --wayland` is an error. The path comes from
  `CMAKE_INSTALL_FULL_LIBEXECDIR` through `vncGetWaylandServerPath()` in
  `RFBGlue.cc`. For build-tree testing, the `TIGERVNC_XVNC_WAYLAND`
  environment variable can override it. The override is ignored for
  set-id processes.
* **wlroots is never linked into the X.Org-built server.** `ldd Xvnc` is
  checked by the tests.
* **Same VNC core.** `Xvnc-wayland` uses the same `rfb::VNCServerST`,
  security stack, encoders and `core::Configuration` parameters as Xvnc.
  So `-rfbport`, `-rfbunixpath`, `-rfbunixmode`, `-localhost`,
  `-interface`, `-SecurityTypes`, `-PasswordFile`/`-rfbauth`, `-X509Cert`,
  `-X509Key`, `-PlainUsers`, `-pam_service`, `-AcceptCutText`,
  `-SendCutText`, `-SetPrimary`, `-SendPrimary`, `-MaxClipboard`,
  `-AlwaysShared`, `-IdleTimeout` and the rest behave identically.
  `-geometry`, `-depth 24`, `-pixelformat rgb888` and `-inetd` are
  accepted as in Xvnc.
* **Compositor** (`unix/xvnc-wayland/compositor.c`):
  * wlroots 0.19 with the headless backend, the pixman renderer and one
    XRGB8888 output.
  * Each frame is composited by `wlr_scene`, with direct scanout
    disabled, and the damaged rectangles are copied into a persistent
    framebuffer. Damage is reported to `VNCServerST::add_changed`.
  * Input:
    * Keyboard events go to a virtual `wlr_keyboard`, with the keymap
      from `XKB_DEFAULT_*`. Keysyms are mapped to keycodes, faking
      Shift/AltGr where needed. With `RawKeyboard`, QEMU extended key
      events use the raw keycode.
    * Pointer events go to the seat, including the wheel and buttons
      8/9 as side/extra.
    * Viewer key auto-repeat is dropped, because Wayland clients repeat
      locally.
  * Cursor: the client's cursor surface, or the xcursor default, is sent
    as the VNC cursor.
* **Minimal window manager policy.**
  * Floating, stacking windows.
  * New xdg toplevels are centred and cascaded. X11 windows go where
    they ask to be.
  * Click to focus and raise.
  * Move and resize through client-side decorations (xdg
    `move`/`resize` requests and `_NET_WM_MOVERESIZE`).
  * Maximize and fullscreen requests are honoured.
  * Popups are constrained to the output.
  * layer-shell (panels, backgrounds, launchers) is supported, with an
    exclusive-zone usable area.
* **Clipboard.**
  * Both the CLIPBOARD and PRIMARY selections are synced with the
    viewer, with the same semantics as Xvnc: `SetPrimary`,
    `SendPrimary`, lazy data transfer, and Latin-1/UTF-8 handling
    through the RFB core.
  * Wayland apps use `wl_data_device` and primary-selection-v1.
  * Clipboard managers and `wl-copy`/`wl-paste` use wlr-data-control
    and ext-data-control.
  * X11 apps are bridged by the wlroots Xwayland WM.
* **Client-driven resize.** `SetDesktopSize` changes the output's custom
  mode. Wayland clients see a new `wl_output` mode and Xwayland resizes
  its screen. All viewers get `ExtendedDesktopSize`. Multi-screen layouts
  are accepted as metadata only, because there is a single output.
* **Xwayland** is started when the compositor starts. Disable it with
  `-Xwayland=0`. Its display number is chosen automatically by wlroots,
  and is not necessarily the VNC display number.
* **Session start.** Everything after `--` is started once the
  compositor and Xwayland are ready. Its environment has
  `WAYLAND_DISPLAY`, `DISPLAY` (Xwayland) and `XDG_SESSION_TYPE=wayland`.
  When the session command exits, the server exits, like `xinit` does in
  X11 mode. The Wayland socket is `wayland-<display#>` in
  `$XDG_RUNTIME_DIR`. If that is unset, a private
  `/tmp/runtime-<uid>` is used. The `WaylandDisplay` parameter overrides
  the socket name.
* **vncserver / vncsession / systemd.**
  * The mode is chosen only in the config files
    (`~/.config/tigervnc/config`, `vncserver-config-defaults`,
    `vncserver-config-mandatory`), with `mode=x11` or `mode=wayland`.
    Leaving it out is the same as an explicit `mode=x11`.
  * The value is case-insensitive and surrounding whitespace is
    ignored. The last setting wins, and the mandatory file wins over
    the user's file.
  * Any other value is an error: vncsession refuses to start the
    session and logs `Invalid mode` to syslog, and vncserver exits with
    `Invalid mode '<value>', must be 'x11' or 'wayland'`.
  * There is no per-display mode in `vncserver.users`.
  * vncserver: `session=` then refers to
    `/usr/share/wayland-sessions/*.desktop`, and the X-only defaults
    (`auth`, `fp`, `pn`) are not passed on.
  * vncserver starts `Xvnc --wayland :N <options> -- [dbus-run-session --]
    /bin/sh -c "exec <Exec>"`.
  * vncsession reads the same `mode` key before PAM, with the user's
    file read with the user's privileges. It then opens the PAM session
    with `XDG_SESSION_TYPE=wayland`, so pam_systemd/logind register a
    Wayland session.
  * Nothing changes for X11: the same `xinit`/Xsession path and
    `XDG_SESSION_TYPE=x11`. The `vncserver@.service` unit is unchanged.

## Files

| Path | What |
|---|---|
| `unix/xserver/hw/vnc/xvnc.c` | `--x11`/`--wayland` dispatch in `main()`, usage lines |
| `unix/xserver/hw/vnc/RFBGlue.{h,cc}` | `vncGetWaylandServerPath()` |
| `unix/xserver/hw/vnc/Xvnc.man` | synopsis + WAYLAND MODE section |
| `unix/xvnc-wayland/` | new: the Wayland mode server (`Xvnc-wayland`) |
| `unix/xvnc-wayland/protocol/wlr-layer-shell-unstable-v1.xml` | protocol XML (from wlroots 0.19.2, MIT-style licence) for a header wlroots needs |
| `CMakeLists.txt`, `unix/CMakeLists.txt` | `ENABLE_WAYLAND_SERVER` (AUTO/ON/OFF), `WLROOTS_PKG` |
| `config.h.in` | `CMAKE_INSTALL_FULL_SYSCONFDIR` for vncsession |
| `unix/vncserver/vncserver.in` | `mode=` handling, Wayland session start |
| `unix/vncserver/vncsession.c` | read `mode` before PAM, set `XDG_SESSION_TYPE` |
| `unix/vncserver/vncsession.man.in`, `vncserver-config-defaults` | docs |
| `contrib/wayland-mode/` | Dockerfiles, build script, test runner, tests |
| `test-artifacts/` | screenshots, logs, results of the last test run |

## Pinned dependencies

Reference platform: Ubuntu 26.04 (resolute), as pinned in
`contrib/wayland-mode/Dockerfile.buildenv`.

| Dependency | Version |
|---|---|
| wlroots (`libwlroots-0.19-dev`) | 0.19.2-1 (API: 0.19; `WLROOTS_PKG=wlroots-0.19`) |
| Xorg server source (`xorg-server-source`) | 2:21.1.22-1ubuntu1.3 (built with `unix/xserver21.patch`) |
| Xwayland | 2:24.1.10-1 |
| wayland-protocols | 1.47 |
| libwayland, xkbcommon, pixman, libdrm, xcb | distro versions |

wlroots breaks its API between minor releases. The code targets 0.19
only, and `WLROOTS_PKG` must name a 0.19 build.

## Building

```sh
# everything in Docker (needs docker; PROXY optional)
PROXY=http://127.0.0.1:8791 contrib/wayland-mode/run-tests.sh [build-dir] [artifact-dir]

# or by hand on Ubuntu 26.04 with the packages from Dockerfile.buildenv
SRC=$PWD BUILD=/tmp/build contrib/wayland-mode/build.sh
```

`build.sh` follows `contrib/packages/deb/ubuntu-resolute/debian/rules`:

1. CMake builds the libraries, `Xvnc-wayland`, vncsession, vncserver and
   the rest (`-DENABLE_WAYLAND_SERVER=ON`).
2. The system Xorg source is extracted, `xserver21.patch` is applied,
   then autotools configure and make produce `Xvnc`.
3. `make install DESTDIR=$BUILD/stage`.

`Xvnc` goes to `/usr/bin`, and `Xvnc-wayland` goes to `/usr/libexec`
(`CMAKE_INSTALL_LIBEXECDIR`).

## Usage

```sh
vncpasswd ~/.config/tigervnc/passwd
Xvnc --wayland :1 -rfbauth ~/.config/tigervnc/passwd -geometry 1600x900 \
     -SecurityTypes X509Vnc,VncAuth -X509Cert cert.pem -X509Key key.pem \
     -localhost -- foot
Xvnc --wayland -help          # full option list
```

With vncsession/systemd, add the following to
`~/.config/tigervnc/config`:

```
mode=wayland
session=<name of a /usr/share/wayland-sessions/*.desktop file>
```

Then start the service: `systemctl start vncserver@:1`, with
`:1=user` in `/etc/tigervnc/vncserver.users`.

A wlroots-based desktop such as `labwc` or `sway` should be able to run
nested as the session command, as a window in this compositor. This is
untested. Nested compositors are not in the test image, and the window
is not automatically made full size.

## Tests

Run with `contrib/wayland-mode/run-tests.sh`. Results are in
`test-artifacts/SUMMARY.md`, plus `results-*.json`, screenshots and
`logs/`. Everything goes through a real RFB client
(`tests/rfbclient.py`, standard library only) talking to the server:

* `test_x11.py`: regression for plain `Xvnc` and `Xvnc --x11`.
* `test_wayland.py`: the Wayland mode criteria.
* `test_vncsession.py`: the vncsession → PAM → vncserver → Xvnc path,
  as root.
* `test_systemd.py`: `vncserver@.service` under systemd with
  pam_systemd/logind, in a privileged container.

## Limitations / known gaps

* Single output: multi-screen `SetDesktopSize` layouts are kept as
  metadata only.
* Depth 24 (XRGB8888) only. `-depth` other than 24 is refused.
* No `vncconfig` equivalent. `QueryConnect` therefore rejects
  connections, as Xvnc does when no vncconfig is running. Runtime
  parameter changes through vncconfig are not possible.
* `AvoidShiftNumLock` is accepted but has no effect.
* Software rendering only (pixman). There is no GPU/dmabuf, and clients
  use shm buffers.
* Xwayland is started without X authority, like sway/labwc. Any local
  user who can reach its socket can connect. Its display number is
  automatic.
* X11 apps can only read or write the clipboard while an X11 window has
  keyboard focus. This is the wlroots Xwayland WM policy, which matches
  the normal interactive use.
* Minimal WM: no server-side decorations, so X11 windows have no title
  bars, plus no workspaces, no keyboard shortcuts and no
  screensaver/idle handling. No input methods, tablets, touch or
  pointer constraints.
* `wlr-output-management` is not exposed, so `wlr-randr`/`kanshi` cannot
  see or change the output. Resize goes through the viewer.
* `vncserver` in Wayland mode ignores X-only config keys with a warning
  (`auth`, `fp`, `pn`, `nopn`, `listen`, `nolisten`, `dpi`, `ac`,
  `noreset`, `co`, `xkbdir`). Any other X server option in the config
  makes `Xvnc --wayland` refuse to start.
* vncsession looks for the user config at `~/.config/tigervnc/config`, or
  `~/.vnc/config` as a fallback. A custom `XDG_CONFIG_HOME` set only by
  PAM is not seen when the mode is read.
