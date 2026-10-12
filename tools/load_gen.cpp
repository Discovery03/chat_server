// ---------------------------------------------------------------------------
// load_gen: load generator / benchmark client for the chat server.
//
// WHAT IT DOES
//   1. LOGIN    Opens N TCP connections and logs each one in as u<i> / p<i>.
//               Reports how long it took until all N were logged in.
//   2. SETTLE   Waits until the server goes quiet. Every login broadcasts
//               "<name> has joined the chat." to everyone already online, so
//               N logins cause ~N^2/2 messages; we let those drain first.
//               Then prints "READY" — a good moment to measure server memory.
//   3. PING-PONG Clients are paired (u0<->u1, u2<->u3, ...). In each pair one
//               side sends "/msg <partner> <seq> <send_time_ns>"; the partner
//               receives it, records the latency, and immediately replies the
//               same way. So every pair always has exactly ONE message in
//               flight ("closed loop"): throughput = pairs / round-trip time.
//               A warm-up period runs first, then we measure for --duration s.
//   4. REPORT   Prints one "RESULT {json}" line: login time, messages/sec and
//               latency percentiles (p50 / p99 / p99.9 / max).
//
// WHY C++ AND EPOLL?
//   The load generator must be much faster than the server, or we'd be
//   benchmarking the tool instead of the server. It uses the same technique
//   as the server (non-blocking sockets + epoll) and splits the connections
//   across --threads worker threads, each with its own epoll instance.
//
// WHY ONE MESSAGE IN FLIGHT PER CONNECTION?
//   The old thread-per-client server treats each recv() as one command, so
//   two commands arriving together would be misread. Sending one at a time
//   keeps the benchmark fair to both versions.
//
// LATENCY = time from "load_gen sent the /msg" to "load_gen received the
//   forwarded message", measured with one clock in one process. It includes
//   the loopback network, the server, and a little load_gen overhead.
//
// The stdout lines READY / MEASURE_START / MEASURE_END / RESULT are read by
// tools/bench.py, which samples the server's memory and CPU at those moments.
//
// Usage: load_gen [--host 127.0.0.1] [--port 5599] [--clients 1000]
//                 [--threads 4] [--hold 2] [--warmup 2] [--duration 10]
//        --duration 0 skips the ping-pong phase (login/memory test only).
// ---------------------------------------------------------------------------

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

void sleepSeconds(double s) {
    std::this_thread::sleep_for(std::chrono::duration<double>(s));
}

struct Options {
    std::string host = "127.0.0.1";
    int port = 5599;
    int clients = 1000;      // must be even (clients are paired)
    int threads = 4;         // worker threads, each with its own epoll
    int maxInflight = 256;   // per thread: connections logging in at the same time
    double holdS = 2;        // pause after READY (bench.py samples memory here)
    double warmupS = 2;      // ping-pong before measuring, so caches etc. warm up
    double durationS = 10;   // measured ping-pong time; 0 = skip ping-pong
    double loginTimeoutS = 300;
};

// ---- State shared between the main thread and the workers -------------------
// The main thread moves the whole run from stage to stage on a timer; workers
// check the current stage on every loop iteration.
enum Stage : int { kLogin, kHold, kWarmup, kMeasure, kStop };
std::atomic<int> gStage{kLogin};
std::atomic<int> gLoggedIn{0};          // connections that finished login
std::atomic<int> gErrors{0};            // anything unexpected (see fail())
std::atomic<std::uint64_t> gBytesIn{0}; // total bytes received (for "settled?")

// Per-connection login progress. The server's prompts have no '\n', so during
// login we search for the exact prompt text instead of reading lines.
enum class Phase { Connecting, WaitUserPrompt, WaitPassPrompt, WaitWelcome, Ready, Dead };

struct Conn {
    int id = 0;          // global client number: logs in as u<id>
    int fd = -1;
    Phase phase = Phase::Connecting;
    std::string in;      // received bytes not processed yet
    std::string out;     // bytes queued but not yet accepted by the kernel
    bool wantWrite = false;
};

