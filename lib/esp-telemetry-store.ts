export type EspTelemetry = {
  lat: number;
  lng: number;
  uptimeSec: number;
  rssi: number | null;
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

  const rssiRaw = o.rssi ?? o.signal ?? o.wifiRssi ?? o.signalStrength;
  let rssi: number | null = null;
  if (rssiRaw !== undefined && rssiRaw !== null) {
    const n = Number(rssiRaw);
    if (Number.isFinite(n)) rssi = n;
  }

  latest = {
    lat,
    lng,
    uptimeSec,
    rssi,
    updatedAt: new Date().toISOString(),
  };
}

export function getLatestEspTelemetry(): EspTelemetry | null {
  return latest;
}
