import binding from "bindings";
import { arch, platform } from "node:os";

export type LoopbackCapture = {
  /** Terminal native capture failure, or null while idle/healthy. Read before stop(), which clears the session. */
  readonly lastError: string | null;
  /** Capture a process and all its descendants, including streams created after capture starts. Linux ownership detection is best effort. */
  start(processId: number, callback: (chunk: Buffer) => void): void;
  stop: () => void;
  /** Capture the default system output, or pass excludeProcessId to mix playback streams outside that process tree across devices. Linux skips streams whose ownership cannot be determined. */
  startSystemAudio: (callback: (chunk: Buffer) => void, excludeProcessId?: number) => void;
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
