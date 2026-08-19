#!/usr/bin/env node
import { realpathSync } from "node:fs";
import { lstat, mkdir, rename, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { assertSafeOutputParent, generateProject, generateToDirectory } from "./generator.ts";
import { scanFile } from "./scanner.ts";

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

async function writeNewAtomic(filePath: string, contents: string): Promise<void> {
  const output = path.resolve(filePath);
  await assertNewPath(output);
  await assertSafeOutputParent(output);
  const parent = path.dirname(output);
  await mkdir(parent, { recursive: true });
  const parentStat = await lstat(parent);
  if (parentStat.isSymbolicLink()) throw new Error(`OUTPUT_REPARSE_POINT: ${parent}`);
  const temporary = path.join(parent, `.${path.basename(output)}.qbrowser-${process.pid}-${Date.now()}`);
  await writeFile(temporary, contents, { encoding: "utf8", flag: "wx" });
  try { await rename(temporary, output); }
  catch (error) { throw error; }
}

async function assertNewPath(filePath: string): Promise<void> {
  const output = path.resolve(filePath);
  try { await stat(output); throw new Error(`OUTPUT_ALREADY_EXISTS: ${output}`); }
  catch (error) {
    if (error instanceof Error && error.message.startsWith("OUTPUT_ALREADY_EXISTS")) throw error;
    if (!(error instanceof Error) || !("code" in error) || error.code !== "ENOENT") throw error;
  }
}

export async function runCli(args: string[], io: CliIo = {
  stdout: (message) => process.stdout.write(message),
  stderr: (message) => process.stderr.write(message),
}): Promise<number> {
  try {
    const command = args[0];
    const input = args[1];
    if (!command || !input) throw new Error("INVALID_ARGUMENTS");
    if (command === "scan") {
      if (args.length !== 4 || args[2] !== "--json") throw new Error("INVALID_ARGUMENTS");
      const ir = await scanFile(input);
      const destination = parseFlag(args, "--json");
      await writeNewAtomic(destination, `${JSON.stringify(ir, null, 2)}\n`);
      io.stdout(`Scanned ${ir.sourceFile}: ${ir.diagnostics.length} diagnostic(s)\n`);
      return ir.diagnostics.some((item) => item.severity === "fatal") ? 2 : 0;
    }
    if (command === "generate") {
      if (args.length !== 6 || args[2] !== "--output" || args[4] !== "--report") throw new Error("INVALID_ARGUMENTS");
      const output = parseFlag(args, "--output");
      const report = parseFlag(args, "--report");
      const resolvedOutput = path.resolve(output);
      const resolvedReport = path.resolve(report);
      const reportRelativeToOutput = path.relative(resolvedOutput, resolvedReport);
      if (reportRelativeToOutput.length === 0
          || (!reportRelativeToOutput.startsWith("..") && !path.isAbsolute(reportRelativeToOutput))) {
        throw new Error("REPORT_INSIDE_OUTPUT");
      }
      await assertNewPath(resolvedOutput);
      await assertNewPath(resolvedReport);
      await assertSafeOutputParent(resolvedOutput);
      await assertSafeOutputParent(resolvedReport);
      const generated = generateProject(await scanFile(input));
      await generateToDirectory(output, generated);
      await writeNewAtomic(report, `${JSON.stringify(generated.report, null, 2)}\n`);
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
