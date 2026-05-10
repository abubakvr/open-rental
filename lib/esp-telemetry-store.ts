export type EspTelemetry = {
  lat: number;
  lng: number;
  uptimeSec: number;
  /** Estimated RSSI in dBm when CSQ is known; null when unknown */
  rssi: number | null;
  /** Raw GSM CSQ 0–31, or 99 when unknown (see AT+CSQ) */
  csq: number | null;
  updatedAt: string;
};

let latest: EspTelemetry | null = null;

export function applyEspTelemetryPayload(data: unknown): void {
  if (!data || typeof data !== "object") return;
  const o = data as Record<string, unknown>;
  const lat = Number(o.lat ?? o.latitude);
  const lng = Number(o.lng ?? o.lon ?? o.longitude);
  if (!Number.isFinite(lat) || !Number.isFinite(lng)) return;

  const uptimeRaw = o.uptimeSec ?? o.uptime ?? o.uptime_seconds;
  const uptimeNum = Number(uptimeRaw);
  const uptimeSec = Number.isFinite(uptimeNum)
    ? Math.max(0, Math.floor(uptimeNum))
    : 0;

  let csq: number | null = null;
  const csqRaw = o.csq ?? o.csqRaw ?? o.signalQuality;
  if (csqRaw !== undefined && csqRaw !== null) {
    const n = Number(csqRaw);
    if (Number.isFinite(n)) csq = Math.floor(n);
  }

  let rssi: number | null = null;
  if (o.rssi === null || o.rssi === undefined) {
    rssi = null;
  } else {
    const n = Number(o.rssi);
    if (Number.isFinite(n)) rssi = n;
  }

  if (rssi === null && csq !== null && csq >= 0 && csq <= 31) {
    rssi = -113 + 2 * csq;
  }

  latest = {
    lat,
    lng,
    uptimeSec,
    rssi,
    csq,
    updatedAt: new Date().toISOString(),
  };
}

export function getLatestEspTelemetry(): EspTelemetry | null {
  return latest;
}
