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
    const toolsManifest = JSON.parse(await readFile(path.join(migratorRoot, "../package.json"), "utf8")) as {
      dependencies?: Record<string, string>;
      scripts?: Record<string, string>;
    };
    expect(toolsManifest.dependencies).toEqual({ entities: "8.0.0" });
    expect(toolsManifest.scripts?.build).toContain("npm rebuild --workspace @q-browser/migrator --ignore-scripts");
    await access(path.join(migratorRoot, "dist/cli.js"));
    await access(path.join(migratorRoot, "stable-lock-helper.ps1"));
    const files = await readdir(path.join(migratorRoot, "dist"), { recursive: true });
    expect(files.some((file) => /(?:test|tsbuildinfo|\.map$)/u.test(String(file)))).toBe(false);
  });
});
