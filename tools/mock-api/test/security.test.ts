import { connect } from "node:net";
import { afterEach, beforeEach, describe, expect, test } from "vitest";

import {
  MAX_REQUEST_BODY_BYTES,
  type RunningMockApi,
  startMockApi,
} from "../src/server.ts";

interface RawResponse {
  status: number;
  headers: Map<string, string>;
  body: Buffer;
}

function requestBytes(
  api: RunningMockApi,
  options: {
    method?: string;
    target?: string;
    headers?: ReadonlyArray<readonly [string, string]>;
    body?: Buffer | string;
    includeHost?: boolean;
  } = {},
): Buffer {
  const body = Buffer.isBuffer(options.body)
    ? options.body
    : Buffer.from(options.body ?? "", "utf8");
  const headers: Array<readonly [string, string]> = [];
  if (options.includeHost !== false) {
    headers.push(["Host", `${api.host}:${api.port}`]);
  }
  headers.push(...(options.headers ?? []), ["Connection", "close"]);
  const head =
    `${options.method ?? "GET"} ${options.target ?? "/api/dashboard"} HTTP/1.1\r\n` +
    headers.map(([name, value]) => `${name}: ${value}\r\n`).join("") +
    "\r\n";
  return Buffer.concat([Buffer.from(head, "ascii"), body]);
}

async function exchange(api: RunningMockApi, bytes: Buffer): Promise<RawResponse> {
  const raw = await new Promise<Buffer>((resolve, reject) => {
    const socket = connect(api.port, api.host);
    const chunks: Buffer[] = [];
    const deadline = setTimeout(() => {
      socket.destroy();
      reject(new Error("raw HTTP exchange exceeded its deadline"));
    }, 2_000);
    deadline.unref();
    socket.once("connect", () => socket.end(bytes));
    socket.on("data", (chunk: Buffer) => chunks.push(chunk));
    socket.once("error", (error) => {
      clearTimeout(deadline);
      reject(error);
    });
    socket.once("end", () => {
      clearTimeout(deadline);
      resolve(Buffer.concat(chunks));
    });
  });

  return parseRawResponse(raw);
}

function parseRawResponse(raw: Buffer): RawResponse {
  const separator = raw.indexOf("\r\n\r\n");
  if (separator < 0) throw new Error("raw HTTP response has no header terminator");
  const head = raw.subarray(0, separator).toString("ascii");
  const lines = head.split("\r\n");
  const statusMatch = /^HTTP\/1\.1 ([0-9]{3}) /.exec(lines[0] ?? "");
  if (statusMatch === null) throw new Error("raw HTTP response has no status line");
  const headers = new Map<string, string>();
  for (const line of lines.slice(1)) {
    const colon = line.indexOf(":");
    if (colon > 0) {
      headers.set(line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim());
    }
  }
  return {
    status: Number(statusMatch[1]),
    headers,
    body: raw.subarray(separator + 4),
  };
}

async function slowBodyExchange(
  api: RunningMockApi,
  head: Buffer,
  body: Buffer,
  intervalMs: number,
): Promise<RawResponse> {
  const raw = await new Promise<Buffer>((resolve, reject) => {
    const socket = connect(api.port, api.host);
    const chunks: Buffer[] = [];
    let bodyIndex = 0;
    const deadline = setTimeout(() => {
      socket.destroy();
      reject(new Error("slow HTTP exchange exceeded its deadline"));
    }, 2_000);
    deadline.unref();
    let sender: ReturnType<typeof setInterval> | undefined;
    const finish = () => {
      clearTimeout(deadline);
      if (sender !== undefined) clearInterval(sender);
    };
    socket.once("connect", () => {
      socket.write(head);
      sender = setInterval(() => {
        if (bodyIndex < body.byteLength) {
          socket.write(body.subarray(bodyIndex, bodyIndex + 1));
          bodyIndex += 1;
        } else {
          clearInterval(sender);
          socket.end();
        }
      }, intervalMs);
      sender.unref();
    });
    socket.on("data", (chunk: Buffer) => chunks.push(chunk));
    socket.once("error", (error) => {
      finish();
      reject(error);
    });
    socket.once("end", () => {
      finish();
      resolve(Buffer.concat(chunks));
    });
  });
  return parseRawResponse(raw);
}

function errorCode(response: RawResponse): string {
  return (JSON.parse(response.body.toString("utf8")) as {
    error: { code: string };
  }).error.code;
}

function jsonHeaders(body: Buffer | string): ReadonlyArray<readonly [string, string]> {
  const size = Buffer.isBuffer(body) ? body.byteLength : Buffer.byteLength(body);
  return [
    ["Content-Type", "application/json"],
    ["Content-Length", String(size)],
  ];
}

