// Pipe (named FIFO) on the IoDriver. Every syscall goes through the backend seam in
// include/coro/detail/sys/pipe.h. See doc/design/pipe_streaming.md.

#include <coro/io/pipe.h>
#include <coro/sync/sleep.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>

namespace coro {

namespace {

using namespace std::chrono_literals;

// Write-side open retry: no reader yet gives no readiness event, so poll on a timer.
constexpr std::chrono::milliseconds kFirstOpenRetry = 1ms;
constexpr std::chrono::milliseconds kMaxOpenRetry   = 50ms;

/// Waits for a read end to see a writer (fifo_try_writer_seen). A leaf future, so
/// dropping a Read-mode open() mid-wait just drops the state, which closes the fd.
class WriterSeenFuture {
public:
    using OutputType = void;

    explicit WriterSeenFuture(std::shared_ptr<detail::SocketState> state)
        : m_state(std::move(state)) {}

    PollResult<void> poll(detail::Context& ctx) {
        // Readiness starts set, so the first poll checks at once; a writer that wrote
        // or left before the registration is seen then. After that, data (EPOLLIN) or
        // the writer closing (EPOLLHUP) wakes it; a writer that only opens doesn't.
        auto result = m_state->reg.poll_io(IoDirection::Read, ctx, [this] {
            return detail::sys::fifo_try_writer_seen(m_state->fd);
        });
        if (!result) return PollPending;
        if (!*result) return PollError(detail::socket_error(result->error(), "Pipe::open"));
        return PollReady;
    }

private:
    std::shared_ptr<detail::SocketState> m_state;
};

} // namespace

Pipe::Pipe(std::shared_ptr<State> state) : m_state(std::move(state)) {}

Pipe::Pipe(Pipe&&) noexcept = default;
Pipe& Pipe::operator=(Pipe&&) noexcept = default;
Pipe::~Pipe() = default;

Coro<Pipe> Pipe::open(std::string path, PipeMode mode) {
    IoDriver& driver = detail::socket_io_driver("Pipe::open");
    switch (mode) {
    case PipeMode::Read: {
        auto state = std::make_shared<State>(driver, detail::sys::fifo_open_read(path));
        co_await WriterSeenFuture(state);
        co_return Pipe(std::move(state));
    }
    case PipeMode::Write: {
        auto delay = kFirstOpenRetry;
        for (;;) {
            auto fd = detail::sys::fifo_try_open_write(path);
            if (fd) co_return Pipe(std::make_shared<State>(driver, *fd));
            if (fd.error() != ENXIO)
                throw std::system_error(fd.error(), std::system_category(),
                                        "Pipe::open: " + path);
            // Race (benign): a reader may open just after the failed attempt; it is
            // seen on the next retry, at most kMaxOpenRetry later.
            co_await sleep_for(delay);
            delay = std::min(delay * 2, kMaxOpenRetry);
        }
    }
    case PipeMode::ReadWrite:
        co_return Pipe(std::make_shared<State>(driver, detail::sys::fifo_open_read_write(path)));
    }
    throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                            "Pipe::open: bad PipeMode");
}

Coro<void> Pipe::create(std::string path, int permission) {
    detail::sys::fifo_create(path, permission);
    co_return;
}

} // namespace coro
