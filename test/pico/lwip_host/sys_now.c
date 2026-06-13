/* sys_now() for lwIP NO_SYS mode on Linux host.
 *
 * lwIP calls sys_now() to drive its internal timeout wheel
 * (sys_check_timeouts). Returns monotonic milliseconds. */

#include <lwip/arch.h>
#include <time.h>

u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)((u32_t)ts.tv_sec * 1000u + (u32_t)(ts.tv_nsec / 1000000u));
}
