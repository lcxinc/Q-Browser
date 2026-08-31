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

describe("release acceptance script", () => {
  async function releaseFunction(name: string): Promise<string> {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    const start = script.indexOf(`function ${name}(`);
    const end = script.indexOf("\nfunction ", start + 1);
    expect(start, `${name} must exist`).toBeGreaterThanOrEqual(0);
    return script.slice(start, end < 0 ? undefined : end);
  }

  test("does not shadow PowerShell's read-only Host automatic variable", async () => {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    expect(script).not.toMatch(/function\s+[\w-]+\([^)]*\$Host(?:\W|$)/su);
  });

  test("guards optional mock request records under StrictMode", async () => {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    expect(script).not.toContain("$null -ne $message.request");
    expect(script.match(/\$message\.PSObject\.Properties\['request'\]/gu)).toHaveLength(2);
  });

  test("adds optional manifest permissions under StrictMode", async () => {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    expect(script).toContain("Add-Member -NotePropertyName clipboardRead");
    expect(script).not.toContain("$manifest.permissions.clipboardRead = 'user-gesture'");
  });

  test("imports GetCurrentThreadId from kernel32", async () => {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    expect(script).toContain(
      '[DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();',
    );
    expect(script).not.toContain(
      '[DllImport("user32.dll")] static extern uint GetCurrentThreadId();',
    );
  });

  test("derives embedded Worker interaction points from its client size", async () => {
    const business = await releaseFunction("Invoke-DeployedPilotBusinessAcceptance");
    const clipboard = await releaseFunction("Invoke-DeployedClipboardAcceptance");
    expect(business).toContain("ClientHeight($window)");
    expect(clipboard).toContain("ClientWidth($window)");
    expect(clipboard).toContain("ClientHeight($window)");
    expect(business).not.toMatch(/Click\(\$window,\s*900,\s*634\)/u);
    expect(clipboard).not.toMatch(/Click\(\$window,\s*550,\s*360\)/u);
  });

  test("revalidates focus and bounds retries for a missed native file click", async () => {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    const business = await releaseFunction("Invoke-DeployedPilotBusinessAcceptance");
    expect(script).toMatch(
      /if \(!FocusWorker\(worker\)\) return false;\r?\n      Input down = new Input\(\); down\.type = InputMouse;/u,
    );
    expect(business).toContain("$fileCancelAttempt -lt 2");
  });

  test("samples composited Worker pixels from the desktop DC", async () => {
    const script = await readFile(
      path.resolve(migratorRoot, "../..", "scripts/build-release.ps1"),
      "utf8",
    );
    const business = await releaseFunction("Invoke-DeployedPilotBusinessAcceptance");
    expect(script.match(/GetDC\(IntPtr\.Zero\)/gu)?.length ?? 0).toBeGreaterThanOrEqual(2);
    expect(script).toContain("byte[] pixels = CaptureClient(window);");
    expect(script).toContain("System.Threading.Thread.Sleep(250);");
    expect(script.match(/System\.Threading\.Thread\.Sleep\(50\);/gu)).toHaveLength(2);
    expect(script).toContain("SendMessageTextW(information.focus");
    expect(script).not.toContain("GetWindowTextW(information.focus");
    expect(script).toContain(
      '[DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageW(',
    );
    expect(script).toContain("bool posted = readbackMatches && accept");
    expect(script).toContain("readbackMatch=");
    expect(business).toContain("$scaleY = $clientHeight / 679.0");
    expect(business).toContain("$orderStatusY = [int](634 * $scaleY)");
    expect(business).toContain("$customerRegionX = [int](264 * $scaleX)");
    expect(business.match(/NativeAutomation\]::IsBluePixel\(/gu)).toHaveLength(3);
    expect(business).not.toContain("Invoke-DeployedNamedControl");
  });

  test.each([
    "Invoke-DeployedPilotBusinessAcceptance",
    "Assert-DeployedStoragePersistence",
    "Invoke-DeployedClipboardAcceptance",
  ])("activates the Worker surface before native discovery in %s", async (name) => {
    const source = await releaseFunction(name);
    const activation = source.indexOf("Invoke-DeployedNavigation");
    const discovery = source.indexOf("Get-DeployedWorkerWindow");
    expect(activation, `${name} must activate a Worker route`).toBeGreaterThanOrEqual(0);
    expect(discovery, `${name} must discover the Worker HWND`).toBeGreaterThan(activation);
  });
});
