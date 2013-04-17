/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2011 Tokutek Inc.  All rights reserved."
#ident "$Id$"

#include <zlib.h>
#include <lzma.h>

#include "compress.h"
#include "memory.h"
#include "quicklz.h"
#include "toku_assert.h"

static inline enum toku_compression_method
normalize_compression_method(enum toku_compression_method method)
// Effect: resolve "friendly" names like "fast" and "small" into their real values.
{
    switch (method) {
    case TOKU_DEFAULT_COMPRESSION_METHOD:
    case TOKU_FAST_COMPRESSION_METHOD:
        return TOKU_QUICKLZ_METHOD;
    case TOKU_SMALL_COMPRESSION_METHOD:
        return TOKU_LZMA_METHOD;
    default:
        return method; // everything else is fine
    }
}

size_t toku_compress_bound (enum toku_compression_method a, size_t size)
// See compress.h for the specification of this function.
{
    a = normalize_compression_method(a);
    switch (a) {
    case TOKU_NO_COMPRESSION:
        return size + 1;
    case TOKU_LZMA_METHOD:
	return 1+lzma_stream_buffer_bound(size); // We need one extra for the rfc1950-style header byte (bits -03 are TOKU_LZMA_METHOD (1), bits 4-7 are the compression level)
    case TOKU_QUICKLZ_METHOD:
        return size+400 + 1;  // quicklz manual says 400 bytes is enough.  We need one more byte for the rfc1950-style header byte.  bits 0-3 are 9, bits 4-7 are the QLZ_COMPRESSION_LEVEL.
    case TOKU_ZLIB_METHOD:
        return compressBound (size);
    default:
        break;
    }
    // fall through for bad enum (thus compiler can warn us if we didn't use all the enums
    assert(0); return 0;
}

static const int zlib_compression_level = 5;

void toku_compress (enum toku_compression_method a,
                    // the following types and naming conventions come from zlib.h
                    Bytef       *dest,   uLongf *destLen,
                    const Bytef *source, uLong   sourceLen)
// See compress.h for the specification of this function.
{
    a = normalize_compression_method(a);
    assert(sourceLen < (1LL << 32));
    switch (a) {
    case TOKU_NO_COMPRESSION:
        dest[0] = TOKU_NO_COMPRESSION;
        memcpy(dest + 1, source, sourceLen);
        *destLen = sourceLen + 1;
        return;
    case TOKU_ZLIB_METHOD: {
        int r = compress2(dest, destLen, source, sourceLen, zlib_compression_level);
        assert(r == Z_OK);
        assert((dest[0]&0xF) == TOKU_ZLIB_METHOD);
        return;
    }
    case  TOKU_QUICKLZ_METHOD: {
        if (sourceLen==0) {
            // quicklz requires at least one byte, so we handle this ourselves
            assert(1 <= *destLen);
            *destLen = 1;
        } else {
            qlz_state_compress *XMALLOC(qsc);
            size_t actual_destlen = qlz_compress(source, (char*)(dest+1), sourceLen, qsc);
            assert(actual_destlen +1 <= *destLen);
            *destLen = actual_destlen+1; // add one for the rfc1950-style header byte.
            toku_free(qsc);
        }
        // Fill in that first byte
        dest[0] = TOKU_QUICKLZ_METHOD + (QLZ_COMPRESSION_LEVEL << 4);
        return;
    }
    case TOKU_LZMA_METHOD: {
	const int lzma_compression_level = 2;
	if (sourceLen==0) {
	    // lzma version 4.999 requires at least one byte, so we'll do it ourselves.
	    assert(1<=*destLen);
	    *destLen = 1;
	} else {
	    size_t out_pos = 1;
	    lzma_ret r = lzma_easy_buffer_encode(lzma_compression_level, LZMA_CHECK_CRC32, NULL,
						 source, sourceLen,
						 dest, &out_pos, *destLen);
	    assert(out_pos < *destLen);
	    assert(r==LZMA_OK);
	    *destLen = out_pos;
	}
	dest[0] = TOKU_LZMA_METHOD + (lzma_compression_level << 4);

	return;
    }
    default:
        break;
    }
    // default fall through to error.
    assert(0);
}

void toku_decompress (Bytef       *dest,   uLongf destLen,
                      const Bytef *source, uLongf sourceLen)
// See compress.h for the specification of this function.
{
    assert(sourceLen>=1); // need at least one byte for the RFC header.
    switch (source[0] & 0xF) {
    case TOKU_NO_COMPRESSION:
        memcpy(dest, source + 1, sourceLen - 1);
        return;
    case TOKU_ZLIB_METHOD: {
        uLongf actual_destlen = destLen;
        int r = uncompress(dest, &actual_destlen, source, sourceLen);
        assert(r == Z_OK);
        assert(actual_destlen == destLen);
        return;
    }
    case TOKU_QUICKLZ_METHOD:
        if (sourceLen>1) {
            qlz_state_decompress *XMALLOC(qsd);
            uLongf actual_destlen = qlz_decompress((char*)source+1, dest, qsd);
            assert(actual_destlen == destLen);
            toku_free(qsd);
        } else {
            // length 1 means there is no data, so do nothing.
            assert(destLen==0);
        }
        return;
    case TOKU_LZMA_METHOD: {
	if (sourceLen>1) {
	    uint64_t memlimit = UINT64_MAX;
	    size_t out_pos = 0;
	    size_t in_pos  = 1;
	    lzma_ret r = lzma_stream_buffer_decode(&memlimit,  // memlimit, use UINT64_MAX to disable this check
						   0,          // flags
						   NULL,       // allocator
						   source, &in_pos, sourceLen,
						   dest,   &out_pos, destLen);
	    assert(r==LZMA_OK);
	    assert(out_pos == destLen);
	} else {
	    // length 1 means there is no data, so do nothing.
	    assert(destLen==0);
	}
	return;
    }
    }
    // default fall through to error.
    assert(0);
}
