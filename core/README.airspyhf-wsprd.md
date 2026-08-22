# Synced wsprd core

These sources are synchronized from the upstream
[`kholia/wsprd`](https://github.com/kholia/wsprd/tree/main) repository using
`scripts/sync-wsprd-core.sh`. The exact source revision is recorded in
`UPSTREAM_COMMIT`; `core/` is only the vendored build copy.

`airspyhf-wsprd` uses the normal C Fano decoder. The optional upstream
ordered-statistics decoder is not enabled, allowing the core to link without a
Fortran compiler or runtime. This is the same default path used by upstream
`wsprd` and is covered by the `150426_0918.wav` nine-spot regression.
