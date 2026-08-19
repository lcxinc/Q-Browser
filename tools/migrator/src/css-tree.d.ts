declare module "css-tree" {
  export interface CssNode {
    type?: string;
    [key: string]: unknown;
  }

  export interface ParseOptions {
    context?: "declarationList";
    filename?: string;
    positions?: boolean;
  }

  export interface WalkContext {
    atrule?: CssNode | null;
  }

  export function parse(source: string, options?: ParseOptions): CssNode;
  export function generate(node: CssNode, options?: { compact?: boolean }): string;
  export function walk(node: CssNode, visitor: { visit: string; enter(this: WalkContext, node: CssNode): void }): void;
}
