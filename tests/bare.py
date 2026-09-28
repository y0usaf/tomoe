start("--bare")
state = inspect()
assert state[":EXTENSIONS"] == "NIL", show(state[":EXTENSIONS"])
assert state[":LAST-ERROR"] == "NIL", state[":LAST-ERROR"]
assert items(state[":OUTPUTS"]), "no output"
quit()
