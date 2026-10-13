"""Report the desktop geometry the overlay has to position itself inside.

SPI_GETWORKAREA - the call the overlay uses to place itself - returns the
PRIMARY monitor's work area. A full-desktop grab, by contrast, covers the whole
virtual screen, which on a multi-monitor machine is much larger and may start at
a negative origin. Mapping one to the other is what makes a crop of a screenshot
line up with the window coordinates in engine.log.

usage: screen_geometry.py
"""
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32

SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79
SPI_GETWORKAREA = 0x0030


class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]


def main():
    vx = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
    vy = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
    vw = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
    vh = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)

    wa = RECT()
    user32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(wa), 0)

    print(f"virtual screen : {vw}x{vh} at {vx},{vy}")
    print(f"work area      : {wa.right - wa.left}x{wa.bottom - wa.top} "
          f"at {wa.left},{wa.top}   (SPI_GETWORKAREA - what the overlay uses)")

    monitors = []

    MonitorEnumProc = ctypes.WINFUNCTYPE(
        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        ctypes.POINTER(RECT), ctypes.c_double)

    def cb(hmon, hdc, lprect, data):
        r = lprect.contents
        monitors.append((r.left, r.top, r.right - r.left, r.bottom - r.top))
        return 1

    user32.EnumDisplayMonitors(None, None, MonitorEnumProc(cb), 0)
    print(f"monitors       : {len(monitors)}")
    for i, (x, y, w, h) in enumerate(monitors):
        print(f"  [{i}] {w}x{h} at {x},{y}")

    print()
    print("To crop a full-desktop grab at work-area coords (wx, wy):")
    print(f"  virtual_x = wx - {vx}      virtual_y = wy - {vy}")


if __name__ == "__main__":
    main()
