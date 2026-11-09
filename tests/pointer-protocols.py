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
    env["TOMOE_GESTURE_TEST"] = str(root / "gesture-command")
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
        def command(value, target=None):
            target = target or client
            target.stdin.write(value + "\n")
            target.stdin.flush()
            events = []
            while (line := target.stdout.readline().strip()) != "ok":
                assert line, f"client exited during {value}"
                events.append(line)
            return events
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
        if len(sys.argv) > 3:
            assert "zwp_pointer_gestures_v1" in info
            cli("mount", sys.argv[3])
            def inject(request):
                path = root / "gesture-command"
                temporary = root / "gesture-command.tmp"
                temporary.write_text(request)
                os.replace(temporary, path)
                cli("msg", "cursor")
                cli("msg", "cursor")
                assert not path.exists(), "gesture injection was not consumed"
            for kind, name, fingers in ((1, "swipe", 3), (2, "pinch", 2), (3, "hold", 1)):
                inject(f"(:begin {kind} 10 {fingers})")
                assert command("sync") == [f"{name} begin 10 {fingers}"]
                if kind != 3:
                    inject(f"(:update {kind} 11 2.5d0 -1.25d0 1.5d0 15d0)")
                    expected = f"{name} update 11 2.500 -1.250" + (" 1.500 15.000" if kind == 2 else "")
                    assert command("sync") == [expected]
                inject(f"(:end {kind} 12 0)")
                assert command("sync") == [f"{name} end 0"]
            inject("(:update 1 20 1d0 2d0 1d0 0d0)")
            assert command("sync") == []
            inject("(:begin 1 21 3)")
            assert command("sync") == ["swipe begin 21 3"]
            inject("(:other-update 1 22 1d0 2d0 1d0 0d0)")
            assert command("sync") == []
            inject("(:begin 2 23 2)")
            assert command("sync") == ["swipe end 1", "pinch begin 23 2"]
            inject("(:end 2 24 1)")
            assert command("sync") == ["pinch end 1"]
            inject("(:begin 1 30 3)")
            assert command("sync") == ["swipe begin 30 3"]
            command("resubscribe")
            inject("(:update 1 31 1d0 2d0 1d0 0d0)")
            inject("(:end 1 32 0)")
            assert command("sync") == []
            inject("(:begin 3 40 2)")
            assert command("sync") == ["hold begin 40 2"]
            inject("(:remove)")
            assert command("sync") == ["hold end 1"]
            command("release-gestures")
            inject("(:begin 1 50 3)")
            assert command("sync") == ["swipe begin 50 3"]
            second = subprocess.Popen([sys.argv[2]], env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                while (line := second.stdout.readline().strip()) != "ready":
                    assert line, "second client failed to enter"
                assert command("sync") == ["swipe end 1"]
                inject("(:update 1 51 1d0 2d0 1d0 0d0)")
                assert command("sync", second) == []
                inject("(:begin 2 52 2)")
                assert command("sync", second) == ["pinch begin 52 2"]
                assert command("sync") == []
                assert command("unmap", second) == ["pinch end 1"]
                inject("(:update 2 53 1d0 2d0 1d0 0d0)")
                assert command("sync", second) == []
            finally:
                second.stdin.write("quit\n")
                second.stdin.flush()
                second.wait(timeout=10)
            command("remap")
            inject("(:begin 3 60 1)")
            assert command("sync") == ["hold begin 60 1"]
            assert command("unmap") == ["hold end 1"]
            inject("(:stop)")
            print("gestures: swipe/pinch/hold phases and values, cancelled overlap/device removal, late subscriptions, manager lifetime, focus isolation and loss passed")
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
        if sys.exc_info()[0]:
            log.flush()
            log.seek(0)
            print(log.read(), file=sys.stderr)
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
