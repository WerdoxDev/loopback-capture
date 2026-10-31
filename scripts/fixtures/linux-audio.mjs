import { fork, spawn } from "node:child_process";
import { writeFileSync, rmSync } from "node:fs";
import { join } from "node:path";

const players = new Map();
let nextId = 0;
const reply = (message) => process.connected && process.send(message);

function floatWave(samples, rate, channels) {
  // A WAV container works with older pw-cat versions that lack --raw.
  // Keep IEEE float samples so the clipping test still exercises values > 1.
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + samples.length, 4);
  header.write("WAVEfmt ", 8);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(3, 20); // WAVE_FORMAT_IEEE_FLOAT
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(rate, 24);
  header.writeUInt32LE(rate * channels * 4, 28);
  header.writeUInt16LE(channels * 4, 32);
  header.writeUInt16LE(32, 34);
  header.write("data", 36);
  header.writeUInt32LE(samples.length, 40);
  return Buffer.concat([header, samples]);
}

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
  const file = join(
    process.env.LOOPBACK_TEST_DIR,
    `tone-${process.pid}.${spec.pulse ? "raw" : "wav"}`,
  );
  writeFileSync(file, spec.pulse ? samples : floatWave(samples, rate, channels.length));
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
        `--rate=${rate}`,
        `--channels=${channels.length}`,
        `--channel-map=${channels.join(",")}`,
        "--format=f32",
        "--target=0",
        "-P",
        `{ node.name = tone-${process.pid} node.always-process = true adapter.auto-port-config = { mode = dsp position = preserve } }`,
        file,
      ];
  const executable = spec.pulse ? "pacat" : "pw-cat";
  const player = spawn(executable, args, {
    stdio: ["ignore", "ignore", "pipe"],
  });
  let ready = false,
    stopping = false,
    finished = false,
    stderr = "";
  const readiness = setTimeout(() => {
    ready = true;
    reply({ ready: true, playerPid: player.pid });
  }, 200);
  player.stderr.on("data", (chunk) => {
    stderr = (stderr + chunk).slice(-8192);
  });
  function finish(code, signal, error) {
    if (finished) return;
    finished = true;
    clearTimeout(readiness);
    rmSync(file, { force: true });
    if (!ready && !stopping) {
      const message = error
        ? `${executable}: ${error.message}`
        : `${executable} exited with ${signal ? `signal ${signal}` : `code ${code}`}: ${stderr.trim()}`;
      if (process.connected) return process.send({ error: message }, () => process.exit(1));
      console.error(message);
    } else if (!stopping && code !== 0 && stderr) {
      process.stderr.write(stderr);
    }
    process.exit(stopping ? 0 : (code ?? 1));
  }
  const stop = () => {
    stopping = true;
    clearTimeout(readiness);
    player.kill();
  };
  process.on("SIGTERM", stop);
  process.on("message", (message) => {
    if (message.command === "stop") stop();
  });
  player.once("error", (error) => finish(null, null, error));
  player.once("close", (code, signal) => finish(code, signal));
} else {
  process.on("message", (message) => {
    if (message.command === "play") {
      const id = ++nextId;
      const child = fork(new URL(import.meta.url), ["tone", JSON.stringify(message.spec)], {
        stdio: ["ignore", "ignore", "inherit", "ipc"],
      });
      players.set(id, child);
      let answered = false;
      const answer = (result) => {
        if (answered) return;
        answered = true;
        reply({ request: message.request, id, helperPid: child.pid, ...result });
      };
      child.once("message", answer);
      child.once("error", (error) => answer({ error: error.message }));
      child.once("exit", (code, signal) => {
        players.delete(id);
        answer({ error: `audio fixture exited before playback: ${signal ?? code}` });
      });
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
