import { readFile } from "node:fs/promises";
import {
  createServer,
  type Server,
  type ServerResponse,
} from "node:http";
import type { Duplex } from "node:stream";

import { createPilotFixtures } from "./fixtures.ts";
import {
  createRouteHandler,
  MAX_REQUEST_BODY_BYTES,
  MAX_REQUEST_HEADER_FIELDS,
} from "./routes.ts";

export { MAX_REQUEST_BODY_BYTES } from "./routes.ts";

const LOOPBACK_HOST = "127.0.0.1";
const MAX_HEADER_BYTES = 8 * 1024;
const HEADERS_TIMEOUT_MS = 2_000;
const REQUEST_TIMEOUT_MS = 5_000;
const MIN_REQUEST_TIMEOUT_MS = 25;
const KEEP_ALIVE_TIMEOUT_MS = 1_000;

export interface StartMockApiOptions {
  requestTimeoutMs?: number;
  onRequest?: (request: Readonly<{ method: string; target: string }>) => void;
}

export interface RunningMockApi {
  readonly host: typeof LOOPBACK_HOST;
  readonly port: number;
  readonly origin: string;
  close(): Promise<void>;
}

function requestTimeoutFor(options: StartMockApiOptions): number {
  const timeout = options.requestTimeoutMs ?? REQUEST_TIMEOUT_MS;
  if (
    !Number.isSafeInteger(timeout) ||
    timeout < MIN_REQUEST_TIMEOUT_MS ||
    timeout > REQUEST_TIMEOUT_MS
  ) {
    throw new RangeError(
      `requestTimeoutMs must be an integer from ${MIN_REQUEST_TIMEOUT_MS} to ${REQUEST_TIMEOUT_MS}`,
    );
  }
  return timeout;
}

interface HttpClientError extends Error {
  code?: string;
  rawPacket?: Buffer;
}

interface RawHeaderProbe {
  bytes: Buffer;
  fieldCount?: number;
}

function headerFieldCount(packet: Buffer): number | undefined {
  const headerEnd = packet.indexOf("\r\n\r\n");
  if (headerEnd < 0) return undefined;
  const lines = packet.subarray(0, headerEnd).toString("latin1").split("\r\n");
  if (lines.length === 0) return undefined;
  return lines.slice(1).filter((line) => line.includes(":")).length;
}

function captureRawHeader(probe: RawHeaderProbe, chunk: Buffer): void {
  if (probe.fieldCount !== undefined) return;
  const remaining = MAX_HEADER_BYTES + 4 - probe.bytes.byteLength;
  if (remaining <= 0) return;
  probe.bytes = Buffer.concat([
    probe.bytes,
    chunk.subarray(0, remaining),
  ]);
  probe.fieldCount = headerFieldCount(probe.bytes);
}

function stableClientError(
  error: HttpClientError,
  response: Duplex,
  probedHeaderCount?: number,
): void {
  if (!response.writable) {
    response.destroy();
    return;
  }
  const timedOut = error.code === "ERR_HTTP_REQUEST_TIMEOUT";
  const headerOverflow = error.code === "HPE_HEADER_OVERFLOW";
  const packetHeaderCount =
    error.rawPacket === undefined
      ? undefined
      : headerFieldCount(error.rawPacket);
  const observedHeaderCount = probedHeaderCount ?? packetHeaderCount ?? 0;
  const tooManyHeaders = observedHeaderCount > MAX_REQUEST_HEADER_FIELDS;
  const invalidFraming =
    error.code === "HPE_UNEXPECTED_CONTENT_LENGTH" ||
    error.code === "HPE_INVALID_CONTENT_LENGTH" ||
    error.code === "HPE_INVALID_TRANSFER_ENCODING";
  const status = timedOut ? 408 : headerOverflow || tooManyHeaders ? 431 : 400;
  const reason = timedOut
    ? "Request Timeout"
    : headerOverflow || tooManyHeaders
      ? "Request Header Fields Too Large"
      : "Bad Request";
  const code = timedOut
    ? "request_timeout"
    : tooManyHeaders
      ? "too_many_headers"
      : invalidFraming
        ? "invalid_http_framing"
        : "invalid_http_request";
  const message = timedOut
    ? "The HTTP request did not complete before the deadline."
    : tooManyHeaders
      ? "The request contains too many header fields."
      : "The HTTP request is malformed or exceeds the header limit.";
  const body = Buffer.from(
    JSON.stringify({
      error: {
        code,
        message,
      },
    }),
    "utf8",
  );
  response.end(
    `HTTP/1.1 ${status} ${reason}\r\n` +
      "Connection: close\r\n" +
      "Content-Type: application/json; charset=utf-8\r\n" +
      "X-Content-Type-Options: nosniff\r\n" +
      `Content-Length: ${body.byteLength}\r\n\r\n` +
      body.toString("utf8"),
  );
}

