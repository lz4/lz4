#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LZ4_STATIC_LINKING_ONLY
#include "lz4.h"
#include "lz4hc.h"
#include "lz4frame.h"
#include "lz4frame_static.h"

#define LZ4F_CHECK(x) { typeof(x) _x = (x); if (LZ4F_isError(_x)) { fprintf(stderr, "Error!: %s\n", LZ4F_getErrorName(_x)); return 0; } }

typedef struct {
  size_t iter;
  LZ4_stream_t *ctx;
  LZ4_streamHC_t *hcctx;
  LZ4F_cctx *cctx;
  LZ4F_dctx *dctx;
  const char *dictbuf;
  size_t dictsize;
  char *obuf;
  size_t osize;
  const char* ibuf;
  size_t isize;
  size_t num_ibuf;
  char *checkbuf;
  size_t checksize;
  int clevel;
  const LZ4F_CDict* cdict;
  LZ4F_preferences_t* prefs;
  const LZ4F_compressOptions_t* options;
} bench_params_t;

size_t compress_frame(bench_params_t *p) {
  size_t iter = p->iter;
  LZ4F_cctx *cctx = p->cctx;
  char *obuf = p->obuf;
  size_t osize = p->osize;
  const char* ibuf = p->ibuf;
  size_t isize = p->isize;
  size_t num_ibuf = p->num_ibuf;
  const LZ4F_CDict* cdict = p->cdict;
  LZ4F_preferences_t* prefs = p->prefs;

  size_t oused;

  prefs->frameInfo.contentSize = isize;

  oused = LZ4F_compressFrame_usingCDict(
    cctx,
    obuf,
    osize,
    ibuf + ((iter * 2654435761U) % num_ibuf) * isize,
    isize,
    cdict,
    prefs);
  LZ4F_CHECK(oused);

  return oused;
}

size_t compress_begin(bench_params_t *p) {
  size_t iter = p->iter;
  LZ4F_cctx *cctx = p->cctx;
  char *obuf = p->obuf;
  size_t osize = p->osize;
  const char* ibuf = p->ibuf;
  size_t isize = p->isize;
  size_t num_ibuf = p->num_ibuf;
  const LZ4F_CDict* cdict = p->cdict;
  LZ4F_preferences_t* prefs = p->prefs;
  const LZ4F_compressOptions_t* options = p->options;

  char *oend = obuf + osize;
  size_t oused;

  prefs->frameInfo.contentSize = isize;

  oused = LZ4F_compressBegin_usingCDict(cctx, obuf, oend - obuf, cdict, prefs);
  LZ4F_CHECK(oused);
  obuf += oused;
  oused = LZ4F_compressUpdate(
    cctx,
    obuf,
    oend - obuf,
    ibuf + ((iter * 2654435761U) % num_ibuf) * isize,
    isize,
    options);
  LZ4F_CHECK(oused);
  obuf += oused;
  oused = LZ4F_compressEnd(cctx, obuf, oend - obuf, options);
  LZ4F_CHECK(oused);

  return obuf - p->obuf;
}

size_t compress_default(bench_params_t *p) {
  size_t iter = p->iter;
  char *obuf = p->obuf;
  size_t osize = p->osize;
  const char* ibuf = p->ibuf;
  size_t isize = p->isize;
  size_t num_ibuf = p->num_ibuf;

  char *oend = obuf + osize;
  size_t oused;

  oused = LZ4_compress_default(
      ibuf + ((iter * 2654435761U) % num_ibuf) * isize, obuf,
      isize, oend - obuf);
  obuf += oused;

  return obuf - p->obuf;
}

size_t compress_extState(bench_params_t *p) {
  size_t iter = p->iter;
  LZ4_stream_t *ctx = p->ctx;
  char *obuf = p->obuf;
  size_t osize = p->osize;
  const char* ibuf = p->ibuf;
  size_t isize = p->isize;
  size_t num_ibuf = p->num_ibuf;
  int clevel = p->clevel;

  char *oend = obuf + osize;
  size_t oused;

  oused = LZ4_compress_fast_extState_fastReset(
      ctx,
      ibuf + ((iter * 2654435761U) % num_ibuf) * isize, obuf,
      isize, oend - obuf, clevel);
  obuf += oused;

  return obuf - p->obuf;
}

