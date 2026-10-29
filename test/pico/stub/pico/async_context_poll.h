#pragma once
// Stub for the Pico SDK's pico/async_context_poll.h. See async_context.h.
#include <pico/async_context.h>

typedef struct async_context_poll {
    async_context_t core;
} async_context_poll_t;

bool async_context_poll_init_with_defaults(async_context_poll_t* self);
