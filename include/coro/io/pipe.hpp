#pragma once

// Template implementations for Pipe::write / Pipe::read.
// Included at the bottom of pipe.h — not meant to be included directly.

#include <coro/io/pipe.h>

namespace coro {

template<ByteBuffer Buf>
WriteHandle Pipe::write(Buf buf) {
    auto req = detail::make_write_request(std::move(buf));
    detail::push_write_request(*m_state, req);  // req shared between queue and handle
    return WriteHandle(std::move(req));
}

template<ByteBuffer Buf>
ReadHandle<Buf> Pipe::read(Buf buf) {
    auto req = detail::make_read_request<Buf>(std::move(buf));
    detail::push_read_request(*m_state, req);   // req shared between queue and handle
    return ReadHandle<Buf>(std::move(req));
}

} // namespace coro
