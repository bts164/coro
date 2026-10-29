#pragma once
// Stub for the Pico SDK's pico/async_context.h, for Linux test builds. Implemented in
// async_context_stub.cpp. See doc/design/pico_port.md, "Testing on the host".
//
// The types and field names are the SDK's, so the lwIP backend compiles against
// either. The behaviour coro relies on is reproduced exactly:
//   - async_context_set_work_pending() may be called from any thread (on the board,
//     from an interrupt handler), and releases the context's binary, latched permit;
//   - async_context_wait_for_work_until() waits for that permit or the earlier of its
//     deadline and the next at-time worker, and consumes the permit;
//   - async_context_poll() runs due at-time workers, then every when-pending worker
//     whose flag is set, clearing the flag before calling it.
#include <pico/time.h>
#include <cstdint>

typedef struct async_context async_context_t;

typedef struct async_when_pending_worker {
    struct async_when_pending_worker* next;
    void (*do_work)(async_context_t* context, struct async_when_pending_worker* worker);
    bool  work_pending;
    void* user_data;
} async_when_pending_worker_t;

typedef struct async_work_on_timeout {
    struct async_work_on_timeout* next;
    void (*do_work)(async_context_t* context, struct async_work_on_timeout* timeout);
    absolute_time_t next_time;
    void* user_data;
} async_at_time_worker_t;

enum {
    ASYNC_CONTEXT_POLL                  = 1,
    ASYNC_CONTEXT_THREADSAFE_BACKGROUND = 2,
    ASYNC_CONTEXT_FREERTOS              = 3,
};

// Only the tag: the stub's functions are plain functions, not this table.
typedef struct async_context_type {
    uint16_t type;
} async_context_type_t;

// The stub's own state lives behind `state`, created by
// async_context_poll_init_with_defaults(), which also sets `type`.
struct async_context_stub_state;
struct async_context {
    const async_context_type_t* type;
    async_context_stub_state*   state;
};

bool async_context_add_when_pending_worker(async_context_t* context,
                                           async_when_pending_worker_t* worker);
bool async_context_remove_when_pending_worker(async_context_t* context,
                                              async_when_pending_worker_t* worker);
void async_context_set_work_pending(async_context_t* context,
                                    async_when_pending_worker_t* worker);
bool async_context_add_at_time_worker(async_context_t* context,
                                      async_at_time_worker_t* worker);
bool async_context_add_at_time_worker_in_ms(async_context_t* context,
                                            async_at_time_worker_t* worker, uint32_t ms);
bool async_context_remove_at_time_worker(async_context_t* context,
                                         async_at_time_worker_t* worker);
void async_context_poll(async_context_t* context);
void async_context_wait_for_work_until(async_context_t* context, absolute_time_t until);
void async_context_deinit(async_context_t* context);
