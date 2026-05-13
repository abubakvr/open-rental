import type { Metadata } from "next";

import ImuLiveClient from "@/app/dashboard/imu/imu-live-client";

export const metadata: Metadata = {
  title: "Live IMU · Open Rental",
  description:
    "Three.js view of ICM-20948 motion streamed from the device over MQTT",
};

export default function ImuDashboardPage() {
  return <ImuLiveClient />;
}
