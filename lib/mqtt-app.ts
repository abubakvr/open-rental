import mqtt, { type MqttClient } from "mqtt";

import { applyEspImuOrientationPayload } from "@/lib/esp-imu-store";
import { applyEspImuRawPayload } from "@/lib/esp-imu-raw-store";
import { applyEspTelemetryPayload } from "@/lib/esp-telemetry-store";
import { applyModemReplyPayload } from "@/lib/modem-reply-store";

type GlobalMqtt = typeof globalThis & {
  __mqttAppStarted?: boolean;
  __mqttNoUrlWarned?: boolean;
};

export function getTelemetryTopic(): string {
  return (
    process.env.MQTT_TOPIC_TELEMETRY?.trim() ||
    process.env.MQTT_TOPIC?.trim() ||
    "open-rental/esp/telemetry"
  );
}

export function getCommandsTopic(): string {
  return (
    process.env.MQTT_TOPIC_COMMANDS?.trim() || "open-rental/esp/commands"
  );
}

export function getRepliesTopic(): string {
  return (
    process.env.MQTT_TOPIC_REPLIES?.trim() || "open-rental/esp/replies"
  );
}

/** Fused quaternion JSON: qw,qx,qy,qz (Hamilton). */
export function getImuOrientationTopic(): string {
  return (
    process.env.MQTT_TOPIC_IMU_ORIENTATION?.trim() ||
    process.env.MQTT_TOPIC_IMU?.trim() ||
    "device/imu/orientation"
  );
}

/** Optional raw accel (g) + gyro (°/s). */
export function getImuRawTopic(): string {
  return (
    process.env.MQTT_TOPIC_IMU_RAW?.trim() || "device/imu/raw"
  );
}

let client: MqttClient | null = null;

function buildSubscribeMap(): Record<string, { qos: 0 | 1 | 2 }> {
  const telemetryTopic = getTelemetryTopic();
  const repliesTopic = getRepliesTopic();
  const imuOrient = getImuOrientationTopic();
  const imuRaw = getImuRawTopic();

  const m: Record<string, { qos: 0 | 1 | 2 }> = {
    [telemetryTopic]: { qos: 1 },
    [repliesTopic]: { qos: 1 },
    [imuOrient]: { qos: 0 },
  };
  if (imuRaw !== imuOrient) {
    m[imuRaw] = { qos: 0 };
  }
  return m;
}

export function ensureMqttApp(): void {
  const g = globalThis as GlobalMqtt;
  if (g.__mqttAppStarted) return;

  const url = process.env.MQTT_URL?.trim();
  if (!url) {
    if (!g.__mqttNoUrlWarned) {
      g.__mqttNoUrlWarned = true;
      console.warn("[mqtt] MQTT_URL is not set; broker features are disabled.");
    }
    return;
  }

  g.__mqttAppStarted = true;

  const user = process.env.MQTT_USER?.trim();
  const password = process.env.MQTT_PASSWORD?.trim();

  client = mqtt.connect(url, {
    protocolVersion: 4,
    username: user || undefined,
    password: password || undefined,
    reconnectPeriod: 5_000,
    connectTimeout: 30_000,
    clean: true,
    resubscribe: true,
  });

  const imuOrient = getImuOrientationTopic();
  const imuRaw = getImuRawTopic();

  client.on("connect", () => {
    const subs = buildSubscribeMap();
    console.info("[mqtt] connected pid=%s", process.pid);
    client!.subscribe(subs, (err) => {
      if (err) console.error("[mqtt] subscribe failed:", err);
      else
        console.info(
          "[mqtt] subscribed pid=%s topics=%s",
          process.pid,
          Object.keys(subs).join(", "),
        );
    });
  });

  client.on("message", (receivedTopic, payload) => {
    try {
      const raw = payload.toString().trim();
      const data = JSON.parse(raw) as unknown;
      const row = JSON.stringify(data);

      if (receivedTopic === getRepliesTopic()) {
        applyModemReplyPayload(data);
        console.info(
          "[mqtt] reply rx topic=%s pid=%s bytes=%d sample=%s",
          receivedTopic,
          process.pid,
          raw.length,
          row.length > 120 ? `${row.slice(0, 120)}…` : row,
        );
        return;
      }

      if (receivedTopic === imuOrient) {
        applyEspImuOrientationPayload(data);
        console.info(
          "[mqtt] imu/orientation rx pid=%s bytes=%d sample=%s",
          process.pid,
          raw.length,
          row.length > 120 ? `${row.slice(0, 120)}…` : row,
        );
        return;
      }

      if (receivedTopic === imuRaw) {
        applyEspImuRawPayload(data);
        console.info(
          "[mqtt] imu/raw rx pid=%s bytes=%d",
          process.pid,
          raw.length,
        );
        return;
      }

      applyEspTelemetryPayload(data);
      console.info(
        "[mqtt] telemetry rx topic=%s pid=%s bytes=%d sample=%s",
        receivedTopic,
        process.pid,
        raw.length,
        row.length > 120 ? `${row.slice(0, 120)}…` : row,
      );
    } catch (e) {
      console.error("[mqtt] message parse error:", e);
    }
  });

  client.on("error", (err) => console.error("[mqtt]", err));
  client.on("reconnect", () => console.info("[mqtt] reconnecting…"));
}

function getClient(): MqttClient | null {
  ensureMqttApp();
  return client;
}

function waitUntilConnected(c: MqttClient, ms: number): Promise<void> {
  if (c.connected) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      cleanup();
      reject(new Error("MQTT connection timeout"));
    }, ms);
    const onConnect = () => {
      cleanup();
      resolve();
    };
    const onError = (e: Error) => {
      cleanup();
      reject(e);
    };
    function cleanup() {
      clearTimeout(timer);
      c.off("connect", onConnect);
      c.off("error", onError);
    }
    c.once("connect", onConnect);
    c.once("error", onError);
  });
}

export async function mqttPublishJson(
  topic: string,
  payload: unknown,
): Promise<void> {
  const c = getClient();
  if (!c) {
    throw new Error("MQTT_URL is not configured");
  }
  await waitUntilConnected(c, 15_000);
  await new Promise<void>((resolve, reject) => {
    c.publish(topic, JSON.stringify(payload), { qos: 0 }, (err) => {
      if (err) reject(err);
      else resolve();
    });
  });
}
