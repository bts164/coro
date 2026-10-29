#pragma once
// NetRuntime: the runtime the socket tests run on, so one test body serves every
// backend.
//
//   desktop   coro::Runtime with its defaults: the sockets are the kernel's.
//   CORO_PICO a runtime that runs lwIP but not the radio (PicoNetwork::Lwip), which
//             starts lwIP itself. The tests talk to 127.0.0.1, so a board without
//             Wi-Fi runs them, and so does the host build against lwIP.

#include <coro/runtime/runtime.h>

#ifdef CORO_PICO

struct NetRuntime : coro::Runtime {
    NetRuntime() : coro::Runtime(coro::PicoNetwork::Lwip) {}
};

#else

using NetRuntime = coro::Runtime;

#endif
