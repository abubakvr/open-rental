import { randomUUID } from "crypto";
import { NextResponse } from "next/server";

import {
  ensureMqttApp,
  getCommandsTopic,
  mqttPublishJson,
} from "@/lib/mqtt-app";

export const runtime = "nodejs";

function safeUssdCode(raw: unknown): string | null {
  if (typeof raw !== "string") return null;
  const t = raw.trim();
  if (t.length === 0 || t.length > 40) return null;
  if (!/^[\d\*#]+$/.test(t)) return null;
  return t;
}

export async function POST(req: Request) {
  ensureMqttApp();

  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return NextResponse.json(
      { ok: false, error: "Invalid JSON body" },
      { status: 400 },
    );
  }

  if (!body || typeof body !== "object") {
    return NextResponse.json(
      { ok: false, error: "Expected a JSON object" },
      { status: 400 },
    );
  }

  const o = body as Record<string, unknown>;
  const code = safeUssdCode(o.code);
  if (!code) {
    return NextResponse.json(
      {
        ok: false,
        error:
          'Invalid or missing "code" (non-empty string, digits * # only, max 40 chars)',
      },
      { status: 400 },
    );
  }

  let requestId =
    typeof o.requestId === "string" && o.requestId.trim().length > 0
      ? o.requestId.trim().slice(0, 64)
      : randomUUID();

  const topic = getCommandsTopic();
  const payload = { cmd: "ussd" as const, code, requestId };

  try {
    await mqttPublishJson(topic, payload);
    return NextResponse.json({
      ok: true,
      topic,
      requestId,
      mqtt: payload,
    });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "MQTT publish failed";
    return NextResponse.json({ ok: false, error: msg }, { status: 503 });
  }
}
