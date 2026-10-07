// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/kref.h>

#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "m9_drv.h"

/* Source-qualified prepared180 shape. These are allocation extents, not
 * permission to clear opaque tails or evidence of GPU initialization. */
static const struct {
	u64 logical, capacity, clear;
} m9_faa_graph_owner_spec[PARHELIA_M9_FAA_GRAPH_OWNERS] = {
	[PARHELIA_M9_FAA_PRIMARY]   = { 0x50000, 0x50000, 0 },
	[PARHELIA_M9_FAA_MAIN]      = { 0x136080, 0x137000, 0 },
	[PARHELIA_M9_FAA_AUXILIARY] = { 0x13080, 0x14000, 0 },
	[PARHELIA_M9_FAA_COMPANION] = { 0x96020, 0x98000, 0x97000 },
	[PARHELIA_M9_FAA_HZ]        = { 0x3000, 0x8000, 0x3000 },
	[PARHELIA_M9_FAA_SENTINEL]  = { 0x80, 0x1000, 0 },
};

struct m9_faa_prepared_graph {
	struct kref refs;
	struct drm_gem_object *owner[PARHELIA_M9_FAA_GRAPH_OWNERS];
	bool pinned[PARHELIA_M9_FAA_GRAPH_OWNERS];
	u64 id;
	u32 generation;
};

struct m9_faa_file {
	struct mutex lock;
	struct m9_faa_prepared_graph *graph;
};

static bool m9_faa_graph_owner_distinct(
		const struct m9_faa_prepared_graph *graph, u32 role)
{
	u32 i;

	if (!graph->owner[role])
		return false;
	for (i = 0; i < role; i++)
		if (graph->owner[role] == graph->owner[i])
			return false;
	return true;
}

static bool m9_faa_graph_release_matches(
		const struct m9_faa_prepared_graph *graph,
		const struct drm_parhelia_m9_faa_graph_release *args)
{
	return graph && graph->id == args->graph_id &&
	       graph->generation == args->generation && !args->flags;
}

static bool m9_faa_graph_request_valid(
		const struct drm_parhelia_m9_faa_graph_prepare *args)
{
	u32 i, j;

	if (args->width != 320 || args->height != 240 ||
	    args->format != 0x66 || args->primary_desc != 0xcc220140 ||
	    args->companion_desc != 0x7a220140 ||
	    args->hz_desc != 0xf0010140 || args->ad != (0x136080 >> 5) ||
	    args->flags || args->graph_id || args->init_point ||
	    args->generation || args->state)
		return false;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		const struct drm_parhelia_m9_faa_owner *o = &args->owners[i];

		if (!o->handle || o->pad ||
		    o->logical_bytes != m9_faa_graph_owner_spec[i].logical ||
		    o->capacity_bytes != m9_faa_graph_owner_spec[i].capacity ||
		    o->clear_bytes != m9_faa_graph_owner_spec[i].clear ||
		    o->logical_bytes > o->capacity_bytes ||
		    o->clear_bytes > o->capacity_bytes)
			return false;
		for (j = 0; j < i; j++)
			if (o->handle == args->owners[j].handle)
				return false;
	}
	return true;
}