// ---------------------------------------------------------------------------
// Worker: one thread driving a subset of the connections with its own epoll.
// Pairs are never split across workers: local index i is always partnered
// with local index i ^ 1 (0<->1, 2<->3, ...), so no cross-thread messaging.
// ---------------------------------------------------------------------------
class Worker {
    public:
        Worker(const Options& opts, sockaddr_in addr, std::vector<int> ids)
            : opts_(opts), addr_(addr) {
            conns_.resize(ids.size());
            for (std::size_t i = 0; i < ids.size(); ++i) conns_[i].id = ids[i];
        }

        void run() {
            ep_ = epoll_create1(EPOLL_CLOEXEC);
            if (ep_ < 0) {
                std::perror("load_gen: epoll_create1");
                gErrors++;
                return;
            }

            std::vector<epoll_event> events(256);
            bool pingStarted = false;

            while (gStage.load(std::memory_order_relaxed) != kStop) {
                startConnects();  // keep up to maxInflight logins in progress

                // Short timeout so we notice stage changes from the main thread.
                int n = epoll_wait(ep_, events.data(), static_cast<int>(events.size()), 20);
                if (n < 0 && errno != EINTR) {
                    std::perror("load_gen: epoll_wait");
                    gErrors++;
                    break;
                }
                for (int i = 0; i < n; ++i) onEvent(events[i].data.u32, events[i].events);

                // The main thread only leaves kLogin when every client is logged
                // in, so all our connections are Ready here. Start one ping per pair.
                if (!pingStarted && gStage.load(std::memory_order_relaxed) >= kWarmup) {
                    for (std::size_t i = 0; i < conns_.size(); i += 2) sendPing(i, 0);
                    pingStarted = true;
                }
            }

            // Abortive close (RST, via SO_LINGER 0) instead of a normal close:
            // normal closes leave every client port in TIME_WAIT for ~60 s, and
            // after a few big runs we'd run out of local ports.
            for (Conn& c : conns_) {
                if (c.fd < 0) continue;
                linger lg{1, 0};
                setsockopt(c.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
                ::close(c.fd);
                c.fd = -1;
            }
            ::close(ep_);
        }

        // Results, read by main() after the thread has been joined.
        std::vector<std::uint32_t> latenciesUs;
        std::uint64_t measuredMsgs = 0;

    private:
        // ---- Connecting -----------------------------------------------------

        // Starting all N connects at once would overflow the server's listen
        // backlog (dropped SYNs -> 1 s retransmit delays), so we limit how many
        // connections are in the connect/login phase at the same time.
        void startConnects() {
            while (inflight_ < opts_.maxInflight && nextToConnect_ < conns_.size()) {
                startConnect(static_cast<std::uint32_t>(nextToConnect_++));
            }
        }

        void startConnect(std::uint32_t idx) {
            Conn& c = conns_[idx];
            inflight_++;

            c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (c.fd < 0) return fail(idx, "socket");

            // Disable Nagle's algorithm: we send small messages and care about
            // latency, so don't let the kernel hold them back to batch them.
            int one = 1;
            setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            // Non-blocking connect: usually returns -1/EINPROGRESS straight away.
            // epoll reports the socket as writable once the connect finished.
            int rc = ::connect(c.fd, reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
            if (rc < 0 && errno != EINPROGRESS) return fail(idx, "connect");

            epoll_event ev{};
            ev.events = EPOLLOUT;  // "writable" = connect completed (or failed)
            ev.data.u32 = idx;     // which connection this is, when it fires
            if (epoll_ctl(ep_, EPOLL_CTL_ADD, c.fd, &ev) < 0) return fail(idx, "epoll_ctl add");
        }

        // ---- Event dispatch -------------------------------------------------

        void onEvent(std::uint32_t idx, std::uint32_t ev) {
            Conn& c = conns_[idx];
            if (c.phase == Phase::Dead) return;

            if (c.phase == Phase::Connecting) {
                // Did the connect succeed? SO_ERROR holds the result.
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0 || (ev & (EPOLLERR | EPOLLHUP))) {
                    errno = err;
                    return fail(idx, "connect (async)");
                }
                c.phase = Phase::WaitUserPrompt;
                setInterest(idx, EPOLLIN);  // from now on we wait for data
                return;
            }

            if (ev & EPOLLIN) {
                if (!readAll(c)) return fail(idx, "server closed connection / recv error");
                handleInput(idx);
                if (c.phase == Phase::Dead) return;
            }
            if (ev & EPOLLOUT) flush(idx);
            if ((ev & (EPOLLERR | EPOLLHUP)) && c.phase != Phase::Dead) fail(idx, "socket error/hangup");
        }

        // Reads until EAGAIN (same pattern as the server). false = closed/error.
        bool readAll(Conn& c) {
            char buf[65536];
            while (true) {
                ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    c.in.append(buf, static_cast<std::size_t>(n));
                    gBytesIn.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
                    continue;
                }
                if (n == 0) return false;
                if (errno == EINTR) continue;
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
        }

        // ---- Protocol -------------------------------------------------------

        // Finds `text` in c.in. If present, drops everything up to and including
        // it (e.g. a join notice that arrived before the welcome) and returns true.
        static bool consume(Conn& c, const char* text) {
            std::size_t pos = c.in.find(text);
            if (pos == std::string::npos) return false;
            c.in.erase(0, pos + std::strlen(text));
            return true;
        }

        void handleInput(std::uint32_t idx) {
            Conn& c = conns_[idx];
            // Login: one step per prompt. A single recv may contain several
            // steps' worth of data, hence the loop.
            while (c.phase != Phase::Ready) {
                switch (c.phase) {
                    case Phase::WaitUserPrompt:
                        if (!consume(c, "Enter username: ")) return;
                        queue(idx, "u" + std::to_string(c.id) + "\n");
                        c.phase = Phase::WaitPassPrompt;
                        break;
                    case Phase::WaitPassPrompt:
                        if (!consume(c, "Enter password: ")) return;
                        queue(idx, "p" + std::to_string(c.id) + "\n");
                        c.phase = Phase::WaitWelcome;
                        break;
                    case Phase::WaitWelcome:
                        if (c.in.find("Authentication failed") != std::string::npos)
                            return fail(idx, "login rejected (users file missing u<i>:p<i>?)");
                        if (!consume(c, "Welcome to the chat server !\n")) return;
                        c.phase = Phase::Ready;
                        inflight_--;  // frees a slot for the next connect
                        gLoggedIn.fetch_add(1, std::memory_order_relaxed);
                        break;
                    default:
                        return;  // Dead / Connecting: nothing to do
                }
                if (c.phase == Phase::Dead) return;
            }

            // Logged in: everything from here on is '\n'-terminated lines.
            // Walk them with a cursor and erase once at the end (erasing after
            // every line would shift the buffer again and again).
            std::size_t start = 0;
            std::size_t nl;
            while ((nl = c.in.find('\n', start)) != std::string::npos) {
                handleLine(idx, c.in.data() + start, nl - start);
                if (c.phase == Phase::Dead) return;
                start = nl + 1;
            }
            c.in.erase(0, start);
        }

        // Lines we care about look like "[u12]: <seq> <send_time_ns>" — a ping
        // from our partner. Join notices and anything else are ignored.
        void handleLine(std::uint32_t idx, const char* line, std::size_t len) {
            if (len >= 5 && std::strncmp(line, "Error", 5) == 0) {
                // e.g. partner not online: the pair would stall, so report it.
                return fail(idx, "server replied with an error to /msg");
            }
            if (len == 0 || line[0] != '[') return;

            std::string s(line, len);  // strtoull needs a terminated string
            std::size_t colon = s.find("]: ");
            if (colon == std::string::npos) return;
            char* end = nullptr;
            const char* p = s.c_str() + colon + 3;
            std::uint64_t seq = std::strtoull(p, &end, 10);
            if (end == p) return;
            const char* p2 = end;
            std::uint64_t sentNs = std::strtoull(p2, &end, 10);
            if (end == p2) return;

            int stage = gStage.load(std::memory_order_relaxed);
            if (stage == kMeasure) {
                std::uint64_t us = (nowNs() - sentNs) / 1000;
                latenciesUs.push_back(static_cast<std::uint32_t>(std::min<std::uint64_t>(us, UINT32_MAX)));
                measuredMsgs++;
            }
            // Bounce it straight back: keeps exactly one message in flight per pair.
            if (stage == kWarmup || stage == kMeasure) sendPing(idx, seq + 1);
        }

        void sendPing(std::size_t fromIdx, std::uint64_t seq) {
            const Conn& partner = conns_[fromIdx ^ 1];
            queue(static_cast<std::uint32_t>(fromIdx),
                  "/msg u" + std::to_string(partner.id) + " " + std::to_string(seq) + " " +
                      std::to_string(nowNs()) + "\n");
        }

        // ---- Writing (same backpressure scheme as the server) ---------------

        void queue(std::uint32_t idx, const std::string& data) {
            conns_[idx].out += data;
            flush(idx);
        }

        void flush(std::uint32_t idx) {
            Conn& c = conns_[idx];
            while (!c.out.empty()) {
                ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    return fail(idx, "send");
                }
                c.out.erase(0, static_cast<std::size_t>(n));
            }
            // Only ask for EPOLLOUT while there is something left to send.
            if (!c.out.empty() && !c.wantWrite) {
                setInterest(idx, EPOLLIN | EPOLLOUT);
                c.wantWrite = true;
            } else if (c.out.empty() && c.wantWrite) {
                setInterest(idx, EPOLLIN);
                c.wantWrite = false;
            }
        }

