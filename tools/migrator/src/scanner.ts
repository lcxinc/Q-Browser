import path from "node:path";
import { DecodingMode, EntityDecoder } from "entities";
import { htmlDecodeTree } from "entities/decode";
import { parse, type DefaultTreeAdapterMap, type ParserError } from "parse5";

import { createCssBudget, parseInlineStyle, parseStylesheets, resolveStyle, type StylesheetSource } from "./css.ts";
import { addDiagnostic, createDiagnostic, sortDiagnostics } from "./diagnostics.ts";
import { IrBuilder } from "./ir.ts";
import { readStableRegularFile } from "./stable-io.ts";
import type { ControlValidation, MigrationIR, MigrationNode, MigrationNodeKind, ScanDocumentInput, SourceRange } from "./types.ts";

export const SCANNER_LIMITS = Object.freeze({
  inputBytes: 2_097_152,
  files: 32,
  domDepth: 64,
  nodes: 10_000,
  attributesPerNode: 64,
  attributeLength: 8_192,
  textLength: 65_536,
});

type P5Node = DefaultTreeAdapterMap["node"];
type P5Element = DefaultTreeAdapterMap["element"];
type P5TextNode = DefaultTreeAdapterMap["textNode"];

const ALLOWED_ATTRIBUTES = new Set([
  "id", "class", "name", "type", "for", "placeholder", "aria-label", "role", "value",
  "checked", "disabled", "required", "minlength", "maxlength", "pattern", "width", "height", "alt", "src", "style",
]);
const BOOLEAN_ATTRIBUTES = new Set(["checked", "disabled", "required"]);
const UNSUPPORTED_TAGS = new Map([
  ["script", "UNSUPPORTED_SCRIPT"], ["canvas", "UNSUPPORTED_CANVAS"], ["iframe", "UNSUPPORTED_IFRAME"],
]);

function safeDisplayName(sourceFile: string): string {
  const normalized = sourceFile.replaceAll("\\", "/");
  return normalized.slice(normalized.lastIndexOf("/") + 1).normalize("NFC") || "input.html";
}

function rangeFor(node: P5Node, file: string): SourceRange {
  const loc = "sourceCodeLocation" in node ? node.sourceCodeLocation : undefined;
  const startLine = loc?.startLine ?? 1;
  const startCol = loc?.startCol ?? 1;
  const startOffset = loc?.startOffset ?? 0;
  return {
    file,
    start: { line: startLine, column: startCol, offset: startOffset },
    end: { line: loc?.endLine ?? startLine, column: loc?.endCol ?? startCol, offset: loc?.endOffset ?? startOffset },
  };
}

function rangeForParseError(error: ParserError, file: string): SourceRange {
  return {
    file,
    start: { line: error.startLine, column: error.startCol, offset: error.startOffset },
    end: { line: error.endLine, column: error.endCol, offset: error.endOffset },
  };
}

function isElement(node: P5Node): node is P5Element {
  return "tagName" in node;
}

function isText(node: P5Node): node is P5TextNode {
  return node.nodeName === "#text";
}

function nodeKind(tag: string): MigrationNodeKind {
  if (/^h[1-6]$/u.test(tag)) return "heading";
  const kinds: Partial<Record<string, MigrationNodeKind>> = {
    nav: "navigation", main: "main", section: "section", article: "article", form: "form", label: "label",
    input: "control", select: "control", textarea: "control", button: "control", table: "table", tr: "tableRow",
    th: "tableCell", td: "tableCell", ul: "list", ol: "list", li: "listItem", img: "image",
  };
  return kinds[tag] ?? "container";
}

function boundedInteger(value: string | undefined): number | undefined {
  if (!value || !/^\d{1,7}$/u.test(value)) return undefined;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed <= 1_000_000 ? parsed : undefined;
}

function isLocalAssetReference(value: string): boolean {
  const normalized = value.trim().replaceAll("\\", "/");
  return normalized.length > 0
    && !normalized.startsWith("/")
    && !normalized.startsWith("//")
    && !/^[A-Za-z][A-Za-z0-9+.-]*:/u.test(normalized)
    && !normalized.split("/").includes("..");
}

