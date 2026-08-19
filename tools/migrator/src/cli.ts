#!/usr/bin/env node
import { realpathSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { assertDisjointGenerationPaths, assertGenerationPlatform, generateProject, publishGenerationTransaction, publishNewFile } from "./generator.js";
import { scanFile } from "./scanner.js";

export interface CliIo {
  stdout(message: string): void;
  stderr(message: string): void;
}

function usage(): string {
  return [
    "Usage:",
    "  qbrowser-migrate scan <input> --json <ir.json>",
    "  qbrowser-migrate generate <input> --output <dir> --report <report.json>",
  ].join("\n");
}

function parseFlag(args: string[], name: string): string {
  const index = args.indexOf(name);
  if (index < 0 || index + 1 >= args.length || args[index + 1]!.startsWith("--")) throw new Error(`MISSING_ARGUMENT: ${name}`);
  return args[index + 1]!;
}

export async function runCli(args: string[], io: CliIo = {
  stdout: (message) => process.stdout.write(message),
  stderr: (message) => process.stderr.write(message),
}, platform: string = process.platform): Promise<number> {
  try {
    const command = args[0];
    const input = args[1];
    if (!command || !input) throw new Error("INVALID_ARGUMENTS");
    if (command === "scan") {
      if (args.length !== 4 || args[2] !== "--json") throw new Error("INVALID_ARGUMENTS");
      const ir = await scanFile(input);
      const destination = parseFlag(args, "--json");
      await publishNewFile(destination, `${JSON.stringify(ir, null, 2)}\n`);
      io.stdout(`Scanned ${ir.sourceFile}: ${ir.diagnostics.length} diagnostic(s)\n`);
      return ir.diagnostics.some((item) => item.severity === "fatal") ? 2 : 0;
    }
    if (command === "generate") {
      if (args.length !== 6 || args[2] !== "--output" || args[4] !== "--report") throw new Error("INVALID_ARGUMENTS");
      assertGenerationPlatform(platform);
      const output = parseFlag(args, "--output");
      const report = parseFlag(args, "--report");
      const resolvedOutput = path.resolve(output);
      const resolvedReport = path.resolve(report);
      assertDisjointGenerationPaths(resolvedOutput, resolvedReport);
      const ir = await scanFile(input);
      if (ir.diagnostics.some((item) => item.severity === "fatal")) {
        io.stdout(`Generation blocked: ${ir.diagnostics.length} diagnostic(s)\n`);
        return 2;
      }
      const generated = generateProject(ir);
      await publishGenerationTransaction(output, report, generated);
      io.stdout(`Generated ${generated.report.generatedFiles.length} file(s): ${generated.report.diagnostics.length} diagnostic(s)\n`);
      return generated.report.diagnostics.some((item) => item.severity === "fatal") ? 2 : 0;
    }
    throw new Error(`UNKNOWN_COMMAND: ${command}`);
  } catch (error) {
    const message = error instanceof Error ? error.message : "UNKNOWN_ERROR";
    io.stderr(`qbrowser-migrate: ${message}\n${usage()}\n`);
    return message === "INVALID_ARGUMENTS" || message.startsWith("MISSING_ARGUMENT") || message.startsWith("UNKNOWN_COMMAND") ? 64 : 1;
  }
}

function canonicalEntryPath(value: string): string {
  const canonical = path.normalize(realpathSync.native(path.resolve(value)));
  return process.platform === "win32" ? canonical.toLocaleLowerCase("en-US") : canonical;
}

if (process.argv[1] && canonicalEntryPath(process.argv[1]) === canonicalEntryPath(fileURLToPath(import.meta.url))) {
  process.exitCode = await runCli(process.argv.slice(2));
}
