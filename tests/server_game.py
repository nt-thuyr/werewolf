import os
import signal
import socket
import struct
import time
from collections import Counter
from contextlib import ExitStack

from lobby import expect_room_members, frame, launch, login_peer, recv_frame, wait_output


def request(peer, kind, request_id, body=b"", status=0):
    peer.sendall(frame(kind, request_id, body))
    response = recv_frame(peer)
    assert response == (0xFF, request_id, status, b""), response


def test_play_and_restart():
    with ExitStack() as stack:
        server = launch(stack, "./build/server")
        wait_output(server, b"Listening on ")
        names = [b"alice", b"bob", b"carol", b"dave", b"eve"]
        host, *others = peers = [login_peer(stack, name) for name in names]
        guest = login_peer(stack, b"guest")
        request(guest, 0x14, 2, status=0x02)
        request(host, 0x04, 2, b"\x07village\x06")
        room_id, players = expect_room_members(host, names[:1])
        room_body = struct.pack("!I", room_id)

        for count, peer in enumerate(others, start=2):
            if count == 5:
                request(others[0], 0x14, 3, status=0x02)
                request(host, 0x14, 3, status=0x0A)
            request(peer, 0x05, 2, room_body)
            for member in peers[:count]:
                _, players = expect_room_members(member, names[:count])

        request(host, 0x14, 4, b"\x00", status=0x08)
        request(host, 0x14, 5)
        roles = []
        wolves = []
        deadlines = set()
        for peer in peers:
            header = recv_frame(peer)
            assert header[:3] == (0x20, 0, 0) and len(header[3]) == 1, header
            role = header[3][0]
            roles.append(role)
            if role == 1:
                wolves.append(recv_frame(peer))
            kind, request_id, status, body = recv_frame(peer)
            assert (kind, request_id, status) == (0x30, 0, 0)
            phase, duration, deadline = struct.unpack("!BHQ", body)
            assert (phase, duration) == (1, 30)
            deadlines.add(deadline)

        assert Counter(roles) == {1: 1, 2: 2, 3: 1, 4: 1}, roles
        assert len(deadlines) == 1
        assert 0 < min(deadlines) - time.monotonic_ns() // 1_000_000 <= 30_000
        wolf_index = roles.index(1)
        wolf_id = players[names[wolf_index]]
        assert wolves == [(0x22, 0, 0, struct.pack("!BI", 1, wolf_id))]
        request(host, 0x14, 6, status=0x07)
        request(guest, 0x05, 3, room_body, status=0x07)
        request(host, 0x10, 7, b"\x05guest", status=0x07)
        request(host, 0x12, 8, struct.pack("!I", players[names[1]]), status=0x07)

        seer = peers[roles.index(3)]
        guard_index = roles.index(4)
        guard_id = players[names[guard_index]]
        request(seer, 0x31, 10, struct.pack("!I", wolf_id))
        assert recv_frame(seer) == (0x32, 0, 0, struct.pack("!IB", wolf_id, 1))
        request(peers[wolf_index], 0x31, 10, struct.pack("!I", guard_id))
        request(peers[guard_index], 0x31, 10, struct.pack("!I", guard_id))
        for peer in peers:
            assert recv_frame(peer) == (0x34, 0, 0, b"\x00" * 4)
            kind, request_id, status, body = recv_frame(peer)
            assert (kind, request_id, status) == (0x30, 0, 0)
            assert struct.unpack("!BHQ", body)[:2] == (2, 90)

        request(host, 0x37, 11, b"\x00hello")
        chat = b"\x00" + struct.pack("!I", players[names[0]]) + b"hello"
        for peer in peers:
            assert recv_frame(peer) == (0x39, 0, 0, chat)
            peer.settimeout(98)
        for peer in peers:
            kind, request_id, status, body = recv_frame(peer)
            assert (kind, request_id, status) == (0x30, 0, 0)
            assert struct.unpack("!BHQ", body)[:2] == (3, 30)
            peer.settimeout(38)

        for i, peer in enumerate(peers):
            target_id = 0 if i == wolf_index else wolf_id
            request(peer, 0x35, 12, struct.pack("!I", target_id))
        for peer in peers:
            assert recv_frame(peer) == (0x36, 0, 0, struct.pack("!I", wolf_id))
            assert recv_frame(peer) == (0x38, 0, 0, b"\x01")
            expect_room_members(peer, names)
        request(host, 0x14, 13)
        assert recv_frame(host)[:3] == (0x20, 0, 0)
    print("PASS: START, private events, night, day chat, vote, game over and restart over TCP")


