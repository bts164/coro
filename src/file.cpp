#include <coro/io/file.h>
#include <coro/runtime/uv_future.h>
#include <coro/task/spawn_on.h>
#include <coro/coro.h>
#include <system_error>
#include <fcntl.h>

namespace coro {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

[[noreturn]] void throw_uv_error(int status, const char* what) {
    throw std::system_error(
        std::error_code(-status, std::system_category()), what);
}

int translate_flags(FileMode mode) {
    int flags = 0;
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite) flags |= O_RDWR;
    else if ((mode & FileMode::Write) != FileMode{})         flags |= O_WRONLY;
    else                                                     flags |= O_RDONLY;
    if ((mode & FileMode::Create)   != FileMode{}) flags |= O_CREAT;
    if ((mode & FileMode::Truncate) != FileMode{}) flags |= O_TRUNC;
    if ((mode & FileMode::Append)   != FileMode{}) flags |= O_APPEND;
    return flags;
}

} // namespace

// ---------------------------------------------------------------------------
// File — construction / move / destruction
// ---------------------------------------------------------------------------

File::File(uv_file fd, SingleThreadedUvExecutor* exec)
    : m_fd(fd), m_exec(exec) {}

File::File(File&& other) noexcept
    : m_fd(other.m_fd), m_exec(other.m_exec) {
    other.m_fd = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (m_fd >= 0 && m_exec) {
            with_context(*m_exec, [](SingleThreadedUvExecutor& exec, uv_file fd) -> Coro<void> {
                uv_fs_t req;
                UvCallbackResult<uv_fs_t*> result;
                req.data = &result;
                uv_fs_close(exec.loop(), &req, fd, [](uv_fs_t *req) {
                    reinterpret_cast<decltype(result)*>(req->data)->complete(req);
                });
                co_await wait(result);
                uv_fs_req_cleanup(&req);
            }(*m_exec, m_fd)).detach();
        }
        m_fd   = other.m_fd;
        m_exec = other.m_exec;
        other.m_fd = -1;
    }
    return *this;
}

File::~File() {
    if (m_fd >= 0 && m_exec) {
        with_context(*m_exec, [](SingleThreadedUvExecutor& exec, uv_file fd) -> Coro<void> {
            uv_fs_t req;
            UvCallbackResult<uv_fs_t*> result;
            req.data = &result;
            uv_fs_close(exec.loop(), &req, fd, [](uv_fs_t *req) {
                reinterpret_cast<decltype(result)*>(req->data)->complete(req);
            });
            co_await wait(result);
            uv_fs_req_cleanup(&req);
        }(*m_exec, m_fd)).detach();
    }
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------

File::OpenFuture File::open(std::string path, FileMode mode) {
    int flags = translate_flags(mode);
    auto& exec = current_uv_executor();
    return with_context(exec,
            [](SingleThreadedUvExecutor& exec,
               std::string path, int flags) -> Coro<File> {

                UvCallbackResult<uv_file> result;
                uv_fs_t req;
                req.data = &result;

                int r = uv_fs_open(exec.loop(), &req, path.c_str(), flags, 0644,
                    [](uv_fs_t* r) {
                        static_cast<decltype(result)*>(r->data)
                            ->complete(static_cast<uv_file>(r->result));
                        uv_fs_req_cleanup(r);
                    });
                if (r < 0)
                    throw_uv_error(r, "uv_fs_open");

                auto [fd] = co_await wait(result);
                if (fd < 0)
                    throw_uv_error(static_cast<int>(fd), "uv_fs_open");

                co_return File(fd, &exec);
            }(exec, std::move(path), flags)
    );
}


} // namespace coro
