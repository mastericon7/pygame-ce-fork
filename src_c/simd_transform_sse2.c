#include "simd_transform.h"

#if PG_ENABLE_ARM_NEON
// sse2neon.h is from here: https://github.com/DLTcollab/sse2neon
#include "include/sse2neon.h"
#endif /* PG_ENABLE_ARM_NEON */

/* This returns 1 when sse2 is available at runtime but support for it isn't
 * compiled in, 0 in all other cases */
int
pg_sse2_at_runtime_but_uncompiled()
{
    if (SDL_HasSSE2()) {
#ifdef __SSE2__
        return 0;
#else
        return 1;
#endif /* __SSE2__ */
    }
    return 0;
}

/* This returns 1 when neon is available at runtime but support for it isn't
 * compiled in, 0 in all other cases */
int
pg_neon_at_runtime_but_uncompiled()
{
    if (SDL_HasNEON()) {
#if PG_ENABLE_ARM_NEON
        return 0;
#else
        return 1;
#endif /* PG_ENABLE_ARM_NEON */
    }
    return 0;
}

#if (defined(__SSE2__) || defined(PG_ENABLE_ARM_NEON))

// For some reason this is not defined on some non Windows compilers
#define _pg_loadu_si32(p) _mm_cvtsi32_si128(*(unsigned int const *)(p))
#define _pg_loadu_si64(p) _mm_loadl_epi64((__m128i const *)(p))
#define _pg_storeu_si32(p, a) (void)(*(int *)(p) = _mm_cvtsi128_si32((a)))
#define _pg_storeu_si64(p, a) (_mm_storel_epi64((__m128i *)(p), (a)))

void
filter_shrink_X_SSE2(Uint8 *srcpix, Uint8 *dstpix, int height, int srcpitch,
                     int dstpitch, int srcwidth, int dstwidth)
{
    // This filter run through multiple pixels in a row at once, since it
    // accumulates then writes -- the pixels in a row are not independent.
    // However, this accumulate/write cycle is the same for each row, so
    // multiple rows can be run at once, saving about 37% X-shrink runtime
    // in my testing.

    // These need to skip remaining bytes in a row, plus the entire next
    // row, with the alternating row strategy. The redundancy in the equations
    // makes them more clear, so I left it.
    int srcdiff = srcpitch - (srcwidth * 4) + srcpitch;
    int dstdiff = dstpitch - (dstwidth * 4) + dstpitch;
    Uint8 *srcpix2 = srcpix + srcpitch;
    Uint8 *dstpix2 = dstpix + dstpitch;

    int x, y;
    __m128i src, src2, dst, accumulate, mm_xcounter, mm_xfrac;

    int xspace = 0x04000 * srcwidth / dstwidth; /* must be > 1 */
    __m128i xrecip = _mm_set1_epi16((Uint16)(0x40000000 / xspace));

    for (y = 0; y < height; y += 2) {
        accumulate = _mm_setzero_si128();
        int xcounter = xspace;

        // Prevent overwrite over final row of pixels when the surface height
        // is odd (as in not even)
        if (y == height - 1) {
            srcpix2 = srcpix;
            dstpix2 = dstpix;
        }

        for (x = 0; x < srcwidth; x++) {
            if (xcounter > 0x04000) {
                // Load a pixel from two separate lines at once
                // Unpack RGBA into 16 bit lanes
                src = _mm_unpacklo_epi8(_pg_loadu_si32(srcpix),
                                        _mm_setzero_si128());
                src2 = _mm_unpacklo_epi8(_pg_loadu_si32(srcpix2),
                                         _mm_setzero_si128());

                // Combine the two expanded pixels
                src = _mm_unpacklo_epi64(src, src2);

                // Accumulate[127:64] tracks the srcpix2 pixel line,
                // [63:0] tracks the srcpix pixel line
                accumulate = _mm_add_epi16(accumulate, src);
                srcpix += 4;
                srcpix2 += 4;
                xcounter -= 0x04000;
            }
            /* write out a destination pixel */
            else {
                int xfrac = 0x04000 - xcounter;

                // Broadcast variables into intrinsics
                mm_xcounter = _mm_set1_epi16(xcounter);
                mm_xfrac = _mm_set1_epi16(xfrac);

                // Load a pixel from two separate lines at once
                // Unpack RGBA into 16 bit lanes
                src = _mm_unpacklo_epi8(_pg_loadu_si32(srcpix),
                                        _mm_setzero_si128());
                src2 = _mm_unpacklo_epi8(_pg_loadu_si32(srcpix2),
                                         _mm_setzero_si128());

                // Combine the two expanded pixels
                src = _mm_unpacklo_epi64(src, src2);

                // The operation! Translated from old filter_shrink_X_SSE
                // assembly. I don't understand the equivalence between
                // these operations and the C version of these operations,
                // but it works.
                src = _mm_slli_epi16(src, 2);
                dst = _mm_mulhi_epu16(src, mm_xcounter);
                dst = _mm_add_epi16(dst, accumulate);
                accumulate = _mm_mulhi_epu16(src, mm_xfrac);

                dst = _mm_mulhi_epu16(dst, xrecip);

                // Pack and store results. Once packed, both pixel results are
                // in 64 bits. First 32 is stored at dstpix, next 32 is stored
                // at dstpix2.
                dst = _mm_packus_epi16(dst, _mm_setzero_si128());
                _pg_storeu_si32(dstpix, dst);
                _pg_storeu_si32(dstpix2, _mm_srli_si128(dst, 4));

                dstpix += 4;
                dstpix2 += 4;
                srcpix += 4;
                srcpix2 += 4;
                xcounter = xspace - xfrac;
            }
        }
        srcpix += srcdiff;
        srcpix2 += srcdiff;
        dstpix += dstdiff;
        dstpix2 += dstdiff;
    }
}

