"""Drive UFC 5 (KytyPS5) through the menus with a virtual Xbox pad via ViGEm.

Requires: pip install vgamepad   (ViGEmBus driver already installed & running)

Usage:
    python drive_ufc.py                 # run the default "into a match" sequence
    python drive_ufc.py --hold 8        # wait 8s before the first press (slow boots)
    python drive_ufc.py --seq "A:3, A:2, A:5, A:3, START:2"
        each step is BUTTON:SECONDS_TO_WAIT_AFTER

Buttons: A B X Y LB RB START BACK UP DOWN LEFT RIGHT
The emulator just needs to have window focus; SDL picks up the virtual pad as XInput.
"""
import argparse
import time

import vgamepad as vg

BTN = {
    "A": vg.XUSB_BUTTON.XUSB_GAMEPAD_A,
    "B": vg.XUSB_BUTTON.XUSB_GAMEPAD_B,
    "X": vg.XUSB_BUTTON.XUSB_GAMEPAD_X,
    "Y": vg.XUSB_BUTTON.XUSB_GAMEPAD_Y,
    "LB": vg.XUSB_BUTTON.XUSB_GAMEPAD_LEFT_SHOULDER,
    "RB": vg.XUSB_BUTTON.XUSB_GAMEPAD_RIGHT_SHOULDER,
    "START": vg.XUSB_BUTTON.XUSB_GAMEPAD_START,
    "BACK": vg.XUSB_BUTTON.XUSB_GAMEPAD_BACK,
    "UP": vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_UP,
    "DOWN": vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_DOWN,
    "LEFT": vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_LEFT,
    "RIGHT": vg.XUSB_BUTTON.XUSB_GAMEPAD_DPAD_RIGHT,
}

# The user's manual path: space, space, space, ENTER -> into the stage we're at.
# space -> A (cross), enter -> START. Which physical button that is depends on the
# controller type; SDL/the game map A=confirm, START=advance regardless.
DEFAULT_SEQ = "A:6, A:2, A:6, START:8"


def tap(pad, button, press=0.12):
    pad.press_button(button=button)
    pad.update()
    time.sleep(press)
    pad.release_button(button=button)
    pad.update()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seq", default=DEFAULT_SEQ)
    ap.add_argument("--hold", type=float, default=3.0,
                    help="seconds to wait before the first press")
    ap.add_argument("--loop", action="store_true",
                    help="repeat the sequence forever (keeps nudging past prompts)")
    args = ap.parse_args()

    steps = []
    for raw in args.seq.split(","):
        raw = raw.strip()
        if not raw:
            continue
        name, _, wait = raw.partition(":")
        name = name.strip().upper()
        if name not in BTN:
            raise SystemExit(f"unknown button {name!r}; valid: {', '.join(BTN)}")
        steps.append((name, float(wait) if wait else 1.0))

    pad = vg.VX360Gamepad()
    time.sleep(1.0)  # let SDL enumerate the device
    print(f"virtual pad up. waiting {args.hold}s, then: "
          + " -> ".join(f"{n}(+{w}s)" for n, w in steps))
    time.sleep(args.hold)

    while True:
        for name, wait in steps:
            print(f"  {name}")
            tap(pad, BTN[name])
            time.sleep(wait)
        if not args.loop:
            break
    print("done")


if __name__ == "__main__":
    main()
