# gRPC Examples

Two example servers built with `coro::Runtime`, each with a matching client.

## Building the servers

Install dependencies and configure with Conan, then build:

```bash
pip install conan
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## HelloWorld

Demonstrates all four gRPC streaming patterns (unary, client-streaming, server-streaming, bidirectional) using a simple greeting service.

### Running the server

```bash
./build/hello_server
# or on a custom port:
./build/hello_server 50052
```

### Running the Python client

Install the Python gRPC tools and generate stubs from the proto file, from the `python` directory:

```bash
pip install grpcio grpcio-tools
python -m grpc_tools.protoc -I../src --python_out=. --grpc_python_out=. helloworld.proto
```

Then run the client:

```bash
python hello_client.py
# or with a custom host/port:
python hello_client.py --host localhost --port 50052
```

---

## Live Signal Plot

A server-streaming example that pushes signal frames to a live vispy plot. The server
sends an initial `SignalInfo` message describing the x-axis, then streams `DataFrame`
messages at the requested frame rate.

### Running the server

```bash
./build/plot_server
# or on a custom port:
./build/plot_server 50052
```

### Running the Python client

Install dependencies and generate stubs from the proto file, from the `python` directory:

```bash
pip install grpcio grpcio-tools vispy
python -m grpc_tools.protoc -I../src --python_out=. --grpc_python_out=. plotsignal.proto
```

Then run the client:

```bash
python plot_live_grpc.py
# or with a custom host/port:
python plot_live_grpc.py --host localhost --port 50052
```
