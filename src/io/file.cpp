// Desktop File on the Runtime's blocking pool: no libuv. Every syscall goes through the
// backend seam in include/coro/detail/sys/file.h. See doc/design/file_io.md.

#include <coro/io/file.h>
#include <stdexcept>
#include <system_error>

namespace coro {

namespace {

bool has(FileMode mode, FileMode flag) { return (mode & flag) != FileMode{}; }

detail::sys::FileOpenFlags translate_flags(FileMode mode) {
    detail::sys::FileOpenFlags flags;
    flags.write    = has(mode, FileMode::Write);
    // Neither Read nor Write still opens read-only, as open(2) with O_RDONLY would.
    flags.read     = has(mode, FileMode::Read) || !flags.write;
    flags.create   = has(mode, FileMode::Create);
    flags.truncate = has(mode, FileMode::Truncate);
    flags.append   = has(mode, FileMode::Append);
    return flags;
}

} // namespace

const std::shared_ptr<detail::FileState>& File::state(const char* what) const {
    if (!m_state) throw std::logic_error(std::string(what) + ": moved-from File");
    return m_state;
}

File::OpenFuture File::open(std::string path, FileMode mode) {
    return spawn_blocking([path = std::move(path), flags = translate_flags(mode)]() -> File {
        auto fd = detail::sys::file_open(path, flags);
        if (!fd)
            throw std::system_error(fd.error(), std::system_category(),
                                    "coro::File::open: " + path);
        // If the OpenFuture was dropped, this File dies with the job's result and
        // closes the fd.
        return File(std::make_shared<detail::FileState>(*fd));
    }).detach();  // a dropped operation still runs; see the File class comment
}

BlockingHandle<void> File::sync_all() {
    return spawn_blocking([state = state("coro::File::sync_all")] {
        if (auto r = detail::sys::file_sync(state->fd); !r)
            throw std::system_error(r.error(), std::system_category(), "coro::File::sync_all");
    }).detach();
}

} // namespace coro
