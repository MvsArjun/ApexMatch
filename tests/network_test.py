#!/usr/bin/env python3
"""Portable localhost integration checks. No third-party Python packages."""
import argparse
import json
import queue
import socket
import subprocess
import threading


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(5)
        self.pending = b""
        assert self.line()["type"] == "HELLO"

    def line(self):
        while b"\n" not in self.pending:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("connection closed")
            self.pending += data
        line, self.pending = self.pending.split(b"\n", 1)
        return json.loads(line)

    def response(self):
        result = []
        while True:
            event = self.line()
            result.append(event)
            if event.get("type") == "RESPONSE_END":
                return result

    def command(self, text, fragments=False):
        data = text.encode() + b"\n"
        if fragments:
            for byte in data:
                self.sock.sendall(bytes([byte]))
        else:
            self.sock.sendall(data)
        return self.response()

    def close(self):
        self.sock.close()


def start_server(path, *arguments):
    process = subprocess.Popen([path, "--port", "0", *arguments], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    ready = queue.Queue()
    threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True).start()
    try:
        line = ready.get(timeout=15)
        assert line.startswith("LISTENING 127.0.0.1:"), repr(line)
        port = int(line.rsplit(":", 1)[1])
        return process, port
    except BaseException:
        process.kill()
        process.wait(timeout=5)
        raise


def finish_server(process, port, already_stopping=False):
    if process.poll() is None and not already_stopping:
        peer = Peer(port)
        result = peer.command("SHUTDOWN")
        assert any(event.get("type") == "SHUTDOWN" for event in result)
        peer.close()
    code = process.wait(timeout=8)
    errors = process.stderr.read()
    assert code == 0, (code, errors)


def event_kind(event):
    # Engine structured events have a type field; permit casing differences.
    return str(event.get("type", event.get("kind", ""))).upper()


