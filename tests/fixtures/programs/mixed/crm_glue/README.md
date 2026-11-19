# Mixed sample: one job done twice

`sync.py` (Python), `sync.js` (Node), `sync.c` (C with libcurl and sqlite) and `order_to_crm.json`
(an n8n workflow) all pull orders from the shop API and push deals to the CRM; the C program and
the workflow also both write the `orders` table. Each frontend sees only its own file, but because they name
APIs the same way, the merged graph has one node per API with both tools pointing at it. That
overlap is what Optum's later phases look for: four pieces of glue doing one job.
