import { open } from "node:fs/promises";

const binaries = [
  {
    path: "prebuilds/linux-x64/loopback_capture_addon.node",
    validate(header) {
      const isElf64 =
        header.subarray(0, 4).equals(Buffer.from([0x7f, 0x45, 0x4c, 0x46])) && header[4] === 2;
      const isX64 = header.readUInt16LE(18) === 0x3e;
      return isElf64 && isX64;
    },
    description: "64-bit x86 ELF",
  },
  {
    path: "prebuilds/win32-x64/loopback_capture_addon.node",
    validate(header) {
      if (header.toString("ascii", 0, 2) !== "MZ") return false;

      const peOffset = header.readUInt32LE(0x3c);
      return (
        peOffset + 6 <= header.length &&
        header.toString("binary", peOffset, peOffset + 4) === "PE\0\0" &&
        header.readUInt16LE(peOffset + 4) === 0x8664
      );
    },
    description: "64-bit x86 Windows PE",
  },
];

for (const binary of binaries) {
  let file;
  try {
    file = await open(binary.path, "r");
    const header = Buffer.alloc(4096);
    const { bytesRead } = await file.read(header, 0, header.length, 0);
    const contents = header.subarray(0, bytesRead);

    if (!binary.validate(contents)) {
      throw new Error(`${binary.path} is not a ${binary.description} binary`);
    }
  } catch (error) {
    if (error?.code === "ENOENT") {
      throw new Error(`Missing required prebuild: ${binary.path}`);
    }
    throw error;
  } finally {
    await file?.close();
  }

  console.log(`Verified ${binary.path}`);
}
