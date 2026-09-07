import binding from "bindings";
import { arch, platform } from "node:os";

export type LoopbackCapture = {
  /** Capture a PID/process tree. Best effort on Linux because PipeWire PID metadata varies by client. */
  start: (
    processId: number,
    includeProcessTree: boolean,
    callback: (chunk: Buffer) => void,
  ) => void;
  stop: () => void;
  /** Capture the default system output on Windows or Linux. */
  startSystemAudio: (callback: (chunk: Buffer) => void) => void;
};

export type Addon = {
  LoopbackCapture: {
    new (): LoopbackCapture;
  };
};

const operatingSystem = platform();
const architecture = arch();
if (operatingSystem === "win32" && architecture !== "x64") {
  console.warn("loopback-capture supports Windows on x64 only");
} else if (operatingSystem !== "win32" && operatingSystem !== "linux") {
  console.warn("loopback-capture supports Windows and Linux only");
}

const platformDirectory = `${operatingSystem}-${architecture}`;
const addon: Addon = binding({
  bindings: "loopback_capture_addon.node",
  try: [
    ["loopback-capture", "prebuilds", platformDirectory, "bindings"],
    ["module_root", "prebuilds", platformDirectory, "bindings"],
    ["module_root", "build", "Release", "bindings"],
  ],
});

export default { ...addon };
