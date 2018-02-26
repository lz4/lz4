/*
    lz4opt.h - Optimal Mode of LZ4
    Copyright (C) 2015-2017, Przemyslaw Skibinski <inikep@gmail.com>
    Note : this file is intended to be included within lz4hc.c

    BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    You can contact the author at :
       - LZ4 source repository : https://github.com/lz4/lz4
       - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

#define LZ4_OPT_NUM   (1<<12)

typedef struct {
    int price;
    int off;
    int mlen;
    int litlen;
} LZ4HC_optimal_t;


/* price in bytes */
LZ4_FORCE_INLINE int LZ4HC_literalsPrice(int const litlen)
{
    int price = litlen;
    if (litlen >= (int)RUN_MASK)
        price += 1 + (litlen-RUN_MASK)/255;
    return price;
}


/* requires mlen >= MINMATCH */
LZ4_FORCE_INLINE int LZ4HC_sequencePrice(int litlen, int mlen)
{
    int price = 1 + 2 ; /* token + 16-bit offset */

    price += LZ4HC_literalsPrice(litlen);

    if (mlen >= (int)(ML_MASK+MINMATCH))
        price += 1 + (mlen-(ML_MASK+MINMATCH))/255;

    return price;
}


/*-*************************************
*  Match finder
***************************************/
typedef struct {
    int off;
    int len;
} LZ4HC_match_t;

LZ4_FORCE_INLINE
LZ4HC_match_t LZ4HC_FindLongerMatch(LZ4HC_CCtx_internal* const ctx,
                        const BYTE* ip, const BYTE* const iHighLimit,
                        int minLen, int nbSearches)
{
    LZ4HC_match_t match = { 0 , 0 };
    const BYTE* matchPtr = NULL;
    /* note : LZ4HC_InsertAndGetWiderMatch() is able to modify the starting position of a match (*startpos),
     * but this won't be the case here, as we define iLowLimit==ip,
     * so LZ4HC_InsertAndGetWiderMatch() won't be allowed to search past ip */
    int const matchLength = LZ4HC_InsertAndGetWiderMatch(ctx,
                                ip, ip, iHighLimit, minLen, &matchPtr, &ip,
                                nbSearches, 1 /* patternAnalysis */);
    if (matchLength <= minLen) return match;
    match.len = matchLength;
    match.off = (int)(ip-matchPtr);
    return match;
}


