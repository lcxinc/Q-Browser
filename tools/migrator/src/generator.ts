import { randomUUID } from "node:crypto";
import { link, lstat, mkdir, open, rename, rmdir, unlink } from "node:fs/promises";
import path from "node:path";

import { addDiagnostic, createDiagnostic, sortDiagnostics } from "./diagnostics.ts";
import {
  capturePathIdentity,
  identityFromStats,
  materializeStableParent,
  pathHasIdentity,
  planStableParent,
  prepareStableParent,
  requireStableParent,
  rollbackOwnedDirectories,
  runStableIoHook,
  stableParentUnchanged,
  type OwnedDirectoryMap,
  type PathIdentity,
  type StableParentSnapshot,
} from "./stable-io.ts";
import type { Diagnostic, MigrationIR, MigrationNode } from "./types.ts";

export const GENERATOR_LIMITS = Object.freeze({ files: 64, outputBytes: 5_242_880, stringLength: 65_536 });

export interface GenerationReport {
  version: 1;
  sourceFile: string;
  generatedFiles: string[];
  diagnostics: Diagnostic[];
}

export interface GeneratedProject {
  files: Record<string, string>;
  report: GenerationReport;
}

export function assertDisjointGenerationPaths(outputPath: string, reportPath: string): void {
  const lexicalKey = (value: string): string => {
    let normalized = path.resolve(value).normalize("NFC");
    if (process.platform !== "win32") return path.normalize(normalized);
    if (normalized.startsWith("\\\\?\\UNC\\")) normalized = `\\\\${normalized.slice(8)}`;
    else if (normalized.startsWith("\\\\?\\")) normalized = normalized.slice(4);
    const parsed = path.win32.parse(normalized);
    const segments = normalized.slice(parsed.root.length).split(/[\\/]/u)
      .map((segment) => segment.replace(/[ .]+$/u, ""));
    return path.win32.join(parsed.root, ...segments).toLocaleLowerCase("en-US");
  };
  const output = lexicalKey(outputPath);
  const report = lexicalKey(reportPath);
  const contains = (ancestor: string, candidate: string): boolean => {
    if (ancestor === candidate) return true;
    const separator = process.platform === "win32" ? "\\" : path.sep;
    return candidate.startsWith(ancestor.endsWith(separator) ? ancestor : `${ancestor}${separator}`);
  };
  if (contains(output, report) || contains(report, output)) throw new Error("OUTPUT_REPORT_OVERLAP");
}

const QML_RESERVED = new Set([
  "as", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else",
  "enum", "export", "extends", "false", "finally", "for", "from", "function", "get", "if", "import", "in", "instanceof",
  "let", "new", "null", "of", "on", "package", "private", "property", "protected", "public", "readonly", "required",
  "return", "set", "signal", "static", "super", "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield",
]);

export function qmlIdentifier(value: string, fallback = "item"): string {
  let normalized = value.normalize("NFC").replace(/[^\p{L}\p{N}_]/gu, "_");
  if (!/^[_\p{L}]/u.test(normalized)) normalized = `_${normalized}`;
  if (normalized.length === 0) normalized = fallback;
  if (QML_RESERVED.has(normalized)) normalized = `${normalized}_item`;
  return normalized.slice(0, 128);
}

export function qmlString(value: string): string {
  if (value.length > GENERATOR_LIMITS.stringLength) throw new Error("OUTPUT_STRING_LIMIT_EXCEEDED");
  let encoded = "";
  for (const character of value.normalize("NFC")) {
    if (character === "\\") { encoded += "\\\\"; continue; }
    if (character === "\"") { encoded += "\\\""; continue; }
    if (character === "\n") { encoded += "\\n"; continue; }
    if (character === "\r") { encoded += "\\r"; continue; }
    if (character === "\t") { encoded += "\\t"; continue; }
    const codePoint = character.codePointAt(0)!;
    const escaped = codePoint <= 0x1f
      || (codePoint >= 0x7f && codePoint <= 0x9f)
      || codePoint === 0x2028 || codePoint === 0x2029
      || (codePoint >= 0xd800 && codePoint <= 0xdfff)
      || /\p{Cf}/u.test(character);
    if (!escaped) { encoded += character; continue; }
    for (let index = 0; index < character.length; index += 1) {
      encoded += `\\u${character.charCodeAt(index).toString(16).toUpperCase().padStart(4, "0")}`;
    }
  }
  return `"${encoded}"`;
}