function collectAttributes(element: P5Element): Record<string, string | boolean> {
  if (element.attrs.length > SCANNER_LIMITS.attributesPerNode) throw new Error("ATTRIBUTE_LIMIT_EXCEEDED");
  const attributes: Record<string, string | boolean> = {};
  for (const attribute of element.attrs) {
    const name = attribute.name.toLowerCase();
    if (attribute.value.length > SCANNER_LIMITS.attributeLength) throw new Error("ATTRIBUTE_LENGTH_LIMIT_EXCEEDED");
    if (name.startsWith("on") || !ALLOWED_ATTRIBUTES.has(name)) continue;
    attributes[name] = BOOLEAN_ATTRIBUTES.has(name) ? true : attribute.value.normalize("NFC");
  }
  return attributes;
}

function validationFor(attributes: Readonly<Record<string, string | boolean>>): ControlValidation | undefined {
  const validation: ControlValidation = {};
  if (attributes.required === true) validation.required = true;
  if (typeof attributes.minlength === "string") validation.minLength = boundedInteger(attributes.minlength);
  if (typeof attributes.maxlength === "string") validation.maxLength = boundedInteger(attributes.maxlength);
  if (typeof attributes.pattern === "string") validation.pattern = attributes.pattern;
  return Object.keys(validation).length > 0 ? validation : undefined;
}

function childNodes(node: P5Node): P5Node[] {
  return "childNodes" in node ? [...node.childNodes] : [];
}

function positionAt(source: string, offset: number): { line: number; column: number; offset: number } {
  let line = 1;
  let column = 1;
  for (let index = 0; index < offset;) {
    if (source[index] === "\r") {
      index += source[index + 1] === "\n" ? 2 : 1;
      line += 1;
      column = 1;
    } else if (source[index] === "\n") {
      index += 1;
      line += 1;
      column = 1;
    } else {
      index += 1;
      column += 1;
    }
  }
  return { line, column, offset };
}

function inlineStyleSource(
  element: P5Element,
  source: string,
  sourceFile: string,
  decodedValue: string,
): StylesheetSource {
  const attribute = element.sourceCodeLocation?.attrs?.style;
  if (!attribute) return { sourceFile, css: decodedValue };
  const raw = source.slice(attribute.startOffset, attribute.endOffset);
  const equals = raw.indexOf("=");
  if (equals < 0) return { sourceFile, css: decodedValue };
  let relative = equals + 1;
  while (relative < raw.length && /[ \t\n\r\f]/u.test(raw[relative]!)) relative += 1;
  const quote = raw[relative] === "\"" || raw[relative] === "'" ? raw[relative] : undefined;
  if (quote) relative += 1;
  const rawStart = attribute.startOffset + relative;
  const rawEnd = quote && raw.endsWith(quote) ? attribute.endOffset - 1 : attribute.endOffset;
  const decodedToRaw = decodedOffsetMap(source.slice(rawStart, rawEnd), decodedValue, rawStart);
  const fallback: SourceRange = {
    file: sourceFile,
    start: positionAt(source, rawStart),
    end: positionAt(source, rawEnd),
  };
  return {
    sourceFile,
    css: decodedValue,
    rawMapping: { source, decodedToRaw: decodedToRaw ?? [], fallback },
  };
}

function decodedOffsetMap(raw: string, decoded: string, absoluteStart: number): Array<number | undefined> | undefined {
  const offsets: Array<number | undefined> = new Array(decoded.length + 1);
  let rawIndex = 0;
  let decodedIndex = 0;
  offsets[0] = absoluteStart;

  function append(consumed: number, value: string, exactInternal: boolean): boolean {
    if (decoded.slice(decodedIndex, decodedIndex + value.length) !== value) return false;
    offsets[decodedIndex] = absoluteStart + rawIndex;
    for (let index = 1; index < value.length; index += 1) {
      offsets[decodedIndex + index] = exactInternal ? absoluteStart + rawIndex + index : undefined;
    }
    rawIndex += consumed;
    decodedIndex += value.length;
    offsets[decodedIndex] = absoluteStart + rawIndex;
    return true;
  }

  while (rawIndex < raw.length) {
    if (raw[rawIndex] === "\r") {
      const consumed = raw[rawIndex + 1] === "\n" ? 2 : 1;
      if (!append(consumed, "\n", false)) return undefined;
      continue;
    }
    if (raw[rawIndex] === "&") {
      let decodedEntity = "";
      const decoder = new EntityDecoder(htmlDecodeTree, (codePoint) => { decodedEntity += String.fromCodePoint(codePoint); });
      decoder.startEntity(DecodingMode.Attribute);
      let consumed = decoder.write(raw, rawIndex + 1);
      if (consumed < 0) consumed = decoder.end();
      if (consumed > 0) {
        if (!append(consumed, decodedEntity, false)) return undefined;
        continue;
      }
    }
    const codePoint = raw.codePointAt(rawIndex);
    if (codePoint === undefined) return undefined;
    const value = codePoint === 0 ? "\uFFFD" : String.fromCodePoint(codePoint);
    if (!append(codePoint > 0xffff ? 2 : 1, value, true)) return undefined;
  }
  return decodedIndex === decoded.length ? offsets : undefined;
}

