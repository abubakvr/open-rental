# IMU orientation MQTT and Three.js 3D visualization

This Open Rental **web** stack consumes **fused orientation** (Madgwick quaternion) from your firmware repo over MQTT and drives `/dashboard/imu` with **Three.js**.

## Topics (defaults)

| Purpose | Default topic | Env override |
|--------|----------------|--------------|
| Orientation `qw,qx,qy,qz` | `device/imu/orientation` | `MQTT_TOPIC_IMU_ORIENTATION` |
| Optional raw `ax,ay,az,gx,gy,gz` | `device/imu/raw` | `MQTT_TOPIC_IMU_RAW` |

Legacy: `MQTT_TOPIC_IMU` still maps to the **orientation** topic if `MQTT_TOPIC_IMU_ORIENTATION` is unset (for older `.env` files that pointed `open-rental/esp/imu` at a single JSON stream).

## Orientation JSON

```json
{"qw":0.99812,"qx":0.00123,"qy":-0.04567,"qz":0.03210}
```

The Next.js server subscribes with the `mqtt` package, keeps the latest sample, and exposes:

- `GET /api/esp/imu` — JSON: `data` (orientation), `raw` (last raw if published), `topicOrientation`, `topicRaw`
- `GET /api/esp/imu/stream` — Server-Sent Events, **orientation** only (low latency for the figure)

## Three.js

The client applies `THREE.Quaternion(qx, qy, qz, qw)`, normalizes, multiplies by an **offset** quaternion when you click **Zero orientation** (rest pose calibration), and **slerps** each frame so short MQTT gaps do not pop the mesh.

## Broker connectivity

Browsers do not open raw TCP `1883` from JavaScript; this app uses a **backend** MQTT client. Set `MQTT_URL` (e.g. `mqtt://mosquitto:1883` in Docker). See your deployment README for firewall and ACLs.

## Firmware

Implementation and publish rates live in **your firmware repository** (`icm20948`, `madgwick`, `config.h`). This repo intentionally does not ship that firmware.
