"""Send keyboard taps to the Kyty window. Space is Cross after the UFC 5 remap."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
import time
from pathlib import Path

user32 = ctypes.windll.user32
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
VK = {
    "space": 0x20,
    "up": 0x26,
    "down": 0x28,
    "left": 0x25,
    "right": 0x27,
    "j": 0x4A,
    "enter": 0x0D,
    "esc": 0x1B,
}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = (
        ("wVk", wt.WORD),
        ("wScan", wt.WORD),
        ("dwFlags", wt.DWORD),
        ("time", wt.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    )


class INPUTUNION(ctypes.Union):
    _fields_ = (("ki", KEYBDINPUT),)


class INPUT(ctypes.Structure):
    _fields_ = (("type", wt.DWORD), ("union", INPUTUNION))


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
        title = buf.value.lower()
        if "kyty" in title or "ufc" in title or "ppsa03541" in title:
            found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(enum_cb, 0)
    return found[0] if found else (None, None)


def focus_kyty():
    hwnd, title = find_kyty_window()
    if not hwnd:
        print("kyty window not found")
        return None
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.20)
    print("focused:", title)
    return hwnd


def send_vk(vk, down):
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.union.ki = KEYBDINPUT(vk, 0, 0 if down else KEYEVENTF_KEYUP, 0, None)
    if user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT)) != 1:
        raise OSError("SendInput failed")


def tap(name, hold=0.12):
    vk = VK[name.lower()]
    if not focus_kyty():
        sys.exit(1)
    send_vk(vk, True)
    time.sleep(hold)
    send_vk(vk, False)
    time.sleep(0.15)
    print("tapped", name)


def dump_now():
    path = Path(r"D:\PS5\dumps\DUMP_NOW")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("1", encoding="utf-8")
    print("requested dump", path)


def screenshot(path):
    from PIL import ImageGrab

    hwnd, title = find_kyty_window()
    if not hwnd:
        print("kyty window not found")
        return
    focus_kyty()
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    img = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom), all_screens=True)
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print("shot ->", out, title, "%dx%d" % (rect.right - rect.left, rect.bottom - rect.top))


def main():
    if len(sys.argv) < 2:
        print("usage: ufc5_keys.py tap space|up|down|left|right|j|enter|esc")
        print("       ufc5_keys.py shot PATH")
        print("       ufc5_keys.py dump")
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "tap":
        tap(sys.argv[2] if len(sys.argv) > 2 else "space")
    elif cmd == "shot":
        screenshot(sys.argv[2] if len(sys.argv) > 2 else r"D:\PS5\KytyWindow.png")
    elif cmd == "dump":
        dump_now()
    else:
        print("unknown command", cmd)
        sys.exit(2)


if __name__ == "__main__":
    main()
