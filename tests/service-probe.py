import os
import socket
import sys
import time

socket_path, pid_path, fork, child_path = sys.argv[1:5]
if socket_path == "-":
    root = os.environ.get("TOMOE_SERVICE_RUNTIME_DIR")
    if not root:
        raise SystemExit("TOMOE_SERVICE_RUNTIME_DIR is not set")
    socket_path = os.path.join(root, "probe.sock")
directory = os.path.dirname(socket_path)
if directory:
    os.makedirs(directory, exist_ok=True)
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    os.unlink(socket_path)
except FileNotFoundError:
    pass
server.bind(socket_path)
server.listen(8)
with open(pid_path, "w") as file:
    file.write(str(os.getpid()))
if fork == "1":
    child = os.fork()
    if child == 0:
        with open(child_path, "w") as file:
            file.write(str(os.getpid()))
        while True:
            time.sleep(1)
while True:
    time.sleep(1)
