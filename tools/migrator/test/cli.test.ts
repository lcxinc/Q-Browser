import { access, mkdir, readFile, rm, symlink, writeFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { describe, expect, test } from "vitest";

const repositoryRoot = path.resolve(import.meta.dirname, "../../..");
const cli = path.join(repositoryRoot, "tools/migrator/src/cli.ts");
const workspaceBin = path.join(repositoryRoot, "tools/node_modules/.bin", process.platform === "win32" ? "qbrowser-migrate.cmd" : "qbrowser-migrate");
const input = path.join(repositoryRoot, "fixtures/migration/dashboard/index.html");

function launch(args: string[]) {
  return spawnSync(process.execPath, [cli, ...args], {
    cwd: repositoryRoot,
    encoding: "utf8",
    env: {
      ...process.env,
      PATH: "C:\\Windows\\System32",
      HTTP_PROXY: "http://127.0.0.1:1",
      HTTPS_PROXY: "http://127.0.0.1:1",
      LANG: "tr_TR.UTF-8",
      TZ: "Pacific/Kiritimati",
    },
    timeout: 10_000,
  });
}

function launchWorkspaceBin(args: string[]) {
  const executable = process.platform === "win32" ? (process.env.ComSpec ?? "cmd.exe") : workspaceBin;
  const executableArgs = process.platform === "win32"
    ? ["/d", "/c", workspaceBin, ...args]
    : args;
  return spawnSync(executable, executableArgs, {
    cwd: repositoryRoot,
    encoding: "utf8",
    timeout: 10_000,
  });
}

describe("qbrowser-migrate CLI", () => {
  test("launches as a real process and emits deterministic scan/generate files", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-cli-${process.pid}-${Date.now()}`);
    try {
      const scanA = launch(["scan", input, "--json", path.join(root, "a.json")]);
      const scanB = launch(["scan", input, "--json", path.join(root, "b.json")]);
      expect({ status: scanA.status, stdout: scanA.stdout, stderr: scanA.stderr }).toEqual({
        status: 0, stdout: "Scanned index.html: 0 diagnostic(s)\n", stderr: "",
      });
      expect(scanB.status).toBe(0);
      expect(await readFile(path.join(root, "a.json"))).toEqual(await readFile(path.join(root, "b.json")));

      const generateA = launch(["generate", input, "--output", path.join(root, "out-a"), "--report", path.join(root, "report-a.json")]);
      const generateB = launch(["generate", input, "--output", path.join(root, "out-b"), "--report", path.join(root, "report-b.json")]);
      expect({ status: generateA.status, stdout: generateA.stdout, stderr: generateA.stderr }).toEqual({
        status: 0, stdout: "Generated 1 file(s): 0 diagnostic(s)\n", stderr: "",
      });
      expect(generateB.status).toBe(0);
      expect(await readFile(path.join(root, "out-a/Main.qml"))).toEqual(await readFile(path.join(root, "out-b/Main.qml")));
      expect(await readFile(path.join(root, "report-a.json"))).toEqual(await readFile(path.join(root, "report-b.json")));
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("uses stable usage errors and exit code 64", () => {
    const result = launch(["generate", input]);
    expect(result.status).toBe(64);
    expect(result.stdout).toBe("");
    expect(result.stderr).toContain("qbrowser-migrate: INVALID_ARGUMENTS\nUsage:\n");
  });

  test("preflights report collisions before publishing an output directory", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-cli-collision-${process.pid}-${Date.now()}`);
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(report, "sentinel", "utf8");
      const result = launch(["generate", input, "--output", output, "--report", report]);
      expect(result.status).toBe(1);
      expect(result.stderr).toContain("OUTPUT_ALREADY_EXISTS");
      await expect(access(output)).rejects.toThrow();
      expect(await readFile(report, "utf8")).toBe("sentinel");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("rejects scan JSON beneath a symlink or Windows junction ancestor", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-cli-reparse-${process.pid}-${Date.now()}`);
    const real = path.join(root, "real");
    const linked = path.join(root, "linked");
    try {
      await mkdir(path.join(real, "existing"), { recursive: true });
      await symlink(real, linked, process.platform === "win32" ? "junction" : "dir");
      const result = launch(["scan", input, "--json", path.join(linked, "existing", "ir.json")]);
      expect(result.status).toBe(1);
      expect(result.stderr).toContain("OUTPUT_REPARSE_POINT");
      await expect(access(path.join(real, "existing", "ir.json"))).rejects.toThrow();
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("runs through the npm workspace bin shim", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-cli-bin-${process.pid}-${Date.now()}`);
    try {
      const result = launchWorkspaceBin(["scan", input, "--json", path.join(root, "ir.json")]);
      expect({ status: result.status, stdout: result.stdout, stderr: result.stderr }).toEqual({
        status: 0, stdout: "Scanned index.html: 0 diagnostic(s)\n", stderr: "",
      });
      expect(JSON.parse(await readFile(path.join(root, "ir.json"), "utf8"))).toMatchObject({ version: 1, sourceFile: "index.html" });
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("returns exit 2 and publishes nothing for fatal diagnostics", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-cli-fatal-${process.pid}-${Date.now()}`);
    const fatalInput = path.join(root, "fatal.html");
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    try {
      await mkdir(root, { recursive: true });
      await writeFile(fatalInput, `<!doctype html>${"<script></script>".repeat(510)}`, "utf8");
      const result = launch(["generate", fatalInput, "--output", output, "--report", report]);
      expect(result.status).toBe(2);
      expect(result.stderr).toBe("");
      await expect(access(output)).rejects.toThrow();
      await expect(access(report)).rejects.toThrow();
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  test("fatal diagnostics take precedence and leave pre-existing targets unchanged", async () => {
    const root = path.join(os.tmpdir(), `qbrowser-cli-fatal-existing-${process.pid}-${Date.now()}`);
    const fatalInput = path.join(root, "fatal.html");
    const output = path.join(root, "output");
    const report = path.join(root, "report.json");
    try {
      await mkdir(output, { recursive: true });
      await writeFile(path.join(output, "sentinel.txt"), "output-sentinel", "utf8");
      await writeFile(report, "report-sentinel", "utf8");
      await writeFile(fatalInput, `<!doctype html>${"<script></script>".repeat(510)}`, "utf8");
      const result = launch(["generate", fatalInput, "--output", output, "--report", report]);
      expect({ status: result.status, stdout: result.stdout, stderr: result.stderr }).toEqual({
        status: 2,
        stdout: "Generation blocked: 500 diagnostic(s)\n",
        stderr: "",
      });
      expect(await readFile(path.join(output, "sentinel.txt"), "utf8")).toBe("output-sentinel");
      expect(await readFile(report, "utf8")).toBe("report-sentinel");
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });
});
