import { ensureMqttApp } from "@/lib/mqtt-app";
import {
  getLatestEspImu,
  subscribeEspImu,
} from "@/lib/esp-imu-store";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(req: Request) {
  ensureMqttApp();
  const encoder = new TextEncoder();

  const state = {
    intervalId: null as ReturnType<typeof setInterval> | null,
    unsub: null as null | (() => void),
    disposed: false,
  };

  const dispose = () => {
    if (state.disposed) return;
    state.disposed = true;
    if (state.intervalId) {
      clearInterval(state.intervalId);
      state.intervalId = null;
    }
    state.unsub?.();
    state.unsub = null;
  };

  const stream = new ReadableStream<Uint8Array>({
    start(controller) {
      const latest = getLatestEspImu();
      if (latest) {
        controller.enqueue(
          encoder.encode(`data: ${JSON.stringify(latest)}\n\n`),
        );
      }

      state.unsub = subscribeEspImu((json) => {
        try {
          controller.enqueue(encoder.encode(`data: ${json}\n\n`));
        } catch {
          /* stream closed */
        }
      });

      state.intervalId = setInterval(() => {
        try {
          controller.enqueue(encoder.encode(`: keepalive\n\n`));
        } catch {
          /* stream closed */
        }
      }, 20_000);

      req.signal.addEventListener("abort", dispose);
    },
    cancel() {
      dispose();
    },
  });

  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-cache, no-transform",
      Connection: "keep-alive",
    },
  });
}
