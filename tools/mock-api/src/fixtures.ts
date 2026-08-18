export type OrderStatus =
  | "pending"
  | "processing"
  | "shipped"
  | "delivered"
  | "cancelled";

export interface Order {
  id: string;
  customerId: string;
  customerName: string;
  createdAt: string;
  updatedAt: string;
  status: OrderStatus;
  totalCents: number;
  currency: "USD";
  priority: "normal" | "high";
  shippingAddress: string;
  notes: string;
}

export interface Customer {
  id: string;
  name: string;
  email: string;
  phone: string;
  city: string;
  tier: "standard" | "premium";
  since: string;
}

export interface PilotFixtures {
  orders: Order[];
  customers: Customer[];
}

const INITIAL_ORDERS: readonly Order[] = [
  {
    id: "ORD-1001",
    customerId: "CUS-001",
    customerName: "Acme North",
    createdAt: "2026-08-18T09:30:00.000Z",
    updatedAt: "2026-08-18T09:30:00.000Z",
    status: "pending",
    totalCents: 125000,
    currency: "USD",
    priority: "high",
    shippingAddress: "120 Market Street, Seattle, WA",
    notes: "Confirm loading dock before dispatch.",
  },
  {
    id: "ORD-1002",
    customerId: "CUS-002",
    customerName: "Globex Retail",
    createdAt: "2026-08-17T15:45:00.000Z",
    updatedAt: "2026-08-17T15:45:00.000Z",
    status: "processing",
    totalCents: 78250,
    currency: "USD",
    priority: "normal",
    shippingAddress: "44 King Road, Austin, TX",
    notes: "",
  },
  {
    id: "ORD-1003",
    customerId: "CUS-003",
    customerName: "Initech Labs",
    createdAt: "2026-08-16T11:20:00.000Z",
    updatedAt: "2026-08-16T11:20:00.000Z",
    status: "shipped",
    totalCents: 45100,
    currency: "USD",
    priority: "normal",
    shippingAddress: "9 Innovation Way, Boston, MA",
    notes: "Tracking sent to customer.",
  },
  {
    id: "ORD-1004",
    customerId: "CUS-004",
    customerName: "Umbrella Health",
    createdAt: "2026-08-15T10:00:00.000Z",
    updatedAt: "2026-08-15T10:00:00.000Z",
    status: "delivered",
    totalCents: 96500,
    currency: "USD",
    priority: "high",
    shippingAddress: "88 Lake Avenue, Chicago, IL",
    notes: "Signature required.",
  },
  {
    id: "ORD-1005",
    customerId: "CUS-002",
    customerName: "Globex Retail",
    createdAt: "2026-08-14T08:40:00.000Z",
    updatedAt: "2026-08-14T08:40:00.000Z",
    status: "cancelled",
    totalCents: 32875,
    currency: "USD",
    priority: "normal",
    shippingAddress: "44 King Road, Austin, TX",
    notes: "Cancelled by customer.",
  },
  {
    id: "ORD-1006",
    customerId: "CUS-001",
    customerName: "Acme North",
    createdAt: "2026-08-13T16:10:00.000Z",
    updatedAt: "2026-08-18T09:30:00.000Z",
    status: "pending",
    totalCents: 100000,
    currency: "USD",
    priority: "normal",
    shippingAddress: "120 Market Street, Seattle, WA",
    notes: "Awaiting inventory allocation.",
  },
];

const INITIAL_CUSTOMERS: readonly Customer[] = [
  {
    id: "CUS-001",
    name: "Acme North",
    email: "operations@acme.example",
    phone: "+1 206 555 0101",
    city: "Seattle",
    tier: "premium",
    since: "2022-03-12",
  },
  {
    id: "CUS-002",
    name: "Globex Retail",
    email: "orders@globex.example",
    phone: "+1 512 555 0102",
    city: "Austin",
    tier: "standard",
    since: "2023-07-08",
  },
  {
    id: "CUS-003",
    name: "Initech Labs",
    email: "lab@initech.example",
    phone: "+1 617 555 0103",
    city: "Boston",
    tier: "premium",
    since: "2021-11-19",
  },
  {
    id: "CUS-004",
    name: "Umbrella Health",
    email: "supply@umbrella.example",
    phone: "+1 312 555 0104",
    city: "Chicago",
    tier: "standard",
    since: "2024-02-02",
  },
];

export const DASHBOARD = Object.freeze({
  kpis: Object.freeze({
    orderCount: 6,
    pendingCount: 2,
    customerCount: 4,
    revenueCents: 477725,
  }),
  revenueByMonth: Object.freeze([
    Object.freeze({ month: "2026-05", amountCents: 108400 }),
    Object.freeze({ month: "2026-06", amountCents: 132050 }),
    Object.freeze({ month: "2026-07", amountCents: 117275 }),
    Object.freeze({ month: "2026-08", amountCents: 120000 }),
  ]),
  recentActivity: Object.freeze([
    Object.freeze({
      id: "act-001",
      at: "2026-08-18T09:30:00.000Z",
      text: "Order ORD-1006 moved to pending",
    }),
    Object.freeze({
      id: "act-002",
      at: "2026-08-18T08:15:00.000Z",
      text: "Customer Acme North was updated",
    }),
  ]),
});

export function createPilotFixtures(): PilotFixtures {
  return {
    orders: INITIAL_ORDERS.map((order) => ({ ...order })),
    customers: INITIAL_CUSTOMERS.map((customer) => ({ ...customer })),
  };
}
