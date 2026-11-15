MUTATORS = ("purity-mangle-snapshot", "purity-mangle-state", "purity-mangle-event",
            "purity-mangle-indirect")
RUNS = 4


def snapshot():
    fields = inspect()
    return " ".join(f"{key}={show(fields[key])}"
                    for key in (":WINDOWS", ":WINDOW-GEOMETRY", ":RULES", ":DATA", ":LAYOUT",
                                ":FOCUS", ":STACKING", ":WORKAREAS"))


def extensions():
    return {text(plist(entry)[":NAME"]): plist(entry) for entry in items(inspect()[":EXTENSIONS"])}


def published(name):
    for pair in items(inspect()[":DATA"]):
        if text(pair[0]) == name:
            return pair[1:]
    raise AssertionError(f"{name} was never published")


def window_ids():
    return [text(plist(entry)[":ID"]) for entry in items(inspect()[":WINDOWS"])]


def stop(unit):
    machine.execute(f"systemctl stop {unit} 2>/dev/null")
    machine.execute(f"systemctl reset-failed {unit} 2>/dev/null")


machine.wait_for_unit("multi-user.target")
start("--bare")
cli(f"mount {PURITY_POLICY}")

for name in ("one", "two", "three"):
    client(f"purity-{name}", f"--app-id {name}", f"--title {name}", "--size 320x200")

fields = extensions()
assert set(fields) == {"purity-snapshot", "purity-carrier", "purity-event", "purity-indirect",
                       "purity-honest"}, (
    sorted(fields))
assert text(fields["purity-honest"][":SHARED"]) == "T", (
    "the walker no longer proves the non-mutating reducer, so it is served copies: "
    + show(fields["purity-honest"]))
for name in ("purity-snapshot", "purity-carrier", "purity-event", "purity-indirect"):
    assert text(fields[name][":SHARED"]) == "NIL", (
        name + " mutates its arguments and must not be served the committed data: "
        + show(fields[name]))

ids = window_ids()
assert len(ids) == 3, ids
assert [text(value) for value in items(published(":PURITY-ORDER"))] == ids, (
    "the proven reducer did not read the committed window order: "
    + show(published(":PURITY-ORDER")))

before = snapshot()
for _ in range(RUNS):
    for command in MUTATORS:
        cli(f"command {command}")
after = snapshot()
assert before == after, "a mutating reducer changed the committed context:\nbefore " + before \
    + "\nafter  " + after

assert [text(value) for value in items(published(":PURITY-OWNER"))] == ["purity-indirect"], (
    "a reducer that mutated its event corrupted the sibling that runs after it: "
    + show(published(":PURITY-OWNER")))
assert [text(value) for value in items(published(":PURITY-COMMAND"))] == ["purity-mangle-indirect"], (
    show(published(":PURITY-COMMAND")))
assert [text(value) for value in items(published(":PURITY-ORDER"))] == ids, (
    "a reducer that mutated its snapshot changed the window order later reducers read: "
    + show(published(":PURITY-ORDER")))
assert [text(value) for value in items(published(":PURITY-STATE-ORDER"))] == ["99", "2", "3"], (
    "the reducer state is not the state the reducer returned: "
    + show(published(":PURITY-STATE-ORDER")))
cli(f"command patch file={PURITY_PATCH}")
patched = extensions()
for name in ("purity-honest", "purity-snapshot", "purity-indirect"):
    assert text(patched[name][":SHARED"]) == "NIL", (
        "a livepatch left a proof made against the loaded definition standing: "
        + show(patched[name]))
before = snapshot()
for _ in range(2):
    for command in MUTATORS:
        cli(f"command {command}")
assert before == snapshot(), "a mutating reducer changed the context after a livepatch"
assert [text(value) for value in items(published(":PURITY-ORDER"))] == window_ids(), (
    show(published(":PURITY-ORDER")))
assert all(text(plist(entry)[":FAILURES"]) == "0" for entry in items(inspect()[":EXTENSIONS"])), (
    show(inspect()[":EXTENSIONS"]))

for name in ("one", "two", "three"):
    stop(f"purity-{name}")
quit()
