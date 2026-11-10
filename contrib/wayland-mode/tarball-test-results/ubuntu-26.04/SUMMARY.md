# Test results

## nested

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | sway session detected as nested compositor, clipboard bridge connected | server log: ['WaylandDesktop: Client pid 13 (wlroots) is a nested compositor serving', 'WaylandDesktop: Clipboard bridge to nested compositor (pid 13) ready,'] ['WaylandDesktop: Clipboard bridge to nested compositor (pid 13) ready,'] |  |
| PASS | nested sway output is fullscreen and undecorated (screenshot over VNC) | foot background covers 100% of 1024x768, corners [(36, 36, 36), (36, 36, 36), (36, 36, 36), (36, 36, 36)], sway outputs [('WL-1', 0, 0, 1024, 768)] | test-artifacts/nested-01-sway-foot-fullscreen.png |
| PASS | keyboard input reaches a client of the nested compositor (foot in sway) | file content 'nested-key-42-foot-sway' | test-artifacts/nested-02-typed.png |
| PASS | pointer reaches nested client (double click selects word), primary nested -> viewer | primary in sway after double click: 'NESTEDPTR'; viewer received 'NESTEDPTR' (primary bridged to VNC) | test-artifacts/nested-03-selected.png |
| PASS | clipboard nested -> viewer (wl-copy in sway -> ServerCutText) | viewer received 'from-sway-123' |  |
| PASS | clipboard viewer -> nested (ClientCutText -> wl-paste and wl-paste --primary in sway) | sway clipboard='from-vnc-to-sway-456' primary='from-vnc-to-sway-456' |  |
| PASS | clipboard viewer -> paste into foot inside sway with Ctrl+Shift+V | file content 'pasted-42' |  |
| PASS | client resize: nested output follows (1024x768 -> 1280x720) | viewer fb 1280x720, sway outputs [('WL-1', 0, 0, 1280, 720)], foot covers 100% | test-artifacts/nested-04-resized-1280x720.png |
| PASS | multi-monitor: two-screen client layout -> two nested outputs (sway WLR_WL_OUTPUTS=2) | viewer fb 1600x600; sway outputs [('WL-2', 0, 0, 800, 600), ('WL-1', 800, 0, 800, 600)]; foot is on WL-2; left screen foot 0%/background 100%, right screen foot 100%/background 0% | test-artifacts/nested-05-two-screens.png |
| PASS | labwc session: detected, output fills the screen, clipboard both ways, resize follows | detected=True; labwc output (1024, 768), our background visible on 0.0%; clipboard labwc->viewer 'from-labwc-77', viewer->labwc 'to-labwc-88'; after resize labwc output (1280, 720), our background 0.0% | test-artifacts/nested-06-labwc.png, test-artifacts/nested-07-labwc-1280x720.png |
| PASS | NestedCompositor=Off: sway is managed as an ordinary window (floating WM fallback) | detected=False; sway output [('WL-1', 0, 0, 1280, 720)]; our background visible on 6% | test-artifacts/nested-08-nestedcompositor-off.png |

