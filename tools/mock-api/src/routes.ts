import type { IncomingMessage, ServerResponse } from "node:http";

import {
  DASHBOARD,
  type Order,
  type OrderStatus,
  type PilotFixtures,
} from "./fixtures.ts";

export const MAX_REQUEST_BODY_BYTES = 64 * 1024;
const MAX_PAGE_SIZE = 100;
const VALID_ORDER_STATUSES = new Set<OrderStatus>([
  "pending",
  "processing",
  "shipped",
  "delivered",
  "cancelled",
]);
const ORDER_UPDATE_FIELDS = new Set([
  "status",
  "priority",
  "shippingAddress",
  "notes",
]);

interface ApiErrorBody {
  error: { code: string; message: string };
}

class RouteError extends Error {
  readonly status: number;
  readonly code: string;

  constructor(
    status: number,
    code: string,
    message: string,
  ) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

function jsonBody(value: unknown): Buffer {
  return Buffer.from(JSON.stringify(value), "utf8");
}

function sendJson(
  response: ServerResponse,
  status: number,
  value: unknown,
  extraHeaders: Readonly<Record<string, string>> = {},
): void {
  if (response.destroyed || response.writableEnded) {
    return;
  }

  const body = jsonBody(value);
  response.writeHead(status, {
    "cache-control": "no-store",
    "content-length": String(body.byteLength),
    "content-type": "application/json; charset=utf-8",
    "x-content-type-options": "nosniff",
    ...extraHeaders,
  });
  response.end(body);
}

function sendError(
  response: ServerResponse,
  status: number,
  code: string,
  message: string,
  extraHeaders: Readonly<Record<string, string>> = {},
): void {
  const body: ApiErrorBody = { error: { code, message } };
  sendJson(response, status, body, extraHeaders);
}

function sendHtml(response: ServerResponse, html: string): void {
  const body = Buffer.from(html, "utf8");
  response.writeHead(200, {
    "cache-control": "no-store",
    "content-length": String(body.byteLength),
    "content-security-policy":
      "default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
    "content-type": "text/html; charset=utf-8",
    "referrer-policy": "no-referrer",
    "x-content-type-options": "nosniff",
  });
  response.end(body);
}

function assertMethod(
  request: IncomingMessage,
  response: ServerResponse,
  method: string,
): boolean {
  if (request.method === method) {
    return true;
  }

  sendError(
    response,
    405,
    "method_not_allowed",
    "The requested method is not allowed for this resource.",
    { allow: method },
  );
  return false;
}

function parsePositiveInteger(
  value: string | null,
  fallback: number,
  maximum: number,
): number {
  if (value === null) {
    return fallback;
  }
  if (!/^[1-9][0-9]*$/.test(value)) {
    throw new RouteError(
      400,
      "invalid_pagination",
      "Pagination parameters must be positive integers.",
    );
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed > maximum) {
    throw new RouteError(
      400,
      "invalid_pagination",
      "Pagination parameters are outside the allowed range.",
    );
  }
  return parsed;
}

function paginated<T>(items: readonly T[], page: number, pageSize: number) {
  const start = (page - 1) * pageSize;
  return {
    items: items.slice(start, start + pageSize),
    page,
    pageSize,
    total: items.length,
    totalPages: Math.ceil(items.length / pageSize),
  };
}

function normalizeSearch(value: string): string {
  return value.trim().toLocaleLowerCase("en-US");
}

async function readJsonRequest(request: IncomingMessage): Promise<unknown> {
  const contentType = request.headers["content-type"];
  if (contentType?.split(";", 1)[0]?.trim().toLowerCase() !== "application/json") {
    throw new RouteError(
      415,
      "unsupported_media_type",
      "The request body must use application/json.",
    );
  }

  const declaredLength = request.headers["content-length"];
  if (declaredLength !== undefined) {
    if (!/^(0|[1-9][0-9]*)$/.test(declaredLength)) {
      throw new RouteError(
        400,
        "invalid_content_length",
        "The Content-Length header is invalid.",
      );
    }
    const length = Number(declaredLength);
    if (!Number.isSafeInteger(length) || length > MAX_REQUEST_BODY_BYTES) {
      throw new RouteError(
        413,
        "request_body_too_large",
        "The request body exceeds the allowed size.",
      );
    }
  }

  const chunks: Buffer[] = [];
  let received = 0;
  for await (const value of request) {
    const chunk = Buffer.isBuffer(value) ? value : Buffer.from(value);
    received += chunk.byteLength;
    if (received > MAX_REQUEST_BODY_BYTES) {
      throw new RouteError(
        413,
        "request_body_too_large",
        "The request body exceeds the allowed size.",
      );
    }
    chunks.push(chunk);
  }

  try {
    return JSON.parse(Buffer.concat(chunks, received).toString("utf8")) as unknown;
  } catch {
    throw new RouteError(
      400,
      "invalid_json",
      "The request body is not valid JSON.",
    );
  }
}

function requireRecord(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new RouteError(
      400,
      "invalid_request",
      "The request body must be a JSON object.",
    );
  }
  return value as Record<string, unknown>;
}

