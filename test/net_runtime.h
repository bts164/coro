#pragma once
// NetRuntime: the runtime the socket tests run on, so one test body serves every
// backend.
//
//   desktop   coro::Runtime with its defaults: the sockets are the kernel's.
//   CORO_PICO a runtime that drives lwIP but not the radio (PicoNetwork::Lwip).
//             The tests talk to 127.0.0.1, so a board without Wi-Fi runs them, and
//             so does the host build against lwIP.

#include <coro/runtime/runtime.h>

#ifdef CORO_PICO

#include <lwip/init.h>

struct NetRuntime : coro::Runtime {
    NetRuntime() : coro::Runtime(started_lwip()) {}

private:
    // lwip_init() must run exactly once per program, before the first socket call.
    // On the host each test file is its own program; a firmware image holds several.
    static coro::PicoNetwork started_lwip() {
        static bool started = false;
        if (!started) {
            lwip_init();
            started = true;
        }
        return coro::PicoNetwork::Lwip;
    }
};

#else

using NetRuntime = coro::Runtime;

#endif
