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
    int off;
    int len;
} LZ4HC_match_t;

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

LZ4_FORCE_INLINE
int LZ4HC_FindLongerMatch(LZ4HC_CCtx_internal* const ctx,   /* Index table will be updated */
                        const BYTE* const ip, const BYTE* const iHighLimit,
                        int longest,
                        const BYTE** matchpos,
                        const int maxNbAttempts)
{
    const BYTE* uselessPtr = ip;
    return LZ4HC_InsertAndGetWiderMatch(ctx, ip, ip, iHighLimit, longest, matchpos, &uselessPtr, maxNbAttempts);
}

LZ4_FORCE_INLINE int LZ4HC_HashChain_GetAllMatches (
                        LZ4HC_CCtx_internal* ctx,
                        const BYTE* const ip, const BYTE* const iHighLimit,
                        size_t best_mlen, LZ4HC_match_t* matches)
{
    const BYTE* matchPtr = NULL;
    int matchLength = LZ4HC_FindLongerMatch(ctx, ip, iHighLimit, (int)best_mlen, &matchPtr, ctx->searchNum);
    if ((size_t)matchLength <= best_mlen) return 0;
    assert(matches != NULL);
    matches[0].len = matchLength;
    matches[0].off = (int)(ip-matchPtr);
    return 1;
}


static int LZ4HC_compress_optimal (
    LZ4HC_CCtx_internal* ctx,
    const char* const source,
    char* dst,
    int inputSize,
    int dstCapacity,
    limitedOutput_directive limit,
    size_t sufficient_len,
    const int fullUpdate
    )
{
    LZ4HC_optimal_t opt[LZ4_OPT_NUM + 3];   /* this uses a bit too much stack memory to my taste ... */
    LZ4HC_match_t matches[LZ4_OPT_NUM + 1];

    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;
    BYTE* op = (BYTE*) dst;
    BYTE* const oend = op + dstCapacity;

    /* init */
    DEBUGLOG(5, "LZ4HC_compress_optimal");
    if (sufficient_len >= LZ4_OPT_NUM) sufficient_len = LZ4_OPT_NUM-1;
    ctx->end += inputSize;
    ip++;

    /* Main Loop */
    assert(ip - anchor < LZ4_MAX_INPUT_SIZE);
    while (ip < mflimit) {
        int const llen = (int)(ip - anchor);
        int best_mlen, best_off;
        int cur, last_match_pos = 0;

        int const nb_matches_initial = LZ4HC_HashChain_GetAllMatches(ctx, ip, matchlimit, MINMATCH-1, matches);
        if (!nb_matches_initial) { ip++; continue; }

        if ((size_t)matches[nb_matches_initial-1].len > sufficient_len) {
            /* good enough solution : immediate encoding */
            int const firstML = matches[nb_matches_initial-1].len;
            const BYTE* const matchPos = ip - matches[nb_matches_initial-1].off;
            if ( LZ4HC_encodeSequence(&ip, &op, &anchor, firstML, matchPos, limit, oend) )   /* updates ip, op and anchor */
                return 0;  /* error */
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
        /* set prices using matches found for rPos = 0 */
        {   int matchNb;
            for (matchNb = 0; matchNb < nb_matches_initial; matchNb++) {
                int mlen = (matchNb>0) ? matches[matchNb-1].len+1 : MINMATCH;
                int const matchML = matches[matchNb].len;   /* necessarily < sufficient_len < LZ4_OPT_NUM */
                int const offset = matches[matchNb].off;
                assert(matchML < LZ4_OPT_NUM);
                for ( ; mlen <= matchML ; mlen++) {
                    int const cost = LZ4HC_sequencePrice(llen, mlen);
                    opt[mlen].mlen = mlen;
                    opt[mlen].off = offset;
                    opt[mlen].litlen = llen;
                    opt[mlen].price = cost;
                    DEBUGLOG(7, "rPos:%3i => price:%3i (matchlen=%i) -- initial setup",
                                mlen, cost, mlen);
        }   }   }
        last_match_pos = matches[nb_matches_initial-1].len;
        {   int addLit;
            for (addLit = 1; addLit <= 3; addLit ++) {
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
            int nb_matches;

            if (curPtr >= mflimit) break;
            DEBUGLOG(7, "rPos:%u[%u] vs [%u]%u",
                    cur, opt[cur].price, opt[cur+1].price, cur+1);
            if (fullUpdate) {
                if ((opt[cur+1].price <= opt[cur].price) && (opt[cur+4].price < opt[cur].price+3)) continue;
            } else {
                if (opt[cur+1].price <= opt[cur].price) continue;
            }

            DEBUGLOG(7, "search at rPos:%u", cur);
            if (fullUpdate)
                nb_matches = LZ4HC_HashChain_GetAllMatches(ctx, curPtr, matchlimit, MINMATCH-1, matches);
            else
                nb_matches = LZ4HC_HashChain_GetAllMatches(ctx, curPtr, matchlimit, last_match_pos - cur, matches);   /* only test matches of a minimum length; slightly faster, but misses a few bytes */
            if (!nb_matches) continue;

            if ( ((size_t)matches[nb_matches-1].len > sufficient_len)
              || (matches[nb_matches-1].len + cur >= LZ4_OPT_NUM) ) {
                /* immediate encoding */
                best_mlen = matches[nb_matches-1].len;
                best_off = matches[nb_matches-1].off;
                last_match_pos = cur + 1;
                goto encode;
            }

            /* before first match : set price with literals at beginning */
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

            /* set prices using matches at position = cur */
            {   int matchNb;
                assert(cur + matches[nb_matches-1].len < LZ4_OPT_NUM);
                for (matchNb = 0; matchNb < nb_matches; matchNb++) {
                    int const matchML = matches[matchNb].len;
                    int ml = (matchNb>0) ? matches[matchNb-1].len+1 : MINMATCH;

                    for ( ; ml <= matchML ; ml++) {
                        int const pos = cur + ml;
                        int const offset = matches[matchNb].off;
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

                        if (pos > last_match_pos+3 || price <= opt[pos].price) {
                            DEBUGLOG(7, "rPos:%3i => price:%3i (matchlen=%i)",
                                        pos, price, ml);
                            assert(pos < LZ4_OPT_NUM);
                            if ( (matchNb == nb_matches-1)  /* last match */
                              && (ml == matchML)  /* last post of last match */
                              && (last_match_pos < pos) )
                                last_match_pos = pos;
                            opt[pos].mlen = ml;
                            opt[pos].off = offset;
                            opt[pos].litlen = ll;
                            opt[pos].price = price;
            }   }   }   }
            /* complete following positions with literals */
            {   int addLit;
                for (addLit = 1; addLit <= 3; addLit ++) {
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
                if ( LZ4HC_encodeSequence(&ip, &op, &anchor, ml, ip - offset, limit, oend) )   /* updates ip, op and anchor */
                    return 0;  /* error */
        }   }
    }  /* while (ip < mflimit) */

    /* Encode Last Literals */
    {   int lastRun = (int)(iend - anchor);
        if ( (limit)
          && (((char*)op - dst) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)dstCapacity))
            return 0;  /* Check output limit */
        if (lastRun >= (int)RUN_MASK) {
            *op++=(RUN_MASK<<ML_BITS);
            lastRun-=RUN_MASK;
            for (; lastRun > 254 ; lastRun-=255) *op++ = 255;
            *op++ = (BYTE) lastRun;
        } else *op++ = (BYTE)(lastRun<<ML_BITS);
        memcpy(op, anchor, iend - anchor);
        op += iend-anchor;
    }

    /* End */
    return (int) ((char*)op-dst);
}