function stableRequestTimeout(response: ServerResponse): void {
  if (response.headersSent || response.writableEnded || response.destroyed) {
    response.destroy();
    return;
  }
  const body = Buffer.from(
    JSON.stringify({
      error: {
        code: "request_timeout",
        message: "The HTTP request did not complete before the deadline.",
      },
    }),
    "utf8",
  );
  response.writeHead(408, {
    connection: "close",
    "content-length": String(body.byteLength),
    "content-type": "application/json; charset=utf-8",
    "x-content-type-options": "nosniff",
  });
  response.end(body);
}

async function loadHelpDocument(): Promise<string> {
  const helpUrl = new URL("../public/help/index.html", import.meta.url);
  return await readFile(helpUrl, "utf8");
}

function closeServer(server: Server): Promise<void> {
  return new Promise((resolveClose, rejectClose) => {
    server.close((error) => {
      if (error !== undefined) {
        rejectClose(error);
      } else {
        resolveClose();
      }
    });
    server.closeIdleConnections();
    server.closeAllConnections();
  });
}

export async function startMockApi(
  options: StartMockApiOptions = {},
): Promise<RunningMockApi> {
  const requestTimeoutMs = requestTimeoutFor(options);
  const helpHtml = await loadHelpDocument();
  let handler: ReturnType<typeof createRouteHandler> | undefined;
  const protocolRejections = new WeakSet<Duplex>();
  const rawHeaderProbes = new WeakMap<Duplex, RawHeaderProbe>();
  const server = createServer(
    {
      connectionsCheckingInterval: Math.min(requestTimeoutMs, 100),
      maxHeaderSize: MAX_HEADER_BYTES,
      requestTimeout: requestTimeoutMs,
      requireHostHeader: false,
    },
    (request, response) => {
      const probe = rawHeaderProbes.get(request.socket);
      if (probe !== undefined) {
        probe.bytes = Buffer.alloc(0);
        probe.fieldCount = undefined;
      }
      request.setTimeout(requestTimeoutMs, () => stableRequestTimeout(response));
      if (handler === undefined) {
        response.writeHead(503, { connection: "close", "content-length": "0" });
        response.end();
        return;
      }
      void handler(request, response);
    },
  );
  server.headersTimeout = Math.min(HEADERS_TIMEOUT_MS, requestTimeoutMs);
  server.keepAliveTimeout = KEEP_ALIVE_TIMEOUT_MS;
  server.maxHeadersCount = 0;
  server.maxRequestsPerSocket = 100;
  server.on("connection", (socket: Duplex) => {
    const probe: RawHeaderProbe = { bytes: Buffer.alloc(0) };
    rawHeaderProbes.set(socket, probe);
    socket.prependListener("data", (chunk: Buffer) => captureRawHeader(probe, chunk));
  });
  server.on("clientError", (error, socket) => {
    if (protocolRejections.has(socket)) {
      return;
    }
    stableClientError(error, socket, rawHeaderProbes.get(socket)?.fieldCount);
  });

  await new Promise<void>((resolveListen, rejectListen) => {
    const failed = (error: Error) => {
      server.off("listening", listening);
      rejectListen(error);
    };
    const listening = () => {
      server.off("error", failed);
      resolveListen();
    };
    server.once("error", failed);
    server.once("listening", listening);
    server.listen({ host: LOOPBACK_HOST, port: 0, exclusive: true });
  });

  const address = server.address();
  if (address === null || typeof address === "string" || address.address !== LOOPBACK_HOST) {
    await closeServer(server);
    throw new Error("mock-api did not bind the required IPv4 loopback address");
  }

  const expectedAuthority = `${LOOPBACK_HOST}:${address.port}`;
  handler = createRouteHandler({
    expectedAuthority,
    expectedOrigin: `http://${expectedAuthority}`,
    fixtures: createPilotFixtures(),
    helpHtml,
    onProtocolRejection: (connection) => protocolRejections.add(connection),
    onRequest: options.onRequest,
  });

  let closePromise: Promise<void> | undefined;
  return Object.freeze({
    host: LOOPBACK_HOST,
    port: address.port,
    origin: `http://${LOOPBACK_HOST}:${address.port}`,
    close: () => {
      closePromise ??= closeServer(server);
      return closePromise;
    },
  });
}

async function runFromCommandLine(): Promise<void> {
  const api = await startMockApi({
    onRequest: (request) => {
      process.stdout.write(`${JSON.stringify({ request })}\n`);
    },
  });
  process.stdout.write(`${JSON.stringify({ origin: api.origin })}\n`);
  let closing = false;
  const stop = () => {
    if (closing) return;
    closing = true;
    void api.close().then(
      () => process.exit(0),
      () => process.exit(1),
    );
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
}

if (import.meta.main) {
  void runFromCommandLine().catch((error: unknown) => {
    const message = error instanceof Error ? error.message : "unknown startup error";
    process.stderr.write(`mock-api failed to start: ${message}\n`);
    process.exitCode = 1;
  });
}
