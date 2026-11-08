import os
import selectors
import socket
import struct
import subprocess
import time
from contextlib import ExitStack

ADDRESS = ("127.0.0.1", 5555)
HEADER = struct.Struct("!BHIB")


def wait_output(process, marker):
    result = bytearray()
    deadline = time.monotonic() + 4
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while marker not in result:
            if not selector.select(max(0, deadline - time.monotonic())):
                raise AssertionError(f"Expected {marker!r}, got {result!r}")
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                raise AssertionError(f"Process exited before {marker!r}: {result!r}")
            result.extend(chunk)
    return bytes(result)


def command(client, line):
    client.stdin.write((line + "\n").encode())
    client.stdin.flush()


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def launch(stack, *args):
    process = stack.enter_context(subprocess.Popen(
        args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    ))
    stack.callback(stop, process)
    return process


def test_demo():
    with ExitStack() as stack:
        alice = launch(stack, "./build/client", ADDRESS[0], "alice")
        assert b"Logged in as alice" in wait_output(alice, b"> ")
        bob = launch(stack, "./build/client", ADDRESS[0], "bob")
        assert b"Logged in as bob" in wait_output(bob, b"> ")
        command(alice, "create LangSoi 5")
        created = wait_output(alice, b"  1 alice\n")
        assert b"Room created" in created
        assert b"room_id=1 host_id=1 self_id=1" in created

        command(bob, "list")
        assert b"room_id=1 LangSoi (1/5)" in wait_output(bob, b"> ")
        command(bob, "join 9999")
        assert b"ERR_ROOM_NOT_FOUND (0x09)" in wait_output(bob, b"> ")
        with selectors.DefaultSelector() as selector:
            selector.register(alice.stdout, selectors.EVENT_READ)
            assert not selector.select(0.15), "Failed JOIN updated Alice's room"

        command(bob, "join 1")
        alice_update = wait_output(alice, b"  2 bob\n")
        bob_update = wait_output(bob, b"  2 bob\n")
        for output, self_id in ((alice_update, 1), (bob_update, 2)):
            assert f"room_id=1 host_id=1 self_id={self_id}".encode() in output
            assert b"  1 alice\n  2 bob\n" in output
        assert bob_update.index(b"Room joined") < bob_update.index(b"Room update:")
        command(bob, "list")
        assert b"ERR_UNAUTHORIZED (0x02)" in wait_output(bob, b"> ")
        print("PASS: Alice/Bob login, create, list, missing room, join and unsolicited update")
        print(alice_update.decode().strip())
        print(bob_update.decode().strip())

        alice.stdin.close()
        assert alice.wait(timeout=4) == 0
        moved = wait_output(bob, b"  2 bob\n")
        assert b"room_id=1 host_id=2 self_id=2" in moved
        assert b"  1 alice\n" not in moved
        bob.stdin.close()
        assert bob.wait(timeout=4) == 0
        with socket.create_connection(ADDRESS, timeout=4) as check:
            check.sendall(frame(1, 1, b"alice") + frame(2, 2))
            assert recv_frame(check) == (0xFF, 1, 0, b"")
            assert recv_frame(check) == (3, 2, 0, b"\x00\x00")
        print("PASS: Host transfers on disconnect, empty room is removed, username reusable")


def frame(kind, request_id, body=b""):
    return HEADER.pack(kind, len(body), request_id, 0) + body


def recv_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise AssertionError("Socket closed before full frame")
        result.extend(chunk)
    return result


def recv_frame(sock):
    kind, size, request_id, status = HEADER.unpack(recv_exact(sock, HEADER.size))
    return kind, request_id, status, bytes(recv_exact(sock, size))


def test_wire():
    with socket.create_connection(ADDRESS, timeout=12) as peer:
        peer.sendall(frame(2, 10))
        assert recv_frame(peer) == (0xFF, 10, 1, b"")
        for byte in frame(1, 11, b"wire"):
            peer.sendall(bytes([byte]))
            time.sleep(0.01)
        assert recv_frame(peer) == (0xFF, 11, 0, b"")

        with socket.create_connection(ADDRESS, timeout=4) as duplicate:
            duplicate.sendall(frame(1, 12, b"wire"))
            assert recv_frame(duplicate) == (0xFF, 12, 6, b"")
            duplicate.sendall(HEADER.pack(2, 4097, 13, 0))
            assert duplicate.recv(1) == b""

        peer.sendall(frame(5, 14, b"\x00") + frame(5, 15, struct.pack("!I", 9999))
                     + frame(2, 16))
        assert recv_frame(peer) == (0xFF, 14, 8, b"")
        assert recv_frame(peer) == (0xFF, 15, 9, b"")
        assert recv_frame(peer) == (3, 16, 0, b"\x00\x00")

        peer.sendall(b"".join(frame(5, i, b"\x00") for i in range(20, 25)))
        for i in range(20, 25):
            assert recv_frame(peer) == (0xFF, i, 8, b"")
        assert peer.recv(1) == b""

    with socket.create_connection(ADDRESS, timeout=12) as stalled:
        stalled.sendall(b"\x01")
        assert stalled.recv(1) == b""
    print("PASS: partial and coalesced frames, request IDs, invalid input isolation and frame timeout")


def main():
    with ExitStack() as stack:
        server = launch(stack, "./build/server")
        wait_output(server, b"Listening on ")
        descriptors = f"/proc/{server.pid}/fd"
        initial_count = len(os.listdir(descriptors))
        test_demo()
        test_wire()
        assert server.poll() is None
        deadline = time.monotonic() + 4
        while len(os.listdir(descriptors)) != initial_count:
            assert time.monotonic() < deadline, "Server left client descriptors open"
            time.sleep(0.02)
        print("PASS: server descriptor count returns to baseline")


if __name__ == "__main__":
    main()
