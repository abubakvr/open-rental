import { NextResponse } from "next/server";

import { ensureMqttApp } from "@/lib/mqtt-app";
import { getLatestEspTelemetry } from "@/lib/esp-telemetry-store";

export const runtime = "nodejs";

export async function GET() {
  ensureMqttApp();
  const data = getLatestEspTelemetry();
  return NextResponse.json({
    ok: true,
    mqttConfigured: Boolean(process.env.MQTT_URL),
    data,
  });
}
