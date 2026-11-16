import os
import socket
import struct
import sys

CLIENT_OPEN_SEAT = 1
CLIENT_CLOSE_SEAT = 2
CLIENT_OPEN_DEVICE = 3
CLIENT_CLOSE_DEVICE = 4
CLIENT_SWITCH_SESSION = 6
CLIENT_PING = 7
SERVER_SEAT_OPENED = 0x8001
SERVER_SEAT_CLOSED = 0x8002
SERVER_DEVICE_CLOSED = 0x8004
SERVER_ENABLE_SEAT = 0x8006
SERVER_PONG = 0x8007
SERVER_SESSION_SWITCHED = 0x8008
SERVER_ERROR = 0xffff


def send(connection, opcode, payload=b""):
    connection.sendall(struct.pack("<HH", opcode, len(payload)) + payload)


def recv_exact(connection, count):
    data = b""
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


path = sys.argv[1]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
connection, _ = server.accept()
opened = refused = closed = False
while True:
    header = recv_exact(connection, 4)
    if header is None:
        break
    opcode, size = struct.unpack("<HH", header)
    payload = recv_exact(connection, size) if size else b""
    if payload is None:
        break
    if opcode == CLIENT_OPEN_SEAT:
        opened = True
        send(connection, SERVER_SEAT_OPENED, struct.pack("<H", 6) + b"seat0\0")
        send(connection, SERVER_ENABLE_SEAT)
    elif opcode == CLIENT_OPEN_DEVICE:
        refused = True
        send(connection, SERVER_ERROR, struct.pack("<i", 2))
    elif opcode == CLIENT_CLOSE_SEAT:
        closed = True
        send(connection, SERVER_SEAT_CLOSED)
    elif opcode == CLIENT_PING:
        send(connection, SERVER_PONG)
    elif opcode == CLIENT_CLOSE_DEVICE:
        send(connection, SERVER_DEVICE_CLOSED)
    elif opcode == CLIENT_SWITCH_SESSION:
        send(connection, SERVER_SESSION_SWITCHED)
    else:
        break
print("seat opened" if opened else "seat not opened", flush=True)
print("device refused" if refused else "device not refused", flush=True)
print("seat closed" if closed else "seat not closed", flush=True)
connection.close()
server.close()
