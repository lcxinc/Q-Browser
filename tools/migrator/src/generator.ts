import { lstat, mkdir, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";

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
  return `"${value.normalize("NFC")
    .replaceAll("\\", "\\\\")
    .replaceAll("\"", "\\\"")
    .replaceAll("\b", "\\b")
    .replaceAll("\f", "\\f")
    .replaceAll("\n", "\\n")
    .replaceAll("\r", "\\r")
    .replaceAll("\t", "\\t")
    .replaceAll("\u2028", "\\u2028")
    .replaceAll("\u2029", "\\u2029")}"`;
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

function indent(lines: string[], level: number): string[] {
  const prefix = "    ".repeat(level);
  return lines.map((line) => line.length > 0 ? `${prefix}${line}` : line);
}

function lengthValue(value: string | undefined, fallback: string): string {
  if (!value) return fallback;
  const numeric = /^(\d+(?:\.\d+)?)px$/u.exec(value);
  return numeric ? numeric[1]! : fallback;
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

function emitChildren(node: MigrationNode): string[] {
  return node.children.flatMap((child) => emitNode(child));
}

function emitLayout(node: MigrationNode, card: boolean): string[] {
  const content = emitChildren(node);
  const layout = [
    "ColumnLayout {",
    ...(card ? ["    anchors.fill: parent"] : ["    Layout.fillWidth: true"]),
    `    spacing: ${lengthValue(node.style.gap, "Spacing.md")}`,
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

function emitNode(node: MigrationNode): string[] {
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
    const label = String(node.attributes.placeholder ?? node.attributes.name ?? textContent(node) ?? "Field");
    if (node.tag === "button") return ["AppButton {", `    text: ${qmlString(textContent(node) || "Continue")}`, "}"];
    return [
      "AppTextField {",
      "    Layout.fillWidth: true",
      `    label: ${qmlString(label)}`,
      ...(node.validation?.required ? ["    required: true"] : []),
      ...(node.validation?.maxLength !== undefined ? [`    maximumLength: ${node.validation.maxLength}`] : []),
      "}",
    ];
  }
  if (node.kind === "label") return emitText(textContent(node), false, node);
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
  if (node.tag === "html" || node.tag === "body" || node.kind === "document") return emitChildren(node);
  return emitLayout(node, node.kind === "section" || node.kind === "article");
}

export function generateProject(ir: MigrationIR): GeneratedProject {
  const content = emitChildren(ir.root);
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
    report: { version: 1, sourceFile: ir.sourceFile, generatedFiles: Object.keys(files), diagnostics: ir.diagnostics },
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

export async function assertSafeOutputParent(outputPath: string): Promise<void> {
  let current = path.dirname(path.resolve(outputPath));
  while (true) {
    try {
      const currentStat = await lstat(current);
      if (currentStat.isSymbolicLink()) throw new Error(`OUTPUT_REPARSE_POINT: ${current}`);
      if (!currentStat.isDirectory()) throw new Error(`OUTPUT_PARENT_NOT_DIRECTORY: ${current}`);
    } catch (error) {
      if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
    }
    const parent = path.dirname(current);
    if (parent === current) return;
    current = parent;
  }
}

export async function generateToDirectory(outputDirectory: string, generated: GeneratedProject): Promise<void> {
  const output = path.resolve(outputDirectory);
  await assertSafeOutputParent(output);
  try { await stat(output); throw new Error(`OUTPUT_ALREADY_EXISTS: ${output}`); }
  catch (error) {
    if (error instanceof Error && error.message.startsWith("OUTPUT_ALREADY_EXISTS")) throw error;
    if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
  }
  const entries = validateOutputFiles(generated.files);
  const parent = path.dirname(output);
  await mkdir(parent, { recursive: true });
  const temporary = path.join(parent, `.${path.basename(output)}.qbrowser-${process.pid}-${Date.now()}`);
  await mkdir(temporary, { recursive: false });
  try {
    for (const [name, contents] of entries) await writeFile(path.join(temporary, name), contents, { encoding: "utf8", flag: "wx" });
    await rename(temporary, output);
  } catch (error) {
    await rm(temporary, { recursive: true, force: true });
    throw error;
  }
}
