# 9. Portable sockets layer, Linux-first

- Status: accepted
- Date: 2026-09-09

## Context

The target is Linux (epoll, `SO_REUSEPORT`, multicast, eventually kernel
bypass). Development and CI also happen on Windows and macOS. Pulling in Boost.Asio
or libuv for three socket calls is disproportionate and adds an abstraction
layer over exactly the syscalls that matter for latency.

## Decision

A ~250-line `lob::net` wrapper (`src/net.cpp`) over raw BSD sockets:

- RAII `Socket`, non-blocking by default, `TCP_NODELAY` on.
- `Poller`: `epoll` on Linux, `WSAPoll` on Windows / `poll` elsewhere, behind
  one `add_read` / `wait` interface.
- UDP helpers for multicast publish (`IP_MULTICAST_TTL/LOOP`) and subscribe
  (`IP_ADD_MEMBERSHIP`, 4-8 MB socket buffers).
- One `#ifdef` block at the top maps the platform differences
  (`closesocket`/`close`, `WSAStartup`, error codes); the rest is shared.

The Linux path is the one that is tuned; the Windows path exists so the whole
stack builds and runs on a dev box (the primary build machine for this repo
has no Linux toolchain).

## Consequences

- No third-party dependency for networking.
- The hot receive/send paths are visibly plain `recv`/`send`; there is no
  framework between the code and the kernel to reason about.
- `epoll` edge-triggered mode, `SO_REUSEPORT` sharding and `io_uring` are
  future changes isolated to this file.
- Windows loopback TCP latency (~15-20 µs floor) and scheduler behaviour make
  the Windows `e2e` numbers unrepresentative of the Linux target; see
  `docs/benchmarks.md`.
