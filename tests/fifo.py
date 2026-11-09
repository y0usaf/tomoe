start("--bare")
machine.succeed(ENV + "WAYLAND_DISPLAY=check wayland-info > /run/fifo-globals")
machine.succeed("grep -q wp_fifo_manager_v1 /run/fifo-globals")
for mode in ["paced", "unpaced", "mixed", "destroy", "sync", "power", "duplicate", "dead-set", "dead-wait"]:
    machine.succeed(ENV + f"WAYLAND_DISPLAY=check tomoe-pacing-client {mode}")
quit()