function orderWithCustomer(fixtures: PilotFixtures, order: Order) {
  return {
    ...order,
    customer: fixtures.customers.find(({ id }) => id === order.customerId),
  };
}

function findOrder(fixtures: PilotFixtures, id: string): Order {
  const order = fixtures.orders.find((candidate) => candidate.id === id);
  if (order === undefined) {
    throw new RouteError(404, "order_not_found", "The requested order was not found.");
  }
  return order;
}

function findCustomer(fixtures: PilotFixtures, id: string) {
  const customer = fixtures.customers.find((candidate) => candidate.id === id);
  if (customer === undefined) {
    throw new RouteError(
      404,
      "customer_not_found",
      "The requested customer was not found.",
    );
  }
  return customer;
}

function validateOrderUpdate(body: Record<string, unknown>) {
  if (Object.keys(body).length === 0) {
    throw new RouteError(
      400,
      "invalid_order_update",
      "At least one supported order field is required.",
    );
  }
  for (const key of Object.keys(body)) {
    if (!ORDER_UPDATE_FIELDS.has(key)) {
      throw new RouteError(
        400,
        "invalid_order_update",
        "The order update contains an unsupported field.",
      );
    }
  }

  const update: Partial<Order> = {};
  if (body.status !== undefined) {
    if (typeof body.status !== "string" || !VALID_ORDER_STATUSES.has(body.status as OrderStatus)) {
      throw new RouteError(
        400,
        "invalid_order_status",
        "The order status is not supported.",
      );
    }
    update.status = body.status as OrderStatus;
  }
  if (body.priority !== undefined) {
    if (body.priority !== "normal" && body.priority !== "high") {
      throw new RouteError(
        400,
        "invalid_order_priority",
        "The order priority is not supported.",
      );
    }
    update.priority = body.priority;
  }
  for (const key of ["shippingAddress", "notes"] as const) {
    const value = body[key];
    if (value !== undefined) {
      if (typeof value !== "string" || value.length > 500) {
        throw new RouteError(
          400,
          "invalid_order_update",
          "Order text fields must contain at most 500 characters.",
        );
      }
      update[key] = value;
    }
  }
  return update;
}

async function waitForDelay(
  request: IncomingMessage,
  response: ServerResponse,
  delayMs: number,
): Promise<boolean> {
  return await new Promise<boolean>((resolve) => {
    let settled = false;
    const finish = (completed: boolean) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      request.off("aborted", aborted);
      response.off("close", aborted);
      resolve(completed);
    };
    const aborted = () => finish(false);
    const timer = setTimeout(() => finish(true), delayMs);
    timer.unref();
    request.once("aborted", aborted);
    response.once("close", aborted);
  });
}

export interface RouteHandlerOptions {
  fixtures: PilotFixtures;
  helpHtml: string;
}

