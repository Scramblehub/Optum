# Diagnose sample: one shared table, nothing more

`report.py` and `signup.js` share the `customers` table and nothing else; each calls a different
API. `optum diagnose` must report one finding, "2 tools reach `sql:customers`", and must **not**
call them the same job ([expected_diagnosis.json](expected_diagnosis.json)).
