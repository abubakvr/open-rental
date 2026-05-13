export type EspImuSnapshot = {
  ax: number;
  ay: number;
  az: number;
  aMag: number;
  gx: number;
  gy: number;
  gz: number;
  rax: number;
  ray: number;
  raz: number;
  rgx: number;
  rgy: number;
  rgz: number;
  updatedAt: string;
};

let latest: EspImuSnapshot | null = null;

const listeners = new Set<(json: string) => void>();

function isFiniteNum(v: unknown): v is number {
  return typeof v === "number" && Number.isFinite(v);
}

function validateImu(data: unknown): Omit<EspImuSnapshot, "updatedAt"> | null {
  if (!data || typeof data !== "object") return null;
  const o = data as Record<string, unknown>;
  const keys: (keyof Omit<EspImuSnapshot, "updatedAt">)[] = [
    "ax",
    "ay",
    "az",
    "aMag",
    "gx",
    "gy",
    "gz",
    "rax",
    "ray",
    "raz",
    "rgx",
    "rgy",
    "rgz",
  ];
  const out: Partial<Omit<EspImuSnapshot, "updatedAt">> = {};
  for (const k of keys) {
    const n = Number(o[k]);
    if (!isFiniteNum(n)) return null;
    out[k] = n;
  }
  return out as Omit<EspImuSnapshot, "updatedAt">;
}

export function applyEspImuPayload(data: unknown): void {
  const row = validateImu(data);
  if (!row) return;
  const snapshot: EspImuSnapshot = {
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

export function getLatestEspImu(): EspImuSnapshot | null {
  return latest;
}

/** Subscribe to new IMU rows (JSON string per MQTT message). Returns unsubscribe. */
export function subscribeEspImu(listener: (json: string) => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}
