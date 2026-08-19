import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { describe, expect, test } from "vitest";

import { scanDocument, scanFile } from "../src/scanner.ts";
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
      display: "grid", gridTemplateColumns: "repeat(2,1fr)", gap: "var(--space)", padding: "24px",
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

    expect(lf.root).toEqual(crlf.root);
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
      display: "flex", flexDirection: "row", justifyContent: "space-between", alignItems: "center", gap: "4px",
    });
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
});