## systemd

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | vncserver@:1.service, mode=wayland: Wayland session, logind session Type=wayland | systemctl start rc=0, active=active, logind sessions=[{'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'wayland', 'Class': 'user', 'State': 'active'}], session XDG_RUNTIME_DIR='/run/user/1001' WAYLAND_DISPLAY='wayland-1', foot (302, 332, 720, 512), xterm (20, 22, 322, 128) | test-artifacts/systemd-01-wayland-unit.png |
| PASS | vncserver@:2.service, no mode: X11 session, logind Type=x11 | active=active, logind sessions=[{'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'wayland', 'Class': 'user', 'State': 'active'}, {'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'x11', 'Class': 'user', 'State': 'active'}], xterm (42, 42, 404, 174) | test-artifacts/systemd-02-x11-unit.png |
| PASS | systemctl stop ends both sessions | after stop: :1 inactive, remaining tigervnc sessions [] |  |

## vncsession

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | vncsession with mode=wayland starts Xvnc-wayland as the user, XDG_SESSION_TYPE=wayland for PAM | vncsession rc=0 pid=10, port 5901 up=True, servers=[(12, 'Xvnc-wayland', 'vncuser')], server env from vncsession: XDG_SESSION_TYPE='wayland' XDG_SESSION_CLASS='user'  |  |
| PASS | Wayland session environment | USER=vncuser, XDG_SESSION_TYPE=wayland, WAYLAND_DISPLAY=wayland-1, DISPLAY=:0, XDG_CURRENT_DESKTOP=VncTest, XDG_SESSION_DESKTOP=vnc-test, DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-GAOiZqPohM,guid=6276f56a4368558196ebd22f6af3327c |  |
| PASS | Wayland session visible over VNC (foot + xterm) | foot area (302, 332, 720, 512), xterm area (20, 22, 322, 128) | test-artifacts/vncsession-01-wayland-session.png |
| PASS | session log | /home/vncuser/.local/state/tigervnc/ce952aa32e75:1.log (copied to logs/vncsession-wayland-user.log) |  |
| PASS | stop | vncsession exited and removed its pid file after the server stopped |  |
| PASS | vncsession without mode keeps X11 (Xvnc + Xsession) | no mode: server env XDG_SESSION_TYPE='x11', servers=[(65, 'Xvnc', 'vncuser')], session DISPLAY=':2' | test-artifacts/vncsession-02-x11-default.png |
| PASS | vncsession starts the stock sway session (sway.desktop) as a nested compositor | stock sway.desktop via vncsession: rc=0, sway outputs [('WL-1', 1024, 768)], server log ['WaylandDesktop: Client pid 129 (wlroots) is a nested compositor serving', 'WaylandDesktop: Clipboard bridge to nested compositor (pid 129) ready,'] (log copied to logs/vncsession-sway-user.log) | test-artifacts/vncsession-03-sway-session.png |
| PASS | stock sway session: Super+Return opens foot, typed command runs inside sway, resize follows | Super+Return + typed command in sway's terminal: 'sway-session-42-sway'; after resize to 1280x800 sway outputs [('WL-1', 1280, 800)] | test-artifacts/vncsession-04-sway-terminal.png |
| PASS | explicit mode=x11 behaves like the default | mode=x11: servers=[(201, 'Xvnc', 'vncuser')], XDG_SESSION_TYPE='x11', Xvnc args '/usr/bin/Xvnc :3 -geometry 1024x768 -localhost -securitytypes VncAuth -auth /home/vncuser/.Xauthority -desktop ce952aa32e75:3 (vncuser) -fp /usr/share/fonts/X11//misc -pn -rfbauth /home/vncuser/.config/tigervnc/passwd -rfbport 5903' |  |
| PASS | invalid mode is rejected by vncsession and vncserver | vncsession rc=71, servers=[]; vncserver rc=255: "Invalid mode 'wayland2', must be 'x11' or 'wayland'" |  |
| PASS | mode value is case-insensitive and whitespace-trimmed | 'mode = Wayland ': servers=[(264, 'Xvnc-wayland', 'vncuser')] XDG_SESSION_TYPE='wayland' |  |

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
| PASS | Wayland client renders (foot) | foot content area (296, 320, 726, 526) (430x206) | test-artifacts/wayland-02-foot.png |
| PASS | X11 clients render via Xwayland (xterm, xeyes) | xterm white area (560, 430, 862, 562) (want origin 560,430), xdotool windows=['6291468'], xeyes colours=162, appeared after 2.5s | test-artifacts/wayland-03-foot-xterm-xeyes.png |
| PASS | keyboard input reaches Wayland client (foot) | file content 'wl-key-ok-42-foot' | test-artifacts/wayland-04-foot-typed.png |
| PASS | keyboard input reaches X11 client (xterm via Xwayland) | file content 'x-key-ok-42-XTerm(407)' | test-artifacts/wayland-05-xterm-typed.png |
| PASS | pointer motion+buttons reach Wayland client (foot word selection by double click) | primary selection after double click in foot: 'PTRWLTEST' | test-artifacts/wayland-06-foot-selected.png |
| PASS | pointer motion+buttons reach X11 client (xterm selection, xdotool pointer position) | X PRIMARY='PTRXTEST', Wayland primary='PTRXTEST', X pointer='x:637 y:463 screen:0 window:6291468' (want x:637 y:463) | test-artifacts/wayland-07-xterm-selected.png |
| PASS | clipboard Wayland -> viewer (wl-copy -> ServerCutText) | viewer received 'from-wayland-123' |  |
| PASS | primary selection Wayland -> viewer (wl-copy --primary) | viewer received 'primary-wayland-321' (SendPrimary default on) |  |
| PASS | clipboard viewer -> Wayland+X11 (ClientCutText -> wl-paste, wl-paste --primary, xclip) | Wayland clipboard='from-vnc-456' primary='from-vnc-456'; X11 CLIPBOARD='from-vnc-456' PRIMARY='from-vnc-456' (SetPrimary default on) |  |
| PASS | clipboard X11 app -> viewer (xclip via Xwayland -> ServerCutText) | viewer received 'from-x11-789' |  |
| PASS | clipboard viewer -> paste into foot with Ctrl+Shift+V | file content 'pasted-42' |  |
| PASS | client resize 1024x768 -> 1280x720 changes the output (viewers, wl_output mode, Xwayland screen) | viewer fb 1280x720, ExtendedDesktopSize reply status=0, second viewer 1280x720, wl_output mode (1280, 720), Xwayland 'dimensions:    1280x720 pixels (338x190 millimeters)' | test-artifacts/wayland-08-resized-1280x720.png |
| PASS | client resize to 1000x700 | viewer fb 1000x700, wl_output mode (1000, 700), foot area (296, 320, 726, 526) | test-artifacts/wayland-09-resized-1000x700.png |
| PASS | input still works after resize | file content 'after-resize' |  |
| PASS | second Wayland client (weston-terminal) starts | weston-terminal pid 62, 88650 sampled pixels changed | test-artifacts/wayland-10-weston-terminal.png |
| PASS | Xwayland uses a private X authority file (XAUTHORITY for the session, clients without the cookie refused) | session XAUTHORITY=/tmp/wltest/xdg-runtime/tigervnc-wayland-1.Xauthority mode=0o600; xdpyinfo with it rc=0, without it rc=1 (); Xwayland args 'Xwayland :0 -auth /tmp/wltest/xdg-runtime/tigervnc-wayland-1.Xauthority -rootless -core -terminate -listenfd 15 -listenfd 16 -displayfd 25 -wm 22' |  |
| PASS | server survived the whole run and exits on SIGTERM | server alive until SIGTERM, exit code 0 |  |
| PASS | -inetd (connection on stdin/stdout) | RFB over inetd stdin/stdout, 640x480 | test-artifacts/wayland-11-inetd-640x480.png |
| PASS | display persists after the session command exits (like Xvnc) | session exited (status 3), server still running=True, viewer connected afterwards at 640x480 |  |
| PASS | -ExitWithSession stops the server with the session (used by vncserver) | -ExitWithSession: server exited with 0 after the session |  |
| PASS | -depth 16: native RGB565 framebuffer, picture correct | server pixel format bpp=16 depth=16; foot found at (200, 214, 438, 342); background (49, 52, 65) | test-artifacts/wayland-12-depth16.png |
| PASS | X-only options (-dpi -ac -fp -noreset -pn) are ignored with a warning instead of refused | server started=True, warnings for ['-dpi', '-ac', '-fp', '-noreset', '-pn'] |  |
| PASS | AvoidShiftNumLock: no fake Shift on keypad keys (xev); keypad keysyms unreachable without NumLock use the plain key | XK_KP_4 (NumLock off), Num_Lock, XK_KP_Left (NumLock on), Num_Lock: default -> X11 key presses [('13', '4'), ('77', 'Num_Lock'), ('113', 'Left'), ('77', 'Num_Lock')] (want [('13', '4'), ('77', 'Num_Lock'), ('113', 'Left'), ('77', 'Num_Lock')]: the arrow key instead of a fake Shift); -AvoidShiftNumLock=0 -> [('13', '4'), ('77', 'Num_Lock'), ('113', 'Left'), ('77', 'Num_Lock')] (want [('13', '4'), ( |  |
| PASS | built-in WM with two screens: windows open, maximize and go fullscreen on one screen (Wayland and X11) | screens 0+0 800x600 and 800+0 800x600: fullscreen foot (pointer right) (800, 0, 1598, 598); maximized foot (pointer left) (0, 26, 798, 598); xterm -fullscreen at +900+100 (800, 0, 1598, 598) | test-artifacts/wayland-13-two-screens-fullscreen-right.png, test-artifacts/wayland-14-two-screens-maximized-left.png, test-artifacts/wayland-15-two-screens-xterm-fullscreen-right.png |
| PASS | wlr-output-management: wlr-randr --custom-mode resizes the desktop for all viewers; unsupported changes are refused | wlr-randr --custom-mode 800x600 rc=0 -> viewer 800x600, wlr-randr current (800, 600), Xwayland 'dimensions:    800x600 pixels (211x158 millimeters)'; --scale 2 rejected rc=1 '' | test-artifacts/wayland-16-wlr-randr-800x600.png |
| PASS | X11 title bars: window title drawn as text (pixel exact, follows renames), resize by edges and corners, drag moves, maximize/restore and close buttons work | xterm at (100, 100, 184, 108), title bar pixel (51, 84, 135); title text 'Title Test 42' drawn (0 pixels differ); after renaming, title text 'Renamed-xterm' drawn (0 pixels differ); right edge drag by +100,+0 -> (100, 100, 284, 108) (want (100, 100, 284, 108)); bottom-right corner drag by +50,+40 -> (100, 100, 334, 148) (want (100, 100, 334, 148)); left edge drag by -60,+0 -> (40, 100, 394, 148) ( | test-artifacts/wayland-17-x11-title-bar.png, test-artifacts/wayland-20-x11-title-renamed.png, test-artifacts/wayland-21-x11-title-bar-resized.png, test-artifacts/wayland-18-x11-title-bar-moved.png, test-artifacts/wayland-19-x11-title-bar-maximized.png |
| PASS | vncconfig works in Wayland mode (control socket): -list -get -desc -set/AllowOverride, desktop rename, -disconnect, -connect | control socket /tmp/wltest/xdg-runtime/tigervnc-wayland-12.vncconfig mode 0o700; -list rc=0, 56 names; -get SendCutText -> 'on'; -get Password refused (rc=1 'vncconfig: No such parameter\nvncconfig: Getting the value of parameter Password failed'); -desc SendCutText -> 'Send clipboard changes to clients.'; -set SendCutText=0 rc=0, now 'off'; with SendCutText=0 the viewer got None; SendCutText=1 rc |  |
| PASS | QueryConnect with the vncconfig prompt (as on Xvnc): rejected without vncconfig, Accept/Reject/busy/timeout with it, -nowin; vncconfig window shows and sets the clipboard parameters | no prompt running: viewer -> 'error:authentication failed: Unable to query the local user to accept the connection.'; observer: reverse connection (no query, as on Xvnc); vncconfig window on screen at (0, 22, 300, 100); checkboxes show the server's settings (dark pixels in boxes {'AcceptCutText': 0, 'SetPrimary': 17, 'SendCutText': 14, 'SendPrimary': 12}); clicking 'Send clipboard' -> SendCutText= | test-artifacts/wayland-22-vncconfig-window.png, test-artifacts/wayland-23-vncconfig-toggled.png, test-artifacts/wayland-24-queryconnect-dialog.png, test-artifacts/wayland-25-queryconnect-nowin.png |

## x11

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | Xvnc does not link wlroots | Xvnc links wlroots/wayland-server: [] |  |
| PASS | Xvnc -help (X server usage + mode flags) | Xvnc -help lists X server options, VNC parameters and the --x11/--wayland lines (logs/x11-Xvnc-help.txt) |  |
| PASS | Xvnc -version | Xvnc TigerVNC 1.16.2 - built Nov 10 2026 13:45:41 / Copyright (C) 1999-2026 TigerVNC team and many others (see README.rst) / See https://www.tigervnc.org for information on TigerVNC. / Underlying X server release 12101018 |  |
| PASS | Xvnc :2 starts (no flag) | port 5902 up=True; dimensions: 1024x768 pixels (271x203 millimeters) depth of root window: 24 planes |  |
| PASS | auth: VncAuth + X509Vnc, wrong password rejected | VncAuth ok, wrong password rejected, X509Vnc ok (TLSv1.3) |  |
| PASS | X11 clients render (xterm, xeyes) | xterm area (102, 102, 464, 260), xeyes colours 161 | test-artifacts/x11-01-xterm-xeyes.png |
| PASS | keyboard input reaches xterm | file content 'x11-key-ok-42-XTerm(407)' | test-artifacts/x11-02-typed.png |
| PASS | pointer input reaches xterm (selection, pointer position) | PRIMARY='PTRX11TEST' pointer='x:333 y:444 screen:0 window:555' | test-artifacts/x11-03-selected.png |
| PASS | clipboard both ways (X11) | viewer->X CLIPBOARD='x11-from-vnc', X->viewer='x11-from-xclip' |  |
| PASS | client resize (X11, RandR) | viewer 1280x720, X screen dimensions: 1280x720 pixels (338x190 millimeters) | test-artifacts/x11-04-resized-1280x720.png |
| PASS | Xvnc --x11 runs the X server | Xvnc --x11 :3 -> vendor string: The X.Org Foundation dimensions: 800x600 pixels (212x159 millimeters) | test-artifacts/x11-05-x11-flag-800x600.png |
| PASS | Xvnc --x11 --wayland is rejected | Xvnc: --x11 and --wayland cannot be combined |  |

**72 tests, 72 passed, 0 failed**
