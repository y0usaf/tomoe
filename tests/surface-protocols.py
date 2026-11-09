start("--bare")
client("pixel", "--mode layer --anchor top,left --size 120x80 --color e04020 --single-pixel")
for _ in range(100):
    data = machine.succeed(ENV + "WAYLAND_DISPLAY=check grim -g '40,40 1x1' -t ppm - | od -An -v -tu1").split()
    if [int(v) for v in data[-3:]] == [224, 64, 32]:
        break
    time.sleep(0.05)
else:
    raise AssertionError(data)
machine.succeed("systemctl stop pixel")

def pixel(overlays=False):
    return [int(v) for v in machine.succeed(ENV + f"WAYLAND_DISPLAY=check grim {'-c ' if overlays else ''}-g '45,45 1x1' -t ppm - | od -An -v -tu1").split()][-3:]

def expect_pixel(want, overlays=False):
    for _ in range(100):
        got = pixel(overlays)
        if all(abs(a - b) <= 2 for a, b in zip(got, want)):
            return
        time.sleep(0.05)
    raise AssertionError((got, want))

client("alpha", "--mode layer --anchor top,left --size 120x80 --color e04020 --alpha-probe")
expect_pixel([119, 39, 23])
for step, want in enumerate([[119, 39, 23], [13, 13, 13], [13, 13, 13], [224, 64, 32]], 1):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 alpha")
    machine.wait_until_succeeds(f"journalctl -u alpha -o cat | grep -qx 'alpha-step {step}'")
    expect_pixel(want)
machine.succeed("systemctl stop alpha")
for mode, interface in [("alpha-duplicate", "wp_alpha_modifier_v1"), ("alpha-orphan", "wp_alpha_modifier_surface_v1")]:
    error = machine.fail(ENV + f"WAYLAND_DISPLAY=check tomoe-test-client --protocol-error {mode} 2>&1")
    assert f"protocol error on {interface}@" in error and ": 0" in error, error

machine.succeed("systemd-run --unit=cursor-alpha --property=Type=exec --setenv=XDG_RUNTIME_DIR=/run/tomoe --setenv=WAYLAND_DISPLAY=check tomoe-cursor-client")
machine.wait_until_succeeds("journalctl -u cursor-alpha -o cat | grep -qx cursor-ready")
expect_pixel([128, 64, 64], True)
import base64
import json
cursor = json.loads(cli("msg cursor '{}'"))
image = base64.b64decode("".join(cursor["image"]))
assert image[:4] == bytes([16, 32, 112, 127]), image[:4]
for step, want in enumerate([[128, 64, 64], [32, 64, 96], [32, 64, 96], [224, 64, 32]], 1):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 cursor-alpha")
    machine.wait_until_succeeds(f"journalctl -u cursor-alpha -o cat | grep -qx 'alpha-step {step}'")
    expect_pixel(want, True)
machine.succeed("systemctl stop cursor-alpha")

def content_type():
    return plist(items(inspect()[":WINDOWS"])[0])[":CONTENT-TYPE"]

client("content", "--content-probe")
for _ in range(100):
    if content_type() == ":GAME":
        break
    time.sleep(0.05)
else:
    raise AssertionError(inspect())
for step, want in enumerate([":GAME", ":PHOTO", ":PHOTO", ":NONE"], 1):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 content")
    machine.wait_until_succeeds(f"journalctl -u content -o cat | grep -qx 'content-step {step}'")
    assert content_type() == want, inspect()
machine.succeed("systemctl stop content")
for mode in ["content-duplicate", "content-invalid"]:
    error = machine.fail(ENV + f"WAYLAND_DISPLAY=check tomoe-test-client --protocol-error {mode} 2>&1")
    assert "protocol error on wp_content_type" in error, error
machine.succeed(ENV + "WAYLAND_DISPLAY=check tomoe-test-client --protocol-error content-orphan")

client("tag", "--tag-probe")
for _ in range(100):
    windows = items(inspect()[":WINDOWS"])
    if windows and plist(windows[0]).get(":TAG") == '"settings"':
        break
    time.sleep(0.05)
else:
    raise AssertionError(inspect())
