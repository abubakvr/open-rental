export type ModemReply = {
  requestId: string;
  kind: string;
  ok: boolean;
  detail: string;
  receivedAt: string;
};

const byId = new Map<string, ModemReply>();
const MAX_ENTRIES = 64;

export function applyModemReplyPayload(data: unknown): void {
  if (!data || typeof data !== "object") return;
  const o = data as Record<string, unknown>;
  const requestId = String(o.requestId ?? "").trim();
  if (!requestId) return;

  const kind = String(o.kind ?? "unknown");
  const ok = Boolean(o.ok);
  const detail =
    typeof o.detail === "string"
      ? o.detail
      : o.detail != null
        ? JSON.stringify(o.detail)
        : "";

  const row: ModemReply = {
    requestId,
    kind,
    ok,
    detail,
    receivedAt: new Date().toISOString(),
  };

  byId.set(requestId, row);
  while (byId.size > MAX_ENTRIES) {
    const first = byId.keys().next().value;
    if (first) byId.delete(first);
    else break;
  }
}

export function getModemReply(requestId: string): ModemReply | null {
  return byId.get(requestId) ?? null;
}
