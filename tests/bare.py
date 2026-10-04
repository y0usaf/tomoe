start("--bare")
state = inspect()
assert state[":EXTENSIONS"] == "NIL", show(state[":EXTENSIONS"])
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
assert items(state[":OUTPUTS"]), "no output"
cli(f"mount {VIRTUAL_PROBE}")
for _ in range(100):
    state = inspect()
    outputs = sorted(text(plist(output)[":NAME"]) for output in items(state[":OUTPUTS"]))
    if "VIRTUAL-2" in outputs:
        break
    time.sleep(0.1)
assert outputs == ["HEADLESS-1", "VIRTUAL-1", "VIRTUAL-2"], outputs
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
surfaces = sorted(text(plist(surface)[":OUTPUT"]) for surface in items(state[":SURFACES"]))
assert surfaces == outputs, surfaces
quit()