        void setInterest(std::uint32_t idx, std::uint32_t events) {
            epoll_event ev{};
            ev.events = events;
            ev.data.u32 = idx;
            epoll_ctl(ep_, EPOLL_CTL_MOD, conns_[idx].fd, &ev);
        }

        // Any failure is counted; the main thread aborts the run when it sees one,
        // because a dead client invalidates the numbers (its pair stops pinging).
        void fail(std::uint32_t idx, const char* why) {
            Conn& c = conns_[idx];
            if (c.phase == Phase::Dead) return;
            // Errors after the run ended (server shutting down etc.) don't count.
            if (gStage.load(std::memory_order_relaxed) != kStop) {
                if (gErrors.fetch_add(1) < 5) {
                    std::fprintf(stderr, "load_gen: client u%d: %s (%s)\n", c.id, why, std::strerror(errno));
                }
            }
            if (c.phase != Phase::Ready) inflight_--;
            if (c.fd >= 0) {
                ::close(c.fd);  // also removes it from epoll
                c.fd = -1;
            }
            c.phase = Phase::Dead;
        }

        const Options& opts_;
        sockaddr_in addr_;
        int ep_ = -1;
        std::vector<Conn> conns_;
        std::size_t nextToConnect_ = 0;
        int inflight_ = 0;
};

