"""
Live-updating plot fed from a gRPC signal stream.
==================================================
Receives raw int16 two-channel frames, computes complex magnitude, and plots.

Generate the gRPC stubs first:
    python -m grpc_tools.protoc -I.. --python_out=. --grpc_python_out=. ../plotsignal.proto
"""

import asyncio
import argparse
import threading
import numpy as np
import grpc
import grpc.aio
import plotsignal_pb2
import plotsignal_pb2_grpc
import os

from vispy import plot as vp

# ── vispy setup ───────────────────────────────────────────────────────────────
fig = vp.Fig(size=(900, 400), show=False)
pw = fig[0, 0]
pw.title.text = "Live Signal — magnitude (gRPC)"
pw.xlabel = "x"
pw.ylabel = "|I + jQ|"

_placeholder_x = np.linspace(0, 1, 2, dtype=np.float32)
_placeholder_y = np.zeros(2, dtype=np.float32)
line1 = pw.plot((_placeholder_x, _placeholder_y), color="steelblue", width=2, marker_size=0)
line2 = pw.plot((_placeholder_x, _placeholder_y), color="red", width=2, marker_size=0)

async def read_stream(args) -> None:
    """Connect to the signal server and update the plot on each incoming frame."""
    x: np.ndarray | None = None
    x_start: float = 0.0
    x_end: float = 1.0

    async with grpc.aio.insecure_channel(f"{args.host}:{args.port}") as channel:
        stub = plotsignal_pb2_grpc.SignalServiceStub(channel)
        request = plotsignal_pb2.SubscribeRequest(pipePath=os.path.abspath(args.pipe_path))

        async for response in stub.Subscribe(request):
            if response.HasField("info"):
                x_start = response.info.x_start
                x_end   = response.info.x_end

            elif response.HasField("frame"):
                # Decode raw bytes → int16 → shape (N, 2)
                raw  = np.frombuffer(response.frame.y, dtype=np.int16)
                data = raw.reshape(-1, 2).astype(np.float32)
                n    = data.shape[0]

                # Rebuild x only when sample count changes
                if x is None or len(x) != n:
                    x = np.linspace(x_start, x_end, n, dtype=np.float32)
                    # Max possible magnitude for int16 ±1000 range ≈ 1414
                    pw.view.camera.set_range(x=(x_start, x_end), y=(0, 1500))

                #magnitude = np.real(data[:, 0] + 1j * data[:, 1]).astype(np.float32)
                line1.set_data(np.column_stack([x, data[:,0]]))
                line2.set_data(np.column_stack([x, data[:,1]]))
                fig.update()


# ── Run both event loops ──────────────────────────────────────────────────────
def _start_grpc(args):
    threading.Thread(target=asyncio.run, args=(read_stream(args),), daemon=True).start()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Live plot fed from a gRPC signal stream.")
    parser.add_argument("--host",      default="localhost", help="Signal server host")
    parser.add_argument("--port",      type=int, default=50051, help="Signal server port")
    parser.add_argument("--pipe-path", required=True, help="Path to the named pipe (passed to server)")
    args = parser.parse_args()
    _start_grpc(args)
    fig.show(run=True)
