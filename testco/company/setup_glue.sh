#!/bin/sh
# Imports the company's n8n flows and switches them on (idempotent). Run from the repo root after
# `docker compose up` and `load.py` (the flow needs accounting's VAT tax).
set -e
cd "$(dirname "$0")/.."
VAT=$(python3 -c "
import sys; sys.path.insert(0, '.')
from load import Odoo
o = Odoo(); print(o('account.tax', 'search', [['name', '=', 'VAT 20% (incl.)'], ['type_tax_use', '=', 'sale']])[0])")
for wf in company/n8n/*.json; do
  name=$(basename "$wf")
  # n8n imports a list of workflows; each file holds one.
  sed "s/__VAT_TAX_ID__/$VAT/g" "$wf" | python3 -c "import json, sys; print(json.dumps([json.load(sys.stdin)]))" \
    > "recordings/.import-$name"
  docker compose cp "recordings/.import-$name" "automation:/tmp/$name" >/dev/null
  rm -f "recordings/.import-$name"
  out=$(docker compose exec -T automation n8n import:workflow --input="/tmp/$name" 2>&1 | grep -v "Permissions 0644\|User settings")
  case "$out" in *rror*) echo "$out"; exit 1 ;; esac
  echo "imported $name"
done
for id in orders-to-accounting abandoned-cart-reminder; do
  docker compose exec -T automation n8n update:workflow --id="$id" --active=true 2>&1 | grep -v "Permissions 0644\|User settings" | tail -1
done
docker compose restart automation >/dev/null && docker compose up -d --wait automation >/dev/null
echo "glue: n8n flows imported and active (VAT tax id $VAT)"
