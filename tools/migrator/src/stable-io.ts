import { createHash } from "node:crypto";
import { constants, type BigIntStats } from "node:fs";
import { lstat, mkdir, open, realpath, rmdir, stat } from "node:fs/promises";
import path from "node:path";

export interface StableIoTestHooks {
  afterInputOpen?(filePath: string): void | Promise<void>;
  afterInputInitialStat?(filePath: string): void | Promise<void>;
  betweenInputSnapshots?(filePath: string): void | Promise<void>;
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
    if (!sameSnapshotMetadata(opened, named)) throw new Error(`INPUT_CHANGED: ${resolved}`);

    await runStableIoHook("afterInputInitialStat", resolved);
    const first = await readCompleteSnapshot(handle, maximumBytes, resolved, opened);
    await runStableIoHook("betweenInputSnapshots", resolved);

    // Node cannot request deny-write sharing portably. Two full content and
    // metadata snapshots detect inconsistency; they do not claim to defeat a
    // coordinated writer that restores bytes and metadata between all barriers.
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
    } finally {
      await secondHandle.close();
    }
    return first.bytes;
  } finally {
    await handle.close();
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
