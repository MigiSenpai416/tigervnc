# Test results

## systemd

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | vncserver@:1.service, mode=wayland: Wayland session, logind session Type=wayland | systemctl start rc=0, active=active, logind sessions=[{'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'wayland', 'Class': 'user', 'State': 'active'}], session XDG_RUNTIME_DIR='/run/user/1001' WAYLAND_DISPLAY='wayland-1', foot (302, 332, 720, 512), xterm (20, 20, 322, 126) | test-artifacts/systemd-01-wayland-unit.png |
| PASS | vncserver@:2.service, no mode: X11 session, logind Type=x11 | active=active, logind sessions=[{'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'wayland', 'Class': 'user', 'State': 'active'}, {'Name': 'vncuser', 'Service': 'tigervnc', 'Type': 'x11', 'Class': 'user', 'State': 'active'}], xterm (42, 42, 404, 174) | test-artifacts/systemd-02-x11-unit.png |
| PASS | systemctl stop ends both sessions | after stop: :1 inactive, remaining tigervnc sessions [] |  |

## vncsession

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | vncsession with mode=wayland starts Xvnc-wayland as the user, XDG_SESSION_TYPE=wayland for PAM | vncsession rc=0 pid=10, port 5901 up=True, servers=[(12, 'Xvnc-wayland', 'vncuser')], server env from vncsession: XDG_SESSION_TYPE='wayland' XDG_SESSION_CLASS='user'  |  |
| PASS | Wayland session environment | USER=vncuser, XDG_SESSION_TYPE=wayland, WAYLAND_DISPLAY=wayland-1, DISPLAY=:0, XDG_CURRENT_DESKTOP=VncTest, XDG_SESSION_DESKTOP=vnc-test, DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-DBrfhpYI5E,guid=55ce23c38b077c6899ea75286af2d7d4 |  |
| PASS | Wayland session visible over VNC (foot + xterm) | foot area (302, 332, 720, 512), xterm area (20, 20, 322, 126) | test-artifacts/vncsession-01-wayland-session.png |
| PASS | session log | /home/vncuser/.local/state/tigervnc/7af06f2560be:1.log (copied to logs/vncsession-wayland-user.log) |  |
| PASS | stop | vncsession exited and removed its pid file after the server stopped |  |
| PASS | vncsession without mode keeps X11 (Xvnc + Xsession) | no mode: server env XDG_SESSION_TYPE='x11', servers=[(65, 'Xvnc', 'vncuser')], session DISPLAY=':2' | test-artifacts/vncsession-02-x11-default.png |

## wayland

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | headless start (no GPU, no parent display), session started with WAYLAND_DISPLAY+DISPLAY | session env WAYLAND_DISPLAY=wayland-1 DISPLAY=:0 XDG_SESSION_TYPE=wayland; /dev/dri present=False |  |
| PASS | auth: VncAuth with correct password | offered=[19, 2] name='wayland-test' size=1024x768 |  |
| PASS | auth: VncAuth with wrong password rejected | rejected: authentication failed: Authentication failed |  |
| PASS | auth: X509Vnc (VeNCrypt, X.509 TLS + VNC password) | subtypes=[2, 261, 258] tls=TLSv1.3 cipher=TLS_AES_256_GCM_SHA384 cert=781 bytes |  |
| PASS | auth: TLSVnc (VeNCrypt, anonymous TLS + VNC password) | tls=TLSv1.2 cipher=AECDH-AES256-SHA |  |
| PASS | auth: security type None not offered when not configured | security type None not offered (offered [19, 2]) |  |
| PASS | -localhost: not reachable on external address | connect to 172.17.0.3:5901 refused ([Errno 111] Connection refused) |  |
| PASS | Wayland client renders (foot) | foot content area (296, 320, 726, 526) (430x206) | test-artifacts/wayland-02-foot.png |
| PASS | X11 clients render via Xwayland (xterm, xeyes) | xterm white area (560, 430, 862, 562) (want origin 560,430), xdotool windows=['6291468'], xeyes colours=161, appeared after 2.5s | test-artifacts/wayland-03-foot-xterm-xeyes.png |
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
| PASS | second Wayland client (weston-terminal) starts | weston-terminal pid 63, 88643 sampled pixels changed | test-artifacts/wayland-10-weston-terminal.png |
| PASS | server survived the whole run and exits on SIGTERM | server alive until SIGTERM, exit code 0 |  |
| PASS | -inetd (connection on stdin/stdout) | RFB over inetd stdin/stdout, 640x480 | test-artifacts/wayland-11-inetd-640x480.png |

## x11

| Result | Test | Detail | Artifacts |
|---|---|---|---|
| PASS | Xvnc does not link wlroots | Xvnc links wlroots/wayland-server: [] |  |
| PASS | Xvnc -help (X server usage + mode flags) | Xvnc -help lists X server options, VNC parameters and the --x11/--wayland lines (logs/x11-Xvnc-help.txt) |  |
| PASS | Xvnc -version | Xvnc TigerVNC 1.16.80 - built 2026-11-10 08:36 / Copyright (C) 1999-2026 TigerVNC team and many others (see README.rst) / See https://www.tigervnc.org for information on TigerVNC. / Underlying X server release 12101022 |  |
| PASS | Xvnc :2 starts (no flag) | port 5902 up=True; dimensions: 1024x768 pixels (271x203 millimeters) depth of root window: 24 planes |  |
| PASS | auth: VncAuth + X509Vnc, wrong password rejected | VncAuth ok, wrong password rejected, X509Vnc ok (TLSv1.3) |  |
| PASS | X11 clients render (xterm, xeyes) | xterm area (102, 102, 464, 260), xeyes colours 161 | test-artifacts/x11-01-xterm-xeyes.png |
| PASS | keyboard input reaches xterm | file content 'x11-key-ok-42-XTerm(407)' | test-artifacts/x11-02-typed.png |
| PASS | pointer input reaches xterm (selection, pointer position) | PRIMARY='PTRX11TEST' pointer='x:333 y:444 screen:0 window:555' | test-artifacts/x11-03-selected.png |
| PASS | clipboard both ways (X11) | viewer->X CLIPBOARD='x11-from-vnc', X->viewer='x11-from-xclip' |  |
| PASS | client resize (X11, RandR) | viewer 1280x720, X screen dimensions: 1280x720 pixels (338x190 millimeters) | test-artifacts/x11-04-resized-1280x720.png |
| PASS | Xvnc --x11 runs the X server | Xvnc --x11 :3 -> vendor string: The X.Org Foundation dimensions: 800x600 pixels (212x159 millimeters) | test-artifacts/x11-05-x11-flag-800x600.png |
| PASS | Xvnc --x11 --wayland is rejected | Xvnc: --x11 and --wayland cannot be combined |  |

**45 tests, 45 passed, 0 failed**
