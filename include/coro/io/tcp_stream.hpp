#pragma once

// Template method bodies for the desktop TcpStream. Included at the bottom of
// tcp_stream.h; never include this file directly.
//
// The futures themselves are the shared byte-stream futures in
// coro/detail/stream_io.h. What dropping one does to the byte stream is described on
// TcpStream. See doc/design/tcp_stream.md, "Byte-stream futures".

#include <utility>

namespace coro {

template<ByteBuffer Buf>
TcpReadFuture<Buf, false> TcpStream::read(Buf buf) {
    return TcpReadFuture<Buf, false>(m_state, std::move(buf));
}

template<ByteBuffer Buf>
TcpReadFuture<Buf, true> TcpStream::read_exact(Buf buf) {
    return TcpReadFuture<Buf, true>(m_state, std::move(buf));
}

template<ByteBuffer Buf>
TcpWriteFuture<Buf> TcpStream::write(Buf buf) {
    return TcpWriteFuture<Buf>(m_state, std::move(buf));
}

} // namespace coro