void
filter_shrink_Y_SSE2(Uint8 *srcpix, Uint8 *dstpix, int width, int srcpitch,
                     int dstpitch, int srcheight, int dstheight)
{
    // This filter also iterates over Y and then over X, but unlike
    // filter_shrink_X_SSE2 it can be parallelized in the X direction because
    // each pixel in a row is independent from the algorithm's perspective.
    // Going from SSE2 single pixel to SSE2 double pixel in this saved about
    // 25% Y-shrink runtime in my testing.

    int srcdiff = srcpitch - (width * 4);
    int dstdiff = dstpitch - (width * 4);
    int x, y;
    __m128i src, dst, mm_acc, mm_yfrac, mm_ycounter;

    int during_2_width = width / 2;
    int post_2_width = width % 2;

    int yspace = 0x04000 * srcheight / dstheight; /* must be > 1 */
    __m128i yrecip = _mm_set1_epi16(0x40000000 / yspace);
    int ycounter = yspace;

    Uint16 *templine;
    /* allocate a clear memory area for storing the accumulator line */
    // Future: when we support SDL 2.0.10 and up, we can use SDL_SIMDAlloc
    // here so accumulate load/stores can be aligned, for a small perf
    // improvement.
    templine = (Uint16 *)calloc(dstpitch, 2);
    if (templine == NULL) {
        return;
    }

    for (y = 0; y < srcheight; y++) {
        Uint16 *accumulate = templine;
        if (ycounter > 0x04000) {
            // Loads up the whole pixel row in 16 bit lanes and adds to
            // existing data in accumulate/templine.
            for (x = 0; x < during_2_width; x++) {
                src = _mm_unpacklo_epi8(_pg_loadu_si64(srcpix),
                                        _mm_setzero_si128());
                _mm_storeu_si128(
                    (__m128i *)accumulate,
                    _mm_add_epi16(_mm_loadu_si128((const __m128i *)accumulate),
                                  src));
                accumulate += 8;  // 8 Uint16s, so 16 bytes
                srcpix += 8;      // 8 Uint8s, so 8 bytes (2 pixels)
            }
            if (post_2_width) {  // either 0 or 1, no need for second for loop
                src = _mm_unpacklo_epi8(_pg_loadu_si32(srcpix),
                                        _mm_setzero_si128());
                _pg_storeu_si64(
                    accumulate,
                    _mm_add_epi16(_pg_loadu_si64(accumulate), src));
                accumulate += 4;  // 4 Uint16s, so 8 bytes
                srcpix += 4;      // 4 bytes (1 pixel)
            }
            ycounter -= 0x04000;
        }
        else {
            // Calculates and tracks variables in C then broadcasts them
            // to intrinsics when needed for calculations.
            int yfrac = 0x04000 - ycounter;
            mm_yfrac = _mm_set1_epi16(yfrac);
            mm_ycounter = _mm_set1_epi16(ycounter);

            /* write out a destination line */
            for (x = 0; x < during_2_width; x++) {
                src = _mm_unpacklo_epi8(_pg_loadu_si64(srcpix),
                                        _mm_setzero_si128());
                srcpix += 8;  // 8 bytes
                mm_acc = _mm_loadu_si128((const __m128i *)accumulate);

                src = _mm_slli_epi16(src, 2);
                dst = _mm_mulhi_epu16(src, mm_yfrac);
                src = _mm_mulhi_epu16(src, mm_ycounter);

                _mm_storeu_si128((__m128i *)accumulate, dst);
                accumulate += 8;  // 4 Uint16s, so 8 bytes

                dst = _mm_add_epi16(src, mm_acc);
                dst = _mm_mulhi_epu16(dst, yrecip);
                dst = _mm_packus_epi16(dst, _mm_setzero_si128());
                _pg_storeu_si64(dstpix, dst);
                dstpix += 8;  // 8 bytes
            }
            if (post_2_width) {  // either 0 or 1, no need for second for loop
                src = _mm_unpacklo_epi8(_pg_loadu_si32(srcpix),
                                        _mm_setzero_si128());
                srcpix += 4;
                mm_acc = _pg_loadu_si64(accumulate);

                src = _mm_slli_epi16(src, 2);
                dst = _mm_mulhi_epu16(src, mm_yfrac);
                src = _mm_mulhi_epu16(src, mm_ycounter);

                _pg_storeu_si64(accumulate, dst);
                // accumulate doesn't need to be incremented here because
                // it is reassigned at the top of the loop.

                dst = _mm_add_epi16(src, mm_acc);
                dst = _mm_mulhi_epu16(dst, yrecip);
                dst = _mm_packus_epi16(dst, _mm_setzero_si128());
                _pg_storeu_si32(dstpix, dst);
                dstpix += 4;
            }
            dstpix += dstdiff;
            ycounter = yspace - yfrac;
        }
        srcpix += srcdiff;
    }

    /* free the temporary memory */
    free(templine);
}

