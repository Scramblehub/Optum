// SQL through node-postgres: literal and template queries, a query held in a local, a schema prefix.
import { Pool } from "pg";

const pool = new Pool();

export async function addCustomer(email: string) {
  const res = await pool.query("INSERT INTO customers (email) VALUES ($1) RETURNING id", [email]);
  return res.rows[0].id;
}

export async function addOrder(customerId: number, total: number) {
  await pool.query(`INSERT INTO public.orders (customer_id, total) VALUES ($1, $2)`, [customerId, total]);
}

export async function revenueByCustomer() {
  const sql = `
    SELECT c.email, SUM(o.total) FROM orders o
    JOIN customers c ON c.id = o.customer_id GROUP BY c.email`;
  return (await pool.query(sql)).rows;
}
