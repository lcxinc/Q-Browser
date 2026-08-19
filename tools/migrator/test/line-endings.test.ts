import { readFile } from "node:fs/promises";
import path from "node:path";
import { describe, expect, test } from "vitest";

const repositoryRoot = path.resolve(import.meta.dirname, "../../..");

describe("migrator line ending contract", () => {
  test("declares and preserves LF for migrator sources, fixtures, and goldens", async () => {
    const attributes = await readFile(path.join(repositoryRoot, ".gitattributes"), "utf8");
    expect(attributes).toContain("/tools/migrator/** text eol=lf");
    expect(attributes).toContain("/fixtures/migration/** text eol=lf");
    expect(attributes).toContain("/tests/golden/migrator/** text eol=lf");
    for (const relative of [
      "tools/migrator/src/cli.ts",
      "fixtures/migration/dashboard/index.html",
      "tests/golden/migrator/dashboard/Main.qml",
      "tests/golden/migrator/dashboard/report.json",
    ]) {
      expect(await readFile(path.join(repositoryRoot, relative), "utf8"), relative).not.toContain("\r");
    }
  });
});
