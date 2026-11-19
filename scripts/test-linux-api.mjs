import assert from "node:assert/strict";
import { execFileSync, fork, spawn } from "node:child_process";
import { once } from "node:events";
import { mkdirSync, mkdtempSync, writeFileSync, existsSync, rmSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import test from "node:test";
import { Worker } from "node:worker_threads";

const require = createRequire(import.meta.url);
const config = `
context.properties = { core.daemon = true core.name = loopback-test default.clock.rate = 48000 default.clock.quantum = 256 }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-spa-node-factory }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-link-factory }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
 { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver priority.driver = 20000 } }
 { factory = adapter args = { factory.name = support.null-audio-sink node.name = test-output media.class = Audio/Sink audio.channels = 2 audio.position = [ FL FR ] adapter.auto-port-config = { mode = dsp monitor = true position = preserve } } }
]
`;

async function waitFor(predicate, message, timeout = 5000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await delay(30);
  }
  throw new Error(message);
}

async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, "exit");
  child.kill();
  await exited;
}

function amplitude(buffer, hz, channel = 0) {
  const frames = Math.min(buffer.length / 4, 12000);
  let real = 0,
    imaginary = 0;
  for (let i = 0; i < frames; i++) {
    const sample = buffer.readInt16LE(i * 4 + channel * 2);
    real += sample * Math.cos((2 * Math.PI * hz * i) / 48000);
    imaginary += sample * Math.sin((2 * Math.PI * hz * i) / 48000);
  }
  return (Math.hypot(real, imaginary) * 2) / frames;
}

class Recording {
  chunks = [];
  constructor(Capture) {
    this.capture = new Capture();
  }
  onData = (chunk) => {
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
    await waitFor(
      () => this.chunks.reduce((sum, chunk) => sum + chunk.length, 0) >= 48000,
      "capture did not produce 250 ms of audio",
    );
    return Buffer.concat(this.chunks);
  }
  async expectTones(present, absent = []) {
    let last;
    try {
      await waitFor(
        async () => {
          last = await this.sample();
          return (
            present.every((hz) => amplitude(last, hz) > 1500) &&
            absent.every((hz) => amplitude(last, hz) < 100)
          );
        },
        `wrong capture spectrum: ${JSON.stringify({ present, absent })}`,
      );
    } catch (error) {
      if (last)
        error.message += `; measured ${JSON.stringify(Object.fromEntries([...present, ...absent].map((hz) => [hz, amplitude(last, hz)])))}`;
      throw error;
    }
  }
}

