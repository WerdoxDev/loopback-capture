# 🎧 Application Audio Capture

A native N-API addon that delivers system or application audio as Node.js `Buffer` objects on Windows and Linux.

## Platform support

| Feature | Windows 10 x64 2004+ | Linux with PipeWire |
| --- | --- | --- |
| System audio | WASAPI loopback | Default PipeWire output sink |
| PID/process-tree audio | Yes | Best effort via PipeWire client/node metadata |
| Output format | 16-bit PCM, stereo, 48 kHz | 16-bit PCM, stereo, 48 kHz |

On Linux, `start()` finds playback nodes by correlating their `client.id` with PID metadata from PipeWire client objects. This is best effort: applications using the PulseAudio compatibility server may expose the compatibility server's PID rather than the application's PID.

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

## Capture one process

```ts
import loopback from "loopback-capture";

const capture = new loopback.LoopbackCapture();

capture.start(1234, true, (chunk: Buffer) => {
  console.log("Application audio data:", chunk);
});

process.on("SIGINT", () => {
  capture.stop();
  process.exit(0);
});
```

Pass `true` to include descendant processes. On Windows, passing `false` selects WASAPI's exclude-target-process-tree mode; on Linux it captures only the specified PID.

## Implementation

Windows uses WASAPI. Per-process capture is built on `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`; system capture uses the default render endpoint with `AUDCLNT_STREAMFLAGS_LOOPBACK`.

Linux system capture uses a PipeWire input stream with `stream.capture.sink=true`, which routes capture from the default output sink rather than from a microphone. Process capture discovers `Stream/Output/Audio` nodes and targets their object serial after matching node or owning-client PID metadata. The Linux and Windows native implementations live in separate `src/platform/linux` and `src/platform/windows` directories and CMake selects exactly one backend.

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
