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
        |                    Xwayland, seat, selections, floating WM  |
        |                    or whole screens for a nested compositor |
        | nested_bridge.c    wlr-data-control client to a nested      |
        |                    compositor: clipboard + primary bridge   |
        | ControlSocket.cxx  vncconfig's way in (no VNC X extension)  |
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
  `-geometry`, `-depth`, `-pixelformat` and `-inetd` are accepted as in
  Xvnc.
* **Depths.** The compositor always renders XRGB8888. With `-depth 8`,
  `15`, `16` or `32`, or a `-pixelformat` such as `rgb565` or `bgr888`,
  the server's native pixel format is that format (the same ones Xvnc
  picks for each depth), and damaged areas are converted into a
  separate buffer. Viewers see it in `ServerInit` and can still ask for
  any format. `-depth 24`/`rgb888` uses the compositor's buffer directly.
* **X server options with no meaning here** (`-auth`, `-fp`, `-pn`,
  `-dpi`, `-ac`, `-noreset`, `-co`, `-xkbdir`, `-nolisten`, `-s`,
  `-dpms`, `+extension`, ... see `ignoredXOptions` in
  `xvnc-wayland.cxx`) are skipped with the warning
  `option '-dpi' has no effect in Wayland mode`, so existing configs
  and scripts keep working. Unknown options are still errors.
* **Compositor** (`unix/xvnc-wayland/compositor.c`):
  * wlroots 0.19 with the headless backend, the pixman renderer and one
    XRGB8888 output.
  * Each frame is composited by `wlr_scene`, with direct scanout
    disabled, and the damaged rectangles are copied into a persistent
    framebuffer. Damage is reported to `VNCServerST::add_changed`.
  * Input:
    * Keyboard events go to a virtual `wlr_keyboard`, with the keymap
      from `XKB_DEFAULT_*`. Keysyms are mapped to keycodes, faking
      Shift/AltGr where needed. Candidates are tried on a copy of the
      real XKB state, so a locked NumLock or CapsLock is taken into
      account. With `RawKeyboard`, QEMU extended key events use the raw
      keycode.
    * `AvoidShiftNumLock` (default on) works as in Xvnc: if a keypad
      key would need a fake Shift (e.g. `XK_KP_Left` while NumLock is
      on), the equivalent key outside the keypad is used instead. A
      keypad keysym that is only reachable by changing NumLock (e.g.
      `XK_KP_4` with NumLock off) is sent as the plain key (`4`).
    * The LED state is reported from the start. Without it, the RFB
      core would ignore lock keys and skip its NumLock/CapsLock
      synchronisation for viewers that do not track LEDs.
    * Pointer events go to the seat, including the wheel and buttons
      8/9 as side/extra.
    * Viewer key auto-repeat is dropped, because Wayland clients repeat
      locally.
  * Cursor: the client's cursor surface, or the xcursor default, is sent
    as the VNC cursor.
* **Minimal window manager policy.**
  * Floating, stacking windows.
  * New xdg toplevels are centred and cascaded on the screen that holds
    the pointer. X11 windows go where they ask to be.
  * Multi-monitor: the viewer's `SetDesktopSize` screen layout is
    used. Maximize and fullscreen fill the screen the window is on (its
    centre), minus panel exclusive zones; a window that asks to start
    maximized or fullscreen gets the screen with the pointer. Without a
    layout, or after a resize without one, there is one screen.
  * Click to focus and raise.
  * Move and resize through client-side decorations (xdg
    `move`/`resize` requests and `_NET_WM_MOVERESIZE`).
  * X11 title bars (`X11TitleBars`, default on): managed X11 windows
    get a 22 pixel server-side bar.
    * *Title.* The bar shows the window's title (`_NET_WM_NAME`/`WM_NAME`)
      in white. It is drawn with a built-in copy of the public domain
      X.Org misc-fixed 6x13 font (`deco_font.h`, Latin-1; other
      characters show as `?`), so there is no font library dependency.
      The text follows renames and is cut with `...` before the buttons.
    * *Buttons and dragging.* Dragging the bar (also on the text) moves
      the window. It has maximize/restore and close (`WM_DELETE_WINDOW`)
      buttons. It is blue when focused and grey otherwise.
    * *Resize borders.* An invisible 5 pixel border around the frame
      (title bar plus window) resizes with the pointer, by one edge or,
      within 16 pixels of a corner, by two. The pointer shows the
      matching resize cursor. Resizing keeps the window's
      `WM_NORMAL_HINTS` minimum size and room for the buttons. There are
      no borders while the window is maximized.
    * *Windows without a bar.* Windows that ask for no title
    through `_MOTIF_WM_HINTS`, override-redirect windows and fullscreen
    windows get no bar. A window whose bar would be off the top edge is
    moved down to make room.
  * X11 windows get their initial geometry (and the zero border width
    every wlroots WM sets) on the client's `MapRequest`, before the X
    server maps them. Doing it after mapping made the X server
    reallocate the window's pixmap and could lose contents drawn before
    that, e.g. an intermittently black `xeyes` window.
  * Maximize and fullscreen requests are honoured, including requests
    that arrive between the first commit and the first map (e.g.
    `foot --fullscreen`).
  * Popups are constrained to the output.
  * layer-shell (panels, backgrounds, launchers) is supported, with an
    exclusive-zone usable area.
