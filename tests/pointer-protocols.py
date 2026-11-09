import base64
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

names = "default context-menu help pointer progress wait cell crosshair text vertical-text alias copy move no-drop not-allowed grab grabbing e-resize n-resize ne-resize nw-resize s-resize se-resize sw-resize w-resize ew-resize ns-resize nesw-resize nwse-resize col-resize row-resize all-scroll zoom-in zoom-out dnd-ask all-resize".split()
with tempfile.TemporaryDirectory(prefix="tomoe-pointer-", dir=os.environ.get("TMPDIR")) as directory:
    root = pathlib.Path(directory)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    theme = root / "icons" / "check" / "cursors"
    theme.mkdir(parents=True)
    for i, name in enumerate(names, 1):
        data = struct.pack("<4I3I9I4I", 0x72756358, 16, 0x10000, 1,
                           0xfffd0002, 24, 28, 36, 0xfffd0002, 24, 1, 2, 2, 0, 0, 0,
                           *([0xff000000 | i] * 4))
        (theme / name).write_bytes(data)
    env = {key: os.environ[key] for key in ("PATH", "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_DRIVERS_PATH", "__EGL_VENDOR_LIBRARY_FILENAMES", "LD_LIBRARY_PATH") if key in os.environ}
    env.update(HOME=str(root), XDG_RUNTIME_DIR=str(runtime), XCURSOR_PATH=str(root / "icons"),
               XCURSOR_THEME="check", LIBGL_ALWAYS_SOFTWARE="1")
    log = open(root / "compositor.log", "w+")
    compositor = subprocess.Popen([sys.argv[1], "--backend", "headless", "--bare", "--socket", "pointer-check", "--no-watch"], env=env, stdout=log, stderr=log)
    client = None
    def cli(*args):
        return subprocess.check_output([sys.argv[1], "--socket", "pointer-check", *args], env=env, text=True)
    try:
        for _ in range(600):
            if compositor.poll() is not None:
                log.seek(0)
                raise AssertionError(log.read())
            if (runtime / "pointer-check").exists():
                break
            time.sleep(0.1)
        else:
            raise AssertionError("private compositor did not start")
        env["WAYLAND_DISPLAY"] = "pointer-check"
        info = subprocess.check_output(["wayland-info"], env=env, text=True)
        assert "wp_cursor_shape_manager_v1" in info
        client = subprocess.Popen([sys.argv[2]], env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        while (line := client.stdout.readline().strip()) != "ready":
            assert line, "pointer client exited before enter"
        def command(value):
            client.stdin.write(value + "\n")
            client.stdin.flush()
            assert client.stdout.readline().strip() == "ok"
        def image():
            state = json.loads(cli("msg", "cursor"))
            pixels = base64.b64decode("".join(state.get("image", [])))
            return state, struct.unpack("<I", pixels[:4])[0] if pixels else None
        for i in range(1, 37):
            command(f"shape {i}")
            state, pixel = image()
            assert pixel == 0xff000000 | i, (i, state, pixel)
            assert not state["hidden"] and state["width"] == 2
        command("stale 9")
        assert image()[1] == 0xff000024
        command("destroy")
        assert image()[1] == 0xff000024
        command("hide")
        assert image()[0]["hidden"]
        command("shape 9")
        assert image()[1] == 0xff000009 and not image()[0]["hidden"]
        command("manager")
        command("shape 4")
        assert image()[1] == 0xff000004
        command("unmap")
        before = image()
        command("shape 9")
        assert image() == before
        client.stdin.write("shape 0\n")
        client.stdin.flush()
        assert client.wait(timeout=10) == 1
        assert compositor.poll() is None
        print("cursor: 36 themed shapes, serial rejection, surface/shape replacement, destruction, focus rejection, invalid-shape error passed")
    finally:
        if client and client.poll() is None:
            client.terminate()
            client.wait(timeout=10)
        if compositor.poll() is None:
            try:
                cli("quit")
                compositor.wait(timeout=15)
            except Exception:
                compositor.terminate()
                compositor.wait(timeout=15)
        log.close()
