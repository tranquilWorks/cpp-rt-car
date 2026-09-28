# M25-02 additive SDK inventory revalidation

Baseline: M25-01 main d3052a0f7f17a0cd68334123aea3869fcae2094a.
Canonical M25-02 control PR524 at ad159304c00151e248ffe2b7f5699c341a8b5361.

The only new product-source inventory member is optional `rt/include/rt/sdk.hpp`.
No prior rt/core implementation or header changed. The exact default package
inventory gains that header and an independent header-compilation target; all
previous entries, isolation assertions and consumers remain present.

Fresh command:

```text
python3 tests/cuda_physics/verify_package.py --work-directory build/m25-02-cuda-package
```

Exit0: relocated installed CUDA source kit11/11 and source-embedded kit11/11
passed; four generated kernel/Graph benchmark artifacts validated. Clean SDK
installation, custom include/data layout, source kit archive and source removal
checks remained enabled. Log: `/tmp/cpp-m25-02-cuda-package.log`.
The typed package suite also exercises the full raw C/C++ SDK and independently
compiled headers; its exact final command/results are in
[M25-02 evidence](M25-02-2026-09-28.md).

The matrix retains every old evidence digest, previous product-source digest,
row, required gate, threshold, critical flag and M23 crosswalk. Its package gate
retains historical source/evidence context and binds this fresh revalidation
alongside the reviewed additive package-contract source revision. No historical
evidence file was rewritten. The checker and all bound production sources stay
unchanged. Coverage remains40/44 (90.91%) overall,37/37 critical; all four
physical M18 rows remain NOT RUN. This is portable package evidence, with no
new hardware, performance or RT qualification.
