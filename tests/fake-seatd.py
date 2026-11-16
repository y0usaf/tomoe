import fcntl
import os
import socket
import struct
import sys

CLIENT_OPEN_SEAT = 1
CLIENT_CLOSE_SEAT = 2
CLIENT_OPEN_DEVICE = 3
CLIENT_CLOSE_DEVICE = 4
CLIENT_DISABLE_SEAT = 5
CLIENT_SWITCH_SESSION = 6
CLIENT_PING = 7
SERVER_SEAT_OPENED = 0x8001
SERVER_SEAT_CLOSED = 0x8002
SERVER_DEVICE_OPENED = 0x8003
SERVER_DEVICE_CLOSED = 0x8004
SERVER_DISABLE_SEAT = 0x8005
SERVER_ENABLE_SEAT = 0x8006
SERVER_PONG = 0x8007
SERVER_SESSION_SWITCHED = 0x8008
SERVER_SEAT_DISABLED = 0x8009
SERVER_ERROR = 0xffff
DRM_IOCTL_SET_MASTER = 0x641e


def send_frames(connection, frames, fds=()):
    data = b"".join(struct.pack("<HH", opcode, len(payload)) + payload
                    for opcode, payload in frames)
    if fds:
        connection.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                     struct.pack("%di" % len(fds), *fds))])
    else:
        connection.sendall(data)


def recv_exact(connection, count):
    data = b""
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


path = sys.argv[1]
mode = sys.argv[2]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
connection, _ = server.accept()
opened = refused = handed = pinged = acked = closed = False
while True:
    header = recv_exact(connection, 4)
    if header is None:
        break
    opcode, size = struct.unpack("<HH", header)
    payload = recv_exact(connection, size) if size else b""
    if payload is None:
        break
    print("request", opcode, flush=True)
    if opcode == CLIENT_OPEN_SEAT:
        opened = True
        send_frames(connection, [(SERVER_SEAT_OPENED, struct.pack("<H", 6) + b"seat0\0"),
                                 (SERVER_ENABLE_SEAT, b"")])
    elif opcode == CLIENT_OPEN_DEVICE:
        device = payload[2:].split(b"\0")[0].decode()
        print("open", device, flush=True)
        if mode == "error":
            refused = True
            send_frames(connection, [(SERVER_ERROR, struct.pack("<i", 2)),
                                     (SERVER_DISABLE_SEAT, b"")])
            print("refused with disable", flush=True)
        else:
            fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK |
                          os.O_CLOEXEC)
            if device.startswith("/dev/dri/card"):
                try:
                    fcntl.ioctl(fd, DRM_IOCTL_SET_MASTER)
                except OSError:
                    pass
                handed = True
                frames = [(SERVER_DEVICE_OPENED, struct.pack("<i", 1)),
                          (SERVER_DISABLE_SEAT, b"")]
                print("opened with disable", flush=True)
            else:
                frames = [(SERVER_DEVICE_OPENED, struct.pack("<i", 2))]
            send_frames(connection, frames, [fd])
            os.close(fd)
    elif opcode == CLIENT_PING:
        pinged = True
        print("ping", flush=True)
        send_frames(connection, [(SERVER_PONG, b"")])
    elif opcode == CLIENT_DISABLE_SEAT:
        acked = True
        print("disable acked", flush=True)
        send_frames(connection, [(SERVER_SEAT_DISABLED, b"")])
    elif opcode == CLIENT_CLOSE_SEAT:
        closed = True
        print("seat closed", flush=True)
        send_frames(connection, [(SERVER_SEAT_CLOSED, b"")])
    elif opcode == CLIENT_CLOSE_DEVICE:
        send_frames(connection, [(SERVER_DEVICE_CLOSED, b"")])
    elif opcode == CLIENT_SWITCH_SESSION:
        send_frames(connection, [(SERVER_SESSION_SWITCHED, b"")])
    else:
        break
print("seat opened" if opened else "seat not opened", flush=True)
print("device refused" if refused else "device not refused", flush=True)
print("device handed" if handed else "device not handed", flush=True)
print("ping seen" if pinged else "ping not seen", flush=True)
print("disable acked" if acked else "disable not acked", flush=True)
print("seat closed" if closed else "seat not closed", flush=True)
connection.close()
server.close()
