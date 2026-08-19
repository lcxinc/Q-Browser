import type { Diagnostic, DiagnosticSeverity, SourceRange } from "./types.ts";

export const MAX_DIAGNOSTICS = 500;
export const MAX_DIAGNOSTIC_MESSAGE_LENGTH = 512;

export function createDiagnostic(
  code: string,
  severity: DiagnosticSeverity,
  message: string,
  location: SourceRange,
): Diagnostic {
  return {
    code,
    severity,
    message: message.slice(0, MAX_DIAGNOSTIC_MESSAGE_LENGTH),
    location,
  };
}

export function addDiagnostic(target: Diagnostic[], diagnostic: Diagnostic): void {
  if (target.length >= MAX_DIAGNOSTICS) {
    if (target[MAX_DIAGNOSTICS - 1]?.code !== "DIAGNOSTIC_LIMIT_EXCEEDED") {
      target[MAX_DIAGNOSTICS - 1] = createDiagnostic(
        "DIAGNOSTIC_LIMIT_EXCEEDED",
        "fatal",
        `Diagnostic limit of ${MAX_DIAGNOSTICS} exceeded`,
        diagnostic.location,
      );
    }
    return;
  }
  target.push(diagnostic);
}

export function sortDiagnostics(diagnostics: Diagnostic[]): Diagnostic[] {
  return diagnostics.toSorted((left, right) =>
    left.location.file.localeCompare(right.location.file, "en")
    || left.location.start.offset - right.location.start.offset
    || left.code.localeCompare(right.code, "en")
    || left.message.localeCompare(right.message, "en"));
}