* **Nested full compositors** (sway, labwc, KDE Plasma, ...). The
  built-in floating WM stays the fallback for ordinary applications. A
  session that is itself a compositor, running with its wayland backend,
  shows each of its outputs as an xdg toplevel. Those toplevels are
  handled differently:
  * Detection (`NestedCompositor=Auto`, the default): at the toplevel's
    first commit, or at the latest when it maps, the client's pid is read
    with `wl_client_get_credentials()`. Each `wayland-*` socket in our
    `XDG_RUNTIME_DIR`, owned by our uid and not our own, is connected to,
    and `SO_PEERCRED` is checked. If the client serves one of them, it is
    a compositor. Only parentless toplevels qualify. `NestedCompositor=On`
    treats every parentless toplevel as a nested output, and `Off` never
    does.
  * Layout: nested outputs are fullscreen (`xdg_toplevel` fullscreen
    state, so wlroots-based compositors draw no decorations; no
    xdg-decoration is offered). They are kept at the bottom of the stack,
    so ordinary windows of ours still show above them. Move and resize
    requests are ignored, and maximize/fullscreen requests re-assert the
    layout.
  * Multi-monitor: the client's `SetDesktopSize` screen layout is passed
    to the compositor (`wvnc_set_screens()`). With two or more screens
    and two or more nested outputs, output *i* (in creation order) gets
    screen *i*, and extra outputs get the whole framebuffer. Otherwise
    one output covers the whole framebuffer. Resizes re-arrange
    everything, and the nested compositor sees a new size in the
    `xdg_toplevel.configure`. For sway, `WLR_WL_OUTPUTS=n` creates *n*
    outputs. Where the nested compositor places its outputs in its own
    logical layout is its configuration, not ours.
  * Input: keyboard focus and pointer events go to the nested output
    surface like any other window. The nested compositor routes them on.
  * Clipboard bridge (`nested_bridge.c`): a libwayland-client connection
    to the nested compositor's socket, driven fully asynchronously from
    our event loop (no blocking round trips, since the nested compositor
    is also our client). It uses `zwlr_data_control_manager_v1` (v2 for
    primary). Nested selections become sources on our seat, so they reach
    the viewer and our own clients. Our selections, including those from
    the viewer, are offered to the nested seat with a marker MIME type,
    so they don't loop back. Data is streamed fd to fd and never
    buffered. The bridge goes away with the compositor's last output.
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
  its screen. All viewers get `ExtendedDesktopSize`. Our single output
  covers the whole framebuffer; the screen layout drives the window
  manager (see above) and the outputs of a nested compositor.
* **wlr-output-management.** `wlr-randr`, `kanshi` and similar tools
  see the output. A new size (`wlr-randr --output HEADLESS-1
  --custom-mode 1280x720`, or `--mode`) resizes the framebuffer like
  `xrandr` does in Xvnc. All viewers are told, and the layout becomes a
  single screen. Disabling the output, or changing its position, scale
  or transform, is refused.