def regular_tests(server, client):
    process, port = start_server(server)
    peers = []
    try:
        order = Peer(port)
        peers.append(order)
        subscribed = Peer(port)
        peers.append(subscribed)
        initial = subscribed.command("SUBSCRIBE")
        assert initial[0]["type"] == "MD_SNAPSHOT"
        reconstructed = {}

        def market_batch():
            messages = []
            while True:
                event = subscribed.line()
                messages.append(event)
                if event["type"] == "MD_LEVEL":
                    key = (event["side"], event["price"])
                    if event["quantity"]:
                        reconstructed[key] = (event["quantity"], event["orders"])
                    else:
                        reconstructed.pop(key, None)
                if event["type"] == "MD_SNAPSHOT":
                    depth = event["depth"]
                    expected = {}
                    for side, key in (("BUY", "bids"), ("SELL", "asks")):
                        for level in depth[key]:
                            expected[(side, level["price"])] = (level["quantity"], level["orders"])
                    assert reconstructed == expected, (reconstructed, expected)
                    top = next(message for message in messages if message["type"] == "MD_TOP")
                    assert top["bid"] == (depth["bids"][0] if depth["bids"] else None)
                    assert top["ask"] == (depth["asks"][0] if depth["asks"] else None)
                    return messages

        response = order.command("ADD 1 BUY 100 9", fragments=True)
        assert any(event_kind(event) == "ACCEPTED" for event in response), response
        md = market_batch()
        assert [event["type"] for event in md] == ["MD_LEVEL", "MD_TOP", "MD_SNAPSHOT"]
        assert md[0]["quantity"] == 9 and md[0]["orders"] == 1
        assert md[1]["bid"]["price"] == 100 and md[1]["ask"] is None
        assert md[2]["depth"]["bids"][0]["quantity"] == 9
        # Multiple complete frames in one write retain request order.
        order.sock.sendall(b"ADD 2 SELL 100 4\nBOOK\nSTATS\n")
        trade = order.response()
        assert any(event_kind(event) == "EXECUTION" for event in trade), trade
        assert order.response()[-1]["command_sequence"] == 3
        assert order.response()[-1]["command_sequence"] == 4
        market_batch()  # partial execution replaces BUY 100 aggregate with five.
        for text in ("ADD 3 BUY 99 2", "ADD 4 BUY 98 2", "ADD 5 SELL 98 8"):
            order.command(text)
            last_market = market_batch()
        assert sum(event["type"] == "MD_LEVEL" for event in last_market) == 3
        queried = order.command("BOOK")[0]
        assert queried == last_market[-1]["depth"], (queried, last_market)
        bad = order.command("ADD nonsense BUY 100 1")
        assert any(event_kind(event) == "REJECTED" for event in bad), bad
        unknown = order.command("BOGUS")
        assert any(event_kind(event) == "REJECTED" for event in unknown), unknown
        # A half-close drains all already framed requests and reports a trailing frame.
        closing = Peer(port)
        peers.append(closing)
        closing.sock.sendall(b"BOOK\nCANCEL 1")
        closing.sock.shutdown(socket.SHUT_WR)
        assert closing.response()[-1]["command_sequence"] == 1
        trailing = closing.response()
        assert any(event.get("reason") == "unterminated_line" for event in trailing), trailing
        try:
            closing.line()
            raise AssertionError("server did not close after EOF")
        except EOFError:
            pass
        # Abrupt reset/close of one peer does not disturb the engine or other clients.
        abrupt = Peer(port)
        abrupt.sock.sendall(b"BOOK\n")
        abrupt.close()
        assert order.command("BOOK")[-1]["type"] == "RESPONSE_END"
        # Exercise delivered client with multiple command response groups.
        completed = subprocess.run([client, "--port", str(port)], input="BOOK\nSTATS\n",
                                   capture_output=True, text=True, timeout=15)
        assert completed.returncode == 0, completed.stderr
        decoded = [json.loads(line) for line in completed.stdout.splitlines()]
        assert sum(event.get("type") == "RESPONSE_END" for event in decoded) == 2
        # Stop while an idle subscribed client remains; response queues drain cleanly.
        stopped = order.command("SHUTDOWN")
        assert any(event.get("type") == "SHUTDOWN" for event in stopped)
        finish_server(process, port, already_stopping=True)
    finally:
        for peer in peers:
            peer.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def bounds_tests(server):
    process, port = start_server(server, "--max-line", "64", "--max-output", "8192")
    try:
        peer = Peer(port)
        # 64 bytes is accepted as a bounded line (and rejected by command parser).
        normal = peer.command("X" * 64)
        assert any(event_kind(event) == "REJECTED" for event in normal)
        overlong = peer.command("X" * 65)
        assert any(event.get("reason") == "line_too_long" for event in overlong), overlong
        try:
            peer.line()
            raise AssertionError("oversized line connection stayed open")
        except EOFError:
            pass
        peer.close()
        healthy = Peer(port)
        assert healthy.command("BOOK")[-1]["type"] == "RESPONSE_END"
        healthy.close()
        finish_server(process, port)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

    # One large-integer ADD response exceeds 256 bytes before any socket
    # write. This deterministically exercises hard backpressure independent of
    # how the OS fragments/coalesces TCP packets.
    process, port = start_server(server, "--max-output", "256")
    try:
        slow = Peer(port)
        slow.sock.sendall(b"ADD 18446744073709551615 BUY 9223372036854775807 9223372036854775807\n")
        received = b""
        while True:
            try:
                data = slow.sock.recv(65536)
            except ConnectionResetError:
                break
            if not data:
                break
            received += data
        assert len(received) <= 256, "slow peer output exceeded application bound"
        slow.close()
        healthy = Peer(port)
        assert healthy.command("BOOK")[-1]["type"] == "RESPONSE_END"
        healthy.close()
        finish_server(process, port)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def signal_test(server):
    process, port = start_server(server)
    peer = Peer(port)
    try:
        process.terminate()
        code = process.wait(timeout=8)
        # Windows TerminateProcess does not deliver SIGTERM; POSIX does.
        if __import__("os").name != "nt":
            assert code == 0, process.stderr.read()
    finally:
        peer.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def large_response_test(server, client):
    """Force queued send offsets and client framing across many receive calls."""
    process, port = start_server(server)
    seed = None
    try:
        seed = Peer(port)
        count = 600
        commands = "".join(f"ADD {index} SELL 100 1\n" for index in range(1, count + 1))
        seed.sock.sendall(commands.encode())
        for index in range(1, count + 1):
            response = seed.response()
            assert response[-1]["command_sequence"] == index
            accepted = [event for event in response if event_kind(event) == "ACCEPTED"]
            assert len(accepted) == 1 and accepted[0]["order_id"] == index

        completed = subprocess.run([client, "--port", str(port)],
                                   input=f"ADD 1000000 BUY 100 {count}\nBOOK\n",
                                   capture_output=True, text=True, timeout=30)
        assert completed.returncode == 0, completed.stderr
        # The server caps each send at 65536 bytes, while the delivered client
        # reads at most 4096 bytes. This response necessarily spans both loops,
        # even when TCP accepts every individual send without a short count.
        assert len(completed.stdout.encode()) > 65536, "sweep did not exercise send offsets"
        events = [json.loads(line) for line in completed.stdout.splitlines()]
        executions = [event for event in events if event_kind(event) == "EXECUTION"]
        assert len(executions) == count
        assert all(event["quantity"] == 1 and event["price"] == 100 for event in executions)
        assert [event["sell_id"] for event in executions] == list(range(1, count + 1))
        assert sum(event["quantity"] for event in executions) == count
        markers = [event["command_sequence"] for event in events if event.get("type") == "RESPONSE_END"]
        assert markers == [1, 2]
        books = [event for event in events if event.get("type") == "book"]
        assert len(books) == 1 and books[0]["bids"] == [] and books[0]["asks"] == []
        finish_server(process, port)
    finally:
        if seed is not None:
            seed.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    regular_tests(args.server, args.client)
    bounds_tests(args.server)
    large_response_test(args.server, args.client)
    signal_test(args.server)
    print("network integration checks passed")
