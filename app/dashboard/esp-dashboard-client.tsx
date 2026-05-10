"use client";

import Link from "next/link";
import { useCallback, useEffect, useState } from "react";

type TelemetryRow = {
  lat: number;
  lng: number;
  uptimeSec: number;
  rssi: number | null;
  updatedAt: string;
};

type ApiBody = {
  ok: boolean;
  mqttConfigured: boolean;
  data: TelemetryRow | null;
};

function formatUptime(sec: number): string {
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (d > 0) return `${d}d ${h}h ${m}m ${s}s`;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

/** Map RSSI (approx -90 poor … -30 excellent) to 0–100 for a bar */
function rssiQualityPercent(rssi: number): number {
  const lo = -90;
  const hi = -30;
  const clamped = Math.min(hi, Math.max(lo, rssi));
  return Math.round(((clamped - lo) / (hi - lo)) * 100);
}

function osmEmbedSrc(lat: number, lng: number): string {
  const delta = 0.02;
  const minLon = lng - delta;
  const minLat = lat - delta;
  const maxLon = lng + delta;
  const maxLat = lat + delta;
  return `https://www.openstreetmap.org/export/embed.html?bbox=${minLon}%2C${minLat}%2C${maxLon}%2C${maxLat}&layer=mapnik&marker=${lat}%2C${lng}`;
}

export default function EspDashboardClient() {
  const [body, setBody] = useState<ApiBody | null>(null);
  const [error, setError] = useState<string | null>(null);

  const fetchTelemetry = useCallback(async () => {
    try {
      const res = await fetch("/api/esp/telemetry", { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const json = (await res.json()) as ApiBody;
      setBody(json);
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Request failed");
    }
  }, []);

  useEffect(() => {
    const id = setInterval(() => void fetchTelemetry(), 4000);
    queueMicrotask(() => void fetchTelemetry());
    return () => clearInterval(id);
  }, [fetchTelemetry]);

  const data = body?.data ?? null;
  const mqttConfigured = body?.mqttConfigured ?? false;

  return (
    <div className="mx-auto flex w-full max-w-4xl flex-col gap-8 px-4 py-10">
      <header className="flex flex-col gap-2 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <p className="text-sm text-zinc-500 dark:text-zinc-400">
            ESP controller
          </p>
          <h1 className="text-2xl font-semibold tracking-tight">
            Telemetry dashboard
          </h1>
        </div>
        <Link
          href="/"
          className="text-sm font-medium text-zinc-600 underline-offset-4 hover:underline dark:text-zinc-300"
        >
          Back to home
        </Link>
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
          (and optional{" "}
          <code className="rounded bg-amber-100/80 px-1 py-0.5 font-mono text-xs dark:bg-amber-900/50">
            MQTT_TOPIC
          </code>
          ) so this server can subscribe and fill telemetry.
        </div>
      )}

      {error && (
        <div
          className="rounded-xl border border-red-200 bg-red-50 px-4 py-3 text-sm text-red-900 dark:border-red-900/50 dark:bg-red-950/40 dark:text-red-100"
          role="alert"
        >
          {error}
        </div>
      )}

      <section className="grid gap-6 md:grid-cols-2">
        <div className="rounded-2xl border border-zinc-200 bg-white p-6 shadow-sm dark:border-zinc-800 dark:bg-zinc-950">
          <h2 className="text-sm font-medium uppercase tracking-wide text-zinc-500 dark:text-zinc-400">
            Coordinates
          </h2>
          {data ? (
            <dl className="mt-4 space-y-2 font-mono text-lg">
              <div className="flex justify-between gap-4">
                <dt className="text-zinc-500 dark:text-zinc-400">Latitude</dt>
                <dd>{data.lat.toFixed(6)}</dd>
              </div>
              <div className="flex justify-between gap-4">
                <dt className="text-zinc-500 dark:text-zinc-400">Longitude</dt>
                <dd>{data.lng.toFixed(6)}</dd>
              </div>
              <div className="flex justify-between gap-4 border-t border-zinc-100 pt-2 text-xs text-zinc-500 dark:border-zinc-800 dark:text-zinc-400">
                <dt>Last update</dt>
                <dd>{new Date(data.updatedAt).toLocaleString()}</dd>
              </div>
            </dl>
          ) : (
            <p className="mt-4 text-zinc-600 dark:text-zinc-400">
              Waiting for the first MQTT message…
            </p>
          )}
        </div>

        <div className="rounded-2xl border border-zinc-200 bg-white p-6 shadow-sm dark:border-zinc-800 dark:bg-zinc-950">
          <h2 className="text-sm font-medium uppercase tracking-wide text-zinc-500 dark:text-zinc-400">
            Device &amp; network
          </h2>
          {data ? (
            <dl className="mt-4 space-y-4">
              <div>
                <dt className="text-xs uppercase text-zinc-500 dark:text-zinc-400">
                  Uptime since last reboot
                </dt>
                <dd className="mt-1 font-mono text-xl">
                  {formatUptime(data.uptimeSec)}
                </dd>
                <dd className="mt-0.5 font-mono text-xs text-zinc-500 dark:text-zinc-400">
                  {data.uptimeSec.toLocaleString()} s
                </dd>
              </div>
              <div>
                <dt className="text-xs uppercase text-zinc-500 dark:text-zinc-400">
                  Wi‑Fi signal (RSSI)
                </dt>
                {data.rssi !== null ? (
                  <>
                    <dd className="mt-1 font-mono text-xl">{data.rssi} dBm</dd>
                    <div
                      className="mt-3 h-2 w-full overflow-hidden rounded-full bg-zinc-200 dark:bg-zinc-800"
                      aria-label="Signal strength"
                    >
                      <div
                        className="h-full rounded-full bg-emerald-500 transition-[width] duration-500 dark:bg-emerald-400"
                        style={{
                          width: `${rssiQualityPercent(data.rssi)}%`,
                        }}
                      />
                    </div>
                    <dd className="mt-1 text-xs text-zinc-500 dark:text-zinc-400">
                      Approx. quality: {rssiQualityPercent(data.rssi)}%
                    </dd>
                  </>
                ) : (
                  <dd className="mt-1 text-zinc-600 dark:text-zinc-400">
                    Not reported
                  </dd>
                )}
              </div>
            </dl>
          ) : (
            <p className="mt-4 text-zinc-600 dark:text-zinc-400">
              Waiting for device metrics…
            </p>
          )}
        </div>
      </section>

      <section className="overflow-hidden rounded-2xl border border-zinc-200 bg-white shadow-sm dark:border-zinc-800 dark:bg-zinc-950">
        <div className="flex items-center justify-between border-b border-zinc-100 px-6 py-4 dark:border-zinc-800">
          <h2 className="text-sm font-medium uppercase tracking-wide text-zinc-500 dark:text-zinc-400">
            Map
          </h2>
          {data && (
            <a
              href={`https://www.openstreetmap.org/?mlat=${data.lat}&mlon=${data.lng}#map=15/${data.lat}/${data.lng}`}
              target="_blank"
              rel="noopener noreferrer"
              className="text-sm font-medium text-zinc-700 underline-offset-4 hover:underline dark:text-zinc-200"
            >
              Open in OpenStreetMap
            </a>
          )}
        </div>
        <div className="aspect-[16/10] w-full bg-zinc-100 dark:bg-zinc-900">
          {data ? (
            <iframe
              title="Device location"
              className="h-full w-full border-0"
              src={osmEmbedSrc(data.lat, data.lng)}
              loading="lazy"
              referrerPolicy="no-referrer-when-downgrade"
            />
          ) : (
            <div className="flex h-full items-center justify-center text-sm text-zinc-500 dark:text-zinc-400">
              Map appears after coordinates are received.
            </div>
          )}
        </div>
      </section>
    </div>
  );
}
