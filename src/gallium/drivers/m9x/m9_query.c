/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Occlusion queries.
 *
 * The engine publishes a counter pair to memory when it runs the indexed
 * record `1e=<address>, e6=0x64000004, e6=0x64000101, e6=0x40008000`
 * (CAPTURED, the begin and end packets of the vendor's query cases in
 * captures/m9x-linux-gl2-core-results-20260914).  The counter it publishes
 * is a free-running one the context shares, and it advances only while a job
 * states the a7/c6 enable pair, so a range is measured by enabling it in
 * every job of the range and taking the unsigned 32-bit difference of the
 * readings in front of and behind the draws.  That difference is the whole of
 * the vendor ICD's own result path and agrees with the 32 counter bits it
 * advertises; core Mesa reports 64 for GL_QUERY_COUNTER_BITS, so a range
 * passing more than 2^32 samples is the one case this cannot describe.
 *
 * The records travel in the job stream itself rather than in jobs of their
 * own, and the closing one waits on a dependency token the job raises when
 * its draws have retired: without that wait it publishes a partial count.
 *
 * A snapshot is reset to the pending marker before its record goes out and is
 * read back straight from the mapping, so availability costs no ioctl and a
 * result is waited on only when the application asks for one.  A counter
 * reading that is itself the marker would look pending, which is the vendor's
 * own contract and bounded here by the blocking wait's deadline.
 *
 * The kernel validator has to admit index 1e, and admits it only inside a
 * packet whose other three indices carry the captured literals (kernel
 * 23fd309).  A module without that refuses the record, and the refusal
 * surfaces here as a submit failing with -EPERM and the context lost, with
 * nothing in dmesg -- occvis, occpart and occhid all failed that way against
 * a stale module on 2026-09-18, and passed against a current one.
 *
 * -EPERM with a clean dmesg is worth reading as a signature rather than as a
 * driver fault: the kernel's own checks refused what was asked for, so the
 * engine never saw it.  A hardware failure announces itself in dmesg as a
 * stall or a stream timeout instead.  When a job stops working after a module
 * swap, check which module is loaded before suspecting the stream.
 */
#include "m9_query.h"

#include <sched.h>
#include <stdio.h>

#include "m9_batch.h"
#include "m9_screen.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

/* Offsets of the two snapshots inside one query object.  The vendor aligns
 * and sizes a snapshot by 0x20 (CAPTURED), which is what the kernel demands
 * of a query relocation. */
#define M9_QUERY_SLOT_BYTES 0x20u
#define M9_QUERY_BEGIN 0u
#define M9_QUERY_END M9_QUERY_SLOT_BYTES
#define M9_QUERY_OBJECT_BYTES 4096u
/* The value a snapshot carries while the engine has not published it
 * (CAPTURED: the vendor resets both dwords of the selected pair to this
 * before queueing the record). */
#define M9_QUERY_PENDING 0xffffffffu

/* How long a blocking result waits for the engine before giving up.  A job
 * that has not retired by then has stalled, and reporting the query
 * unavailable keeps the application running instead of spinning for ever. */
#define M9_QUERY_WAIT_NS (2ull * 1000 * 1000 * 1000)

struct m9_query {
   enum pipe_query_type type;
   struct m9_resource bo;
   bool active;
   bool ended;
   bool have_result;
   bool failed;
   bool begin_emitted, end_emitted;
   uint32_t count;
};

static bool m9_query_debug;

static struct m9_query *
m9_query(struct pipe_query *pq)
{
   return (struct m9_query *)pq;
}

static bool
m9_query_supported(unsigned type)
{
   return type == PIPE_QUERY_OCCLUSION_COUNTER ||
          type == PIPE_QUERY_OCCLUSION_PREDICATE ||
          type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE;
}

static volatile uint32_t *
m9_query_slot(const struct m9_query *q, uint64_t offset)
{
   return (volatile uint32_t *)((uint8_t *)q->bo.map + offset);
}

static void
m9_query_reset_slot(struct m9_query *q, uint64_t offset)
{
   volatile uint32_t *slot = m9_query_slot(q, offset);

   for (unsigned i = 0; i < M9_QUERY_SLOT_BYTES / sizeof(uint32_t); i++)
      slot[i] = M9_QUERY_PENDING;
   __sync_synchronize();
}

static bool
m9_query_slot_published(const struct m9_query *q, uint64_t offset)
{
   __sync_synchronize();
   return m9_query_slot(q, offset)[0] != M9_QUERY_PENDING;
}

/*
 * A snapshot record, framed the way the vendor frames one (CAPTURED): the
 * open block is ended and raises a dependency token, the record waits on that
 * token, and only then publishes.  Without the wait the record runs while the
 * draws in front of it are still rasterising and publishes a partial count
 * (MEASURED: 5919, 5578 and 5297 for the same 7000-sample draw).  A token of
 * zero is the one the job's own first range raises, so it is emitted
 * unframed -- which is what the record in front of a job's draws wants.
 */
