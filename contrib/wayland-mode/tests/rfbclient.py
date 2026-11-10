#!/usr/bin/env python3
"""Minimal RFB (VNC) test client, standard library only.

Used by the Wayland mode test suite to talk to Xvnc exactly like a
viewer would: security negotiation (None, VncAuth, VeNCrypt with
TLSNone/TLSVnc/TLSPlain/X509None/X509Vnc/X509Plain), raw framebuffer
updates saved as PNG, key and pointer events, cut text in both
directions and SetDesktopSize (client-driven resize).
"""

import socket
import ssl
import struct
import time
import zlib

# ---------------------------------------------------------------- DES
# Plain DES (encryption only) for VNC authentication.

_PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51,
        43, 35, 27, 19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,
        62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20,
        12, 4]
_PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16,
        7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44,
        49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
_SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
_IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54,
       46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33,
       25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21,
       13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
_FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6,
       46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12,
       52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18,
       58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
_E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14,
      15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26,
      27, 28, 29, 28, 29, 30, 31, 32, 1]
_P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24,
      14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
_S = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2,
     13, 1, 10, 6, 12, 11, 9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9,
     7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2,
     8, 14, 12, 0, 1, 10, 6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12,
     6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4,
     6, 10, 2, 8, 5, 14, 12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2,
     12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15,
     0, 3, 4, 7, 2, 12, 1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3,
     14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7,
     13, 1, 5, 0, 15, 10, 3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5,
     6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12,
     9, 5, 6, 1, 13, 14, 0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10,
     1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9,
     1, 10, 14, 3, 5, 12, 2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6,
     8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3,
     7, 4, 12, 5, 6, 11, 0, 14, 9, 2, 7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13,
     15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def _bits(data):
    return [(data[i // 8] >> (7 - i % 8)) & 1 for i in range(len(data) * 8)]


def _perm(bits, table):
    return [bits[t - 1] for t in table]


def des_encrypt_block(key, block):
    k = _perm(_bits(key), _PC1)
    c, d = k[:28], k[28:]
    subkeys = []
    for s in _SHIFTS:
        c, d = c[s:] + c[:s], d[s:] + d[:s]
        subkeys.append(_perm(c + d, _PC2))
    b = _perm(_bits(block), _IP)
    left, right = b[:32], b[32:]
    for sk in subkeys:
        e = [x ^ y for x, y in zip(_perm(right, _E), sk)]
        out = []
        for i in range(8):
            chunk = e[i * 6:(i + 1) * 6]
            row = (chunk[0] << 1) | chunk[5]
            col = (chunk[1] << 3) | (chunk[2] << 2) | (chunk[3] << 1) | chunk[4]
            v = _S[i][row * 16 + col]
            out += [(v >> 3) & 1, (v >> 2) & 1, (v >> 1) & 1, v & 1]
        f = _perm(out, _P)
        left, right = right, [x ^ y for x, y in zip(left, f)]
    bits = _perm(right + left, _FP)
    return bytes(sum(bits[i * 8 + j] << (7 - j) for j in range(8))
                 for i in range(8))


def vnc_auth_response(password, challenge):
    key = (password.encode("latin-1") + b"\0" * 8)[:8]
    # VNC uses the key bytes bit-reversed
    key = bytes(int("{:08b}".format(b)[::-1], 2) for b in key)
    return (des_encrypt_block(key, challenge[:8]) +
            des_encrypt_block(key, challenge[8:16]))


# ---------------------------------------------------------------- PNG

def write_png(path, width, height, rgb):
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                           8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


# ---------------------------------------------------------------- RFB

SEC_NONE = 1
SEC_VNC = 2
SEC_VENCRYPT = 19
VENCRYPT = {
    "Plain": 256, "TLSNone": 257, "TLSVnc": 258, "TLSPlain": 259,
    "X509None": 260, "X509Vnc": 261, "X509Plain": 262,
}

ENC_RAW = 0
ENC_DESKTOP_SIZE = -223
ENC_LAST_RECT = -224
ENC_EXT_DESKTOP_SIZE = -308
ENC_LED_STATE = -261

XK_SHIFT_L = 0xffe1
XK_RETURN = 0xff0d
XK_BACKSPACE = 0xff08
XK_TAB = 0xff09
XK_ESCAPE = 0xff1b
XK_CONTROL_L = 0xffe3

_SHIFTED = '~!@#$%^&*()_+{}|:"<>?'


class RFBError(Exception):
    pass


class RFBClient:
    def __init__(self, host="127.0.0.1", port=5900, security="VncAuth",
                 password=None, username=None, timeout=10.0,
                 unix_path=None, sock=None, led_state=False):
        self.log = []
        if sock is not None:
            self.sock = sock
            self.sock.settimeout(timeout)
        elif unix_path:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect(unix_path)
        else:
            self.sock = socket.create_connection((host, port), timeout)
        self.timeout = timeout
        self.cut_texts = []
        self.bells = 0
        self.ext_desktop_size = []   # (reason, status, w, h, screens)
        self._led_state = led_state
        self._handshake(security, password, username)

    # -- low level
    def _recv(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RFBError("connection closed by server")
            buf += chunk
        return bytes(buf)

    def _send(self, data):
        self.sock.sendall(data)

    def _u8(self):
        return self._recv(1)[0]

    def _u32(self):
        return struct.unpack(">I", self._recv(4))[0]

    def _reason(self):
        n = self._u32()
        return self._recv(n).decode("utf-8", "replace")

    # -- handshake
    def _handshake(self, security, password, username):
        ver = self._recv(12)
        if not ver.startswith(b"RFB "):
            raise RFBError("not an RFB server: %r" % ver)
        self.server_version = ver.decode().strip()
        self._send(b"RFB 003.008\n")
        n = self._u8()
        if n == 0:
            raise RFBError("server refused: " + self._reason())
        types = list(self._recv(n))
        self.offered_types = types

        if security in ("None",):
            want = SEC_NONE
        elif security in ("VncAuth", "VNC"):
            want = SEC_VNC
        elif security in VENCRYPT:
            want = SEC_VENCRYPT
        else:
            raise RFBError("unknown security type " + security)
        if want not in types:
            raise RFBError("security type %s not offered (offered %r)" %
                           (security, types))
        self._send(bytes([want]))

        if want == SEC_VNC:
            self._vnc_auth(password)
        elif want == SEC_VENCRYPT:
            self._vencrypt(security, password, username)

        result = self._u32()
        if result != 0:
            raise RFBError("authentication failed: " + self._reason())
        self.security = security

        self._send(b"\x01")  # shared
        w, h = struct.unpack(">HH", self._recv(4))
        self.server_pf = self._recv(16)
        self.name = self._recv(self._u32()).decode("utf-8", "replace")
        self.width, self.height = w, h
        self.fb = bytearray(w * h * 4)

        # 32bpp little endian true colour, red at shift 16 (BGRX bytes)
        pf = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255,
                         16, 8, 0)
        self._send(struct.pack(">B3x", 0) + pf)
        encs = [ENC_RAW, ENC_DESKTOP_SIZE, ENC_EXT_DESKTOP_SIZE, ENC_LAST_RECT]
        self.leds = None
        if self._led_state:
            # the server then leaves lock key state to the client
            encs.append(ENC_LED_STATE)
        self._send(struct.pack(">BxH", 2, len(encs)) +
                   b"".join(struct.pack(">i", e) for e in encs))

    def _vnc_auth(self, password):
        challenge = self._recv(16)
        if password is None:
            raise RFBError("VncAuth needs a password")
        self._send(vnc_auth_response(password, challenge))

    def _vencrypt(self, security, password, username):
        major, minor = self._recv(2)
        if (major, minor) < (0, 2):
            raise RFBError("unsupported VeNCrypt version %d.%d" %
                           (major, minor))
        self._send(b"\x00\x02")
        if self._u8() != 0:
            raise RFBError("server rejected VeNCrypt version")
        n = self._u8()
        subtypes = [self._u32() for _ in range(n)]
        self.offered_subtypes = subtypes
        sub = VENCRYPT[security]
        if sub not in subtypes:
            raise RFBError("VeNCrypt subtype %s not offered (offered %r)" %
                           (security, subtypes))
        self._send(struct.pack(">I", sub))
        if self._u8() != 1:
            raise RFBError("server rejected VeNCrypt subtype")

        if security.startswith("TLS") or security.startswith("X509"):
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            if security.startswith("TLS"):
                # anonymous Diffie-Hellman, not available in TLS 1.3
                ctx.maximum_version = ssl.TLSVersion.TLSv1_2
                ctx.set_ciphers("aNULL:@SECLEVEL=0")
            self.sock = ctx.wrap_socket(self.sock, do_handshake_on_connect=True)
            self.tls_version = self.sock.version()
            self.tls_cipher = self.sock.cipher()
            if security.startswith("X509"):
                self.server_cert = self.sock.getpeercert(binary_form=True)

        if security.endswith("Vnc"):
            self._vnc_auth(password)
        elif security.endswith("Plain"):
            u = (username or "").encode()
            p = (password or "").encode()
            self._send(struct.pack(">II", len(u), len(p)) + u + p)

    # -- client messages
    def request_update(self, incremental=True, x=0, y=0, w=None, h=None):
        w = self.width if w is None else w
        h = self.height if h is None else h
        self._send(struct.pack(">BBHHHH", 3, 1 if incremental else 0,
                               x, y, w, h))

    def key(self, keysym, down):
        self._send(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    def tap(self, keysym):
        self.key(keysym, True)
        self.key(keysym, False)

    def type_text(self, text, delay=0.01):
        for ch in text:
            if ch == "\n":
                self.tap(XK_RETURN)
            else:
                shifted = ch.isupper() or ch in _SHIFTED
                if shifted:
                    self.key(XK_SHIFT_L, True)
                self.tap(ord(ch))
                if shifted:
                    self.key(XK_SHIFT_L, False)
            if delay:
                time.sleep(delay)

    def pointer(self, x, y, mask=0):
        self._send(struct.pack(">BBHH", 5, mask, x, y))

    def click(self, x, y, button=1):
        bit = 1 << (button - 1)
        self.pointer(x, y, 0)
        self.pointer(x, y, bit)
        self.pointer(x, y, 0)

    def drag(self, x0, y0, x1, y1, button=1, steps=10, delay=0.02):
        bit = 1 << (button - 1)
        self.pointer(x0, y0, 0)
        time.sleep(delay)
        self.pointer(x0, y0, bit)
        for i in range(1, steps + 1):
            time.sleep(delay)
            self.pointer(x0 + (x1 - x0) * i // steps,
                         y0 + (y1 - y0) * i // steps, bit)
        time.sleep(delay)
        self.pointer(x1, y1, 0)

    def cut_text(self, text):
        data = text.encode("latin-1")
        self._send(struct.pack(">BxxxI", 6, len(data)) + data)

    def set_desktop_size(self, w, h, screens=None):
        if screens is None:
            screens = [(0, 0, 0, w, h, 0)]
        msg = struct.pack(">BxHHBx", 251, w, h, len(screens))
        for sid, sx, sy, sw, sh, flags in screens:
            msg += struct.pack(">IHHHHI", sid, sx, sy, sw, sh, flags)
        self._send(msg)

    # -- server messages
    def _read_message(self):
        t = self._u8()
        if t == 0:
            self._read_update()
            return "update"
        if t == 1:
            self._recv(3)
            n = struct.unpack(">H", self._recv(2))[0]
            self._recv(n * 6)
            return "colourmap"
        if t == 2:
            self.bells += 1
            return "bell"
        if t == 3:
            self._recv(3)
            n = struct.unpack(">i", self._recv(4))[0]
            if n < 0:
                raise RFBError("unexpected extended clipboard message")
            self.cut_texts.append(self._recv(n).decode("latin-1"))
            return "cuttext"
        if t == 150:
            return "eocu"
        raise RFBError("unknown server message type %d" % t)

    def _read_update(self):
        self._recv(1)
        n = struct.unpack(">H", self._recv(2))[0]
        for _ in range(n):
            x, y, w, h, enc = struct.unpack(">HHHHi", self._recv(12))
            if enc == ENC_RAW:
                data = self._recv(w * h * 4)
                for row in range(h):
                    off = ((y + row) * self.width + x) * 4
                    self.fb[off:off + w * 4] = data[row * w * 4:(row + 1) * w * 4]
            elif enc == ENC_DESKTOP_SIZE:
                self._resize_fb(w, h)
            elif enc == ENC_EXT_DESKTOP_SIZE:
                ns = self._u8()
                self._recv(3)
                screens = [struct.unpack(">IHHHHI", self._recv(16))
                           for _ in range(ns)]
                self.ext_desktop_size.append((x, y, w, h, screens))
                if y == 0:
                    self._resize_fb(w, h)
            elif enc == ENC_LED_STATE:
                self.leds = self._u8()
            elif enc == ENC_LAST_RECT:
                break
            else:
                raise RFBError("unexpected encoding %d" % enc)

    def _resize_fb(self, w, h):
        if (w, h) != (self.width, self.height):
            self.width, self.height = w, h
            self.fb = bytearray(w * h * 4)

    def pump(self, seconds):
        """Process server messages for the given time."""
        end = time.time() + seconds
        seen = []
        while True:
            left = end - time.time()
            if left <= 0:
                return seen
            self.sock.settimeout(left)
            try:
                seen.append(self._read_message())
            except (socket.timeout, TimeoutError):
                return seen
            except ssl.SSLError as e:
                if "timed out" in str(e):
                    return seen
                raise
            finally:
                self.sock.settimeout(self.timeout)

    def refresh(self, settle=0.5, full=True):
        """Get an up to date framebuffer: full update, then keep
        asking for incremental updates until nothing changes for
        `settle` seconds."""
        self.request_update(incremental=not full)
        self._wait_for("update", self.timeout)
        while True:
            self.request_update(incremental=True)
            seen = self.pump(settle)
            if "update" not in seen:
                return
            # an update arrived; there may be more

    def _wait_for(self, kind, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if kind in self.pump(min(0.5, end - time.time())):
                return True
        raise RFBError("timed out waiting for " + kind)

    def wait_cut_text(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.cut_texts:
                return self.cut_texts.pop(0)
            self.request_update(incremental=True)
            self.pump(0.3)
        return None

    # -- framebuffer helpers
    def pixel(self, x, y):
        off = (y * self.width + x) * 4
        b, g, r = self.fb[off], self.fb[off + 1], self.fb[off + 2]
        return (r, g, b)

    def rgb(self):
        out = bytearray(self.width * self.height * 3)
        out[0::3] = self.fb[2::4]
        out[1::3] = self.fb[1::4]
        out[2::3] = self.fb[0::4]
        return out

    def screenshot(self, path):
        write_png(path, self.width, self.height, self.rgb())
        return path

    def colour_count(self, x, y, w, h):
        """Number of distinct colours in a region (cheap content check)."""
        seen = set()
        for row in range(y, y + h):
            off = (row * self.width + x) * 4
            line = self.fb[off:off + w * 4]
            for i in range(0, len(line), 4):
                seen.add(bytes(line[i:i + 3]))
        return len(seen)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="RFB test client")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5901)
    ap.add_argument("--security", default="VncAuth")
    ap.add_argument("--password")
    ap.add_argument("--user")
    ap.add_argument("--screenshot")
    a = ap.parse_args()
    c = RFBClient(a.host, a.port, a.security, a.password, a.user)
    print("connected:", c.server_version, c.name, c.width, c.height)
    c.refresh()
    if a.screenshot:
        c.screenshot(a.screenshot)
        print("saved", a.screenshot)