assert plist(windows[0])[":DESCRIPTION"] == '"Settings window"', windows
for step in range(1, 3):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 tag")
    machine.wait_until_succeeds(f"journalctl -u tag -o cat | grep -qx 'tag-step {step}'")
    window = plist(items(inspect()[":WINDOWS"])[0])
    assert window[":TAG"] == '"composer"' and window[":DESCRIPTION"] == '"Compose a message"', window
machine.succeed("systemctl stop tag")

client("dialog", "--dialog-probe")
def dialog_window():
    return next(plist(w) for w in items(inspect()[":WINDOWS"]) if plist(w)[":TITLE"] == '"Tomoe test client"')

for _ in range(100):
    window = dialog_window()
    if window[":DIALOG"] == "T" and window[":MODAL"] == "T":
        break
    time.sleep(0.05)
else:
    raise AssertionError(window)
parent = window[":PARENT"]
assert parent != "NIL", window
for step, (dialog, modal, parented) in enumerate([
    ("T", "NIL", True), ("T", "T", True), ("NIL", "NIL", False), ("T", "T", True),
    ("NIL", "NIL", True), ("T", "NIL", True), ("NIL", "NIL", False),
], 1):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 dialog")
    machine.wait_until_succeeds(f"journalctl -u dialog -o cat | grep -qx 'dialog-step {step}'")
    window = dialog_window()
    assert window[":DIALOG"] == dialog and window[":MODAL"] == modal, window
    assert window[":PARENT"] == (parent if parented else "NIL"), window
machine.succeed("systemctl kill --kill-whom=main --signal=USR1 dialog")
machine.wait_until_succeeds("journalctl -u dialog -o cat | grep -qx 'dialog-step 8'")
machine.succeed("systemctl stop dialog")
for mode, interface, code in [("dialog-duplicate", "xdg_wm_dialog_v1", 0), ("parent-self", "xdg_toplevel", 1)]:
    error = machine.fail(ENV + f"WAYLAND_DISPLAY=check tomoe-test-client --protocol-error {mode} 2>&1")
    assert f"protocol error on {interface}@" in error and f": {code}" in error, error

import base64
import io
import json
import math
import shlex
import struct
import wave

config = 'context.properties = {\n    core.daemon = true\n    core.name = pipewire-0\n    default.clock.rate = 48000\n    default.clock.quantum = 256\n}\ncontext.spa-libs = {\n    audio.convert.* = audioconvert/libspa-audioconvert\n    support.* = support/libspa-support\n}\ncontext.modules = [\n    { name = libpipewire-module-protocol-native }\n    { name = libpipewire-module-metadata }\n    { name = libpipewire-module-spa-node-factory }\n    { name = libpipewire-module-client-node }\n    { name = libpipewire-module-adapter }\n    { name = libpipewire-module-link-factory }\n    { name = libpipewire-module-access }\n]\ncontext.objects = [\n    { factory = spa-node-factory args = {\n        factory.name = support.node.driver\n        node.name = Dummy-Driver\n        priority.driver = 20000\n    } }\n    { factory = adapter args = {\n        factory.name = support.null-audio-sink\n        node.name = bell-sink\n        media.class = Audio/Sink\n        audio.position = [ FL FR ]\n        adapter.auto-port-config = { mode = dsp monitor = true position = preserve }\n    } }\n]\n'
machine.succeed("printf %s " + shlex.quote(config) + " > /run/tomoe/pipewire.conf")
samples = b"".join(struct.pack("<hh", v, v) for i in range(9600)
                   for v in [int(12000 * math.sin(i * 2 * math.pi * 440 / 48000))])
wav = io.BytesIO()
with wave.open(wav, "wb") as output:
    output.setnchannels(2)
    output.setsampwidth(2)
    output.setframerate(48000)
    output.writeframes(samples)