void
filter_expand_X_SSE2(Uint8 *srcpix, Uint8 *dstpix, int height, int srcpitch,
                     int dstpitch, int srcwidth, int dstwidth)
{
    int dstdiff = dstpitch - (dstwidth * 4);
    int *xidx0, *xmult_combined;
    int x, y;
    const int factorwidth = 8;

    // Inherited this from the ONLYC variant, maybe can be removed/
#ifdef _MSC_VER
    /* Make MSVC static analyzer happy by assuring dstwidth >= 2 to suppress
     * a false analyzer report */
    __analysis_assume(dstwidth >= 2);
#endif

    /* Allocate memory for factors */
    xidx0 = malloc(dstwidth * 4);
    if (xidx0 == 0)
        return;
    // This algorithm uses two multipliers, xm0 and xm1. Each multiplier
    // gets 32 bits of space, so this gives 64 bits per dstwidth.
    xmult_combined = (int *)malloc(dstwidth * factorwidth);
    if (xmult_combined == 0) {
        free(xidx0);
        return;
    }

    /* Create multiplier factors and starting indices and put them in arrays */
    for (x = 0; x < dstwidth; x++) {
        // Could it be worth it to reduce the fixed point there to fit
        // inside 16 bits (0xFF), and then pack xidx0 in with mult factors?
        int xm1 = 0x100 * ((x * (srcwidth - 1)) % dstwidth) / dstwidth;
        int xm0 = 0x100 - xm1;
        xidx0[x] = x * (srcwidth - 1) / dstwidth;

        // packs xm0 and xm1 scaling factors into a combined array, for easy
        // loading
        xmult_combined[x * 2] = xm0 | (xm0 << 16);
        xmult_combined[x * 2 + 1] = xm1 | (xm1 << 16);
    }

    __m128i src, multcombined, dst;

    /* Do the scaling in raster order so we don't trash the cache */
    for (y = 0; y < height; y++) {
        Uint8 *srcrow0 = srcpix + y * srcpitch;
        for (x = 0; x < dstwidth; x++) {
            Uint8 *src_p =
                srcrow0 + xidx0[x] * 4;  // *8 now because of factorwidth?

            src =
                _mm_unpacklo_epi8(_pg_loadu_si64(src_p), _mm_setzero_si128());

            // uses combined multipliers against 2 src pixels
            // xm0 against src[0-3] (1 px), and xm1 against src[4-7] (1 px)
            multcombined = _mm_shuffle_epi32(
                _pg_loadu_si64(xmult_combined + x * 2), 0b01010000);
            src = _mm_mullo_epi16(src, multcombined);

            // shift over pixel 2 results and add with pixel 1 results
            dst = _mm_add_epi16(src, _mm_bsrli_si128(src, 8));

            // pack results and store destination pixel.
            dst =
                _mm_packus_epi16(_mm_srli_epi16(dst, 8), _mm_setzero_si128());
            _pg_storeu_si32(dstpix, dst);

            dstpix += 4;
        }
        dstpix += dstdiff;
    }

    /* free memory */
    free(xidx0);
    free(xmult_combined);
}

