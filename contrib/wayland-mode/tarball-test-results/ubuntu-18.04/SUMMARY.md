# Test results

## nested

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| SKIP | sway session detected as nested compositor, clipboard bridge connected | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | nested sway output is fullscreen and undecorated (screenshot over VNC) | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | keyboard input reaches a client of the nested compositor (foot in sway) | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | pointer reaches nested client (double click selects word), primary nested -> viewer | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | clipboard nested -> viewer (wl-copy in sway -> ServerCutText) | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | clipboard viewer -> nested (ClientCutText -> wl-paste and wl-paste --primary in sway) | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | clipboard viewer -> paste into foot inside sway with Ctrl+Shift+V | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | client resize: nested output follows (1024x768 -> 1280x720) | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | multi-monitor: two-screen client layout -> two nested outputs (sway WLR_WL_OUTPUTS=2) | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | labwc session: detected, output fills the screen, clipboard both ways, resize follows | needs sway, swaymsg, foot, not installed on this distro |  |
| SKIP | NestedCompositor=Off: sway is managed as an ordinary window (floating WM fallback) | needs sway, swaymsg, foot, not installed on this distro |  |

## systemd

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| SKIP | vncserver@:1.service, mode=wayland: Wayland session, logind session Type=wayland | needs foot, not installed on this distro |  |
| PASS | vncserver@:2.service, no mode: X11 session, logind Type=x11 | active=active, logind sessions=[{'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'x11', 'Class': 'user', 'State': 'active'}], xterm (42, 42, 404, 174) | test-artifacts/systemd-02-x11-unit.png |
| PASS | systemctl stop ends both sessions | after stop: :1 inactive, remaining tigervnc sessions [] |  |

## vncsession

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | vncsession with mode=wayland starts Xvnc-wayland as the user, XDG_SESSION_TYPE=wayland for PAM | vncsession rc=0 pid=10, port 5901 up=True, servers=[(12, 'Xvnc-wayland', 'vncuser')], server env from vncsession: XDG_SESSION_TYPE='wayland' XDG_SESSION_CLASS='user'  |  |
| PASS | Wayland session environment | USER=vncuser, XDG_SESSION_TYPE=wayland, WAYLAND_DISPLAY=wayland-1, DISPLAY=:0, XDG_CURRENT_DESKTOP=VncTest, XDG_SESSION_DESKTOP=vnc-test, DBUS_SESSION_BUS_ADDRESS=unix:abstract=/tmp/dbus-G5OLREDNHS,guid=e58c12af3a420c294d47f5226af342b2 |  |
| SKIP | Wayland session visible over VNC (foot + xterm) | needs foot, not installed on this distro |  |
| PASS | session log | /home/vncuser/.local/state/tigervnc/a3aee4b6b6cc:1.log (copied to logs/vncsession-wayland-user.log) |  |
| PASS | stop | vncsession exited and removed its pid file after the server stopped |  |
| PASS | vncsession without mode keeps X11 (Xvnc + Xsession) | no mode: server env XDG_SESSION_TYPE='x11', servers=[(66, 'Xvnc', 'vncuser')], session DISPLAY=':2' | test-artifacts/vncsession-02-x11-default.png |
| SKIP | vncsession starts the stock sway session (sway.desktop) as a nested compositor | needs sway, swaymsg, not installed on this distro |  |
| SKIP | stock sway session: Super+Return opens foot, typed command runs inside sway, resize follows | needs sway, swaymsg, foot, not installed on this distro |  |
| PASS | explicit mode=x11 behaves like the default | mode=x11: servers=[(137, 'Xvnc', 'vncuser')], XDG_SESSION_TYPE='x11', Xvnc args '/usr/bin/Xvnc :3 -geometry 1024x768 -localhost -securitytypes VncAuth -auth /home/vncuser/.Xauthority -desktop a3aee4b6b6cc:3 (vncuser) -fp /usr/share/fonts/X11//misc -pn -rfbauth /home/vncuser/.config/tigervnc/passwd -rfbport 5903' |  |
| PASS | invalid mode is rejected by vncsession and vncserver | vncsession rc=71, servers=[]; vncserver rc=255: "Invalid mode 'wayland2', must be 'x11' or 'wayland'" |  |
| PASS | mode value is case-insensitive and whitespace-trimmed | 'mode = Wayland ': servers=[(212, 'Xvnc-wayland', 'vncuser')] XDG_SESSION_TYPE='wayland' |  |

## wayland

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | headless start (no GPU, no parent display), session started with WAYLAND_DISPLAY+DISPLAY | session env WAYLAND_DISPLAY=wayland-1 DISPLAY=:0 XDG_SESSION_TYPE=wayland; /dev/dri present=False |  |
| PASS | auth: VncAuth with correct password | offered=[19, 2] name='wayland-test' size=1024x768 |  |
| PASS | auth: VncAuth with wrong password rejected | rejected: authentication failed: Authentication failed |  |
| PASS | auth: X509Vnc (VeNCrypt, X.509 TLS + VNC password) | subtypes=[2, 261, 258] tls=TLSv1.3 cipher=TLS_AES_256_GCM_SHA384 cert=781 bytes |  |
| PASS | auth: TLSVnc (VeNCrypt, anonymous TLS + VNC password) | tls=TLSv1.2 cipher=AECDH-AES256-SHA |  |
| PASS | auth: security type None not offered when not configured | security type None not offered (offered [19, 2]) |  |
| PASS | -localhost: not reachable on external address | connect to 172.17.0.2:5901 refused ([Errno 111] Connection refused) |  |
| SKIP | Wayland client renders (foot) | needs foot, not installed on this distro |  |
| PASS | X11 clients render via Xwayland (xterm, xeyes) | xterm white area (560, 430, 862, 562) (want origin 560,430), xdotool windows=['6291469'], xeyes colours=154, appeared after 2.5s | test-artifacts/wayland-03-foot-xterm-xeyes.png |
| SKIP | keyboard input reaches Wayland client (foot) | needs foot, not installed on this distro |  |
| PASS | keyboard input reaches X11 client (xterm via Xwayland) | file content 'x-key-ok-42-XTerm(330)' | test-artifacts/wayland-05-xterm-typed.png |
| SKIP | pointer motion+buttons reach Wayland client (foot word selection by double click) | needs foot, wl-paste, not installed on this distro |  |
| SKIP | pointer motion+buttons reach X11 client (xterm selection, xdotool pointer position) | needs wl-paste, not installed on this distro |  |
| SKIP | clipboard Wayland -> viewer (wl-copy -> ServerCutText) | needs wl-copy, not installed on this distro |  |
| SKIP | primary selection Wayland -> viewer (wl-copy --primary) | needs wl-copy, not installed on this distro |  |
| SKIP | clipboard viewer -> Wayland+X11 (ClientCutText -> wl-paste, wl-paste --primary, xclip) | needs wl-paste, not installed on this distro |  |
| PASS | clipboard X11 app -> viewer (xclip via Xwayland -> ServerCutText) | viewer received 'from-x11-789' |  |
| SKIP | clipboard viewer -> paste into foot with Ctrl+Shift+V | needs foot, not installed on this distro |  |
| SKIP | client resize 1024x768 -> 1280x720 changes the output (viewers, wl_output mode, Xwayland screen) | needs wayland-info, not installed on this distro |  |
| SKIP | client resize to 1000x700 | needs foot, wayland-info, not installed on this distro |  |
| SKIP | input still works after resize | needs foot, not installed on this distro |  |
| SKIP | second Wayland client (weston-terminal) starts | weston-terminal (weston 3.0.0-1) does not speak xdg_wm_base: it was offered but not bound (WAYLAND_DEBUG: bound ['wl_compositor', 'wl_data_device_manager', 'wl_output', 'wl_seat', 'wl_shm', 'wl_subcompositor']; weston-terminal pid 43, 0 sampled pixels changed) |  |
| PASS | Xwayland uses a private X authority file (XAUTHORITY for the session, clients without the cookie refused) | session XAUTHORITY=/tmp/wltest/xdg-runtime/tigervnc-wayland-1.Xauthority mode=0o600; xdpyinfo with it rc=0, without it rc=1 (); Xwayland args 'Xwayland :0 -auth /tmp/wltest/xdg-runtime/tigervnc-wayland-1.Xauthority -rootless -core -terminate -listenfd 15 -listenfd 16 -displayfd 25 -wm 22' |  |
| PASS | server survived the whole run and exits on SIGTERM | server alive until SIGTERM, exit code 0 |  |
| PASS | -inetd (connection on stdin/stdout) | RFB over inetd stdin/stdout, 640x480 | test-artifacts/wayland-11-inetd-640x480.png |
| PASS | display persists after the session command exits (like Xvnc) | session exited (status 3), server still running=True, viewer connected afterwards at 640x480 |  |
| PASS | -ExitWithSession stops the server with the session (used by vncserver) | -ExitWithSession: server exited with 0 after the session |  |
| SKIP | -depth 16: native RGB565 framebuffer, picture correct | needs foot, not installed on this distro |  |
| PASS | X-only options (-dpi -ac -fp -noreset -pn) are ignored with a warning instead of refused | server started=True, warnings for ['-dpi', '-ac', '-fp', '-noreset', '-pn'] |  |
| PASS | AvoidShiftNumLock: no fake Shift on keypad keys (xev); keypad keysyms unreachable without NumLock use the plain key | XK_KP_4 (NumLock off), Num_Lock, XK_KP_Left (NumLock on), Num_Lock: default -> X11 key presses [('13', '4'), ('77', 'Num_Lock'), ('113', 'Left'), ('77', 'Num_Lock')] (want [('13', '4'), ('77', 'Num_Lock'), ('113', 'Left'), ('77', 'Num_Lock')]: the arrow key instead of a fake Shift); -AvoidShiftNumLock=0 -> [('13', '4'), ('77', 'Num_Lock'), ('113', 'Left'), ('77', 'Num_Lock')] (want [('13', '4'), ( |  |
| SKIP | built-in WM with two screens: windows open, maximize and go fullscreen on one screen (Wayland and X11) | needs foot, not installed on this distro |  |
| SKIP | wlr-output-management: wlr-randr --custom-mode resizes the desktop for all viewers; unsupported changes are refused | needs wlr-randr, not installed on this distro |  |
| PASS | X11 title bars: window title drawn as text (pixel exact, follows renames), resize by edges and corners, drag moves, maximize/restore and close buttons work | xterm at (100, 100, 184, 108), title bar pixel (51, 84, 135); title text 'Title Test 42' drawn (0 pixels differ); after renaming, title text 'Renamed-xterm' drawn (0 pixels differ); right edge drag by +100,+0 -> (100, 100, 284, 108) (want (100, 100, 284, 108)); bottom-right corner drag by +50,+40 -> (100, 100, 334, 148) (want (100, 100, 334, 148)); left edge drag by -60,+0 -> (40, 100, 394, 148) ( | test-artifacts/wayland-17-x11-title-bar.png, test-artifacts/wayland-20-x11-title-renamed.png, test-artifacts/wayland-21-x11-title-bar-resized.png, test-artifacts/wayland-18-x11-title-bar-moved.png, test-artifacts/wayland-19-x11-title-bar-maximized.png |
| SKIP | vncconfig works in Wayland mode (control socket): -list -get -desc -set/AllowOverride, desktop rename, -disconnect, -connect | needs wl-copy, not installed on this distro |  |
| PASS | QueryConnect with the vncconfig prompt (as on Xvnc): rejected without vncconfig, Accept/Reject/busy/timeout with it, -nowin; vncconfig window shows and sets the clipboard parameters | no prompt running: viewer -> 'error:authentication failed: Unable to query the local user to accept the connection.'; observer: reverse connection (no query, as on Xvnc); vncconfig window on screen at (0, 22, 300, 100); checkboxes show the server's settings (dark pixels in boxes {'AcceptCutText': 0, 'SetPrimary': 17, 'SendCutText': 14, 'SendPrimary': 12}); clicking 'Send clipboard' -> SendCutText= | test-artifacts/wayland-22-vncconfig-window.png, test-artifacts/wayland-23-vncconfig-toggled.png, test-artifacts/wayland-24-queryconnect-dialog.png, test-artifacts/wayland-25-queryconnect-nowin.png |

## x11

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | Xvnc does not link wlroots | Xvnc NEEDED ['libpam.so.0', 'libpthread.so.0', 'libGL.so.1', 'libgbm.so.1', 'libdl.so.2', 'libm.so.6', 'libc.so.6', 'ld-linux-x86-64.so.2']: wayland/wlroots []; wlroots/wayland-server symbols in the binary []; via system libraries (ldd) ['libwayland-server.so.0'] |  |
| PASS | Xvnc -help (X server usage + mode flags) | Xvnc -help lists X server options, VNC parameters and the --x11/--wayland lines (logs/x11-Xvnc-help.txt) |  |
| PASS | Xvnc -version | Xvnc TigerVNC 1.16.2 - built Nov 10 2026 16:02:11 / Copyright (C) 1999-2026 TigerVNC team and many others (see README.rst) / See https://www.tigervnc.org for information on TigerVNC. / Underlying X server release 12101018 |  |
| PASS | Xvnc :2 starts (no flag) | port 5902 up=True; dimensions: 1024x768 pixels (271x203 millimeters) depth of root window: 24 planes |  |
| PASS | auth: VncAuth + X509Vnc, wrong password rejected | VncAuth ok, wrong password rejected, X509Vnc ok (TLSv1.3) |  |
| PASS | X11 clients render (xterm, xeyes) | xterm area (102, 102, 464, 260), xeyes colours 143 | test-artifacts/x11-01-xterm-xeyes.png |
| PASS | keyboard input reaches xterm | file content 'x11-key-ok-42-XTerm(330)' | test-artifacts/x11-02-typed.png |
| PASS | pointer input reaches xterm (selection, pointer position) | PRIMARY='PTRX11TEST' pointer='x:333 y:444 screen:0 window:675' | test-artifacts/x11-03-selected.png |
| PASS | clipboard both ways (X11) | viewer->X CLIPBOARD='x11-from-vnc', X->viewer='x11-from-xclip' |  |
| PASS | client resize (X11, RandR) | viewer 1280x720, X screen dimensions: 1280x720 pixels (338x190 millimeters) | test-artifacts/x11-04-resized-1280x720.png |
| PASS | Xvnc --x11 runs the X server | Xvnc --x11 :3 -> vendor string: The X.Org Foundation dimensions: 800x600 pixels (212x159 millimeters) | test-artifacts/x11-05-x11-flag-800x600.png |
| PASS | Xvnc --x11 --wayland is rejected | Xvnc: --x11 and --wayland cannot be combined |  |

**72 tests, 41 passed, 0 failed, 31 skipped**
