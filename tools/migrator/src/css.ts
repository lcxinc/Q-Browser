import * as csstree from "css-tree";
import { tokenize, tokenTypes } from "css-tree/tokenizer";

import { addDiagnostic, createDiagnostic } from "./diagnostics.js";
import type { Diagnostic, MigrationNodeKind, MigrationStyle, SourceRange } from "./types.js";

export const MAX_CSS_BYTES = 1_048_576;
export const MAX_CSS_DECLARATIONS = 20_000;
export const MAX_CSS_VALUE_LENGTH = 4_096;
export const MAX_CSS_TOKENS_PER_DECLARATION = 2_048;
export const MAX_CSS_STRING_LENGTH = 2_048;

export interface CssBudget {
  declarations: number;
}

interface CssRule {
  selector: string;
  specificity: number;
  order: number;
  declarations: ReadonlyArray<CssDeclaration>;
}

interface CssDeclaration { property: string; value: string; location: SourceRange; supported: boolean; important: boolean }

export interface ParsedStylesheets {
  variables: Record<string, string>;
  rules: CssRule[];
}

export interface StylesheetSource {
  sourceFile: string;
  css: string;
  origin?: { line: number; column: number; offset: number };
  rawMapping?: {
    source: string;
    decodedToRaw: ReadonlyArray<number | undefined>;
    lineStarts: ReadonlyArray<number>;
    fallback: SourceRange;
  };
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
  ["display", "display"], ["flex-direction", "flexDirection"], ["grid-template-columns", "gridTemplateColumns"], ["gap", "gap"],
]);

const DIAGNOSTIC_PROPERTIES = new Set(["animation", "animation-name", "transition", "float", "position", "columns", "column-count"]);

export function createCssBudget(): CssBudget {
  return { declarations: 0 };
}

