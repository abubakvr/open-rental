import { NextResponse } from "next/server";

import {
  getCommandsTopic,
  ensureMqttApp,
  mqttPublishJson,
} from "@/lib/mqtt-app";

export const runtime = "nodejs";

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
  const payload = o.payload ?? o.message ?? o.data;
  if (payload === undefined) {
    return NextResponse.json(
      { ok: false, error: "Missing payload (use { \"payload\": { ... } })" },
      { status: 400 },
    );
  }

  const topicOverride =
    typeof o.topic === "string" && o.topic.trim() ? o.topic.trim() : null;
  const topic = topicOverride ?? getCommandsTopic();

  try {
    await mqttPublishJson(topic, payload);
    return NextResponse.json({ ok: true, topic });
  } catch (e) {
    const msg = e instanceof Error ? e.message : "Publish failed";
    return NextResponse.json({ ok: false, error: msg }, { status: 503 });
  }
}
