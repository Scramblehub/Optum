# Diagnose sample: one package, no waste

`inventory/writer.py` and `inventory/reader.py` share the `items` table, as do the tests. They are
one component (the `inventory` package), and test code is left out of findings, so `optum
diagnose` must report nothing ([expected_diagnosis.json](expected_diagnosis.json)).
