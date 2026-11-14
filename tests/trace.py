
import hashlib

CONTEXT_KEYS = (
    ":LAYOUT", ":STACKING", ":FOCUS", ":LAYERS", ":SETTINGS", ":WORKAREAS", ":SURFACES",
    ":DATA", ":RULES", ":WINDOW-GEOMETRY", ":VIRTUAL-OUTPUTS", ":CONFIG-ERROR",
    ":KEYBOARD-GRAB", ":COMMANDS", ":BINDINGS", ":PENDING-CONTEXT", ":VIEW", ":OUTPUTS",
    ":CONNECTORS", ":OUTPUT-ERRORS", ":SOUNDS", ":TIMERS", ":WATCHES", ":EXECUTIONS",
    ":RULE-INSTANCES", ":PATCHES", ":LAST-ERROR",
)
WINDOW_FIELDS = (":ID", ":TITLE", ":APP-ID", ":WIDTH", ":HEIGHT", ":BUFFERED", ":FULLSCREEN",
                 ":MAXIMIZE", ":FULLSCREEN-REQUESTED", ":MAXIMIZE-REQUESTED")
EXTENSION_FIELDS = (":NAME", ":READS", ":ADMISSION", ":STATE", ":DISPATCHES", ":FAILURES",
                    ":LAST-ERROR", ":EFFECTS")
RULE_FIELDS = (":OWNER", ":NAME", ":WINDOW", ":IDENTITY", ":STATE", ":DISPATCHES", ":FAILURES")
TIMER_FIELDS = (":OWNER", ":NAME", ":KIND", ":MILLISECONDS", ":STATUS")


def project(value, keys):
    record = plist(value)
    return "(" + " ".join(key + " " + show(record.get(key, "NIL")) for key in keys) + ")"


def project_list(value, keys):
    return "(" + " ".join(project(entry, keys) for entry in items(value)) + ")"


def canonical():
    state = inspect()
    lines = []
    for key in CONTEXT_KEYS:
        if key not in state:
            continue
        value = state[key]
        if key == ":WINDOWS":
            text = project_list(value, WINDOW_FIELDS)
        elif key == ":RULE-INSTANCES":
            text = project_list(value, RULE_FIELDS)
        elif key == ":TIMERS":
            text = project_list(value, TIMER_FIELDS)
        else:
            text = show(value)
        lines.append(key + " " + text)
    for extension in items(state.get(":EXTENSIONS", [])):
        fields = plist(extension)
        if "trace-" not in fields.get(":SOURCE", ""):
            continue
        lines.append("extension " + project(extension, EXTENSION_FIELDS))
    return "\n".join(lines)


def lines(path):
    return [line for line in machine.succeed(f"cat {path} 2>/dev/null || true").splitlines()
            if line]


seen = 0
records = []


def step(label):
    global seen
    time.sleep(0.2)
    trace = lines("/run/trace.log")
    record = ("== " + label
              + "\n-- dispatch\n" + "\n".join(trace[seen:])
              + "\n-- commands\n" + "\n".join(
                  line for line in trace[seen:] if " emits=" in line)
              + "\n-- committed\n" + canonical())
    record = "\n".join(part.rstrip()
                        for line in record.splitlines()
                        for part in (line[index:index + 600]
                                     for index in range(0, max(1, len(line)), 600)))
    records.append(record)
    seen = len(trace)


def inject(event):
    code, output = machine.execute(ENV + "tomoe --socket check event '" + event + "'")
    print("-- reply " + str(code) + " " + output.strip(), flush=True)
    records.append(("-- reply " + str(code) + " " + output.strip()).rstrip())
    return code


def wait_windows(count):
    for _ in range(400):
        if len(items(inspect().get(":WINDOWS", []))) >= count:
            return
        time.sleep(0.05)
    raise AssertionError(f"expected {count} windows")




start("--bare", bus=False)
machine.succeed("rm -f /run/trace.log")

client("trace-one", "--app-id", "trace-window", "--title", "trace-one", "--size", "400x300")
client("trace-other", "--app-id", "trace-other", "--title", "trace-two", "--size", "240x160")
wait_windows(2)

cli(f"mount {TRACE_CHAIN}")
step("mount chain")
inject("(:type :button :state :pressed :button 272 :x 40 :y 40 :id 1)")
step("chain button one")
inject("(:type :button :state :pressed :button 272 :x 41 :y 41 :id 1)")
step("chain button two")
cli("unmount chain-gamma")
step("unmount chain gamma")
inject("(:type :button :state :pressed :button 273 :x 42 :y 42 :id 1)")
step("chain button three")
cli("unmount chain-beta")
cli("unmount chain-alpha")
step("unmount chain")

cli(f"mount {TRACE_RULES}")
step("mount rules")
inject("(:type :button :state :pressed :button 272 :x 60 :y 60 :id 1)")
step("rule button one")
inject("(:type :button :state :pressed :button 272 :x 61 :y 61 :id 1)")
step("rule button two")
cli("unmount rule-owner")
step("unmount rule owner")

cli(f"mount {TRACE_FAIL}")
step("mount fail")
inject("(:type :button :state :pressed :fail t :button 272 :x 70 :y 70 :id 1)")
step("failing button")
inject("(:type :button :state :pressed :button 272 :x 70 :y 70 :id 1)")
step("recovered button")
cli("unmount trace-fail-a")
cli("unmount trace-fail-b")
step("unmount fail")

cli(f"mount {TRACE_CYCLE}")
step("mount cycle")
inject("(:type :button :state :pressed :cycle t :button 272 :x 80 :y 80 :id 1)")
step("cyclic button")
cli("unmount trace-cycle-a")
cli("unmount trace-cycle-b")
step("unmount cycle")

cli(f"mount {TRACE_MOUNT_A}")
step("mount owner a")
cli(f"mount {TRACE_MOUNT_B}")
step("mount owner b")
inject("(:type :button :state :pressed :button 272 :x 90 :y 90 :id 1)")
step("shared button")
cli("unmount trace-mount-b")
step("unmount owner b")
inject("(:type :button :state :pressed :button 272 :x 91 :y 91 :id 1)")
step("shared button again")
cli("unmount trace-mount-a")
step("unmount owner a")

quit()

report = "\n".join(records) + "\n"
print("TRACE-BEGIN", flush=True)
print(report, flush=True)
print("TRACE-END", flush=True)
print("trace fingerprint: " + hashlib.sha256(report.encode()).hexdigest(), flush=True)
if TRACE_GOLDEN:
    golden = machine.succeed(f"cat {TRACE_GOLDEN}")
    if report.rstrip("\n") != golden.rstrip("\n"):
        import difflib
        print("".join(difflib.unified_diff(golden.rstrip("\n").splitlines(True),
                                           report.rstrip("\n").splitlines(True),
                                           "golden", "trace")))
        raise AssertionError("the transaction trace differs from the recorded base trace")
    print("trace matches the recorded base trace")
