import { constants, type BigIntStats } from "node:fs";
import { lstat, mkdir, open, realpath, stat } from "node:fs/promises";
import path from "node:path";

export interface StableIoTestHooks {
  afterInputOpen?(filePath: string): void | Promise<void>;
  afterGenerationStaged?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeReportPublish?(outputPath: string, reportPath: string): void | Promise<void>;
  afterReportPublish?(outputPath: string, reportPath: string): void | Promise<void>;
  beforeReportRollback?(outputPath: string, reportPath: string): void | Promise<void>;
}

let testHooks: StableIoTestHooks = {};

export function setStableIoTestHooks(hooks: StableIoTestHooks = {}): void {
  testHooks = { ...hooks };
}

export async function runStableIoHook(
  name: keyof StableIoTestHooks,
  ...paths: string[]
): Promise<void> {
  const hook = testHooks[name] as ((...values: string[]) => void | Promise<void>) | undefined;
  await hook?.(...paths);
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

function comparable(value: string): string {
  const normalized = path.normalize(value);
  return process.platform === "win32" ? normalized.toLocaleLowerCase("en-US") : normalized;
}

function insideBoundary(boundary: string, candidate: string): boolean {
  const relative = path.relative(boundary, candidate);
  return relative.length === 0 || (!relative.startsWith("..") && !path.isAbsolute(relative));
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
  const handle = await open(resolved, constants.O_RDONLY | noFollow);
  try {
    await runStableIoHook("afterInputOpen", resolved);
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
    if (!sameIdentity(opened, after) || !sameIdentity(opened, namedAfter)
        || after.size !== BigInt(total)) {
      throw new Error(`INPUT_CHANGED_DURING_READ: ${resolved}`);
    }
    const contents = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) { contents.set(chunk, offset); offset += chunk.length; }
    return contents;
  } finally {
    await handle.close();
  }
}

export interface StableParentSnapshot {
  path: string;
  canonical: string;
  identity: BigStats;
}

export async function prepareStableParent(targetPath: string): Promise<StableParentSnapshot> {
  const parent = path.dirname(path.resolve(targetPath));
  await assertNoReparseAncestors(parent, "OUTPUT_REPARSE_POINT");
  await mkdir(parent, { recursive: true });
  await assertNoReparseAncestors(parent, "OUTPUT_REPARSE_POINT");
  const canonical = await realpath(parent);
  const identity = await stat(parent, { bigint: true });
  if (!identity.isDirectory()) throw new Error(`OUTPUT_PARENT_NOT_DIRECTORY: ${parent}`);
  return { path: parent, canonical, identity };
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
