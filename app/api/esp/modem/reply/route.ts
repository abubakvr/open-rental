import { NextResponse } from "next/server";

import { ensureMqttApp } from "@/lib/mqtt-app";
import { getModemReply } from "@/lib/modem-reply-store";

export const runtime = "nodejs";

export async function GET(req: Request) {
  ensureMqttApp();

  const url = new URL(req.url);
  const requestId = url.searchParams.get("requestId")?.trim() ?? "";
  if (!requestId) {
    return NextResponse.json(
      { ok: false, error: "Missing query parameter requestId" },
      { status: 400 },
    );
  }

  const reply = getModemReply(requestId);
  return NextResponse.json(
    {
      ok: true,
      mqttConfigured: Boolean(process.env.MQTT_URL),
      pid: process.pid,
      requestId,
      reply,
    },
    {
      headers: {
        "Cache-Control": "no-store, no-cache, must-revalidate",
        Pragma: "no-cache",
      },
    },
  );
}
