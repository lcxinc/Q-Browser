import { MIGRATION_IR_VERSION, type Diagnostic, type MigrationIR, type MigrationNode, type MigrationNodeKind, type SourceRange } from "./types.js";

export class IrBuilder {
  readonly diagnostics: Diagnostic[] = [];
  #nextId = 1;

  createNode(kind: MigrationNodeKind, location: SourceRange, properties: Partial<MigrationNode> = {}): MigrationNode {
    return {
      id: `${kind}-${String(this.#nextId++).padStart(5, "0")}`,
      kind,
      attributes: {},
      style: {},
      location,
      children: [],
      ...properties,
    };
  }

  finish(sourceFile: string, root: MigrationNode, variables: Record<string, string>): MigrationIR {
    return {
      version: MIGRATION_IR_VERSION,
      sourceFile,
      root,
      styles: { variables: Object.fromEntries(Object.entries(variables).toSorted(([left], [right]) => left.localeCompare(right, "en"))) },
      diagnostics: this.diagnostics,
    };
  }
}
