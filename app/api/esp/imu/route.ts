import { NextResponse } from "next/server";

import {
  ensureMqttApp,
  getImuOrientationTopic,
  getImuRawTopic,
} from "@/lib/mqtt-app";
import { getLatestEspImuOrientation } from "@/lib/esp-imu-store";
import { getLatestEspImuRaw } from "@/lib/esp-imu-raw-store";

export const runtime = "nodejs";

export async function GET() {
  ensureMqttApp();
  return NextResponse.json(
    {
      ok: true,
      mqttConfigured: Boolean(process.env.MQTT_URL),
      topicOrientation: getImuOrientationTopic(),
      topicRaw: getImuRawTopic(),
      pid: process.pid,
      data: getLatestEspImuOrientation(),
      raw: getLatestEspImuRaw(),
    },
    {
      headers: {
        "Cache-Control": "no-store, no-cache, must-revalidate",
        Pragma: "no-cache",
      },
    },
  );
}
