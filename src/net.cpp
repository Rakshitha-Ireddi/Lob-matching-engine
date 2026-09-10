// SPDX-License-Identifier: MIT
#include "lob/net.hpp"

#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socklen_t = int;
    #define LOB_LAST_ERR WSAGetLastError()
    #define LOB_EWOULDBLOCK WSAEWOULDBLOCK
    #define LOB_EAGAIN WSAEWOULDBLOCK
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #if defined(__linux__)
        #include <sys/epoll.h>
    #endif
    #include <poll.h>
    #define LOB_LAST_ERR errno
    #define LOB_EWOULDBLOCK EWOULDBLOCK
    #define LOB_EAGAIN EAGAIN
    using SOCKET = int;
    static constexpr int INVALID_SOCKET = -1;
#endif

namespace lob::net {
namespace {

sockaddr_in make_addr(const std::string& host, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0" || host == "*") {
        a.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    }
    return a;
}

bool would_block(int err) {
    return err == LOB_EWOULDBLOCK || err == LOB_EAGAIN;
}

}  // namespace

void startup() {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
}

void shutdown() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

std::string last_error() {
    return "errno=" + std::to_string(LOB_LAST_ERR);
}

void Socket::close() noexcept {
    if (fd_ == kInvalidFd) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd_));
#else
    ::close(fd_);
#endif
    fd_ = kInvalidFd;
}

void set_nonblocking(Fd fd) {
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &on);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void set_tcp_nodelay(Fd fd) {
    int on = 1;
    setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&on), sizeof(on));
}

void set_reuseaddr(Fd fd) {
    int on = 1;
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&on), sizeof(on));
}

static void set_socket_buffers(SOCKET s, int bytes) {
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

Socket tcp_listen(const std::string& host, std::uint16_t port, int backlog) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Socket{};
    Socket sock{static_cast<Fd>(s)};
    set_reuseaddr(sock.get());
    sockaddr_in a = make_addr(host, port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return Socket{};
    if (::listen(s, backlog) != 0) return Socket{};
    set_nonblocking(sock.get());
    return sock;
}

Socket tcp_accept(Fd listener) {
    SOCKET c = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (c == INVALID_SOCKET) return Socket{};
    Socket sock{static_cast<Fd>(c)};
    set_nonblocking(sock.get());
    set_tcp_nodelay(sock.get());
    return sock;
}

Socket tcp_connect(const std::string& host, std::uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Socket{};
    Socket sock{static_cast<Fd>(s)};
    sockaddr_in a = make_addr(host, port);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return Socket{};
    set_tcp_nodelay(sock.get());
    set_nonblocking(sock.get());
    return sock;
}

long tcp_send(Fd fd, const void* data, std::size_t len) {
    long sent = static_cast<long>(
        ::send(static_cast<SOCKET>(fd), static_cast<const char*>(data),
               static_cast<int>(len), 0));
    if (sent < 0) return would_block(LOB_LAST_ERR) ? -2 : -1;
    return sent;
}

long tcp_recv(Fd fd, void* data, std::size_t len) {
    long got = static_cast<long>(
        ::recv(static_cast<SOCKET>(fd), static_cast<char*>(data),
               static_cast<int>(len), 0));
    if (got < 0) return would_block(LOB_LAST_ERR) ? -2 : -1;
    return got;
}

Socket udp_publisher(const std::string& group, std::uint16_t port, int ttl) {
    (void)group;
    (void)port;
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return Socket{};
    Socket sock{static_cast<Fd>(s)};
    set_socket_buffers(s, 4 * 1024 * 1024);
    int t = ttl;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&t), sizeof(t));
    unsigned char loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loop), sizeof(loop));
    set_nonblocking(sock.get());
    return sock;
}

Socket udp_subscriber(const std::string& group, std::uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return Socket{};
    Socket sock{static_cast<Fd>(s)};
    set_reuseaddr(sock.get());
    set_socket_buffers(s, 8 * 1024 * 1024);
    sockaddr_in a = make_addr("0.0.0.0", port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return Socket{};
    ip_mreq mreq{};
    inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    set_nonblocking(sock.get());
    return sock;
}

long udp_send(Fd fd, const std::string& group, std::uint16_t port,
              const void* data, std::size_t len) {
    sockaddr_in a = make_addr(group, port);
    long sent = static_cast<long>(
        ::sendto(static_cast<SOCKET>(fd), static_cast<const char*>(data),
                 static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&a),
                 sizeof(a)));
    if (sent < 0) return would_block(LOB_LAST_ERR) ? -2 : -1;
    return sent;
}

long udp_recv(Fd fd, void* data, std::size_t len) {
    long got = static_cast<long>(
        ::recvfrom(static_cast<SOCKET>(fd), static_cast<char*>(data),
                   static_cast<int>(len), 0, nullptr, nullptr));
    if (got < 0) return would_block(LOB_LAST_ERR) ? -2 : -1;
    return got;
}

// --- Poller ---------------------------------------------------------

Poller::Poller() {
#if defined(__linux__)
    epfd_ = epoll_create1(0);
#endif
}

Poller::~Poller() {
#if defined(__linux__)
    if (epfd_ >= 0) ::close(epfd_);
#endif
}

void Poller::add_read(Fd fd) {
#if defined(__linux__)
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
#endif
    fds_.push_back(fd);
}

void Poller::remove(Fd fd) {
#if defined(__linux__)
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
    for (auto it = fds_.begin(); it != fds_.end(); ++it) {
        if (*it == fd) {
            fds_.erase(it);
            break;
        }
    }
}

std::vector<Poller::Ready> Poller::wait(int timeout_ms) {
    std::vector<Ready> out;
#if defined(__linux__)
    epoll_event evs[256];
    int n = epoll_wait(epfd_, evs, 256, timeout_ms);
    for (int i = 0; i < n; ++i) {
        out.push_back({evs[i].data.fd,
                       (evs[i].events & (EPOLLIN | EPOLLHUP)) != 0,
                       (evs[i].events & EPOLLERR) != 0});
    }
#else
    if (fds_.empty()) return out;
    std::vector<WSAPOLLFD> pfds;
    pfds.reserve(fds_.size());
    for (Fd fd : fds_) {
        WSAPOLLFD p{};
        p.fd = static_cast<SOCKET>(fd);
        p.events = POLLRDNORM;
        pfds.push_back(p);
    }
    int n = WSAPoll(pfds.data(), static_cast<ULONG>(pfds.size()), timeout_ms);
    if (n <= 0) return out;
    for (const auto& p : pfds) {
        if (p.revents == 0) continue;
        out.push_back({static_cast<Fd>(p.fd),
                       (p.revents & (POLLRDNORM | POLLHUP)) != 0,
                       (p.revents & (POLLERR | POLLNVAL)) != 0});
    }
#endif
    return out;
}

}  // namespace lob::net
