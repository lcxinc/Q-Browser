import { createHash } from "node:crypto";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { constants, type BigIntStats } from "node:fs";
import { lstat, mkdir, open, realpath, rmdir, stat } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";

export interface StableIoTestHooks {
  afterInputOpen?(filePath: string): void | Promise<void>;
  afterInputInitialStat?(filePath: string): void | Promise<void>;
  betweenInputSnapshots?(filePath: string): void | Promise<void>;
  afterInputLockReady?(filePath: string, processId: number): void | Promise<void>;
  beforeInputLockReadyAcceptance?(filePath: string): void | Promise<void>;
  beforeDirectoryLockReadyAcceptance?(directoryPath: string): void | Promise<void>;
  lockHelperReadyTimeoutMs?: number;
  afterLockHelperSpawn?(targetPath: string, attempt: number, processId: number): void | Promise<void>;
  afterFirstDirectoryLockReady?(outputParent: string, reportParent: string, processId?: number): void | Promise<void>;
  beforeLockRelease?(targetPath: string): void | Promise<void>;
  afterGenerationStaged?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeOutputCommit?(outputPath: string, reportPath: string): void | Promise<void>;
  afterOutputPublish?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeOutputRollback?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeReportPublish?(outputPath: string, reportPath: string): void | Promise<void>;
}

let testHooks: StableIoTestHooks = {};

export function setStableIoTestHooks(hooks: StableIoTestHooks = {}): void {
  testHooks = { ...hooks };
}

export async function runStableIoHook(
  name: Exclude<keyof StableIoTestHooks, "lockHelperReadyTimeoutMs">,
  ...values: unknown[]
): Promise<void> {
  const hook = testHooks[name] as ((...args: unknown[]) => void | Promise<void>) | undefined;
  await hook?.(...values);
}

type BigStats = BigIntStats;

export interface PathIdentity {
  dev: bigint;
  ino: bigint;
  birthtimeMs: bigint;
}

function sameIdentity(left: PathIdentity, right: PathIdentity): boolean {
  return left.dev === right.dev && left.ino === right.ino && left.birthtimeMs === right.birthtimeMs;
}

function sameSnapshotMetadata(left: BigStats, right: BigStats): boolean {
  return sameIdentity(left, right)
    && left.size === right.size
    && left.mtimeNs === right.mtimeNs
    && left.ctimeNs === right.ctimeNs;
}

function comparable(value: string): string {
  const normalized = path.normalize(value);
  return process.platform === "win32" ? normalized.toLocaleLowerCase("en-US") : normalized;
}

function insideBoundary(boundary: string, candidate: string): boolean {
  const relative = path.relative(boundary, candidate);
  return relative.length === 0 || (!relative.startsWith("..") && !path.isAbsolute(relative));
}

const LOCK_PROTOCOL_LIMIT = 4096;
const DEFAULT_LOCK_TIMEOUT_MS = 10_000;
const WINDOWS_POWERSHELL_NT = String.raw`\\?\GLOBALROOT\SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe`;
const LOCK_HELPER_SCRIPT = fileURLToPath(new URL("../stable-lock-helper.ps1", import.meta.url));

interface WindowsSystemPaths {
  powershell: string;
  systemRoot: string;
  commandProcessor: string;
}

let windowsSystemPathsPromise: Promise<WindowsSystemPaths> | undefined;

async function windowsSystemPaths(): Promise<WindowsSystemPaths> {
  windowsSystemPathsPromise ??= (async () => {
    // GLOBALROOT\SystemRoot is an OS object-manager alias, not caller-controlled
    // environment state. realpath converts it to a CreateProcess-compatible path.
    const powershell = await realpath(WINDOWS_POWERSHELL_NT);
    const suffix = path.normalize("System32/WindowsPowerShell/v1.0/powershell.exe").toLocaleLowerCase("en-US");
    if (!path.isAbsolute(powershell) || !path.normalize(powershell).toLocaleLowerCase("en-US").endsWith(suffix)) {
      throw new Error("LOCK_HELPER_UNAVAILABLE");
    }
    const systemRoot = path.resolve(path.dirname(powershell), "../../..");
    return { powershell, systemRoot, commandProcessor: path.join(systemRoot, "System32", "cmd.exe") };
  })();
  return windowsSystemPathsPromise;
}

