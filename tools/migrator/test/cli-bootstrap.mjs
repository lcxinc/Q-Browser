import { runCli } from "../dist/cli.js";

process.env.PATH = "Z:\\poisoned-path";
process.env.SystemRoot = "Z:\\poisoned-system-root";
process.env.WINDIR = "Z:\\poisoned-system-root";
process.env.COMSPEC = "Z:\\poisoned-command-processor.exe";
process.env.HTTP_PROXY = "http://127.0.0.1:1";
process.env.HTTPS_PROXY = "http://127.0.0.1:1";
process.env.LANG = "tr_TR.UTF-8";
process.env.TZ = "Pacific/Kiritimati";

process.exitCode = await runCli(process.argv.slice(2));