function location(sheet: StylesheetSource, node: CssAstNode): SourceRange {
  const start = node.loc?.start;
  const end = node.loc?.end;
  if (sheet.rawMapping) {
    const rawStart = sheet.rawMapping.decodedToRaw[start?.offset ?? -1];
    const rawEnd = sheet.rawMapping.decodedToRaw[end?.offset ?? -1];
    if (rawStart === undefined || rawEnd === undefined) return sheet.rawMapping.fallback;
    return {
      file: sheet.sourceFile,
      start: rawPosition(sheet.rawMapping.source, rawStart, sheet.rawMapping.lineStarts),
      end: rawPosition(sheet.rawMapping.source, rawEnd, sheet.rawMapping.lineStarts),
    };
  }
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

function rawPosition(source: string, offset: number, lineStarts?: ReadonlyArray<number>): { line: number; column: number; offset: number } {
  if (lineStarts) {
    let low = 0;
    let high = lineStarts.length;
    while (low + 1 < high) {
      const middle = Math.floor((low + high) / 2);
      if (lineStarts[middle]! <= offset) low = middle;
      else high = middle;
    }
    return { line: low + 1, column: offset - lineStarts[low]! + 1, offset };
  }
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

function selectorSpecificity(selector: string): number {
  if (selector.startsWith("#")) return 100;
  if (selector.startsWith(".")) return 10;
  return selector === ":root" ? 10 : 1;
}

function isSimpleSelector(selector: string): boolean {
  return selector === ":root" || /^#[A-Za-z_][\w-]*$/u.test(selector) || /^\.[A-Za-z_][\w-]*$/u.test(selector) || /^[A-Za-z][\w-]*$/u.test(selector);
}

export function parseSupportedGap(value: string): { row: string; column: string } | undefined {
  const values = value.split(" ");
  if (values.length < 1 || values.length > 2 || values.some((item) => !/^(?:0|\d+(?:\.\d+)?px)$/u.test(item))) return undefined;
  const numeric = (item: string): string => item === "0" ? "0" : item.slice(0, -2);
  return { row: numeric(values[0]!), column: numeric(values[1] ?? values[0]!) };
}

export function parseSupportedGridColumns(value: string): number | undefined {
  const repeated = /^repeat\(([1-9]\d?),1fr\)$/u.exec(value);
  if (repeated) {
    const count = Number(repeated[1]);
    return count <= 32 ? count : undefined;
  }
  if (!/^(?:1fr)(?: 1fr){0,31}$/u.test(value)) return undefined;
  return value.split(" ").length;
}

export function isGeneratedStyleSupported(
  kind: MigrationNodeKind,
  tag: string,
  style: Readonly<MigrationStyle>,
  key: keyof MigrationStyle,
): boolean {
  const layoutContainer = ["main", "section", "article", "form", "container"].includes(kind)
    && !["html", "body", "head", "link", "style", "meta", "title"].includes(tag);
  if (!layoutContainer) return false;
  if (key === "display") return true;
  if (key === "flexDirection") return style.display === "flex";
  if (key === "gridTemplateColumns") return style.display === "grid";
  if (key === "gap") return style.display === "flex" || style.display === "grid";
  return false;
}

function supportedValue(property: string, value: string): boolean {
  if (property === "display") return value === "flex" || value === "grid";
  if (property === "flex-direction") return value === "row" || value === "column";
  if (property === "gap") return parseSupportedGap(value) !== undefined;
  if (property === "grid-template-columns") return parseSupportedGridColumns(value) !== undefined;
  return true;
}

function classifyDeclaration(
  declaration: CssAstNode,
  sheet: StylesheetSource,
  diagnostics: Diagnostic[],
): CssDeclaration | undefined {
  const rawProperty = String(declaration.property ?? "");
  const property = rawProperty.startsWith("--") ? rawProperty : rawProperty.toLowerCase();
  const value = generate(declaration.value);
  if (declaration.important) {
    const declarationLocation = location(sheet, declaration);
    addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_CSS_CASCADE", "warning", `Unsupported !important declaration: ${property}`, declarationLocation,
    ));
    return property.startsWith("--") || SUPPORTED_PROPERTIES.has(property)
      ? { property, value, location: declarationLocation, supported: false, important: true }
      : undefined;
  }
  // Custom declarations are consumed only as :root values and never emit a
  // source-located diagnostic. Avoid repeatedly mapping their offsets across
  // a large HTML source when enforcing the shared inline declaration budget.
  if (property.startsWith("--")) return { property, value, location: location(sheet, declaration), supported: true, important: false };
  const declarationLocation = location(sheet, declaration);
  if (DIAGNOSTIC_PROPERTIES.has(property) || property.startsWith("animation") || property.startsWith("transition")) {
    const code = property.startsWith("animation") || property.startsWith("transition") ? "UNSUPPORTED_ANIMATION" : "UNSUPPORTED_LAYOUT";
    addDiagnostic(diagnostics, createDiagnostic(code, "warning", `Unsupported CSS property: ${property}`, declarationLocation));
    return undefined;
  }
  if (!SUPPORTED_PROPERTIES.has(property)) {
    addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_CSS_PROPERTY", "warning", `Unsupported CSS property: ${property}`, declarationLocation,
    ));
    return undefined;
  }
  if (!value.includes("var(") && !supportedValue(property, value)) {
    addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_LAYOUT", "warning", `Unsupported CSS value for ${property}: ${value}`, declarationLocation,
    ));
    return { property, value, location: declarationLocation, supported: false, important: false };
  }
  return { property, value, location: declarationLocation, supported: true, important: false };
}

function preflightDeclarations(ast: csstree.CssNode, budget: CssBudget): void {
  csstree.walk(ast, {
    visit: "Declaration",
    enter(rawDeclaration) {
      budget.declarations += 1;
      if (budget.declarations > MAX_CSS_DECLARATIONS) throw new Error("CSS_DECLARATION_LIMIT_EXCEEDED");
      const declaration = rawDeclaration as unknown as CssAstNode;
      const value = generate(declaration.value);
      if (value.length > MAX_CSS_VALUE_LENGTH) throw new Error("CSS_VALUE_LIMIT_EXCEEDED");
      let tokens = 0;
      tokenize(value, (type) => {
        if (type !== tokenTypes.WhiteSpace && type !== tokenTypes.Comment) tokens += 1;
        if (tokens > MAX_CSS_TOKENS_PER_DECLARATION) throw new Error("CSS_TOKEN_LIMIT_EXCEEDED");
      });
      csstree.walk(declaration.value as csstree.CssNode, {
        visit: "String",
        enter(rawString) {
          const stringValue = String((rawString as CssAstNode).value ?? "");
          if ([...stringValue].length > MAX_CSS_STRING_LENGTH) throw new Error("CSS_STRING_LIMIT_EXCEEDED");
        },
      });
    },
  });
}