export interface StablePathLock {
  readonly helperProcessId?: number;
  assertAlive(): Promise<void>;
  release(): Promise<void>;
}

class NodeDirectoryLock implements StablePathLock {
  private readonly handle: Awaited<ReturnType<typeof open>>;
  constructor(handle: Awaited<ReturnType<typeof open>>) { this.handle = handle; }
  async assertAlive(): Promise<void> { await this.handle.stat(); }
  async release(): Promise<void> { await this.handle.close(); }
}

class WindowsHelperLock implements StablePathLock {
  private released = false;
  private readonly child: ChildProcessWithoutNullStreams;
  private readonly closePromise: Promise<{ code: number | null; signal: NodeJS.Signals | null }>;
  private readonly donePromise: Promise<void>;
  private readonly protocolError: () => Error | undefined;
  private readonly targetPath: string;

  constructor(
    child: ChildProcessWithoutNullStreams,
    closePromise: Promise<{ code: number | null; signal: NodeJS.Signals | null }>,
    donePromise: Promise<void>,
    protocolError: () => Error | undefined,
    targetPath: string,
  ) {
    this.child = child;
    this.closePromise = closePromise;
    this.donePromise = donePromise;
    this.protocolError = protocolError;
    this.targetPath = targetPath;
  }

  processId(): number {
    if (this.child.pid === undefined) throw new Error("LOCK_HELPER_EXITED");
    return this.child.pid;
  }

  get helperProcessId(): number | undefined { return this.child.pid; }

  async assertAlive(): Promise<void> {
    if (this.protocolError() || this.child.exitCode !== null || this.child.signalCode !== null) {
      throw this.protocolError() ?? new Error("LOCK_HELPER_EXITED");
    }
  }

  async release(): Promise<void> {
    if (this.released) return;
    this.released = true;
    let hookError: unknown;
    try { await runStableIoHook("beforeLockRelease", this.targetPath); }
    catch (error) { hookError = error; }
    try {
      if (this.child.exitCode === null && this.child.signalCode === null) {
        const inputError = new Promise<never>((_resolve, reject) => {
          this.child.stdin.once("error", () => reject(new Error("LOCK_HELPER_PROTOCOL")));
        });
        this.child.stdin.end("DONE\n");
        const [result] = await raceTimeout(Promise.all([
          this.closePromise,
          Promise.race([this.donePromise, inputError]),
        ]), 2_000);
        if (result.code !== 0 || result.signal !== null || this.protocolError()) {
          throw this.protocolError() ?? new Error("LOCK_HELPER_EXITED");
        }
      } else {
        await raceTimeout(this.closePromise, 2_000);
        if (this.protocolError()) throw this.protocolError();
      }
    } catch (error) {
      await terminateHelper(this.child, this.closePromise);
      throw hookError ?? error;
    }
    if (hookError) throw hookError;
  }
}

function raceTimeout<T>(promise: Promise<T>, milliseconds: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("LOCK_HELPER_TIMEOUT")), milliseconds);
    timer.unref();
    promise.then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error: unknown) => { clearTimeout(timer); reject(error); },
    );
  });
}

async function terminateHelper(
  child: ChildProcessWithoutNullStreams,
  closePromise: Promise<{ code: number | null; signal: NodeJS.Signals | null }>,
): Promise<void> {
  child.stdin.destroy();
  if (child.exitCode === null && child.signalCode === null) child.kill();
  try { await raceTimeout(closePromise, 2_000); }
  catch {
    if (child.exitCode === null && child.signalCode === null) child.kill("SIGKILL");
    try { await raceTimeout(closePromise, 2_000); }
    catch { throw new Error("LOCK_HELPER_TERMINATION_FAILED"); }
  }
}

