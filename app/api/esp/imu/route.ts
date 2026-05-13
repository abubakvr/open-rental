import { NextResponse } from "next/server";

import { ensureMqttApp, getImuTopic } from "@/lib/mqtt-app";
import { getLatestEspImu } from "@/lib/esp-imu-store";

export const runtime = "nodejs";

export async function GET() {
  ensureMqttApp();
  const data = getLatestEspImu();
  return NextResponse.json(
    {
      ok: true,
      mqttConfigured: Boolean(process.env.MQTT_URL),
      topic: getImuTopic(),
      pid: process.pid,
      data,
    },
    {
      headers: {
        "Cache-Control": "no-store, no-cache, must-revalidate",
        Pragma: "no-cache",
      },
    },
  );
}
