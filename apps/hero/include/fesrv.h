
#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "libpulp.h"
#include "pulp_common.h"

typedef struct {
  useconds_t pollInterval;
  bool abort;
  char *putCharBuf;
  unsigned putCharIdx;
  unsigned long nCalls;
  useconds_t abortAfter;
  uint32_t coreExited;
  uint32_t exitCode;
  int *exitCodes;
  FILE *logfile;
  FILE *stdout_file;
  volatile struct ring_buf *a2h_rb;
  // required to flush the D$
  pulp_dev_t *dev;
} fesrv_t;

void fesrv_init(fesrv_t *fs, pulp_dev_t *dev, void **a2h_rb_p);
void fesrv_run(fesrv_t *fs);
