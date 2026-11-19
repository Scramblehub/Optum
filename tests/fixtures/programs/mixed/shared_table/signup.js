// Diagnose sample: a signup handler. Shares only the customers table with report.py.
const { Pool } = require("pg");

const pool = new Pool();

async function signup(email) {
  await pool.query("INSERT INTO customers (email) VALUES ($1)", [email]);
  return fetch("https://crm.example.com/api/contacts", { method: "POST", body: JSON.stringify({ email }) });
}

module.exports = { signup };
