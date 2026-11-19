/* Glue in C: pull orders from a shop API, push deals to a CRM, log to sqlite. The same job as
   the Python and JS http_glue samples, so the API and table nodes have the same ids. */
#include <curl/curl.h>
#include <sqlite3.h>
#include <stdio.h>

#define SHOP_ORDERS "https://shop.example.com/api/orders"

static const char *CRM_DEALS = "https://crm.example.com/api/deals";

static int fetch_orders(CURL *curl) {
  curl_easy_setopt(curl, CURLOPT_URL, SHOP_ORDERS "?since=today");
  return (int)curl_easy_perform(curl);
}

static int push_deal(CURL *curl, const char *json) {
  curl_easy_setopt(curl, CURLOPT_URL, CRM_DEALS);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
  return (int)curl_easy_perform(curl);
}

static int close_deal(CURL *curl, const char *deal_id) {
  char url[256];
  snprintf(url, sizeof url, "https://crm.example.com/api/deals/%s", deal_id);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
  curl_easy_setopt(curl, CURLOPT_URL, url);
  return (int)curl_easy_perform(curl);
}

static int log_order(sqlite3 *db) {
  return sqlite3_exec(db, "INSERT INTO orders (total) VALUES (1)", NULL, NULL, NULL);
}

int sync_once(sqlite3 *db) {
  CURL *curl = curl_easy_init();
  int failed = fetch_orders(curl) || push_deal(curl, "{\"title\":\"Order 1\"}") || close_deal(curl, "1");
  curl_easy_cleanup(curl);
  return failed || log_order(db);
}
