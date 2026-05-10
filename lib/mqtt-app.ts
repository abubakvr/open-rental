import mqtt, { type MqttClient } from "mqtt";

import { applyEspTelemetryPayload } from "@/lib/esp-telemetry-store";

type GlobalMqtt = typeof globalThis & {
  __mqttAppStarted?: boolean;
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

let client: MqttClient | null = null;

export function ensureMqttApp(): void {
  const g = globalThis as GlobalMqtt;
  if (g.__mqttAppStarted) return;
  g.__mqttAppStarted = true;

  const url = process.env.MQTT_URL?.trim();
  if (!url) {
    console.warn("[mqtt] MQTT_URL is not set; broker features are disabled.");
    return;
  }

  const user = process.env.MQTT_USER?.trim();
  const password = process.env.MQTT_PASSWORD?.trim();

  client = mqtt.connect(url, {
    username: user || undefined,
    password: password || undefined,
    reconnectPeriod: 5_000,
    connectTimeout: 30_000,
  });

  const topic = getTelemetryTopic();

  client.on("connect", () => {
    client!.subscribe(topic, (err) => {
      if (err) console.error("[mqtt] subscribe failed:", err);
      else console.info("[mqtt] subscribed:", topic);
    });
  });

  client.on("message", (_receivedTopic, payload) => {
    try {
      applyEspTelemetryPayload(JSON.parse(payload.toString()));
    } catch (e) {
      console.error("[mqtt] telemetry parse error:", e);
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
