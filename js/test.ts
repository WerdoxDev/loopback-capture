import addon from "./";
import Bun from "bun";

const mode = process.argv[2]; // "system" or a numeric processId

const usage = () => {
  console.error("Usage: bun js/test.ts <processId> | system [excludeProcessId]");
  process.exit(1);
};

const readProcessId = (value: string | undefined) => {
  const pid = Number(value);
  if (!Number.isInteger(pid) || pid < 1 || pid > 0xffffffff) usage();
  return pid;
};

const capture = new addon.LoopbackCapture();
let totalBytes = 0;

const buffer = new Uint8Array(1024 * 1024 * 10); // 10 MB buffer

const onData = (chunk: Buffer) => {
  // chunk is a Buffer of raw 16-bit PCM, 2 channels, 48000 Hz.
  buffer.set(chunk, totalBytes);
  totalBytes += chunk.length;
  // chunk.
  console.log(`received ${chunk.length} bytes (total: ${totalBytes})`);
  // console.log(
  //   chunk.toString("hex", 0, Math.min(chunk.length, 16)) + (chunk.length > 16 ? "..." : ""),
  // );
};

if (mode === "system") {
  const excluded = process.argv[3];
  if (excluded !== undefined) {
    capture.startSystemAudio(onData, readProcessId(excluded));
  } else {
    // Capture everything going out through the default playback device.
    capture.startSystemAudio(onData);
  }
} else {
  const targetProcessId = readProcessId(mode);
  capture.start(targetProcessId, onData);
}

function pcmToWav(
  pcm: Uint8Array,
  sampleRate = 48000,
  channels = 2,
  bitsPerSample = 16,
): Uint8Array {
  const byteRate = sampleRate * channels * (bitsPerSample / 8);
  const blockAlign = channels * (bitsPerSample / 8);
  const header = new ArrayBuffer(44);
  const v = new DataView(header);
  const str = (off: number, s: string) =>
    [...s].forEach((c, i) => v.setUint8(off + i, c.charCodeAt(0)));

  str(0, "RIFF");
  v.setUint32(4, 36 + pcm.byteLength, true); // file size - 8
  str(8, "WAVE");
  str(12, "fmt ");
  v.setUint32(16, 16, true); // fmt chunk size
  v.setUint16(20, 1, true); // PCM format
  v.setUint16(22, channels, true);
  v.setUint32(24, sampleRate, true);
  v.setUint32(28, byteRate, true);
  v.setUint16(32, blockAlign, true);
  v.setUint16(34, bitsPerSample, true);
  str(36, "data");
  v.setUint32(40, pcm.byteLength, true); // data size

  const wav = new Uint8Array(44 + pcm.byteLength);
  wav.set(new Uint8Array(header), 0);
  wav.set(pcm, 44);
  return wav;
}

// Usage

process.on("SIGINT", async () => {
  capture.stop();

  await Bun.write("output.wav", pcmToWav(buffer));

  process.exit(0);
});
