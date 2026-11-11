"""
Drive KytyPS5 / UFC 5 with a ViGEmBus virtual DualShock 4.

Kyty reads SDL game-controller events (A/Cross, B/Circle, …). A virtual DS4 is
enough for “PRESS ANY BUTTON” and menus. Vulkan swapchain pixels are captured
with CopyFromScreen after focusing the window (PrintWindow stays black).

  py -3.13 D:\\PS5\\tools\\ufc5_pad.py probe
  py -3.13 D:\\PS5\\tools\\ufc5_pad.py tap cross
  py -3.13 D:\\PS5\\tools\\ufc5_pad.py shot D:\\PS5\\KytyWindow.png
  py -3.13 D:\\PS5\\tools\\ufc5_pad.py run D:\\PS5\\tools\\ufc5_title.route --shots D:\\PS5\\dumps
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
import time
from pathlib import Path

try:
    import vgamepad as vg
except Exception as exc:  # pragma: no cover
    print("FATAL: vgamepad unavailable:", exc)
    sys.exit(1)

BUTTONS = {
    "cross": vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
    "circle": vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE,
    "square": vg.DS4_BUTTONS.DS4_BUTTON_SQUARE,
    "triangle": vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE,
    "l1": vg.DS4_BUTTONS.DS4_BUTTON_SHOULDER_LEFT,
    "r1": vg.DS4_BUTTONS.DS4_BUTTON_SHOULDER_RIGHT,
    "options": vg.DS4_BUTTONS.DS4_BUTTON_OPTIONS,
    "share": vg.DS4_BUTTONS.DS4_BUTTON_SHARE,
    "l3": vg.DS4_BUTTONS.DS4_BUTTON_THUMB_LEFT,
    "r3": vg.DS4_BUTTONS.DS4_BUTTON_THUMB_RIGHT,
}

DPAD = {
    "up": vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,
    "down": vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH,
    "left": vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,
    "right": vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
    "none": vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NONE,
}

user32 = ctypes.windll.user32


def find_kyty_window():
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def enum_cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        title = buf.value
        if "kyty" in title.lower() or "ufc" in title.lower() or "ppsa03541" in title.lower():
            found.append((hwnd, title))
        return True

    user32.EnumWindows(enum_cb, 0)
    if not found:
        return None, None, None
    hwnd, title = found[0]
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return hwnd, title, (rect.left, rect.top, rect.right, rect.bottom)


def focus_kyty():
    hwnd, title, box = find_kyty_window()
    if not hwnd:
        print("kyty window not found")
        return None, None, None
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.25)
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    box = (rect.left, rect.top, rect.right, rect.bottom)
    return hwnd, title, box


def screenshot(path):
    from PIL import ImageGrab

    hwnd, title, box = focus_kyty()
    if not hwnd:
        return None
    left, top, right, bottom = box
    img = ImageGrab.grab(bbox=(left, top, right, bottom), all_screens=True)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    print("  shot -> %s  [%s %dx%d]" % (path, title, right - left, bottom - top))
    return path


class Pad:
    def __init__(self):
        self.gp = vg.VDS4Gamepad()
        self.gp.reset()
        self.gp.update()
        time.sleep(1.5)

    def tap(self, name, hold=0.20):
        focus_kyty()
        b = BUTTONS[name]
        self.gp.press_button(button=b)
        self.gp.update()
        time.sleep(hold)
        self.gp.release_button(button=b)
        self.gp.update()
        time.sleep(0.20)

    def hold(self, name, secs):
        focus_kyty()
        b = BUTTONS[name]
        self.gp.press_button(button=b)
        self.gp.update()
        time.sleep(secs)
        self.gp.release_button(button=b)
        self.gp.update()
        time.sleep(0.20)

    def dpad(self, direction, hold=0.18):
        focus_kyty()
        self.gp.directional_pad(direction=DPAD[direction])
        self.gp.update()
        time.sleep(hold)
        self.gp.directional_pad(direction=DPAD["none"])
        self.gp.update()
        time.sleep(0.20)

    def stick(self, which, direction, secs=1.5):
        focus_kyty()
        x, y = 0.0, 0.0
        if direction == "down":
            y = 1.0
        elif direction == "up":
            y = -1.0
        elif direction == "left":
            x = -1.0
        elif direction == "right":
            x = 1.0
        else:
            raise ValueError("stick direction: %s" % direction)
        if which in ("r", "right"):
            self.gp.right_joystick_float(x, y)
        else:
            self.gp.left_joystick_float(x, y)
        self.gp.update()
        time.sleep(secs)
        if which in ("r", "right"):
            self.gp.right_joystick_float(0.0, 0.0)
        else:
            self.gp.left_joystick_float(0.0, 0.0)
        self.gp.update()
        time.sleep(0.20)


def run_route(route_path, shots_dir=None):
    pad = Pad()
    route_start = time.monotonic()
    steps = [
        ln.split("#", 1)[0].strip()
        for ln in Path(route_path).read_text(encoding="utf-8").splitlines()
    ]
    steps = [s for s in steps if s]
    idx = 0
    for i, step in enumerate(steps, 1):
        parts = step.split()
        op = parts[0].lower()
        print("[%02d/%02d +%6.1fs] %s" % (i, len(steps), time.monotonic() - route_start, step),
              flush=True)
        if op == "wait":
            time.sleep(float(parts[1]))
        elif op == "tap":
            pad.tap(parts[1])
        elif op == "hold":
            pad.hold(parts[1], float(parts[2]))
        elif op == "dpad":
            pad.dpad(parts[1])
        elif op == "stick":
            which, direction = parts[1], parts[2]
            secs = float(parts[3]) if len(parts) > 3 else 1.5
            pad.stick(which, direction, secs)
        elif op == "shot":
            if shots_dir:
                idx += 1
                screenshot(str(Path(shots_dir) / ("step%02d.png" % idx)))
        else:
            print("  ! unknown step, ignored")
    print("route complete")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "probe":
        hwnd, title, box = find_kyty_window()
        print("kyty window:", ("%s %s" % (title, box)) if hwnd else "NOT FOUND")
        Pad()
        print("virtual DS4 live; holding 8s for SDL to enumerate")
        time.sleep(8)
        hwnd, title, box = find_kyty_window()
        print("kyty window:", ("%s %s" % (title, box)) if hwnd else "NOT FOUND")
    elif cmd == "tap":
        pad = Pad()
        name = sys.argv[2] if len(sys.argv) > 2 else "cross"
        print("tap", name)
        pad.tap(name)
        time.sleep(0.5)
    elif cmd == "dpad":
        pad = Pad()
        pad.dpad(sys.argv[2] if len(sys.argv) > 2 else "down")
    elif cmd == "stick":
        pad = Pad()
        which = sys.argv[2] if len(sys.argv) > 2 else "r"
        direction = sys.argv[3] if len(sys.argv) > 3 else "down"
        secs = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
        print("stick", which, direction, secs)
        pad.stick(which, direction, secs)
    elif cmd == "shot":
        screenshot(sys.argv[2] if len(sys.argv) > 2 else r"D:\PS5\KytyWindow.png")
    elif cmd == "run":
        shots = None
        if "--shots" in sys.argv:
            shots = sys.argv[sys.argv.index("--shots") + 1]
        run_route(sys.argv[2], shots)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
