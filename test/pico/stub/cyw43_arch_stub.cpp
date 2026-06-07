// No-op cyw43_arch_poll for executor-only tests.
// The executor calls this in its poll loop in place of blocking; without any
// TCP sources compiled there are no lwIP callbacks to fire, so a no-op is correct.
void cyw43_arch_poll() {}
