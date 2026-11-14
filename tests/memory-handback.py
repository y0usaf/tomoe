HANDBACK_SAMPLES = 4
HANDBACK_WAIT = 1.8


def handback_gate(label, handbacks, period):
    assert handbacks, f"{label}: no idle handback was logged in the measured window"
    pauses = sorted(end - start for start, end in handbacks)
    print(
        f"HANDBACK {label} period_ns={period} count={len(handbacks)}"
        f" min_us={pauses[0] // 1000} median_us={pauses[len(pauses) // 2] // 1000}"
        f" max_us={pauses[-1] // 1000} pauses_us={[pause // 1000 for pause in pauses]}",
        flush=True,
    )
    assert pauses[len(pauses) // 2] <= period, (
        f"{label}: the median idle handback paused {pauses[len(pauses) // 2]} ns over one"
        f" refresh ({period} ns)")
    return pauses


def handback_window(prefix):
    fresh("--bare", bus=False, env={"TOMOE_TIMINGS": "1"})
    cli(f"mount {REFRESH_POLICY}")
    cli(f"mount {SCENE_POLICY}")
    cli(f"mount {HANDBACK_POLICY}")
    open_windows(2, width=320, height=240, prefix=prefix)
    time.sleep(3)
    refresh, period = refresh_period()
    print("handback refresh:", refresh, "period:", period, flush=True)
    assert refresh >= BUDGET_REFRESH_MHZ, (
        f"the measured output runs at {refresh} mHz, below the {BUDGET_REFRESH_MHZ} mHz"
        " refresh the handback budget is stated against")
    before = measure("handback-before")
    cli("timings reset")
    cli("command churn")
    churn = [plist(entry).get(":STATE") for entry in items(inspect()[":EXTENSIONS"])
             if text(plist(entry).get(":NAME", '""')) == "memory-handback"]
    assert churn and plist(churn[0]).get(":CELLS") == "600000", churn
    for _ in range(HANDBACK_SAMPLES - 1):
        time.sleep(HANDBACK_WAIT)
        cli("command churn")
    time.sleep(HANDBACK_WAIT)
    _, _, handbacks = read_timings("idle-handback")
    after = measure("handback-after")
    close_windows(2, prefix=prefix)
    quit()
    assert after["dynamic-usage"] <= before["dynamic-usage"] + CHURN_LIMIT, (
        f"the idle handback left {after['dynamic-usage'] - before['dynamic-usage']} bytes of"
        f" the churn behind, over {CHURN_LIMIT}")
    return handbacks, period


def slow_handback_window(prefix, period):
    fresh("--bare", bus=False, env={"TOMOE_TIMINGS": "1",
                                    "TOMOE_TEST_HANDBACK_PAUSE_US": str(period * 3 // 2 // 1000)})
    cli(f"mount {REFRESH_POLICY}")
    cli(f"mount {SCENE_POLICY}")
    cli(f"mount {HANDBACK_POLICY}")
    open_windows(2, width=320, height=240, prefix=prefix)
    time.sleep(3)
    slow = []
    for attempt in range(3):
        cli("timings reset")
        for sample in range(HANDBACK_SAMPLES):
            if sample:
                time.sleep(HANDBACK_WAIT)
            cli("command churn")
        time.sleep(HANDBACK_WAIT)
        _, _, slow = read_timings(f"forced-slow-handback-{attempt}")
        if slow:
            break
    close_windows(2, prefix=prefix)
    quit()
    assert slow and min(end - start for start, end in slow) > period, (
        "the test handback pause floor did not reach the handback: "
        + str(sorted(end - start for start, end in slow)))
    try:
        handback_gate("forced-slow-handback", slow, period)
    except AssertionError as error:
        print("forced slow handback rejected:", error, flush=True)
    else:
        raise AssertionError("an idle handback slower than one refresh was accepted")
