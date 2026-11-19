#!/bin/sh
# One-off setup of the shop stand-in (WooCommerce on WordPress). Idempotent: run again, it only
# checks. A test credential, fixed and documented (testco/README.md): this shop only listens on
# 127.0.0.1, like Odoo's admin/admin.
set -e
cd /var/www/html
until wp db check >/dev/null 2>&1; do sleep 2; done
if ! wp core is-installed 2>/dev/null; then
  wp core install --url=http://127.0.0.1:8080 --title="Testco Shop" --admin_user=admin --admin_password=admin \
    --admin_email=admin@example.com --skip-email
fi
wp plugin is-installed woocommerce || wp plugin install woocommerce --version=9.4.3
wp plugin is-active woocommerce || wp plugin activate woocommerce
wp rewrite structure '/%postname%/' --hard >/dev/null
# A UK shop: GBP, prices including VAT (taxes not calculated by the shop: accounting does that).
wp option update woocommerce_currency GBP >/dev/null
wp option update woocommerce_default_country GB >/dev/null
wp option update woocommerce_prices_include_tax yes >/dev/null
wp option update woocommerce_calc_taxes no >/dev/null
wp option update woocommerce_coming_soon no >/dev/null
# The API credential the company's glue uses: an application password for admin, fixed for tests.
wp eval '
$uid = 1; $name = "testco-glue"; $pw = "testcoShopApiPassword00";
foreach (WP_Application_Passwords::get_user_application_passwords($uid) as $p) { if ($p["name"] === $name) { echo "api credential: present\n"; return; } }
WP_Application_Passwords::create_new_application_password($uid, array("name" => $name));
$all = WP_Application_Passwords::get_user_application_passwords($uid);
foreach ($all as $i => $p) { if ($p["name"] === $name) { $all[$i]["password"] = wp_hash_password($pw); } }
update_user_meta($uid, WP_Application_Passwords::USERMETA_KEY_APPLICATION_PASSWORDS, $all);
echo "api credential: created\n";'
echo "shop: ready"
