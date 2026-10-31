# 🎧 Application Audio Capture

A native N-API addon that delivers system or application audio as Node.js `Buffer` objects on Windows and Linux.

## Platform support

| Feature                               | Windows x64                | Linux with PipeWire                           |
| ------------------------------------- | -------------------------- | --------------------------------------------- |
| System audio                          | WASAPI loopback            | Default PipeWire output sink                  |
| System audio excluding a process tree | Yes, Windows build 20348+  | Yes, for streams with identifiable ownership  |
| PID/process-tree audio                | Yes, Windows build 20348+  | Best effort via PipeWire client/node metadata |
| Output format                         | 16-bit PCM, stereo, 48 kHz | 16-bit PCM, stereo, 48 kHz                    |

On Linux, filtered capture continuously watches PipeWire playback streams and mixes all streams belonging to the selected process tree. It reads full node/client metadata, including originating PIDs for PulseAudio-compatible streams. Ownership detection is best effort; sandboxed applications or streams with missing PID metadata may not be identifiable.

## Requirements

- Node.js with N-API 9 support
- CMake and a C++20 compiler when building from source
- Windows: Windows SDK and MSVC
- Linux: PipeWire 0.3 development files and `pkg-config`

Common Linux dependency packages:

```sh
# Debian / Ubuntu
sudo apt install libpipewire-0.3-dev pkg-config

# Fedora
sudo dnf install pipewire-devel pkgconf-pkg-config

# Arch Linux
sudo pacman -S pipewire pkgconf
```

## Installation

```sh
npm install loopback-capture
```

To build a checkout with Bun:

```sh
bun install
bun run build
```

## Capture system audio

This API works on both Windows and Linux and captures the current default playback device.

```ts
import loopback from "loopback-capture";

const capture = new loopback.LoopbackCapture();

capture.startSystemAudio((chunk: Buffer) => {
  console.log("System audio data:", chunk);
});

process.on("SIGINT", () => {
  capture.stop();
  process.exit(0);
});
```

Each chunk contains interleaved signed 16-bit little-endian PCM with two channels at 48 kHz.

### Exclude a process from system audio

Pass `excludeProcessId` as the optional second argument to capture system audio except the specified process and all its descendants:

```ts
capture.startSystemAudio((chunk: Buffer) => {
  console.log("System audio excluding this app:", chunk);
}, process.pid);
```

This captures across playback devices: Windows uses WASAPI process loopback with `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`, and Linux mixes identifiable playback streams outside the excluded process tree. Linux skips streams whose owner cannot be resolved and reports them to stderr, rather than risking including the excluded application. Omitting `excludeProcessId` (or passing `undefined`) keeps the normal default-playback-device loopback behavior.

On Windows, process capture and system capture with an exclusion require [build 20348 or later](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ne-audioclientactivationparams-process_loopback_mode). Process IDs must be positive integers up to 4294967295 on Windows or 2147483647 on Linux. On Linux, the target/excluded process must exist and its `/proc/<pid>/stat` must be readable when capture starts.

## Capture one process

Process capture always includes the specified process and its descendants on both platforms:

```ts
import loopback from "loopback-capture";

const capture = new loopback.LoopbackCapture();

capture.start(1234, (chunk: Buffer) => {
  console.log("Application audio data:", chunk);
});

process.on("SIGINT", () => {
  capture.stop();
  process.exit(0);
});
```

The `start()` signature has changed on both platforms: replace `start(pid, true, callback)` with `start(pid, callback)`. On Windows, replace `start(pid, false, callback)` with `startSystemAudio(callback, pid)`. Linux process capture now always includes descendants; its former PID-only mode has been removed.

Linux capture can start before the target plays audio. It emits silence while no matching streams are available and adds/removes streams as playback changes. Children and grandchildren discovered later are included automatically. Known descendants stay associated after reparenting; PID start times prevent unrelated processes from being admitted through PID reuse. Capture remains active until `stop()`, including after the target exits. PipeWire disconnections trigger reconnection attempts; no audio is delivered while the server is unavailable.

## Implementation

Windows uses WASAPI with three capture modes:

| Call                              | Captured audio                                             | WASAPI activation                |
| --------------------------------- | ---------------------------------------------------------- | -------------------------------- |
| `start(pid, callback)`            | The process and its descendants                            | Process loopback, include tree   |
| `startSystemAudio(callback, pid)` | All other processes, excluding the PID and its descendants | Process loopback, exclude tree   |
| `startSystemAudio(callback)`      | All audio on the current default playback device           | Default render endpoint loopback |

Linux system capture uses a PipeWire input stream with `stream.capture.sink=true`, which routes capture from the default output sink rather than from a microphone. Filtered capture keeps the registry listener active, binds full node/client information, and links selected `Stream/Output/Audio` ports to one capture stream. PipeWire mixes and resamples the streams on its graph; the addon downmixes mono/stereo/standard surround channels to stereo PCM16 with saturation. Existing application playback links are preserved. The JS audio queue is bounded to 64 chunks; slow callbacks can drop audio rather than growing memory without limit.

Process matching relies on observable ancestry and metadata. Descendants that become detached before they can be observed, namespace-translated PIDs, and audio outside the accessible PipeWire graph remain limitations. The Linux and Windows native implementations live in separate `src/platform/linux` and `src/platform/windows` directories and CMake selects exactly one backend.

Run `bun run test:linux` after building the addon to exercise live process capture against a private PipeWire/PulseAudio test server. The tests require `pipewire`, `pipewire-pulse`, `pw-cat`, and `pacat` (on Debian/Ubuntu: `pipewire pipewire-bin pipewire-pulse pulseaudio-utils`). They use temporary sockets and synthetic audio, without accessing your desktop audio server or physical devices.

The original Windows implementation is based on Microsoft's Windows classic samples:
https://github.com/microsoft/Windows-classic-samples

## Example use cases

- Record desktop/system audio.
- Build a real-time audio visualizer.
- Stream raw audio into an encoder or network transport.
- Capture a selected application and its process tree.

## Publishing

The `Build and publish` GitHub Actions workflow builds x64 native addons on Linux and Windows,
combines them into one npm package, and verifies that the packed package loads on both operating
systems. Pull requests, pushes to `master`, and manual runs build and test without publishing.

To publish a release:

1. Update `version` in `package.json` and commit it.
2. Create and publish a GitHub Release whose tag is exactly `v<version>` (for example, `v3.1.0`).
3. The workflow publishes to npm after both platform smoke tests pass.

Publishing uses npm trusted publishing, so the workflow does not need a long-lived npm token. In
the package settings on npmjs.com, add a GitHub Actions trusted publisher for this repository with
workflow filename `release.yml`, environment `npm`, and direct publishing enabled.
