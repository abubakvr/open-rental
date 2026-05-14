"use client";

import Link from "next/link";
import { useEffect, useState } from "react";

import ImuThreeCanvas from "@/app/dashboard/imu/imu-three-canvas";
import type { EspImuOrientation } from "@/lib/esp-imu-store";
import type { EspImuRaw } from "@/lib/esp-imu-raw-store";

type ApiBody = {
  ok: boolean;
  mqttConfigured: boolean;
  topicOrientation: string;
  topicRaw: string;
  pid?: number;
  data: EspImuOrientation | null;
  raw: EspImuRaw | null;
};

export default function ImuLiveClient() {
  const [orientation, setOrientation] = useState<EspImuOrientation | null>(
    null,
  );
  const [raw, setRaw] = useState<EspImuRaw | null>(null);
  const [mqttConfigured, setMqttConfigured] = useState(false);
  const [topicOrientation, setTopicOrientation] = useState(
    "device/imu/orientation",
  );
  const [topicRaw, setTopicRaw] = useState("device/imu/raw");
  const [pid, setPid] = useState<number | undefined>();
  const [sseOk, setSseOk] = useState<boolean | null>(null);
  const [sseError, setSseError] = useState<string | null>(null);
  const [zeroNonce, setZeroNonce] = useState(0);

  useEffect(() => {
    void (async () => {
      try {
        const res = await fetch(`/api/esp/imu?t=${Date.now()}`, {
          cache: "no-store",
          headers: { Accept: "application/json" },
        });
        const json = (await res.json()) as ApiBody;
        if (json.ok && json.data) setOrientation(json.data);
        if (json.ok && json.raw) setRaw(json.raw);
        setMqttConfigured(json.mqttConfigured);
        setTopicOrientation(json.topicOrientation);
        setTopicRaw(json.topicRaw);
        setPid(json.pid);
      } catch {
        /* initial snapshot optional */
      }
    })();
  }, []);

  useEffect(() => {
    const id = window.setInterval(() => {
      void (async () => {
        try {
          const res = await fetch(`/api/esp/imu?t=${Date.now()}`, {
            cache: "no-store",
            headers: { Accept: "application/json" },
          });
          const json = (await res.json()) as ApiBody;
          if (json.ok && json.raw) setRaw(json.raw);
        } catch {
          /* ignore */
        }
      })();
    }, 2000);
    return () => window.clearInterval(id);
  }, []);

  useEffect(() => {
    const es = new EventSource("/api/esp/imu/stream");
    es.onopen = () => {
      setSseOk(true);
      setSseError(null);
    };
    es.onmessage = (ev) => {
      try {
        const row = JSON.parse(ev.data) as EspImuOrientation;
        if (
          typeof row.qw === "number" &&
          typeof row.qx === "number" &&
          typeof row.qy === "number" &&
          typeof row.qz === "number"
        ) {
          setOrientation(row);
        }
      } catch {
        /* ignore bad chunk */
      }
    };
    es.onerror = () => {
      setSseOk(false);
      setSseError("EventSource disconnected (check network / server).");
    };
    return () => es.close();
  }, []);

  return (
    <div className="mx-auto flex w-full max-w-4xl flex-col gap-8 px-4 py-10">
      <header className="flex flex-col gap-2 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <p className="text-sm text-zinc-500 dark:text-zinc-400">
            Madgwick quaternion ·{" "}
            <code className="rounded bg-zinc-200/80 px-1 py-0.5 font-mono text-xs dark:bg-zinc-800">
              {topicOrientation}
            </code>
            {pid != null ? (
              <span className="ml-2 font-mono text-xs opacity-70">
                (api pid {pid})
              </span>
            ) : null}
          </p>
          <h1 className="text-2xl font-semibold tracking-tight">
            Live IMU orientation
          </h1>
          <p className="mt-1 max-w-2xl text-sm text-zinc-600 dark:text-zinc-400">
            The mesh uses the fused quaternion from MQTT each tick (with slerp so
            brief gaps do not snap). Yaw is not magnetically referenced in 6-DOF
            mode—expect slow drift. Use &quot;Zero orientation&quot; with the
            device in a known pose if the rest attitude does not match the model.
          </p>
        </div>
        <div className="flex flex-wrap items-center gap-3">
          <button
            type="button"
            onClick={() => setZeroNonce((n) => n + 1)}
            className="rounded-lg border border-zinc-300 bg-white px-3 py-2 text-sm font-medium text-zinc-900 shadow-sm hover:bg-zinc-50 dark:border-zinc-600 dark:bg-zinc-800 dark:text-zinc-100 dark:hover:bg-zinc-700"
          >
            Zero orientation
          </button>
          <Link
            href="/dashboard"
            className="text-sm font-medium text-zinc-600 underline-offset-4 hover:underline dark:text-zinc-300"
          >
            Telemetry dashboard
          </Link>
          <Link
            href="/"
            className="text-sm font-medium text-zinc-600 underline-offset-4 hover:underline dark:text-zinc-300"
          >
            Home
          </Link>
        </div>
      </header>

      {!mqttConfigured && (
        <div
          className="rounded-xl border border-amber-200 bg-amber-50 px-4 py-3 text-sm text-amber-950 dark:border-amber-900/60 dark:bg-amber-950/40 dark:text-amber-100"
          role="status"
        >
          <strong className="font-medium">MQTT not configured.</strong> Set{" "}
          <code className="rounded bg-amber-100/80 px-1 py-0.5 font-mono text-xs dark:bg-amber-900/50">
            MQTT_URL
          </code>
          . Topics default to{" "}
          <span className="font-mono">device/imu/orientation</span> and{" "}
          <span className="font-mono">device/imu/raw</span> (
          <code className="font-mono text-xs">MQTT_TOPIC_IMU_ORIENTATION</code>,{" "}
          <code className="font-mono text-xs">MQTT_TOPIC_IMU_RAW</code>).
        </div>
      )}

      {sseError && sseOk === false && (
        <div
          className="rounded-xl border border-red-200 bg-red-50 px-4 py-3 text-sm text-red-900 dark:border-red-900/50 dark:bg-red-950/40 dark:text-red-100"
          role="alert"
        >
          {sseError}
        </div>
      )}

      <ImuThreeCanvas orientation={orientation} zeroNonce={zeroNonce} />

      <section className="rounded-2xl border border-zinc-200 bg-zinc-50/90 p-4 text-sm shadow-sm dark:border-zinc-700 dark:bg-zinc-900/50">
        <h2 className="mb-3 font-medium text-zinc-900 dark:text-zinc-100">
          Latest orientation
        </h2>
        {!orientation ? (
          <p className="text-zinc-600 dark:text-zinc-400">
            Waiting for the first message on{" "}
            <span className="font-mono">{topicOrientation}</span>…
          </p>
        ) : (
          <dl className="grid grid-cols-2 gap-x-4 gap-y-2 font-mono text-xs sm:grid-cols-4">
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">qw</dt>
              <dd>{orientation.qw.toFixed(5)}</dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">qx</dt>
              <dd>{orientation.qx.toFixed(5)}</dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">qy</dt>
              <dd>{orientation.qy.toFixed(5)}</dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">qz</dt>
              <dd>{orientation.qz.toFixed(5)}</dd>
            </div>
            <div className="col-span-2 sm:col-span-4">
              <dt className="text-zinc-500 dark:text-zinc-400">updatedAt</dt>
              <dd className="break-all">{orientation.updatedAt}</dd>
            </div>
          </dl>
        )}
      </section>

      <section className="rounded-2xl border border-zinc-200 bg-zinc-50/90 p-4 text-sm shadow-sm dark:border-zinc-700 dark:bg-zinc-900/50">
        <h2 className="mb-2 font-medium text-zinc-900 dark:text-zinc-100">
          Optional raw stream
        </h2>
        <p className="mb-2 text-xs text-zinc-600 dark:text-zinc-400">
          Last sample from{" "}
          <code className="rounded bg-zinc-200/80 px-1 dark:bg-zinc-800">
            {topicRaw}
          </code>{" "}
          (only if your firmware publishes there).
        </p>
        {!raw ? (
          <p className="text-zinc-600 dark:text-zinc-400">No raw sample yet.</p>
        ) : (
          <dl className="grid grid-cols-2 gap-x-4 gap-y-2 font-mono text-xs sm:grid-cols-3">
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">ax, ay, az (g)</dt>
              <dd>
                {raw.ax.toFixed(4)}, {raw.ay.toFixed(4)}, {raw.az.toFixed(4)}
              </dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">gx, gy, gz (°/s)</dt>
              <dd>
                {raw.gx.toFixed(2)}, {raw.gy.toFixed(2)}, {raw.gz.toFixed(2)}
              </dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">updatedAt</dt>
              <dd className="break-all">{raw.updatedAt}</dd>
            </div>
          </dl>
        )}
      </section>
    </div>
  );
}
