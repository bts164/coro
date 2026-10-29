#pragma once
// Stub for the Pico SDK's pico/lwip_nosys.h: runs lwIP's timers on an async_context,
// as the SDK's does. Implemented in async_context_stub.cpp.
#include <pico/async_context.h>

bool lwip_nosys_init(async_context_t* context);
void lwip_nosys_deinit(async_context_t* context);
