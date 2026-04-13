#!/usr/bin/env python3
"""Simple gRPC client for the HelloWorld example server."""

import grpc
import helloworld_pb2
import helloworld_pb2_grpc


def run(host: str = "localhost", port: int = 50051) -> None:
    with grpc.insecure_channel(f"{host}:{port}") as channel:
        stub = helloworld_pb2_grpc.GreeterStub(channel)

        # Unary: SayHello
        reply = stub.SayHello(helloworld_pb2.HelloRequest(name="World"))
        print(f"SayHello reply:              {reply.message}")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="HelloWorld gRPC client")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=50051)
    args = parser.parse_args()

    run(args.host, args.port)
