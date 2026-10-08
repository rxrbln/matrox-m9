/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_TEXTURE_OWNER_H
#define M9_FAA_GRAPH_TEXTURE_OWNER_H

#include "m9_faa_graph_texture_dual.h"

struct drm_file;
struct drm_gem_object;
struct dma_fence;
struct m9x_device;

/* References and pins are acquired before graph reservations. They remain
 * held after any END post until the read fence retires or DMA is stopped.
 * Initialize this object to zero before its first acquire. Reacquiring an
 * occupied object returns -EBUSY without modifying its retained owners. */
struct m9x_faa_texture_owners {
	struct drm_gem_object *gem[2];
	bool pinned[2];
	struct dma_fence *retirement;
	struct m9x_faa_texture_bind_input bind[2];
	u32 generation;
};

int m9x_faa_texture_owners_acquire(struct m9x_device *mdev,
	struct drm_file *file, const u32 handle[2],
	const struct m9x_faa_texture_bind_input request[2],
	struct drm_gem_object *const graph_owner[6], u32 generation,
	struct m9x_faa_texture_owners *out);

/* Caller holds both texture reservations and transport.lock, and has already
 * authenticated the retained graph generation and complete command image. */
int m9x_faa_texture_owners_place_locked(struct m9x_device *mdev,
	const struct m9x_faa_texture_owners *owners, u32 generation,
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);
int m9x_faa_texture_owners_place_append_locked(struct m9x_device *mdev,
	const struct m9x_faa_texture_owners *owners, u32 generation,
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);
int m9x_faa_texture_owners_place_colorpass_locked(struct m9x_device *mdev,
	const struct m9x_faa_texture_owners *owners, u32 generation,
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);

/* Reserve both READ fence slots before installing either. A genuinely
 * signaled earlier retirement may be replaced for same-pair frame reuse;
 * an unsignaled one refuses without modifying either reservation. The caller
 * also fences its six graph owners before command writes or END doorbells. */
int m9x_faa_texture_owners_fence_locked(struct m9x_faa_texture_owners *owners,
	struct dma_fence *fence);

/* False leaves every reference and pin in place after an unretired post. */
bool m9x_faa_texture_owners_put(struct m9x_faa_texture_owners *owners,
	bool dma_stopped);

#endif
