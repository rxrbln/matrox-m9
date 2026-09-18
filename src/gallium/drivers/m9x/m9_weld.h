/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Welding an unindexed draw into an indexed one.  An application that draws a
 * mesh with glDrawArrays hands the driver one record per triangle corner, so a
 * closed manifold arrives with about six copies of every vertex (Euler: a
 * corner count of 3T against a vertex count of about T/2).  Two corners whose
 * whole source record is bit-identical are interchangeable to both the
 * preprocessing pass and the rasteriser, so welding them costs nothing in
 * exactness and divides the records a job carries.
 *
 * NOT ENABLED.  M9X_WELD=1 turns this on; the default is off, for two
 * measured reasons rather than caution.
 *
 * It does not pay.  A job's record count is the preprocessing pass's work
 * alone: the raster stage still walks every index, once per raster pass and
 * so four times for a four-pass shader.  Welding removes CPU record building
 * and preprocessing, not the raster vertex work, and that is where the frame
 * is (MEASURED on refract at 1920x1200: 47.7 ns a record of raster against
 * 0.018 ms a job of submission).
 *
 * It stops the engine.  A welded draw inside a multi-pass split job is a
 * schedule this driver has never emitted, and refract at a 46080 ceiling
 * with welding on produced a stream timeout ("M9 stall [7845] 45452d2c"),
 * after which the device returned errors until a reboot.  Anyone switching
 * this on is re-opening that, not collecting a win.
 *
 * It is also benchmark-shaped rather than general: an application that
 * indexes its own meshes already hands the driver the short form, and only
 * one that could have indexed and did not gains anything here.
 */
#ifndef M9_WELD_H
#define M9_WELD_H

#include <stdbool.h>
#include <stdint.h>

/* The result of welding one run of source records.  index maps a corner of
 * the original draw to a unique record, and source maps a unique record back
 * to the corner it was first seen at.  Unique records are numbered in
 * first-use order, which is what keeps the index span of a run of corners
 * local: with the corners in primitive order, a run of them refers to a
 * window of unique records rather than to the whole mesh, and a job can be
 * cut on that window. */
struct m9_weld {
   uint32_t *index;    /* count entries */
   uint32_t *source;   /* unique entries */
   uint32_t count;
   uint32_t unique;
   /* The widest window any three consecutive corners reach, which bounds a
    * single triangle and a single line.  A job cannot carry a primitive
    * whose own window exceeds its record ceiling, so this says up front
    * whether the draw can be welded at all. */
   uint32_t widest;
};

/* Welds count records of record_dwords floats each.  Returns false without
 * allocating on a bad argument or out of memory, so a caller falls back to
 * the unwelded draw rather than failing it. */
bool m9_weld_build(struct m9_weld *weld, const float *records, uint32_t count,
                   unsigned record_dwords);
void m9_weld_fini(struct m9_weld *weld);

/* The lowest and highest unique record a run of corners refers to.  A job
 * that draws the run has to carry every record of that window. */
bool m9_weld_span(const struct m9_weld *weld, uint32_t first, uint32_t count,
                  uint32_t *lo, uint32_t *hi);

/* Corners of the longest run starting at first whose window fits max records,
 * a whole grain of primitives at a time, with the window in lo/hi.  Growing
 * the run forward keeps the window tight: halving a run instead re-reads the
 * records either side of every cut, which on a real mesh cost more records
 * than the unwelded draw carried (MEASURED offline, bunny.obj at a ceiling of
 * 23040: 24 jobs and 424109 records against 208998 unwelded).  Returns 0 when
 * even one primitive reaches past max. */
uint32_t m9_weld_run(const struct m9_weld *weld, uint32_t first, uint32_t max,
                     unsigned grain, uint32_t *lo, uint32_t *hi);

#endif
