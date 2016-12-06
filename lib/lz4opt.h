/*
    lz4opt.h - Optimal Mode of LZ4
    Copyright (C) 2015-2016, Przemyslaw Skibinski <inikep@gmail.com>
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

#define LZ4_LOG_PARSER(fmt, ...) //printf(fmt, __VA_ARGS__) 
#define LZ4_LOG_PRICE(fmt, ...) //printf(fmt, __VA_ARGS__) 
#define LZ4_LOG_ENCODE(fmt, ...) //printf(fmt, __VA_ARGS__) 


#define LZ4_OPT_NUM   (1<<12) 

typedef struct
{
	int off;
	int len;
	int back;
} LZ4HC_match_t;

typedef struct
{
	int price;
	int off;
	int mlen;
	int litlen;
} LZ4HC_optimal_t;


FORCE_INLINE size_t LZ4_LIT_ONLY_COST(size_t litlen)
{
    size_t price = 8*litlen;
    if (litlen>=(int)RUN_MASK) { litlen-=RUN_MASK; price+=8*(1+litlen/255); }
    return price;
}

FORCE_INLINE size_t LZ4HC_get_price(size_t litlen, size_t mlen)
{
    size_t price = 16 + 8; /* 16-bit offset + token */

    price += 8*litlen;
    if (litlen>=(int)RUN_MASK) { litlen-=RUN_MASK; price+=8*(1+litlen/255); }

  //  mlen-=MINMATCH;
    if (mlen>=(int)ML_MASK) { mlen-=ML_MASK; price+=8*(1+mlen/255); }

    return price;
}


FORCE_INLINE int LZ4HC_GetAllMatches (
    LZ4HC_CCtx_internal* ctx,
    const BYTE* const ip,
    const BYTE* const iLowLimit,
    const BYTE* const iHighLimit,
    size_t best_mlen,
    LZ4HC_match_t* matches)
{
    U16* const chainTable = ctx->chainTable;
    U32* const HashTable = ctx->hashTable;
    const BYTE* const base = ctx->base;
    const U32 dictLimit = ctx->dictLimit;
    const BYTE* const lowPrefixPtr = base + dictLimit;
    const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + MAX_DISTANCE > current) ? ctx->lowLimit : current - (MAX_DISTANCE - 1);
    const BYTE* const dictBase = ctx->dictBase;
    const BYTE* match;
    U32   matchIndex;
    int nbAttempts = ctx->searchNum;
    int mnum = 0;
    U32* HashPos;

    if (ip + MINMATCH > iHighLimit) return 0;

    /* First Match */
    HashPos = &HashTable[LZ4HC_hashPtr(ip)];
    matchIndex = *HashPos;


    DELTANEXTU16(current) = (U16)(current - matchIndex);
    *HashPos =  current;
    ctx->nextToUpdate++;


    while ((matchIndex < current) && (matchIndex>=lowLimit) && (nbAttempts))
    {
        nbAttempts--;
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;

            if ((ip[best_mlen] == match[best_mlen]) && (LZ4_read32(match) == LZ4_read32(ip)))
            {
                size_t mlt = MINMATCH + LZ4_count(ip+MINMATCH, match+MINMATCH, iHighLimit);
                int back = 0;

                while ((ip+back>iLowLimit)
                       && (match+back > lowPrefixPtr)
                       && (ip[back-1] == match[back-1]))
                        back--;

                mlt -= back;

                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = -back;
                    mnum++;
                }

                if (best_mlen > LZ4_OPT_NUM) break;
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (LZ4_read32(match) == LZ4_read32(ip))
            {
                size_t mlt;
                int back=0;
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iHighLimit) vLimit = iHighLimit;
                mlt = LZ4_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                    mlt += LZ4_count(ip+mlt, base+dictLimit, iHighLimit);
                while ((ip+back > iLowLimit) && (matchIndex+back > lowLimit) && (ip[back-1] == match[back-1])) back--;
                mlt -= back;
                
                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = -back;
                    mnum++;
                }

                if (best_mlen > LZ4_OPT_NUM) break;
            }
        }
        matchIndex -= DELTANEXTU16(matchIndex);
    }


    return mnum;
}



