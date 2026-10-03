#pragma once

// Template method bodies for Pipe. Included at the bottom of pipe.h; never include
// this file directly.
//
// The futures themselves are the shared byte-stream futures in
// coro/detail/stream_io.h. See doc/design/tcp_stream.md, "Byte-stream futures".

#include <utility>

namespace coro {

template<ByteBuffer Buf>
PipeReadFuture<Buf, false> Pipe::read(Buf buf) {
    return PipeReadFuture<Buf, false>(m_state, std::move(buf));
}

template<ByteBuffer Buf>
PipeReadFuture<Buf, true> Pipe::read_exact(Buf buf) {
    return PipeReadFuture<Buf, true>(m_state, std::move(buf));
}

template<ByteBuffer Buf>
PipeWriteFuture<Buf> Pipe::write(Buf buf) {
    return PipeWriteFuture<Buf>(m_state, std::move(buf));
}

} // namespace coro