describe("mock-api security boundary", () => {
  let api: RunningMockApi;

  beforeEach(async () => {
    api = await startMockApi();
  });

  afterEach(async () => {
    await api.close();
  });

  test("accepts only its derived canonical Host authority", async () => {
    const accepted = await exchange(api, requestBytes(api));
    expect(accepted.status).toBe(200);
    expect(accepted.headers.has("access-control-allow-origin")).toBe(false);

    const hostileAuthorities = [
      undefined,
      `attacker.example:${api.port}`,
      `localhost:${api.port}`,
      `[::1]:${api.port}`,
      `${api.host}:${api.port + 1}`,
    ];
    for (const authority of hostileAuthorities) {
      const headers = authority === undefined ? [] : [["Host", authority] as const];
      const response = await exchange(
        api,
        requestBytes(api, {
          headers,
          includeHost: false,
        }),
      );
      expect(response.status, String(authority)).toBe(400);
      expect(errorCode(response), String(authority)).toBe("invalid_authority");
    }
  });

  test("rejects duplicate Host fields", async () => {
    const response = await exchange(
      api,
      requestBytes(api, {
        headers: [
          ["Host", `${api.host}:${api.port}`],
          ["Host", `attacker.example:${api.port}`],
        ],
        includeHost: false,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_authority");
  });

  test.each([
    "http://127.0.0.1/api/dashboard",
    "//attacker.example/api/dashboard",
    "/\\attacker.example/api/dashboard",
    "/api/dashboard\\suffix",
    "*",
  ])("rejects non-origin-form request target %s", async (target) => {
    const response = await exchange(api, requestBytes(api, { target }));

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_request_target");
  });

  test("accepts only its exact local Origin and emits no CORS grant", async () => {
    const accepted = await exchange(
      api,
      requestBytes(api, {
        headers: [["Origin", api.origin]],
      }),
    );
    expect(accepted.status).toBe(200);
    expect(accepted.headers.has("access-control-allow-origin")).toBe(false);

    for (const origin of [
      "null",
      `http://attacker.example:${api.port}`,
      `http://localhost:${api.port}`,
      `${api.origin}/`,
    ]) {
      const response = await exchange(
        api,
        requestBytes(api, { headers: [["Origin", origin]] }),
      );
      expect(response.status, origin).toBe(400);
      expect(errorCode(response), origin).toBe("invalid_origin");
    }
  });

  test("rejects duplicate Origin fields", async () => {
    const response = await exchange(
      api,
      requestBytes(api, {
        headers: [
          ["Origin", api.origin],
          ["Origin", `http://attacker.example:${api.port}`],
        ],
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_origin");
  });

  test.each([
    [
      "duplicate Content-Length",
      [
        ["Content-Length", "0"],
        ["Content-Length", "0"],
      ],
    ],
    [
      "Transfer-Encoding with Content-Length",
      [
        ["Transfer-Encoding", "chunked"],
        ["Content-Length", "0"],
      ],
    ],
    ["unsupported Transfer-Encoding", [["Transfer-Encoding", "gzip"]]],
  ] as const)("rejects invalid framing: %s", async (_label, headers) => {
    const response = await exchange(api, requestBytes(api, { headers }));

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_http_framing");
  });

  test.each(["01", "+1", "-1", "1.0", "18446744073709551616"])(
    "rejects invalid Content-Length %s",
    async (contentLength) => {
      const response = await exchange(
        api,
        requestBytes(api, { headers: [["Content-Length", contentLength]] }),
      );

      expect(response.status).toBe(400);
      expect(errorCode(response)).toBe("invalid_http_framing");
    },
  );

  test("allows an explicit zero Content-Length on a bodyless route", async () => {
    const response = await exchange(
      api,
      requestBytes(api, { headers: [["Content-Length", "0"]] }),
    );

    expect(response.status).toBe(200);
  });

  test("rejects unsupported content encoding before route dispatch", async () => {
    const body = '{"email":"pilot@example.com","password":"pilot-pass"}';
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: [...jsonHeaders(body), ["Content-Encoding", "gzip"]],
        body,
      }),
    );

    expect(response.status).toBe(415);
    expect(errorCode(response)).toBe("unsupported_content_encoding");
  });

  test.each([
    ["missing", []],
    ["non-JSON", [["Content-Type", "text/plain"]]],
    [
      "duplicate",
      [
        ["Content-Type", "application/json"],
        ["Content-Type", "application/json"],
      ],
    ],
  ] as const)("rejects %s content type on a body route", async (_label, contentTypes) => {
    const body = '{"email":"pilot@example.com","password":"pilot-pass"}';
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: [
          ...contentTypes,
          ["Content-Length", String(Buffer.byteLength(body))],
        ],
        body,
      }),
    );

    expect(response.status).toBe(415);
    expect(errorCode(response)).toBe("unsupported_media_type");
  });

  test("accepts a bounded chunked JSON body on a body route", async () => {
    const json = '{"email":"pilot@example.com","password":"pilot-pass"}';
    const chunkedBody = `${Buffer.byteLength(json).toString(16)}\r\n${json}\r\n0\r\n\r\n`;
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: [
          ["Content-Type", "application/json"],
          ["Transfer-Encoding", "chunked"],
        ],
        body: chunkedBody,
      }),
    );

    expect(response.status).toBe(200);
  });

  test.each([
    ["GET route", "GET", "/api/dashboard"],
    ["unknown route", "POST", "/api/unknown"],
    ["wrong method", "POST", "/api/dashboard"],
  ] as const)(
    "rejects an oversized declared body before dispatch for %s",
    async (_label, method, target) => {
      const response = await exchange(
        api,
        requestBytes(api, {
          method,
          target,
          headers: [["Content-Length", String(MAX_REQUEST_BODY_BYTES + 1)]],
        }),
      );

      expect(response.status).toBe(413);
      expect(errorCode(response)).toBe("request_body_too_large");
    },
  );

  test.each([
    ["GET route", "GET", "/api/dashboard"],
    ["unknown route", "POST", "/api/unknown"],
    ["wrong method", "POST", "/api/dashboard"],
  ] as const)("rejects any body on %s", async (_label, method, target) => {
    const body = "{}";
    const response = await exchange(
      api,
      requestBytes(api, {
        method,
        target,
        headers: [["Content-Length", String(Buffer.byteLength(body))]],
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("request_body_not_allowed");
  });

  test("rejects chunked bodies on routes that do not accept a body", async () => {
    const response = await exchange(
      api,
      requestBytes(api, {
        headers: [["Transfer-Encoding", "chunked"]],
        body: "2\r\n{}\r\n0\r\n\r\n",
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("request_body_not_allowed");
  });

  test.each([
    [
      "login escaped-equivalent key",
      "/api/login",
      '{"\\u0065mail":"attacker","email":"pilot@example.com","password":"pilot-pass"}',
    ],
    [
      "PATCH escaped-equivalent key",
      "/api/orders/ORD-1002",
      '{"status":"cancelled","\\u0073tatus":"shipped"}',
    ],
    [
      "nested escaped-equivalent key",
      "/api/login",
      '{"email":"pilot@example.com","password":"pilot-pass","meta":{"x":1,"\\u0078":2}}',
    ],
  ] as const)("rejects duplicate decoded JSON members: %s", async (_label, target, body) => {
    const response = await exchange(
      api,
      requestBytes(api, {
        method: target === "/api/login" ? "POST" : "PATCH",
        target,
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("duplicate_json_member");
  });

  test.each([
    ["overlong UTF-8", Buffer.from([0xc0, 0xaf])],
    ["UTF-8 surrogate", Buffer.from([0xed, 0xa0, 0x80])],
  ])("rejects %s before JSON parsing", async (_label, invalidBytes) => {
    const body = Buffer.concat([
      Buffer.from('{"email":"pilot@example.com","password":"', "ascii"),
      invalidBytes,
      Buffer.from('"}', "ascii"),
    ]);
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_utf8");
  });

  test("rejects a UTF-8 BOM outside the JSON grammar", async () => {
    const body = Buffer.concat([
      Buffer.from([0xef, 0xbb, 0xbf]),
      Buffer.from('{"email":"pilot@example.com","password":"pilot-pass"}'),
    ]);
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_json");
  });

  test("enforces a total deadline while a body continues streaming", async () => {
    await api.close();
    api = await startMockApi({ requestTimeoutMs: 50 });
    const body = Buffer.from('{"email":"pilot@example.com","password":"pilot-pass"}');
    const head = requestBytes(api, {
      method: "POST",
      target: "/api/login",
      headers: jsonHeaders(body),
    });

    const response = await slowBodyExchange(api, head, body, 20);

    expect(response.status).toBe(408);
    expect(errorCode(response)).toBe("request_timeout");
  });

  test("rejects an unpaired escaped JSON surrogate", async () => {
    const body = '{"notes":"\\uD800"}';
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "PATCH",
        target: "/api/orders/ORD-1002",
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("invalid_json");
  });

  test("bounds JSON nesting depth", async () => {
    const body =
      '{"email":"pilot@example.com","password":"pilot-pass","extra":' +
      "[".repeat(33) +
      "0" +
      "]".repeat(33) +
      "}";
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("json_too_deep");
  });

  test("bounds total JSON members", async () => {
    const entries = Array.from({ length: 1_025 }, (_, index) => `"k${index}":0`);
    const body =
      '{"email":"pilot@example.com","password":"pilot-pass","extra":{' +
      entries.join(",") +
      "}}";
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "POST",
        target: "/api/login",
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(400);
    expect(errorCode(response)).toBe("json_too_complex");
  });

  test("accepts valid non-ASCII UTF-8 JSON", async () => {
    const notes = "仓库三号门";
    const body = JSON.stringify({ notes });
    const response = await exchange(
      api,
      requestBytes(api, {
        method: "PATCH",
        target: "/api/orders/ORD-1002",
        headers: jsonHeaders(body),
        body,
      }),
    );

    expect(response.status).toBe(200);
    expect((JSON.parse(response.body.toString("utf8")) as { notes: string }).notes).toBe(notes);
  });
});
