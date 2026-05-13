"use client";

import Link from "next/link";
import { useEffect, useState } from "react";

import ImuThreeCanvas from "@/app/dashboard/imu/imu-three-canvas";
import type { EspImuSnapshot } from "@/lib/esp-imu-store";

type ApiBody = {
  ok: boolean;
  mqttConfigured: boolean;
  topic: string;
  pid?: number;
  data: EspImuSnapshot | null;
};

export default function ImuLiveClient() {
  const [imu, setImu] = useState<EspImuSnapshot | null>(null);
  const [mqttConfigured, setMqttConfigured] = useState(false);
  const [topic, setTopic] = useState("open-rental/esp/imu");
  const [pid, setPid] = useState<number | undefined>();
  const [sseOk, setSseOk] = useState<boolean | null>(null);
  const [sseError, setSseError] = useState<string | null>(null);

  useEffect(() => {
    void (async () => {
      try {
        const res = await fetch(`/api/esp/imu?t=${Date.now()}`, {
          cache: "no-store",
          headers: { Accept: "application/json" },
        });
        const json = (await res.json()) as ApiBody;
        if (json.ok && json.data) setImu(json.data);
        setMqttConfigured(json.mqttConfigured);
        setTopic(json.topic);
        setPid(json.pid);
      } catch {
        /* initial snapshot optional */
      }
    })();
  }, []);

  useEffect(() => {
    const es = new EventSource("/api/esp/imu/stream");
    es.onopen = () => {
      setSseOk(true);
      setSseError(null);
    };
    es.onmessage = (ev) => {
      try {
        const row = JSON.parse(ev.data) as EspImuSnapshot;
        if (typeof row.ax === "number") setImu(row);
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
            ICM-20948 · MQTT topic{" "}
            <code className="rounded bg-zinc-200/80 px-1 py-0.5 font-mono text-xs dark:bg-zinc-800">
              {topic}
            </code>
            {pid != null ? (
              <span className="ml-2 font-mono text-xs opacity-70">
                (api pid {pid})
              </span>
            ) : null}
          </p>
          <h1 className="text-2xl font-semibold tracking-tight">
            Live IMU figure
          </h1>
          <p className="mt-1 max-w-2xl text-sm text-zinc-600 dark:text-zinc-400">
            Acceleration tilts and shifts the figure; gyro rates add gentle spin.
            Updates follow the device publish interval (often about 3 seconds).
          </p>
        </div>
        <div className="flex flex-wrap gap-3">
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
          </code>{" "}
          and optionally{" "}
          <code className="rounded bg-amber-100/80 px-1 py-0.5 font-mono text-xs dark:bg-amber-900/50">
            MQTT_TOPIC_IMU
          </code>{" "}
          (default <span className="font-mono">open-rental/esp/imu</span>).
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

      <ImuThreeCanvas imu={imu} />

      <section className="rounded-2xl border border-zinc-200 bg-zinc-50/90 p-4 text-sm shadow-sm dark:border-zinc-700 dark:bg-zinc-900/50">
        <h2 className="mb-3 font-medium text-zinc-900 dark:text-zinc-100">
          Latest sample
        </h2>
        {!imu ? (
          <p className="text-zinc-600 dark:text-zinc-400">
            Waiting for the first IMU message on{" "}
            <span className="font-mono">{topic}</span>…
          </p>
        ) : (
          <dl className="grid grid-cols-2 gap-x-4 gap-y-2 font-mono text-xs sm:grid-cols-3 md:grid-cols-4">
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">ax, ay, az (g)</dt>
              <dd>
                {imu.ax.toFixed(4)}, {imu.ay.toFixed(4)}, {imu.az.toFixed(4)}
              </dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">aMag</dt>
              <dd>{imu.aMag.toFixed(4)}</dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">gx, gy, gz (°/s)</dt>
              <dd>
                {imu.gx.toFixed(2)}, {imu.gy.toFixed(2)}, {imu.gz.toFixed(2)}
              </dd>
            </div>
            <div>
              <dt className="text-zinc-500 dark:text-zinc-400">updatedAt</dt>
              <dd className="break-all">{imu.updatedAt}</dd>
            </div>
          </dl>
        )}
      </section>
    </div>
  );
}
