/*
 * The upstream OSD decoder is optional and implemented in Fortran. The normal
 * Fano/stack decoder never calls this symbol unless the user explicitly asks
 * for -o. airspyhf-wsprd does not expose -o, so this link-time stub removes the
 * Fortran toolchain/runtime dependency without changing the default path.
 */
void osdwspr_(float symbols[], unsigned char mask[], int *depth,
              unsigned char codeword[], int *hard_errors, float *distance)
{
    (void)symbols;
    (void)mask;
    (void)depth;
    (void)codeword;
    *hard_errors = 163;
    *distance = 0.0f;
}
