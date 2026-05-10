"use client";

import Link from "next/link";
import { useCallback, useEffect, useRef, useState } from "react";

type TelemetryRow = {
  lat: number;
  lng: number;
  uptimeSec: number;
  rssi: number | null;
  csq: number | null;
  updatedAt: string;
};

type ApiBody = {
  ok: boolean;
  mqttConfigured: boolean;
  pid?: number;
  data: TelemetryRow | null;
};

type DebugSnapshot = {
  lastFetchAt: string | null;
  lastHttpStatus: number | null;
  rawBody: string | null;
  fetchCount: number;
  lastFetchError: string | null;
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

/** Map cellular RSSI estimate (approx -113 + 2*CSQ dBm) to 0–100 */
function cellularRssiQualityPercent(rssi: number): number {
  const lo = -110;
  const hi = -51;
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
  const [debug, setDebug] = useState<DebugSnapshot>({
    lastFetchAt: null,
    lastHttpStatus: null,
    rawBody: null,
    fetchCount: 0,
    lastFetchError: null,
  });

  const fetchTelemetry = useCallback(async () => {
    const ts = new Date().toISOString();
    try {
      const res = await fetch(`/api/esp/telemetry?t=${Date.now()}`, {
        cache: "no-store",
        headers: { Accept: "application/json" },
      });
      const text = await res.text();

      let parsed: ApiBody;
      try {
        parsed = JSON.parse(text) as ApiBody;
      } catch {
        setDebug((d) => ({
          lastFetchAt: ts,
          lastHttpStatus: res.status,
          rawBody: text.slice(0, 12000),
          fetchCount: d.fetchCount + 1,
          lastFetchError: "Response was not valid JSON",
        }));
        setError(`Invalid JSON (HTTP ${res.status})`);
        return;
      }

      if (!res.ok) {
        setDebug((d) => ({
          lastFetchAt: ts,
          lastHttpStatus: res.status,
          rawBody: JSON.stringify(parsed, null, 2),
          fetchCount: d.fetchCount + 1,
          lastFetchError: `HTTP ${res.status}`,
        }));
        setError(`HTTP ${res.status}`);
        return;
      }

      setBody(parsed);
      setError(null);
      setDebug((d) => ({
        lastFetchAt: ts,
        lastHttpStatus: res.status,
        rawBody: JSON.stringify(parsed, null, 2),
        fetchCount: d.fetchCount + 1,
        lastFetchError: null,
      }));
    } catch (e) {
      const msg = e instanceof Error ? e.message : "Request failed";
      setError(msg);
      setDebug((d) => ({
        ...d,
        lastFetchAt: ts,
        fetchCount: d.fetchCount + 1,
        lastFetchError: msg,
      }));
    }
  }, []);

  useEffect(() => {
    const id = setInterval(() => void fetchTelemetry(), 4000);
    queueMicrotask(() => void fetchTelemetry());
    return () => clearInterval(id);
  }, [fetchTelemetry]);

  const data = body?.data ?? null;
  const mqttConfigured = body?.mqttConfigured ?? false;

  const [ledBusy, setLedBusy] = useState(false);
  const [ledMsg, setLedMsg] = useState<string | null>(null);

  type ModemReplyRow = {
    kind: string;
    ok: boolean;
    detail: string;
    receivedAt: string;
  };

  const [modemBusy, setModemBusy] = useState(false);
  const [modemStatus, setModemStatus] = useState<string | null>(null);
  const [modemReply, setModemReply] = useState<ModemReplyRow | null>(null);
  const [pendingRequestId, setPendingRequestId] = useState<string | null>(null);
  const [smsTo, setSmsTo] = useState("");
  const [smsText, setSmsText] = useState("");
  const pollAttempts = useRef(0);

  useEffect(() => {
    if (!pendingRequestId) {
      pollAttempts.current = 0;
      return;
    }

    const maxAttempts = 90;
    const id = window.setInterval(() => {
      void (async () => {
        pollAttempts.current += 1;
        if (pollAttempts.current > maxAttempts) {
          window.clearInterval(id);
          setPendingRequestId(null);
          setModemStatus("Timed out waiting for device reply (check MQTT / firmware).");
          return;
        }

        try {
          const res = await fetch(
            `/api/esp/modem/reply?requestId=${encodeURIComponent(pendingRequestId)}&t=${Date.now()}`,
            { cache: "no-store", headers: { Accept: "application/json" } },
          );
          const json = (await res.json()) as {
            reply?: ModemReplyRow | null;
          };
          if (json.reply) {
            window.clearInterval(id);
            setModemReply(json.reply);
            setPendingRequestId(null);
            setModemStatus(null);
          }
        } catch {
          /* keep polling */
        }
      })();
    }, 500);

    return () => window.clearInterval(id);
  }, [pendingRequestId]);

  const sendUssd = useCallback(async (code: string) => {
    setModemBusy(true);
    setModemStatus(null);
    setModemReply(null);
    try {
      const res = await fetch("/api/esp/modem/ussd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ code }),
      });
      const json = (await res.json()) as {
        ok?: boolean;
        requestId?: string;
        error?: string;
      };
      if (!res.ok || !json.ok || !json.requestId) {
        throw new Error(json.error ?? `HTTP ${res.status}`);
      }
      setPendingRequestId(json.requestId);
      setModemStatus("Waiting for modem…");
    } catch (e) {
      setModemStatus(e instanceof Error ? e.message : "USSD request failed");
    } finally {
      setModemBusy(false);
    }
  }, []);

  const sendSms = useCallback(async () => {
    setModemBusy(true);
    setModemStatus(null);
    setModemReply(null);
    try {
      const res = await fetch("/api/esp/modem/sms", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ to: smsTo.trim(), text: smsText }),
      });
      const json = (await res.json()) as {
        ok?: boolean;
        requestId?: string;
        error?: string;
      };
      if (!res.ok || !json.ok || !json.requestId) {
        throw new Error(json.error ?? `HTTP ${res.status}`);
      }
      setPendingRequestId(json.requestId);
      setModemStatus("Waiting for modem…");
    } catch (e) {
      setModemStatus(e instanceof Error ? e.message : "SMS request failed");
    } finally {
      setModemBusy(false);
    }
  }, [smsText, smsTo]);

  const sendLed = useCallback(async (on: boolean) => {
    setLedBusy(true);
    setLedMsg(null);
    try {
      const res = await fetch("/api/esp/led", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ on }),
      });
      const json = (await res.json()) as { ok?: boolean; error?: string };
      if (!res.ok || !json.ok) {
        throw new Error(json.error ?? `HTTP ${res.status}`);
      }
      setLedMsg(on ? "LED on command sent." : "LED off command sent.");
    } catch (e) {
      setLedMsg(e instanceof Error ? e.message : "LED command failed");
    } finally {
      setLedBusy(false);
    }
  }, []);

  return (
    <div className="mx-auto flex w-full max-w-4xl flex-col gap-8 px-4 py-10">
      <header className="flex flex-col gap-2 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <p className="text-sm text-zinc-500 dark:text-zinc-400">
            ESP controller
            {body?.pid != null ? (
              <span className="ml-2 font-mono text-xs opacity-70">
                (api pid {body.pid})
              </span>
            ) : null}
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

      <details className="rounded-2xl border border-zinc-200 bg-zinc-50/90 text-zinc-900 shadow-sm dark:border-zinc-700 dark:bg-zinc-900/50 dark:text-zinc-100">
        <summary className="flex cursor-pointer list-none items-center justify-between gap-3 px-4 py-3 text-sm font-medium outline-none marker:content-none [&::-webkit-details-marker]:hidden">
          <span>Debug</span>
          <span className="font-mono text-xs font-normal text-zinc-500 dark:text-zinc-400">
            polls {debug.fetchCount}
            {debug.lastHttpStatus != null ? ` · HTTP ${debug.lastHttpStatus}` : ""}
          </span>
        </summary>
        <div className="space-y-3 border-t border-zinc-200 px-4 py-3 text-sm dark:border-zinc-700">
          <dl className="grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 font-mono text-xs">
            <dt className="text-zinc-500 dark:text-zinc-400">Last fetch</dt>
            <dd>
              {debug.lastFetchAt
                ? new Date(debug.lastFetchAt).toLocaleString()
                : "-"}
            </dd>
            <dt className="text-zinc-500 dark:text-zinc-400">MQTT env</dt>
            <dd>{body?.mqttConfigured ?? false ? "configured" : "missing"}</dd>
            <dt className="text-zinc-500 dark:text-zinc-400">API pid</dt>
            <dd>{body?.pid ?? "-"}</dd>
            <dt className="text-zinc-500 dark:text-zinc-400">data</dt>
            <dd>{data ? "present" : "null"}</dd>
            <dt className="text-zinc-500 dark:text-zinc-400">Poll error</dt>
            <dd className="break-all">
              {debug.lastFetchError ?? "none"}
            </dd>
          </dl>

          {body?.mqttConfigured && !data && !debug.lastFetchError && (
            <p className="rounded-lg border border-amber-200/80 bg-amber-50/90 px-3 py-2 text-xs text-amber-950 dark:border-amber-900/50 dark:bg-amber-950/30 dark:text-amber-100">
              API is up and MQTT is configured, but <strong>data</strong> is still{" "}
              <strong>null</strong>. The server has not stored a telemetry payload yet:
              confirm the device publishes JSON to the same topic as{" "}
              <code className="rounded bg-amber-100/80 px-1 dark:bg-amber-900/60">
                MQTT_TOPIC_TELEMETRY
              </code>{" "}
              and that Docker logs show{" "}
              <code className="rounded bg-amber-100/80 px-1 dark:bg-amber-900/60">
                [mqtt] telemetry rx
              </code>
              .
            </p>
          )}

          <div className="flex flex-wrap gap-2">
            <a
              href="/api/esp/telemetry"
              target="_blank"
              rel="noopener noreferrer"
              className="rounded-lg border border-zinc-300 bg-white px-2 py-1 text-xs font-medium text-zinc-800 hover:bg-zinc-50 dark:border-zinc-600 dark:bg-zinc-800 dark:text-zinc-100 dark:hover:bg-zinc-700"
            >
              Open /api/esp/telemetry
            </a>
            <a
              href="/api/esp/config"
              target="_blank"
              rel="noopener noreferrer"
              className="rounded-lg border border-zinc-300 bg-white px-2 py-1 text-xs font-medium text-zinc-800 hover:bg-zinc-50 dark:border-zinc-600 dark:bg-zinc-800 dark:text-zinc-100 dark:hover:bg-zinc-700"
            >
              Open /api/esp/config
            </a>
            {debug.rawBody ? (
              <button
                type="button"
                className="rounded-lg border border-zinc-300 bg-white px-2 py-1 text-xs font-medium text-zinc-800 hover:bg-zinc-50 dark:border-zinc-600 dark:bg-zinc-800 dark:text-zinc-100 dark:hover:bg-zinc-700"
                onClick={() =>
                  void navigator.clipboard?.writeText(debug.rawBody ?? "")
                }
              >
                Copy JSON
              </button>
            ) : null}
          </div>

          {debug.rawBody ? (
            <pre className="max-h-56 overflow-auto rounded-lg border border-zinc-200 bg-white p-3 text-xs leading-relaxed text-zinc-800 dark:border-zinc-700 dark:bg-zinc-950 dark:text-zinc-200">
              {debug.rawBody}
            </pre>
          ) : (
            <p className="text-xs text-zinc-500 dark:text-zinc-400">
              No response body captured yet.
            </p>
          )}
        </div>
      </details>

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
              Waiting for the first MQTT message...
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
                  Cellular signal (CSQ / RSSI est.)
                </dt>
                {data.csq !== null && data.csq !== undefined ? (
                  <>
                    <dd className="mt-1 font-mono text-lg">
                      CSQ {data.csq}
                      {data.csq >= 0 && data.csq <= 31
                        ? ` (~${-113 + 2 * data.csq} dBm est.)`
                        : data.csq === 99
                          ? " (unknown / not measured)"
                          : ""}
                    </dd>
                    {data.rssi !== null ? (
                      <>
                        <div
                          className="mt-3 h-2 w-full overflow-hidden rounded-full bg-zinc-200 dark:bg-zinc-800"
                          aria-label="Estimated cellular signal strength"
                        >
                          <div
                            className="h-full rounded-full bg-emerald-500 transition-[width] duration-500 dark:bg-emerald-400"
                            style={{
                              width: `${cellularRssiQualityPercent(data.rssi)}%`,
                            }}
                          />
                        </div>
                        <dd className="mt-1 text-xs text-zinc-500 dark:text-zinc-400">
                          Approx. quality:{" "}
                          {cellularRssiQualityPercent(data.rssi)}%
                        </dd>
                      </>
                    ) : (
                      <dd className="mt-2 text-sm text-zinc-600 dark:text-zinc-400">
                        RSSI estimate unavailable until CSQ is 0–31 (run AT+CSQ /
                        wait for registration).
                      </dd>
                    )}
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

      <section className="rounded-2xl border border-zinc-200 bg-white p-6 shadow-sm dark:border-zinc-800 dark:bg-zinc-950">
        <h2 className="text-sm font-medium uppercase tracking-wide text-zinc-500 dark:text-zinc-400">
          Modem &amp; SIM (USSD / SMS)
        </h2>
        <p className="mt-2 max-w-xl text-sm text-zinc-600 dark:text-zinc-400">
          Commands go to the ESP over MQTT; the device runs AT+CUSD / AT+CMGS and
          publishes a JSON reply to{" "}
          <code className="rounded bg-zinc-100 px-1 font-mono text-xs dark:bg-zinc-800">
            MQTT_TOPIC_REPLIES
          </code>
          . Only one job runs at a time on the device.
        </p>

        <div className="mt-4 flex flex-wrap gap-2">
          <button
            type="button"
            disabled={modemBusy || !mqttConfigured || pendingRequestId !== null}
            onClick={() => void sendUssd("*310#")}
            className="rounded-xl border border-zinc-300 bg-white px-4 py-2 text-sm font-medium text-zinc-800 transition-colors hover:bg-zinc-50 disabled:cursor-not-allowed disabled:opacity-40 dark:border-zinc-600 dark:bg-zinc-900 dark:text-zinc-100 dark:hover:bg-zinc-800"
          >
            Airtime balance (*310#)
          </button>
          <button
            type="button"
            disabled={modemBusy || !mqttConfigured || pendingRequestId !== null}
            onClick={() => void sendUssd("*312#")}
            className="rounded-xl border border-zinc-300 bg-white px-4 py-2 text-sm font-medium text-zinc-800 transition-colors hover:bg-zinc-50 disabled:cursor-not-allowed disabled:opacity-40 dark:border-zinc-600 dark:bg-zinc-900 dark:text-zinc-100 dark:hover:bg-zinc-800"
          >
            Data balance (*312#)
          </button>
        </div>

        <div className="mt-6 grid gap-3 sm:grid-cols-[1fr_2fr] sm:items-end">
          <label className="flex flex-col gap-1 text-sm">
            <span className="text-zinc-500 dark:text-zinc-400">SMS to</span>
            <input
              type="tel"
              value={smsTo}
              onChange={(e) => setSmsTo(e.target.value)}
              placeholder="+234…"
              className="rounded-lg border border-zinc-300 bg-white px-3 py-2 font-mono text-sm dark:border-zinc-600 dark:bg-zinc-900"
            />
          </label>
          <label className="flex flex-col gap-1 text-sm sm:col-span-2">
            <span className="text-zinc-500 dark:text-zinc-400">Message</span>
            <textarea
              value={smsText}
              onChange={(e) => setSmsText(e.target.value)}
              rows={3}
              className="rounded-lg border border-zinc-300 bg-white px-3 py-2 text-sm dark:border-zinc-600 dark:bg-zinc-900"
            />
          </label>
          <button
            type="button"
            disabled={
              modemBusy ||
              !mqttConfigured ||
              pendingRequestId !== null ||
              smsTo.trim().length < 5
            }
            onClick={() => void sendSms()}
            className="rounded-xl bg-zinc-800 px-4 py-2 text-sm font-medium text-white transition-opacity hover:opacity-90 disabled:cursor-not-allowed disabled:opacity-40 dark:bg-zinc-200 dark:text-zinc-900"
          >
            Send SMS
          </button>
        </div>

        {modemStatus && (
          <p className="mt-3 text-sm text-zinc-600 dark:text-zinc-400">
            {modemStatus}
          </p>
        )}
        {modemReply && (
          <div className="mt-4 rounded-lg border border-zinc-200 bg-zinc-50 px-3 py-2 text-sm dark:border-zinc-700 dark:bg-zinc-900/60">
            <p className="font-medium text-zinc-800 dark:text-zinc-100">
              {modemReply.ok ? "OK" : "Failed"} · {modemReply.kind}
            </p>
            <pre className="mt-2 max-h-40 overflow-auto whitespace-pre-wrap break-words font-mono text-xs text-zinc-700 dark:text-zinc-300">
              {modemReply.detail}
            </pre>
            <p className="mt-2 text-xs text-zinc-500 dark:text-zinc-400">
              {new Date(modemReply.receivedAt).toLocaleString()}
            </p>
          </div>
        )}
      </section>

      <section className="rounded-2xl border border-zinc-200 bg-white p-6 shadow-sm dark:border-zinc-800 dark:bg-zinc-950">
        <h2 className="text-sm font-medium uppercase tracking-wide text-zinc-500 dark:text-zinc-400">
          Remote LED (MQTT)
        </h2>
        <p className="mt-2 max-w-xl text-sm text-zinc-600 dark:text-zinc-400">
          Sends{" "}
          <code className="rounded bg-zinc-100 px-1 font-mono text-xs dark:bg-zinc-800">
            {"{ \"cmd\": \"led\", \"on\": true|false }"}
          </code>{" "}
          on the commands topic. The ESP32 firmware must subscribe and drive
          GPIO.
        </p>
        <div className="mt-4 flex flex-wrap gap-3">
          <button
            type="button"
            disabled={ledBusy || !mqttConfigured}
            onClick={() => void sendLed(true)}
            className="rounded-xl bg-emerald-600 px-4 py-2 text-sm font-medium text-white transition-opacity hover:opacity-90 disabled:cursor-not-allowed disabled:opacity-40"
          >
            LED on
          </button>
          <button
            type="button"
            disabled={ledBusy || !mqttConfigured}
            onClick={() => void sendLed(false)}
            className="rounded-xl border border-zinc-300 bg-white px-4 py-2 text-sm font-medium text-zinc-800 transition-colors hover:bg-zinc-50 disabled:cursor-not-allowed disabled:opacity-40 dark:border-zinc-600 dark:bg-zinc-900 dark:text-zinc-100 dark:hover:bg-zinc-800"
          >
            LED off
          </button>
        </div>
        {ledMsg && (
          <p className="mt-3 text-sm text-zinc-600 dark:text-zinc-400">
            {ledMsg}
          </p>
        )}
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
