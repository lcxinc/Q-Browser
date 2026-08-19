import { access, readFile, readdir } from "node:fs/promises";
import path from "node:path";
import { describe, expect, test } from "vitest";

const migratorRoot = path.resolve(import.meta.dirname, "..");

describe("migrator production package", () => {
  test("publishes only built runtime files with exact runtime dependencies", async () => {
    const manifest = JSON.parse(await readFile(path.join(migratorRoot, "package.json"), "utf8")) as {
      bin?: Record<string, string>;
      files?: string[];
      engines?: Record<string, string>;
      dependencies?: Record<string, string>;
    };
    expect(manifest.bin).toEqual({ "qbrowser-migrate": "dist/cli.js" });
    expect(manifest.files).toEqual(["dist", "stable-lock-helper.ps1"]);
    expect(manifest.engines).toEqual({ node: ">=24" });
    expect(manifest.dependencies).toEqual({ "css-tree": "3.2.1", entities: "8.0.0", parse5: "8.0.1" });
    expect((manifest as { scripts?: Record<string, string> }).scripts).toMatchObject({
      build: "npm run clean && tsc -b --force",
      prepack: "npm run build",
    });
    const toolsManifest = JSON.parse(await readFile(path.join(migratorRoot, "../package.json"), "utf8")) as {
      dependencies?: Record<string, string>;
      scripts?: Record<string, string>;
    };
    expect(toolsManifest.dependencies).toEqual({ entities: "8.0.0" });
    expect(toolsManifest.scripts?.build).toBe("npm run clean --workspace @q-browser/migrator && tsc -b --force && npm rebuild --workspace @q-browser/migrator --ignore-scripts");
    await access(path.join(migratorRoot, "dist/cli.js"));
    await access(path.join(migratorRoot, "stable-lock-helper.ps1"));
    const files = await readdir(path.join(migratorRoot, "dist"), { recursive: true });
    expect(files.some((file) => /(?:test|tsbuildinfo|\.map$)/u.test(String(file)))).toBe(false);
    const productionSurface = (await Promise.all(files
      .map(String)
      .filter((file) => /\.(?:js|d\.ts)$/u.test(file))
      .map((file) => readFile(path.join(migratorRoot, "dist", file), "utf8"))))
      .join("\n");
    expect(files.map(String)).not.toContain("stable-io-checkpoint.js");
    expect(files.map(String)).not.toContain("stable-io-checkpoint.d.ts");
    expect(productionSurface).not.toMatch(
      /setStableIoTestHooks|runStableIoHook|StableIoTestHooks|ioCheckpoint|ioReadyTimeout|stable-io-test-control/u,
    );
  });
});