function textContent(node: MigrationNode): string {
  const values: string[] = [];
  function visit(current: MigrationNode): void {
    if (current.kind === "text" && current.text) values.push(current.text);
    for (const child of current.children) visit(child);
  }
  visit(node);
  return values.join(" ").replace(/[ \t\n\r\f]+/gu, " ").trim();
}

function labelText(node: MigrationNode): string {
  const values: string[] = [];
  function visit(current: MigrationNode): void {
    if (current.kind === "control") return;
    if (current.kind === "text" && current.text) values.push(current.text);
    for (const child of current.children) visit(child);
  }
  visit(node);
  return values.join(" ").replace(/[ \t\n\r\f]+/gu, " ").trim();
}

interface GenerationContext {
  diagnostics: Diagnostic[];
  labels: Map<string, string>;
}

function createGenerationContext(ir: MigrationIR): GenerationContext {
  const labels = new Map<string, string>();
  function visit(node: MigrationNode): void {
    if (node.kind === "label") {
      const text = labelText(node);
      if (text.length > 0 && typeof node.attributes.for === "string") labels.set(node.attributes.for, text);
      function associateNested(current: MigrationNode): void {
        if (current.kind === "control" && text.length > 0) labels.set(current.id, text);
        for (const child of current.children) associateNested(child);
      }
      for (const child of node.children) associateNested(child);
    }
    for (const child of node.children) visit(child);
  }
  visit(ir.root);
  return { diagnostics: [...ir.diagnostics], labels };
}

function indent(lines: string[], level: number): string[] {
  const prefix = "    ".repeat(level);
  return lines.map((line) => line.length > 0 ? `${prefix}${line}` : line);
}

function lengthValue(value: string | undefined, fallback: string): string {
  if (!value) return fallback;
  const numeric = /^(\d+(?:\.\d+)?)px$/u.exec(value);
  return numeric ? numeric[1]! : fallback;
}

function gridSpacing(value: string | undefined): { row: string; column: string } {
  const [row, column] = value?.trim().split(/\s+/u) ?? [];
  return {
    row: lengthValue(row, "Spacing.md"),
    column: lengthValue(column ?? row, "Spacing.md"),
  };
}

function sourceCommentFile(value: string): string {
  return value.normalize("NFC").replace(/[^\p{L}\p{N}_.-]/gu, "_").slice(0, 128) || "input.html";
}

function emitText(text: string, heading: boolean, node: MigrationNode): string[] {
  const pixelSize = heading ? "Typography.heading" : "Typography.body";
  const weight = heading ? "Typography.boldWeight" : "Typography.normalWeight";
  return [
    "Text {",
    `    Layout.fillWidth: true`,
    `    text: ${qmlString(text)}`,
    "    color: Theme.textPrimary",
    "    font.family: Typography.family",
    `    font.pixelSize: ${pixelSize}`,
    `    font.weight: ${weight}`,
    "    wrapMode: Text.Wrap",
    "    Accessible.name: text",
    "    Accessible.role: Accessible.StaticText",
    `    // Source: ${sourceCommentFile(node.location.file)}:${node.location.start.line}:${node.location.start.column}`,
    "}",
  ];
}

function emitChildren(node: MigrationNode, context: GenerationContext): string[] {
  return node.children.flatMap((child) => emitNode(child, context));
}

