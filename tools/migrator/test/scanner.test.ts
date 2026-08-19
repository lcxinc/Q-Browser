import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { describe, expect, test } from "vitest";

import { SCANNER_LIMITS, scanDocument, scanFile } from "../src/scanner.ts";
import type { MigrationNode } from "../src/types.ts";

const repositoryRoot = path.resolve(import.meta.dirname, "../../..");

function flatten(node: MigrationNode): MigrationNode[] {
  return [node, ...node.children.flatMap(flatten)];
}

describe("migrator scanner and IR", () => {
  test("extracts semantic structure, CSS basics, deterministic IDs, and source ranges", async () => {
    const input = path.join(repositoryRoot, "fixtures/migration/dashboard/index.html");
    const first = await scanFile(input);
    const second = await scanFile(input);
    const nodes = flatten(first.root);

    expect(first).toEqual(second);
    expect(JSON.parse(JSON.stringify(first))).toEqual(first);
    expect(nodes.map((node) => node.kind)).toEqual(expect.arrayContaining([
      "navigation", "main", "heading", "section", "article", "text",
    ]));
    expect(new Set(nodes.map((node) => node.id)).size).toBe(nodes.length);
    expect(nodes.every((node) => node.location.file === "index.html")).toBe(true);
    expect(nodes.every((node) => node.location.start.line > 0 && node.location.start.column > 0)).toBe(true);
    expect(first.styles.variables).toEqual({ "--brand": "#315efb", "--space": "16px" });
    expect(nodes.find((node) => node.kind === "section")?.style).toMatchObject({
      display: "grid", gridTemplateColumns: "repeat(2,1fr)", gap: "16px",
    });
  });

  test("models forms, labels, controls, validation, tables, lists, and local image metadata", async () => {
    const cases = ["form", "list", "settings"] as const;
    const scanned = await Promise.all(cases.map((name) =>
      scanFile(path.join(repositoryRoot, `fixtures/migration/${name}/index.html`))));
    const formNodes = flatten(scanned[0].root);
    const listNodes = flatten(scanned[1].root);
    const settingsNodes = flatten(scanned[2].root);

    expect(formNodes.find((node) => node.kind === "control" && node.attributes.name === "customer")?.validation)
      .toEqual({ required: true, minLength: 2 });
    expect(formNodes.filter((node) => node.kind === "label")).toHaveLength(2);
    expect(listNodes.map((node) => node.kind)).toEqual(expect.arrayContaining(["list", "listItem", "table", "tableRow", "tableCell"]));
    expect(settingsNodes.find((node) => node.kind === "image")?.image).toEqual({
      source: "icons/theme.png", alt: "Theme preview", width: 320, height: 180, local: true,
    });
  });

  test("does not execute active content or fetch network resources", async () => {
    let executed = false;
    Object.defineProperty(globalThis, "__migratorExecuted", { set: () => { executed = true; }, configurable: true });
    const ir = scanDocument({
      sourceFile: "unsafe.html",
      html: "<main onclick=\"globalThis.__migratorExecuted=true\"><script>globalThis.__migratorExecuted=true</script><img src=\"https://example.invalid/pixel\"><h1>Safe</h1></main>",
    });
    delete (globalThis as { __migratorExecuted?: unknown }).__migratorExecuted;

    expect(executed).toBe(false);
    expect(flatten(ir.root).some((node) => "onclick" in node.attributes)).toBe(false);
    expect(flatten(ir.root).find((node) => node.kind === "image")?.image).toMatchObject({ source: "", local: false });
    expect(ir.diagnostics.map((diagnostic) => diagnostic.code)).toEqual(expect.arrayContaining([
      "UNSUPPORTED_SCRIPT", "UNSUPPORTED_REMOTE_IMAGE",
    ]));
  });

  test("normalizes LF and CRLF input to the same IR semantics", async () => {
    const html = await readFile(path.join(repositoryRoot, "fixtures/migration/list/index.html"), "utf8");
    const lf = scanDocument({ sourceFile: "list.html", html: html.replaceAll("\r\n", "\n") });
    const crlf = scanDocument({ sourceFile: "list.html", html: html.replaceAll("\r\n", "\n").replaceAll("\n", "\r\n") });

    const semantics = (node: MigrationNode): unknown => ({
      ...node,
      location: undefined,
      children: node.children.map(semantics),
    });
    expect(semantics(lf.root)).toEqual(semantics(crlf.root));
    expect(crlf.root.location.end.offset).toBe(lf.root.location.end.offset + 1);
  });

  test("parses bounded inline style elements without executing or exposing them as content", () => {
    const ir = scanDocument({
      sourceFile: "inline.html",
      html: "<style>:root { --accent: #123456 } main { display: flex; gap: 9px }</style><main><h1>Inline</h1></main>",
    });
    const nodes = flatten(ir.root);
    expect(ir.styles.variables).toEqual({ "--accent": "#123456" });
    expect(nodes.find((node) => node.kind === "main")?.style).toMatchObject({ display: "flex", gap: "9px" });
    expect(nodes.some((node) => node.tag === "style")).toBe(false);
    expect(nodes.some((node) => node.text?.includes("--accent"))).toBe(false);
  });

  test("captures basic flex alignment in the serializable IR", () => {
    const ir = scanDocument({
      sourceFile: "flex.html",
      html: "<main class='layout'>Content</main>",
      stylesheets: [{ sourceFile: "flex.css", css: ".layout { display: flex; flex-direction: row; justify-content: space-between; align-items: center; gap: 4px; }" }],
    });
    expect(flatten(ir.root).find((node) => node.kind === "main")?.style).toMatchObject({
      display: "flex", flexDirection: "row", gap: "4px",
    });
    expect(ir.diagnostics.filter((item) => item.code === "UNSUPPORTED_CSS_PROPERTY")).toHaveLength(2);
  });

  test("fails closed on invalid UTF-8 and supports NFC Unicode Windows paths", async () => {
    const temporaryRoot = path.join(os.tmpdir(), `迁移-é-${process.pid}-${Date.now()}`);
    try {
      await mkdir(temporaryRoot, { recursive: true });
      const invalid = path.join(temporaryRoot, "invalid.html");
      await writeFile(invalid, Buffer.from([0x3c, 0x68, 0x31, 0x3e, 0xc3, 0x28]));
      await expect(scanFile(invalid)).rejects.toThrow("INVALID_UTF8");
      const unicode = path.join(temporaryRoot, "控制台.html");
      await writeFile(unicode, "<main><h1>Résumé 设置</h1></main>", "utf8");
      const ir = await scanFile(unicode);
      expect(ir.sourceFile).toBe("控制台.html");
      expect(flatten(ir.root).some((node) => node.text === "Résumé 设置")).toBe(true);
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });

  test("uses exact normalized document ranges and diagnoses remote stylesheets without loading them", () => {
    const ir = scanDocument({
      sourceFile: "ranges.html",
      html: "<!doctype html>\n<link rel='stylesheet' href='https://example.invalid/x.css'>\n<main>Safe</main>",
    });
    expect(ir.root.location.end).toEqual({ line: 3, column: 18, offset: 94 });
    expect(ir.diagnostics.find((item) => item.code === "UNSUPPORTED_EXTERNAL_STYLESHEET")?.location.start)
      .toMatchObject({ line: 2, column: 1 });
  });

  test("classifies every inline declaration with source-located diagnostics", () => {
    const ir = scanDocument({
      sourceFile: "inline-unsafe.html",
      html: "<!doctype html>\n<main style=\"display:block; position:absolute; animation:pulse 1s; transition-duration:2s; background-image:url(https://invalid/x)\">Safe</main>",
    });
    const diagnostics = ir.diagnostics.filter((item) => item.location.file === "inline-unsafe.html");
    expect(diagnostics.map((item) => item.code)).toEqual([
      "UNSUPPORTED_LAYOUT", "UNSUPPORTED_LAYOUT", "UNSUPPORTED_ANIMATION", "UNSUPPORTED_ANIMATION", "UNSUPPORTED_CSS_PROPERTY",
    ]);
    expect(diagnostics.map((item) => item.location.start.line)).toEqual([2, 2, 2, 2, 2]);
    expect(new Set(diagnostics.map((item) => item.location.start.column)).size).toBe(5);
  });

  test("maps inline diagnostics through named, decimal, and hexadecimal HTML entities", () => {
    const html = "<!doctype html>\n<main style=\"/*😀&amp;&#x1F600;*/ &Tab;&#112;osition:absolute; transition-duration:2s\">Safe</main>";
    const ir = scanDocument({ sourceFile: "entity-style.html", html });
    const layout = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_LAYOUT")!;
    const animation = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_ANIMATION")!;
    expect(layout.location.start).toEqual({
      line: 2,
      column: html.indexOf("&#112;") - html.lastIndexOf("\n", html.indexOf("&#112;")),
      offset: html.indexOf("&#112;"),
    });
    expect(animation.location.start.offset).toBe(html.indexOf("transition-duration"));
  });

  test("maps multiline entity-decoded inline CSS back to raw CRLF and astral offsets", () => {
    const html = "<!doctype html>\r\n<main style=\"/*😀&amp;*/\r\n  &#x70;osition:absolute;\r\n  transition-duration:2s\">Safe</main>";
    const ir = scanDocument({ sourceFile: "entity-lines.html", html });
    const layout = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_LAYOUT")!;
    const animation = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_ANIMATION")!;
    expect(layout.location.start).toMatchObject({ line: 3, column: 3, offset: html.indexOf("&#x70;") });
    expect(animation.location.start).toMatchObject({ line: 4, column: 3, offset: html.indexOf("transition-duration") });
  });

  test("uses the complete raw style value range when decoded offsets cannot be mapped exactly", () => {
    const html = "<main style=\"/*e\u0301*/ position:absolute\">Safe</main>";
    const ir = scanDocument({ sourceFile: "fallback-style.html", html });
    const diagnostic = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_LAYOUT")!;
    const rawStart = html.indexOf("\"") + 1;
    const rawEnd = html.lastIndexOf("\"");
    expect(diagnostic.location).toEqual({
      file: "fallback-style.html",
      start: { line: 1, column: rawStart + 1, offset: rawStart },
      end: { line: 1, column: rawEnd + 1, offset: rawEnd },
    });
  });

  test("preserves canonical root-relative CSS paths and document cascade order", async () => {
    const temporaryRoot = path.join(os.tmpdir(), `qbrowser-css-paths-${process.pid}-${Date.now()}`);
    try {
      await mkdir(path.join(temporaryRoot, "a"), { recursive: true });
      await mkdir(path.join(temporaryRoot, "b"), { recursive: true });
      await writeFile(path.join(temporaryRoot, "index.html"), "<!doctype html><link rel='stylesheet' href='b/styles.css'><link rel='stylesheet' href='a/styles.css'><main class='x'>Safe</main>", "utf8");
      await writeFile(path.join(temporaryRoot, "a/styles.css"), ".x { display:flex; gap:4px; unknown-a: 1 }", "utf8");
      await writeFile(path.join(temporaryRoot, "b/styles.css"), ".x { display:flex; gap:2px; unknown-b: 1 }", "utf8");
      const ir = await scanFile(path.join(temporaryRoot, "index.html"));
      expect(flatten(ir.root).find((node) => node.kind === "main")?.style.gap).toBe("4px");
      expect(ir.diagnostics.filter((item) => item.code === "UNSUPPORTED_CSS_PROPERTY").map((item) => item.location.file))
        .toEqual(["a/styles.css", "b/styles.css"]);
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });

  test("applies interleaved inline and external stylesheets in document order", async () => {
    const temporaryRoot = path.join(os.tmpdir(), `qbrowser-css-document-order-${process.pid}-${Date.now()}`);
    try {
      await mkdir(temporaryRoot, { recursive: true });
      await writeFile(path.join(temporaryRoot, "index.html"), [
        "<style>.x { display:flex; gap:1px }</style>",
        "<link rel='alternate STYLEsheet preload' href='middle.css'>",
        "<style>.x { gap:3px }</style>",
        "<main class='x'>Safe</main>",
      ].join("\n"), "utf8");
      await writeFile(path.join(temporaryRoot, "middle.css"), ".x { gap:2px }", "utf8");
      const ir = await scanFile(path.join(temporaryRoot, "index.html"));
      expect(flatten(ir.root).find((node) => node.kind === "main")?.style.gap).toBe("3px");
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });

  test("enforces the stylesheet file budget while collecting descriptors", () => {
    const links = Array.from({ length: SCANNER_LIMITS.files }, (_, index) =>
      `<link rel='stylesheet' href='sheet-${index}.css'>`).join("");
    expect(() => scanDocument({ sourceFile: "files.html", html: `${links}<main>Safe</main>` }))
      .toThrow("FILE_LIMIT_EXCEEDED");
  });

  test("uses iterative bounded DOM traversal for very deep and wide documents", () => {
    const deep = "<main>" + "<div>".repeat(20_000) + "x" + "</div>".repeat(20_000) + "</main>";
    expect(() => scanDocument({ sourceFile: "deep.html", html: deep })).toThrow("DOM_DEPTH_LIMIT_EXCEEDED");
    const wide = `<main>${"<span>x</span>".repeat(10_100)}</main>`;
    expect(() => scanDocument({ sourceFile: "wide.html", html: wide })).toThrow("NODE_LIMIT_EXCEEDED");
  });

  test("computes root end positions for CR, LF, and CRLF", () => {
    const endings = ["<main>A</main>\rB", "<main>A</main>\nB", "<main>A</main>\r\nB"];
    expect(endings.map((html) => scanDocument({ sourceFile: "lines.html", html }).root.location.end))
      .toEqual([
        { line: 2, column: 2, offset: 16 },
        { line: 2, column: 2, offset: 16 },
        { line: 2, column: 2, offset: 17 },
      ]);
  });

  test("treats rel as an ASCII-whitespace case-insensitive token list", () => {
    const ir = scanDocument({
      sourceFile: "rel.html",
      html: "<link rel='alternate\tSTYLEsheet\npreload' href='https://example.invalid/theme.css'><main>Safe</main>",
    });
    expect(ir.diagnostics.map((item) => item.code)).toContain("UNSUPPORTED_EXTERNAL_STYLESHEET");
  });

  test("preserves custom-property case and diagnoses undefined and cyclic var references", () => {
    const ir = scanDocument({
      sourceFile: "variables.html",
      html: "<style>:root { --Brand: 6px; --brand: 10px; --a: var(--b); --b: var(--a) } .x { display:flex; gap:var(--Brand); grid-template-columns:var(--missing); flex-direction:var(--a) }</style><main class='x'>Safe</main>",
    });
    const main = flatten(ir.root).find((node) => node.kind === "main")!;
    expect(ir.styles.variables).toMatchObject({ "--Brand": "6px", "--brand": "10px" });
    expect(main.style.gap).toBe("6px");
    expect(ir.diagnostics.map((item) => item.code)).toEqual(expect.arrayContaining([
      "UNDEFINED_CSS_VARIABLE", "CYCLIC_CSS_VARIABLE",
    ]));
  });

  test("diagnoses unsupported custom-property scope and important cascade", () => {
    const ir = scanDocument({
      sourceFile: "variable-cascade.html",
      html: "<style>:root { --Brand: 6px !important } .x { --Local: 4px; display:flex; gap:var(--Local) }</style><main class='x'>Safe</main>",
    });
    expect(ir.styles.variables).toEqual({});
    expect(ir.diagnostics.map((item) => item.code)).toEqual(expect.arrayContaining([
      "UNSUPPORTED_CSS_CASCADE", "UNSUPPORTED_CSS_VARIABLE_SCOPE", "UNDEFINED_CSS_VARIABLE",
    ]));
  });

  test("uses important tombstones for supported properties and root variables", () => {
    const ir = scanDocument({
      sourceFile: "important.html",
      html: "<style>:root { --Brand:4px; --Brand:8px !important; --Brand:10px } .x { display:flex; flex-direction:var(--Brand); gap:4px } #target { gap:6px } .x { gap:1em !important }</style><main id='target' class='x'>Safe</main>",
    });
    const main = flatten(ir.root).find((node) => node.kind === "main")!;
    expect(main.style).toEqual({ display: "flex" });
    expect(ir.styles.variables).toEqual({});
    expect(ir.diagnostics.filter((item) => item.code === "UNSUPPORTED_CSS_CASCADE")).toHaveLength(2);
    expect(ir.diagnostics.map((item) => item.code)).toContain("UNDEFINED_CSS_VARIABLE");

    const inline = scanDocument({
      sourceFile: "inline-important.html",
      html: "<main class='x' style='gap:2em !important'>Safe</main>",
      stylesheets: [{ sourceFile: "base.css", css: ".x { display:flex; gap:4px }" }],
    });
    expect(flatten(inline.root).find((node) => node.kind === "main")?.style).toEqual({ display: "flex" });
  });

  test("locates unsupported custom properties declared in an inline style", () => {
    const ir = scanDocument({
      sourceFile: "inline-variable.html",
      html: "<main style='display:flex; --Local:4px; gap:var(--Local)'>Safe</main>",
    });
    const diagnostic = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_CSS_VARIABLE_SCOPE");
    expect(diagnostic?.location).toMatchObject({
      file: "inline-variable.html",
      start: { line: 1, column: 28 },
    });
  });

  test("uses var fallback and never silently accepts unsupported explicit CSS semantics", () => {
    const ir = scanDocument({
      sourceFile: "capabilities.html",
      html: "<main class='x'>Safe</main>",
      stylesheets: [{ sourceFile: "capabilities.css", css: ".x { display:flex; gap:var(--missing, 12px); justify-content:center; align-items:center; color:red; font-size:18px; margin:4px; padding:8px }" }],
    });
    const main = flatten(ir.root).find((node) => node.kind === "main")!;
    expect(main.style).toEqual({ display: "flex", gap: "12px" });
    expect(ir.diagnostics.filter((item) => item.code === "UNSUPPORTED_CSS_PROPERTY")).toHaveLength(6);
  });

  test("rejects unmappable gap values and applies specificity before source order", () => {
    const ir = scanDocument({
      sourceFile: "cascade.html",
      html: "<main id='target' class='x'>Safe</main>",
      stylesheets: [{ sourceFile: "cascade.css", css: ".x { display:flex; gap:calc(1px + 2%); } #target { gap:4px } .x { gap:8px !important }" }],
    });
    expect(flatten(ir.root).find((node) => node.kind === "main")?.style.gap).toBeUndefined();
    expect(ir.diagnostics.map((item) => item.code)).toEqual(expect.arrayContaining([
      "UNSUPPORTED_LAYOUT", "UNSUPPORTED_CSS_CASCADE",
    ]));
  });

  test("diagnoses supported CSS used on nodes or combinations the generator cannot consume", () => {
    const ir = scanDocument({
      sourceFile: "style-context.html",
      html: "<style>input { display:flex; gap:8px } main { gap:4px; grid-template-columns:repeat(2,1fr) }</style><main><input></main>",
    });
    const nodes = flatten(ir.root);
    expect(nodes.find((node) => node.kind === "control")?.style).toEqual({});
    expect(nodes.find((node) => node.kind === "main")?.style).toEqual({});
    expect(ir.diagnostics.filter((item) => item.code === "UNSUPPORTED_CSS_CONTEXT")).toHaveLength(4);
  });

  test("does not revive lower-priority supported values beneath unsupported cascade winners", () => {
    const stylesheetWinner = scanDocument({
      sourceFile: "cascade-tombstone.html",
      html: "<main class='x'>Safe</main>",
      stylesheets: [{ sourceFile: "cascade.css", css: ".x { display:flex; gap:8px } .x { display:block; gap:1em }" }],
    });
    expect(flatten(stylesheetWinner.root).find((node) => node.kind === "main")?.style).toEqual({});
    expect(stylesheetWinner.diagnostics.filter((item) => item.code === "UNSUPPORTED_LAYOUT")).toHaveLength(2);

    const inlineWinner = scanDocument({
      sourceFile: "inline-tombstone.html",
      html: "<main class='x' style='display:block'>Safe</main>",
      stylesheets: [{ sourceFile: "base.css", css: ".x { display:flex; gap:8px }" }],
    });
    expect(flatten(inlineWinner.root).find((node) => node.kind === "main")?.style).toEqual({});
  });

  test("diagnoses query and fragment stylesheet URLs instead of opening literal filenames", async () => {
    const temporaryRoot = path.join(os.tmpdir(), `qbrowser-css-url-${process.pid}-${Date.now()}`);
    try {
      await mkdir(temporaryRoot, { recursive: true });
      await writeFile(path.join(temporaryRoot, "index.html"), "<link rel='stylesheet' href='styles.css?rev=1'><link rel='stylesheet' href='theme.css#dark'><main>Safe</main>", "utf8");
      await writeFile(path.join(temporaryRoot, "styles.css"), "main { display:flex }", "utf8");
      const ir = await scanFile(path.join(temporaryRoot, "index.html"));
      expect(ir.diagnostics.filter((item) => item.code === "UNSUPPORTED_EXTERNAL_STYLESHEET")).toHaveLength(2);
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });

  test("preflights attribute value lengths even on skipped and unsupported elements", () => {
    const oversized = "x".repeat(SCANNER_LIMITS.attributeLength + 1);
    for (const html of [`<style data-long='${oversized}'>main{display:flex}</style>`, `<script data-long='${oversized}'></script>`]) {
      expect(() => scanDocument({ sourceFile: "attributes.html", html })).toThrow("ATTRIBUTE_LENGTH_LIMIT_EXCEEDED");
    }
  });
});