/* Pure admission checks run at module initialization, before any MMIO. */
int m9x_faa_graph_selftest(void)
{
	struct drm_parhelia_m9_faa_graph_prepare args = {
		.width = 320, .height = 240, .format = 0x66,
		.primary_desc = 0xcc220140,
		.companion_desc = 0x7a220140,
		.hz_desc = 0xf0010140, .ad = 0x136080 >> 5,
	};
	u32 i;
	struct drm_gem_object gem[2];
	struct m9_faa_prepared_graph graph = {
		.id = 7, .generation = 0,
	};
	struct drm_parhelia_m9_faa_graph_release release = {
		.graph_id = 7, .generation = 0,
	};

	BUILD_BUG_ON(ARRAY_SIZE(m9_faa_graph_owner_spec) !=
		     PARHELIA_M9_FAA_GRAPH_OWNERS);
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		args.owners[i].handle = i + 1;
		args.owners[i].logical_bytes = m9_faa_graph_owner_spec[i].logical;
		args.owners[i].capacity_bytes = m9_faa_graph_owner_spec[i].capacity;
		args.owners[i].clear_bytes = m9_faa_graph_owner_spec[i].clear;
	}
	if (!m9_faa_graph_request_valid(&args))
		return -EINVAL;
	args.owners[PARHELIA_M9_FAA_COMPANION].clear_bytes++;
	if (m9_faa_graph_request_valid(&args))
		return -EINVAL;
	args.owners[PARHELIA_M9_FAA_COMPANION].clear_bytes--;
	args.owners[PARHELIA_M9_FAA_HZ].capacity_bytes--;
	if (m9_faa_graph_request_valid(&args))
		return -EINVAL;
	args.owners[PARHELIA_M9_FAA_HZ].capacity_bytes++;
	args.owners[PARHELIA_M9_FAA_SENTINEL].handle = 1;
	if (m9_faa_graph_request_valid(&args))
		return -EINVAL;
	args.owners[PARHELIA_M9_FAA_SENTINEL].handle = 6;
	args.primary_desc ^= 1;
	if (m9_faa_graph_request_valid(&args))
		return -EINVAL;
	args.primary_desc ^= 1;
	args.init_point = 1;
	if (m9_faa_graph_request_valid(&args))
		return -EINVAL;
	graph.owner[0] = &gem[0];
	graph.owner[1] = &gem[0];
	if (m9_faa_graph_owner_distinct(&graph, 1))
		return -EINVAL;
	graph.owner[1] = &gem[1];
	if (!m9_faa_graph_owner_distinct(&graph, 1) ||
	    !m9_faa_graph_release_matches(&graph, &release))
		return -EINVAL;
	release.generation = 1;
	if (m9_faa_graph_release_matches(&graph, &release))
		return -EINVAL;
	release.generation = 0;
	release.graph_id = 8;
	if (m9_faa_graph_release_matches(&graph, &release))
		return -EINVAL;
	return 0;
}

static bool m9_faa_graph_owner_class_valid(u32 role,
					   struct drm_gem_object *gem)
{
	if (role == PARHELIA_M9_FAA_PRIMARY)
		return m9x_gem_vram_tiled(gem);
	if (role == PARHELIA_M9_FAA_SENTINEL)
		return m9x_gem_is_vram(gem) && !m9x_gem_vram_tiled(gem);
	return m9x_gem_is_engine_vram(gem);
}

static void m9_faa_graph_destroy(struct kref *ref)
{
	struct m9_faa_prepared_graph *graph =
		container_of(ref, struct m9_faa_prepared_graph, refs);
	u32 i;

	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (graph->pinned[i])
			m9x_gem_vram_unpin(graph->owner[i]);
		if (graph->owner[i])
			drm_gem_object_put(graph->owner[i]);
	}
	kfree(graph);
}

void m9x_faa_graph_put(struct m9_faa_prepared_graph *graph)
{
	if (graph)
		kref_put(&graph->refs, m9_faa_graph_destroy);
}

/* Acquiring pins the exact group beyond RELEASE or file close. A future GPU
 * producer must also compare generation with the live transport under its
 * posting lock, then hold this reference through retirement/quarantine. */
struct m9_faa_prepared_graph *
m9x_faa_graph_acquire(struct drm_file *file, u64 id, u32 generation)
{
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_prepared_graph *graph = NULL;

	if (!state || !id)
		return NULL;
	mutex_lock(&state->lock);
	if (state->graph && state->graph->id == id &&
	    state->graph->generation == generation) {
		graph = state->graph;
		kref_get(&graph->refs);
	}
	mutex_unlock(&state->lock);
	return graph;
}

struct drm_gem_object *
m9x_faa_graph_owner(const struct m9_faa_prepared_graph *graph, u32 role)
{
	return graph && role < PARHELIA_M9_FAA_GRAPH_OWNERS ?
		graph->owner[role] : NULL;
}

