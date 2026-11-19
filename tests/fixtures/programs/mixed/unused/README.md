# Diagnose sample: unused code

Planted unused definitions in Python, JS and C (marked `PLANTED` in the code), one function only
a test uses, and decoys that look unused but are reached by name, as values or from outside
(marked `decoy`). [expected_diagnosis.json](expected_diagnosis.json) lists exactly what `optum
diagnose` must report.
