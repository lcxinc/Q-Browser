import * as csstree from "css-tree";

import { addDiagnostic, createDiagnostic } from "./diagnostics.ts";
import type { Diagnostic, MigrationStyle, SourceRange } from "./types.ts";

export const MAX_CSS_BYTES = 1_048_576;
export const MAX_CSS_DECLARATIONS = 20_000;
export const MAX_CSS_VALUE_LENGTH = 4_096;

export interface CssBudget {
  declarations: number;
}

interface CssRule {
  selector: string;
  specificity: number;
  order: number;
  declarations: ReadonlyArray<{ property: string; value: string }>;
}

export interface ParsedStylesheets {
  variables: Record<string, string>;
  rules: CssRule[];
}

export interface StylesheetSource {
  sourceFile: string;
  css: string;
  origin?: { line: number; column: number; offset: number };
}

interface CssAstNode {
  type?: string;
  name?: string;
  property?: string;
  value?: unknown;
  important?: boolean;
  prelude?: unknown;
  block?: unknown;
  loc?: { start?: { line?: number; column?: number; offset?: number }; end?: { line?: number; column?: number; offset?: number } };
}

const SUPPORTED_PROPERTIES = new Map<string, keyof MigrationStyle>([
  ["display", "display"], ["flex-direction", "flexDirection"], ["grid-template-columns", "gridTemplateColumns"],
  ["justify-content", "justifyContent"], ["align-items", "alignItems"],
  ["gap", "gap"], ["margin", "margin"], ["margin-top", "marginTop"], ["margin-right", "marginRight"],
  ["margin-bottom", "marginBottom"], ["margin-left", "marginLeft"], ["padding", "padding"],
  ["padding-top", "paddingTop"], ["padding-right", "paddingRight"], ["padding-bottom", "paddingBottom"],
  ["padding-left", "paddingLeft"], ["font-family", "fontFamily"], ["font-size", "fontSize"],
  ["font-weight", "fontWeight"], ["line-height", "lineHeight"], ["color", "color"],
  ["background-color", "backgroundColor"], ["text-align", "textAlign"],
]);

const DIAGNOSTIC_PROPERTIES = new Set(["animation", "animation-name", "transition", "float", "position", "columns", "column-count"]);

export function createCssBudget(): CssBudget {
  return { declarations: 0 };
}

function location(sheet: StylesheetSource, node: CssAstNode): SourceRange {
  const start = node.loc?.start;
  const end = node.loc?.end;
  const origin = sheet.origin ?? { line: 1, column: 1, offset: 0 };
  const startLocalLine = start?.line ?? 1;
  const endLocalLine = end?.line ?? startLocalLine;
  return {
    file: sheet.sourceFile,
    start: {
      line: origin.line + startLocalLine - 1,
      column: startLocalLine === 1 ? origin.column + (start?.column ?? 1) - 1 : start?.column ?? 1,
      offset: origin.offset + (start?.offset ?? 0),
    },
    end: {
      line: origin.line + endLocalLine - 1,
      column: endLocalLine === 1 ? origin.column + (end?.column ?? start?.column ?? 1) - 1 : end?.column ?? start?.column ?? 1,
      offset: origin.offset + (end?.offset ?? start?.offset ?? 0),
    },
  };
}

function selectorSpecificity(selector: string): number {
  if (selector.startsWith("#")) return 100;
  if (selector.startsWith(".")) return 10;
  return selector === ":root" ? 10 : 1;
}

function isSimpleSelector(selector: string): boolean {
  return selector === ":root" || /^#[A-Za-z_][\w-]*$/u.test(selector) || /^\.[A-Za-z_][\w-]*$/u.test(selector) || /^[A-Za-z][\w-]*$/u.test(selector);
}

