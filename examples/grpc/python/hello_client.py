#!/usr/bin/env python3
"""gRPC client for the HelloWorld example server.

Runs all four RPC types in parallel, --iterations times each.
Uses grpc.aio so every call is a native asyncio coroutine; asyncio.gather
fires all iterations concurrently and waits for all of them to finish.
"""

import asyncio
import argparse
import grpc
import grpc.aio
import helloworld_pb2
import helloworld_pb2_grpc


async def say_hello(stub: helloworld_pb2_grpc.GreeterStub, i: int) -> None:
    reply = await stub.SayHello(helloworld_pb2.HelloRequest(name=f"World-{i}"))
    print(f"[SayHello/{i}] {reply.message}")


async def say_hello_stream_request(stub: helloworld_pb2_grpc.GreeterStub, i: int) -> None:
    async def requests():
        for name in [f"Alice-{i}", f"Bob-{i}", f"Charlie-{i}"]:
            yield helloworld_pb2.HelloRequest(name=name)

    reply = await stub.SayHelloStreamRequest(requests())
    print(f"[SayHelloStreamRequest/{i}] {reply.message}")


async def say_hello_stream_reply(stub: helloworld_pb2_grpc.GreeterStub, i: int) -> None:
    request = helloworld_pb2.HelloRequest(name=f"foo-{i} bar-{i} baz-{i}")
    async for reply in stub.SayHelloStreamReply(request):
        print(f"[SayHelloStreamReply/{i}] {reply.message}")


async def say_hello_bidi(stub: helloworld_pb2_grpc.GreeterStub, i: int) -> None:
    async def requests():
        for name in [f"ping-{i}", f"pong-{i}", f"foo-{i}"]:
            yield helloworld_pb2.HelloRequest(name=name)

    async for reply in stub.SayHelloBidiStream(requests()):
        print(f"[SayHelloBidiStream/{i}] {reply.message}")


async def run(host: str = "localhost", port: int = 50051, iterations: int = 3) -> None:
    async with grpc.aio.insecure_channel(f"{host}:{port}") as channel:
        stub = helloworld_pb2_grpc.GreeterStub(channel)

        await asyncio.gather(*(
            coro
            for i in range(iterations)
            for coro in (
                say_hello(stub, i),
                say_hello_stream_request(stub, i),
                say_hello_stream_reply(stub, i),
                say_hello_bidi(stub, i),
            )
        ))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="HelloWorld gRPC client")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=50051)
    parser.add_argument("--iterations", type=int, default=5,
                        help="Number of times to run each RPC type in parallel")
    args = parser.parse_args()

    asyncio.run(run(args.host, args.port, args.iterations))