export function createRouteHandler({ fixtures, helpHtml }: RouteHandlerOptions) {
  return async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
    try {
      if (request.url === undefined || request.method === undefined) {
        throw new RouteError(400, "invalid_request", "The HTTP request is incomplete.");
      }
      const url = new URL(request.url, "http://127.0.0.1");
      const path = url.pathname;

      if (path === "/api/login") {
        if (!assertMethod(request, response, "POST")) return;
        const body = requireRecord(await readJsonRequest(request));
        if (
          body.email !== "pilot@example.com" ||
          body.password !== "pilot-pass" ||
          Object.keys(body).some((key) => key !== "email" && key !== "password")
        ) {
          sendError(
            response,
            401,
            "invalid_credentials",
            "The email or password is incorrect.",
          );
          return;
        }
        sendJson(response, 200, {
          token: "pilot-session-token",
          user: {
            id: "usr-001",
            name: "Lin Chen",
            email: "pilot@example.com",
            role: "Operations Manager",
          },
        });
        return;
      }

      if (path === "/api/dashboard") {
        if (!assertMethod(request, response, "GET")) return;
        sendJson(response, 200, DASHBOARD);
        return;
      }

      if (path === "/api/orders") {
        if (!assertMethod(request, response, "GET")) return;
        const page = parsePositiveInteger(url.searchParams.get("page"), 1, 1_000_000);
        const pageSize = parsePositiveInteger(url.searchParams.get("pageSize"), 20, MAX_PAGE_SIZE);
        const status = url.searchParams.get("status");
        if (status !== null && status !== "all" && !VALID_ORDER_STATUSES.has(status as OrderStatus)) {
          throw new RouteError(
            400,
            "invalid_order_status",
            "The order status is not supported.",
          );
        }
        const query = normalizeSearch(url.searchParams.get("query") ?? "");
        const filtered = fixtures.orders.filter((order) => {
          const statusMatches = status === null || status === "all" || order.status === status;
          const queryMatches =
            query.length === 0 ||
            normalizeSearch(`${order.id} ${order.customerName}`).includes(query);
          return statusMatches && queryMatches;
        });
        sendJson(response, 200, paginated(filtered, page, pageSize));
        return;
      }

      const orderMatch = /^\/api\/orders\/(ORD-[0-9]{4})$/.exec(path);
      if (orderMatch !== null) {
        const order = findOrder(fixtures, orderMatch[1]!);
        if (request.method === "GET") {
          sendJson(response, 200, orderWithCustomer(fixtures, order));
          return;
        }
        if (request.method === "PATCH") {
          const update = validateOrderUpdate(requireRecord(await readJsonRequest(request)));
          Object.assign(order, update, { updatedAt: "2026-08-18T12:00:00.000Z" });
          sendJson(response, 200, orderWithCustomer(fixtures, order));
          return;
        }
        sendError(
          response,
          405,
          "method_not_allowed",
          "The requested method is not allowed for this resource.",
          { allow: "GET, PATCH" },
        );
        return;
      }

      if (path === "/api/customers") {
        if (!assertMethod(request, response, "GET")) return;
        const page = parsePositiveInteger(url.searchParams.get("page"), 1, 1_000_000);
        const pageSize = parsePositiveInteger(url.searchParams.get("pageSize"), 20, MAX_PAGE_SIZE);
        const query = normalizeSearch(url.searchParams.get("query") ?? "");
        const filtered = fixtures.customers.filter(
          (customer) =>
            query.length === 0 ||
            normalizeSearch(`${customer.id} ${customer.name} ${customer.email}`).includes(query),
        );
        sendJson(response, 200, paginated(filtered, page, pageSize));
        return;
      }

      const customerMatch = /^\/api\/customers\/(CUS-[0-9]{3})$/.exec(path);
      if (customerMatch !== null) {
        if (!assertMethod(request, response, "GET")) return;
        const customer = findCustomer(fixtures, customerMatch[1]!);
        sendJson(response, 200, {
          ...customer,
          orders: fixtures.orders.filter(({ customerId }) => customerId === customer.id),
        });
        return;
      }

      if (path === "/api/errors/500") {
        if (!assertMethod(request, response, "GET")) return;
        sendError(
          response,
          500,
          "fixture_internal_error",
          "The deliberate error fixture failed.",
        );
        return;
      }

      if (path === "/api/timeout") {
        if (!assertMethod(request, response, "GET")) return;
        const rawDelay = url.searchParams.get("delayMs") ?? "250";
        if (!/^[0-9]{1,4}$/.test(rawDelay) || Number(rawDelay) > 1000) {
          throw new RouteError(
            400,
            "invalid_delay",
            "The timeout fixture delay must be between 0 and 1000 milliseconds.",
          );
        }
        if (await waitForDelay(request, response, Number(rawDelay))) {
          sendJson(response, 200, { delayedMs: Number(rawDelay) });
        }
        return;
      }

      if (path === "/help" || path === "/help/" || path === "/help/index.html") {
        if (!assertMethod(request, response, "GET")) return;
        sendHtml(response, helpHtml);
        return;
      }

      sendError(response, 404, "not_found", "The requested resource was not found.");
    } catch (error) {
      if (error instanceof RouteError) {
        const closeConnection = error.status === 413;
        sendError(response, error.status, error.code, error.message, closeConnection ? { connection: "close" } : {});
        if (closeConnection) {
          request.resume();
        }
        return;
      }
      sendError(
        response,
        500,
        "internal_error",
        "The mock API could not process the request.",
      );
    }
  };
}
