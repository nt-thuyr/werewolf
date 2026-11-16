"""CLIENT-03: real clients against the lobby server and a protocol peer."""

import socket
import struct
from contextlib import ExitStack

from lobby import ADDRESS, command, frame, launch, recv_frame, wait_output


def test_administration():
    with ExitStack() as stack:
        server = launch(stack, "./build/server")
        wait_output(server, b"Listening on ")
        alice = launch(stack, "./build/client", ADDRESS[0], "alice")
        bob = launch(stack, "./build/client", ADDRESS[0], "bob")
        wait_output(alice, b"> ")
        wait_output(bob, b"> ")
        command(alice, "create village 5")
        assert b"You are the host" in wait_output(alice, b"You are the host")
        command(alice, "invite bob")
        wait_output(alice, b"Invitation sent")
        assert b"Invitation from alice: join 1" in wait_output(bob, b"join 1")
        command(bob, "join 1")
        assert b"You are a member" in wait_output(bob, b"You are a member")
        wait_output(alice, b"  2 bob\n")
        command(bob, "kick 1")
        wait_output(bob, b"ERR_UNAUTHORIZED (0x02)")
        command(alice, "kick 999")
        wait_output(alice, b"ERR_INVALID_TARGET (0x04)")
        command(alice, "kick 2")
        wait_output(alice, b"You are the host")
        wait_output(bob, b"Kicked by host; returned to lobby")
        # KICK keeps the TCP session usable, and the room has one member again.
        command(bob, "list")
        wait_output(bob, b"village (1/5)")
        command(bob, "join 1")
        wait_output(bob, b"You are a member")
        wait_output(alice, b"  2 bob\n")
        command(alice, "leave")
        wait_output(alice, b"Left room; returned to lobby")
        promoted = wait_output(bob, b"You are the host")
        assert b"host_id=2 self_id=2" in promoted
        command(bob, "invite alice")
        wait_output(bob, b"Invitation sent")
        wait_output(alice, b"Invitation from bob: join 1")
        command(bob, "leave")
        wait_output(bob, b"Left room; returned to lobby")
        command(bob, "list")
        wait_output(bob, b"Rooms: 0")
        command(bob, "leave")
        wait_output(bob, b"ERR_UNAUTHORIZED (0x02)")
        # SERVER-05 is absent: report the error without disconnecting or starting locally.
        command(bob, "start")
        wait_output(bob, b"ERR_INVALID_PAYLOAD (0x08)")
        command(bob, "list")
        wait_output(bob, b"Rooms: 0")
    print("PASS: client invite/kick/leave, errors, host transfer and persistent sessions")


def open_fake(stack):
    address = ("127.0.0.2", ADDRESS[1])
    listener = stack.enter_context(socket.create_server(address))
    listener.settimeout(4)
    client = launch(stack, "./build/client", address[0], "alice")
    peer, _ = listener.accept()
    stack.enter_context(peer)
    peer.settimeout(4)
    assert recv_frame(peer) == (1, 1, 0, b"alice")
    peer.sendall(frame(0xFF, 1))
    wait_output(client, b"> ")
    return client, peer


def test_start_and_interleaved_push():
    with ExitStack() as stack:
        client, peer = open_fake(stack)
        room = struct.pack("!IIBI", 7, 101, 1, 101) + b"\x05alice"
        peer.sendall(frame(7, 0, room))
        wait_output(client, b"You are the host")
        command(client, "start")
        assert recv_frame(peer) == (0x14, 2, 0, b"")
        # An unsolicited invite does not consume the pending start request.
        notice = frame(0x11, 0, struct.pack("!I", 9) + b"\x03bob")
        for byte in notice:
            peer.sendall(bytes([byte]))
        wait_output(client, b"Invitation from bob: join 9")
        peer.sendall(frame(0xFF, 2, status=0x0A))
        wait_output(client, b"ERR_NOT_ENOUGH_PLAYERS (0x0A)")
        command(client, "start")
        assert recv_frame(peer) == (0x14, 3, 0, b"")
        peer.sendall(frame(0xFF, 3))
        wait_output(client, b"Start accepted; waiting for role and phase from server")
        # Acceptance alone is not a phase transition. This peer sends no game events.
    print("PASS: START bytes, error/success acknowledgement and fragmented interleaved push")


def test_invalid_notifications():
    invalid = [
        frame(0x11, 0, b"\x00\x00\x00"),
        frame(0x11, 0, struct.pack("!I", 7) + b"\x03bobX"),
        frame(0x11, 2, struct.pack("!I", 7) + b"\x03bob"),
        frame(0x13, 0, b""),
        frame(0x13, 0, b"\x01\x01"),
        frame(0x13, 0, b"\x02"),
    ]
    for packet in invalid:
        with ExitStack() as stack:
            client, peer = open_fake(stack)
            peer.sendall(packet)
            wait_output(client, b"Unexpected message from server")
            assert client.wait(timeout=4) != 0
    print("PASS: malformed invitation/kick notifications fail without applying state")


if __name__ == "__main__":
    test_administration()
    test_start_and_interleaved_push()
    test_invalid_notifications()
