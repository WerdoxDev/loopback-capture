import addon from "./";

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

const onData = (chunk: Buffer) => {
  // chunk is a Buffer of raw 16-bit PCM, 2 channels, 48000 Hz.
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

process.on("SIGINT", () => {
  capture.stop();
  process.exit(0);
});
