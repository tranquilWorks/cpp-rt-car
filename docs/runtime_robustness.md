# Fixed Runtime regression cases

`runtime_robustness` exercises only the supported public C++ SDK. Its two
fixed operation sequences check independent Runtime owners, callback counts,
checkpoint round trips, compatible cross-owner restore, refused corrupt
checkpoint restore without application mutation, stopped-step refusal and
checked repeated stop. A separate fixed mixed-rate graph writes an actual
checkpoint and two-frame active replay artifact, checks its metadata, and
rejects an altered magic or truncated artifact without changing live state.
The typed-payload case independently checks little-endian bytes and round-trip
value, then preserves the output sentinel on digest, extent, magic and kind
mismatches. Tests report ordinary nonzero exits on assertion failures.

Run the in-tree test:

```sh
cmake -S . -B build/agent-quick
cmake --build build/agent-quick --target rtfw_runtime_robustness --parallel 2
ctest --test-dir build/agent-quick -R '^runtime_robustness$' --output-on-failure
```

The same test sources support an independent installed consumer through
`find_package(rtfw CONFIG REQUIRED)`. The package verifier builds actual default
and optional CPack archives, removes original install prefixes, relocates to
paths with spaces, copies the test sources outside the checkout, verifies no
private compile paths, and runs all 71 preserved optional SDK consumers:

```sh
python3 tests/runtime_robustness/verify_package.py --work-directory build/regression-package
```

The record directory must be new. Temporary relocated SDKs and logs remain
available for inspection. These fixed examples are regression evidence, not
exhaustive lifecycle or parser coverage.

The owner-scoped M27-05 amendment is canonical control PR600. Earlier campaign,
seeded-defect and additional-workflow requirements were withdrawn from this
batch and remain unperformed; no continuous-coverage acceptance is promoted.
Production sources, installed SDK inventory, ABI, defaults and predecessor
assertions are unchanged. M27-06 build/provenance verification is selected next;
M27-07/08 and independent qualification remain separate.