function inlineStylesheets(document: DefaultTreeAdapterMap["document"], sourceFile: string): StylesheetSource[] {
  const sheets: StylesheetSource[] = [];
  function visit(node: P5Node): void {
    if (isElement(node) && node.tagName.toLowerCase() === "style") {
      const css = childNodes(node).filter(isText).map((child) => child.value).join("");
      const start = node.sourceCodeLocation?.startTag;
      sheets.push({
        sourceFile,
        css,
        origin: start ? { line: start.endLine, column: start.endCol, offset: start.endOffset } : undefined,
      });
      return;
    }
    for (const child of childNodes(node)) visit(child);
  }
  for (const node of document.childNodes) visit(node);
  return sheets;
}

export function scanDocument(input: ScanDocumentInput): MigrationIR {
  if (Buffer.byteLength(input.html, "utf8") > SCANNER_LIMITS.inputBytes) throw new Error("INPUT_LIMIT_EXCEEDED");
  const html = input.html;
  const sourceFile = safeDisplayName(input.sourceFile);
  const builder = new IrBuilder();
  const document = parse(html, {
    sourceCodeLocationInfo: true,
    onParseError(error) {
      addDiagnostic(builder.diagnostics, createDiagnostic(
        error.code === "duplicate-attribute" ? "DUPLICATE_ATTRIBUTE" : "HTML_PARSE_ERROR",
        "warning", `HTML parse issue: ${error.code}`, rangeForParseError(error, sourceFile),
      ));
    },
  });
  const cssBudget = createCssBudget();
  const parsedCss = parseStylesheets([...inlineStylesheets(document, sourceFile), ...(input.stylesheets ?? [])], builder.diagnostics, cssBudget);
  const rootLocation: SourceRange = {
    file: sourceFile,
    start: { line: 1, column: 1, offset: 0 },
    end: {
      line: html.split("\n").length,
      column: html.length - html.lastIndexOf("\n"),
      offset: html.length,
    },
  };
  const root = builder.createNode("document", rootLocation, { tag: "#document" });
  let count = 1;

  function visit(node: P5Node, parent: MigrationNode, depth: number): void {
    if (depth > SCANNER_LIMITS.domDepth) throw new Error("DOM_DEPTH_LIMIT_EXCEEDED");
    if (++count > SCANNER_LIMITS.nodes) throw new Error("NODE_LIMIT_EXCEEDED");

    if (isText(node)) {
      const text = node.value.replace(/[ \t\n\r\f]+/gu, " ").trim().normalize("NFC");
      if (text.length === 0) return;
      if (text.length > SCANNER_LIMITS.textLength) throw new Error("TEXT_LIMIT_EXCEEDED");
      parent.children.push(builder.createNode("text", rangeFor(node, sourceFile), { text }));
      return;
    }
    if (!isElement(node)) {
      for (const child of childNodes(node)) visit(child, parent, depth);
      return;
    }

    const tag = node.tagName.toLowerCase();
    const location = rangeFor(node, sourceFile);
    if (tag === "style") return;
    const rawAttributes = Object.fromEntries(node.attrs.map((attribute) => [attribute.name.toLowerCase(), attribute.value]));
    if (tag === "link" && rawAttributes.rel?.toLowerCase() === "stylesheet" && rawAttributes.href
        && !isLocalAssetReference(rawAttributes.href)) {
      addDiagnostic(builder.diagnostics, createDiagnostic(
        "UNSUPPORTED_EXTERNAL_STYLESHEET", "warning", "External stylesheet is not loaded", location,
      ));
    }
    const unsupportedCode = UNSUPPORTED_TAGS.get(tag);
    if (unsupportedCode) {
      addDiagnostic(builder.diagnostics, createDiagnostic(unsupportedCode, "warning", `Unsupported HTML element: ${tag}`, location));
      return;
    }
    if (node.attrs.some((attribute) => attribute.name.toLowerCase() === "contenteditable")) {
      addDiagnostic(builder.diagnostics, createDiagnostic("UNSUPPORTED_CONTENTEDITABLE", "warning", "contenteditable is not migrated", location));
    }

    const attributes = collectAttributes(node);
    let inlineDeclarations: Array<{ property: string; value: string }> = [];
    if (typeof attributes.style === "string") {
      try {
        inlineDeclarations = parseInlineStyle(
          attributes.style,
          builder.diagnostics,
          inlineStyleSource(node, html, sourceFile, attributes.style),
          cssBudget,
        );
      }
      catch (error) {
        if (error instanceof Error && error.message.endsWith("_LIMIT_EXCEEDED")) throw error;
        addDiagnostic(builder.diagnostics, createDiagnostic("CSS_PARSE_ERROR", "warning", "Malformed inline style", location));
      }
      delete attributes.style;
    }
    const kind = nodeKind(tag);
    const imageSource = kind === "image" && typeof attributes.src === "string" ? attributes.src : "";
    const imageIsLocal = isLocalAssetReference(imageSource);
    if (kind === "image" && imageSource.length > 0 && !imageIsLocal) {
      addDiagnostic(builder.diagnostics, createDiagnostic(
        "UNSUPPORTED_REMOTE_IMAGE", "warning", "Remote image source is not retained or loaded", location,
      ));
    }
    const migrationNode = builder.createNode(kind, location, {
      tag,
      level: kind === "heading" ? Number(tag.slice(1)) : undefined,
      attributes,
      validation: kind === "control" ? validationFor(attributes) : undefined,
      image: kind === "image" ? {
        source: imageIsLocal ? imageSource : "",
        alt: typeof attributes.alt === "string" ? attributes.alt : "",
        width: boundedInteger(typeof attributes.width === "string" ? attributes.width : undefined),
        height: boundedInteger(typeof attributes.height === "string" ? attributes.height : undefined),
        local: imageIsLocal,
      } : undefined,
      style: resolveStyle(parsedCss, tag, attributes, inlineDeclarations),
    });
    if (kind === "image") delete migrationNode.attributes.src;
    parent.children.push(migrationNode);
    for (const child of childNodes(node)) visit(child, migrationNode, depth + 1);
  }

  for (const node of document.childNodes) visit(node, root, 1);
  const result = builder.finish(sourceFile, root, parsedCss.variables);
  result.diagnostics = sortDiagnostics(result.diagnostics);
  return result;
}

