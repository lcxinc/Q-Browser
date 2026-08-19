import { access, appendFile, mkdir, readFile, readdir, rename, rm, symlink, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { afterEach, describe, expect, test } from "vitest";

import { generateProject, publishGenerationTransaction, publishNewFile } from "../src/generator.ts";
import { scanDocument, scanFile } from "../src/scanner.ts";
import { setStableIoTestHooks } from "../src/stable-io.ts";

afterEach(() => setStableIoTestHooks());

describe("migrator stable IO", () => {
  test.runIf(process.platform === "win32")("holds a deny-write/delete lock before exposing the initial-stat hook", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-lock-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    const original = "<!doctype html><main>AAAAAAAA</main>";
    let sharingViolation = false;
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, original, "utf8");
      setStableIoTestHooks({
        afterInputInitialStat: async () => {
          try { await writeFile(input, "<!doctype html><main>BBBBBBBB</main>", "utf8"); }
          catch (error) {
            sharingViolation = error instanceof Error && "code" in error
              && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
          }
        },
      });
      const scanned = await scanFile(input);
      expect(sharingViolation).toBe(true);
      expect(JSON.stringify(scanned)).toContain("AAAAAAAA");
      expect(await readFile(input, "utf8")).toBe(original);
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("cleans up a crashed or timed-out input lock helper", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-helper-failure-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><main>Safe</main>", "utf8");
      setStableIoTestHooks({
        afterInputLockReady: async (_opened, processId) => {
          process.kill(processId);
          await new Promise((resolve) => setTimeout(resolve, 100));
        },
      });
      await expect(scanFile(input)).rejects.toThrow("LOCK_HELPER_EXITED");
      await writeFile(input, "<!doctype html><main>After crash</main>", "utf8");

      setStableIoTestHooks({
        beforeInputLockReadyAcceptance: async () => new Promise<void>(() => {}),
        lockHelperReadyTimeoutMs: 100,
      });
      await expect(scanFile(input)).rejects.toThrow("LOCK_HELPER_TIMEOUT");
      await writeFile(input, "<!doctype html><main>After timeout</main>", "utf8");
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("retries a bounded pre-READY helper crash without exposing input", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-helper-retry-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><main>Stable retry</main>", "utf8");
      const attempts: number[] = [];
      setStableIoTestHooks({
        afterLockHelperSpawn: async (_target, attempt, processId) => {
          attempts.push(attempt);
          if (attempt === 0) process.kill(processId);
        },
      });
      expect(JSON.stringify(await scanFile(input))).toContain("Stable retry");
      expect(attempts).toEqual([0, 1]);
      await rename(input, path.join(root, "released.html"));
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("releases helpers after aborts and handles consecutive Unicode long-path scans", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-unicode-${process.pid}-${Date.now()}`);
    const longParent = path.join(root, "界面-😀", ...Array.from({ length: 9 }, (_, index) => `segment-${index}-${"x".repeat(24)}`));
    const input = path.join(longParent, "页面.html");
    const invalid = path.join(root, "invalid.html");
    try {
      await mkdir(longParent, { recursive: true });
      await writeFile(input, "<!doctype html><main>稳定</main>", "utf8");
      for (let iteration = 0; iteration < 6; iteration += 1) {
        expect(JSON.stringify(await scanFile(input))).toContain("稳定");
      }
      await writeFile(invalid, new Uint8Array([0xff, 0xfe, 0xfd]));
      await expect(scanFile(invalid)).rejects.toThrow("INVALID_UTF8");
      await rename(invalid, path.join(root, "abort-released.html"));
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  }, 15_000);

  test("rejects direct bidirectional output/report overlap before creating parents", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-direct-overlap-${process.pid}-${Date.now()}`);
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      await expect(publishGenerationTransaction(
        path.join(root, "report-root", "output"),
        path.join(root, "report-root"),
        generated,
      )).rejects.toThrow("OUTPUT_REPORT_OVERLAP");
      await expect(access(root)).rejects.toThrow();
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("rejects repeated same-size input overwrites after the initial metadata snapshot", async () => {
    for (let iteration = 0; iteration < 8; iteration += 1) {
      const root = path.join(os.tmpdir(), `qbrowser-input-same-size-${process.pid}-${Date.now()}-${iteration}`);
      const input = path.join(root, "index.html");
      const original = "<!doctype html><main>AAAAAAAA</main>";
      const replacement = "<!doctype html><main>BBBBBBBB</main>";
      try {
        await mkdir(root, { recursive: true });
        await writeFile(input, original, "utf8");
        let sharingViolation = false;
        setStableIoTestHooks({
          afterInputInitialStat: async (opened: string) => {
            if (opened !== input) return;
            try { await writeFile(input, replacement, "utf8"); }
            catch (error) {
              sharingViolation = error instanceof Error && "code" in error
                && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
            }
          },
        });
        if (process.platform === "win32") {
          expect(JSON.stringify(await scanFile(input))).toContain("AAAAAAAA");
          expect(sharingViolation).toBe(true);
        } else {
          await expect(scanFile(input)).rejects.toThrow("INPUT_CHANGED");
        }
      } finally {
        setStableIoTestHooks();
        await rm(root, { recursive: true, force: true });
      }
    }
  }, 15_000);

  test("rejects same-size A/B changes across two complete snapshots for HTML and CSS", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-two-pass-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    const css = path.join(root, "site.css");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><link rel='stylesheet' href='site.css'><main>AAAAAAAA</main>", "utf8");
      await writeFile(css, "main { color: #111111; }", "utf8");
      let htmlSharingViolation = false;
      setStableIoTestHooks({
        betweenInputSnapshots: async (opened: string) => {
          if (opened !== input) return;
          try { await writeFile(input, "<!doctype html><link rel='stylesheet' href='site.css'><main>BBBBBBBB</main>", "utf8"); }
          catch (error) {
            htmlSharingViolation = error instanceof Error && "code" in error
              && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
          }
        },
      });
      if (process.platform === "win32") {
        expect(JSON.stringify(await scanFile(input))).toContain("AAAAAAAA");
        expect(htmlSharingViolation).toBe(true);
      } else {
        await expect(scanFile(input)).rejects.toThrow("INPUT_CHANGED");
      }

      let cssSharingViolation = false;
      setStableIoTestHooks({
        betweenInputSnapshots: async (opened: string) => {
          if (opened !== css) return;
          try { await writeFile(css, "main { color: #222222; }", "utf8"); }
          catch (error) {
            cssSharingViolation = error instanceof Error && "code" in error
              && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
          }
        },
      });
      if (process.platform === "win32") {
        expect(JSON.stringify(await scanFile(input))).toContain("#111111");
        expect(cssSharingViolation).toBe(true);
      } else {
        await expect(scanFile(input)).rejects.toThrow("INPUT_CHANGED");
      }
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("rejects an input name swapped after its native handle opens", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-swap-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><main>Original</main>", "utf8");
      let renameBlocked = false;
      setStableIoTestHooks({
        afterInputOpen: async (opened) => {
          if (opened !== input) return;
          try {
            await rename(input, path.join(root, "original.html"));
            await writeFile(input, "<!doctype html><main>Replacement</main>", "utf8");
          } catch (error) {
            renameBlocked = error instanceof Error && "code" in error
              && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
          }
        },
      });
      if (process.platform === "win32") {
        expect(JSON.stringify(await scanFile(input))).toContain("Original");
        expect(renameBlocked).toBe(true);
      } else {
        await expect(scanFile(input)).rejects.toThrow("INPUT_IDENTITY_CHANGED");
      }
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
      let sharingViolation = false;
      setStableIoTestHooks({ afterInputOpen: async () => {
        try { await appendFile(input, "x".repeat(2_097_152), "utf8"); }
        catch (error) {
          sharingViolation = error instanceof Error && "code" in error
            && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
        }
      } });
      if (process.platform === "win32") {
        expect(JSON.stringify(await scanFile(input))).toContain("Small");
        expect(sharingViolation).toBe(true);
      } else {
        await expect(scanFile(input)).rejects.toThrow("INPUT_LIMIT_EXCEEDED");
      }
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("rejects truncation after the initial metadata snapshot", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-input-truncate-${process.pid}-${Date.now()}`);
    const input = path.join(root, "index.html");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(input, "<!doctype html><main>Original content</main>", "utf8");
      let sharingViolation = false;
      setStableIoTestHooks({ afterInputInitialStat: async () => {
        try { await writeFile(input, "<!doctype html><main>x</main>", "utf8"); }
        catch (error) {
          sharingViolation = error instanceof Error && "code" in error
            && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
        }
      } });
      if (process.platform === "win32") {
        expect(JSON.stringify(await scanFile(input))).toContain("Original content");
        expect(sharingViolation).toBe(true);
      } else {
        await expect(scanFile(input)).rejects.toThrow("INPUT_CHANGED");
      }
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

  test("rolls back only its owned output after a report race", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-output-race-${process.pid}-${Date.now()}`);
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      setStableIoTestHooks({
        beforeReportPublish: async () => {
          await writeFile(report, "replacement", { encoding: "utf8", flag: "wx" });
        },
        beforeOutputRollback: async () => {
          await rename(output, path.join(root, "owned-displaced"));
          await mkdir(output);
          await writeFile(path.join(output, "competitor.txt"), "competitor", "utf8");
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
      setStableIoTestHooks({ beforeOutputCommit: async () => mkdir(output) });
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

  test("removes owned missing parent chains when staging fails before publication", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-parent-cleanup-${process.pid}-${Date.now()}`);
    const output = path.join(root, "nested", "output-parent", "output");
    const report = path.join(root, "nested", "report-parent", "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      setStableIoTestHooks({ afterGenerationStaged: async () => { throw new Error("INJECTED_STAGE_FAILURE"); } });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("INJECTED_STAGE_FAILURE");
      await expect(access(path.join(root, "nested"))).rejects.toThrow();
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test("fails closed when a parent is swapped after the final checks and before publish", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-final-publish-race-${process.pid}-${Date.now()}`);
    const outputParent = path.join(root, "output-parent");
    const reportParent = path.join(root, "report-parent");
    const displaced = path.join(root, "output-displaced");
    const attacker = path.join(root, "attacker");
    const output = path.join(outputParent, "output");
    const report = path.join(reportParent, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      await mkdir(outputParent, { recursive: true });
      await mkdir(reportParent, { recursive: true });
      await mkdir(attacker, { recursive: true });
      await writeFile(path.join(attacker, "sentinel.txt"), "competitor", "utf8");
      setStableIoTestHooks({
        beforeOutputCommit: async () => {
          await rename(outputParent, displaced);
          await symlink(attacker, outputParent, process.platform === "win32" ? "junction" : "dir");
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_PARENT_CHANGED");
      expect(await readFile(path.join(attacker, "sentinel.txt"), "utf8")).toBe("competitor");
      await expect(access(path.join(attacker, "output"))).rejects.toThrow();
      await expect(access(report)).rejects.toThrow();
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("holds native child handles that deny parent rename through both publish operations", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-parent-lease-${process.pid}-${Date.now()}`);
    const outputParent = path.join(root, "output-parent");
    const reportParent = path.join(root, "report-parent");
    const output = path.join(outputParent, "output");
    const report = path.join(reportParent, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    let outputRenameBlocked = false;
    let reportRenameBlocked = false;
    try {
      await mkdir(outputParent, { recursive: true });
      await mkdir(reportParent, { recursive: true });
      setStableIoTestHooks({
        beforeOutputCommit: async () => {
          try {
            const displaced = path.join(root, "output-displaced");
            await rename(outputParent, displaced);
            await rename(displaced, outputParent);
          } catch (error) {
            outputRenameBlocked = error instanceof Error && "code" in error && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
          }
        },
        beforeReportPublish: async () => {
          try {
            const displaced = path.join(root, "report-displaced");
            await rename(reportParent, displaced);
            await rename(displaced, reportParent);
          } catch (error) {
            reportRenameBlocked = error instanceof Error && "code" in error && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code));
          }
        },
      });
      await publishGenerationTransaction(output, report, generated);
      expect({ outputRenameBlocked, reportRenameBlocked }).toEqual({ outputRenameBlocked: true, reportRenameBlocked: true });
      expect(await readdir(outputParent)).toEqual(["output"]);
      expect(await readdir(reportParent)).toEqual(["report.json"]);
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("locks both parents without children before staging and rejects a swap between acquisitions", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-parent-lock-order-${process.pid}-${Date.now()}`);
    const outputParent = path.join(root, "output-parent");
    const reportParent = path.join(root, "report-parent");
    const displaced = path.join(root, "report-displaced");
    const output = path.join(outputParent, "output");
    const report = path.join(reportParent, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      await mkdir(outputParent, { recursive: true });
      await mkdir(reportParent, { recursive: true });
      setStableIoTestHooks({
        afterFirstDirectoryLockReady: async () => {
          expect(await readdir(outputParent)).toEqual([]);
          await rename(reportParent, displaced);
          await mkdir(reportParent);
          await writeFile(path.join(reportParent, "sentinel.txt"), "competitor", "utf8");
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("OUTPUT_PARENT_CHANGED");
      expect(await readdir(outputParent)).toEqual([]);
      expect(await readdir(reportParent)).toEqual(["sentinel.txt"]);
      expect(await readFile(path.join(reportParent, "sentinel.txt"), "utf8")).toBe("competitor");
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("cleans directory helpers after a crash or readiness timeout without creating children", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-parent-helper-failure-${process.pid}-${Date.now()}`);
    const outputParent = path.join(root, "output-parent");
    const reportParent = path.join(root, "report-parent");
    const output = path.join(outputParent, "output");
    const report = path.join(reportParent, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      await mkdir(outputParent, { recursive: true });
      await mkdir(reportParent, { recursive: true });
      setStableIoTestHooks({
        afterFirstDirectoryLockReady: async (_outputParent, _reportParent, processId) => {
          if (processId === undefined) throw new Error("missing helper process id");
          process.kill(processId);
          await new Promise((resolve) => setTimeout(resolve, 100));
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("LOCK_HELPER_EXITED");
      expect(await readdir(outputParent)).toEqual([]);
      expect(await readdir(reportParent)).toEqual([]);

      setStableIoTestHooks({
        beforeDirectoryLockReadyAcceptance: async (directory) => {
          if (directory === outputParent) await new Promise<void>(() => {});
        },
        lockHelperReadyTimeoutMs: 100,
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("LOCK_HELPER_TIMEOUT");
      expect(await readdir(outputParent)).toEqual([]);
      expect(await readdir(reportParent)).toEqual([]);
      await rename(outputParent, path.join(root, "output-released"));
      await rename(reportParent, path.join(root, "report-released"));
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("rolls back missing scan-output parents when the directory helper cannot become ready", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-scan-parent-helper-${process.pid}-${Date.now()}`);
    const target = path.join(root, "missing", "nested", "ir.json");
    try {
      setStableIoTestHooks({
        beforeDirectoryLockReadyAcceptance: async () => new Promise<void>(() => {}),
        lockHelperReadyTimeoutMs: 100,
      });
      await expect(publishNewFile(target, "{}\n")).rejects.toThrow("LOCK_HELPER_TIMEOUT");
      await expect(access(root)).rejects.toThrow();
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });

  test.runIf(process.platform === "win32")("releases both parent helpers even when one cleanup callback fails", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-parent-release-failure-${process.pid}-${Date.now()}`);
    const outputParent = path.join(root, "output-parent");
    const reportParent = path.join(root, "report-parent");
    const output = path.join(outputParent, "output");
    const report = path.join(reportParent, "report.json");
    const generated = generateProject(scanDocument({ sourceFile: "safe.html", html: "<!doctype html><main>Safe</main>" }));
    try {
      await mkdir(outputParent, { recursive: true });
      await mkdir(reportParent, { recursive: true });
      setStableIoTestHooks({
        afterGenerationStaged: async () => { throw new Error("INJECTED_STAGE_FAILURE"); },
        beforeLockRelease: async (target) => {
          if (target === reportParent) throw new Error("INJECTED_RELEASE_FAILURE");
        },
      });
      await expect(publishGenerationTransaction(output, report, generated)).rejects.toThrow("INJECTED_RELEASE_FAILURE");
      expect(await readdir(outputParent)).toEqual([]);
      expect(await readdir(reportParent)).toEqual([]);
      await rename(outputParent, path.join(root, "output-released"));
      await rename(reportParent, path.join(root, "report-released"));
    } finally {
      setStableIoTestHooks();
      await rm(root, { recursive: true, force: true });
    }
  });
});