// ---- Command line -----------------------------------------------------------

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage: %s [--host H] [--port P] [--clients N] [--threads T] [--hold S]\n"
                 "          [--warmup S] [--duration S] [--max-inflight K]\n",
                 prog);
    std::exit(2);
}

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (i + 1 >= argc) usage(argv[0]);
        const char* val = argv[++i];
        if (key == "--host") o.host = val;
        else if (key == "--port") o.port = std::atoi(val);
        else if (key == "--clients") o.clients = std::atoi(val);
        else if (key == "--threads") o.threads = std::atoi(val);
        else if (key == "--hold") o.holdS = std::atof(val);
        else if (key == "--warmup") o.warmupS = std::atof(val);
        else if (key == "--duration") o.durationS = std::atof(val);
        else if (key == "--max-inflight") o.maxInflight = std::atoi(val);
        else usage(argv[0]);
    }
    if (o.clients < 2 || o.clients % 2 != 0) {
        std::fprintf(stderr, "load_gen: --clients must be an even number >= 2\n");
        std::exit(2);
    }
    o.threads = std::max(1, std::min(o.threads, o.clients / 2));
    return o;
}

// Value at percentile p (0..100) of an already sorted vector.
std::uint32_t percentile(const std::vector<std::uint32_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::size_t i = static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size() - 1));
    return sorted[i];
}

}  // namespace

