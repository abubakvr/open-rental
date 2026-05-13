import { NextResponse } from "next/server";

import {
  getCommandsTopic,
  getImuTopic,
  getRepliesTopic,
  getTelemetryTopic,
} from "@/lib/mqtt-app";

export const runtime = "nodejs";

/**
 * Static hints for firmware / provisioning: topic names and HTTP paths.
 * Broker hostname is not included — the ESP must use your LAN IP or DNS (see README).
 */
export async function GET() {
  return NextResponse.json({
    ok: true,
    mqtt: {
      telemetryTopic: getTelemetryTopic(),
      imuTopic: getImuTopic(),
      commandsTopic: getCommandsTopic(),
      repliesTopic: getRepliesTopic(),
      port: 1883,
      note:
        "ESP connects to the broker at <YOUR_HOST>:1883 (same Wi‑Fi as the broker). Inside Docker Compose the app uses mqtt://mosquitto:1883; devices use your machine/router IP.",
    },
    http: {
      baseUrlHint:
        process.env.NEXT_PUBLIC_APP_URL?.trim() ||
        "Set NEXT_PUBLIC_APP_URL (e.g. http://192.168.1.10:3909) for absolute URLs in clients.",
      getTelemetry: "/api/esp/telemetry",
      getImu: "/api/esp/imu",
      getImuStream: "/api/esp/imu/stream",
      postPublish: "/api/esp/publish",
      postLed: "/api/esp/led",
      postModemUssd: "/api/esp/modem/ussd",
      postModemSms: "/api/esp/modem/sms",
      getModemReply: "/api/esp/modem/reply?requestId=",
      ledPayloadHint: '{ "on": true | false } publishes { cmd: "led", on } to commandsTopic',
      modemPayloadHint:
        'USSD: { cmd: "ussd", code: "*310#", requestId }; SMS: { cmd: "sms", to, text, requestId }; ESP publishes reply JSON to mqtt.repliesTopic',
    },
  });
}
