# Mixed sample: one job done twice

`sync.py` (a Python script) and `order_to_crm.json` (an n8n workflow) both pull orders from the
shop API and push deals to the CRM. Each frontend sees only its own file, but because they name
APIs the same way, the merged graph has one node per API with both tools pointing at it. That
overlap is what Optum's later phases look for: two pieces of glue doing one job.
