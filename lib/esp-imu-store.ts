/**
 * Fused IMU orientation from MQTT (Madgwick quaternion on the device).
 * Default topic: device/imu/orientation — see docs/WEB_APP_IMU_ORIENTATION_THREEJS.md
 */

export type EspImuOrientation = {
  qw: number;
  qx: number;
  qy: number;
  qz: number;
  updatedAt: string;
};

let latest: EspImuOrientation | null = null;

const listeners = new Set<(json: string) => void>();

function isFiniteNum(v: unknown): v is number {
  return typeof v === "number" && Number.isFinite(v);
}

function validateOrientation(
  data: unknown,
): Omit<EspImuOrientation, "updatedAt"> | null {
  if (!data || typeof data !== "object") return null;
  const o = data as Record<string, unknown>;
  const qw = Number(o.qw);
  const qx = Number(o.qx);
  const qy = Number(o.qy);
  const qz = Number(o.qz);
  if (!isFiniteNum(qw) || !isFiniteNum(qx) || !isFiniteNum(qy) || !isFiniteNum(qz)) {
    return null;
  }
  const n = Math.hypot(qw, qx, qy, qz);
  if (n < 0.25 || n > 4.0) return null;
  return { qw, qx, qy, qz };
}

export function applyEspImuOrientationPayload(data: unknown): void {
  const row = validateOrientation(data);
  if (!row) return;
  const snapshot: EspImuOrientation = {
    ...row,
    updatedAt: new Date().toISOString(),
  };
  latest = snapshot;
  const json = JSON.stringify(snapshot);
  for (const l of listeners) {
    try {
      l(json);
    } catch {
      /* ignore subscriber errors */
    }
  }
}

export function getLatestEspImuOrientation(): EspImuOrientation | null {
  return latest;
}

export function subscribeEspImuOrientation(
  listener: (json: string) => void,
): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}