* **vncconfig.** There is no VNC X extension, so `Xvnc-wayland` listens
  on `$XDG_RUNTIME_DIR/tigervnc-<wayland socket>.vncconfig`
  (`ControlSocket.cxx`; exported to the session as `TIGERVNC_VNCCONFIG`,
  so it also works from inside a nested compositor). Neither this nor
  the Xauthority file starts with `wayland-`, because clients scan for
  `wayland-*` sockets.
  Only the owner can use it: the socket is mode 0700 and the peer uid is
  checked. When the X display has no VNC extension, `vncconfig` uses
  this socket instead. `-list`, `-get` (never the password), `-desc`,
  `-set`/`name=value` (only for `AllowOverride` parameters, and
  `desktop` renames the server), `-connect [-view-only] host[:port]`
  and `-disconnect` work as with Xvnc.
  * *Without options* (`vncconfig`, `vncconfig -nowin`, `-iconic`), it
    runs as on Xvnc. It is an X client of Xwayland (`DISPLAY`), using
    the same TX toolkit window and the same Accept/Reject dialog
    (`QueryConnectDialog`). The control socket stands in for the VNC
    extension:
    * The window's clipboard checkboxes (`AcceptCutText`, `SetPrimary`,
      `SendCutText`, `SendPrimary`) read and set the parameters with
      `get`/`set`.
    * A persistent `watch` connection replaces
      `XVncExtSelectInput(VncExtQueryConnectMask)`. The server sends
      `query<TAB>id<TAB>timeout<TAB>address<TAB>user`, and the dialog
      answers `approve id 1|0`.
  * *QueryConnect.* `WaylandServerDesktop::queryConnection()` follows
    `XserverDesktop::queryConnection()`, with the same messages and the
    same `QueryConnectTimeout` parameter (default 10 s):
    * A connection is prompted while a vncconfig is running. "Accept"
      admits it. "Reject" rejects it with "Connection rejected by local
      user".
    * With no answer, it is rejected when the timeout expires.
    * A second connection during a query is rejected ("Another
      connection is currently being queried.").
    * With no vncconfig running, connections are rejected ("Unable to
      query the local user to accept the connection.").
    * Reverse connections (`vncconfig -connect`) are not queried, as
      on Xvnc.
  * *Bool values.* vncconfig now treats "on"/"true"/"yes" as well as
    "1" as true when it reads bool parameters, for both servers. Xvnc
    and the control socket return "on"/"off".
* **Xwayland** is started when the compositor starts. Disable it with
  `-Xwayland=0`. Its display number is chosen automatically by wlroots,
  and is not necessarily the VNC display number.
* **X authority** (`XwaylandAuth`, default on). wlroots itself starts
  Xwayland without access control. `Xvnc-wayland` sets `WLR_XWAYLAND`
  to its own executable. Started as `Xwayland`, it writes a fresh
  random `MIT-MAGIC-COOKIE-1` for that display to
  `$XDG_RUNTIME_DIR/tigervnc-<wayland socket>.Xauthority` (mode 0600, written to
  a temporary file and renamed). It then execs the real Xwayland
  (the user's `WLR_XWAYLAND` if one was set, otherwise `Xwayland` from
  `PATH`) with `-auth`. The session gets `XAUTHORITY`. X clients
  without the cookie get "Authorization required".
* **Session start.** Everything after `--` is started once the
  compositor and Xwayland are ready. Its environment has
  `WAYLAND_DISPLAY`, `DISPLAY` (Xwayland), `XAUTHORITY` and
  `XDG_SESSION_TYPE=wayland`. Our internal variables (`WLR_XWAYLAND` and
  the `TIGERVNC_*` ones) are restored or removed first.
* **The display persists like Xvnc's.** When the session command exits,
  the server logs it and keeps running until it is killed, as `Xvnc`
  does. Viewers can still connect, and clients can still be started on
  the display. `-ExitWithSession` stops the server when the session
  ends instead. vncserver passes it, because that matches what happens
  in X11 mode, where `xinit` ends the X server with the session. The
  Wayland socket is `wayland-<display#>` in
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
    `/usr/share/wayland-sessions/*.desktop`. X-only options from the
    config (`auth`, `fp`, `pn`, `dpi`, ...) are passed on, and
    `Xvnc --wayland` ignores them with a warning.
  * vncserver starts `Xvnc --wayland :N <options> -ExitWithSession --
    [dbus-run-session --] /bin/sh -c "exec <Exec>"`.
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
| `unix/xvnc-wayland/protocol/wlr-data-control-unstable-v1.xml` | protocol XML (from wlroots 0.19.2) for the nested clipboard bridge's client code |
| `unix/xvnc-wayland/nested_bridge.{h,c}` | nested compositor detection helper and clipboard bridge |
| `unix/xvnc-wayland/ControlSocket.{h,cxx}` | vncconfig control socket |
| `unix/vncconfig/vncconfig.{cxx,man}` | control socket in Wayland mode: command-line options, the window and the QueryConnect prompt |
| `unix/xvnc-wayland/deco_font.h` | 6x13 bitmap font for X11 title text (generated from X.Org misc-fixed, public domain) |
| `contrib/wayland-mode/tools/gen-deco-font.py` | generator for `deco_font.h` |
| `CMakeLists.txt`, `unix/CMakeLists.txt` | `ENABLE_WAYLAND_SERVER` (AUTO/ON/OFF), `WLROOTS_PKG` |
| `config.h.in` | `CMAKE_INSTALL_FULL_SYSCONFDIR` for vncsession |
| `unix/vncserver/vncserver.in` | `mode=` handling, Wayland session start |
| `unix/vncserver/vncsession.c` | read `mode` before PAM, set `XDG_SESSION_TYPE` |
| `unix/vncserver/vncsession.man.in`, `vncserver-config-defaults` | docs |
| `contrib/wayland-mode/` | Dockerfiles, build script, test runner, tests |
| `contrib/wayland-mode/patches/wlroots-16cb509a-allocator-finish.patch` | upstream wlroots fix applied to the test image's libwlroots 0.19.2 (nested compositors, see Limitations) |
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
| libwayland (server and client), xkbcommon, pixman, libdrm, xcb | distro versions |
| sway, labwc (test image only) | Ubuntu 26.04 versions, used by the nested tests |

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

### Portable tarball

A relocatable `tigervnc-<version>.<arch>.tar.gz` (prefix `/usr`) can be
checked on a clean distro image with `test-tarball.sh`. That extracts
the tarball and runs the full suite (X11 + Wayland) against it.

```sh
PROXY=... contrib/wayland-mode/test-tarball.sh /path/to/tigervnc-*.tar.gz ubuntu:26.04
```

Runtime dependencies on the target are only glibc (>= 2.27), libGL
(Mesa, plus `swrast_dri.so` for GLX), libpam, and libsystemd/libgbm
when those features are on. `/usr/share/tigervnc/check-runtime.sh`
checks them.

`xinit` and `xauth` are optional and are not bundled. As upstream does,
the distro's packages are used, and only X11 sessions started through
`vncserver`/`vncsession` (and `vncserver@.service`) need them. Starting
`Xvnc` or `Xvnc --wayland` directly needs neither, and nor does
`mode=wayland`. The tarball test image installs `xinit` and `xauth` for
the X11 vncsession/systemd tests.

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

A full desktop compositor can be the session, e.g. `session=sway` or
`session=labwc` (their stock `.desktop` files), or
`Xvnc --wayland :1 ... -- sway`. It gets the whole screen and follows
viewer resizes. To give sway one output per viewer monitor, start it with
`WLR_WL_OUTPUTS=2` (for example in a wrapper session script) and use a
viewer with a two-screen layout. `-NestedCompositor Off` turns this
off, and `On` forces it for compositors whose socket isn't found (then
there is no clipboard bridge).

## Tests

Run with `contrib/wayland-mode/run-tests.sh`. Results are in
`test-artifacts/SUMMARY.md`, plus `results-*.json`, screenshots and
`logs/`. Everything goes through a real RFB client
(`tests/rfbclient.py`, standard library only) talking to the server:

* `test_x11.py`: regression for plain `Xvnc` and `Xvnc --x11`.
* `test_wayland.py`: the Wayland mode criteria, plus Xauthority,
  display persistence and `-ExitWithSession`, `-depth 16`, ignored X
  options, `AvoidShiftNumLock` (checked with `xev`), two-screen window
  placement, `wlr-randr`, X11 title bars and `vncconfig`.
* `test_nested.py`: a real nested sway, and labwc: detection,
  fullscreen over VNC, keyboard and pointer into foot inside sway, both
  selections both ways, resize, a two-screen layout mapped to two sway
  outputs, labwc clipboard and resize, and `NestedCompositor=Off`
  falling back to the floating WM.
* `test_vncsession.py`: the vncsession → PAM → vncserver → Xvnc path,
  as root, including the stock `sway.desktop` session (Super+Return
  opens sway's terminal, typing and resize).
* `test_systemd.py`: `vncserver@.service` under systemd with
  pam_systemd/logind, in a privileged container.

## Limitations / known gaps

* One output of our own. A multi-screen layout from the viewer drives
  window placement, maximize and fullscreen, and nested compositor
  outputs, but Wayland clients and Xwayland see one `wl_output` and one
  X screen covering all monitors. There is no per-monitor `wl_output`
  and no RandR monitor list in Xwayland.
* Nested compositors:
  * The clipboard bridge needs `wlr-data-control` in the nested
    compositor. sway, labwc and other wlroots compositors have it. KDE
    and GNOME (which can't run nested this way anyway) don't, so with
    KDE Plasma the display works but the clipboard is not bridged.
    ext-data-control is not used yet.
  * Detection needs the nested compositor's socket in the same
    `XDG_RUNTIME_DIR` and owned by the same user, which is the normal
    case.
  * labwc applies a new output size at once but only repaints when
    something on its output changes. Until then the old picture is shown
    at the old size. The test interacts with foot in labwc after each
    resize, as a user would.
  * **Nested wlroots compositors need wlroots >= 0.19.3, or 0.19.2 with
    upstream commit 16cb509a** ("render/allocator: add missing
    wlr_buffer_finish() in destroy impls"). This matters most for sway with two
    outputs. Ubuntu 26.04 ships libwlroots 0.19.2-1, which lacks it.
    Without the fix, a nested sway with `WLR_WL_OUTPUTS=2` showed our
    background on its first output after a layout change in about one run in
    four.
    * *Mechanism.* Under Xvnc --wayland the nested compositor renders with
      pixman, so it uses wlroots' shm allocator. In 0.19.2 that allocator
      frees buffers without emitting `wlr_buffer.events.destroy`. The
      wayland backend caches one `wl_buffer` per `wlr_buffer` *pointer* and
      drops a cache entry only on that destroy signal. So when a resize
      replaces sway's swapchains, stale entries survive.
    * *What goes wrong.* When malloc gives a freed buffer's address to a
      new 800x600 output buffer, `get_or_create_wl_buffer()` returns the
      old `wl_buffer`. In the failing runs that was a 10x16 cursor image.
      sway then commits that as the output window's frame, and we
      correctly show a 10x16 buffer on a 800x600 window with our
      background around it.
    * *How it was proven.* A wlroots built with logging in the buffer
      cache showed the same `wl_buffer` reused for one pointer whose size
      had changed (created 10x16, reused as 800x600) with no destroy in
      between. The `WAYLAND_DEBUG` trace shows the 10x16 `wl_buffer`
      attached to the output surface.
    * *What it is not.* The hardware cursor is only involved because the
      cursor swapchain adds buffer churn. `WLR_NO_HARDWARE_CURSORS=1` just
      made the reuse rarer. No `wl_pointer.set_cursor` reaches our seat
      during the test, and our compositor handles the commits correctly.
      A compositor can't tell this stale buffer from a legitimate one, so
      there is no fix on our side.
    * *Test image.* Dockerfile.test rebuilds the distro's libwlroots
      0.19.2-1 with only that upstream patch
      (`contrib/wayland-mode/patches/`). The test still runs sway with
      hardware cursors and checks both outputs strictly. With the patch,
      20 of 20 consecutive standalone `test_nested.py` runs passed
      (hardware cursors on). The same image with the stock library failed
      the two-screen test on its first run. Users of nested sway/labwc on
      0.19.2 need the same fix from their distribution.
  * Only text MIME types reach the viewer, as for any client. Other
    types are passed between the nested seat and our seat.
  * Under Docker's default 64 MiB `/dev/shm`, several servers with nested
    compositors at once can run out of shm. wlroots then dies with
    SIGBUS while rendering. The test stops each server before starting
    the next one. Normal hosts have a much larger `/dev/shm`.
* Depths other than 24 cost a conversion of each damaged area. Depth 8
  is true colour (rgb332), with no colour map.
* vncconfig's window and dialog need Xwayland. With `-Xwayland=0`
  there is no X display, and plain `vncconfig` fails ("Unable to open
  display"), as it would without an X server. The command-line options
  still work.
* vncconfig's Accept dialog shows the buttons, but not the Host/User/
  countdown labels. This also happens with X11 Xvnc in the test image,
  with the unmodified upstream TX code, so it is not specific to Wayland
  mode.
* X11 title bars have no minimize button (there is no taskbar to
  restore from). The title font covers Latin-1 only.
* Clients that don't support the cursor pseudo-encoding see the
  server-rendered cursor at the top-left corner. `Xvnc-wayland` does
  not report pointer motion to the RFB core's cursor position
  (`setCursorPos`). The test client is such a client, so a cursor
  sprite can appear at (0,0) in screenshots. Normal viewers draw the
  cursor locally.
* `wlr-output-management` changes the size only. The refresh rate is
  accepted and ignored.
* Software rendering only (pixman). There is no GPU/dmabuf, and clients
  use shm buffers.
* The Xwayland display number is automatic (wlroots picks a free one).
* X11 apps can only read or write the clipboard while an X11 window has
  keyboard focus. This is the wlroots Xwayland WM policy, which matches
  the normal interactive use.
* Minimal WM: no workspaces, no keyboard shortcuts, no
  screensaver/idle handling. No input methods, tablets, touch or
  pointer constraints. For a full desktop, run a nested compositor
  (sway, labwc) as the session.
* vncsession looks for the user config at `~/.config/tigervnc/config`, or
  `~/.vnc/config` as a fallback. A custom `XDG_CONFIG_HOME` set only by
  PAM is not seen when the mode is read.
