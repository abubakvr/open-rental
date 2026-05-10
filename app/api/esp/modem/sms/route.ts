import { randomUUID } from "crypto";
import { NextResponse } from "next/server";

import {
  ensureMqttApp,
  getCommandsTopic,
  mqttPublishJson,
} from "@/lib/mqtt-app";

export const runtime = "nodejs";

function safePhone(raw: unknown): string | null {
  if (typeof raw !== "string") return null;
  const t = raw.trim();
  if (t.length < 5 || t.length > 24) return null;
  if (!/^\+?[0-9]+$/.test(t)) return null;
  return t;
}

function safeSmsText(raw: unknown): string | null {
  if (typeof raw !== "string") return null;
  const t = raw.trim();
  if (t.length === 0 || t.length > 320) return null;
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
  const to = safePhone(o.to);
  const text = safeSmsText(o.text);
  if (!to || !text) {
    return NextResponse.json(
      {
        ok: false,
        error:
          'Invalid "to" (5–24 digits, optional +) or "text" (1–320 chars after trim)',
      },
      { status: 400 },
    );
  }

  let requestId =
    typeof o.requestId === "string" && o.requestId.trim().length > 0
      ? o.requestId.trim().slice(0, 64)
      : randomUUID();

  const topic = getCommandsTopic();
  const payload = { cmd: "sms" as const, to, text, requestId };

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