void
filter_expand_Y_SSE2(Uint8 *srcpix, Uint8 *dstpix, int width, int srcpitch,
                     int dstpitch, int srcheight, int dstheight)
{
    // This filter parallelizes math operations using SSE2, but it also runs
    // through each row 2 pixels at a time. The 2x pixels at a time strategy
    // was a 23% performance improvment for Y-expand over 1x at a time.

    int x, y;
    __m128i src0, src1, dst, ymult0_mm, ymult1_mm;

    // For some reason the C implementation does not have this, is that ok?
    int dstdiff = dstpitch - (width * 4);

    int during_2_width = width / 2;
    int post_2_width = width % 2;

    for (y = 0; y < dstheight; y++) {
        int yidx0 = y * (srcheight - 1) / dstheight;
        Uint8 *srcrow0 = srcpix + yidx0 * srcpitch;
        Uint8 *srcrow1 = srcrow0 + srcpitch;
        int ymult1 = 0x0100 * ((y * (srcheight - 1)) % dstheight) / dstheight;
        int ymult0 = 0x0100 - ymult1;

        ymult0_mm = _mm_set1_epi16(ymult0);
        ymult1_mm = _mm_set1_epi16(ymult1);

        for (x = 0; x < during_2_width; x++) {
            // Load from srcrow0 and srcrow1 two pixels each, swizzled out
            // into 16 bit lanes.
            src0 = _mm_unpacklo_epi8(_pg_loadu_si64(srcrow0),
                                     _mm_setzero_si128());
            src1 = _mm_unpacklo_epi8(_pg_loadu_si64(srcrow1),
                                     _mm_setzero_si128());

            src0 = _mm_mullo_epi16(src0, ymult0_mm);
            src1 = _mm_mullo_epi16(src1, ymult1_mm);
            dst = _mm_add_epi16(src0, src1);
            dst = _mm_srli_epi16(dst, 8);

            // Pack down and store 2 destination pixels.
            dst = _mm_packus_epi16(dst, _mm_setzero_si128());
            _pg_storeu_si64(dstpix, dst);

            srcrow0 += 8;  // 8 bytes (2 pixels)
            srcrow1 += 8;
            dstpix += 8;
        }
        if (post_2_width) {
            // Load from srcrow0 and srcrow1 one pixel each, swizzled out
            // into 16 bit lanes.
            src0 = _mm_unpacklo_epi8(_pg_loadu_si32(srcrow0),
                                     _mm_setzero_si128());
            src1 = _mm_unpacklo_epi8(_pg_loadu_si32(srcrow1),
                                     _mm_setzero_si128());

            src0 = _mm_mullo_epi16(src0, ymult0_mm);
            src1 = _mm_mullo_epi16(src1, ymult1_mm);
            dst = _mm_add_epi16(src0, src1);
            dst = _mm_srli_epi16(dst, 8);

            // Pack down and store 1 destination pixel.
            dst = _mm_packus_epi16(dst, _mm_setzero_si128());
            _pg_storeu_si32(dstpix, dst);

            srcrow0 += 4;  // 4 bytes (1 pixel)
            srcrow1 += 4;
            dstpix += 4;
        }
        dstpix += dstdiff;
    }
}

#endif /* __SSE2__ || PG_ENABLE_ARM_NEON*/
