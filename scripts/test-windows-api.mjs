import assert from "node:assert/strict";
import { createRequire } from "node:module";
import test from "node:test";

// Validation happens before WASAPI activation, so these tests also work on
// Windows CI hosts without an audio output device.
test("Windows capture API validation", { skip: process.platform !== "win32" }, async (t) => {
  const require = createRequire(import.meta.url);
  const { LoopbackCapture } = require("../build/Release/loopback_capture_addon.node");
  const onData = () => {};

  await t.test("rejects the old process-tree flag and missing callbacks", () => {
    const capture = new LoopbackCapture();
    for (const args of [[], [1234], [1234, true], [1234, true, onData], [1234, false, onData]]) {
      assert.throws(() => capture.start(...args), {
        name: "TypeError",
        message: /Expected start\(processId: number, onData:/,
      });
    }
    for (const args of [[], [1234], [onData, {}, {}]]) {
      assert.throws(() => capture.startSystemAudio(...args), TypeError);
    }
    capture.stop();
  });

  await t.test(
    "rejects invalid target and excluded PIDs before allocating capture resources",
    () => {
      const capture = new LoopbackCapture();
      const invalidPids = [
        [undefined, TypeError],
        [null, TypeError],
        ["1234", TypeError],
        [true, TypeError],
        [0, RangeError],
        [-1, RangeError],
        [1.5, RangeError],
        [2 ** 32, RangeError],
        [NaN, RangeError],
        [Infinity, RangeError],
        [-Infinity, RangeError],
      ];
      for (const [pid, error] of invalidPids) {
        assert.throws(() => capture.start(pid, onData), error);
        // An omitted/undefined exclusion selects normal system capture.
        if (pid !== undefined) {
          assert.throws(() => capture.startSystemAudio(onData, pid), error);
        }
      }
      capture.stop();
    },
  );

  await t.test("rejects an exclusion options object and other nonnumeric arguments", () => {
    const capture = new LoopbackCapture();
    for (const excluded of [null, true, "1234", [], onData, {}, { excludeProcessId: 1234 }]) {
      assert.throws(() => capture.startSystemAudio(onData, excluded), {
        name: "TypeError",
        message: "excludeProcessId must be a number",
      });
    }
    // An invalid exclusion must leave the instance available for reuse.
    assert.throws(() => capture.start(0, onData), RangeError);
    capture.stop();
  });
  await t.test("reports no error before capture and tolerates repeated stop", () => {
    const capture = new LoopbackCapture();
    assert.equal(capture.lastError, null);
    assert.equal(capture.stop(), undefined);
    assert.equal(capture.stop(), undefined);
    assert.equal(capture.lastError, null);
  });

  await t.test("rejects methods and getters called with an invalid receiver", () => {
    const prototype = LoopbackCapture.prototype;
    assert.throws(() => prototype.start.call({}, 1234, onData));
    assert.throws(() => prototype.startSystemAudio.call({}, onData));
    assert.throws(() => prototype.stop.call({}));
    const getter = Object.getOwnPropertyDescriptor(prototype, "lastError").get;
    assert.throws(() => getter.call({}));
  });

});