machine.succeed("printf %s " + shlex.quote(base64.b64encode(wav.getvalue()).decode()) + " | base64 -d > /run/bell.wav")
probe = '(in-package #:tomoe-user) (define-extension "bell-test" (:reads () :state nil) (snapshot state event) (declare (ignore snapshot state event)) (values nil (list (sound :bell "/run/bell.wav")) nil))'
machine.succeed("printf %s " + shlex.quote(probe) + " > /run/bell.lisp")
machine.succeed("systemd-run --unit=check-pipewire --property=Type=exec --setenv=XDG_RUNTIME_DIR=/run/tomoe pipewire -c /run/tomoe/pipewire.conf")
machine.wait_for_file("/run/tomoe/pipewire-0")
cli("mount /run/bell.lisp")
machine.succeed("systemd-run --unit=bell-record --property=Type=exec --setenv=XDG_RUNTIME_DIR=/run/tomoe pw-cat --record --target=0 --raw --format=f32 --channels=2 --rate=48000 -P '{ node.name = bell-record adapter.auto-port-config = { mode = dsp position = preserve } }' /run/bell.raw")
machine.wait_until_succeeds(ENV + "pw-cli ls Node | grep tomoe")
machine.wait_until_succeeds(ENV + "pw-link -i | grep bell-record:input_FR")
graph = json.loads(machine.succeed(ENV + "pw-dump"))
node = next(obj["id"] for obj in graph if obj["type"] == "PipeWire:Interface:Node" and obj["info"]["props"].get("node.name") == "tomoe")
machine.succeed(ENV + f"pw-cli set-param {node} PortConfig '{{ direction = Output mode = dsp format = {{ mediaType = audio mediaSubtype = raw format = F32P rate = 48000 channels = 2 position = [ FL FR ] }} }}'")
for channel in ["FL", "FR"]:
    machine.succeed(ENV + f"pw-link tomoe:output_{channel} bell-sink:playback_{channel}")
    machine.succeed(ENV + f"pw-link bell-sink:monitor_{channel} bell-record:input_{channel}")
for _ in range(100):
    if plist(inspect()[":NATIVE-SOUND"])[":STATE"] == ":STREAMING":
        break
    time.sleep(0.05)
else:
    raise AssertionError(inspect())

def captured_audio():
    raw = base64.b64decode(machine.succeed("base64 -w0 /run/bell.raw"))
    return struct.unpack(f"<{len(raw) // 8 * 2}f", raw[:len(raw) // 8 * 8])

def wait_capture(predicate):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        values = captured_audio()
        if predicate(values):
            return values
        time.sleep(0.05)
    audible = sum(abs(v) > 0.001 for v in values)
    raise AssertionError(("capture progress timed out", len(values) // 2, audible))

def complete_tone(values, offset):
    tone = values[offset:]
    audible = [i for i, v in enumerate(tone) if abs(v) > 0.001]
    assert len(audible) <= tone_samples, (len(audible), tone_samples)
    return (len(audible) == tone_samples
            and len(tone) - audible[-1] > 4800 * 2)

tone_samples = sum(abs(v[0] / 32768) > 0.001 for v in struct.iter_unpack("<h", samples))
values = wait_capture(lambda values: len(values) >= 4800 * 2)
assert not any(abs(v) > 0.001 for v in values), "audio before the first bell"
client("bell", "--bell-probe")
offset = len(values)
for step in range(1, 4):
    machine.succeed("systemctl kill --kill-whom=main --signal=USR1 bell")
    machine.wait_until_succeeds(f"journalctl -u bell -o cat | grep -qx 'bell-step {step}'")
    sound = plist(inspect()[":NATIVE-SOUND"])
    assert sound[":PLAYED"] == str(min(step, 2)) and sound[":DROPPED"] == "0", sound
    if step < 3:
        values = wait_capture(lambda values: complete_tone(values, offset))
        assert 0.35 < max(abs(v) for v in values[offset:]) < 0.38
    else:
        values = wait_capture(lambda values: len(values) >= offset + 14400 * 2)
        assert not any(abs(v) > 0.001 for v in values[offset:]), "bell after manager destroy"
    offset = len(values)
machine.succeed("systemctl stop bell-record")
raw = base64.b64decode(machine.succeed("base64 -w0 /run/bell.raw"))
values = struct.unpack(f"<{len(raw) // 4}f", raw)
audible = sum(abs(v) > 0.001 for v in values)
peak = max(abs(v) for v in values)
assert 0 < audible <= 2 * tone_samples and 0.35 < peak < 0.38, (audible, peak)
print(f"bell capture: {tone_samples} audible samples per cue, {audible} total, peak {peak}; no tone after destroy")
cli("unmount bell-test")
assert plist(inspect()[":NATIVE-SOUND"])[":STATE"] == ":OFF", inspect()
machine.succeed("systemctl stop bell check-pipewire")

quit()
