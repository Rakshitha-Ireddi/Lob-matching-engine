// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Thin sockets layer. The Linux path uses epoll + non-blocking BSD sockets and
// is the primary target; the Windows path uses WSAPoll and exists so the whole
// stack builds and runs on a dev box. See docs/adr/0009-portable-net-layer.md.
namespace lob::net {

#if defined(_WIN32)
using Fd = std::uintptr_t;
inline constexpr Fd kInvalidFd = ~static_cast<Fd>(0);
#else
using Fd = int;
inline constexpr Fd kInvalidFd = -1;
#endif

void startup();   // WSAStartup on Windows, no-op elsewhere
void shutdown();

std::string last_error();

// Move-only RAII handle.
class Socket {
public:
    Socket() = default;
    explicit Socket(Fd fd) : fd_(fd) {}
    ~Socket() { close(); }
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalidFd; }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            o.fd_ = kInvalidFd;
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] Fd get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ != kInvalidFd; }
    Fd release() noexcept {
        Fd f = fd_;
        fd_ = kInvalidFd;
        return f;
    }
    void close() noexcept;

private:
    Fd fd_ = kInvalidFd;
};

void set_nonblocking(Fd fd);
void set_tcp_nodelay(Fd fd);
void set_reuseaddr(Fd fd);

// --- TCP order entry -------------------------------------------------

Socket tcp_listen(const std::string& host, std::uint16_t port, int backlog = 128);
// Returns an invalid Socket when there is nothing to accept (would block).
Socket tcp_accept(Fd listener);
Socket tcp_connect(const std::string& host, std::uint16_t port);

// >=0 bytes moved; -1 on real error; -2 on would-block.
long tcp_send(Fd fd, const void* data, std::size_t len);
long tcp_recv(Fd fd, void* data, std::size_t len);

// --- UDP market data ----------------------------------------------

Socket udp_publisher(const std::string& group, std::uint16_t port, int ttl = 1);
Socket udp_subscriber(const std::string& group, std::uint16_t port);
long udp_send(Fd fd, const std::string& group, std::uint16_t port, const void* data, std::size_t len);
long udp_recv(Fd fd, void* data, std::size_t len);

// --- readiness polling --------------------------------------------

class Poller {
public:
    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void add_read(Fd fd);
    void remove(Fd fd);

    struct Ready {
        Fd fd;
        bool readable;
        bool error;
    };
    // Blocks up to timeout_ms (-1 = forever). Returns fds with activity.
    std::vector<Ready> wait(int timeout_ms);

private:
#if defined(__linux__)
    int epfd_ = -1;
#endif
    std::vector<Fd> fds_;
};

}  // namespace lob::net