FORCE_INLINE int LZ4HC_BinTree_GetAllMatches (
    LZ4HC_CCtx_internal* ctx,
    const BYTE* const ip,
    const BYTE* const iHighLimit,
    size_t best_mlen,
    LZ4HC_match_t* matches)
{
    U16* const chainTable = ctx->chainTable;
    U32* const HashTable = ctx->hashTable;
    const BYTE* const base = ctx->base;
    const U32 dictLimit = ctx->dictLimit;
    const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + MAX_DISTANCE > current) ? ctx->lowLimit : current - (MAX_DISTANCE - 1);
    const BYTE* const dictBase = ctx->dictBase;
    const BYTE* match;
    int nbAttempts = ctx->searchNum;
    int mnum = 0;
    U16 *ptr0, *ptr1;
    U32 matchIndex, delta0, delta1;
    size_t mlt = 0;
    U32* HashPos;
    
    if (ip + MINMATCH > iHighLimit) return 0;

    /* First Match */
    HashPos = &HashTable[LZ4HC_hashPtr(ip)];
    matchIndex = *HashPos;

    *HashPos = current;
    ctx->nextToUpdate++;

    // check rest of matches
    ptr0 = &DELTANEXTMAXD(current*2+1);
    ptr1 = &DELTANEXTMAXD(current*2);
    delta0 = delta1 = current - matchIndex;

    while ((matchIndex < current) && (matchIndex>=lowLimit) && (nbAttempts))
    {
        nbAttempts--;
        mlt = 0;
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;

            if (LZ4_read32(match) == LZ4_read32(ip))
            {
                mlt = MINMATCH + LZ4_count(ip+MINMATCH, match+MINMATCH, iHighLimit);

                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = 0;
                    mnum++;
                }

                if (best_mlen > LZ4_OPT_NUM) break;
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (LZ4_read32(match) == LZ4_read32(ip))
            {
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iHighLimit) vLimit = iHighLimit;
                mlt = LZ4_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                    mlt += LZ4_count(ip+mlt, base+dictLimit, iHighLimit);
                
                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = 0;
                    mnum++;
                }

                if (best_mlen > LZ4_OPT_NUM) break;
            }
        }
        
        if (*(ip+mlt) < *(match+mlt))
        {
            *ptr0 = delta0;
            ptr0 = &DELTANEXTMAXD(matchIndex*2);
    //		printf("delta0=%d\n", delta0);
            if (*ptr0 == (U16)-1) break;
            delta0 = *ptr0;
            delta1 += delta0;
            matchIndex -= delta0;
        }
        else
        {
            *ptr1 = delta1;
            ptr1 = &DELTANEXTMAXD(matchIndex*2+1);
    //		printf("delta1=%d\n", delta1);
            if (*ptr1 == (U16)-1) break;
            delta1 = *ptr1;
            delta0 += delta1;
            matchIndex -= delta1;
        }
    }

    *ptr0 = (U16)-1;
    *ptr1 = (U16)-1;

    return mnum;
}





#define SET_PRICE(pos, mlen, offset, litlen, price)   \
    {                                                 \
        while (last_pos < pos)  { opt[last_pos+1].price = 1<<30; last_pos++; } \
        opt[pos].mlen = (int)mlen;                         \
        opt[pos].off = (int)offset;                        \
        opt[pos].litlen = (int)litlen;                     \
        opt[pos].price = (int)price;                       \
        LZ4_LOG_PARSER("%d: SET price[%d/%d]=%d litlen=%d len=%d off=%d\n", (int)(inr-source), pos, last_pos, opt[pos].price, opt[pos].litlen, opt[pos].mlen, opt[pos].off); \
    }


