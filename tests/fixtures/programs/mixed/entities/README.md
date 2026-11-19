# Diagnose sample: the same data in several places

A customer defined four times (a Python dataclass, a SQL table, a JS class, an API payload in
camelCase) and an order three times, with decoys that share only generic fields or none.
[expected_diagnosis.json](expected_diagnosis.json) lists exactly what `optum diagnose` must report.
