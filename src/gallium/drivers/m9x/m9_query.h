/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_QUERY_H
#define M9_QUERY_H

#include "m9_internal.h"

/* Flushes the pending job, so a record emitted after it runs behind those
 * draws; defined beside the job builder in m9_context.c. */
int m9_context_flush_job(struct pipe_context *pctx);

void m9_query_init(struct pipe_context *pctx);
/* Emit the snapshot records a job's stream opens and closes with. */
int m9_query_job_prologue(struct m9_context *ctx, struct m9_batch *batch);
int m9_query_job_epilogue(struct m9_context *ctx, struct m9_batch *batch,
                          uint32_t token);
/* Keep the samples of the jobs in between out of the active query. */
void m9_query_pause(struct m9_context *ctx);
void m9_query_resume(struct m9_context *ctx);
void m9_query_pause_submitted(struct m9_context *ctx);
void m9_query_resume_submitted(struct m9_context *ctx);

#endif