int main(int argc, char** argv) {
    const Options opts = parseArgs(argc, argv);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(opts.port));
    if (inet_pton(AF_INET, opts.host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "load_gen: bad --host %s (IPv4 address expected)\n", opts.host.c_str());
        return 2;
    }

    // Deal pairs out round-robin: pair p = clients (2p, 2p+1) goes to worker p % T.
    std::vector<std::vector<int>> ids(static_cast<std::size_t>(opts.threads));
    for (int p = 0; p < opts.clients / 2; ++p) {
        auto& v = ids[static_cast<std::size_t>(p % opts.threads)];
        v.push_back(2 * p);
        v.push_back(2 * p + 1);
    }
    std::vector<Worker> workers;
    workers.reserve(ids.size());
    for (auto& v : ids) workers.emplace_back(opts, addr, std::move(v));

    std::vector<std::thread> threads;
    for (Worker& w : workers) threads.emplace_back([&w] { w.run(); });

    // Stops the workers and exits with an error result.
    auto abortRun = [&](const char* why) {
        gStage = kStop;
        for (auto& t : threads) t.join();
        std::printf("RESULT {\"error\": \"%s\", \"logged_in\": %d, \"clients\": %d}\n", why,
                    gLoggedIn.load(), opts.clients);
        return 1;
    };

    // ---- Phase 1: login ----------------------------------------------------
    const auto loginStart = Clock::now();
    while (gLoggedIn.load() < opts.clients) {
        if (gErrors.load() > 0) return abortRun("client error during login");
        if (secondsSince(loginStart) > opts.loginTimeoutS) return abortRun("login timeout");
        sleepSeconds(0.01);
    }
    const double loginS = secondsSince(loginStart);

    // ---- Phase 2: settle (wait for the join-notice storm to finish) --------
    // "Settled" = no new bytes received during a 0.5 s window.
    std::uint64_t lastBytes = gBytesIn.load();
    while (true) {
        sleepSeconds(0.5);
        std::uint64_t b = gBytesIn.load();
        if (b == lastBytes) break;
        lastBytes = b;
        if (gErrors.load() > 0) return abortRun("client error while settling");
    }
    const double settleS = secondsSince(loginStart) - loginS - 0.5;  // minus the quiet window

    gStage = kHold;
    std::printf("READY clients=%d login_s=%.3f settle_s=%.3f\n", opts.clients, loginS, settleS);
    std::fflush(stdout);  // bench.py is waiting for this line
    sleepSeconds(opts.holdS);

    // ---- Phase 3: ping-pong --------------------------------------------------
    double measuredS = 0;
    if (opts.durationS > 0) {
        gStage = kWarmup;
        sleepSeconds(opts.warmupS);

        std::printf("MEASURE_START\n");
        std::fflush(stdout);
        const auto measureStart = Clock::now();
        gStage = kMeasure;
        sleepSeconds(opts.durationS);
        gStage = kStop;  // workers stop recording and shut down
        measuredS = secondsSince(measureStart);
        std::printf("MEASURE_END\n");
        std::fflush(stdout);
    } else {
        gStage = kStop;
    }
    for (auto& t : threads) t.join();
    if (gErrors.load() > 0) {
        std::printf("RESULT {\"error\": \"client error during ping-pong\", \"clients\": %d}\n", opts.clients);
        return 1;
    }

    // ---- Phase 4: report -----------------------------------------------------
    std::vector<std::uint32_t> all;
    std::uint64_t msgs = 0;
    for (Worker& w : workers) {
        all.insert(all.end(), w.latenciesUs.begin(), w.latenciesUs.end());
        msgs += w.measuredMsgs;
    }
    std::sort(all.begin(), all.end());

    std::printf("RESULT {\"clients\": %d, \"login_s\": %.3f, \"settle_s\": %.3f, "
                "\"msgs\": %" PRIu64 ", \"msgs_per_s\": %.0f, "
                "\"p50_us\": %u, \"p99_us\": %u, \"p999_us\": %u, \"max_us\": %u}\n",
                opts.clients, loginS, settleS, msgs, measuredS > 0 ? static_cast<double>(msgs) / measuredS : 0.0,
                percentile(all, 50), percentile(all, 99), percentile(all, 99.9),
                all.empty() ? 0u : all.back());
    return 0;
}
