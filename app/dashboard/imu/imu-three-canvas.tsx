"use client";

import { useEffect, useRef, type MutableRefObject } from "react";
import * as THREE from "three";

import type { EspImuOrientation } from "@/lib/esp-imu-store";

/** Billboard label; rotates with parent so you see device forward / top / etc. in scene space. */
function createDirectionalLabelSprite(
  text: string,
  borderRgb: string,
): THREE.Sprite {
  const w = 280;
  const h = 100;
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    throw new Error("IMU canvas: 2d context unavailable");
  }
  ctx.fillStyle = "rgba(15, 23, 42, 0.92)";
  ctx.fillRect(0, 0, w, h);
  ctx.strokeStyle = borderRgb;
  ctx.lineWidth = 4;
  ctx.strokeRect(2, 2, w - 4, h - 4);
  ctx.fillStyle = "#f8fafc";
  ctx.font = "600 40px system-ui, -apple-system, sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(text, w / 2, h / 2 + 2);

  const tex = new THREE.CanvasTexture(canvas);
  tex.colorSpace = THREE.SRGBColorSpace;
  const mat = new THREE.SpriteMaterial({
    map: tex,
    transparent: true,
    depthTest: true,
    depthWrite: false,
  });
  const sprite = new THREE.Sprite(mat);
  sprite.scale.set(0.58, 0.21, 1);
  return sprite;
}

type Props = {
  orientation: EspImuOrientation | null;
  /** Latest fused sample; updated in the EventSource handler before setState so the render loop tracks ~MQTT rate (e.g. 10 Hz). */
  orientationLiveRef: MutableRefObject<EspImuOrientation | null>;
  /** Increment to capture current sensor quaternion as rest (offset calibration). */
  zeroNonce: number;
};

/**
 * Applies fused quaternion from MQTT: THREE.Quaternion(x,y,z,w) with optional
 * offset so the mesh rest pose matches the PCB (see docs).
 */
export default function ImuThreeCanvas({
  orientation,
  orientationLiveRef,
  zeroNonce,
}: Props) {
  const mountRef = useRef<HTMLDivElement>(null);
  const orientationRef = useRef(orientation);
  orientationRef.current = orientation;

  const offsetQuatRef = useRef(new THREE.Quaternion());
  const sensorQuatRef = useRef(new THREE.Quaternion());
  const displayedQuatRef = useRef(new THREE.Quaternion());

  useEffect(() => {
    if (zeroNonce === 0) return;
    const o = orientationLiveRef.current ?? orientationRef.current;
    if (!o) return;
    const s = sensorQuatRef.current.set(o.qx, o.qy, o.qz, o.qw).normalize();
    offsetQuatRef.current.copy(s).invert();
  }, [zeroNonce, orientationLiveRef]);

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

    /** Forward (+Z) and lateral (+X) cues so yaw (spin about body Y) is visible — capsule/sphere are symmetric about Y. */
    const noseMat = new THREE.MeshStandardMaterial({
      color: 0xef4444,
      roughness: 0.45,
      metalness: 0.08,
    });
    const nose = new THREE.Mesh(
      new THREE.ConeGeometry(0.07, 0.22, 10, 1),
      noseMat,
    );
    nose.castShadow = true;
    nose.rotation.x = Math.PI / 2;
    nose.position.set(0, 1.35, 0.21);

    const wingMat = new THREE.MeshStandardMaterial({
      color: 0x22c55e,
      roughness: 0.5,
      metalness: 0.06,
    });
    const wing = new THREE.Mesh(
      new THREE.BoxGeometry(0.55, 0.06, 0.12),
      wingMat,
    );
    wing.castShadow = true;
    wing.position.set(0, 0.85, 0);

    figure.add(body, head, nose, wing);

    /* Six-way directions in figure space: +Y up (top), +Z forward (nose), +X right — matches Three.js / avatar rig. */
    const gizmo = new THREE.Group();
    gizmo.position.set(0, 0.92, 0);
    figure.add(gizmo);

    const centerDot = new THREE.Mesh(
      new THREE.SphereGeometry(0.055, 14, 12),
      new THREE.MeshStandardMaterial({
        color: 0xffffff,
        emissive: 0x444444,
        emissiveIntensity: 0.35,
        roughness: 0.35,
        metalness: 0.15,
      }),
    );
    centerDot.castShadow = true;
    gizmo.add(centerDot);

    const labelEntries: { text: string; pos: [number, number, number]; border: string }[] =
      [
        { text: "Top", pos: [0, 0.62, 0], border: "#fbbf24" },
        { text: "Bottom", pos: [0, -0.62, 0], border: "#94a3b8" },
        { text: "Forward", pos: [0, 0, 0.68], border: "#38bdf8" },
        { text: "Back", pos: [0, 0, -0.68], border: "#64748b" },
        { text: "Right", pos: [0.68, 0, 0], border: "#a78bfa" },
        { text: "Left", pos: [-0.68, 0, 0], border: "#fb923c" },
      ];

    const directionSprites: THREE.Sprite[] = [];
    for (const { text, pos, border } of labelEntries) {
      const sp = createDirectionalLabelSprite(text, border);
      sp.position.set(pos[0], pos[1], pos[2]);
      gizmo.add(sp);
      directionSprites.push(sp);
    }

    const tmp = new THREE.Vector3();
    const identityQuat = new THREE.Quaternion(0, 0, 0, 1);
    const lastWallMs = { v: performance.now() };

    let raf = 0;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      const now = performance.now();
      const dt = Math.min(1 / 20, Math.max(1 / 240, (now - lastWallMs.v) / 1000));
      lastWallMs.v = now;

      const o = orientationLiveRef.current ?? orientationRef.current;
      if (o) {
        sensorQuatRef.current.set(o.qx, o.qy, o.qz, o.qw).normalize();
        displayedQuatRef.current
          .copy(offsetQuatRef.current)
          .multiply(sensorQuatRef.current);
        /* Apply each sample at full rate (e.g. 10 Hz MQTT); heavy slerp would lag behind the stream. */
        figure.quaternion.copy(displayedQuatRef.current);
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
      nose.geometry.dispose();
      wing.geometry.dispose();
      centerDot.geometry.dispose();
      (centerDot.material as THREE.MeshStandardMaterial).dispose();
      ground.geometry.dispose();
      (bodyMat as THREE.MeshStandardMaterial).dispose();
      (headMat as THREE.MeshStandardMaterial).dispose();
      (noseMat as THREE.MeshStandardMaterial).dispose();
      (wingMat as THREE.MeshStandardMaterial).dispose();
      for (const sp of directionSprites) {
        sp.geometry.dispose();
        const m = sp.material as THREE.SpriteMaterial;
        m.map?.dispose();
        m.dispose();
      }
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
