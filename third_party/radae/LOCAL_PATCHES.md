# Local patches to the vendored RADE tree

`COMMIT` records the upstream commit this tree was vendored from. The tree is
that commit **plus** the upstream changes below, cherry-picked verbatim from
`freedv/rade_c`. Re-vendoring from the pinned commit alone silently drops them,
so a re-vendor must either move the pin past them or re-apply them.

| File | Change | Upstream | Why |
|---|---|---|---|
| `src/rade_bpf.c` | Renormalize the mixer phase at the end of `rade_bpf_process()` | `freedv/rade_c` `5cb24369` | The phase accumulator loses magnitude to float32 rounding; the BPF output scales with its square (#6398) |
| `src/rade_rx.c` | Enable the `rx_phase` renormalization in `rade_rx_process()` that was already present but commented out | `freedv/rade_c` `a2d77e85` | Same drift, once per frame instead of once per sample (#6398) |

`tests/rade_phase_renorm_test.cpp` fails if either normalization is removed, so
a re-vendor that drops them is caught by `ctest -R rade_phase_renorm_test`.

AetherSDR-authored additions that are not upstream cherry-picks carry their own
"AetherSDR changes from upstream" header in the file (see `src/rade_text.c`).
