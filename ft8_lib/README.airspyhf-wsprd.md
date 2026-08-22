# Synced ft8_lib core

These sources are synchronized from the upstream
[`howard0su/ft8_lib`](https://github.com/howard0su/ft8_lib) repository using
`scripts/sync-ft8-lib.sh`. The exact revision is recorded in
`UPSTREAM_COMMIT`.

Only the C runtime, KISS FFT implementation, monitor, WAV helper, and one
upstream regression recording are vendored. The upstream Fortran reference
material, PortAudio demo, and bulk test corpus are not runtime dependencies.