function gridColumns(value: string | undefined): number {
  if (!value) return 1;
  const repeated = /^repeat\(([1-9]\d{0,2}),/u.exec(value);
  if (repeated) return Math.min(32, Number(repeated[1]));
  return Math.max(1, Math.min(32, value.split(" ").length));
}

function emitLayout(node: MigrationNode, card: boolean, context: GenerationContext): string[] {
  const content = emitChildren(node, context);
  const layoutType = node.style.display === "grid"
    ? "GridLayout" : node.style.display === "flex" && node.style.flexDirection !== "column"
      ? "RowLayout" : "ColumnLayout";
  const spacing = lengthValue(node.style.gap, "Spacing.md");
  const gridGap = gridSpacing(node.style.gap);
  const layout = [
    `${layoutType} {`,
    ...(card ? ["    anchors.fill: parent"] : ["    Layout.fillWidth: true"]),
    ...(layoutType === "GridLayout"
      ? [`    columns: ${gridColumns(node.style.gridTemplateColumns)}`, `    rowSpacing: ${gridGap.row}`, `    columnSpacing: ${gridGap.column}`]
      : [`    spacing: ${spacing}`]),
    ...indent(content.length > 0 ? content : emitText(textContent(node) || "Content", false, node), 1),
    "}",
  ];
  if (!card) return layout;
  return [
    "AppCard {",
    "    Layout.fillWidth: true",
    `    accessibleName: ${qmlString(textContent(node) || node.kind)}`,
    ...indent(layout, 1),
    "}",
  ];
}

function listItems(node: MigrationNode): string[] {
  return node.children.filter((child) => child.kind === "listItem").map((child) => textContent(child)).filter(Boolean);
}

function tableRows(node: MigrationNode): string[] {
  const rows: string[] = [];
  function visit(current: MigrationNode): void {
    if (current.kind === "tableRow") rows.push(textContent(current));
    for (const child of current.children) visit(child);
  }
  visit(node);
  return rows;
}

function controlLabel(node: MigrationNode, context: GenerationContext): string {
  const htmlId = typeof node.attributes.id === "string" ? node.attributes.id : "";
  return context.labels.get(htmlId) ?? context.labels.get(node.id)
    ?? String(node.attributes.placeholder ?? node.attributes.name ?? "");
}

function emitUnsupportedControl(node: MigrationNode, kind: string, context: GenerationContext): string[] {
  const label = controlLabel(node, context);
  addDiagnostic(context.diagnostics, createDiagnostic(
    "UNSUPPORTED_CONTROL", "warning", `Unsupported form control: ${kind}`, node.location,
  ));
  return [
    "AppCard {",
    "    Layout.fillWidth: true",
    `    accessibleName: ${qmlString(label || `Unsupported ${kind} control`)}`,
    "    ColumnLayout {",
    "        anchors.fill: parent",
    "        spacing: Spacing.xs",
    ...(label ? [`        Text { text: ${qmlString(label)}; color: Theme.textPrimary; font.family: Typography.family; font.pixelSize: Typography.body }`] : []),
    `        Text { text: ${qmlString(`Unsupported ${kind} control`)}; color: Theme.textSecondary; font.family: Typography.family; font.pixelSize: Typography.label }`,
    "    }",
    "}",
  ];
}

function emitNode(node: MigrationNode, context: GenerationContext): string[] {
  if (node.kind === "text") return emitText(node.text ?? "", false, node);
  if (node.kind === "heading") return emitText(textContent(node), true, node);
  if (node.kind === "navigation") {
    const items = node.children.map(textContent).filter(Boolean);
    return [
      "AppNavigation {",
      "    Layout.fillWidth: true",
      `    model: [${items.map(qmlString).join(", ")}]`,
      `    accessibleName: ${qmlString(String(node.attributes["aria-label"] ?? "Navigation"))}`,
      "}",
    ];
  }
  if (node.kind === "control") {
    const label = controlLabel(node, context);
    if (node.tag === "button") return ["AppButton {", `    text: ${qmlString(textContent(node) || "Continue")}`, "}"];
    if (node.tag === "select") return emitUnsupportedControl(node, "select", context);
    const inputType = node.tag === "textarea" ? "textarea" : String(node.attributes.type ?? "text").toLowerCase();
    if (node.tag === "input" && ["button", "submit", "reset"].includes(inputType)) {
      return ["AppButton {", `    text: ${qmlString(String(node.attributes.value ?? label ?? "Continue"))}`, "}"];
    }
    if (node.tag !== "textarea" && (node.tag !== "input" || !["text", "password", "email"].includes(inputType))) {
      return emitUnsupportedControl(node, inputType || node.tag || "unknown", context);
    }
    return [
      "AppTextField {",
      "    Layout.fillWidth: true",
      `    label: ${qmlString(label)}`,
      ...(inputType === "password" ? ["    echoMode: TextInput.Password"] : []),
      ...(inputType === "email" ? ["    inputMethodHints: Qt.ImhEmailCharactersOnly"] : []),
      ...(node.validation?.required ? ["    required: true"] : []),
      ...(node.validation?.maxLength !== undefined ? [`    maximumLength: ${node.validation.maxLength}`] : []),
      "}",
    ];
  }
  if (node.kind === "label") {
    if (typeof node.attributes.for === "string") return [];
    return [
      ...(labelText(node) ? emitText(labelText(node), false, node) : []),
      ...node.children.filter((child) => child.kind === "control").flatMap((child) => emitNode(child, context)),
    ];
  }
  if (node.kind === "list") {
    const items = listItems(node);
    return [
      "ColumnLayout {",
      "    Layout.fillWidth: true",
      "    spacing: Spacing.sm",
      ...indent(items.flatMap((item) => emitText(`• ${item}`, false, node)), 1),
      "}",
    ];
  }
  if (node.kind === "listItem" || node.kind === "tableRow" || node.kind === "tableCell") return [];
  if (node.kind === "table") {
    return [
      "AppTable {",
      "    Layout.fillWidth: true",
      "    Layout.preferredHeight: Spacing.lg * 8",
      `    model: [${tableRows(node).map(qmlString).join(", ")}]`,
      `    accessibleName: ${qmlString("Migrated table")}`,
      "}",
    ];
  }
  if (node.kind === "image") {
    const label = node.image?.alt || "Image placeholder";
    return [
      "AppCard {",
      "    Layout.fillWidth: true",
      `    accessibleName: ${qmlString(label)}`,
      "    Text {",
      "        anchors.centerIn: parent",
      `        text: ${qmlString(`[Image] ${label}`)}`,
      "        color: Theme.textSecondary",
      "        font.family: Typography.family",
      "        font.pixelSize: Typography.body",
      "    }",
      "}",
    ];
  }
  if (node.tag === "head" || node.tag === "link" || node.tag === "style" || node.tag === "meta" || node.tag === "title") return [];
  if (node.tag === "html" || node.tag === "body" || node.kind === "document") return emitChildren(node, context);
  return emitLayout(node, node.kind === "section" || node.kind === "article", context);
}

export function generateProject(ir: MigrationIR): GeneratedProject {
  const context = createGenerationContext(ir);
  const content = emitChildren(ir.root, context);
  const qml = [
    "pragma ComponentBehavior: Bound",
    "",
    "import QtQuick",
    "import QtQuick.Layouts",
    "import Company.Design",
    "",
    "Item {",
    "    id: root",
    "    implicitWidth: 1024",
    "    implicitHeight: 720",
    `    Accessible.name: ${qmlString(`Migrated ${ir.sourceFile}`)}`,
    "    Accessible.role: Accessible.Pane",
    "",
    "    ColumnLayout {",
    "        anchors.fill: parent",
    "        anchors.margins: Spacing.lg",
    "        spacing: Spacing.md",
    ...indent(content, 2),
    "    }",
    "}",
    "",
  ].join("\n");
  if (Buffer.byteLength(qml, "utf8") > GENERATOR_LIMITS.outputBytes) throw new Error("OUTPUT_LIMIT_EXCEEDED");
  const files = { "Main.qml": qml };
  return {
    files,
    report: { version: 1, sourceFile: ir.sourceFile, generatedFiles: Object.keys(files), diagnostics: sortDiagnostics(context.diagnostics) },
  };
}

function validateOutputFiles(files: Readonly<Record<string, string>>): Array<[string, string]> {
  const entries = Object.entries(files).toSorted(([left], [right]) => left.localeCompare(right, "en"));
  if (entries.length > GENERATOR_LIMITS.files) throw new Error("OUTPUT_FILE_LIMIT_EXCEEDED");
  const collisionKeys = new Set<string>();
  let bytes = 0;
  for (const [name, contents] of entries) {
    const normalized = name.normalize("NFC").replaceAll("\\", "/");
    if (!/^[\p{L}\p{N}_.-]+\.qml$/iu.test(normalized) || normalized.startsWith(".") || normalized.includes("/") || path.isAbsolute(normalized)) {
      throw new Error(`INVALID_OUTPUT_FILE: ${name}`);
    }
    const collisionKey = normalized.toLocaleLowerCase("en-US");
    if (collisionKeys.has(collisionKey)) throw new Error(`OUTPUT_NAME_COLLISION: ${name}`);
    collisionKeys.add(collisionKey);
    bytes += Buffer.byteLength(contents, "utf8");
  }
  if (bytes > GENERATOR_LIMITS.outputBytes) throw new Error("OUTPUT_LIMIT_EXCEEDED");
  return entries;
}

export async function generateToDirectory(outputDirectory: string, generated: GeneratedProject): Promise<void> {
  const output = path.resolve(outputDirectory);
  const entries = validateOutputFiles(generated.files);
  const parent = await prepareStableParent(output);
  await assertTargetMissing(output);
  const temporary = ownedStagePath(parent, output, "directory");
  await mkdir(temporary, { recursive: false });
  const stageIdentity = await capturePathIdentity(temporary);
  const stagedFiles: Array<{ path: string; identity: PathIdentity }> = [];
  try {
    for (const [name, contents] of entries) {
      const filePath = path.join(temporary, name);
      stagedFiles.push({ path: filePath, identity: await writeDurableNewFile(filePath, contents) });
    }
    await requireStableParent(parent);
    await assertTargetMissing(output);
    await requireOwnedDirectory(temporary, stageIdentity, stagedFiles);
    await rename(temporary, output);
  } catch (error) {
    await removeOwnedDirectory(temporary, parent, stageIdentity, stagedFiles);
    throw await normalizePublishError(error, output);
  }
}

function ownedStagePath(parent: StableParentSnapshot, targetPath: string, kind: "directory" | "file"): string {
  const suffix = kind === "directory" ? "stage" : "tmp";
  return path.join(parent.path, `.${path.basename(targetPath)}.qbrowser-${randomUUID()}.${suffix}`);
}

async function writeDurableNewFile(filePath: string, contents: string): Promise<PathIdentity> {
  const handle = await open(filePath, "wx", 0o600);
  let identity: PathIdentity | undefined;
  let failure: unknown;
  let failed = false;
  try {
    await handle.writeFile(contents, { encoding: "utf8" });
    await handle.sync();
    identity = identityFromStats(await handle.stat({ bigint: true }));
  } catch (error) {
    failed = true;
    failure = error;
    try { identity ??= identityFromStats(await handle.stat({ bigint: true })); }
    catch { /* The handle has no usable identity; do not unlink a named path. */ }
  } finally {
    await handle.close();
  }
  if (failed) {
    if (identity && await pathHasIdentity(filePath, identity)) await unlink(filePath);
    throw failure;
  }
  if (!identity) throw new Error(`OUTPUT_STAGE_IDENTITY_UNAVAILABLE: ${filePath}`);
  return identity;
}

async function assertTargetMissing(targetPath: string): Promise<void> {
  try {
    await lstat(targetPath);
    throw new Error(`OUTPUT_ALREADY_EXISTS: ${targetPath}`);
  } catch (error) {
    if (error instanceof Error && error.message.startsWith("OUTPUT_ALREADY_EXISTS")) throw error;
    if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
  }
}

async function normalizePublishError(error: unknown, targetPath: string): Promise<unknown> {
  try {
    await lstat(targetPath);
    return new Error(`OUTPUT_ALREADY_EXISTS: ${targetPath}`);
  } catch (targetError) {
    if (!(targetError instanceof Error) || !("code" in targetError) || targetError.code !== "ENOENT") return targetError;
  }
  return error;
}

async function removeOwnedFile(stagePath: string, parent: StableParentSnapshot, identity: PathIdentity): Promise<void> {
  if (!await stableParentUnchanged(parent)) return;
  if (!await pathHasIdentity(stagePath, identity)) return;
  try { await unlink(stagePath); }
  catch (error) {
    if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
  }
}

async function removeOwnedDirectory(
  stagePath: string,
  parent: StableParentSnapshot,
  identity: PathIdentity,
  files: ReadonlyArray<{ path: string; identity: PathIdentity }>,
): Promise<void> {
  if (!await stableParentUnchanged(parent) || !await pathHasIdentity(stagePath, identity)) return;
  for (const file of files) {
    if (!await pathHasIdentity(stagePath, identity)) return;
    if (await pathHasIdentity(file.path, file.identity)) await unlink(file.path);
  }
  if (!await pathHasIdentity(stagePath, identity)) return;
  try { await rmdir(stagePath); }
  catch (error) {
    if (!(error instanceof Error) || !("code" in error) || !["ENOENT", "ENOTEMPTY", "EEXIST"].includes(String(error.code))) throw error;
  }
}

async function requireOwnedFile(stagePath: string, identity: PathIdentity): Promise<void> {
  if (!await pathHasIdentity(stagePath, identity)) throw new Error(`OUTPUT_STAGE_CHANGED: ${stagePath}`);
}

async function requireOwnedDirectory(
  stagePath: string,
  identity: PathIdentity,
  files: ReadonlyArray<{ path: string; identity: PathIdentity }>,
): Promise<void> {
  await requireOwnedFile(stagePath, identity);
  for (const file of files) await requireOwnedFile(file.path, file.identity);
}

interface ParentLease {
  path: string;
  identity: PathIdentity;
  handle: Awaited<ReturnType<typeof open>>;
  parent: StableParentSnapshot;
}

async function acquireParentLease(parent: StableParentSnapshot, label: string): Promise<ParentLease> {
  const leasePath = path.join(parent.path, `.${label}.qbrowser-${randomUUID()}.lock`);
  const handle = await open(leasePath, "wx", 0o600);
  try {
    await handle.writeFile("qbrowser-migrator-parent-lease\n", { encoding: "utf8" });
    await handle.sync();
    return { path: leasePath, identity: identityFromStats(await handle.stat({ bigint: true })), handle, parent };
  } catch (error) {
    await handle.close();
    throw error;
  }
}

async function releaseParentLease(lease: ParentLease | undefined): Promise<void> {
  if (!lease) return;
  await lease.handle.close();
  await removeOwnedFile(lease.path, lease.parent, lease.identity);
}

async function requireParentLease(lease: ParentLease): Promise<void> {
  await requireStableParent(lease.parent);
  await requireOwnedFile(lease.path, lease.identity);
}

async function runParentRaceHook(
  name: "afterGenerationStaged" | "beforeOutputCommit" | "beforeReportPublish",
  output: string,
  report: string,
  parent: StableParentSnapshot,
): Promise<void> {
  try { await runStableIoHook(name, output, report); }
  catch (error) {
    if (error instanceof Error && "code" in error && ["EPERM", "EACCES"].includes(String(error.code))) {
      throw new Error(`OUTPUT_PARENT_CHANGED: ${parent.path}`);
    }
    throw error;
  }
}

/** Publish one durable file with an exclusive hard-link operation. */
export async function publishNewFile(filePath: string, contents: string): Promise<void> {
  const target = path.resolve(filePath);
  const parent = await prepareStableParent(target);
  await assertTargetMissing(target);
  const stage = ownedStagePath(parent, target, "file");
  const stageIdentity = await writeDurableNewFile(stage, contents);
  try {
    await requireStableParent(parent);
    await requireOwnedFile(stage, stageIdentity);
    await link(stage, target);
  } catch (error) {
    throw await normalizePublishError(error, target);
  } finally {
    await removeOwnedFile(stage, parent, stageIdentity);
  }
}

/** Stage both artifacts, publish the directory without replacement, then link the report exclusively. */
export async function publishGenerationTransaction(
  outputDirectory: string,
  reportPath: string,
  generated: GeneratedProject,
): Promise<void> {
  const output = path.resolve(outputDirectory);
  const report = path.resolve(reportPath);
  assertDisjointGenerationPaths(output, report);
  const entries = validateOutputFiles(generated.files);
  const reportContents = `${JSON.stringify(generated.report, null, 2)}\n`;
  if (Buffer.byteLength(reportContents, "utf8") > GENERATOR_LIMITS.outputBytes) throw new Error("OUTPUT_LIMIT_EXCEEDED");

  const outputPlan = await planStableParent(output);
  const reportPlan = await planStableParent(report);
  await assertTargetMissing(output);
  await assertTargetMissing(report);
  const outputStage = path.join(outputPlan.path, `.${path.basename(output)}.qbrowser-${randomUUID()}.stage`);
  const reportStage = path.join(reportPlan.path, `.${path.basename(report)}.qbrowser-${randomUUID()}.tmp`);
  const ownedParents: OwnedDirectoryMap = new Map();
  let outputParent: StableParentSnapshot | undefined;
  let reportParent: StableParentSnapshot | undefined;
  let outputStageIdentity: PathIdentity | undefined;
  let outputLease: ParentLease | undefined;
  let reportLease: ParentLease | undefined;
  const stagedFiles: Array<{ path: string; identity: PathIdentity }> = [];
  let reportStageIdentity: PathIdentity | undefined;
  let reportPublished = false;
  let outputPublished = false;
  let succeeded = false;
  try {
    outputParent = await materializeStableParent(outputPlan, ownedParents);
    reportParent = await materializeStableParent(reportPlan, ownedParents);
    await requireStableParent(outputParent);
    await requireStableParent(reportParent);
    await assertTargetMissing(output);
    await assertTargetMissing(report);
    // On Windows an open child file denies ancestor rename/delete. POSIX permits
    // renames, so the portable fallback relies on repeated identity barriers and
    // does not claim protection from an active attacker that restores state.
    outputLease = await acquireParentLease(outputParent, path.basename(output));
    reportLease = await acquireParentLease(reportParent, path.basename(report));
    await mkdir(outputStage, { recursive: false });
    outputStageIdentity = await capturePathIdentity(outputStage);
    for (const [name, contents] of entries) {
      const filePath = path.join(outputStage, name);
      stagedFiles.push({ path: filePath, identity: await writeDurableNewFile(filePath, contents) });
    }
    reportStageIdentity = await writeDurableNewFile(reportStage, reportContents);
    await runParentRaceHook("afterGenerationStaged", output, report, outputParent);
    await requireStableParent(outputParent);
    await requireStableParent(reportParent);
    await requireParentLease(outputLease);
    await requireParentLease(reportLease);
    await requireOwnedDirectory(outputStage, outputStageIdentity, stagedFiles);
    await requireOwnedFile(reportStage, reportStageIdentity);
    await assertTargetMissing(output);
    await assertTargetMissing(report);

    await runParentRaceHook("beforeOutputCommit", output, report, outputParent);
    await requireStableParent(outputParent);
    await requireStableParent(reportParent);
    await requireParentLease(outputLease);
    await requireParentLease(reportLease);
    await assertTargetMissing(output);
    await assertTargetMissing(report);
    await requireOwnedDirectory(outputStage, outputStageIdentity, stagedFiles);
    try {
      await rename(outputStage, output);
    } catch (error) {
      throw await normalizePublishError(error, output);
    }
    outputPublished = true;

    await runStableIoHook("afterOutputPublish", output, report);
    await requireStableParent(outputParent);
    await requireStableParent(reportParent);
    await requireParentLease(outputLease);
    await requireParentLease(reportLease);
    const publishedFiles = stagedFiles.map((file) => ({ ...file, path: path.join(output, path.basename(file.path)) }));
    await requireOwnedDirectory(output, outputStageIdentity, publishedFiles);
    await requireOwnedFile(reportStage, reportStageIdentity);
    await assertTargetMissing(report);

    await runParentRaceHook("beforeReportPublish", output, report, reportParent);
    await requireStableParent(outputParent);
    await requireStableParent(reportParent);
    await requireParentLease(outputLease);
    await requireParentLease(reportLease);
    await requireOwnedDirectory(output, outputStageIdentity, publishedFiles);
    await requireOwnedFile(reportStage, reportStageIdentity);
    await assertTargetMissing(report);
    try {
      await link(reportStage, report);
    } catch (error) {
      throw await normalizePublishError(error, report);
    }
    reportPublished = true;
    succeeded = true;
  } catch (error) {
    if (outputPublished && !reportPublished && outputParent && outputStageIdentity) {
      await runStableIoHook("beforeOutputRollback", output, report);
      const publishedFiles = stagedFiles.map((file) => ({ ...file, path: path.join(output, path.basename(file.path)) }));
      if (await stableParentUnchanged(outputParent) && await pathHasIdentity(output, outputStageIdentity)) {
        await removeOwnedDirectory(output, outputParent, outputStageIdentity, publishedFiles);
        outputPublished = false;
      }
    }
    throw error;
  } finally {
    if (!outputPublished && outputParent && outputStageIdentity && await stableParentUnchanged(outputParent)) {
      await removeOwnedDirectory(outputStage, outputParent, outputStageIdentity, stagedFiles);
    }
    if (reportStageIdentity && reportParent) await removeOwnedFile(reportStage, reportParent, reportStageIdentity);
    await releaseParentLease(reportLease);
    await releaseParentLease(outputLease);
    if (!succeeded) await rollbackOwnedDirectories(ownedParents);
  }
}
