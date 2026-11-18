"""Connection to the key-monitor serial port of a TEST firmware build.

Protocol (see zmk-modules/key-monitor/Kconfig):
  device -> host  "H key-monitor <ver> inject=<0|1> keys=<n>", "P <pos> <1|0>",
                  "L <layer mask hex> <highest layer>", "E <error>"
  host -> device  "?", "K <pos> <1|0>"
"""

import os
import select
import termios
import threading
import time
from pathlib import Path

VID = "29ea"
LEFT_PRODUCT = "Adv360 Pro"  # the central half; the right one is "Adv360 Pro rt"


def candidate_ports():
    """ttyACM devices of the left half, highest USB interface first (the
    key-monitor port comes after the Studio one)."""
    ports = []
    for tty in Path("/sys/class/tty").glob("ttyACM*"):
        iface = (tty / "device").resolve()
        usbdev = iface.parent
        try:
            if (usbdev / "idVendor").read_text().strip() != VID:
                continue
            if (usbdev / "product").read_text().strip() != LEFT_PRODUCT:
                continue
            num = int((iface / "bInterfaceNumber").read_text(), 16)
        except OSError:
            continue
        ports.append((num, f"/dev/{tty.name}"))
    return [p for _, p in sorted(ports, reverse=True)]


def open_raw(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    # Always set the speed explicitly: Linux remembers the last speed per
    # ttyACM number, and 1200 baud reboots the keyboard into its bootloader.
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


class KeyMonitor:
    """Thread-safe connection. `on_message(str)` gets every line from the
    device plus synthetic "connected <port>" / "disconnected" lines."""

    def __init__(self, on_message=None, port=None):
        self.on_message = on_message or (lambda line: None)
        self.port = port
        self.fd = None
        self.hello = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    # -- connection management -------------------------------------------

    def _probe(self, path, timeout=1.0):
        fd = open_raw(path)
        try:
            os.write(fd, b"?\n")
            buf, end = b"", time.monotonic() + timeout
            while time.monotonic() < end:
                r, _, _ = select.select([fd], [], [], 0.1)
                if r:
                    buf += os.read(fd, 256)
                    for line in buf.decode(errors="replace").splitlines():
                        if line.startswith("H key-monitor"):
                            return fd, line
        except OSError:
            pass
        os.close(fd)
        return None, None

    def connect(self):
        for path in [self.port] if self.port else candidate_ports():
            try:
                fd, hello = self._probe(path)
            except OSError:
                continue
            if fd is not None:
                with self._lock:
                    self.fd, self.hello, self.path = fd, hello, path
                self.on_message(f"connected {path}")
                self.on_message(hello)
                return True
        return False

    def close(self):
        with self._lock:
            if self.fd is not None:
                try:
                    os.close(self.fd)
                except OSError:
                    pass
            was_open, self.fd = self.fd is not None, None
        if was_open:
            self.on_message("disconnected")

    @property
    def connected(self):
        return self.fd is not None

    # -- io -------------------------------------------------------------

    def send(self, line):
        with self._lock:
            if self.fd is None:
                raise ConnectionError("key monitor not connected")
            os.write(self.fd, (line + "\n").encode())

    def press(self, position, pressed=True):
        self.send(f"K {int(position)} {1 if pressed else 0}")

    def request_state(self):
        self.send("?")

    def _read_loop(self):
        buf = b""
        while not self._stop.is_set():
            if self.fd is None:
                if not self.connect():
                    self._stop.wait(1.0)
                    continue
                buf = b""
            try:
                r, _, _ = select.select([self.fd], [], [], 0.2)
                if not r:
                    continue
                data = os.read(self.fd, 1024)
                if not data:
                    raise OSError("EOF")
            except (OSError, TypeError, ValueError):
                self.close()  # keyboard rebooted / unplugged; reconnect
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line and not line.startswith("H key-monitor"):
                    self.on_message(line)
                elif line:
                    self.hello = line
                    self.on_message(line)

    def start(self):
        """Read in a background thread, reconnecting automatically."""
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self.close()
