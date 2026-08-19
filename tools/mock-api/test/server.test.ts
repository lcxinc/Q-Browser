import { afterEach, beforeEach, describe, expect, test } from "vitest";
import { connect } from "node:net";

import {
  MAX_REQUEST_BODY_BYTES,
  type RunningMockApi,
  startMockApi,
} from "../src/server.ts";

interface ErrorEnvelope {
  error: { code: string; message: string };
}

async function readJson<T>(response: Response): Promise<T> {
  expect(response.headers.get("content-type")).toBe(
    "application/json; charset=utf-8",
  );
  return (await response.json()) as T;
}

describe("mock-api", () => {
  let api: RunningMockApi;

  beforeEach(async () => {
    api = await startMockApi();
  });

  afterEach(async () => {
    await api.close();
  });

  test("binds only the IPv4 loopback interface on an ephemeral port", () => {
    expect(api.host).toBe("127.0.0.1");
    expect(api.port).toBeGreaterThan(0);
    expect(api.origin).toBe(`http://127.0.0.1:${api.port}`);
  });

  test("authenticates the deterministic pilot user", async () => {
    const response = await fetch(`${api.origin}/api/login`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        email: "pilot@example.com",
        password: "pilot-pass",
      }),
    });

    expect(response.status).toBe(200);
    expect(await readJson(response)).toEqual({
      token: "pilot-session-token",
      user: {
        id: "usr-001",
        name: "Lin Chen",
        email: "pilot@example.com",
        role: "Operations Manager",
      },
    });
  });

  test("returns a stable error for invalid credentials", async () => {
    const response = await fetch(`${api.origin}/api/login`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        email: "pilot@example.com",
        password: "wrong",
      }),
    });

    expect(response.status).toBe(401);
    expect(await readJson<ErrorEnvelope>(response)).toEqual({
      error: {
        code: "invalid_credentials",
        message: "The email or password is incorrect.",
      },
    });
  });

  test("returns deterministic dashboard KPIs, activity, and chart data", async () => {
    const response = await fetch(`${api.origin}/api/dashboard`);

    expect(response.status).toBe(200);
    expect(await readJson(response)).toEqual({
      kpis: {
        orderCount: 6,
        pendingCount: 2,
        customerCount: 4,
        revenueCents: 477725,
      },
      revenueByMonth: [
        { month: "2026-05", amountCents: 108400 },
        { month: "2026-06", amountCents: 132050 },
        { month: "2026-07", amountCents: 117275 },
        { month: "2026-08", amountCents: 120000 },
      ],
      recentActivity: [
        {
          id: "act-001",
          at: "2026-08-18T09:30:00.000Z",
          text: "Order ORD-1006 moved to pending",
        },
        {
          id: "act-002",
          at: "2026-08-18T08:15:00.000Z",
          text: "Customer Acme North was updated",
        },
      ],
    });
  });

  test("lists orders with stable pagination", async () => {
    const response = await fetch(
      `${api.origin}/api/orders?page=2&pageSize=2`,
    );

    expect(response.status).toBe(200);
    const body = await readJson<{
      items: Array<{ id: string }>;
      page: number;
      pageSize: number;
      total: number;
      totalPages: number;
      query: string;
      status: string;
    }>(response);
    expect(body).toMatchObject({
      page: 2,
      pageSize: 2,
      total: 6,
      totalPages: 3,
      query: "",
      status: "all",
    });
    expect(body.items.map(({ id }) => id)).toEqual(["ORD-1003", "ORD-1004"]);
  });

  test("filters orders by status and case-insensitive search", async () => {
    const response = await fetch(
      `${api.origin}/api/orders?status=pending&query=acme&page=1&pageSize=10`,
    );

    expect(response.status).toBe(200);
    const body = await readJson<{
      items: Array<{ id: string; status: string }>;
      total: number;
      query: string;
      status: string;
    }>(response);
    expect(body.total).toBe(2);
    expect(body).toMatchObject({ query: "acme", status: "pending" });
    expect(body.items).toEqual([
      expect.objectContaining({ id: "ORD-1001", status: "pending" }),
      expect.objectContaining({ id: "ORD-1006", status: "pending" }),
    ]);
  });

  test("returns canonical empty order pagination metadata", async () => {
    const response = await fetch(
      `${api.origin}/api/orders?status=all&query=missing&page=1&pageSize=20`,
    );

    expect(response.status).toBe(200);
    expect(await readJson(response)).toEqual({
      items: [],
      page: 1,
      pageSize: 20,
      total: 0,
      totalPages: 0,
      query: "missing",
      status: "all",
    });
  });

  test("returns an order detail with its customer", async () => {
    const response = await fetch(`${api.origin}/api/orders/ORD-1002`);

    expect(response.status).toBe(200);
    expect(await readJson(response)).toMatchObject({
      id: "ORD-1002",
      customerId: "CUS-002",
      status: "processing",
      customer: { id: "CUS-002", name: "Globex Retail" },
    });
  });

  test("updates only supported order fields and persists the update", async () => {
    const update = await fetch(`${api.origin}/api/orders/ORD-1002`, {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ status: "shipped", notes: "Dock 3" }),
    });

    expect(update.status).toBe(200);
    expect(await readJson(update)).toMatchObject({
      id: "ORD-1002",
      status: "shipped",
      notes: "Dock 3",
      updatedAt: "2026-08-18T12:00:00.000Z",
    });

    const detail = await fetch(`${api.origin}/api/orders/ORD-1002`);
    expect(await readJson(detail)).toMatchObject({
      id: "ORD-1002",
      status: "shipped",
      notes: "Dock 3",
    });
  });

  test("lists customers with stable pagination", async () => {
    const response = await fetch(
      `${api.origin}/api/customers?page=2&pageSize=2`,
    );

    expect(response.status).toBe(200);
    const body = await readJson<{
      items: Array<{ id: string }>;
      page: number;
      pageSize: number;
      total: number;
      totalPages: number;
      query: string;
    }>(response);
    expect(body).toMatchObject({ page: 2, pageSize: 2, total: 4, totalPages: 2,
      query: "" });
    expect(body.items.map(({ id }) => id)).toEqual(["CUS-003", "CUS-004"]);
  });

  test("returns a customer detail with related orders", async () => {
    const response = await fetch(`${api.origin}/api/customers/CUS-001`);

    expect(response.status).toBe(200);
    const body = await readJson<{
      id: string;
      name: string;
      orders: Array<{ id: string }>;
    }>(response);
    expect(body).toMatchObject({ id: "CUS-001", name: "Acme North" });
    expect(body.orders.map(({ id }) => id)).toEqual(["ORD-1001", "ORD-1006"]);
  });

  test("exposes a deliberate stable server error", async () => {
    const response = await fetch(`${api.origin}/api/errors/500`);

    expect(response.status).toBe(500);
    expect(await readJson<ErrorEnvelope>(response)).toEqual({
      error: {
        code: "fixture_internal_error",
        message: "The deliberate error fixture failed.",
      },
    });
  });

  test("supports an abortable timeout fixture without keeping shutdown open", async () => {
    await expect(
      fetch(`${api.origin}/api/timeout?delayMs=250`, {
        signal: AbortSignal.timeout(25),
      }),
    ).rejects.toThrow();
  });

  test("serves the local help document with restrictive browser headers", async () => {
    const response = await fetch(`${api.origin}/help/`);

    expect(response.status).toBe(200);
    expect(response.headers.get("content-type")).toBe("text/html; charset=utf-8");
    expect(response.headers.get("content-security-policy")).toContain(
      "default-src 'none'",
    );
    expect(response.headers.get("x-content-type-options")).toBe("nosniff");
    const html = await response.text();
    expect(html).toContain("<title>Q-Browser Pilot Help</title>");
    expect(html).toContain("Q-Browser Pilot Help");
    expect(html).not.toMatch(/<script\b/i);
  });

  test("rejects oversized request bodies with a stable error", async () => {
    const response = await fetch(`${api.origin}/api/login`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ value: "x".repeat(MAX_REQUEST_BODY_BYTES) }),
    });

    expect(response.status).toBe(413);
    expect(await readJson<ErrorEnvelope>(response)).toEqual({
      error: {
        code: "request_body_too_large",
        message: "The request body exceeds the allowed size.",
      },
    });
  });

  test("bounds chunked request bodies without resetting the client", async () => {
    const stream = new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(new Uint8Array(MAX_REQUEST_BODY_BYTES));
        controller.enqueue(new Uint8Array([1]));
        controller.close();
      },
    });
    const response = await fetch(`${api.origin}/api/login`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: stream,
      duplex: "half",
    } as RequestInit & { duplex: "half" });

    expect(response.status).toBe(413);
    expect(await readJson<ErrorEnvelope>(response)).toEqual({
      error: {
        code: "request_body_too_large",
        message: "The request body exceeds the allowed size.",
      },
    });
  });

  test("bounds HTTP headers and returns a stable parse error", async () => {
    const response = await fetch(`${api.origin}/api/dashboard`, {
      headers: { "x-oversized-fixture": "x".repeat(9_000) },
    });

    expect(response.status).toBe(431);
    expect(await readJson<ErrorEnvelope>(response)).toEqual({
      error: {
        code: "invalid_http_request",
        message: "The HTTP request is malformed or exceeds the header limit.",
      },
    });
  });

  test("bounds incomplete request time and closes with a stable 408", async () => {
    await api.close();
    api = await startMockApi({ requestTimeoutMs: 50 });

    const rawResponse = await new Promise<string>((resolve, reject) => {
      const socket = connect(api.port, api.host);
      let received = "";
      const deadline = setTimeout(() => {
        socket.destroy();
        reject(new Error("server did not enforce the request deadline"));
      }, 1_000);
      deadline.unref();
      socket.setEncoding("utf8");
      socket.once("connect", () => {
        socket.write(
          "POST /api/login HTTP/1.1\r\n" +
            `Host: ${api.host}:${api.port}\r\n` +
            "Content-Type: application/json\r\n" +
            "Content-Length: 100\r\n\r\n" +
            "{",
        );
      });
      socket.on("data", (chunk: string) => {
        received += chunk;
      });
      socket.once("end", () => {
        clearTimeout(deadline);
        resolve(received);
      });
      socket.once("error", (error) => {
        clearTimeout(deadline);
        reject(error);
      });
    });

    expect(rawResponse).toContain("HTTP/1.1 408 Request Timeout");
    expect(rawResponse).toContain('"code":"request_timeout"');
  });

  test("uses stable validation errors for malformed JSON", async () => {
    const response = await fetch(`${api.origin}/api/login`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: "{",
    });

    expect(response.status).toBe(400);
    expect(await readJson<ErrorEnvelope>(response)).toEqual({
      error: {
        code: "invalid_json",
        message: "The request body is not valid JSON.",
      },
    });
  });

  test("serves concurrent read requests without sharing partial state", async () => {
    const responses = await Promise.all(
      Array.from({ length: 24 }, (_, index) =>
        fetch(
          index % 2 === 0
            ? `${api.origin}/api/dashboard`
            : `${api.origin}/api/orders?page=1&pageSize=2`,
        ),
      ),
    );

    expect(responses.every(({ status }) => status === 200)).toBe(true);
    const bodies = await Promise.all(responses.map((response) => response.json()));
    expect(bodies.filter((_, index) => index % 2 === 0)).toEqual(
      Array.from({ length: 12 }, () => expect.objectContaining({
        kpis: expect.objectContaining({ orderCount: 6 }),
      })),
    );
    expect(bodies.filter((_, index) => index % 2 === 1)).toEqual(
      Array.from({ length: 12 }, () => expect.objectContaining({
        total: 6,
        items: expect.any(Array),
      })),
    );
  });
});
