# M26-04 work in progress

This is a preserved implementation checkpoint, not an installed or validated kit.
The first graph cannot finalize: the frozen scenario requires four ring slots,
while the current Runtime compiles exactly two. Do not reduce the declared
four slots or claim execution from this checkpoint. Runtime and all sibling
kits remain unchanged. The retained comparison uses two slots only in an
isolated diagnostic copy, never as golden scenario evidence.

`session.hpp` derives its configuration/control/lifecycle structure from the
unchanged `../golden_system/session.hpp`. `io.hpp` derives the complete HAL core
forwarding pattern from `../golden_cuda/physics.hpp`. Both use the unchanged
public Runtime SDK. The new card model accepts only native XDMA worker transfers,
control writes and user events. The codec distinguishes the two 120-byte header
formats explicitly; neither Runtime artifact identities nor sibling sources
are rewritten.

See `docs/evidence/M26-04-repair-proposal.md` in the repository for the bounded
repair proposal and acceptance that remains unverified. This checkpoint has no
physical, replay, safe-output, numerical-parity or allocation evidence.
