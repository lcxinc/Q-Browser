import { access, appendFile, mkdir, readFile, readdir, rename, rm, symlink, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { afterEach, describe, expect, test } from "vitest";

import { generateProject, publishGenerationTransaction } from "../src/generator.ts";
import { scanDocument, scanFile } from "../src/scanner.ts";
import { setStableIoTestHooks } from "../src/stable-io.ts";

afterEach(() => setStableIoTestHooks());

describe("migrator stable IO", () => {
  test("rejects an input name swapped after its native handle opens", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-swap-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><main>Original</main>", "utf8");
      setStableIoTestHooks({
        afterInputOpen: async (opened) => {
          if (opened !== input) return;
          await rename(input, path.join(root, "original.html"));
          await writeFile(input, "<!doctype html><main>Replacement</main>", "utf8");
        },
      });
      await expect(scanFile(input)).rejects.toThrow("INPUT_IDENTITY_CHANGED");
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("reads max plus one from the handle and rejects growth", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-growth-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><main>Small</main>", "utf8");
      setStableIoTestHooks({ afterInputOpen: async () => appendFile(input, "x".repeat(2_097_152), "utf8") });
      await expect(scanFile(input)).rejects.toThrow("INPUT_LIMIT_EXCEEDED");
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("rejects HTML and CSS dependencies through a symlink or Windows junction", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-reparse-${process.pid}-${Date.now()}`);
    const real = path.join(root, "real");
    const linked = path.join(root, "linked");
    const outside = path.join(root, "outside");
    try {
      await mkdir(real, { recursive: true });
      await mkdir(outside, { recursive: true });
      await writeFile(path.join(real, "index.html"), "<!doctype html><main>Safe</main>", "utf8");
      await symlink(real, linked, process.platform === "win32" ? "junction" : "dir");
      await expect(scanFile(path.join(linked, "index.html"))).rejects.toThrow("UNSAFE_INPUT_PATH");

      await writeFile(path.join(real, "with-css.html"), "<!doctype html><link rel='stylesheet' href='styles/site.css'><main>Safe</main>", "utf8");
      await writeFile(path.join(outside, "site.css"), "main { display:flex }", "utf8");
      await symlink(outside, path.join(real, "styles"), process.platform === "win32" ? "junction" : "dir");
      await expect(scanFile(path.join(real, "with-css.html"))).rejects.toThrow("UNSAFE_INPUT_PATH");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("stages both targets before a report race and preserves the competitor", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-report-race-${process.pid}-${Date.now()}`);
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      setStableIoTestHooks({ beforeReportPublish: async () => writeFile(report, "competitor", { encoding: "utf8", flag: "wx" }) });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_ALREADY_EXISTS");
      await expect(access(output)).rejects.toThrow();
      expect(await readFile(report, "utf8")).toBe("competitor");
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("rolls back only its owned report after an output race", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-output-race-${process.pid}-${Date.now()}`);
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      setStableIoTestHooks({
        afterReportPublish: async () => {
          await mkdir(output);
          await writeFile(path.join(output, "competitor.txt"), "competitor", "utf8");
        },
        beforeReportRollback: async () => {
          await rm(report, { force: true });
          await writeFile(report, "replacement", { encoding: "utf8", flag: "wx" });
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_ALREADY_EXISTS");
      expect(await readFile(path.join(output, "competitor.txt"), "utf8")).toBe("competitor");
      expect(await readFile(report, "utf8")).toBe("replacement");
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("does not replace even an empty directory created at the output target", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-empty-output-race-${process.pid}-${Date.now()}`);
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      setStableIoTestHooks({ afterReportPublish: async () => mkdir(output) });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_ALREADY_EXISTS");
      expect(await readdir(output)).toEqual([]);
      await expect(access(report)).rejects.toThrow();
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("does not follow or clean through a parent replaced after staging", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-parent-race-${process.pid}-${Date.now()}`);
    const parent = path.join(root, "parent");
    const displaced = path.join(root, "displaced");
    const attacker = path.join(root, "attacker");
    const output = path.join(parent, "output");
    const report = path.join(parent, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      await mkdir(parent, { recursive: true });
      await mkdir(attacker, { recursive: true });
      await writeFile(path.join(attacker, "sentinel.txt"), "attacker", "utf8");
      setStableIoTestHooks({
        afterGenerationStaged: async () => {
          await rename(parent, displaced);
          await symlink(attacker, parent, process.platform === "win32" ? "junction" : "dir");
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_PARENT_CHANGED");
      expect(await readFile(path.join(attacker, "sentinel.txt"), "utf8")).toBe("attacker");
      await expect(access(path.join(attacker, "output"))).rejects.toThrow();
      await expect(access(path.join(attacker, "report.json"))).rejects.toThrow();
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("does not publish or delete a competitor that replaces an owned random stage", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-stage-race-${process.pid}-${Date.now()}`);
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    const displaced = path.join(root, "owned-displaced");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    let competitorStage = "";
    try {
      setStableIoTestHooks({
        afterGenerationStaged: async () => {
          const name = (await readdir(root)).find((entry) => entry.startsWith(".output.qbrowser-") && entry.endsWith(".stage"));
          if (!name) throw new Error("stage not found");
          competitorStage = path.join(root, name);
          await rename(competitorStage, displaced);
          await mkdir(competitorStage);
          await writeFile(path.join(competitorStage, "competitor.txt"), "competitor", "utf8");
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_STAGE_CHANGED");
      expect(await readFile(path.join(competitorStage, "competitor.txt"), "utf8")).toBe("competitor");
      await expect(access(output)).rejects.toThrow();
      await expect(access(report)).rejects.toThrow();
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });
});