function supportedValue(property: string, value: string): boolean {
  if (property === "display") return value === "flex" || value === "grid";
  if (property === "flex-direction") return value === "row" || value === "column";
  if (property === "justify-content") return ["start", "center", "end", "space-between", "space-around", "space-evenly"].includes(value);
  if (property === "align-items") return ["start", "center", "end", "stretch"].includes(value);
  if (property === "text-align") return ["left", "center", "right"].includes(value);
  if (property === "grid-template-columns") {
    return /^repeat\([1-9]\d{0,2},(?:[1-9]\d*(?:\.\d+)?(?:px|fr)|auto|minmax\([^()]+\))\)$/u.test(value)
      || /^(?:[1-9]\d*(?:\.\d+)?(?:px|fr)|auto|minmax\([^()]+\))(?: (?:[1-9]\d*(?:\.\d+)?(?:px|fr)|auto|minmax\([^()]+\))){0,31}$/u.test(value);
  }
  return true;
}

function classifyDeclaration(
  declaration: CssAstNode,
  sheet: StylesheetSource,
  diagnostics: Diagnostic[],
  budget: CssBudget,
): { property: string; value: string } | undefined {
  budget.declarations += 1;
  if (budget.declarations > MAX_CSS_DECLARATIONS) throw new Error("CSS_DECLARATION_LIMIT_EXCEEDED");
  const property = String(declaration.property ?? "").toLowerCase();
  const value = generate(declaration.value);
  if (value.length > MAX_CSS_VALUE_LENGTH) throw new Error("CSS_VALUE_LIMIT_EXCEEDED");
  if (property.startsWith("--")) return { property, value };
  if (declaration.important) {
    addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_CSS_CASCADE", "warning", `Unsupported !important declaration: ${property}`, location(sheet, declaration),
    ));
    return undefined;
  }
  if (DIAGNOSTIC_PROPERTIES.has(property) || property.startsWith("animation") || property.startsWith("transition")) {
    const code = property.startsWith("animation") || property.startsWith("transition") ? "UNSUPPORTED_ANIMATION" : "UNSUPPORTED_LAYOUT";
    addDiagnostic(diagnostics, createDiagnostic(code, "warning", `Unsupported CSS property: ${property}`, location(sheet, declaration)));
    return undefined;
  }
  if (!SUPPORTED_PROPERTIES.has(property)) {
    addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_CSS_PROPERTY", "warning", `Unsupported CSS property: ${property}`, location(sheet, declaration),
    ));
    return undefined;
  }
  if (!supportedValue(property, value)) {
    addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_LAYOUT", "warning", `Unsupported CSS value for ${property}: ${value}`, location(sheet, declaration),
    ));
    return undefined;
  }
  return { property, value };
}

function generate(node: unknown): string {
  return csstree.generate(node as csstree.CssNode, { compact: true }).trim();
}

export function parseStylesheets(
  sheets: ReadonlyArray<StylesheetSource>,
  diagnostics: Diagnostic[],
  budget: CssBudget = createCssBudget(),
): ParsedStylesheets {
  const variables: Record<string, string> = {};
  const rules: CssRule[] = [];
  let order = 0;

  for (const sheet of sheets) {
    if (Buffer.byteLength(sheet.css, "utf8") > MAX_CSS_BYTES) {
      throw new Error(`CSS_LIMIT_EXCEEDED: ${sheet.sourceFile}`);
    }
    let ast: unknown;
    try {
      ast = csstree.parse(sheet.css, { positions: true, filename: sheet.sourceFile });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Malformed CSS";
      addDiagnostic(diagnostics, createDiagnostic("CSS_PARSE_ERROR", "error", message, {
        file: sheet.sourceFile, start: { line: 1, column: 1, offset: 0 }, end: { line: 1, column: 1, offset: 0 },
      }));
      continue;
    }

    csstree.walk(ast as csstree.CssNode, {
      visit: "Atrule",
      enter(rawAtRule) {
        const atRule = rawAtRule as unknown as CssAstNode;
        addDiagnostic(diagnostics, createDiagnostic(
          "UNSUPPORTED_CSS_AT_RULE", "warning", `Unsupported CSS at-rule: @${String(atRule.name ?? "unknown")}`,
          location(sheet, atRule),
        ));
      },
    });

    csstree.walk(ast as csstree.CssNode, {
      visit: "Rule",
      enter(rawRule) {
        if (this.atrule) return;
        const rule = rawRule as unknown as CssAstNode;
        const selectorText = generate(rule.prelude).trim();
        const selectors = selectorText.split(",").map((item) => item.trim());
        if (selectors.some((selector) => !isSimpleSelector(selector))) {
          addDiagnostic(diagnostics, createDiagnostic(
            "UNSUPPORTED_COMPLEX_SELECTOR", "warning", `Unsupported CSS selector: ${selectorText}`, location(sheet, rule),
          ));
          return;
        }
        const declarations: Array<{ property: string; value: string }> = [];
        csstree.walk(rule.block as csstree.CssNode, {
          visit: "Declaration",
          enter(rawDeclaration) {
            const declaration = rawDeclaration as unknown as CssAstNode;
            const classified = classifyDeclaration(declaration, sheet, diagnostics, budget);
            if (!classified) return;
            const { property, value } = classified;
            if (property.startsWith("--")) {
              if (selectors.includes(":root")) variables[property] = value;
              return;
            }
            declarations.push({ property, value });
          },
        });
        for (const selector of selectors) {
          if (selector !== ":root") rules.push({ selector, specificity: selectorSpecificity(selector), order: order++, declarations });
        }
      },
    });
  }
  return { variables, rules };
}

