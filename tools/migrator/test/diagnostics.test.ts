import { describe, expect, test } from "vitest";

import { generateProject } from "../src/generator.ts";
import { scanDocument } from "../src/scanner.ts";

describe("migrator diagnostics", () => {
  test("reports unsupported constructs with stable source locations", () => {
    const html = [
      "<main>",
      "<script>alert(1)</script>",
      "<canvas></canvas>",
      "<iframe src='https://example.invalid'></iframe>",
      "<div contenteditable>editable</div>",
      "</main>",
    ].join("\n");
    const css = [
      ".card > span { color: red; }",
      ".card { animation: pulse 1s; position: absolute; }",
    ].join("\n");
    const first = scanDocument({ html, sourceFile: "unsupported.html", stylesheets: [{ sourceFile: "unsupported.css", css }] });
    const second = scanDocument({ html, sourceFile: "unsupported.html", stylesheets: [{ sourceFile: "unsupported.css", css }] });
    const summary = first.diagnostics.map(({ code, location }) => ({ code, file: location.file, line: location.start.line, column: location.start.column }));

    expect(first.diagnostics).toEqual(second.diagnostics);
    expect(summary).toEqual(expect.arrayContaining([
      { code: "UNSUPPORTED_SCRIPT", file: "unsupported.html", line: 2, column: 1 },
      { code: "UNSUPPORTED_CANVAS", file: "unsupported.html", line: 3, column: 1 },
      { code: "UNSUPPORTED_IFRAME", file: "unsupported.html", line: 4, column: 1 },
      { code: "UNSUPPORTED_CONTENTEDITABLE", file: "unsupported.html", line: 5, column: 1 },
      { code: "UNSUPPORTED_COMPLEX_SELECTOR", file: "unsupported.css", line: 1, column: 1 },
      { code: "UNSUPPORTED_ANIMATION", file: "unsupported.css", line: 2, column: 9 },
      { code: "UNSUPPORTED_LAYOUT", file: "unsupported.css", line: 2, column: 30 },
    ]));
    expect(generateProject(first).report.diagnostics).toEqual(first.diagnostics);
  });

  test("records malformed and duplicate HTML attributes using parse5 semantics", () => {
    const ir = scanDocument({
      sourceFile: "malformed.html",
      html: "<main><input name='first' name='second'><p title='unterminated></main>",
    });
    expect(ir.diagnostics.map((item) => item.code)).toContain("DUPLICATE_ATTRIBUTE");
    expect(ir.diagnostics.map((item) => item.code)).toContain("HTML_PARSE_ERROR");
    const pending = [ir.root];
    let input;
    while (pending.length > 0) {
      const node = pending.shift()!;
      if (node.tag === "input") { input = node; break; }
      pending.push(...node.children);
    }
    expect(input?.attributes.name).toBe("first");
  });

  test("fails closed at bounded input, DOM, attribute, and CSS limits", () => {
    expect(() => scanDocument({ sourceFile: "large.html", html: "x".repeat(2_097_153) })).toThrow("INPUT_LIMIT_EXCEEDED");
    expect(() => scanDocument({ sourceFile: "deep.html", html: "<div>".repeat(70) + "x" + "</div>".repeat(70) })).toThrow("DOM_DEPTH_LIMIT_EXCEEDED");
    expect(() => scanDocument({ sourceFile: "attribute.html", html: `<main title="${"x".repeat(8_193)}"></main>` })).toThrow("ATTRIBUTE_LENGTH_LIMIT_EXCEEDED");
    expect(() => scanDocument({ sourceFile: "css.html", html: "<main></main>", stylesheets: [{ sourceFile: "large.css", css: "x".repeat(1_048_577) }] })).toThrow("CSS_LIMIT_EXCEEDED");
  });

  test("caps diagnostics and marks truncation deterministically", () => {
    const ir = scanDocument({ sourceFile: "noisy.html", html: "<script></script>".repeat(510) });
    expect(ir.diagnostics).toHaveLength(500);
    expect(ir.diagnostics.at(-1)?.code).toBe("DIAGNOSTIC_LIMIT_EXCEEDED");
  });

  test("diagnoses at-rules and unsupported declarations instead of following imports or guessing", () => {
    const ir = scanDocument({
      sourceFile: "css.html",
      html: "<main class='card'>Safe</main>",
      stylesheets: [{
        sourceFile: "unsafe.css",
        css: "@import url(https://example.invalid/x.css); @media (min-width: 1px) { .card { color: red } } .card { background-image: url(https://example.invalid/x.png) }",
      }],
    });
    expect(ir.diagnostics.map((item) => item.code)).toEqual(expect.arrayContaining([
      "UNSUPPORTED_CSS_AT_RULE", "UNSUPPORTED_CSS_PROPERTY",
    ]));
  });

  test("diagnoses unsupported cascade priority instead of applying it incorrectly", () => {
    const ir = scanDocument({
      sourceFile: "cascade.html",
      html: "<main class='card'>Safe</main>",
      stylesheets: [{ sourceFile: "cascade.css", css: ".card { display:grid !important }" }],
    });
    expect(ir.diagnostics.map((item) => item.code)).toContain("UNSUPPORTED_CSS_CASCADE");
  });

  test("maps inline CSS diagnostics back to HTML line and column", () => {
    const ir = scanDocument({
      sourceFile: "inline.html",
      html: "<!doctype html>\n<style>\n.card > span { animation: pulse 1s }\n</style>\n<main>Safe</main>",
    });
    const diagnostic = ir.diagnostics.find((item) => item.code === "UNSUPPORTED_COMPLEX_SELECTOR");
    expect(diagnostic?.location).toMatchObject({
      file: "inline.html",
      start: { line: 3, column: 1 },
    });
  });

  test("enforces one declaration budget across all inline style attributes", () => {
    const declarations = Array.from({ length: 100 }, (_, index) => `--x${index}:0`).join(";");
    const html = `<!doctype html>${`<div style="${declarations}"></div>`.repeat(201)}`;
    expect(() => scanDocument({ sourceFile: "inline-budget.html", html })).toThrow("CSS_DECLARATION_LIMIT_EXCEEDED");
  });

  test("preflights declaration budgets before selector and at-rule early returns", () => {
    const declarations = "unknown:0;".repeat(20_001);
    for (const css of [`.a .b { ${declarations} }`, `@media all { .x { ${declarations} } }`]) {
      expect(() => scanDocument({
        sourceFile: "css-preflight.html",
        html: "<main class='x'>Safe</main>",
        stylesheets: [{ sourceFile: "oversize.css", css }],
      })).toThrow("CSS_DECLARATION_LIMIT_EXCEEDED");
    }
  });

  test("preflights declaration values before unsupported selector diagnostics", () => {
    expect(() => scanDocument({
      sourceFile: "css-value.html",
      html: "<main>Safe</main>",
      stylesheets: [{ sourceFile: "value.css", css: `.a > .b { gap:${"x".repeat(4_097)} }` }],
    })).toThrow("CSS_VALUE_LIMIT_EXCEEDED");
  });

  test("preflights per-declaration token and string limits independently", () => {
    expect(() => scanDocument({
      sourceFile: "css-token.html",
      html: "<main>Safe</main>",
      stylesheets: [{ sourceFile: "token.css", css: `.a > .b { --x:${"x,".repeat(1_024)}x }` }],
    })).toThrow("CSS_TOKEN_LIMIT_EXCEEDED");
    expect(() => scanDocument({
      sourceFile: "css-string.html",
      html: "<main>Safe</main>",
      stylesheets: [{ sourceFile: "string.css", css: `.a > .b { content:"${"x".repeat(2_049)}" }` }],
    })).toThrow("CSS_STRING_LIMIT_EXCEEDED");
  });

  test("uses CSS tokenization and decoded string values for declaration limits", () => {
    const escapedQuote = `${"a".repeat(1_100)}\\\"${"b".repeat(1_100)}`;
    expect(() => scanDocument({
      sourceFile: "escaped-string.html",
      html: "<main>Safe</main>",
      stylesheets: [{ sourceFile: "escaped-string.css", css: `.a > .b { content:"${escapedQuote}" }` }],
    })).toThrow("CSS_STRING_LIMIT_EXCEEDED");

    const escapedNewline = `${"a".repeat(2_048)}\\\nvalue`;
    expect(() => scanDocument({
      sourceFile: "continued-string.html",
      html: "<main>Safe</main>",
      stylesheets: [{ sourceFile: "continued-string.css", css: `.a > .b { content:"${escapedNewline}" }` }],
    })).toThrow("CSS_STRING_LIMIT_EXCEEDED");

    expect(() => scanDocument({
      sourceFile: "punctuation-string.html",
      html: "<main>Safe</main>",
      stylesheets: [{ sourceFile: "punctuation-string.css", css: `.a > .b { content:"${",".repeat(2_048)}" }` }],
    })).not.toThrow();
  });
});