def test_idle_night():
    with ExitStack() as stack:
        server = launch(stack, "./build/server")
        wait_output(server, b"Listening on ")
        names = [b"alice", b"bob", b"carol", b"dave", b"eve"]
        peers = [login_peer(stack, name) for name in names]
        host = peers[0]
        request(host, 0x04, 2, b"\x07village\x05")
        room_id, _ = expect_room_members(host, names[:1])
        for count, peer in enumerate(peers[1:], start=2):
            request(peer, 0x05, 2, struct.pack("!I", room_id))
            for member in peers[:count]:
                expect_room_members(member, names[:count])
        request(host, 0x14, 3)
        roles = []
        for peer in peers:
            role = recv_frame(peer)[3][0]
            roles.append(role)
            if role == 1:
                assert recv_frame(peer)[0] == 0x22
            kind, request_id, status, body = recv_frame(peer)
            assert (kind, request_id, status) == (0x30, 0, 0)
            assert struct.unpack("!BHQ", body)[:2] == (1, 30)
            peer.settimeout(38)

        for peer in peers:
            assert recv_frame(peer) == (0x34, 0, 0, b"\x00" * 4)
            kind, request_id, status, body = recv_frame(peer)
            assert (kind, request_id, status) == (0x30, 0, 0)
            assert struct.unpack("!BHQ", body)[:2] == (2, 90)

        leaver_index = next(i for i in range(1, len(peers)) if roles[i] != 1)
        leaver = peers.pop(leaver_index)
        names.pop(leaver_index)
        request(leaver, 0x06, 3)
        for peer in peers:
            expect_room_members(peer, names)
        leaver.sendall(frame(0x02, 4))
        kind, request_id, status, _ = recv_frame(leaver)
        assert (kind, request_id, status) == (0x03, 4, 0)

        host.close()
        for peer in peers[1:]:
            expect_room_members(peer, names[1:])
    print("PASS: idle night deadline, voluntary leave and lobby host transfer on EOF")


def test_start_after_disconnect(address="127.0.0.1"):
    with ExitStack() as stack:
        server = launch(stack, "./build/server", address)
        wait_output(server, b"Listening on ")
        names = [b"alice", b"bob", b"carol", b"dave", b"eve"]
        peers = [login_peer(stack, name) for name in names]
        host = peers[0]
        request(host, 0x04, 2, b"\x07village\x05")
        room_id, _ = expect_room_members(host, names[:1])
        for count, peer in enumerate(peers[1:], start=2):
            request(peer, 0x05, 2, struct.pack("!I", room_id))
            for member in peers[:count]:
                expect_room_members(member, names[:count])

        server.send_signal(signal.SIGSTOP)
        try:
            _, status = os.waitpid(server.pid, os.WUNTRACED)
            assert os.WIFSTOPPED(status)
            host.sendall(frame(0x14, 3))
            peers[-1].shutdown(socket.SHUT_WR)
        finally:
            server.send_signal(signal.SIGCONT)

        expect_room_members(host, names[:-1])
        assert recv_frame(host) == (0xFF, 3, 0x0A, b"")
    print("PASS: START excludes a member whose EOF is ready in the same poll cycle")


if __name__ == "__main__":
    test_start_after_disconnect()
    test_play_and_restart()
    test_idle_night()
