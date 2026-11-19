import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { Worker } from "node:worker_threads";
import { setTimeout as delay } from "node:timers/promises";

// Explicit opt-in: these tests play audible tones on the default output.
// Run on Windows build 20348+ with an enabled, unmuted playback endpoint:
//   $env:LOOPBACK_WINDOWS_AUDIO_TESTS='1'; npm run test:windows:audio
const enabled = process.platform === "win32" && process.env.LOOPBACK_WINDOWS_AUDIO_TESTS === "1";

function wave(hz, amplitude) {
  const frames = 48_000;
  const buffer = Buffer.alloc(44 + frames * 4);
  buffer.write("RIFF", 0);
  buffer.writeUInt32LE(buffer.length - 8, 4);
  buffer.write("WAVEfmt ", 8);
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20);
  buffer.writeUInt16LE(2, 22);
  buffer.writeUInt32LE(48_000, 24);
  buffer.writeUInt32LE(192_000, 28);
  buffer.writeUInt16LE(4, 32);
  buffer.writeUInt16LE(16, 34);
  buffer.write("data", 36);
  buffer.writeUInt32LE(frames * 4, 40);
  for (let frame = 0; frame < frames; frame++) {
    const value = Math.round(amplitude * Math.sin(2 * Math.PI * hz * frame / 48_000));
    buffer.writeInt16LE(value, 44 + frame * 4);
    buffer.writeInt16LE(value, 46 + frame * 4);
  }
  return buffer;
}

function amplitude(buffer, hz) {
  let real = 0, imaginary = 0;
  const frames = buffer.length / 4;
  for (let frame = 0; frame < frames; frame++) {
    const sample = buffer.readInt16LE(frame * 4);
    const angle = 2 * Math.PI * hz * frame / 48_000;
    real += sample * Math.cos(angle);
    imaginary += sample * Math.sin(angle);
  }
  return 2 * Math.hypot(real, imaginary) / frames;
}

async function waitFor(predicate, message, timeout = 8000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await delay(20);
  }
  throw new Error(message);
}

class Recording {
  chunks = [];
  constructor(Capture) { this.capture = new Capture(); }
  onData = (chunk) => {
    assert.ok(Buffer.isBuffer(chunk));
    assert.equal(chunk.length % 4, 0, "whole stereo PCM16 frames");
    this.chunks.push(chunk);
  };
  async expectNoAudio() {
    this.chunks = [];
    await delay(750);
    assert.equal(this.chunks.length, 0, "silent capture must not deliver audio callbacks");
    assert.equal(this.capture.lastError, null);
  }
  async sample() {
    this.chunks = [];
    await waitFor(() => this.chunks.reduce((sum, chunk) => sum + chunk.length, 0) >= 48_000,
      "No 250 ms audio received; check the playback endpoint and application volumes");
    return Buffer.concat(this.chunks);
  }
  async tones(present, absent = []) {
    await waitFor(async () => {
      const samples = await this.sample();
      return present.every((hz) => amplitude(samples, hz) > 100)
        && absent.every((hz) => amplitude(samples, hz) < 30);
    }, `Wrong captured tones: ${JSON.stringify({ present, absent })}`);
  }
}

