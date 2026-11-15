ARENA_SAMPLES = 4
ARENA_WAIT = 0.5


def arena_window(prefix):
    fresh("--bare", bus=False)
    cli(f"mount {REFRESH_POLICY}")
    cli(f"mount {SCENE_POLICY}")
    cli(f"mount {HANDBACK_POLICY}")
    open_windows(2, width=320, height=240, prefix=prefix)
    time.sleep(3)
    refresh, period = refresh_period()
    before = measure("arena-before")
    for _ in range(ARENA_SAMPLES):
        cli("command churn")
        time.sleep(ARENA_WAIT)
    churn = [plist(entry).get(":STATE") for entry in items(inspect()[":EXTENSIONS"])
             if text(plist(entry).get(":NAME", '""')) == "memory-handback"]
    assert churn and plist(churn[0]).get(":CELLS") == "300000", churn
    after = measure("arena-after")
    close_windows(2, prefix=prefix)
    quit()
    print(f"ARENA period_ns={period} high_water={after['arena-high-water']}"
          f" reclaim_longest_us={after['reclaim-longest-us']}"
          f" persistent_before={before['persistent-used']}"
          f" persistent_after={after['persistent-used']}", flush=True)
    assert after["arena-high-water"] >= 300000 * 16, "the churn never reached the dispatch arena"
    assert after["arena-used"] == 0, f"{after['arena-used']} arena bytes outlived the dispatch"
    assert after["persistent-used"] <= before["persistent-used"] + CHURN_LIMIT, (
        f"the churn left {after['persistent-used'] - before['persistent-used']} persistent bytes")
    assert after["reclaim-longest-us"] * 1000 <= period, (
        f"the longest Lisp reclaim took {after['reclaim-longest-us']} us, over one refresh"
        f" ({period} ns)")
    return after["reclaim-longest-us"]