static int LZ4HC_compress_optimal (
    LZ4HC_CCtx_internal* ctx,
    const char* const source,
    char* dst,
    int* srcSizePtr,
    int dstCapacity,
    int const nbSearches,
    size_t sufficient_len,
    limitedOutput_directive limit,
    int const fullUpdate
    )
{
#define TRAILING_LITERALS 3
    LZ4HC_optimal_t opt[LZ4_OPT_NUM + TRAILING_LITERALS];   /* this uses a bit too much stack memory to my taste ... */

    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + *srcSizePtr;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;
    BYTE* op = (BYTE*) dst;
    BYTE* opSaved = (BYTE*) dst;
    BYTE* oend = op + dstCapacity;

    /* init */
    DEBUGLOG(5, "LZ4HC_compress_optimal");
    *srcSizePtr = 0;
    if (limit == limitedDestSize) oend -= LASTLITERALS;   /* Hack for support LZ4 format restriction */
    if (sufficient_len >= LZ4_OPT_NUM) sufficient_len = LZ4_OPT_NUM-1;

    /* Main Loop */
    assert(ip - anchor < LZ4_MAX_INPUT_SIZE);
    while (ip <= mflimit) {
        int const llen = (int)(ip - anchor);
        int best_mlen, best_off;
        int cur, last_match_pos = 0;

        LZ4HC_match_t const firstMatch = LZ4HC_FindLongerMatch(ctx, ip, matchlimit, MINMATCH-1, nbSearches);
        if (firstMatch.len==0) { ip++; continue; }

        if ((size_t)firstMatch.len > sufficient_len) {
            /* good enough solution : immediate encoding */
            int const firstML = firstMatch.len;
            const BYTE* const matchPos = ip - firstMatch.off;
            opSaved = op;
            if ( LZ4HC_encodeSequence(&ip, &op, &anchor, firstML, matchPos, limit, oend) )   /* updates ip, op and anchor */
                goto _dest_overflow;
            continue;
        }

        /* set prices for first positions (literals) */
        {   int rPos;
            for (rPos = 0 ; rPos < MINMATCH ; rPos++) {
                int const cost = LZ4HC_literalsPrice(llen + rPos);
                opt[rPos].mlen = 1;
                opt[rPos].off = 0;
                opt[rPos].litlen = llen + rPos;
                opt[rPos].price = cost;
                DEBUGLOG(7, "rPos:%3i => price:%3i (litlen=%i) -- initial setup",
                            rPos, cost, opt[rPos].litlen);
        }   }
        /* set prices using initial match */
        {   int mlen = MINMATCH;
            int const matchML = firstMatch.len;   /* necessarily < sufficient_len < LZ4_OPT_NUM */
            int const offset = firstMatch.off;
            assert(matchML < LZ4_OPT_NUM);
            for ( ; mlen <= matchML ; mlen++) {
                int const cost = LZ4HC_sequencePrice(llen, mlen);
                opt[mlen].mlen = mlen;
                opt[mlen].off = offset;
                opt[mlen].litlen = llen;
                opt[mlen].price = cost;
                DEBUGLOG(7, "rPos:%3i => price:%3i (matchlen=%i) -- initial setup",
                            mlen, cost, mlen);
        }   }
        last_match_pos = firstMatch.len;
        {   int addLit;
            for (addLit = 1; addLit <= TRAILING_LITERALS; addLit ++) {
                opt[last_match_pos+addLit].mlen = 1; /* literal */
                opt[last_match_pos+addLit].off = 0;
                opt[last_match_pos+addLit].litlen = addLit;
                opt[last_match_pos+addLit].price = opt[last_match_pos].price + LZ4HC_literalsPrice(addLit);
                DEBUGLOG(7, "rPos:%3i => price:%3i (litlen=%i) -- initial setup",
                            last_match_pos+addLit, opt[last_match_pos+addLit].price, addLit);
        }   }

        /* check further positions */
        for (cur = 1; cur < last_match_pos; cur++) {
            const BYTE* const curPtr = ip + cur;
            LZ4HC_match_t newMatch;

            if (curPtr > mflimit) break;
            DEBUGLOG(7, "rPos:%u[%u] vs [%u]%u",
                    cur, opt[cur].price, opt[cur+1].price, cur+1);
            if (fullUpdate) {
                /* not useful to search here if next position has same (or lower) cost */
                if ( (opt[cur+1].price <= opt[cur].price)
                  /* in some cases, next position has same cost, but cost rises sharply after, so a small match would still be beneficial */
                  && (opt[cur+MINMATCH].price < opt[cur].price + 3/*min seq price*/) )
                    continue;
            } else {
                /* not useful to search here if next position has same (or lower) cost */
                if (opt[cur+1].price <= opt[cur].price) continue;
            }

            DEBUGLOG(7, "search at rPos:%u", cur);
            if (fullUpdate)
                newMatch = LZ4HC_FindLongerMatch(ctx, curPtr, matchlimit, MINMATCH-1, nbSearches);
            else
                /* only test matches of minimum length; slightly faster, but misses a few bytes */
                newMatch = LZ4HC_FindLongerMatch(ctx, curPtr, matchlimit, last_match_pos - cur, nbSearches);
            if (!newMatch.len) continue;

            if ( ((size_t)newMatch.len > sufficient_len)
              || (newMatch.len + cur >= LZ4_OPT_NUM) ) {
                /* immediate encoding */
                best_mlen = newMatch.len;
                best_off = newMatch.off;
                last_match_pos = cur + 1;
                goto encode;
            }

            /* before match : set price with literals at beginning */
            {   int const baseLitlen = opt[cur].litlen;
                int litlen;
                for (litlen = 1; litlen < MINMATCH; litlen++) {
                    int const price = opt[cur].price - LZ4HC_literalsPrice(baseLitlen) + LZ4HC_literalsPrice(baseLitlen+litlen);
                    int const pos = cur + litlen;
                    if (price < opt[pos].price) {
                        opt[pos].mlen = 1; /* literal */
                        opt[pos].off = 0;
                        opt[pos].litlen = baseLitlen+litlen;
                        opt[pos].price = price;
                        DEBUGLOG(7, "rPos:%3i => price:%3i (litlen=%i)",
                                    pos, price, opt[pos].litlen);
            }   }   }

            /* set prices using match at position = cur */
            {   int const matchML = newMatch.len;
                int ml = MINMATCH;

                assert(cur + newMatch.len < LZ4_OPT_NUM);
                for ( ; ml <= matchML ; ml++) {
                    int const pos = cur + ml;
                    int const offset = newMatch.off;
                    int price;
                    int ll;
                    DEBUGLOG(7, "testing price rPos %i (last_match_pos=%i)",
                                pos, last_match_pos);
                    if (opt[cur].mlen == 1) {
                        ll = opt[cur].litlen;
                        price = ((cur > ll) ? opt[cur - ll].price : 0)
                              + LZ4HC_sequencePrice(ll, ml);
                    } else {
                        ll = 0;
                        price = opt[cur].price + LZ4HC_sequencePrice(0, ml);
                    }

                    if (pos > last_match_pos+TRAILING_LITERALS || price <= opt[pos].price) {
                        DEBUGLOG(7, "rPos:%3i => price:%3i (matchlen=%i)",
                                    pos, price, ml);
                        assert(pos < LZ4_OPT_NUM);
                        if ( (ml == matchML)  /* last pos of last match */
                          && (last_match_pos < pos) )
                            last_match_pos = pos;
                        opt[pos].mlen = ml;
                        opt[pos].off = offset;
                        opt[pos].litlen = ll;
                        opt[pos].price = price;
            }   }   }
            /* complete following positions with literals */
            {   int addLit;
                for (addLit = 1; addLit <= TRAILING_LITERALS; addLit ++) {
                    opt[last_match_pos+addLit].mlen = 1; /* literal */
                    opt[last_match_pos+addLit].off = 0;
                    opt[last_match_pos+addLit].litlen = addLit;
                    opt[last_match_pos+addLit].price = opt[last_match_pos].price + LZ4HC_literalsPrice(addLit);
                    DEBUGLOG(7, "rPos:%3i => price:%3i (litlen=%i)", last_match_pos+addLit, opt[last_match_pos+addLit].price, addLit);
            }   }
        }  /* for (cur = 1; cur <= last_match_pos; cur++) */

        best_mlen = opt[last_match_pos].mlen;
        best_off = opt[last_match_pos].off;
        cur = last_match_pos - best_mlen;

encode: /* cur, last_match_pos, best_mlen, best_off must be set */
        assert(cur < LZ4_OPT_NUM);
        assert(last_match_pos >= 1);  /* == 1 when only one candidate */
        DEBUGLOG(6, "reverse traversal, looking for shortest path")
        DEBUGLOG(6, "last_match_pos = %i", last_match_pos);
        {   int candidate_pos = cur;
            int selected_matchLength = best_mlen;
            int selected_offset = best_off;
            while (1) {  /* from end to beginning */
                int const next_matchLength = opt[candidate_pos].mlen;  /* can be 1, means literal */
                int const next_offset = opt[candidate_pos].off;
                DEBUGLOG(6, "pos %i: sequence length %i", candidate_pos, selected_matchLength);
                opt[candidate_pos].mlen = selected_matchLength;
                opt[candidate_pos].off = selected_offset;
                selected_matchLength = next_matchLength;
                selected_offset = next_offset;
                if (next_matchLength > candidate_pos) break; /* last match elected, first match to encode */
                assert(next_matchLength > 0);  /* can be 1, means literal */
                candidate_pos -= next_matchLength;
        }   }

        /* encode all recorded sequences in order */
        {   int rPos = 0;  /* relative position (to ip) */
            while (rPos < last_match_pos) {
                int const ml = opt[rPos].mlen;
                int const offset = opt[rPos].off;
                if (ml == 1) { ip++; rPos++; continue; }  /* literal; note: can end up with several literals, in which case, skip them */
                rPos += ml;
                assert(ml >= MINMATCH);
                assert((offset >= 1) && (offset <= MAX_DISTANCE));
                opSaved = op;
                if ( LZ4HC_encodeSequence(&ip, &op, &anchor, ml, ip - offset, limit, oend) )   /* updates ip, op and anchor */
                    goto _dest_overflow;
        }   }
    }  /* while (ip <= mflimit) */

_last_literals:
    /* Encode Last Literals */
    {   size_t lastRunSize = (size_t)(iend - anchor);  /* literals */
        size_t litLength = (lastRunSize + 255 - RUN_MASK) / 255;
        size_t const totalSize = 1 + litLength + lastRunSize;
        if (limit == limitedDestSize) oend += LASTLITERALS;  /* restore correct value */
        if (limit && (op + totalSize > oend)) {
            if (limit == limitedOutput) return 0;  /* Check output limit */
            /* adapt lastRunSize to fill 'dst' */
            lastRunSize  = (size_t)(oend - op) - 1;
            litLength = (lastRunSize + 255 - RUN_MASK) / 255;
            lastRunSize -= litLength;
        }
        ip = anchor + lastRunSize;

        if (lastRunSize >= RUN_MASK) {
            size_t accumulator = lastRunSize - RUN_MASK;
            *op++ = (RUN_MASK << ML_BITS);
            for(; accumulator >= 255 ; accumulator -= 255) *op++ = 255;
            *op++ = (BYTE) accumulator;
        } else {
            *op++ = (BYTE)(lastRunSize << ML_BITS);
        }
        memcpy(op, anchor, lastRunSize);
        op += lastRunSize;
    }

    /* End */
    *srcSizePtr = (int) (((const char*)ip) - source);
    return (int) ((char*)op-dst);

_dest_overflow:
    if (limit == limitedDestSize) {
        op = opSaved;  /* restore correct out pointer */
        goto _last_literals;
    }
    return 0;
}
