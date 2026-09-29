# Determinism check ordering correction

At checkpoint8f6baba, all37 local feature tests pass together; actual CPack
passes standalone9/full64/embedding9 and negatives. Hosted GCC/FMA-off
job109653869054 failed the new XDMA parity prepare at startup safe acknowledgement.
The retained log shows prior CPU/CUDA checks and compilation running concurrently
with the new matrix through parallel Make dependencies. Prior CUDA completed.

The new golden_xdma_determinism_check now depends on both existing CPU/CUDA
checks. Every prior check, new scenario, assertion and timeout remains unchanged.
This serializes the newly added work without editing protected sources or CI.
Shared-host8ms scheduling failures remain recorded; no RT timing qualification
or unconditional hosted safe-output reliability is claimed. Full fresh checks
and exact-final-head all32 remain required before integration.
