# 🎧 Application Audio Capture

A Rust native Node-API addon that delivers system or application audio as Node.js `Buffer` objects on Windows and Linux. The JavaScript API is the same on both platforms; the native implementation uses WASAPI on Windows and PipeWire on Linux.

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
- Rust 1.88 or newer and Cargo when building from source
- Windows source builds: MSVC Build Tools with the Windows SDK
- Linux runtime: PipeWire 0.3.65 or newer (`libpipewire-0.3`); source builds also need its development headers, `pkg-config`, a C linker, and libclang for generated bindings

Installing a published prebuilt package does not require Rust, Cargo, or development headers.

Common Linux dependency packages:

```sh
# Debian / Ubuntu
sudo apt install libpipewire-0.3-dev pkg-config libclang-dev build-essential

# Fedora
sudo dnf install pipewire-devel pkgconf-pkg-config clang-devel gcc

# Arch Linux
sudo pacman -S pipewire pkgconf clang base-devel
```

## Installation

```sh
npm install loopback-capture
```

To build a checkout:

```sh
pnpm install --frozen-lockfile
pnpm run build
```

Bun also works as a script runner (`bun install`, then `bun run build`). The native build runs `cargo build --release --locked`, then copies the platform library to `build/Release/loopback_capture_addon.node`. `CARGO_TARGET_DIR` is respected. The JavaScript loader uses that location for checkouts and `prebuilds/<platform>-<arch>` for published packages. `pnpm run clean` removes Cargo output and the local addon.

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

Each chunk contains interleaved signed 16-bit little-endian PCM with two channels at 48 kHz. Chunks contain whole frames and may have different lengths.

Both platforms use a bounded queue of at most 64 pending audio chunks. A slow JavaScript consumer causes new chunks to be dropped instead of blocking capture or allowing memory to grow without limit. Capture buffers marked silent are not delivered.

`start()`, `startSystemAudio()`, and `stop()` are synchronous. Startup failures throw immediately. Unexpected terminal failures after startup stop audio delivery and appear in the read-only `capture.lastError` property (`string | null`). Read the property before calling `stop()`, which clears the session and can throw its terminal error. Call `stop()` before starting that instance again. An idle or healthy session reports `null`.

Windows system capture binds to the default playback endpoint selected at startup. It does not automatically switch endpoints or reconnect after device invalidation; inspect `lastError`, stop the failed session, and start a new capture. Linux continues retrying recoverable PipeWire disconnections internally.

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

Linux capture can start before the target plays audio. It delivers no audio while no matching streams are available and adds/removes streams as playback changes. Children and grandchildren discovered later are included automatically. Known descendants stay associated after reparenting; PID start times prevent unrelated processes from being admitted through PID reuse. Capture remains active until `stop()`, including after the target exits. PipeWire disconnections trigger reconnection attempts; no audio is delivered while the server is unavailable.

## Implementation

The native engine is Rust. A shared Node-API adapter validates arguments and passes owned PCM packets to JavaScript. Each capture session owns a dedicated native worker. Platform capture state belongs to that worker; operating-system completion callbacks use explicit synchronization. Shutdown closes delivery before signalling the worker, suppressing queued callbacks from the old session. Native resources are released on their owning worker. If a system call prevents shutdown for six seconds, `stop()` throws and the worker finishes in the background with delivery disabled. Unsafe code is restricted to documented Windows API boundaries; the Linux backend, process tracking, shared session code, and Node adapter contain no unsafe blocks.

The platform-neutral `capture` module can also be used from Rust. Disable the default `node-addon` Cargo feature to build the library without Node-API dependencies (`cargo check --no-default-features`). It exposes capture modes, owned audio delivery, and session lifecycle management independently of the JavaScript adapter.

Windows uses WASAPI with three capture modes:

| Call                              | Captured audio                                             | WASAPI activation                |
| --------------------------------- | ---------------------------------------------------------- | -------------------------------- |
| `start(pid, callback)`            | The process and its descendants                            | Process loopback, include tree   |
| `startSystemAudio(callback, pid)` | All other processes, excluding the PID and its descendants | Process loopback, exclude tree   |
| `startSystemAudio(callback)`      | All audio on the current default playback device           | Default render endpoint loopback |

Linux system capture uses a PipeWire input stream with `stream.capture.sink=true`, which routes capture from the default output sink rather than from a microphone. Filtered capture keeps the registry listener active, binds full node/client information, and links selected `Stream/Output/Audio` ports to one capture stream. PipeWire mixes and resamples the streams on its graph; the addon downmixes mono/stereo/standard surround channels to stereo PCM16 with saturation. Existing application playback links are preserved. Audio delivery follows the shared bounded-queue policy described above.

Process matching relies on observable ancestry and metadata. Descendants that become detached before they can be observed, namespace-translated PIDs, and audio outside the accessible PipeWire graph remain limitations. Cargo selects the backend for the build target. Platform implementations are separate Rust modules; Linux process-tree tracking is independent of Node-API.

## Testing

```sh
cargo fmt --all -- --check
cargo clippy --locked --all-targets -- -D warnings
cargo test --locked
pnpm run build
```

Run `pnpm run test:linux` after building the addon to exercise live process capture against a private PipeWire/PulseAudio test server. The tests require `pipewire`, `pipewire-pulse`, `pw-cat`, and `pacat` (on Debian/Ubuntu: `pipewire pipewire-bin pipewire-pulse pulseaudio-utils`). They use temporary sockets and synthetic audio, without accessing your desktop audio server or physical devices.

On Windows, `pnpm run test:windows` checks argument validation without requiring an audio endpoint. The opt-in live suite requires an actual Windows machine with a working default playback endpoint; its synthetic test audio can be audible:

```powershell
$env:LOOPBACK_WINDOWS_AUDIO_TESTS = "1"
pnpm run test:windows:audio
```

Run the live Windows suite on Windows build 20348 or later to exercise process-loopback capture. Unit tests and cross-target compilation cannot establish driver behavior, audio routing, device invalidation, or real audio timing; live Windows testing remains necessary. The GitHub Actions workflow runs Rust checks and platform API tests, including isolated Linux audio tests. Hosted Windows CI runs validation rather than the opt-in audible suite.

The original Windows implementation was based on Microsoft's Windows classic samples. The Rust backend preserves that attribution:
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
