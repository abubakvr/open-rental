/**
 * Optional raw IMU stream (ax,ay,az in g; gx,gy,gz in °/s).
 * Default topic: device/imu/raw — only used if your firmware publishes there.
 */

export type EspImuRaw = {
  ax: number;
  ay: number;
  az: number;
  gx: number;
  gy: number;
  gz: number;
  updatedAt: string;
};

let latest: EspImuRaw | null = null;

function isFiniteNum(v: unknown): v is number {
  return typeof v === "number" && Number.isFinite(v);
}

export function applyEspImuRawPayload(data: unknown): void {
  if (!data || typeof data !== "object") return;
  const o = data as Record<string, unknown>;
  const ax = Number(o.ax);
  const ay = Number(o.ay);
  const az = Number(o.az);
  const gx = Number(o.gx);
  const gy = Number(o.gy);
  const gz = Number(o.gz);
  if (
    !isFiniteNum(ax) ||
    !isFiniteNum(ay) ||
    !isFiniteNum(az) ||
    !isFiniteNum(gx) ||
    !isFiniteNum(gy) ||
    !isFiniteNum(gz)
  ) {
    return;
  }
  latest = {
    ax,
    ay,
    az,
    gx,
    gy,
    gz,
    updatedAt: new Date().toISOString(),
  };
}

export function getLatestEspImuRaw(): EspImuRaw | null {
  return latest;
}