static int
m9_query_emit_one(struct m9_batch *batch, struct m9_query *q, uint64_t offset,
                  uint32_t token)
{
   const struct m9_packet_value block_end[] = {
      { .index = 0xe6, .value = 3 },
   };
   const struct m9_packet_value raise[] = {
      { .index = 0xe6, .value = 3 }, { .index = 0xe3, .value = token },
   };
   const struct m9_packet_value wait[] = {
      { .index = 0x43, .value = token },
   };
   const struct m9_packet_value record[] = {
      { .index = 0x1e, .handle = q->bo.handle, .offset = offset,
        .kind = PARHELIA_M9_RELOC_QUERY },
      { .index = 0xe6, .value = 0x64000004 },
      { .index = 0xe6, .value = 0x64000101 },
      { .index = 0xe6, .value = 0x40008000 },
   };
   int ret;

   if (token) {
      ret = m9_batch_emit_packet(batch, raise, ARRAY_SIZE(raise));
      if (!ret)
         ret = m9_batch_emit_packet(batch, wait, ARRAY_SIZE(wait));
   } else {
      ret = m9_batch_emit_packet(batch, block_end, ARRAY_SIZE(block_end));
   }
   if (!ret)
      ret = m9_batch_emit_packet(batch, record, ARRAY_SIZE(record));
   return ret;
}

/*
 * The counter is off until the a7/c6 pair names it.  The vendor states this
 * pair inside every range of a job that carries a query and inside no other
 * (CAPTURED, all four query cases of
 * captures/m9x-linux-gl2-core-results-20260914 and none of the seven others);
 * with the pair the record publishes the exact sample count and without it a
 * zero (MEASURED, both on the M9128).  a5, d3 and the 2e/2f block the same
 * capture carries are not needed for the count and are not emitted.
 */
static int
m9_query_enable_counter(struct m9_batch *batch)
{
   const struct m9_packet_value enable[] = {
      { .index = 0xa7, .value = 0x00ffff7f },
      { .index = 0xc6, .value = 0x00ffff09 },
   };

   return m9_batch_emit_packet(batch, enable, ARRAY_SIZE(enable));
}

/*
 * The counter is a free-running one the context shares (MEASURED: a begin
 * snapshot reads a value that grows over the life of a context, so a result
 * is the difference of the two readings and never one of them).  It counts
 * only while a job states the enable pair, so every job of a query's range
 * states it, and the records go into the stream itself: one in front of the
 * first job's draws and one behind the last job's, where a dependency token
 * makes it wait for them.
 */
int
m9_query_job_prologue(struct m9_context *ctx, struct m9_batch *batch)
{
   struct m9_query *q = ctx->query_begin;
   int ret;

   if (!ctx->query_active)
      return 0;
   ret = m9_query_enable_counter(batch);
   if (!ret && q)
      ret = m9_query_emit_one(batch, q, M9_QUERY_BEGIN, 0);
   if (ret)
      return ret;
   if (q) {
      ctx->query_begin = NULL;
      q->begin_emitted = true;
   }
   return 0;
}

int
m9_query_job_epilogue(struct m9_context *ctx, struct m9_batch *batch,
                      uint32_t token)
{
   struct m9_query *q = ctx->query_end;
   int ret;

   if (!q)
      return 0;
   ret = m9_query_emit_one(batch, q, M9_QUERY_END, token);
   if (ret)
      return ret;
   ctx->query_end = NULL;
   q->end_emitted = true;
   return 0;
}

/*
 * A range whose last draws went out in an earlier job leaves no job for the
 * closing record, so it is submitted on its own.  The job before it has
 * retired -- the kernel drains the posted one before it builds the next --
 * so this record needs no dependency token of its own.
 */
static int
m9_query_close_alone(struct m9_context *ctx, struct m9_query *q)
{
   struct m9_batch *batch = &ctx->batch;
   uint64_t point = 0;
   int ret;

   m9_batch_init(batch);
   ret = m9_query_enable_counter(batch);
   if (!ret && !q->begin_emitted)
      ret = m9_query_emit_one(batch, q, M9_QUERY_BEGIN, 0);
   if (!ret)
      ret = m9_query_emit_one(batch, q, M9_QUERY_END, 0);
   if (ret) {
      m9_batch_init(batch);
      return ret;
   }
   batch->sealed = true;
   ret = ctx->dry_run ?
         m9_batch_validate(ctx->screen, &ctx->command, batch,
                           ctx->context_id) :
         m9_batch_submit(ctx->screen, &ctx->command, batch, ctx->context_id,
                         &point);
   if (!ret && !ctx->dry_run)
      ctx->last_point = point;
   m9_batch_init(batch);
   if (ret)
      return ret;
   ctx->query_begin = NULL;
   q->begin_emitted = q->end_emitted = true;
   return 0;
}

