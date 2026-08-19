export const MIGRATION_IR_VERSION = 1 as const;

export interface SourcePosition {
  line: number;
  column: number;
  offset: number;
}

export interface SourceRange {
  file: string;
  start: SourcePosition;
  end: SourcePosition;
}

export type DiagnosticSeverity = "warning" | "error" | "fatal";

export interface Diagnostic {
  code: string;
  severity: DiagnosticSeverity;
  message: string;
  location: SourceRange;
}

export type MigrationNodeKind =
  | "document"
  | "container"
  | "navigation"
  | "main"
  | "section"
  | "article"
  | "heading"
  | "form"
  | "label"
  | "control"
  | "table"
  | "tableRow"
  | "tableCell"
  | "list"
  | "listItem"
  | "image"
  | "text";

export interface MigrationStyle {
  display?: "flex" | "grid";
  flexDirection?: "row" | "column";
  justifyContent?: "start" | "center" | "end" | "space-between" | "space-around" | "space-evenly";
  alignItems?: "start" | "center" | "end" | "stretch";
  gridTemplateColumns?: string;
  gap?: string;
  margin?: string;
  marginTop?: string;
  marginRight?: string;
  marginBottom?: string;
  marginLeft?: string;
  padding?: string;
  paddingTop?: string;
  paddingRight?: string;
  paddingBottom?: string;
  paddingLeft?: string;
  fontFamily?: string;
  fontSize?: string;
  fontWeight?: string;
  lineHeight?: string;
  color?: string;
  backgroundColor?: string;
  textAlign?: "left" | "center" | "right";
}

export interface ControlValidation {
  required?: true;
  minLength?: number;
  maxLength?: number;
  pattern?: string;
}

export interface ImageMetadata {
  source: string;
  alt: string;
  width?: number;
  height?: number;
  local: boolean;
}

export interface MigrationNode {
  id: string;
  kind: MigrationNodeKind;
  tag?: string;
  text?: string;
  level?: number;
  attributes: Record<string, string | boolean>;
  validation?: ControlValidation;
  image?: ImageMetadata;
  style: MigrationStyle;
  location: SourceRange;
  children: MigrationNode[];
}

export interface MigrationIR {
  version: typeof MIGRATION_IR_VERSION;
  sourceFile: string;
  root: MigrationNode;
  styles: { variables: Record<string, string> };
  diagnostics: Diagnostic[];
}

export interface ScanDocumentInput {
  html: string;
  sourceFile: string;
  stylesheets?: ReadonlyArray<{ sourceFile: string; css: string }>;
}