function decodeUtf8(bytes: Uint8Array, label: string): string {
  try { return new TextDecoder("utf-8", { fatal: true }).decode(bytes); }
  catch { throw new Error(`INVALID_UTF8: ${label}`); }
}

function localStylesheetReferences(html: string): string[] {
  const document = parse(html);
  const references: string[] = [];
  function visit(node: P5Node): void {
    if (isElement(node) && node.tagName.toLowerCase() === "link") {
      const values = Object.fromEntries(node.attrs.map((attribute) => [attribute.name.toLowerCase(), attribute.value]));
      if (values.rel?.toLowerCase() === "stylesheet" && values.href && isLocalAssetReference(values.href)) references.push(values.href);
    }
    for (const child of childNodes(node)) visit(child);
  }
  for (const node of document.childNodes) visit(node);
  return references.filter((reference, index) => references.indexOf(reference) === index);
}

export async function scanFile(inputPath: string): Promise<MigrationIR> {
  const resolvedInput = path.resolve(inputPath);
  if (!/\.html?$/iu.test(resolvedInput)) throw new Error("INPUT_EXTENSION_NOT_SUPPORTED");
  const inputDirectory = path.dirname(resolvedInput);
  const html = decodeUtf8(
    await readStableRegularFile(resolvedInput, SCANNER_LIMITS.inputBytes, inputDirectory),
    path.basename(resolvedInput),
  );
  const sheets: Array<{ sourceFile: string; css: string }> = [];
  const references = localStylesheetReferences(html);
  if (references.length + 1 > SCANNER_LIMITS.files) throw new Error("FILE_LIMIT_EXCEEDED");
  for (const reference of references) {
    const candidate = path.resolve(inputDirectory, reference);
    const relative = path.relative(inputDirectory, candidate);
    if (relative.startsWith("..") || path.isAbsolute(relative)) throw new Error("SOURCE_BOUNDARY_VIOLATION");
    sheets.push({
      sourceFile: path.relative(inputDirectory, candidate).replaceAll("\\", "/").normalize("NFC"),
      css: decodeUtf8(await readStableRegularFile(candidate, 1_048_576, inputDirectory), reference),
    });
  }
  return scanDocument({ html, sourceFile: safeDisplayName(resolvedInput), stylesheets: sheets });
}
