"""
Live-updating plot fed from a gRPC signal stream.
==================================================
Replace plot_live.py's timer + synthetic data with a real server stream.

Generate the gRPC stubs first:
    python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. signal.proto
"""

import asyncio
import threading
import numpy as np
import grpc
import grpc.aio
import plotsignal_pb2
import plotsignal_pb2_grpc

from vispy import plot as vp

# ── vispy setup ───────────────────────────────────────────────────────────────
fig = vp.Fig(size=(900, 400), show=False)
pw = fig[0, 0]
pw.title.text = "Live Signal (gRPC)"

_placeholder_x = np.linspace(0, 1, 2, dtype=np.float32)
_placeholder_y = np.zeros(2, dtype=np.float32)
line = pw.plot((_placeholder_x, _placeholder_y), color="steelblue", width=4, marker_size=0)


async def read_stream(host: str = "localhost", port: int = 50051) -> None:
    """Connect to the signal server and update the plot on each incoming frame."""
    x: np.ndarray | None = None

    async with grpc.aio.insecure_channel(f"{host}:{port}") as channel:
        stub = plotsignal_pb2_grpc.SignalServiceStub(channel)
        request = plotsignal_pb2.SubscribeRequest(target_fps=60)

        async for response in stub.Subscribe(request):
            if response.HasField("info"):
                info = response.info
                x = np.linspace(info.x_start, info.x_end, info.num_samples,
                                dtype=np.float32)
                pw.view.camera.set_range(x=(info.x_start, info.x_end), y=(-2, 2))

            elif response.HasField("frame") and x is not None:
                y = np.array(response.frame.y, dtype=np.float32)
                line.set_data(np.column_stack([x, y]))
                fig.update()


# ── Run both event loops ──────────────────────────────────────────────────────
def _start_grpc():
    threading.Thread(target=asyncio.run, args=(read_stream(),), daemon=True).start()


if __name__ == "__main__":
    _start_grpc()
    fig.show(run=True)
