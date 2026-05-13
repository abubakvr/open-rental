"use client";

import { useEffect, useRef } from "react";
import * as THREE from "three";

import type { EspImuSnapshot } from "@/lib/esp-imu-store";

type Props = {
  imu: EspImuSnapshot | null;
};

/** Degrees per second → radians per second */
const DEG2RAD = Math.PI / 180;

export default function ImuThreeCanvas({ imu }: Props) {
  const mountRef = useRef<HTMLDivElement>(null);
  const imuRef = useRef(imu);
  imuRef.current = imu;

  useEffect(() => {
    const mount = mountRef.current;
    if (!mount) return;

    const dark =
      typeof window !== "undefined" &&
      window.matchMedia("(prefers-color-scheme: dark)").matches;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(dark ? 0x0a0a0a : 0xf4f4f5);

    const camera = new THREE.PerspectiveCamera(
      42,
      Math.max(mount.clientWidth, 1) / Math.max(mount.clientHeight, 1),
      0.1,
      100,
    );
    camera.position.set(0, 1.35, 3.6);
    camera.lookAt(0, 0.85, 0);

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.setSize(mount.clientWidth, mount.clientHeight);
    renderer.shadowMap.enabled = true;
    mount.appendChild(renderer.domElement);

    const hemi = new THREE.HemisphereLight(0xffffff, 0x444444, 0.85);
    scene.add(hemi);
    const dir = new THREE.DirectionalLight(0xffffff, 0.9);
    dir.position.set(2.5, 6, 3);
    dir.castShadow = true;
    scene.add(dir);

    const ground = new THREE.Mesh(
      new THREE.PlaneGeometry(12, 12),
      new THREE.MeshStandardMaterial({
        color: dark ? 0x18181b : 0xe4e4e7,
        roughness: 0.95,
        metalness: 0,
      }),
    );
    ground.rotation.x = -Math.PI / 2;
    ground.receiveShadow = true;
    scene.add(ground);

    const figure = new THREE.Group();
    scene.add(figure);

    const bodyMat = new THREE.MeshStandardMaterial({
      color: 0x3b82f6,
      roughness: 0.55,
      metalness: 0.05,
    });
    const headMat = new THREE.MeshStandardMaterial({
      color: 0xfbbf24,
      roughness: 0.45,
      metalness: 0.05,
    });

    const body = new THREE.Mesh(
      new THREE.CapsuleGeometry(0.2, 0.75, 4, 12),
      bodyMat,
    );
    body.castShadow = true;
    body.position.y = 0.575;

    const head = new THREE.Mesh(new THREE.SphereGeometry(0.2, 20, 16), headMat);
    head.castShadow = true;
    head.position.y = 1.35;

    figure.add(body, head);

    const smooth = {
      px: 0,
      pz: 0,
      pitch: 0,
      roll: 0,
      yaw: 0,
      lastWallMs: performance.now(),
    };

    const tmp = new THREE.Vector3();
    const maxOffset = 1.35;

    let raf = 0;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      const now = performance.now();
      const dt = Math.min(0.05, (now - smooth.lastWallMs) / 1000);
      smooth.lastWallMs = now;

      const sample = imuRef.current;
      if (sample) {
        const { ax, ay, az, gx, gy, gz } = sample;
        const roll = Math.atan2(ay, az);
        const pitch = Math.atan2(-ax, Math.sqrt(ay * ay + az * az));

        const targetX = THREE.MathUtils.clamp(ay * 2.2 + Math.sin(roll) * 0.45, -maxOffset, maxOffset);
        const targetZ = THREE.MathUtils.clamp(ax * 2.0 + Math.sin(pitch) * 0.35, -maxOffset, maxOffset);

        smooth.px = THREE.MathUtils.lerp(smooth.px, targetX, 0.08);
        smooth.pz = THREE.MathUtils.lerp(smooth.pz, targetZ, 0.08);
        smooth.pitch = THREE.MathUtils.lerp(smooth.pitch, pitch, 0.12);
        smooth.roll = THREE.MathUtils.lerp(smooth.roll, roll, 0.12);
        smooth.yaw += (gz * DEG2RAD * dt * 0.55 + gx * DEG2RAD * dt * 0.08);
        smooth.yaw += (gy * DEG2RAD * dt * 0.04);
      } else {
        smooth.px = THREE.MathUtils.lerp(smooth.px, 0, 0.04);
        smooth.pz = THREE.MathUtils.lerp(smooth.pz, 0, 0.04);
        smooth.pitch = THREE.MathUtils.lerp(smooth.pitch, 0, 0.04);
        smooth.roll = THREE.MathUtils.lerp(smooth.roll, 0, 0.04);
        smooth.yaw = THREE.MathUtils.lerp(smooth.yaw, 0, 0.02);
      }

      figure.position.x = smooth.px;
      figure.position.z = smooth.pz;
      figure.rotation.order = "YXZ";
      figure.rotation.set(smooth.pitch * 0.85, smooth.yaw, -smooth.roll * 0.9);

      tmp.set(0, 0.9, 0).add(figure.position);
      camera.lookAt(tmp);

      renderer.render(scene, camera);
    };
    tick();

    const ro = new ResizeObserver(() => {
      if (!mountRef.current) return;
      const w = mountRef.current.clientWidth;
      const h = mountRef.current.clientHeight;
      camera.aspect = Math.max(w, 1) / Math.max(h, 1);
      camera.updateProjectionMatrix();
      renderer.setSize(w, h);
    });
    ro.observe(mount);

    return () => {
      cancelAnimationFrame(raf);
      ro.disconnect();
      body.geometry.dispose();
      head.geometry.dispose();
      ground.geometry.dispose();
      (bodyMat as THREE.MeshStandardMaterial).dispose();
      (headMat as THREE.MeshStandardMaterial).dispose();
      (ground.material as THREE.MeshStandardMaterial).dispose();
      renderer.dispose();
      mount.removeChild(renderer.domElement);
    };
  }, []);

  return (
    <div
      ref={mountRef}
      className="h-[min(72vh,520px)] w-full min-h-[280px] overflow-hidden rounded-2xl border border-zinc-200 dark:border-zinc-700"
    />
  );
}
