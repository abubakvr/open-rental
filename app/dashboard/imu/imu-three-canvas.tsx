"use client";

import { useEffect, useRef } from "react";
import * as THREE from "three";

import type { EspImuOrientation } from "@/lib/esp-imu-store";

type Props = {
  orientation: EspImuOrientation | null;
  /** Increment to capture current sensor quaternion as rest (offset calibration). */
  zeroNonce: number;
};

/**
 * Applies fused quaternion from MQTT: THREE.Quaternion(x,y,z,w) with optional
 * offset so the mesh rest pose matches the PCB (see docs).
 */
export default function ImuThreeCanvas({ orientation, zeroNonce }: Props) {
  const mountRef = useRef<HTMLDivElement>(null);
  const orientationRef = useRef(orientation);
  orientationRef.current = orientation;

  const offsetQuatRef = useRef(new THREE.Quaternion());
  const sensorQuatRef = useRef(new THREE.Quaternion());
  const displayedQuatRef = useRef(new THREE.Quaternion());

  useEffect(() => {
    if (zeroNonce === 0) return;
    const o = orientationRef.current;
    if (!o) return;
    const s = sensorQuatRef.current.set(o.qx, o.qy, o.qz, o.qw).normalize();
    offsetQuatRef.current.copy(s).invert();
  }, [zeroNonce]);

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

    const tmp = new THREE.Vector3();
    const identityQuat = new THREE.Quaternion(0, 0, 0, 1);
    const lastWallMs = { v: performance.now() };

    let raf = 0;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      const now = performance.now();
      const dt = Math.min(1 / 20, Math.max(1 / 240, (now - lastWallMs.v) / 1000));
      lastWallMs.v = now;

      const o = orientationRef.current;
      if (o) {
        sensorQuatRef.current.set(o.qx, o.qy, o.qz, o.qw).normalize();
        displayedQuatRef.current
          .copy(offsetQuatRef.current)
          .multiply(sensorQuatRef.current);
        const alpha = THREE.MathUtils.clamp(dt * 28, 0.12, 0.92);
        figure.quaternion.slerp(displayedQuatRef.current, alpha);
      } else {
        figure.quaternion.slerp(identityQuat, 1 - Math.exp(-dt * 2.5));
      }

      tmp.set(0, 0.9, 0);
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