test("Windows WASAPI live audio", { skip: !enabled, timeout: 90_000 }, async (t) => {
  const require = createRequire(import.meta.url);
  const { LoopbackCapture } = require("../build/Release/loopback_capture_addon.node");
  const directory = await mkdtemp(join(tmpdir(), "loopback-windows-test-"));
  const players = new Set();
  const captures = new Set();
  const playerScript = join(directory, "play.ps1");
  await writeFile(playerScript, `param([string]$WaveFile)
$ErrorActionPreference = 'Stop'
$player = New-Object System.Media.SoundPlayer $WaveFile
try {
  $player.Load()
  $player.PlayLooping()
  [Console]::Out.WriteLine('ready')
  while ($true) {
    $line = [Console]::ReadLine()
    if ($null -eq $line -or $line -eq 'stop') { break }
  }
} finally { $player.Stop(); $player.Dispose() }
`);
  async function stopPlayer(child) {
    if (child.exitCode !== null || child.signalCode !== null || !child.pid) return;
    const exited = once(child, "exit");
    child.stdin.end("stop\n");
    const timer = setTimeout(() => child.kill(), 3000);
    try { await exited; } finally { clearTimeout(timer); players.delete(child); }
  }
  t.after(async () => {
    for (const capture of captures) {
      try { capture.stop(); } catch { /* still clean up the playback children */ }
    }
    await Promise.all([...players].map(stopPlayer));
    await rm(directory, { recursive: true, force: true });
  });
  let serial = 0;
  async function player(hz, level = 6000) {
    const file = join(directory, `tone-${serial++}.wav`);
    await writeFile(file, wave(hz, level));
    const child = spawn("powershell.exe", ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
      "-File", playerScript, file], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
    players.add(child);
    let output = "", errors = "", failure;
    child.stdout.on("data", (chunk) => { output += chunk; });
    child.stderr.on("data", (chunk) => { errors += chunk; });
    child.on("error", (error) => { failure = error; });
    await waitFor(() => {
      if (failure) throw failure;
      if (child.exitCode !== null) throw new Error(`Tone player exited: ${errors}`);
      return output.includes("ready");
    }, "PowerShell tone player did not start");
    return child;
  }
  function recording() {
    const recording = new Recording(LoopbackCapture);
    captures.add(recording.capture);
    return recording;
  }
  const target = await player(440);
  const outsider = await player(1760);

  await t.test("captures stereo PCM16 from the system endpoint", async () => {
    const system = recording();
    system.capture.startSystemAudio(system.onData);
    try { await system.tones([440, 1760]); } finally { system.capture.stop(); }
  });
  await t.test("captures a process tree and excludes other processes", async () => {
    const selected = recording();
    // This Node process is the ancestor of both PowerShell tone players.
    selected.capture.start(process.pid, selected.onData);
    try { await selected.tones([440, 1760]); } finally { selected.capture.stop(); }
    selected.capture.start(target.pid, selected.onData);
    try { await selected.tones([440], [1760]); } finally { selected.capture.stop(); }
  });
  await t.test("excludes the selected process", async () => {
    const excluded = recording();
    excluded.capture.startSystemAudio(excluded.onData, target.pid);
    try { await excluded.tones([1760], [440]); } finally { excluded.capture.stop(); }
  });
  await t.test("can stop immediately, stop twice, and reuse an instance", async () => {
    const selected = recording();
    for (let iteration = 0; iteration < 20; iteration++) {
      selected.capture.start(target.pid, selected.onData);
      selected.capture.stop();
      selected.capture.stop();
    }
    selected.capture.start(target.pid, selected.onData);
    try { await selected.tones([440], [1760]); } finally { selected.capture.stop(); }
  });
  await t.test("rejects a second start without disrupting capture", async () => {
    const selected = recording();
    selected.capture.start(target.pid, selected.onData);
    try {
      assert.throws(() => selected.capture.start(target.pid, selected.onData), /already started/);
      await selected.tones([440], [1760]);
    } finally { selected.capture.stop(); }
  });
  await t.test("returns capture buffers intact while JavaScript retains them", async () => {
    const selected = recording();
    selected.capture.start(target.pid, selected.onData);
    try {
      await selected.sample();
      const retained = selected.chunks[0];
      const copy = Buffer.from(retained);
      await selected.sample();
      assert.deepEqual(retained, copy);
    } finally { selected.capture.stop(); }
  });
  await t.test("can stop from inside the audio callback without stale delivery", async () => {
    const selected = recording();
    let calls = 0;
    selected.capture.start(target.pid, (chunk) => {
      assert.ok(Buffer.isBuffer(chunk));
      calls++;
      selected.capture.stop();
    });
    await waitFor(() => calls > 0, "No callback received");
    await delay(100);
    assert.equal(calls, 1, "queued callbacks continued after stop");
    selected.capture.start(target.pid, selected.onData);
    try { await selected.tones([440], [1760]); } finally { selected.capture.stop(); }
  });
  await t.test("terminates a Node worker with active capture", { timeout: 12_000 }, async (workerTest) => {
    const addonPath = require.resolve("../build/Release/loopback_capture_addon.node");
    const worker = new Worker(`
      const { parentPort } = require('node:worker_threads');
      const { LoopbackCapture } = require(${JSON.stringify(addonPath)});
      const capture = new LoopbackCapture();
      capture.startSystemAudio(() => {});
      parentPort.postMessage('started');
    `, { eval: true });
    workerTest.after(() => worker.terminate());
    const [message] = await once(worker, "message");
    assert.equal(message, "started");
    const started = Date.now();
    await worker.terminate();
    assert.ok(Date.now() - started < 5000, "worker capture teardown stalled");
  });
  await t.test("preserves quiet audio above the silence threshold", async () => {
    const quiet = await player(880, 64);
    const selected = recording();
    selected.capture.start(quiet.pid, selected.onData);
    try {
      const samples = await selected.sample();
      assert.ok(amplitude(samples, 880) > 4, "quiet audio was discarded");
    } finally { selected.capture.stop(); await stopPlayer(quiet); }
  });
  for (const [name, hz, level] of [
    ["actively rendered silence", 0, 0],
    ["audio below the silence threshold", 880, 2],
  ]) {
    await t.test(`does not emit packets for ${name}`, async () => {
      const silent = await player(hz, level);
      const selected = recording();
      selected.capture.start(silent.pid, selected.onData);
      try {
        await selected.expectNoAudio();
      } finally { selected.capture.stop(); await stopPlayer(silent); }
    });
  }
  await stopPlayer(target);
  await stopPlayer(outsider);
});