static int LZ4HC_compress_optimal (
    LZ4HC_CCtx_internal* ctx,
    const char* const source,
    char* dest,
    int inputSize,
    int maxOutputSize,
    limitedOutput_directive limit,
    const int binaryTreeFinder,
    const size_t sufficient_len
    )
{
	LZ4HC_optimal_t opt[LZ4_OPT_NUM + 4];
	LZ4HC_match_t matches[LZ4_OPT_NUM + 1];
	const BYTE *inr;
	size_t res, cur, cur2;
	size_t i, llen, litlen, mlen, best_mlen, price, offset, best_off, match_num, last_pos;

    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = (iend - LASTLITERALS);
    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + maxOutputSize;

    /* init */
    ctx->end += inputSize;
    ip++;

    /* Main Loop */
    while (ip < mflimit)
    {
        memset(opt, 0, sizeof(LZ4HC_optimal_t));
        last_pos = 0;
        llen = ip - anchor;

        best_mlen = MINMATCH-1;

        if (!binaryTreeFinder)
        {
            LZ4HC_Insert(ctx, ip);
            match_num = LZ4HC_GetAllMatches(ctx, ip, ip, matchlimit, best_mlen, matches);
        }
        else
        {
            match_num = LZ4HC_BinTree_GetAllMatches(ctx, ip, matchlimit, best_mlen, matches);
        }

       LZ4_LOG_PARSER("%d: match_num=%d last_pos=%d\n", (int)(ip-source), match_num, last_pos);
       if (!last_pos && !match_num) { ip++; continue; }

       if (match_num && (size_t)matches[match_num-1].len > sufficient_len)
       {
            best_mlen = matches[match_num-1].len;
            best_off = matches[match_num-1].off;
            cur = 0;
            last_pos = 1;
            goto encode;
       }

       // set prices using matches at position = 0
       for (i = 0; i < match_num; i++)
       {
           mlen = (i>0) ? (size_t)matches[i-1].len+1 : best_mlen;
           best_mlen = (matches[i].len < LZ4_OPT_NUM) ? matches[i].len : LZ4_OPT_NUM;
           LZ4_LOG_PARSER("%d: start Found mlen=%d off=%d best_mlen=%d last_pos=%d\n", (int)(ip-source), matches[i].len, matches[i].off, best_mlen, last_pos);
           while (mlen <= best_mlen)
           {
                litlen = 0;
                price = LZ4HC_get_price(llen + litlen, mlen - MINMATCH) - llen;
                if (mlen > last_pos || price < (size_t)opt[mlen].price)
                    SET_PRICE(mlen, mlen, matches[i].off, litlen, price);
                mlen++;
           }
        }

        if (last_pos < MINMATCH) { ip++; continue; }

        opt[0].mlen = opt[1].mlen = 1;

        // check further positions
        for (cur = 1; cur <= last_pos; cur++)
        { 
           inr = ip + cur;

           if (opt[cur-1].mlen == 1)
           {
                litlen = opt[cur-1].litlen + 1;
                
                if (cur != litlen)
                {
                    price = opt[cur - litlen].price + LZ4_LIT_ONLY_COST(litlen);
                    LZ4_LOG_PRICE("%d: TRY1 opt[%d].price=%d price=%d cur=%d litlen=%d\n", (int)(inr-source), cur - litlen, opt[cur - litlen].price, price, cur, litlen);
                }
                else
                {
                    price = LZ4_LIT_ONLY_COST(llen + litlen) - llen;
                    LZ4_LOG_PRICE("%d: TRY2 price=%d cur=%d litlen=%d llen=%d\n", (int)(inr-source), price, cur, litlen, llen);
                }
           }
           else
           {
                litlen = 1;
                price = opt[cur - 1].price + LZ4_LIT_ONLY_COST(litlen);                  
                LZ4_LOG_PRICE("%d: TRY3 price=%d cur=%d litlen=%d litonly=%d\n", (int)(inr-source), price, cur, litlen, LZ4_LIT_ONLY_COST(litlen));
           }
           
           mlen = 1;
           best_mlen = 0;
           LZ4_LOG_PARSER("%d: TRY price=%d opt[%d].price=%d\n", (int)(inr-source), price, cur, opt[cur].price);

           if (cur > last_pos || price <= (size_t)opt[cur].price) // || ((price == opt[cur].price) && (opt[cur-1].mlen == 1) && (cur != litlen)))
                SET_PRICE(cur, mlen, best_mlen, litlen, price);

           if (cur == last_pos) break;

            LZ4_LOG_PARSER("%d: CURRENT price[%d/%d]=%d off=%d mlen=%d litlen=%d\n", (int)(inr-source), cur, last_pos, opt[cur].price, opt[cur].off, opt[cur].mlen, opt[cur].litlen); 

            best_mlen = (best_mlen > MINMATCH) ? best_mlen : (MINMATCH-1);

            if (!binaryTreeFinder)
            {
                LZ4HC_Insert(ctx, inr);
                match_num = LZ4HC_GetAllMatches(ctx, inr, ip, matchlimit, best_mlen, matches);
                LZ4_LOG_PARSER("%d: LZ4HC_GetAllMatches match_num=%d\n", (int)(inr-source), match_num);
            }
            else
            {
                match_num = LZ4HC_BinTree_GetAllMatches(ctx, inr, matchlimit, best_mlen, matches);
                LZ4_LOG_PARSER("%d: LZ4HC_BinTree_GetAllMatches match_num=%d\n", (int)(inr-source), match_num);
            }


            if (match_num > 0 && (size_t)matches[match_num-1].len > sufficient_len)
            {
                cur -= matches[match_num-1].back;
                best_mlen = matches[match_num-1].len;
                best_off = matches[match_num-1].off;
                last_pos = cur + 1;
                goto encode;
            }

            // set prices using matches at position = cur
            for (i = 0; i < match_num; i++)
            {
                mlen = (i>0) ? (size_t)matches[i-1].len+1 : best_mlen;
                cur2 = cur - matches[i].back;
                best_mlen = (cur2 + matches[i].len < LZ4_OPT_NUM) ? (size_t)matches[i].len : LZ4_OPT_NUM - cur2;
                LZ4_LOG_PARSER("%d: Found1 cur=%d cur2=%d mlen=%d off=%d best_mlen=%d last_pos=%d\n", (int)(inr-source), cur, cur2, matches[i].len, matches[i].off, best_mlen, last_pos);

                if (mlen < (size_t)matches[i].back + 1)
                    mlen = matches[i].back + 1; 

                while (mlen <= best_mlen)
                {
                    if (opt[cur2].mlen == 1)
                    {
                        litlen = opt[cur2].litlen;

                        if (cur2 != litlen)
                            price = opt[cur2 - litlen].price + LZ4HC_get_price(litlen, mlen - MINMATCH);
                        else
                            price = LZ4HC_get_price(llen + litlen, mlen - MINMATCH) - llen;
                    }
                    else
                    {
                        litlen = 0;
                        price = opt[cur2].price + LZ4HC_get_price(litlen, mlen - MINMATCH);
                    }

                    LZ4_LOG_PARSER("%d: Found2 pred=%d mlen=%d best_mlen=%d off=%d price=%d litlen=%d price[%d]=%d\n", (int)(inr-source), matches[i].back, mlen, best_mlen, matches[i].off, price, litlen, cur - litlen, opt[cur - litlen].price);
    //                if (cur2 + mlen > last_pos || ((matches[i].off != opt[cur2 + mlen].off) && (price < opt[cur2 + mlen].price)))
                    if (cur2 + mlen > last_pos || price < (size_t)opt[cur2 + mlen].price)
                    {
                        SET_PRICE(cur2 + mlen, mlen, matches[i].off, litlen, price);
                    }

                    mlen++;
                }
            }
        } //  for (cur = 1; cur <= last_pos; cur++)


        best_mlen = opt[last_pos].mlen;
        best_off = opt[last_pos].off;
        cur = last_pos - best_mlen;

encode: // cur, last_pos, best_mlen, best_off have to be set
        for (i = 1; i <= last_pos; i++)
        {
            LZ4_LOG_PARSER("%d: price[%d/%d]=%d off=%d mlen=%d litlen=%d\n", (int)(ip-source+i), i, last_pos, opt[i].price, opt[i].off, opt[i].mlen, opt[i].litlen); 
        }

        LZ4_LOG_PARSER("%d: cur=%d/%d best_mlen=%d best_off=%d\n", (int)(ip-source+cur), cur, last_pos, best_mlen, best_off); 

        opt[0].mlen = 1;
        
        while (1)
        {
            mlen = opt[cur].mlen;
            offset = opt[cur].off;
            opt[cur].mlen = (int)best_mlen; 
            opt[cur].off = (int)best_off;
            best_mlen = mlen;
            best_off = offset;
            if (mlen > cur) break;
            cur -= mlen;
        }
          
        for (i = 0; i <= last_pos;)
        {
            LZ4_LOG_PARSER("%d: price2[%d/%d]=%d off=%d mlen=%d litlen=%d\n", (int)(ip-source+i), i, last_pos, opt[i].price, opt[i].off, opt[i].mlen, opt[i].litlen); 
            i += opt[i].mlen;
        }

        cur = 0;

        while (cur < last_pos)
        {
            LZ4_LOG_PARSER("%d: price3[%d/%d]=%d off=%d mlen=%d litlen=%d\n", (int)(ip-source+cur), cur, last_pos, opt[cur].price, opt[cur].off, opt[cur].mlen, opt[cur].litlen); 
            mlen = opt[cur].mlen;
            if (mlen == 1) { ip++; cur++; continue; }
            offset = opt[cur].off;
            cur += mlen;

            LZ4_LOG_ENCODE("%d: ENCODE literals=%d off=%d mlen=%d ", (int)(ip-source), (int)(ip-anchor), (int)(offset), mlen);
            res = LZ4HC_encodeSequence(&ip, &op, &anchor, (int)mlen, ip - offset, limit, oend);
            LZ4_LOG_ENCODE("out=%d\n", (int)((char*)op - dest));

            if (res) return 0; 

            LZ4_LOG_PARSER("%d: offset=%d\n", (int)(ip-source), offset);
        }
    }

    /* Encode Last Literals */
    {
        int lastRun = (int)(iend - anchor);
    //    if (inputSize > LASTLITERALS && lastRun < LASTLITERALS) { printf("ERROR: lastRun=%d\n", lastRun); }
        if ((limit) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize)) return 0;  /* Check output limit */
        if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
        else *op++ = (BYTE)(lastRun<<ML_BITS);
        LZ4_LOG_ENCODE("%d: ENCODE_LAST literals=%d out=%d\n", (int)(ip-source), (int)(iend-anchor), (int)((char*)op -dest));
        memcpy(op, anchor, iend - anchor);
        op += iend-anchor;
    }

    /* End */
    return (int) ((char*)op-dest);
}

