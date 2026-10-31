import { fork, spawn } from "node:child_process";
import { writeFileSync, unlinkSync } from "node:fs";
import { join } from "node:path";

const players = new Map();
let nextId = 0;
const reply = (message) => process.connected && process.send(message);

if (process.argv[2] === "tone") {
  const spec = JSON.parse(process.argv[3]);
  const rate = spec.rate ?? 48000;
  const channels = spec.channels ?? ["FL", "FR"];
  const samples = Buffer.alloc(rate * 30 * channels.length * 4);
  for (let frame = 0; frame < rate * 30; frame++) {
    for (let channel = 0; channel < channels.length; channel++) {
      const hz = Array.isArray(spec.hz) ? spec.hz[channel] : spec.hz;
      const value =
        (Array.isArray(spec.dc) ? spec.dc[channel] : spec.dc) ??
        (hz ? (spec.amplitude ?? 0.1) * Math.sin((2 * Math.PI * hz * frame) / rate) : 0);
      samples.writeFloatLE(value, (frame * channels.length + channel) * 4);
    }
  }
  const file = join(process.env.LOOPBACK_TEST_DIR, `tone-${process.pid}.raw`);
  writeFileSync(file, samples);
  const args = spec.pulse
    ? [
        "--playback",
        "--raw",
        `--rate=${rate}`,
        `--channels=${channels.length}`,
        "--format=float32le",
        "--latency-msec=30",
        ...(spec.unknownPid ? ["--property=application.process.id=2147483647"] : []),
        file,
      ]
    : [
        "--playback",
        "--raw",
        `--rate=${rate}`,
        `--channels=${channels.length}`,
        `--channel-map=${channels.join(",")}`,
        "--format=f32",
        "--target=0",
        "-P",
        `{ node.name = tone-${process.pid} node.always-process = true adapter.auto-port-config = { mode = dsp position = preserve } }`,
        file,
      ];
  const player = spawn(spec.pulse ? "pacat" : "pw-cat", args, {
    stdio: ["ignore", "ignore", "inherit"],
  });
  const stop = () => player.kill();
  process.on("SIGTERM", stop);
  process.on("message", (message) => {
    if (message.command === "stop") stop();
  });
  player.on("exit", () => {
    unlinkSync(file);
    process.exit(0);
  });
  reply({ ready: true, playerPid: player.pid });
} else {
  process.on("message", (message) => {
    if (message.command === "play") {
      const id = ++nextId;
      const child = fork(new URL(import.meta.url), ["tone", JSON.stringify(message.spec)], {
        stdio: ["ignore", "ignore", "inherit", "ipc"],
      });
      players.set(id, child);
      child.once("exit", () => players.delete(id));
      child.once("message", (ready) =>
        reply({ request: message.request, id, helperPid: child.pid, ...ready }),
      );
    } else if (message.command === "stop") {
      const child = players.get(message.id);
      if (!child) return reply({ request: message.request });
      child.once("exit", () => {
        players.delete(message.id);
        reply({ request: message.request });
      });
      child.kill();
    } else if (message.command === "orphan") {
      const pids = [...players.values()].map((child) => child.pid);
      for (const child of players.values()) child.disconnect();
      process.send({ request: message.request, pids }, () => process.exit(0));
    }
  });
  process.on("SIGTERM", () => {
    for (const child of players.values()) child.kill();
    Promise.all(
      [...players.values()].map((child) => new Promise((resolve) => child.once("exit", resolve))),
    ).then(() => process.exit(0));
  });
  reply({ ready: true });
}
