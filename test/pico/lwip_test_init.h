#pragma once
// lwIP start-up shared by the loopback test files.

#include <lwip/init.h>

// Calls lwip_init() the first time only. Each loopback test file is its own
// program on the host, but the on-target firmware links them into one image,
// where every suite's SetUpTestSuite() runs.
inline void lwip_init_once() {
    static bool done = false;
    if (done) return;
    lwip_init();
    done = true;
}