int m9x_faa_graph_file_open(struct drm_device *drm, struct drm_file *file)
{
	struct m9_faa_file *state = kzalloc_obj(*state);

	if (!state)
		return -ENOMEM;
	mutex_init(&state->lock);
	file->driver_priv = state;
	return 0;
}

void m9x_faa_graph_file_close(struct drm_device *drm, struct drm_file *file)
{
	struct m9_faa_file *state = file->driver_priv;

	if (!state)
		return;
	m9x_faa_graph_put(state->graph);
	kfree(state);
	file->driver_priv = NULL;
}

static int m9_faa_graph_prepare(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_prepare *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u64 limit = m9x_vram_gpu_limit(mdev);
	u32 i;
	int ret = 0;

	if (!m9x_enabled())
		return -ENODEV;
	if (!state || !m9_faa_graph_request_valid(args) ||
	    mdev->pdev->device != M9X_PCI_DEVICE_PHOENIX ||
	    mdev->pdev->revision != 1 || mdev->pdev->subsystem_device != 0x22c0 ||
	    m9x_local_gpu_base || !limit)
		return -EINVAL;
	graph = kzalloc_obj(*graph);
	if (!graph)
		return -ENOMEM;
	kref_init(&graph->refs);
	mutex_lock(&state->lock);
	if (state->graph) {
		ret = -EBUSY;
		goto out_unlock;
	}
	/* Lookup and pin every owner before acquiring the reservation set. No
	 * partial group becomes visible to the file or to the transport. */
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		graph->owner[i] = drm_gem_object_lookup(file, args->owners[i].handle);
		if (!graph->owner[i]) {
			ret = -ENOENT;
			goto out_unlock;
		}
		if (!m9_faa_graph_owner_distinct(graph, i)) {
			ret = -EINVAL;
			goto out_unlock;
		}
		if (!m9_faa_graph_owner_class_valid(i, graph->owner[i]) ||
		    graph->owner[i]->size != args->owners[i].capacity_bytes) {
			ret = -EINVAL;
			goto out_unlock;
		}
		ret = m9x_gem_vram_pin(graph->owner[i]);
		if (ret)
			goto out_unlock;
		graph->pinned[i] = true;
	}
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			/* PREPARE posts no work, so it needs the reservation lock
			 * without allocating a fence slot. dma_resv rejects zero
			 * requested fence slots with -EINVAL. */
			ret = drm_exec_lock_obj(&exec, graph->owner[i]);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		s64 offset = m9x_gem_vram_offset(graph->owner[i]);
		u64 end;

		if (offset < 0 ||
		    check_add_overflow((u64)offset,
			args->owners[i].capacity_bytes, &end) || end > limit ||
		    !IS_ALIGNED(offset, PAGE_SIZE)) {
			ret = -ERANGE;
			goto out_exec;
		}
	}
	/* The token names a retained CPU ownership group only. No GPU command,
	 * pool mapping or fence is installed here. */
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.async_stop || mdev->transport.wedged ||
	    mdev->transport.faa_virtual_primary ||
	    mdev->transport.faa_source_context) {
		ret = -EBUSY;
	} else {
		graph->generation = mdev->transport.generation;
		graph->id = atomic64_inc_return(&mdev->faa_graph_next);
		state->graph = graph;
		args->graph_id = graph->id;
		args->generation = graph->generation;
		args->state = PARHELIA_M9_FAA_GRAPH_PREPARED;
		args->init_point = 0;
	}
	mutex_unlock(&mdev->transport.lock);
out_exec:
	drm_exec_fini(&exec);
out_unlock:
	mutex_unlock(&state->lock);
	if (ret)
		m9x_faa_graph_put(graph);
	return ret;
}

int m9x_faa_graph_prepare_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	int idx, ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9_faa_graph_prepare(drm, data, file);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_release_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_release *args = data;
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_prepared_graph *graph;

	if (!state || args->flags || !args->graph_id)
		return -EINVAL;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_release_matches(graph, args)) {
		mutex_unlock(&state->lock);
		return -ENOENT;
	}
	state->graph = NULL;
	mutex_unlock(&state->lock);
	m9x_faa_graph_put(graph);
	return 0;
}
