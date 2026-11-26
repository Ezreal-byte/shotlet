"""Exercise the real X11 app, annotation input, native save dialog and cancellation."""
import argparse
import ctypes as c
import os
from pathlib import Path
import subprocess
import tempfile
import time

from PIL import Image, ImageGrab


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=15).stdout.strip()


def wait_for(predicate, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError("Timed out waiting for the desktop application")


def find_window(pattern):
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", pattern], capture_output=True, text=True)
    return result.stdout.splitlines()[0] if result.returncode == 0 else None


def move(x, y):
    # --sync waits forever for a no-op mouse move; allow ImGui one frame instead.
    run("xdotool", "mousemove", str(x), str(y))
    time.sleep(.2)


def click(x, y):
    move(x, y)
    run("xdotool", "mousedown", "1")
    time.sleep(.1)
    run("xdotool", "mouseup", "1")
    time.sleep(.2)


def drag(x, y, end_x, end_y):
    move(x, y)
    run("xdotool", "mousedown", "1")
    time.sleep(.1)
    move(end_x, end_y)
    run("xdotool", "mouseup", "1")
    time.sleep(.3)


def wait_selected_tool(x):
    def selected():
        image = ImageGrab.grab().convert("RGB")
        return any(pixel[1] > 140 and pixel[0] < 80 and pixel[2] < 100
                   for pixel in image.crop((x - 12, 630, x + 13, 655)).getdata())
    wait_for(selected)


class Background:
    """A real X11 window with known colors, kept alive until the captures finish."""
    def __init__(self):
        self.x = c.CDLL("libX11.so.6")
        self.x.XOpenDisplay.restype = c.c_void_p
        self.x.XDefaultRootWindow.argtypes = [c.c_void_p]
        self.x.XDefaultRootWindow.restype = c.c_ulong
        self.x.XCreateSimpleWindow.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_int,
                                              c.c_uint, c.c_uint, c.c_uint, c.c_ulong, c.c_ulong]
        self.x.XCreateSimpleWindow.restype = c.c_ulong
        self.x.XCreateGC.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_void_p]
        self.x.XCreateGC.restype = c.c_void_p
        self.x.XSetForeground.argtypes = [c.c_void_p, c.c_void_p, c.c_ulong]
        self.x.XFillRectangle.argtypes = [c.c_void_p, c.c_ulong, c.c_void_p, c.c_int, c.c_int, c.c_uint, c.c_uint]
        self.x.XMapRaised.argtypes = [c.c_void_p, c.c_ulong]
        self.x.XSync.argtypes = [c.c_void_p, c.c_int]
        self.x.XCloseDisplay.argtypes = [c.c_void_p]
        self.display = self.x.XOpenDisplay(None)
        assert self.display, "An X11 display is required"
        root = self.x.XDefaultRootWindow(self.display)
        self.window = self.x.XCreateSimpleWindow(self.display, root, 0, 0, 1280, 800, 0, 0, 0x285070)
        # No WM is necessary; avoiding decorations also fixes the reference pixel coordinates.
        self.x.XMapRaised(self.display, self.window)
        gc = self.x.XCreateGC(self.display, self.window, 0, None)
        self.x.XSetForeground(self.display, gc, 0xf1c75b)
        self.x.XFillRectangle(self.display, self.window, gc, 600, 460, 100, 60)
        self.x.XSync(self.display, 0)

    def close(self):
        self.x.XCloseDisplay(self.display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--expect-software", action="store_true")
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    os.environ["LC_ALL"] = "C.UTF-8"
    background = Background()
    app = None
    with tempfile.TemporaryDirectory(prefix="shotlet-gui-") as temporary:
        env = dict(os.environ, SDL_FILE_DIALOG_DRIVER="zenity",
                   XDG_SESSION_TYPE="x11", WAYLAND_DISPLAY="",
                   XDG_CONFIG_HOME=f"{temporary}/config", XDG_DATA_HOME=f"{temporary}/data",
                   XDG_STATE_HOME=f"{temporary}/state", XDG_CACHE_HOME=f"{temporary}/cache")
        log = (args.artifacts / "app.log").open("w", encoding="utf-8")
        try:
            ImageGrab.grab().save(args.artifacts / "desktop.png")

            def start():
                nonlocal app
                process = subprocess.Popen([str(args.app.resolve()), "--theme", "dark", "--language", "zh-CN",
                                            "--log-level", "debug",
                                            "--log-dir", f"{temporary}/logs"], env=env, stdout=log, stderr=log)
                # Keep ownership even if waiting for the first window fails.
                app = process
                window = wait_for(lambda: find_window("^Shotlet_"))
                run("xdotool", "windowfocus", window)
                time.sleep(.5)
                return process

            app = start()
            drag(200, 150, 1000, 600)
            wait_for(lambda: min(ImageGrab.grab().convert("RGB").getpixel((319, 633))) > 80)
            ImageGrab.grab().save(args.artifacts / "selection.png")
            # Capture toolbar: first rectangle icon; text is the eighth icon.
            click(274, 642)
            wait_selected_tool(274)
            drag(300, 230, 550, 430)
            click(589, 642)
            wait_selected_tool(589)
            click(680, 280)
            run("xdotool", "type", "--clearmodifiers", "--delay", "80", "麒麟 UOS 标注")
            time.sleep(.4)
            ImageGrab.grab().save(args.artifacts / "annotations.png")
            if args.expect_software:
                preview = Image.open(args.artifacts / "annotations.png").convert("RGB")
                # The unselected ellipse tool must remain a continuous ring at its actual 25px UI size.
                outline = {(x, y) for y in range(630, 655) for x in range(307, 332)
                           if min(preview.getpixel((x, y))) > 80}
                assert len(outline) > 50, "Ellipse tool icon is missing"
                connected = {outline.pop()}
                pending = list(connected)
                while pending:
                    x, y = pending.pop()
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            point = (x + dx, y + dy)
                            if point in outline:
                                outline.remove(point)
                                connected.add(point)
                                pending.append(point)
                assert not outline, "Ellipse tool icon has disconnected strokes"
            # Completing a standalone Linux capture also opens the save chooser.
            click(924, 642)
            dialog = wait_for(lambda: find_window("保存截图"))
            run("xdotool", "windowfocus", dialog)
            ImageGrab.grab().save(args.artifacts / "save-dialog.png")
            # A rerun must not encounter an overwrite prompt or accept a previous PNG.
            output = args.artifacts.resolve() / f"annotated-{time.time_ns()}.png"
            run("xdotool", "key", "ctrl+l")
            run("xdotool", "type", "--clearmodifiers", "--delay", "5", str(output))
            run("xdotool", "key", "Return")
            time.sleep(.5)
            if not output.exists():
                run("xdotool", "key", "Return")
            wait_for(output.exists)
            assert app.wait(timeout=15) == 0, "Saving must return success"
            image = Image.open(output).convert("RGB")
            assert image.size == (800, 450), image.size
            assert image.getpixel((10, 10)) == (0x28, 0x50, 0x70), "Captured desktop background is incorrect"
            assert image.getpixel((420, 330)) == (0xf1, 0xc7, 0x5b), "Captured source window is missing"
            red = lambda pixel: pixel[0] > 180 and pixel[1] < 120 and pixel[2] < 120
            assert red(image.getpixel((100, 140))), "Rectangle annotation was not exported"
            text_pixels = sum(red(image.getpixel((x, y))) for y in range(120, 200) for x in range(470, 700))
            assert text_pixels > 40, "Text annotation was not exported"
            cjk_pixels = sum(red(image.getpixel((x, y))) for y in range(120, 170) for x in range(478, 508))
            assert cjk_pixels > 30, "Chinese glyphs before UOS were not exported"
            if args.expect_software:
                rendered = (args.artifacts / "app.log").read_text(encoding="utf-8")
                assert "event=overlay.renderer backend=software" in rendered
                assert "event=capture.output_rendered path=software" in rendered
            (args.artifacts / "annotated.png").write_bytes(output.read_bytes())

            # Cancel the real native chooser, then cancel directly with Escape.
            saved = output.read_bytes()
            app = start()
            drag(200, 150, 1000, 600)
            run("xdotool", "key", "ctrl+s")
            dialog = wait_for(lambda: find_window("保存截图"))
            run("xdotool", "windowfocus", dialog)
            run("xdotool", "key", "Escape")
            assert app.wait(timeout=15) == 2, "Chooser cancellation must stay cancelled"
            assert output.read_bytes() == saved
            app = start()
            run("xdotool", "key", "Escape")
            assert app.wait(timeout=15) == 2
            print("PASS: real X11 capture, annotations, Chinese text, PNG save and cancellation")
        finally:
            if app and app.poll() is None:
                ImageGrab.grab().save(args.artifacts / "failure.png")
                app.terminate()
                try:
                    app.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()
            log.close()
            background.close()


if __name__ == "__main__":
    main()
