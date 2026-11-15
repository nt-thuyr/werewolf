import fcntl
import os
import resource
import selectors
import signal
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


def frame(kind, request_id, body=b"", status=0):
    return HEADER.pack(kind, len(body), request_id, status) + body


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


def test_wire(server):
    with socket.create_connection(ADDRESS, timeout=12) as peer:
        peer.sendall(frame(1, 0, b"wire"))
        assert recv_frame(peer) == (0xFF, 0, 8, b"")
        peer.sendall(frame(1, 9, b"wire", status=1))
        assert recv_frame(peer) == (0xFF, 9, 8, b"")
        peer.sendall(frame(2, 10))
        assert recv_frame(peer) == (0xFF, 10, 1, b"")
        for request_id, body in enumerate((b"", b"x" * 33, b"wire\x00"), 30):
            peer.sendall(frame(1, request_id, body))
            assert recv_frame(peer) == (0xFF, request_id, 8, b"")
        for byte in frame(1, 11, b"wire"):
            peer.sendall(bytes([byte]))
            time.sleep(0.01)
        assert recv_frame(peer) == (0xFF, 11, 0, b"")

        with socket.create_connection(ADDRESS, timeout=4) as duplicate:
            duplicate.sendall(frame(1, 12, b"wire"))
            assert recv_frame(duplicate) == (0xFF, 12, 6, b"")
            duplicate.sendall(HEADER.pack(2, 0xFFFF, 13, 0))
            assert duplicate.recv(1) == b""
        wait_output(server, b"frame body length 65535 exceeds 4096 bytes")

        peer.sendall(frame(5, 14, b"\x00") + frame(5, 15, struct.pack("!I", 9999))
                     + frame(3, 16) + frame(0x99, 17) + frame(2, 18))
        assert recv_frame(peer) == (0xFF, 14, 8, b"")
        assert recv_frame(peer) == (0xFF, 15, 9, b"")
        assert recv_frame(peer) == (0xFF, 16, 8, b"")
        assert recv_frame(peer) == (0xFF, 17, 8, b"")
        assert recv_frame(peer) == (3, 18, 0, b"\x00\x00")

        request_ids = range(100, 200)
        peer.sendall(b"".join(frame(2, i) for i in request_ids))
        for i in request_ids:
            assert recv_frame(peer) == (3, i, 0, b"\x00\x00")

        with socket.create_connection(ADDRESS, timeout=12) as header, \
                socket.create_connection(ADDRESS, timeout=12) as body:
            header.sendall(b"\x01")
            body.sendall(frame(1, 19, b"unfinished")[:-1])
            peer.sendall(frame(2, 19))
            assert recv_frame(peer) == (3, 19, 0, b"\x00\x00")
            assert header.recv(1) == b""
            assert body.recv(1) == b""
        wait_output(server, b"incomplete frame timed out")

        with socket.create_connection(ADDRESS, timeout=4) as aborted:
            aborted.sendall(frame(1, 19, b"aborted"))
            assert recv_frame(aborted) == (0xFF, 19, 0, b"")
            aborted.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                               struct.pack("ii", 1, 0))
        peer.sendall(frame(2, 19))
        assert recv_frame(peer) == (3, 19, 0, b"\x00\x00")

        peer.sendall(b"".join(frame(5, i, b"\x00") for i in range(20, 25)))
        for i in range(20, 25):
            assert recv_frame(peer) == (0xFF, i, 8, b"")
        assert peer.recv(1) == b""

    with socket.create_connection(ADDRESS, timeout=4) as replacement:
        replacement.sendall(frame(1, 1, b"wire"))
        assert recv_frame(replacement) == (0xFF, 1, 0, b"")

    print("PASS: split and 100 coalesced frames, invalid input, timeout and reset isolation")


def login_peer(stack, username):
    peer = stack.enter_context(socket.create_connection(ADDRESS, timeout=4))
    peer.sendall(frame(1, 1, username))
    assert recv_frame(peer) == (0xFF, 1, 0, b"")
    return peer


