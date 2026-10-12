#!/usr/bin/env python3
"""Benchmark one or more chat server builds with load_gen and compare them.

For every server and every client count it:
  1. writes a users file with u0:p0 ... u<N-1>:p<N-1>
  2. starts the server, runs load_gen against it
  3. when load_gen prints READY (all clients logged in, server idle):
       samples the server's memory (VmRSS) and thread count from /proc
  4. between MEASURE_START and MEASURE_END:
       measures the server's CPU time -> "cores used" during the ping-pong
  5. collects load_gen's RESULT line (throughput + latency percentiles)
Then prints a Markdown table (paste it into the README).

A server is given as LABEL=COMMAND, where COMMAND is the binary plus any extra
arguments placed after "<port> <users_file>" (e.g. the event-loop count).

Example — compare the old thread-per-client build with the multi-reactor:
    git worktree add ../chat_server_threaded threaded-baseline
    cmake -S ../chat_server_threaded -B ../chat_server_threaded/build
    cmake --build ../chat_server_threaded/build
    cmake --build build
    python3 tools/bench.py \\
        --server threads=../chat_server_threaded/build/chat_server \\
        --server "loops8=build/chat_server 8" \\
        --clients 1000,5000,10000

Notes:
  - Client and server share this machine's CPUs, so absolute numbers depend on
    the machine. Compare builds run on the same machine, same settings.
  - load_gen uses --threads worker threads. The "load_gen CPU" column shows how
    many cores it used: if that is close to --threads, load_gen itself is the
    bottleneck and the server could go faster — rerun with more threads.
"""
import argparse
import json
import shlex
import os
import socket
import subprocess
import sys
import tempfile
import time

CLK_TCK = os.sysconf("SC_CLK_TCK")


def proc_status(pid):
    """VmRSS in MB and thread count of a running process, from /proc/<pid>/status."""
    rss_mb, threads = None, None
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss_mb = int(line.split()[1]) / 1024  # value is in kB
            elif line.startswith("Threads:"):
                threads = int(line.split()[1])
    return rss_mb, threads


def proc_cpu_seconds(pid):
    """Total user+system CPU time the process has used so far, in seconds."""
    with open(f"/proc/{pid}/stat") as f:
        # The command name (field 2) can contain spaces; split after its ')'.
        fields = f.read().rsplit(")", 1)[1].split()
    utime, stime = int(fields[11]), int(fields[12])  # fields 14 and 15 in `man proc`
    return (utime + stime) / CLK_TCK


def wait_for_port(port, server, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if server.poll() is not None:
            raise RuntimeError(f"server exited at startup (code {server.returncode})")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start listening")


def run_one(label, server_cmd, clients, args, tmpdir):
    users = os.path.join(tmpdir, f"users_{clients}.txt")
    if not os.path.exists(users):
        with open(users, "w") as f:
            f.writelines(f"u{i}:p{i}\n" for i in range(clients))

    row = {"server": label, "clients": clients}
    # Server logs every connect/disconnect; we don't need that output.
    # server_cmd = [binary, extra args...]; extra args go after port and users file.
    server = subprocess.Popen([server_cmd[0], str(args.port), users] + server_cmd[1:],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_port(args.port, server)
        # wait_for_port's probe connection makes the server allocate a session;
        # give it a moment to notice the disconnect before we start.
        time.sleep(0.3)

        cmd = [args.load_gen, "--port", str(args.port), "--clients", str(clients),
               "--threads", str(args.threads), "--duration", str(args.duration),
               "--warmup", str(args.warmup), "--hold", "1"]
        gen = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
        cpu_start = None
        for line in gen.stdout:  # load_gen flushes after each marker line
            line = line.strip()
            if line.startswith("READY"):
                row["rss_mb"], row["threads"] = proc_status(server.pid)
            elif line == "MEASURE_START":
                cpu_start, t_start = proc_cpu_seconds(server.pid), time.time()
                gen_cpu_start = proc_cpu_seconds(gen.pid)
            elif line == "MEASURE_END" and cpu_start is not None:
                elapsed = time.time() - t_start
                row["server_cores"] = (proc_cpu_seconds(server.pid) - cpu_start) / elapsed
                row["load_gen_cores"] = (proc_cpu_seconds(gen.pid) - gen_cpu_start) / elapsed
            elif line.startswith("RESULT "):
                row.update(json.loads(line[len("RESULT "):]))
        gen.wait(timeout=60)
    finally:
        server.kill()
        server.wait()
    return row


def fmt(v, spec):
    return format(v, spec) if isinstance(v, (int, float)) else "-"


def print_table(rows):
    print()
    print("| server | clients | login (s) | RSS (MB) | threads | msgs/s | p50 (µs) | p99 (µs) | p99.9 (µs) | server CPU (cores) | load_gen CPU (cores) |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        if "error" in r:
            print(f"| {r['server']} | {r['clients']} | error: {r['error']} ||||||||")
            continue
        print(f"| {r['server']} | {r['clients']} | {fmt(r.get('login_s'), '.2f')} | "
              f"{fmt(r.get('rss_mb'), '.1f')} | {fmt(r.get('threads'), 'd')} | "
              f"{fmt(r.get('msgs_per_s'), ',.0f')} | {fmt(r.get('p50_us'), 'd')} | "
              f"{fmt(r.get('p99_us'), 'd')} | {fmt(r.get('p999_us'), 'd')} | "
              f"{fmt(r.get('server_cores'), '.2f')} | {fmt(r.get('load_gen_cores'), '.2f')} |")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--server", action="append", required=True, metavar="LABEL=COMMAND",
                   help='server to benchmark, e.g. "loops8=build/chat_server 8" (repeatable)')
    p.add_argument("--clients", default="1000,5000,10000",
                   help="comma-separated client counts (even numbers)")
    p.add_argument("--load-gen", default="build/load_gen")
    p.add_argument("--threads", type=int, default=4, help="load_gen worker threads")
    p.add_argument("--duration", type=float, default=10, help="measured ping-pong seconds")
    p.add_argument("--warmup", type=float, default=2)
    p.add_argument("--port", type=int, default=5600)
    p.add_argument("--json-out", help="also save raw results as JSON")
    args = p.parse_args()

    servers = []
    for s in args.server:
        label, _, command = s.partition("=")
        cmd = shlex.split(command)
        if not cmd or not os.path.exists(cmd[0]):
            sys.exit(f"bad --server {s!r}: expected LABEL=COMMAND with an existing binary")
        servers.append((label, cmd))

    rows = []
    with tempfile.TemporaryDirectory() as tmpdir:
        for clients in (int(c) for c in args.clients.split(",")):
            for label, cmd in servers:
                print(f"running {label} with {clients} clients...", file=sys.stderr, flush=True)
                row = run_one(label, cmd, clients, args, tmpdir)
                rows.append(row)
                print(f"  {row}", file=sys.stderr, flush=True)
                time.sleep(1)  # let the kernel clean up sockets between runs

    print_table(rows)
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(rows, f, indent=2)


if __name__ == "__main__":
    main()
