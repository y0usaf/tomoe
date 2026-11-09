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
quit()
