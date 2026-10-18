# Historical initial-delivery measurement

This is the original 108-observation Clang run at source revision `2100021`.
It is preserved for comparison with the later engineering-audit run in
`results/audit-native-clang23`.

Raw CSV, raw JSONL, numeric observations and summary values were preserved.
During release preparation, personal absolute paths in JSON metadata were
replaced by `<repo>` and `<home>` placeholders, with an explicit redaction
annotation. The same treatment was applied to the old verification logs.
`results/audit-verification/historical-preservation.json` records a digest of the
unchanged observations/summary. Existing Git history was not rewritten.

Schema 1 called incoming request throughput `events_per_second`; schema 2 uses
the clearer `requests_per_second`. Emitted engine event counts are separate.
