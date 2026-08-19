import { mkdir, readFile, rm, symlink } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { describe, expect, test } from "vitest";

import { assertGenerationPlatform, generateProject, generateToDirectory, qmlIdentifier, qmlString } from "../src/generator.ts";
import { scanDocument, scanFile } from "../src/scanner.ts";

const repositoryRoot = path.resolve(import.meta.dirname, "../../..");

describe("migrator QML generator", () => {
  test("declares generation transactions unsupported off Windows before filesystem work", () => {
    expect(() => assertGenerationPlatform("linux")).toThrow("PLATFORM_UNSUPPORTED");
    expect(() => assertGenerationPlatform("darwin")).toThrow("PLATFORM_UNSUPPORTED");
    expect(() => assertGenerationPlatform("win32")).not.toThrow();
  });

  for (const fixture of ["dashboard", "list", "form", "settings"] as const) {
    test(`matches the deterministic ${fixture} golden skeleton`, async () => {
      const input = path.join(repositoryRoot, `fixtures/migration/${fixture}/index.html`);
      const generated = generateProject(await scanFile(input));
      const goldenRoot = path.join(repositoryRoot, `tests/golden/migrator/${fixture}`);

      expect(generated.files).toEqual({
        "Main.qml": await readFile(path.join(goldenRoot, "Main.qml"), "utf8"),
      });
      expect(`${JSON.stringify(generated.report, null, 2)}\n`)
        .toBe(await readFile(path.join(goldenRoot, "report.json"), "utf8"));
      expect(generated.files["Main.qml"]).not.toMatch(/XMLHttpRequest|FileDialog|WorkerScript|Qt\.openUrlExternally|createQmlObject/u);
      expect(generated.files["Main.qml"]).not.toContain("Typography.regularWeight");
    });
  }

  test("escapes hostile text as a QML string instead of executable JavaScript", () => {
    const generated = generateProject({
      version: 1,
      sourceFile: "quoted-ä.html",
      styles: { variables: {} },
      diagnostics: [],
      root: {
        id: "document-00001", kind: "document", tag: "#document", attributes: {}, style: {}, children: [{
          id: "heading-00002", kind: "heading", tag: "h1", level: 1,
          text: undefined, attributes: {}, style: {}, children: [{
            id: "text-00003", kind: "text", text: "\"; Qt.openUrlExternally('https://evil') //\u2028line",
            attributes: {}, style: {}, children: [],
            location: { file: "quoted-ä.html", start: { line: 1, column: 5, offset: 4 }, end: { line: 1, column: 60, offset: 59 } },
          }],
          location: { file: "quoted-ä.html", start: { line: 1, column: 1, offset: 0 }, end: { line: 1, column: 65, offset: 64 } },
        }],
        location: { file: "quoted-ä.html", start: { line: 1, column: 1, offset: 0 }, end: { line: 1, column: 65, offset: 64 } },
      },
    });
    expect(generated.files["Main.qml"]).toContain("\\\"; Qt.openUrlExternally('https://evil') //\\u2028line");
    expect(generated.files["Main.qml"]).not.toContain("text: \"\";");
  });

  test("cannot break a QML source comment through a hostile IR filename", () => {
    const sourceFile = "page\u2028}\nQt.openUrlExternally('https://evil.invalid')\nItem {.html";
    const ir = scanDocument({ sourceFile, html: "<main><h1>Safe</h1></main>" });
    const qml = generateProject(ir).files["Main.qml"];
    expect(qml).not.toContain("\u2028");
    expect(qml).not.toContain("\nQt.openUrlExternally");
    expect(qml).toContain("// Source: evil.invalid___Item__.html:1:7");
  });

  test("emits RowLayout and GridLayout only for recognized IR layouts", () => {
    const row = generateProject(scanDocument({
      sourceFile: "row.html",
      html: "<!doctype html><main class='row'><section class='grid'><p>A</p><p>B</p></section></main>",
      stylesheets: [{ sourceFile: "layout.css", css: ".row { display:flex; gap:8px } .grid { display:grid; grid-template-columns:repeat(2,1fr); gap:4px }" }],
    })).files["Main.qml"];
    expect(row).toContain("RowLayout {");
    expect(row).toContain("GridLayout {");
    expect(row).toContain("columns: 2");
    expect(row).toContain("rowSpacing: 4");
    expect(row).toContain("columnSpacing: 4");
  });

  test("emits every CSS capability classified as supported and reports the rest", () => {
    const generated = generateProject(scanDocument({
      sourceFile: "css-contract.html",
      html: "<main class='row'><section class='grid'><p>A</p><p>B</p></section></main>",
      stylesheets: [{ sourceFile: "contract.css", css: ".row { display:flex; flex-direction:row; gap:8px; justify-content:center; color:red } .grid { display:grid; grid-template-columns:repeat(2,1fr); gap:4px 6px; padding:12px }" }],
    }));
    expect(generated.files["Main.qml"]).toContain("RowLayout {");
    expect(generated.files["Main.qml"]).toContain("spacing: 8");
    expect(generated.files["Main.qml"]).toContain("rowSpacing: 4");
    expect(generated.files["Main.qml"]).toContain("columnSpacing: 6");
    expect(generated.report.diagnostics.filter((item) => item.code === "UNSUPPORTED_CSS_PROPERTY")).toHaveLength(3);
  });

  test("does not classify grid track ratios that GridLayout cannot preserve", () => {
    const generated = generateProject(scanDocument({
      sourceFile: "grid-ratio.html",
      html: "<main class='grid'><p>A</p><p>B</p></main>",
      stylesheets: [{ sourceFile: "ratio.css", css: ".grid { display:grid; grid-template-columns:1fr 2fr; gap:4px }" }],
    }));
    expect(generated.files["Main.qml"]).toContain("GridLayout {");
    expect(generated.files["Main.qml"]).toContain("columns: 1");
    expect(generated.report.diagnostics).toContainEqual(expect.objectContaining({
      code: "UNSUPPORTED_LAYOUT",
      message: "Unsupported CSS value for grid-template-columns: 1fr 2fr",
    }));
  });

  test("maps only supported controls and preserves labels for unsupported controls", () => {
    const generated = generateProject(scanDocument({
      sourceFile: "controls.html",
      html: "<!doctype html><form><label for='email'>Email address</label><input id='email' type='email'><label><input type='checkbox'>Accept terms</label><label for='choice'>Choice</label><select id='choice'><option>A</option></select><input type='radio' name='pick'></form>",
    }));
    const qml = generated.files["Main.qml"];
    expect(qml).toContain('label: "Email address"');
    expect(qml).toContain('text: "Accept terms"');
    expect(qml).toContain('text: "Unsupported checkbox control"');
    expect(qml).toContain('text: "Unsupported select control"');
    expect(qml).toContain('text: "Unsupported radio control"');
    expect(qml.match(/AppTextField \{/gu)).toHaveLength(1);
    expect(generated.report.diagnostics.filter((item) => item.code === "UNSUPPORTED_CONTROL")).toHaveLength(3);
  });

  test("escapes NUL, C0, C1, separators, format controls, and lone surrogates", () => {
    const encoded = qmlString("A\0\u0001\t\n\u0085\u200B\u2028\u2029\uD800😀Z");
    expect(encoded).toBe('"A\\u0000\\u0001\\t\\n\\u0085\\u200B\\u2028\\u2029\\uD800😀Z"');
  });

  test("writes atomically and refuses to overwrite or traverse an existing output", async () => {
    const temporaryRoot = path.join(os.tmpdir(), `qbrowser-migrator-${process.pid}-${Date.now()}`);
    const output = path.join(temporaryRoot, "result");
    try {
      const generated = generateProject(await scanFile(path.join(repositoryRoot, "fixtures/migration/form/index.html")));
      await generateToDirectory(output, generated);
      expect(await readFile(path.join(output, "Main.qml"), "utf8")).toBe(generated.files["Main.qml"]);
      await expect(generateToDirectory(output, generated)).rejects.toThrow("OUTPUT_ALREADY_EXISTS");
      await expect(generateToDirectory(path.join(temporaryRoot, "..", "escape"), {
        ...generated, files: { "../escape.qml": "unsafe" },
      })).rejects.toThrow("INVALID_OUTPUT_FILE");
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });

  test("normalizes identifiers and rejects Windows case-folded output collisions", async () => {
    expect(qmlIdentifier("class")).toBe("class_item");
    expect(qmlIdentifier("42 naïve-name")).toBe("_42_naïve_name");
    expect(qmlIdentifier("", "class")).toBe("class_item");
    expect(qmlIdentifier("", "42 fallback-name")).toBe("_42_fallback_name");
    const astral = qmlIdentifier(`${"a".repeat(127)}𐐀`);
    expect(astral.isWellFormed()).toBe(true);
    expect([...astral]).toHaveLength(128);
    const temporaryRoot = path.join(os.tmpdir(), `qbrowser-migrator-collision-${process.pid}-${Date.now()}`);
    try {
      await expect(generateToDirectory(temporaryRoot, {
        files: { "Page.qml": "a", "page.qml": "b" },
        report: { version: 1, sourceFile: "x.html", generatedFiles: [], diagnostics: [] },
      })).rejects.toThrow("OUTPUT_NAME_COLLISION");
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });

  test("rejects output beneath a symlink or Windows junction ancestor", async () => {
    const temporaryRoot = path.join(os.tmpdir(), `qbrowser-migrator-reparse-${process.pid}-${Date.now()}`);
    const real = path.join(temporaryRoot, "real");
    const linked = path.join(temporaryRoot, "linked");
    try {
      await mkdir(path.join(real, "existing"), { recursive: true });
      await symlink(real, linked, process.platform === "win32" ? "junction" : "dir");
      const generated = generateProject(await scanFile(path.join(repositoryRoot, "fixtures/migration/form/index.html")));
      await expect(generateToDirectory(path.join(linked, "existing", "result"), generated))
        .rejects.toThrow("OUTPUT_REPARSE_POINT");
    } finally {
      await rm(temporaryRoot, { recursive: true, force: true });
    }
  });
});