static struct pipe_query *
m9_create_query(struct pipe_context *pctx, unsigned query_type, unsigned index)
{
   struct m9_context *ctx = (struct m9_context *)pctx;
   struct m9_query *q;

   (void)index;
   if (!m9_query_supported(query_type))
      return NULL;
   q = CALLOC_STRUCT(m9_query);
   if (!q)
      return NULL;
   q->type = query_type;
   if (m9_bo_create(ctx->screen, M9_QUERY_OBJECT_BYTES, PARHELIA_M9_BO_VRAM,
                    M9_RESOURCE_LINEAR_BUFFER, &q->bo) || !q->bo.map) {
      FREE(q);
      return NULL;
   }
   m9_query_reset_slot(q, M9_QUERY_BEGIN);
   m9_query_reset_slot(q, M9_QUERY_END);
   return (struct pipe_query *)q;
}

static void
m9_destroy_query(struct pipe_context *pctx, struct pipe_query *pq)
{
   struct m9_context *ctx = (struct m9_context *)pctx;
   struct m9_query *q = m9_query(pq);

   if (!q)
      return;
   m9_bo_destroy(ctx->screen, &q->bo);
   FREE(q);
}

static bool
m9_begin_query(struct pipe_context *pctx, struct pipe_query *pq)
{
   struct m9_context *ctx = (struct m9_context *)pctx;
   struct m9_query *q = m9_query(pq);

   q->active = q->ended = q->have_result = q->failed = false;
   q->begin_emitted = q->end_emitted = false;
   q->count = 0;
   /* Reuse starts from a clean pair: a stale published value would read as
    * an available result the moment the object is asked for one. */
   m9_query_reset_slot(q, M9_QUERY_BEGIN);
   m9_query_reset_slot(q, M9_QUERY_END);
   /* Earlier draws are not this query's: they go out in their own job, and
    * the record leads the job that opens with this query's first draw. */
   if (m9_context_flush_job(pctx)) {
      q->failed = true;
      return false;
   }
   ctx->query_begin = q;
   ctx->query_active = q;
   ctx->query_end = NULL;
   q->active = true;
   return true;
}

static bool
m9_end_query(struct pipe_context *pctx, struct pipe_query *pq)
{
   struct m9_context *ctx = (struct m9_context *)pctx;
   struct m9_query *q = m9_query(pq);

   if (!q->active)
      return false;
   q->active = false;
   ctx->query_end = q;
   /* Closes the job the records bracket -- the same job still carries the
    * begin record when every draw of the range went into it. */
   if (m9_context_flush_job(pctx)) {
      ctx->query_end = NULL;
      q->failed = true;
      return false;
   }
   ctx->query_end = NULL;
   ctx->query_active = NULL;
   /* Nothing was pending, so no job carried the closing record. */
   if (!q->end_emitted && m9_query_close_alone(ctx, q)) {
      q->failed = true;
      return false;
   }
   q->ended = true;
   return true;
}

static bool
m9_query_available(struct m9_query *q)
{
   return m9_query_slot_published(q, M9_QUERY_BEGIN) &&
          m9_query_slot_published(q, M9_QUERY_END);
}

static void
m9_query_collect(struct m9_query *q)
{
   uint32_t begin = m9_query_slot(q, M9_QUERY_BEGIN)[0];
   uint32_t end = m9_query_slot(q, M9_QUERY_END)[0];

   /* The counter is 32 bits wide and free running, so the samples a range
    * passed is the unsigned wrapping difference, not the raw reading. */
   q->count = end - begin;
   q->have_result = true;
   if (m9_query_debug)
      fprintf(stderr, "m9x: query begin %08x/%08x end %08x/%08x count %u\n",
              begin, m9_query_slot(q, M9_QUERY_BEGIN)[1], end,
              m9_query_slot(q, M9_QUERY_END)[1], q->count);
}

static bool
m9_get_query_result(struct pipe_context *pctx, struct pipe_query *pq,
                    bool wait, union pipe_query_result *result)
{
   struct m9_context *ctx = (struct m9_context *)pctx;
   struct m9_query *q = m9_query(pq);

   if (!q->have_result) {
      if (q->failed || !q->ended)
         return false;
      /* A range with no draw in it emitted no record, and passed no
       * samples. */
      if (!q->begin_emitted || !q->end_emitted) {
         q->count = 0;
         q->have_result = true;
      } else if (!m9_query_available(q)) {
         int64_t deadline;

         if (!wait)
            return false;
         deadline = os_time_get_nano() + M9_QUERY_WAIT_NS;
         while (!m9_query_available(q)) {
            if (os_time_get_nano() > deadline) {
               fprintf(stderr, "m9x: an occlusion query did not retire\n");
               ctx->lost = true;
               return false;
            }
            sched_yield();
         }
         m9_query_collect(q);
      } else {
         m9_query_collect(q);
      }
   }
   if (q->type == PIPE_QUERY_OCCLUSION_COUNTER)
      result->u64 = q->count;
   else
      result->b = q->count != 0;
   return true;
}

void
m9_query_init(struct pipe_context *pctx)
{
   m9_query_debug = debug_get_bool_option("M9X_QUERY_DEBUG", false);
   pctx->create_query = m9_create_query;
   pctx->destroy_query = m9_destroy_query;
   pctx->begin_query = m9_begin_query;
   pctx->end_query = m9_end_query;
   pctx->get_query_result = m9_get_query_result;
}
