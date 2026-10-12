#!/usr/bin/env python3
"""End-to-end checks for the chat server. Starts the server itself.

Usage:  python3 tests/smoke_test.py [path/to/chat_server] [event_loops]
        (defaults: build/chat_server, and the server's own default loop count)

Checks run in order and stop at the first failure, so you can implement the
server step by step and see how far you get.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

PORT = 5599
TIMEOUT = 2.0
N_CLIENTS = 200


class Client:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=TIMEOUT)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data.encode())

    def expect(self, text):
        """Read until `text` arrives; consume everything up to and including it."""
        want = text.encode()
        deadline = time.time() + TIMEOUT
        while want not in self.buf:
            if time.time() > deadline:
                raise AssertionError(f"expected {text!r}, got {self.buf!r}")
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise AssertionError(f"connection closed while waiting for {text!r}; got {self.buf!r}")
            self.buf += chunk
        self.buf = self.buf[self.buf.index(want) + len(want):]

    def expect_closed(self):
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            try:
                if self.sock.recv(4096) == b"":
                    return
            except socket.timeout:
                pass
            except ConnectionResetError:
                return
        raise AssertionError("server did not close the connection")

    def close(self):
        self.sock.close()


def login(user, password):
    c = Client()
    c.expect("Enter username: ")
    c.send(user + "\n")
    c.expect("Enter password: ")
    c.send(password + "\n")
    c.expect("Welcome to the chat server !\n")
    return c


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/chat_server"
    loops = sys.argv[2:3]  # optional 3rd server argument: number of event loops
    tmp = tempfile.mkdtemp()
    users = os.path.join(tmp, "users.txt")
    with open(users, "w") as f:
        f.write("alice:pw1\nbob:pw2\n")
        f.writelines(f"u{i}:p{i}\n" for i in range(N_CLIENTS))

    server = subprocess.Popen([binary, str(PORT), users] + loops,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(50):  # wait for it to listen
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.1).close()
            break
        except OSError:
            if server.poll() is not None:
                print("FAIL: server exited at startup:\n" + server.stdout.read().decode())
                return 1
            time.sleep(0.1)

    st = {}

    def t_login():
        st["alice"] = login("alice", "pw1")
        st["bob"] = login("bob", "pw2")

    def t_online():
        st["alice"].send("/online\n")
        st["alice"].expect("Online (2): alice bob\n")

    def t_msg():
        st["alice"].send("/msg bob hi there\n")
        st["bob"].expect("[alice]: hi there\n")

    def t_two_lines_one_packet():
        st["alice"].send("/create_group g\n")
        st["alice"].expect("Group g Created.\n")
        st["bob"].send("/join_group g\n/group_msg g hello group\n")  # ONE send, two lines
        st["bob"].expect("You joined the group g.\n")
        st["alice"].expect("[Group g]: hello group\n")

    def t_line_split_across_packets():
        st["alice"].send("/msg bo")
        time.sleep(0.2)
        st["alice"].send("b split\n")
        st["bob"].expect("[alice]: split\n")

    def t_crlf():
        st["alice"].send("/msg bob crlf\r\n")
        st["bob"].expect("[alice]: crlf\n")

    def t_many_short_lines_one_burst():
        # ~6 KB in one send, but every line is short: must NOT trip the line limit.
        st["alice"].send("".join(f"/msg bob burst{i:03d} {'x' * 40}\n" for i in range(120)))
        st["bob"].expect("[alice]: burst119 ")

    def t_overlong_line_disconnects():
        c = Client()
        c.expect("Enter username: ")
        c.send("y" * 10000)  # no '\n' ever
        c.expect_closed()

    def t_bad_password():
        c = Client()
        c.expect("Enter username: ")
        c.send("alice\n")
        c.expect("Enter password: ")
        c.send("wrong\n")
        c.expect("Authentication failed.")
        c.expect_closed()

    def t_duplicate_login_keeps_original():
        c = Client()
        c.expect("Enter username: ")
        c.send("alice\n")
        c.expect("Enter password: ")
        c.send("pw1\n")
        c.expect("already logged in")
        c.expect_closed()
        st["bob"].send("/msg alice still here?\n")  # the real alice must survive
        st["alice"].expect("[bob]: still here?\n")

    def t_exit():
        st["bob"].send("/exit\n")
        st["bob"].expect_closed()
        st["alice"].send("/msg bob gone\n")
        st["alice"].expect("Error: Check receiver name")

    def t_many_clients():
        clients = [login(f"u{i}", f"p{i}") for i in range(N_CLIENTS)]
        st["alice"].send("/broadcast ping all\n")
        for c in clients:
            c.expect("[Broadcast from alice]: ping all\n")
        for c in clients:
            c.close()

    tests = [
        ("login two users", t_login, "No prompt? Check ChatServer::run -> EventLoop::post/drainMailbox -> adoptConnection. No welcome? Check login in EventLoop::handleLine."),
        ("/online lists both users", t_online, "ClientRegistry::onlineUsers + CommandProcessor::online."),
        ("private message", t_msg, "ChatServer::deliver (registry lookup + post) and EventLoop::deliverLocal (byName_)."),
        ("two lines in one packet", t_two_lines_one_packet, "nextLine() must leave the 2nd line in inBuf_; onReadable must loop."),
        ("line split across packets", t_line_split_across_packets, "nextLine() must return nullopt until the '\\n' arrives."),
        ("\\r\\n line endings", t_crlf, "Strip the trailing '\\r' from the line."),
        ("many short lines in one burst", t_many_short_lines_one_burst, "The line limit must only apply to a line with no '\\n' yet."),
        ("overlong line disconnects", t_overlong_line_disconnects, "readAvailable: inBuf_ > kMaxLineLength with no '\\n' -> return false."),
        ("wrong password rejected + closed", t_bad_password, "markClosed + final flush + closeSession in onReadable."),
        ("duplicate login doesn't kick original", t_duplicate_login_keeps_original, "clients_.add must fail for a name online on ANY loop; closeSession only cleans up when Chatting."),
        ("/exit closes connection", t_exit, "After /exit, bob must be gone from ClientRegistry and from byName_ (closeSession)."),
        (f"{N_CLIENTS} concurrent clients + broadcast", t_many_clients, "ChatServer::broadcast posts once per loop; EventLoop::broadcastLocal sends to that loop's users."),
    ]

    failed = False
    try:
        for name, fn, hint in tests:
            try:
                fn()
                print(f"PASS  {name}")
            except Exception as e:
                print(f"FAIL  {name}\n      {e}\n      hint: {hint}")
                failed = True
                break
    finally:
        server.kill()
        out = server.communicate()[0].decode()
        if failed and out.strip():
            print("---- server output ----\n" + out[-8000:])
    if not failed:
        print("All checks passed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
