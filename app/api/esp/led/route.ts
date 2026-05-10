import { NextResponse } from "next/server";

import {
  ensureMqttApp,
  getCommandsTopic,
  mqttPublishJson,
} from "@/lib/mqtt-app";

export const runtime = "nodejs";

/**
 * Publishes `{ "cmd": "led", "on": boolean }` to the MQTT commands topic.
 * The ESP must subscribe to that topic (see GET /api/esp/config).
 */
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

  const on = (body as Record<string, unknown>).on;
  if (typeof on !== "boolean") {
    return NextResponse.json(
      { ok: false, error: 'Body must include boolean property "on"' },
      { status: 400 },
    );
  }

  const topic = getCommandsTopic();
  const payload = { cmd: "led" as const, on };

  try {
    await mqttPublishJson(topic, payload);
    return NextResponse.json({ ok: true, topic, mqtt: payload });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "MQTT publish failed";
    return NextResponse.json({ ok: false, error: msg }, { status: 503 });
  }
}