interface CascadeCandidate {
  declaration: CssDeclaration;
  specificity: number;
  order: number;
}

function candidateWins(candidate: CascadeCandidate, previous: CascadeCandidate | undefined): boolean {
  if (!previous) return true;
  if (candidate.declaration.important !== previous.declaration.important) return candidate.declaration.important;
  return candidate.specificity > previous.specificity
    || (candidate.specificity === previous.specificity && candidate.order >= previous.order);
}

function resolvedValue(
  value: string,
  variables: Readonly<Record<string, string>>,
  diagnostics: Diagnostic[],
  declaration: CssDeclaration,
  stack: ReadonlySet<string> = new Set(),
): string | undefined {
  const variable = /^var\(\s*(--[A-Za-z_][\w-]*)\s*(?:,\s*(.+))?\)$/u.exec(value);
  if (!variable) return value;
  const name = variable[1]!;
  if (stack.has(name)) {
    addDiagnostic(diagnostics, createDiagnostic(
      "CYCLIC_CSS_VARIABLE", "warning", `Cyclic CSS variable: ${name}`, declaration.location,
    ));
    return undefined;
  }
  const defined = variables[name];
  if (defined === undefined) {
    if (variable[2] !== undefined) return resolvedValue(variable[2].trim(), variables, diagnostics, declaration, stack);
    addDiagnostic(diagnostics, createDiagnostic(
      "UNDEFINED_CSS_VARIABLE", "warning", `Undefined CSS variable: ${name}`, declaration.location,
    ));
    return undefined;
  }
  return resolvedValue(defined, variables, diagnostics, declaration, new Set([...stack, name]));
}

