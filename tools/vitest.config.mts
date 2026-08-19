import { defineConfig } from "vitest/config";

function replaceRequired(source: string, search: string, replacement: string, label: string): string {
  if (!source.includes(search)) throw new Error(`stable I/O test instrumentation anchor missing: ${label}`);
  return source.replace(search, replacement);
}

function replaceRequiredOccurrence(
  source: string,
  search: string,
  replacement: string,
  occurrence: number,
  label: string,
): string {
  let start = -1;
  for (let index = 0; index < occurrence; index += 1) {
    start = source.indexOf(search, start + 1);
    if (start < 0) throw new Error(`stable I/O test instrumentation anchor missing: ${label}`);
  }
  return source.slice(0, start) + replacement + source.slice(start + search.length);
}

function instrumentStableIo(source: string): string {
  let result = `import { ioCheckpoint as __testIoCheckpoint, ioReadyTimeout as __testIoReadyTimeout } from "../test/support/stable-io-test-control.ts";\n${source}`;
  result = replaceRequired(result,
    "  private readonly protocolError: () => Error | undefined;\n",
    "  private readonly protocolError: () => Error | undefined;\n  private readonly __testTargetPath: string;\n",
    "test lock target field");
  result = replaceRequired(result,
    "    protocolError: () => Error | undefined,\n  ) {",
    "    protocolError: () => Error | undefined,\n    targetPath: string,\n  ) {",
    "test lock target parameter");
  result = replaceRequired(result,
    "    this.protocolError = protocolError;\n  }",
    "    this.protocolError = protocolError;\n    this.__testTargetPath = targetPath;\n  }",
    "test lock target assignment");
  result = replaceRequired(result,
    "    try { return await acquireWindowsHelperLockOnce(targetPath, mode); }",
    "    try { return await acquireWindowsHelperLockOnce(targetPath, mode, attempt); }",
    "test helper attempt argument");
  result = replaceRequired(result,
    "async function acquireWindowsHelperLockOnce(\n  targetPath: string,\n  mode: \"file\" | \"directory\",\n): Promise<WindowsHelperLock> {",
    "async function acquireWindowsHelperLockOnce(\n  targetPath: string,\n  mode: \"file\" | \"directory\",\n  attempt: number,\n): Promise<WindowsHelperLock> {",
    "test helper attempt parameter");
  result = replaceRequired(result,
    "  const lock = new WindowsHelperLock(child, closePromise, done, () => protocolFailure);",
    "  const lock = new WindowsHelperLock(child, closePromise, done, () => protocolFailure, targetPath);",
    "test lock target argument");
  result = replaceRequired(result,
    "    this.released = true;\n    try {",
    "    this.released = true;\n    let __testHookError: unknown;\n    try { await __testIoCheckpoint(\"beforeLockRelease\", this.__testTargetPath); }\n    catch (error) { __testHookError = error; }\n    try {",
    "release hook setup");
  result = replaceRequired(result,
    "      await terminateHelper(this.child, this.closePromise);\n      throw error;\n    }\n  }",
    "      await terminateHelper(this.child, this.closePromise);\n      throw __testHookError ?? error;\n    }\n    if (__testHookError) throw __testHookError;\n  }",
    "release hook cleanup");
  result = replaceRequired(result,
    "    if (child.pid === undefined) throw new Error(\"LOCK_HELPER_EXITED\");\n    const timeout = DEFAULT_LOCK_TIMEOUT_MS;",
    "    if (child.pid === undefined) throw new Error(\"LOCK_HELPER_EXITED\");\n    await __testIoCheckpoint(\"afterLockHelperSpawn\", targetPath, attempt, child.pid);\n    const timeout = __testIoReadyTimeout(DEFAULT_LOCK_TIMEOUT_MS);",
    "helper spawn and timeout");
  result = replaceRequired(result,
    "      await ready;\n    })(), timeout);",
    "      await ready;\n      if (mode === \"file\") await __testIoCheckpoint(\"beforeInputLockReadyAcceptance\", targetPath);\n      else await __testIoCheckpoint(\"beforeDirectoryLockReadyAcceptance\", targetPath);\n    })(), timeout);",
    "helper ready acceptance");
  result = replaceRequired(result,
    "    if (windowsLock) {\n      await windowsLock.assertAlive();",
    "    if (windowsLock) {\n      await __testIoCheckpoint(\"afterInputLockReady\", resolved, windowsLock.processId());\n      await windowsLock.assertAlive();",
    "input lock ready");
  result = replaceRequired(result,
    "    handle = await open(resolved, constants.O_RDONLY | noFollow);\n    await windowsLock?.assertAlive();",
    "    handle = await open(resolved, constants.O_RDONLY | noFollow);\n    await __testIoCheckpoint(\"afterInputOpen\", resolved);\n    await windowsLock?.assertAlive();",
    "input open");
  result = replaceRequired(result,
    "    if (!sameSnapshotMetadata(opened, named)) throw new Error(`INPUT_CHANGED: ${resolved}`);\n\n    await windowsLock?.assertAlive();",
    "    if (!sameSnapshotMetadata(opened, named)) throw new Error(`INPUT_CHANGED: ${resolved}`);\n\n    await __testIoCheckpoint(\"afterInputInitialStat\", resolved);\n    await windowsLock?.assertAlive();",
    "initial stat");
  result = replaceRequired(result,
    "    const first = await readCompleteSnapshot(handle, maximumBytes, resolved, opened);\n    await windowsLock?.assertAlive();",
    "    const first = await readCompleteSnapshot(handle, maximumBytes, resolved, opened);\n    await __testIoCheckpoint(\"betweenInputSnapshots\", resolved);\n    await windowsLock?.assertAlive();",
    "snapshot boundary");
  return result;
}

