# gRPC HelloWorld Example

A simple gRPC server built with `coro::Runtime` and a Python test client.

## Building the server

Install dependencies and configure with Conan, then build:

```bash
pip install conan
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Running the server

```bash
./build/hello_server
# or on a custom port:
./build/hello_server 50052
```

## Running the Python client

Install the Python gRPC tools and generate stubs from the proto file:

```bash
pip install grpcio grpcio-tools
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. helloworld.proto
```

Then run the client:

```bash
python hello_client.py
# or with a custom host/port:
python hello_client.py --host localhost --port 50052
```