async function acquireWindowsHelperLock(
  targetPath: string,
  mode: "file" | "directory",
): Promise<WindowsHelperLock> {
  let lastError: unknown;
  for (let attempt = 0; attempt < 3; attempt += 1) {
    try { return await acquireWindowsHelperLockOnce(targetPath, mode, attempt); }
    catch (error) {
      lastError = error;
      if (!(error instanceof Error) || !error.message.startsWith("LOCK_HELPER_EXITED") || attempt === 2) throw error;
      await new Promise<void>((resolve) => setTimeout(resolve, 10));
    }
  }
  throw lastError;
}

async function acquireWindowsHelperLockOnce(
  targetPath: string,
  mode: "file" | "directory",
  attempt: number,
): Promise<WindowsHelperLock> {
  const system = await windowsSystemPaths();
  const child = spawn(system.powershell, [
    "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
    "-File", LOCK_HELPER_SCRIPT, "-Mode", mode, "-Path", targetPath,
  ], {
    cwd: mode === "directory" ? targetPath : path.dirname(LOCK_HELPER_SCRIPT),
    windowsHide: true,
    stdio: ["pipe", "pipe", "pipe"],
    env: {
      SystemRoot: system.systemRoot,
      WINDIR: system.systemRoot,
      COMSPEC: system.commandProcessor,
    },
  });
  const closePromise = new Promise<{ code: number | null; signal: NodeJS.Signals | null }>((resolve) => {
    child.once("close", (code, signal) => resolve({ code, signal }));
  });
  let resolveReady!: () => void;
  let rejectReady!: (error: Error) => void;
  let resolveDone!: () => void;
  let rejectDone!: (error: Error) => void;
  const ready = new Promise<void>((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
  const done = new Promise<void>((resolve, reject) => { resolveDone = resolve; rejectDone = reject; });
  void done.catch(() => undefined);
  let protocolFailure: Error | undefined;
  const failProtocol = (error: Error): void => {
    if (protocolFailure) return;
    protocolFailure = error;
    rejectReady(error);
    rejectDone(error);
  };
  const lock = new WindowsHelperLock(child, closePromise, done, () => protocolFailure, targetPath);
  let stdoutBuffer = "";
  let stdoutBytes = 0;
  let stderrBytes = 0;
  let lineCount = 0;
  let readyAccepted = false;
  let doneAccepted = false;
  child.once("error", () => failProtocol(new Error("LOCK_HELPER_EXITED")));
  child.once("close", () => {
    if (!readyAccepted || !doneAccepted) failProtocol(new Error("LOCK_HELPER_EXITED"));
    else if (lineCount !== 2 || stdoutBuffer.length !== 0) failProtocol(new Error("LOCK_HELPER_PROTOCOL"));
  });
    child.stderr.on("data", (chunk: Buffer) => {
      if (protocolFailure) return;
      stderrBytes += chunk.length;
      if (stderrBytes > LOCK_PROTOCOL_LIMIT) failProtocol(new Error("LOCK_HELPER_PROTOCOL_LIMIT"));
      else failProtocol(new Error("LOCK_HELPER_PROTOCOL"));
    });
    child.stdout.on("data", (chunk: Buffer) => {
      if (protocolFailure) return;
      stdoutBytes += chunk.length;
      if (stdoutBytes > LOCK_PROTOCOL_LIMIT) {
        failProtocol(new Error("LOCK_HELPER_PROTOCOL_LIMIT"));
        return;
      }
      stdoutBuffer += chunk.toString("ascii");
      let lineEnd = stdoutBuffer.indexOf("\n");
      while (lineEnd >= 0) {
        const line = stdoutBuffer.slice(0, lineEnd).replace(/\r$/, "");
        stdoutBuffer = stdoutBuffer.slice(lineEnd + 1);
        if (lineCount === 0 && line === "READY") { readyAccepted = true; resolveReady(); }
        else if (lineCount === 1 && line === "DONE") { doneAccepted = true; resolveDone(); }
        else { failProtocol(new Error("LOCK_HELPER_PROTOCOL")); return; }
        lineCount += 1;
        lineEnd = stdoutBuffer.indexOf("\n");
      }
    });
  try {
    if (child.pid === undefined) throw new Error("LOCK_HELPER_EXITED");
    await runStableIoHook("afterLockHelperSpawn", targetPath, attempt, child.pid);
    const timeout = testHooks.lockHelperReadyTimeoutMs ?? DEFAULT_LOCK_TIMEOUT_MS;
    await raceTimeout((async () => {
      await ready;
      if (mode === "file") await runStableIoHook("beforeInputLockReadyAcceptance", targetPath);
      else await runStableIoHook("beforeDirectoryLockReadyAcceptance", targetPath);
    })(), timeout);
    await lock.assertAlive();
    return lock;
  } catch (error) {
    await terminateHelper(child, closePromise);
    if (error instanceof Error && error.message === "LOCK_HELPER_TIMEOUT") throw error;
    if (error instanceof Error && error.message.startsWith("LOCK_HELPER_PROTOCOL")) throw error;
    throw new Error(`LOCK_HELPER_EXITED: ${targetPath}`);
  }
}

export async function acquireStableDirectoryLock(directoryPath: string): Promise<StablePathLock> {
  if (process.platform === "win32") return acquireWindowsHelperLock(directoryPath, "directory");
  // Holding a directory fd lets callers revalidate the object, but POSIX rename
  // semantics do not provide a deny-delete guarantee against an active writer.
  return new NodeDirectoryLock(await open(directoryPath, constants.O_RDONLY));
}

export async function assertNoReparseAncestors(targetPath: string, errorCode: string): Promise<void> {
  let current = path.resolve(targetPath);
  while (true) {
    try {
      const currentStat = await lstat(current);
      if (currentStat.isSymbolicLink()) throw new Error(`${errorCode}: ${current}`);
    } catch (error) {
      if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
    }
    const parent = path.dirname(current);
    if (parent === current) return;
    current = parent;
  }
}

export async function readStableRegularFile(
  filePath: string,
  maximumBytes: number,
  boundaryRoot: string,
): Promise<Uint8Array> {
  const resolved = path.resolve(filePath);
  const resolvedBoundary = path.resolve(boundaryRoot);
  await assertNoReparseAncestors(resolved, "UNSAFE_INPUT_PATH");
  await assertNoReparseAncestors(resolvedBoundary, "UNSAFE_INPUT_PATH");
  const canonicalBoundary = await realpath(resolvedBoundary);
  const noFollow = "O_NOFOLLOW" in constants ? (constants as typeof constants & { O_NOFOLLOW: number }).O_NOFOLLOW : 0;
  const windowsLock = process.platform === "win32" ? await acquireWindowsHelperLock(resolved, "file") : undefined;
  let handle: Awaited<ReturnType<typeof open>> | undefined;
  try {
    if (windowsLock) {
      await runStableIoHook("afterInputLockReady", resolved, windowsLock.processId());
      await windowsLock.assertAlive();
    }
    handle = await open(resolved, constants.O_RDONLY | noFollow);
    await runStableIoHook("afterInputOpen", resolved);
    await windowsLock?.assertAlive();
    const opened = await handle.stat({ bigint: true });
    if (!opened.isFile()) throw new Error(`UNSAFE_INPUT_PATH: ${resolved}`);
    if (opened.size > BigInt(maximumBytes)) throw new Error(`INPUT_LIMIT_EXCEEDED: ${resolved}`);

    await assertNoReparseAncestors(resolved, "UNSAFE_INPUT_PATH");
    const canonical = await realpath(resolved);
    if (!insideBoundary(comparable(canonicalBoundary), comparable(canonical))) {
      throw new Error(`SOURCE_BOUNDARY_VIOLATION: ${resolved}`);
    }
    const named = await stat(resolved, { bigint: true });
    if (!sameIdentity(opened, named)) throw new Error(`INPUT_IDENTITY_CHANGED: ${resolved}`);
    if (!sameSnapshotMetadata(opened, named)) throw new Error(`INPUT_CHANGED: ${resolved}`);

    await runStableIoHook("afterInputInitialStat", resolved);
    await windowsLock?.assertAlive();
    const first = await readCompleteSnapshot(handle, maximumBytes, resolved, opened);
    await runStableIoHook("betweenInputSnapshots", resolved);
    await windowsLock?.assertAlive();

    // POSIX uses two full content/metadata snapshots. This detects inconsistency
    // but does not claim to defeat a coordinated writer that restores state.
    // Windows additionally holds a FileShare.Read helper before every hook/read.
    await assertNoReparseAncestors(resolved, "UNSAFE_INPUT_PATH");
    const secondCanonical = await realpath(resolved);
    if (!insideBoundary(comparable(canonicalBoundary), comparable(secondCanonical))) {
      throw new Error(`SOURCE_BOUNDARY_VIOLATION: ${resolved}`);
    }
    const secondHandle = await open(resolved, constants.O_RDONLY | noFollow);
    try {
      const secondBefore = await secondHandle.stat({ bigint: true });
      if (!sameSnapshotMetadata(first.metadata, secondBefore)) throw new Error(`INPUT_CHANGED: ${resolved}`);
      const second = await readCompleteSnapshot(secondHandle, maximumBytes, resolved, secondBefore);
      if (!sameSnapshotMetadata(first.metadata, second.metadata)
          || first.digest !== second.digest
          || !Buffer.from(first.bytes).equals(Buffer.from(second.bytes))) {
        throw new Error(`INPUT_CHANGED: ${resolved}`);
      }
      await windowsLock?.assertAlive();
    } finally {
      await secondHandle.close();
    }
    return first.bytes;
  } finally {
    try { await handle?.close(); }
    finally { await windowsLock?.release(); }
  }
}

interface CompleteSnapshot {
  bytes: Uint8Array;
  metadata: BigStats;
  digest: string;
}

async function readCompleteSnapshot(
  handle: Awaited<ReturnType<typeof open>>,
  maximumBytes: number,
  resolved: string,
  before: BigStats,
): Promise<CompleteSnapshot> {
  const chunks: Uint8Array[] = [];
  let total = 0;
  let position = 0;
  while (total <= maximumBytes) {
    const length = Math.min(65_536, maximumBytes + 1 - total);
    if (length <= 0) break;
    const buffer = new Uint8Array(length);
    const result = await handle.read(buffer, 0, length, position);
    if (result.bytesRead === 0) break;
    chunks.push(buffer.subarray(0, result.bytesRead));
    total += result.bytesRead;
    position += result.bytesRead;
  }
  if (total > maximumBytes) throw new Error(`INPUT_LIMIT_EXCEEDED: ${resolved}`);
  const after = await handle.stat({ bigint: true });
  const namedAfter = await stat(resolved, { bigint: true });
  if (!sameSnapshotMetadata(before, after)
      || !sameSnapshotMetadata(before, namedAfter)
      || after.size !== BigInt(total)) {
    throw new Error(`INPUT_CHANGED: ${resolved}`);
  }
  const contents = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) { contents.set(chunk, offset); offset += chunk.length; }
  return {
    bytes: contents,
    metadata: after,
    digest: createHash("sha256").update(contents).digest("hex"),
  };
}

export interface StableParentSnapshot {
  path: string;
  canonical: string;
  identity: BigStats;
}

export interface StableParentPlan {
  path: string;
  anchor: StableParentSnapshot;
  missing: string[];
}

export type OwnedDirectoryMap = Map<string, { path: string; identity: PathIdentity }>;

function pathKey(value: string): string {
  return comparable(path.resolve(value).normalize("NFC"));
}

export async function planStableParent(targetPath: string): Promise<StableParentPlan> {
  const parent = path.dirname(path.resolve(targetPath));
  await assertNoReparseAncestors(parent, "OUTPUT_REPARSE_POINT");
  const missing: string[] = [];
  let current = parent;
  while (true) {
    try {
      const currentStat = await lstat(current);
      if (currentStat.isSymbolicLink()) throw new Error(`OUTPUT_REPARSE_POINT: ${current}`);
      if (!currentStat.isDirectory()) throw new Error(`OUTPUT_PARENT_NOT_DIRECTORY: ${current}`);
      const identity = await stat(current, { bigint: true });
      return {
        path: parent,
        anchor: { path: current, canonical: await realpath(current), identity },
        missing: missing.toReversed(),
      };
    } catch (error) {
      if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
    }
    missing.push(current);
    const next = path.dirname(current);
    if (next === current) throw new Error(`OUTPUT_PARENT_UNAVAILABLE: ${parent}`);
    current = next;
  }
}

export async function materializeStableParent(
  plan: StableParentPlan,
  owned: OwnedDirectoryMap,
): Promise<StableParentSnapshot> {
  await requireStableParent(plan.anchor);
  let previousPath = plan.anchor.path;
  let previousIdentity = identityFromStats(plan.anchor.identity);
  for (const directory of plan.missing) {
    if (!await pathHasIdentity(previousPath, previousIdentity)) throw new Error(`OUTPUT_PARENT_CHANGED: ${previousPath}`);
    const key = pathKey(directory);
    const existingOwned = owned.get(key);
    if (existingOwned) {
      if (!await pathHasIdentity(directory, existingOwned.identity)) throw new Error(`OUTPUT_PARENT_CHANGED: ${directory}`);
      previousPath = directory;
      previousIdentity = existingOwned.identity;
      continue;
    }
    await mkdir(directory, { recursive: false });
    const identity = await capturePathIdentity(directory);
    owned.set(key, { path: directory, identity });
    previousPath = directory;
    previousIdentity = identity;
  }
  await assertNoReparseAncestors(plan.path, "OUTPUT_PARENT_CHANGED");
  const identity = await stat(plan.path, { bigint: true });
  return { path: plan.path, canonical: await realpath(plan.path), identity };
}

export async function rollbackOwnedDirectories(owned: OwnedDirectoryMap): Promise<void> {
  const directories = [...owned.values()].toSorted((left, right) => right.path.length - left.path.length);
  for (const directory of directories) {
    if (!await pathHasIdentity(directory.path, directory.identity)) continue;
    try { await rmdir(directory.path); }
    catch (error) {
      if (!(error instanceof Error) || !("code" in error)
          || !["ENOENT", "ENOTEMPTY", "EEXIST"].includes(String(error.code))) throw error;
    }
  }
}

export async function prepareStableParent(targetPath: string): Promise<StableParentSnapshot> {
  const owned: OwnedDirectoryMap = new Map();
  return materializeStableParent(await planStableParent(targetPath), owned);
}

export async function stableParentUnchanged(snapshot: StableParentSnapshot): Promise<boolean> {
  try {
    await assertNoReparseAncestors(snapshot.path, "OUTPUT_PARENT_CHANGED");
    const canonical = await realpath(snapshot.path);
    const current = await stat(snapshot.path, { bigint: true });
    return comparable(canonical) === comparable(snapshot.canonical) && sameIdentity(snapshot.identity, current);
  } catch {
    return false;
  }
}

export async function requireStableParent(snapshot: StableParentSnapshot): Promise<void> {
  if (!await stableParentUnchanged(snapshot)) throw new Error(`OUTPUT_PARENT_CHANGED: ${snapshot.path}`);
}

export async function samePathIdentity(leftPath: string, rightPath: string): Promise<boolean> {
  try {
    return sameIdentity(await stat(leftPath, { bigint: true }), await stat(rightPath, { bigint: true }));
  } catch {
    return false;
  }
}

export function identityFromStats(value: BigStats): PathIdentity {
  return { dev: value.dev, ino: value.ino, birthtimeMs: value.birthtimeMs };
}

export async function capturePathIdentity(filePath: string): Promise<PathIdentity> {
  return identityFromStats(await stat(filePath, { bigint: true }));
}

export async function pathHasIdentity(filePath: string, identity: PathIdentity): Promise<boolean> {
  try { return sameIdentity(await stat(filePath, { bigint: true }), identity); }
  catch { return false; }
}
