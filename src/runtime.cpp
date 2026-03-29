#include <coro/runtime.h>
#include <coro/single_threaded_executor.h>
#include <stdexcept>

namespace coro {

namespace {
    thread_local Runtime* t_current_runtime = nullptr;
} // namespace

Runtime::Runtime(std::size_t /*num_threads*/)
    : m_executor(std::make_unique<SingleThreadedExecutor>())
{
    // TODO Phase 3: create thread pool and libuv event loop
}

Runtime::~Runtime() = default;

void set_current_runtime(Runtime* rt) {
    t_current_runtime = rt;
}

Runtime& current_runtime() {
    if (!t_current_runtime)
        throw std::runtime_error("coro::current_runtime(): no runtime active on this thread");
    return *t_current_runtime;
}

} // namespace coro