def test_room_capacity_and_display():
    with ExitStack() as stack:
        host = login_peer(stack, b"host\n\x1b")
        room_name = b"room\r\x1b"
        host.sendall(frame(4, 2, bytes([len(room_name)]) + room_name + b"\x05"))
        assert recv_frame(host) == (0xFF, 2, 0, b"")
        kind, _, _, snapshot = recv_frame(host)
        assert kind == 7
        target = snapshot[:4]
        room_id = struct.unpack("!I", target)[0]
        viewer = launch(stack, "./build/client", ADDRESS[0], "viewer")
        wait_output(viewer, b"> ")
        command(viewer, "list")
        assert b"room\\x0d\\x1b (1/5)" in wait_output(viewer, b"> ")
        command(viewer, f"join {room_id}")
        wait_output(viewer, b"host\\x0a\\x1b")

        for index in range(3):
            member = login_peer(stack, f"member{index}".encode())
            member.sendall(frame(5, 2, target))
            assert recv_frame(member) == (0xFF, 2, 0, b"")
        extra = login_peer(stack, b"extra")
        extra.sendall(frame(5, 2, target) + frame(2, 3))
        assert recv_frame(extra) == (0xFF, 2, 5, b"")
        expected = b"\x00\x01" + target + bytes([len(room_name)]) + room_name + b"\x05\x05"
        assert recv_frame(extra) == (3, 3, 0, expected)
    print("PASS: full room rejects JOIN and terminal escapes network names")


def test_log_backpressure(server):
    fcntl.fcntl(server.stdout, fcntl.F_SETPIPE_SZ, 4096)
    with ExitStack() as stack:
        healthy = login_peer(stack, b"healthy")
        for _ in range(100):
            with socket.create_connection(ADDRESS, timeout=2) as bad:
                bad.sendall(HEADER.pack(2, 0xFFFF, 1, 0))
                assert bad.recv(1) == b""
        healthy.sendall(frame(2, 2))
        assert recv_frame(healthy) == (3, 2, 0, b"\x00\x00")
        server.terminate()
        assert server.wait(timeout=2) == 0
        assert healthy.recv(1) == b""
    print("PASS: full log pipe leaves server responsive and SIGTERM closes active sessions")


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as stat:
        fields = stat.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])


def test_descriptor_exhaustion():
    with ExitStack() as stack:
        inherited = [
            stack.enter_context(open(os.devnull, "rb")).fileno() for _ in range(16)
        ]
        server = stack.enter_context(subprocess.Popen(
            ["./build/server"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            pass_fds=inherited,
        ))
        stack.callback(stop, server)
        wait_output(server, b"Listening on ")
        healthy = login_peer(stack, b"existing")
        extra = login_peer(stack, b"disposable")
        limit = max(map(int, os.listdir(f"/proc/{server.pid}/fd"))) + 1
        resource.prlimit(server.pid, resource.RLIMIT_NOFILE, (limit, limit))

        pending = stack.enter_context(socket.create_connection(ADDRESS, timeout=2))
        pending.sendall(frame(1, 1, b"pending"))
        before = cpu_ticks(server.pid)
        time.sleep(0.5)
        assert cpu_ticks(server.pid) - before < os.sysconf("SC_CLK_TCK") / 4
        healthy.sendall(frame(2, 2))
        assert recv_frame(healthy) == (3, 2, 0, b"\x00\x00")
        extra.close()
        assert recv_frame(pending) == (0xFF, 1, 0, b"")
        server.send_signal(signal.SIGINT)
        assert server.wait(timeout=2) == 0
        assert pending.recv(1) == b""
    print("PASS: descriptor exhaustion avoids busy-wait, recovers and handles SIGINT")


def main():
    with ExitStack() as stack:
        server = launch(stack, "./build/server")
        wait_output(server, b"Listening on ")
        descriptors = f"/proc/{server.pid}/fd"
        initial_count = len(os.listdir(descriptors))
        test_demo()
        test_wire(server)
        test_room_capacity_and_display()
        assert server.poll() is None
        deadline = time.monotonic() + 4
        while len(os.listdir(descriptors)) != initial_count:
            assert time.monotonic() < deadline, "Server left client descriptors open"
            time.sleep(0.02)
        print("PASS: server descriptor count returns to baseline")
        test_log_backpressure(server)
    test_descriptor_exhaustion()


if __name__ == "__main__":
    main()