function finalizeDeclarations(
  declarations: ReadonlyArray<CssDeclaration>,
  variables: Readonly<Record<string, string>>,
  diagnostics: Diagnostic[],
): CssDeclaration[] {
  const result: CssDeclaration[] = [];
  for (const declaration of declarations) {
    if (!declaration.supported) { result.push(declaration); continue; }
    const value = resolvedValue(declaration.value, variables, diagnostics, declaration);
    if (value === undefined) { result.push({ ...declaration, supported: false }); continue; }
    if (!supportedValue(declaration.property, value)) {
      addDiagnostic(diagnostics, createDiagnostic(
        "UNSUPPORTED_LAYOUT", "warning", `Unsupported CSS value for ${declaration.property}: ${value}`, declaration.location,
      ));
      result.push({ ...declaration, value, supported: false });
      continue;
    }
    result.push({ ...declaration, value, supported: true });
  }
  return result;
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
  const variableWinners = new Map<string, CascadeCandidate>();
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
    preflightDeclarations(ast as csstree.CssNode, budget);

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
        const ruleOrder = order++;
        if (selectors.some((selector) => !isSimpleSelector(selector))) {
          addDiagnostic(diagnostics, createDiagnostic(
            "UNSUPPORTED_COMPLEX_SELECTOR", "warning", `Unsupported CSS selector: ${selectorText}`, location(sheet, rule),
          ));
          return;
        }
        const declarations: CssDeclaration[] = [];
        csstree.walk(rule.block as csstree.CssNode, {
          visit: "Declaration",
          enter(rawDeclaration) {
            const declaration = rawDeclaration as unknown as CssAstNode;
            const classified = classifyDeclaration(declaration, sheet, diagnostics);
            if (!classified) return;
            const { property, value } = classified;
            if (property.startsWith("--")) {
              if (selectors.some((selector) => selector !== ":root")) {
                addDiagnostic(diagnostics, createDiagnostic(
                  "UNSUPPORTED_CSS_VARIABLE_SCOPE", "warning", `Unsupported scoped CSS variable: ${property}`, classified.location,
                ));
              }
              if (selectors.includes(":root")) {
                const candidate = { declaration: classified, specificity: selectorSpecificity(":root"), order: ruleOrder };
                if (candidateWins(candidate, variableWinners.get(property))) variableWinners.set(property, candidate);
              }
              return;
            }
            declarations.push(classified);
          },
        });
        for (const selector of selectors) {
          if (selector !== ":root") rules.push({ selector, specificity: selectorSpecificity(selector), order: ruleOrder, declarations });
        }
      },
    });
  }
  for (const [property, winner] of variableWinners) {
    if (winner.declaration.supported) variables[property] = winner.declaration.value;
  }
  return { variables, rules: rules.map((rule) => ({
      ...rule,
      declarations: finalizeDeclarations(rule.declarations, variables, diagnostics),
    })) };
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
  inlineDeclarations: ReadonlyArray<CssDeclaration> = [],
  kind: MigrationNodeKind,
  diagnostics: Diagnostic[],
): MigrationStyle {
  const winners = new Map<keyof MigrationStyle, CascadeCandidate>();
  for (const rule of parsed.rules) {
    if (!selectorMatches(rule.selector, tag, attributes)) continue;
    for (const declaration of rule.declarations) {
      const key = SUPPORTED_PROPERTIES.get(declaration.property);
      if (!key) continue;
      const candidate = { declaration, specificity: rule.specificity, order: rule.order };
      if (candidateWins(candidate, winners.get(key))) winners.set(key, candidate);
    }
  }
  for (const declaration of inlineDeclarations) {
    const key = SUPPORTED_PROPERTIES.get(declaration.property);
    if (key) {
      const candidate = { declaration, specificity: 1_000, order: Number.MAX_SAFE_INTEGER };
      if (candidateWins(candidate, winners.get(key))) winners.set(key, candidate);
    }
  }
  const style: MigrationStyle = {};
  for (const [key, { declaration: winner }] of [...winners].sort(([left], [right]) => left.localeCompare(right, "en"))) {
    if (!winner.supported) continue;
    const value = winner.value;
    if (key === "display" && value !== "flex" && value !== "grid") continue;
    if (key === "flexDirection" && value !== "row" && value !== "column") continue;
    Object.assign(style, { [key]: value });
  }
  const supported: MigrationStyle = {};
  for (const [key, { declaration: winner }] of [...winners].sort(([left], [right]) => left.localeCompare(right, "en"))) {
    if (!winner.supported) continue;
    if (isGeneratedStyleSupported(kind, tag, style, key)) Object.assign(supported, { [key]: style[key] });
    else addDiagnostic(diagnostics, createDiagnostic(
      "UNSUPPORTED_CSS_CONTEXT", "warning", `Unsupported CSS context for ${String(key)} on ${tag}`, winner.location,
    ));
  }
  return supported;
}

export function parseInlineStyle(
  value: string,
  diagnostics: Diagnostic[],
  sheet: StylesheetSource,
  budget: CssBudget,
  parsed: ParsedStylesheets,
): CssDeclaration[] {
  if (value.length > MAX_CSS_VALUE_LENGTH) throw new Error("INLINE_STYLE_LIMIT_EXCEEDED");
  const ast = csstree.parse(value, { context: "declarationList", positions: true, filename: sheet.sourceFile });
  preflightDeclarations(ast, budget);
  const declarations: CssDeclaration[] = [];
  csstree.walk(ast, {
    visit: "Declaration",
    enter(rawDeclaration) {
      const declaration = rawDeclaration as unknown as CssAstNode;
      const classified = classifyDeclaration(declaration, sheet, diagnostics);
      if (classified?.property.startsWith("--")) {
        addDiagnostic(diagnostics, createDiagnostic(
          "UNSUPPORTED_CSS_VARIABLE_SCOPE", "warning", `Unsupported scoped CSS variable: ${classified.property}`, classified.location,
        ));
      } else if (classified) declarations.push(classified);
    },
  });
  return finalizeDeclarations(declarations, parsed.variables, diagnostics);
}