function instrumentGenerator(source: string): string {
  let result = `import { ioCheckpoint as __testIoCheckpoint } from "../test/support/stable-io-test-control.ts";\n${source}`;
  result = replaceRequired(result,
    "async function requireParentLock(lock: StablePathLock, parent: StableParentSnapshot): Promise<void> {\n  await lock.assertAlive();\n  await requireStableParent(parent);\n}\n",
    `async function requireParentLock(lock: StablePathLock, parent: StableParentSnapshot): Promise<void> {
  await lock.assertAlive();
  await requireStableParent(parent);
}

async function __testParentRaceHook(
  name: "afterGenerationStaged" | "beforeOutputCommit" | "beforeReportPublish",
  output: string,
  report: string,
  parent: StableParentSnapshot,
): Promise<void> {
  try { await __testIoCheckpoint(name, output, report); }
  catch (error) {
    if (error instanceof Error && "code" in error && ["EPERM", "EACCES", "EBUSY"].includes(String(error.code))) {
      throw new Error(\`OUTPUT_PARENT_CHANGED: \${parent.path}\`);
    }
    throw error;
  }
}
`,
    "parent hook helper");
  result = replaceRequired(result,
    "    outputParentLock = await acquireStableDirectoryLock(outputParent.path);\n    await requireParentLock(outputParentLock, outputParent);",
    `    outputParentLock = await acquireStableDirectoryLock(outputParent.path);
    await __testIoCheckpoint(
      "afterFirstDirectoryLockReady",
      outputParent.path,
      reportParent.path,
      outputParentLock.helperProcessId,
    );
    await requireParentLock(outputParentLock, outputParent);`,
    "first directory lock");
  result = replaceRequired(result,
    "    reportStageIdentity = await writeDurableNewFile(reportStage, reportContents);\n    await requireStableParent(outputParent);",
    "    reportStageIdentity = await writeDurableNewFile(reportStage, reportContents);\n    await __testParentRaceHook(\"afterGenerationStaged\", output, report, outputParent);\n    await requireStableParent(outputParent);",
    "generation staged");
  const publishBarrier = "    await assertTargetMissing(report);\n\n    await requireStableParent(outputParent);";
  result = replaceRequiredOccurrence(result, publishBarrier,
    "    await assertTargetMissing(report);\n\n    await __testParentRaceHook(\"beforeOutputCommit\", output, report, outputParent);\n    await requireStableParent(outputParent);",
    1, "output commit");
  result = replaceRequired(result,
    "    outputPublished = true;\n\n    await requireStableParent(outputParent);",
    "    outputPublished = true;\n\n    await __testIoCheckpoint(\"afterOutputPublish\", output, report);\n    await requireStableParent(outputParent);",
    "output publish");
  result = replaceRequiredOccurrence(result, publishBarrier,
    "    await assertTargetMissing(report);\n\n    await __testParentRaceHook(\"beforeReportPublish\", output, report, reportParent);\n    await requireStableParent(outputParent);",
    1, "report publish");
  result = replaceRequired(result,
    "    if (outputPublished && !reportPublished && outputParent && outputStageIdentity) {\n      const publishedFiles",
    "    if (outputPublished && !reportPublished && outputParent && outputStageIdentity) {\n      await __testIoCheckpoint(\"beforeOutputRollback\", output, report);\n      const publishedFiles",
    "output rollback");
  return result;
}

export default defineConfig({
  plugins: [{
    name: "migrator-stable-io-test-instrumentation",
    enforce: "pre",
    transform(source, id) {
      const normalized = id.replaceAll("\\\\", "/").split("?")[0];
      if (normalized?.endsWith("/migrator/src/stable-io.ts")) return instrumentStableIo(source);
      if (normalized?.endsWith("/migrator/src/generator.ts")) return instrumentGenerator(source);
      return undefined;
    },
  }],
});
