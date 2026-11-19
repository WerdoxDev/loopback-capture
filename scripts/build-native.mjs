import { spawn } from "node:child_process";
import { copyFile, mkdir, rename, rm } from "node:fs/promises";
import { dirname, join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url));
const destination = join(root, "build", "Release", "loopback_capture_addon.node");
const clean = process.argv.slice(2).includes("--clean");

if (!clean && !["linux", "win32"].includes(process.platform)) {
  throw new Error("loopback-capture supports Windows and Linux only");
}

// Never leave an old addon masquerading as the result of a failed build.
await rm(destination, { force: true });
const args = clean
  ? ["clean"]
  : ["build", "--release", "--locked", "--message-format=json-render-diagnostics"];
const cargo = spawn("cargo", args, {
  cwd: root,
  stdio: ["inherit", "pipe", "inherit"],
});
let artifact;
const output = createInterface({ input: cargo.stdout });
output.on("line", (line) => {
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    console.log(line);
    return;
  }
  if (
    message.reason === "compiler-artifact" &&
    message.target.name === "loopback_capture_addon" &&
    message.target.crate_types.includes("cdylib")
  ) {
    artifact = message.filenames.find((path) => /\.(dll|so)$/.test(path));
  }
});

const exitCode = await new Promise((resolve, reject) => {
  cargo.on("error", reject);
  cargo.on("close", (code) => resolve(code ?? 1));
});
if (exitCode !== 0) process.exit(exitCode);
if (!clean) {
  if (!artifact) throw new Error("Cargo did not produce the loopback_capture_addon dynamic library");
  await mkdir(dirname(destination), { recursive: true });
  const temporary = `${destination}.tmp`;
  await copyFile(artifact, temporary);
  await rename(temporary, destination);
  console.log(`Built ${destination}`);
}
