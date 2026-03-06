# kestrel

High-performance, io_uring-based static HTTP server for Linux.
See [BENCHMARKS.md](BENCHMARKS.md) if you're interested in the "high
performance" part.

## Building & Running

Make sure `liburing` is installed and build with `make`.

Run with `./kestrel`, use `-h` / `--help` to display available
options.

## OS Support

Only modern Linux (6.7+) is supported.

## Features

* Performance:
  * Per-thread socket handling via `SO_REUSEPORT` (no inter-thread
    synchronization needed).
  * CPU pinning.
  * io_uring + zerocopy `sendmsg()`.
* Security:
  * Filesystem isolation via `chroot()`.
  * Syscall restriction via seccomp filter.

## Non-features

* `PUT` / `DELETE` & other HTTP methods that are not `GET`.
* TLS support (use a TLS tunnel).
* HTTP/2 or HTTP/3 support.
* File modification detection.
