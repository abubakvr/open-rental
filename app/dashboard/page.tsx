import type { Metadata } from "next";

import EspDashboardClient from "@/app/dashboard/esp-dashboard-client";

export const metadata: Metadata = {
  title: "ESP telemetry · Open Rental",
  description: "Live coordinates, uptime, and signal from the ESP controller",
};

export default function DashboardPage() {
  return <EspDashboardClient />;
}