size_t compress_hc(bench_params_t *p) {
  size_t iter = p->iter;
  char *obuf = p->obuf;
  size_t osize = p->osize;
  const char* ibuf = p->ibuf;
  size_t isize = p->isize;
  size_t num_ibuf = p->num_ibuf;
  int clevel = p->clevel;

  char *oend = obuf + osize;
  size_t oused;

  oused = LZ4_compress_HC(
      ibuf + ((iter * 2654435761U) % num_ibuf) * isize, obuf,
      isize, oend - obuf, clevel);
  obuf += oused;

  return obuf - p->obuf;
}

size_t compress_hc_extState(bench_params_t *p) {
  size_t iter = p->iter;
  LZ4_streamHC_t *hcctx = p->hcctx;
  char *obuf = p->obuf;
  size_t osize = p->osize;
  const char* ibuf = p->ibuf;
  size_t isize = p->isize;
  size_t num_ibuf = p->num_ibuf;
  int clevel = p->clevel;

  char *oend = obuf + osize;
  size_t oused;

  oused = LZ4_compress_HC_extStateHC(
      hcctx,
      ibuf + ((iter * 2654435761U) % num_ibuf) * isize, obuf,
      isize, oend - obuf, clevel);
  obuf += oused;

  return obuf - p->obuf;
}

size_t check_lz4(bench_params_t *p, size_t csize) {
  (void)csize;
  memset(p->checkbuf, 0xFF, p->checksize);
  return LZ4_decompress_fast_usingDict(p->obuf, p->checkbuf, p->isize,
                                       p->dictbuf, p->dictsize)
      && !memcmp(p->ibuf, p->checkbuf, p->isize);
}

size_t check_lz4f(bench_params_t *p, size_t csize) {
  size_t cp = 0;
  size_t dp = 0;
  size_t dsize = p->checksize;
  size_t cleft = csize;
  size_t dleft = dsize;
  size_t ret;
  memset(p->checkbuf, 0xFF, p->checksize);
  LZ4F_resetDecompressionContext(p->dctx);
  do {
    ret = LZ4F_decompress_usingDict(
        p->dctx, p->checkbuf + dp, &dleft, p->obuf + cp, &cleft,
        p->dictbuf, p->dictsize, NULL);
    cp += cleft;
    dp += dleft;
    cleft = csize - cp;
    dleft = dsize - dp;
    if (LZ4F_isError(ret)) return 0;
  } while (cleft);
  return !memcmp(p->ibuf, p->checkbuf, p->isize);
}


uint64_t bench(
    char *bench_name,
    size_t (*fun)(bench_params_t *),
    size_t (*checkfun)(bench_params_t *, size_t),
    bench_params_t *params
) {
  struct timespec start, end;
  size_t i, osize = 0, o = 0;
  size_t time_taken = 0;
  uint64_t total_repetitions = 0;
  uint64_t repetitions = 2;

  if (clock_gettime(CLOCK_MONOTONIC_RAW, &start)) return 0;

  while (time_taken < 25 * 1000 * 1000) { // benchmark over at least 1ms
    if (total_repetitions) {
      repetitions = total_repetitions; // double previous
    }

    for (i = 0; i < repetitions; i++) {
      params->iter = i;
      o = fun(params);
      if (!o) return 0;
      osize += o;
    }

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &end)) return 0;

    time_taken = (1000 * 1000 * 1000 * end.tv_sec + end.tv_nsec) -
                 (1000 * 1000 * 1000 * start.tv_sec + start.tv_nsec);
    total_repetitions += repetitions;
  }

  o = checkfun(params, o);
  if (!o) return 0;

  fprintf(
      stderr,
      "%-30s @ lvl %2d: %8ld B -> %8ld B, %8ld iters, %10ld ns, %10ld ns/iter, %7.2lf MB/s\n",
      bench_name, params->clevel,
      params->isize, osize / repetitions,
      repetitions, time_taken, time_taken / repetitions,
      ((double) 1000 * params->isize * repetitions) / time_taken
  );

  return time_taken;
}