test(
  "Linux live process-tree capture",
  { skip: process.platform !== "linux", timeout: 45000 },
  async (t) => {
    const directory = mkdtempSync(join(tmpdir(), "loopback-capture-test-"));
    const env = {
      ...process.env,
      XDG_RUNTIME_DIR: directory,
      PIPEWIRE_RUNTIME_DIR: directory,
      PIPEWIRE_REMOTE: "loopback-test",
      LOOPBACK_TEST_DIR: directory,
      PULSE_SERVER: `unix:${directory}/pulse-native`,
    };
    // The native addon reads these in the same process as the test.
    const oldEnvironment = Object.fromEntries(
      ["XDG_RUNTIME_DIR", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE"].map((key) => [
        key,
        process.env[key],
      ]),
    );
    Object.assign(process.env, {
      XDG_RUNTIME_DIR: directory,
      PIPEWIRE_RUNTIME_DIR: directory,
      PIPEWIRE_REMOTE: "loopback-test",
    });
    writeFileSync(join(directory, "pipewire.conf"), config);
    const processes = [],
      orphanPids = [],
      captures = [];
    let daemon;
    t.after(async () => {
      for (const recording of captures) recording.capture.stop();
      for (const pid of orphanPids) {
        try {
          process.kill(pid, "SIGTERM");
        } catch {}
      }
      for (const child of processes.reverse()) await stop(child);
      if (daemon) await stop(daemon);
      for (const [key, value] of Object.entries(oldEnvironment)) {
        if (value === undefined) delete process.env[key];
        else process.env[key] = value;
      }
      rmSync(directory, { recursive: true, force: true });
    });
    async function startDaemon() {
      daemon = spawn("pipewire", ["-c", join(directory, "pipewire.conf")], {
        env,
        stdio: ["ignore", "ignore", "pipe"],
      });
      let log = "";
      daemon.stderr.on("data", (chunk) => {
        log += chunk;
      });
      await waitFor(() => {
        if (daemon.exitCode !== null) throw new Error(`test PipeWire server failed: ${log}`);
        return existsSync(join(directory, "loopback-test"));
      }, "test PipeWire server did not start");
      await delay(100);
    }
    await startDaemon();
    const { LoopbackCapture } = require("../build/Release/loopback_capture_addon.node");
    let request = 0;
    async function controller(extraEnv = {}) {
      const child = fork(new URL("./fixtures/linux-audio.mjs", import.meta.url), [], {
        env: { ...env, ...extraEnv },
        stdio: ["ignore", "ignore", "inherit", "ipc"],
      });
      processes.push(child);
      await once(child, "message");
      return {
        pid: child.pid,
        async send(command, extra = {}) {
          const id = ++request;
          return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
              child.off("message", receive);
              reject(new Error(`fixture ${command} timed out`));
            }, 5000);
            function receive(message) {
              if (message.request === id) {
                clearTimeout(timer);
                child.off("message", receive);
                if (message.error) reject(new Error(message.error));
                else resolve(message);
              }
            }
            child.on("message", receive);
            child.send({ request: id, command, ...extra });
          });
        },
      };
    }
    function recording() {
      const value = new Recording(LoopbackCapture);
      captures.push(value);
      return value;
    }

    // These checks share live playback state. Stop on the first failure so a
    // setup problem does not turn into unrelated spectrum timeouts.
    async function step(name, callback) {
      let failure;
      await t.test(name, async () => {
        try {
          await callback();
        } catch (error) {
          failure = error;
          throw error;
        }
      });
      if (failure) throw failure;
    }

    await step("validates PIDs and rejects the old boolean signature", () => {
      const capture = new LoopbackCapture();
      assert.throws(() => capture.start(process.pid, true, () => {}), TypeError);
      assert.throws(() => capture.start(2147483647, () => {}), /Cannot read \/proc/);
      assert.throws(() => capture.startSystemAudio(() => {}, 2147483647), /Cannot read \/proc/);
      for (const [pid, error] of [
        [0, RangeError],
        [-1, RangeError],
        [1.5, RangeError],
        [2 ** 31, RangeError],
        [NaN, RangeError],
        [Infinity, RangeError],
        ["1", TypeError],
        [null, TypeError],
        [{}, TypeError],
      ]) {
        assert.throws(() => capture.start(pid, () => {}), error);
        assert.throws(() => capture.startSystemAudio(() => {}, pid), error);
      }
      capture.stop();
    });

    const target = await controller(),
      outsider = await controller();
    await step("reports playback startup errors directly", async () => {
      const brokenBin = join(directory, "broken-player");
      mkdirSync(brokenBin);
      writeFileSync(
        join(brokenBin, "pw-cat"),
        "#!/bin/sh\necho 'unsupported playback option' >&2\nexit 2\n",
        { mode: 0o755 },
      );
      const broken = await controller({ PATH: brokenBin });
      await assert.rejects(
        broken.send("play", { spec: { hz: 440 } }),
        /pw-cat exited with code 2: unsupported playback option/,
      );
    });
    await step("captures a sink monitor without disturbing existing playback links", async () => {
      const tone = await target.send("play", { spec: { hz: 550 } });
      const system = recording();
      system.capture.startSystemAudio(system.onData);
      assert.equal(system.capture.lastError, null);

      // The private server has no session manager. Establish the routes that
      // WirePlumber would normally create, using only this test's null sink.
      let playback, monitor, configuredCapture;
      await waitFor(() => {
        const graph = JSON.parse(execFileSync("pw-dump", { env, encoding: "utf8" }));
        const node = (name) =>
          graph.find(
            (object) => object.type.endsWith(":Node") && object.info.props["node.name"] === name,
          )?.id;
        const playerId = node(`tone-${tone.helperPid}`);
        const sinkId = node("test-output");
        const captureId = node("loopback-capture");
        if (captureId !== undefined && configuredCapture !== captureId) {
          execFileSync(
            "pw-cli",
            [
              "set-param",
              String(captureId),
              "PortConfig",
              "{ direction = Input mode = dsp monitor = false format = { mediaType = audio mediaSubtype = raw format = F32P channels = 2 position = [ FL FR ] } }",
            ],
            { env, stdio: "ignore" },
          );
          configuredCapture = captureId;
        }
        const port = (nodeId, direction, channel) =>
          graph.find(
            (object) =>
              object.type.endsWith(":Port") &&
              Number(object.info.props["node.id"]) === nodeId &&
              object.info.props["port.direction"] === direction &&
              object.info.props["audio.channel"] === channel,
          )?.id;
        playback = ["FL", "FR"].map((channel) => [
          port(playerId, "out", channel),
          port(sinkId, "in", channel),
        ]);
        monitor = ["FL", "FR"].map((channel) => [
          port(sinkId, "out", channel),
          port(captureId, "in", channel),
        ]);
        return [...playback, ...monitor].flat().every((id) => id !== undefined);
      }, "private sink monitor ports were not created");
      for (const [output, input] of [...playback, ...monitor]) {
        execFileSync("pw-link", [String(output), String(input)], { env, stdio: "ignore" });
      }
      await system.expectTones([550]);
      system.capture.stop();
      const graph = JSON.parse(execFileSync("pw-dump", { env, encoding: "utf8" }));
      for (const [output, input] of playback) {
        assert.ok(
          graph.some(
            (object) =>
              object.type.endsWith(":Link") &&
              Number(object.info.props["link.output.port"]) === output &&
              Number(object.info.props["link.input.port"]) === input,
          ),
          "normal playback link was preserved",
        );
      }
      await target.send("stop", { id: tone.id });
    });
    const selected = recording();
    await step("starts before playback without emitting silent packets", async () => {
      selected.capture.start(target.pid, selected.onData);
      await selected.expectNoAudio();
      assert.throws(() => selected.capture.start(target.pid, selected.onData), /already started/);
    });
    await step("does not emit packets for actively rendered silence", async () => {
      const silent = await target.send("play", { spec: { hz: 0 } });
      try {
        await selected.expectNoAudio();
      } finally {
        await target.send("stop", { id: silent.id });
      }
    });
    let first;
    await step("mixes late grandchildren and excludes unrelated streams", async () => {
      first = await target.send("play", { spec: { hz: 440 } });
      await target.send("play", { spec: { hz: 880 } });
      await outsider.send("play", { spec: { hz: 1760 } });
      await selected.expectTones([440, 880], [1760]);
    });
    await step("can stop immediately without delivering queued callbacks", async () => {
      const { capture } = recording();
      let calls = 0;
      for (let attempt = 0; attempt < 3; attempt++) {
        capture.start(target.pid, () => calls++);
        capture.stop();
        capture.stop();
      }
      await delay(100);
      assert.equal(calls, 0);
    });
    await step("can stop and restart inside a callback without stale delivery", async () => {
      const { capture } = recording();
      let oldCalls = 0,
        newCalls = 0;
      capture.start(target.pid, () => {
        oldCalls++;
        capture.stop();
        capture.start(target.pid, () => {
          newCalls++;
          capture.stop();
        });
      });
      await waitFor(() => newCalls > 0, "capture did not restart inside its callback");
      await delay(100);
      assert.equal(oldCalls, 1);
      assert.equal(newCalls, 1);
    });
    await step("stops cleanly after the JavaScript consumer falls behind", async () => {
      const { capture } = recording();
      let calls = 0,
        stopped = false;
      capture.start(target.pid, () => {
        if (calls++ === 0) {
          const until = performance.now() + 700;
          while (performance.now() < until) {
            /* Fill the bounded native queue. */
          }
        } else {
          capture.stop();
          stopped = true;
        }
      });
      await waitFor(() => stopped, "capture did not stop after a slow callback");
      await delay(100);
      assert.equal(calls, 2, "queued callbacks must be discarded after stop");
    });
    await step("terminates a Node Worker with active native capture", async () => {
      const worker = new Worker(
        `
        const { parentPort, workerData } = require("node:worker_threads");
        const { LoopbackCapture } = require(workerData.addon);
        const capture = new LoopbackCapture();
        capture.start(workerData.pid, () => {});
        parentPort.postMessage("ready");
      `,
        {
          eval: true,
          workerData: {
            addon: require.resolve("../build/Release/loopback_capture_addon.node"),
            pid: target.pid,
          },
        },
      );
      try {
        await Promise.race([
          once(worker, "message"),
          delay(3000).then(() => {
            throw new Error("Worker capture startup timed out");
          }),
        ]);
        await Promise.race([
          worker.terminate(),
          delay(3000).then(() => {
            throw new Error("Worker capture cleanup timed out");
          }),
        ]);
      } finally {
        await worker.terminate();
      }
    });
    await step("forwards a throwing audio callback and permits clean shutdown", async () => {
      // Isolate uncaughtException handling from node:test's own global handler.
      const worker = new Worker(
        `
        const { parentPort, workerData } = require("node:worker_threads");
        const { LoopbackCapture } = require(workerData.addon);
        const capture = new LoopbackCapture();
        process.once("uncaughtException", (error) => {
          if (error.message !== "expected audio callback failure") throw error;
          capture.stop();
          parentPort.postMessage("handled callback failure");
          parentPort.close();
        });
        capture.start(workerData.pid, () => {
          throw new Error("expected audio callback failure");
        });
      `,
        {
          eval: true,
          workerData: {
            addon: require.resolve("../build/Release/loopback_capture_addon.node"),
            pid: target.pid,
          },
        },
      );
      try {
        const completed = Promise.all([once(worker, "message"), once(worker, "exit")]);
        const [[message], [exitCode]] = await Promise.race([
          completed,
          delay(3000).then(() => {
            throw new Error("throwing callback cleanup timed out");
          }),
        ]);
        assert.equal(message, "handled callback failure");
        assert.equal(exitCode, 0);
      } finally {
        await worker.terminate();
      }
    });
    await step("removes closed streams and picks up replacements", async () => {
      await target.send("stop", { id: first.id });
      await selected.expectTones([880], [440, 1760]);
      await target.send("play", { spec: { hz: 660, rate: 44100, channels: ["MONO"] } });
      await selected.expectTones([660, 880], [440, 1760]);
      const audio = await selected.sample();
      assert.ok(amplitude(audio, 660, 1) > 1500, "mono resampled into both stereo channels");
    });
    const excluded = recording();
    await step("preserves stereo separation and downmixes center audio", async () => {
      const stereo = await target.send("play", { spec: { hz: [220, 330] } });
      const center = await target.send("play", { spec: { hz: 1540, channels: ["FC"] } });
      await selected.expectTones([220, 1540], [330]);
      const audio = await selected.sample();
      assert.ok(amplitude(audio, 330, 1) > 1500);
      assert.ok(amplitude(audio, 220, 1) < 100);
      assert.ok(amplitude(audio, 1540, 1) > 1500);
      await target.send("stop", { id: stereo.id });
      await target.send("stop", { id: center.id });
    });
    await step("saturates loud mixed audio instead of wrapping PCM16", async () => {
      const loud = await target.send("play", { spec: { dc: [1.5, -1.5] } });
      await waitFor(async () => {
        const audio = await selected.sample();
        for (let offset = 0; offset < audio.length; offset += 4) {
          if (audio.readInt16LE(offset) !== 32767 || audio.readInt16LE(offset + 2) !== -32768)
            return false;
        }
        return true;
      }, "loud PCM16 audio was not saturated");
      await target.send("stop", { id: loud.id });
    });
    await step("system capture excludes the entire target process tree", async () => {
      excluded.capture.startSystemAudio(excluded.onData, target.pid);
      await excluded.expectTones([1760], [660, 880]);
    });
    await step("identifies PulseAudio compatibility streams by originating PID", async () => {
      const pulseConfig = `
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-protocol-pulse }
]
pulse.properties = { server.address = [ "unix:${directory}/pulse-native" ] server.dbus-name = "org.pulseaudio.LoopbackTest.p${process.pid}" pulse.default.tlength = 1440/48000 }
stream.properties = { node.always-process = true adapter.auto-port-config = { mode = dsp position = preserve } }
`;
      writeFileSync(join(directory, "pulse.conf"), pulseConfig);
      const pulse = spawn("pipewire-pulse", ["-c", join(directory, "pulse.conf")], {
        env,
        stdio: ["ignore", "ignore", "inherit"],
      });
      processes.push(pulse);
      await waitFor(
        () => existsSync(join(directory, "pulse-native")),
        "private PulseAudio server did not start",
      );
      const tone = await target.send("play", { spec: { hz: 1100, pulse: true } });
      // This server has no session manager to create the normal playback route.
      for (const channel of ["FL", "FR"]) {
        await waitFor(() => {
          try {
            execFileSync(
              "pw-link",
              [`pacat:output_${channel}`, `test-output:playback_${channel}`],
              { env, stdio: "ignore" },
            );
            return true;
          } catch {
            return false;
          }
        }, "PulseAudio playback route was not created");
      }
      try {
        await selected.expectTones([660, 880, 1100], [1760]);
      } catch (error) {
        const graph = JSON.parse(execFileSync("pw-dump", { env, encoding: "utf8" }));
        t.diagnostic(
          JSON.stringify(
            graph
              .filter((object) => object.type.endsWith(":Link") || object.type.endsWith(":Node"))
              .map((object) => ({
                id: object.id,
                type: object.type,
                state: object.info.state,
                name: object.info.props["node.name"],
                pid: object.info.props["application.process.id"],
                output: object.info.props["link.output.node"],
                input: object.info.props["link.input.node"],
              })),
          ),
        );
        throw error;
      }
      await excluded.expectTones([1760], [660, 880, 1100]);
      await target.send("stop", { id: tone.id });
    });
    await step("skips playback with unresolvable ownership during exclusion", async () => {
      const unknown = await target.send("play", {
        spec: { hz: 1980, pulse: true, unknownPid: true },
      });
      for (const channel of ["FL", "FR"]) {
        await waitFor(() => {
          try {
            execFileSync(
              "pw-link",
              [`pacat:output_${channel}`, `test-output:playback_${channel}`],
              { env, stdio: "ignore" },
            );
            return true;
          } catch {
            return false;
          }
        }, "unattributable PulseAudio stream was not created");
      }
      await selected.expectTones([660, 880], [1980]);
      await excluded.expectTones([1760], [660, 880, 1980]);
      await target.send("stop", { id: unknown.id });
    });
    await step("retains known descendants when their parent exits", async () => {
      const { pids } = await target.send("orphan");
      orphanPids.push(...pids);
      await delay(1100);
      await selected.expectTones([660, 880], [1760]);
      await excluded.expectTones([1760], [660, 880]);
    });
    await step("reconnects after the PipeWire server restarts", async () => {
      await stop(daemon);
      await waitFor(
        () => !existsSync(join(directory, "loopback-test")),
        "server socket was not removed",
      );
      await delay(1250); // Exercise failed reconnect attempts while the server is unavailable.
      assert.equal(
        excluded.capture.lastError,
        null,
        "recoverable disconnection is not a terminal failure",
      );
      await startDaemon();
      await outsider.send("play", { spec: { hz: 1320 } });
      await excluded.expectTones([1320], [660, 880]);
      assert.equal(excluded.capture.lastError, null);
    });
    await step("can stop and restart an instance", async () => {
      excluded.capture.stop();
      excluded.capture.stop();
      excluded.capture.start(outsider.pid, excluded.onData);
      await excluded.expectTones([1320]);
    });
  },
);