function selectorMatches(selector: string, tag: string, attributes: Readonly<Record<string, string | boolean>>): boolean {
  if (selector.startsWith("#")) return attributes.id === selector.slice(1);
  if (selector.startsWith(".")) return typeof attributes.class === "string" && attributes.class.split(/\s+/u).includes(selector.slice(1));
  return selector.toLowerCase() === tag;
}

export function resolveStyle(
  parsed: ParsedStylesheets,
  tag: string,
  attributes: Readonly<Record<string, string | boolean>>,
  inlineDeclarations: ReadonlyArray<{ property: string; value: string }> = [],
): MigrationStyle {
  const winners = new Map<keyof MigrationStyle, { specificity: number; order: number; value: string }>();
  for (const rule of parsed.rules) {
    if (!selectorMatches(rule.selector, tag, attributes)) continue;
    for (const declaration of rule.declarations) {
      const key = SUPPORTED_PROPERTIES.get(declaration.property);
      if (!key) continue;
      const previous = winners.get(key);
      if (!previous || rule.specificity > previous.specificity || (rule.specificity === previous.specificity && rule.order >= previous.order)) {
        winners.set(key, { specificity: rule.specificity, order: rule.order, value: declaration.value });
      }
    }
  }
  for (const declaration of inlineDeclarations) {
    const key = SUPPORTED_PROPERTIES.get(declaration.property);
    if (key) winners.set(key, { specificity: 1_000, order: Number.MAX_SAFE_INTEGER, value: declaration.value });
  }
  const style: MigrationStyle = {};
  for (const [key, winner] of [...winners].sort(([left], [right]) => left.localeCompare(right, "en"))) {
    const variable = /^var\((--[A-Za-z_][\w-]*)\)$/u.exec(winner.value);
    const value = variable ? (parsed.variables[variable[1]!] ?? winner.value) : winner.value;
    if (key === "display" && value !== "flex" && value !== "grid") continue;
    if (key === "flexDirection" && value !== "row" && value !== "column") continue;
    if (key === "justifyContent" && !["start", "center", "end", "space-between", "space-around", "space-evenly"].includes(value)) continue;
    if (key === "alignItems" && !["start", "center", "end", "stretch"].includes(value)) continue;
    if (key === "textAlign" && value !== "left" && value !== "center" && value !== "right") continue;
    Object.assign(style, { [key]: value });
  }
  return style;
}

export function parseInlineStyle(
  value: string,
  diagnostics: Diagnostic[],
  sheet: StylesheetSource,
  budget: CssBudget,
): Array<{ property: string; value: string }> {
  if (value.length > MAX_CSS_VALUE_LENGTH) throw new Error("INLINE_STYLE_LIMIT_EXCEEDED");
  const ast = csstree.parse(value, { context: "declarationList", positions: true, filename: sheet.sourceFile });
  const declarations: Array<{ property: string; value: string }> = [];
  csstree.walk(ast, {
    visit: "Declaration",
    enter(rawDeclaration) {
      const declaration = rawDeclaration as unknown as CssAstNode;
      const classified = classifyDeclaration(declaration, sheet, diagnostics, budget);
      if (classified && !classified.property.startsWith("--")) declarations.push(classified);
    },
  });
  return declarations;
}