int main(int argc, char *argv[]) {


  struct stat st;
  size_t bytes_read;

  const char *dict_fn;
  size_t dict_size;
  char *dict_buf;
  FILE *dict_file;

  const char *in_fn;
  size_t in_size;
  size_t num_in_buf;
  size_t cur_in_buf;
  char *in_buf;
  FILE *in_file;

  size_t out_size;
  char *out_buf;

  size_t check_size;
  char *check_buf;

  LZ4_stream_t *ctx;
  LZ4_streamHC_t *hcctx;
  LZ4F_cctx *cctx;
  LZ4F_dctx *dctx;
  LZ4F_CDict *cdict;
  LZ4F_preferences_t prefs;
  LZ4F_compressOptions_t options;

  int clevels[] = {1, 2, 3, 6, 9, 10, 12};

  bench_params_t params;

  if (argc != 3) return 1;
  dict_fn = argv[1];
  in_fn = argv[2];

  if (stat(dict_fn, &st)) return 1;
  dict_size = st.st_size;
  dict_buf = (char *)malloc(dict_size);
  if (!dict_buf) return 1;
  dict_file = fopen(dict_fn, "r");
  bytes_read = fread(dict_buf, 1, dict_size, dict_file);
  if (bytes_read != dict_size) return 1;

  if (stat(in_fn, &st)) return 1;
  in_size = st.st_size;
  num_in_buf = 256 * 1024 * 1024 / in_size;
  if (num_in_buf == 0) {
    num_in_buf = 1;
  }

  in_buf = (char *)malloc(in_size * num_in_buf);
  if (!in_buf) return 1;
  in_file = fopen(in_fn, "r");
  bytes_read = fread(in_buf, 1, in_size, in_file);
  if (bytes_read != in_size) return 1;

  for(cur_in_buf = 1; cur_in_buf < num_in_buf; cur_in_buf++) {
    memcpy(in_buf + cur_in_buf * in_size, in_buf, in_size);
  }

  check_size = in_size;
  check_buf = (char *)malloc(check_size);
  if (!check_buf) return 1;

  memset(&prefs, 0, sizeof(prefs));
  prefs.autoFlush = 1;
  if (in_size < 64 * 1024)
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
  prefs.frameInfo.contentSize = in_size;

  memset(&options, 0, sizeof(options));
  options.stableSrc = 1;

  out_size = LZ4F_compressFrameBound(in_size, &prefs);
  out_buf = (char *)malloc(out_size);
  if (!out_buf) return 1;

  if (LZ4F_isError(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION))) return 1;
  if (cctx == NULL) return 1;

  if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) return 1;
  if (cctx == NULL) return 1;

  ctx = LZ4_createStream();
  if (ctx == NULL) return 1;

  hcctx = LZ4_createStreamHC();
  if (hcctx == NULL) return 1;

  cdict = LZ4F_createCDict(dict_buf, dict_size);
  if (!cdict) return 1;

  fprintf(stderr, "dict  size: %zd\n", dict_size);
  fprintf(stderr, "input size: %zd\n", in_size);

  params.ctx = ctx;
  params.hcctx = hcctx;
  params.cctx = cctx;
  params.dctx = dctx;
  params.dictbuf = dict_buf;
  params.dictsize = dict_size;
  params.obuf = out_buf;
  params.osize = out_size;
  params.ibuf = in_buf;
  params.isize = in_size;
  params.num_ibuf = num_in_buf;
  params.checkbuf = check_buf;
  params.checksize = check_size;
  params.clevel = 1;
  params.cdict = NULL;
  params.prefs = &prefs;
  params.options = &options;

  for (unsigned int clevelidx = 0; clevelidx < sizeof(clevels) / sizeof(clevels[0]); clevelidx++) {
    params.clevel = clevels[clevelidx];
    params.prefs->compressionLevel = clevels[clevelidx];
    params.cdict = NULL;

    bench("LZ4_compress_default"         , compress_default    , check_lz4 , &params);
    bench("LZ4_compress_fast_extState"   , compress_extState   , check_lz4 , &params);
    bench("LZ4_compress_HC"              , compress_hc         , check_lz4 , &params);
    bench("LZ4_compress_HC_extStateHC"   , compress_hc_extState, check_lz4 , &params);
    bench("LZ4F_compressFrame"           , compress_frame      , check_lz4f, &params);
    bench("LZ4F_compressBegin"           , compress_begin      , check_lz4f, &params);

    params.cdict = cdict;

    bench("LZ4F_compressFrame_usingCDict", compress_frame      , check_lz4f, &params);
    bench("LZ4F_compressBegin_usingCDict", compress_begin      , check_lz4f, &params);
  }

  return 0;
}
