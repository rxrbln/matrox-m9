// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kref.h>

#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "m9_drv.h"
#include "m9_faa_graph_public_vs_encode.h"
#include "m9_faa_graph_color_encode.h"

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
	u64 init_point;
	bool initialized;
};

struct m9_faa_file {
	struct mutex lock;
	struct m9_faa_prepared_graph *graph;
};

/* The original 320x240 init path's six caller-owned roles. This is only a
 * source-role join; executable words must still come from a trusted producer. */
static const u8 m9_faa_graph_source_owner[PARHELIA_M9_FAA_GRAPH_OWNERS] = {
	[PARHELIA_M9_FAA_PRIMARY] = 1,
	[PARHELIA_M9_FAA_MAIN] = 2,
	[PARHELIA_M9_FAA_AUXILIARY] = 3,
	[PARHELIA_M9_FAA_COMPANION] = 4,
	[PARHELIA_M9_FAA_HZ] = 5,
	[PARHELIA_M9_FAA_SENTINEL] = 8,
};

/* A future posted init transfers this whole binding to its retirement or
 * quarantine record. File RELEASE only drops its own graph reference. */
struct m9_faa_graph_binding {
	struct kref refs;
	struct m9_faa_prepared_graph *graph;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS];
	u32 generation;
};

static bool m9_faa_graph_local_span(u64 local_base, u64 offset, u64 bytes,
				    u64 limit, u32 *address)
{
	u64 end, base;

	if (!address || !bytes || !IS_ALIGNED(offset, PAGE_SIZE) ||
	    !IS_ALIGNED(local_base, PAGE_SIZE) ||
	    check_add_overflow(offset, bytes, &end) || end > limit ||
	    check_add_overflow(local_base, offset, &base) ||
	    check_add_overflow(base, bytes, &end) ||
	    end > (u64)U32_MAX + 1)
		return false;
	*address = (u32)base;
	return true;
}

static bool m9_faa_graph_ranges_disjoint(const u32 *address)
{
	u32 i, j;

	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		for (j = 0; j < i; j++)
			if ((u64)address[i] < (u64)address[j] +
				m9_faa_graph_owner_spec[j].capacity &&
			    (u64)address[j] < (u64)address[i] +
				m9_faa_graph_owner_spec[i].capacity)
				return false;
	return true;
}

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

static bool m9_faa_graph_continuation_matches(
		const struct m9_faa_prepared_graph *graph,
		u64 id, u32 generation, u64 init_point)
{
	return graph && id && init_point && graph->initialized &&
	       graph->id == id && graph->generation == generation &&
	       graph->init_point == init_point;
}

static bool m9_faa_graph_init_request_valid(
		const struct drm_parhelia_m9_faa_graph_init *args)
{
	return args && args->graph_id && !args->flags && !args->init_point &&
	       !args->state && !args->pad;
}

static bool m9_faa_graph_record_retired(struct m9_faa_prepared_graph *graph,
		struct drm_parhelia_m9_faa_graph_init *args, u64 point)
{
	if (!graph || !args || !point || graph->initialized ||
	    graph->id != args->graph_id ||
	    graph->generation != args->generation)
		return false;
	graph->initialized = true;
	graph->init_point = point;
	args->init_point = point;
	args->state = PARHELIA_M9_FAA_GRAPH_INITIALIZED;
	return true;
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
	struct m9_faa_graph_binding retained = { .graph = &graph };
	struct drm_parhelia_m9_faa_graph_release release = {
		.graph_id = 7, .generation = 0,
	};
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS] = {
		0x1000000, 0x1100000, 0x1240000,
		0x1260000, 0x1300000, 0x1310000,
	};
	u32 native = 0;

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
	graph.initialized = true;
	graph.init_point = 0xb0;
	if (!m9_faa_graph_continuation_matches(&graph, 7, 0, 0xb0) ||
	    m9_faa_graph_continuation_matches(&graph, 8, 0, 0xb0) ||
	    m9_faa_graph_continuation_matches(&graph, 7, 1, 0xb0) ||
	    m9_faa_graph_continuation_matches(&graph, 7, 0, 0xc0) ||
	    m9_faa_graph_continuation_matches(&graph, 7, 0, 0))
		return -EINVAL;
	graph.initialized = false;
	if (m9_faa_graph_continuation_matches(&graph, 7, 0, 0xb0))
		return -EINVAL;
	/* A matching file token can disappear after authentication, while an
	 * acquired binding reference must remain live for the admitted caller. */
	kref_init(&retained.refs);
	if (m9x_faa_graph_binding_get(&retained) != &retained ||
	    kref_read(&retained.refs) != 2)
		return -EINVAL;
	m9x_faa_graph_binding_put(&retained);
	if (kref_read(&retained.refs) != 1 ||
	    m9_faa_graph_continuation_matches(NULL, 7, 0, 0xb0))
		return -EINVAL;
	release.generation = 0;
	release.graph_id = 8;
	if (m9_faa_graph_release_matches(&graph, &release))
		return -EINVAL;
	if (m9_faa_graph_source_owner[PARHELIA_M9_FAA_PRIMARY] != 1 ||
	    m9_faa_graph_source_owner[PARHELIA_M9_FAA_MAIN] != 2 ||
	    m9_faa_graph_source_owner[PARHELIA_M9_FAA_AUXILIARY] != 3 ||
	    m9_faa_graph_source_owner[PARHELIA_M9_FAA_COMPANION] != 4 ||
	    m9_faa_graph_source_owner[PARHELIA_M9_FAA_HZ] != 5 ||
	    m9_faa_graph_source_owner[PARHELIA_M9_FAA_SENTINEL] != 8 ||
	    !m9_faa_graph_ranges_disjoint(address) ||
	    !m9_faa_graph_local_span(0, 0x2f00000, 0x98000,
				    0x10000000, &native) || native != 0x2f00000)
		return -EINVAL;
	address[PARHELIA_M9_FAA_HZ] = address[PARHELIA_M9_FAA_COMPANION];
	if (m9_faa_graph_ranges_disjoint(address) ||
	    m9_faa_graph_local_span(0, 0x2f00001, 0x98000,
				    0x10000000, &native) ||
	    m9_faa_graph_local_span(0, 0x2f00000, 0x98000,
				    0x2f00000, &native))
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

/* Caller holds one WW reservation set covering all six owners. A retained
 * pin should make these positions stable, but a continuation must compare
 * its live placement to the original binding before using any address. */
static int m9_faa_graph_live_addresses(struct m9x_device *mdev,
		const struct m9_faa_prepared_graph *graph,
		u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS])
{
	u64 limit = m9x_vram_gpu_limit(mdev);
	u32 i;

	if (!limit)
		return -ERANGE;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		struct drm_gem_object *gem = graph->owner[i];
		s64 offset;

		if (!gem || !graph->pinned[i] ||
		    !m9_faa_graph_owner_class_valid(i, gem) ||
		    gem->size != m9_faa_graph_owner_spec[i].capacity)
			return -ERANGE;
		offset = m9x_gem_vram_offset(gem);
		if (offset < 0 ||
		    !m9_faa_graph_local_span(M9_VRAM_GPU_BASE, (u64)offset,
			m9_faa_graph_owner_spec[i].capacity, limit,
			&address[i]))
			return -ERANGE;
	}
	return m9_faa_graph_ranges_disjoint(address) ? 0 : -EINVAL;
}

int m9x_faa_graph_binding_acquire(struct m9x_device *mdev,
		struct drm_file *file, u64 id, u32 generation,
		struct m9_faa_graph_binding **out)
{
	struct m9_faa_prepared_graph *graph;
	struct m9_faa_graph_binding *binding;
	struct drm_exec exec;
	u32 i;
	int ret = 0;

	if (!mdev || !file || !file->minor ||
	    file->minor->dev != &mdev->drm || !out || !id)
		return -EINVAL;
	*out = NULL;
	graph = m9x_faa_graph_acquire(file, id, generation);
	if (!graph)
		return -ENOENT;
	binding = kzalloc_obj(*binding);
	if (!binding) {
		m9x_faa_graph_put(graph);
		return -ENOMEM;
	}
	binding->graph = graph;
	kref_init(&binding->refs);
	binding->generation = generation;
	if (m9x_local_gpu_base || !m9x_vram_gpu_limit(mdev) ||
	    mdev->pdev->device != M9X_PCI_DEVICE_PHOENIX ||
	    mdev->pdev->revision != 1 || mdev->pdev->subsystem_device != 0x22c0) {
		ret = -EOPNOTSUPP;
		goto out_binding;
	}
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_lock_obj(&exec, graph->owner[i]);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, binding->address);
	if (ret)
		goto out_exec;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.generation != generation)
		ret = -ESTALE;
	else if (mdev->transport.async_stop || mdev->transport.wedged ||
		 mdev->transport.faa || mdev->transport.faa_virtual_primary ||
		 mdev->transport.faa_source_context)
		ret = -EBUSY;
	mutex_unlock(&mdev->transport.lock);
out_exec:
	drm_exec_fini(&exec);
out_binding:
	if (ret) {
		m9x_faa_graph_binding_put(binding);
		return ret;
	}
	*out = binding;
	return 0;
}

struct drm_gem_object *
m9x_faa_graph_binding_owner(const struct m9_faa_graph_binding *binding,
			    u32 role)
{
	return binding ? m9x_faa_graph_owner(binding->graph, role) : NULL;
}

bool m9x_faa_graph_binding_address(const struct m9_faa_graph_binding *binding,
				u32 role, u32 *address, u32 *source_owner)
{
	if (!binding || role >= PARHELIA_M9_FAA_GRAPH_OWNERS ||
	    !address || !source_owner)
		return false;
	*address = binding->address[role];
	*source_owner = m9_faa_graph_source_owner[role];
	return true;
}

struct m9_faa_graph_binding *
m9x_faa_graph_binding_get(struct m9_faa_graph_binding *binding)
{
	if (binding)
		kref_get(&binding->refs);
	return binding;
}

static void m9_faa_graph_binding_destroy(struct kref *ref)
{
	struct m9_faa_graph_binding *binding =
		container_of(ref, struct m9_faa_graph_binding, refs);

	m9x_faa_graph_put(binding->graph);
	kfree(binding);
}

void m9x_faa_graph_binding_put(struct m9_faa_graph_binding *binding)
{
	if (!binding)
		return;
	kref_put(&binding->refs, m9_faa_graph_binding_destroy);
}

/* This is an admission snapshot, not permission to post later without a
 * second generation/placement/health check under the draw's reservations.
 * Lock order matches INIT: file state, six GEM reservations, transport. The
 * file lock prevents RELEASE from winning between token and native identity
 * checks; the returned binding kref survives either later RELEASE or fini. */
int m9x_faa_graph_continuation_acquire(struct m9x_device *mdev,
		struct drm_file *file, u64 id, u32 generation, u64 init_point,
		struct m9_faa_graph_binding **out)
{
	struct m9_faa_file *state;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS];
	u32 i;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!mdev || !file || !file->minor ||
	    file->minor->dev != &mdev->drm || !id || !init_point)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	state = file->driver_priv;
	if (!state)
		return -ENOENT;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, id, generation,
						init_point)) {
		ret = -ENOENT;
		goto out_state;
	}
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_lock_obj(&exec, graph->owner[i]);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_retained_get_locked(mdev, generation,
						  init_point, &binding);
	if (!ret && (binding->graph != graph ||
		     memcmp(binding->address, address, sizeof(address))))
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
out_exec:
	drm_exec_fini(&exec);
out_state:
	mutex_unlock(&state->lock);
	if (ret)
		m9x_faa_graph_binding_put(binding);
	else
		*out = binding;
	return ret;
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

int m9x_faa_graph_init_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_init *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding;
	struct drm_exec exec;
	u64 point = 0;
	u32 i;
	bool posted = false;
	bool prepared = false;
	int idx, ret;

	if (!state || !m9_faa_graph_init_request_valid(args))
		return -EINVAL;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9x_faa_graph_binding_acquire(mdev, file, args->graph_id,
						args->generation, &binding);
	if (ret)
		goto out_dev;
	mutex_lock(&state->lock);
	if (state->graph != binding->graph) {
		ret = -ENOENT;
		goto out_state;
	}
	if (state->graph->initialized) {
		ret = -EALREADY;
		goto out_state;
	}
	/* Allocation, known-byte upload and private mappings precede the six
	 * caller BO reservations. A successful prepare transfers the binding to
	 * the retained native producer; it stays alive after file RELEASE. */
	ret = m9x_faa_prepare_graph(mdev, binding, args->generation);
	if (ret)
		goto out_state;
	prepared = true;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec,
				m9x_faa_graph_binding_owner(binding, i), 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(
			m9x_faa_graph_binding_owner(binding, i)->resv,
			DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	/* The native producer itself checks the fresh queue and transport
	 * generation before its first doorbell. It waits for the real source
	 * final target; neither a CPU seed nor a captured desired counter is a
	 * receipt. Failed posts keep the producer's owners in quarantine. */
	ret = m9x_faa_submit_graph(mdev, binding, &point, &posted);
	if (!ret && !m9_faa_graph_record_retired(state->graph, args, point))
		ret = -EIO;
out_exec:
	drm_exec_fini(&exec);
out_state:
	mutex_unlock(&state->lock);
	if (!prepared)
		m9x_faa_graph_binding_put(binding);
out_dev:
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_draw_red_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_draw_red *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS];
	u64 point = 0;
	u32 i;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || !args->init_point ||
	    (args->flags & ~(PARHELIA_M9_FAA_DRAW_WITH_FINISH |
			     PARHELIA_M9_FAA_DRAW_WITH_LOCK)) ||
	    ((args->flags & PARHELIA_M9_FAA_DRAW_WITH_LOCK) &&
	     !(args->flags & PARHELIA_M9_FAA_DRAW_WITH_FINISH)) ||
	    args->draw_point || args->state || args->pad)
		return -EINVAL;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9x_faa_graph_continuation_acquire(mdev, file,
			args->graph_id, args->generation, args->init_point,
			&binding);
	if (ret)
		goto out_dev;
	mutex_lock(&state->lock);
	if (state->graph != binding->graph ||
	    !m9_faa_graph_continuation_matches(state->graph,
		args->graph_id, args->generation, args->init_point)) {
		ret = -ENOENT;
		goto out_state;
	}
	/* Firmware, private local TTM reservation, and late SYSTEM mapping
	 * must finish before taking any of the six caller BO reservations. */
	ret = m9x_faa_graph_draw_prepare(mdev, binding, args->generation,
					args->init_point,
					args->flags & PARHELIA_M9_FAA_DRAW_WITH_FINISH,
					args->flags & PARHELIA_M9_FAA_DRAW_WITH_LOCK);
	if (ret)
		goto out_state;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec,
				m9x_faa_graph_binding_owner(binding, i), 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, binding->graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(address, binding->address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(
			m9x_faa_graph_binding_owner(binding, i)->resv,
			DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = m9x_faa_graph_draw_submit(mdev, binding, args->generation,
					 args->init_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->init_point +
		    (args->flags & PARHELIA_M9_FAA_DRAW_WITH_LOCK ? 0xc0 :
		     args->flags & PARHELIA_M9_FAA_DRAW_WITH_FINISH ? 0x90 : 0x60))
			ret = -EIO;
		else {
			args->draw_point = point;
			args->state = args->flags & PARHELIA_M9_FAA_DRAW_WITH_LOCK ?
				PARHELIA_M9_FAA_LOCK_PREFIX_RETIRED :
				args->flags & PARHELIA_M9_FAA_DRAW_WITH_FINISH ?
				PARHELIA_M9_FAA_DRAW_FINISH_RETIRED :
				PARHELIA_M9_FAA_DRAW_PREFIX_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_state:
	mutex_unlock(&state->lock);
	m9x_faa_graph_binding_put(binding);
out_dev:
	drm_dev_exit(idx);
	return ret;
}

static int m9_faa_graph_public_vs_ioctl_mode(struct drm_device *drm, void *data,
				 struct drm_file *file, bool head)
{
	struct drm_parhelia_m9_faa_graph_public_vs *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS];
	u64 point = 0;
	u32 i;
	bool posted = false;
	struct m9x_faa_public_vs_input input;
	int idx, ret;

	BUILD_BUG_ON(sizeof(args->layout) != sizeof(input.layout));
	if (!state || !args->graph_id || !args->init_point || args->flags ||
	    args->prefix_point || args->state || args->pad)
		return -EINVAL;
	input = (struct m9x_faa_public_vs_input) {
		.program = args->program, .constants = args->constants,
		.geometry = args->geometry,
		.program_bytes = sizeof(args->program),
		.constants_bytes = sizeof(args->constants),
		.geometry_bytes = sizeof(args->geometry),
	};
	memcpy(&input.layout, &args->layout, sizeof(input.layout));
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9x_faa_graph_continuation_acquire(mdev, file,
			args->graph_id, args->generation, args->init_point,
			&binding);
	if (ret)
		goto out_dev;
	mutex_lock(&state->lock);
	if (state->graph != binding->graph ||
	    !m9_faa_graph_continuation_matches(state->graph,
		args->graph_id, args->generation, args->init_point)) {
		ret = -ENOENT;
		goto out_state;
	}
	/* Firmware, typed encoding and the private local TTM reservation
	 * finish before taking any of the six caller BO reservations. The
	 * two SYSTEM maps remain exactly those retained by INIT. */
	ret = m9x_faa_graph_public_vs_prepare(mdev, binding, args->generation,
					    args->init_point, &input);
	if (ret)
		goto out_state;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec,
				m9x_faa_graph_binding_owner(binding, i), 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, binding->graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(address, binding->address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(
			m9x_faa_graph_binding_owner(binding, i)->resv,
			DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = head ?
		m9x_faa_graph_public_vs_head_submit(mdev, binding, args->generation,
					 args->init_point, &point, &posted) :
		m9x_faa_graph_public_vs_submit(mdev, binding, args->generation,
					 args->init_point, &point, &posted);
	if (!ret) {
		if (point != args->init_point + (head ? 0x10 : 0x50))
			ret = -EIO;
		else {
			args->prefix_point = point;
			args->state = head ? PARHELIA_M9_FAA_PUBLIC_VS_HEAD_RETIRED :
				PARHELIA_M9_FAA_PUBLIC_VS_PREFIX_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_state:
	mutex_unlock(&state->lock);
	m9x_faa_graph_binding_put(binding);
out_dev:
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_public_vs_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	return m9_faa_graph_public_vs_ioctl_mode(drm, data, file, false);
}

int m9x_faa_graph_public_vs_head_ioctl(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	return m9_faa_graph_public_vs_ioctl_mode(drm, data, file, true);
}

int m9x_faa_graph_unlock_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_unlock *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->lock_point < 0xc0 ||
	    args->unlock_point || args->flags || args->state || args->pad)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	/* File identity is independent of the number of retired frames. Native
	 * admission below authenticates the exact CURRENT Lock point and view.
	 * Subtracting a fixed first-frame delta would reject later real epochs. */
	if (!graph || !graph->initialized || !graph->init_point ||
	    graph->id != args->graph_id || graph->generation != args->generation) {
		ret = -ENOENT;
		goto out_state;
	}
	/* Token matching admits the public-VS Lock epoch too. Native admission
	 * separately requires its actual Lock marker and successful F021 view;
	 * a numerical point alone never authorizes Unlock.
	 * Keep RELEASE excluded through live address and native binding joins,
	 * reserve every owner, then retain the exact existing native binding. */
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_unlock_get_locked(mdev, args->generation,
			args->lock_point, &binding);
	if (!ret && (binding->graph != graph ||
		     memcmp(binding->address, address, sizeof(address))))
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_exec;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(graph->owner[i]->resv, DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = m9x_faa_graph_unlock_submit(mdev, binding, args->generation,
			args->lock_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->lock_point + 0x20)
			ret = -EIO;
		else {
			args->unlock_point = point;
			args->state = PARHELIA_M9_FAA_UNLOCK_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_color_lock_ioctl(struct drm_device *drm,void *data,struct drm_file *file)
{
 struct drm_parhelia_m9_faa_graph_color_lock *args=data;
 struct m9x_device *mdev=to_m9x(drm);struct m9_faa_file *state=file->driver_priv;
 struct m9_faa_prepared_graph *graph;struct m9_faa_graph_binding *binding=NULL;
 struct m9x_faa_color_input input;struct drm_exec exec;
 u8 *upload=NULL;u32 sum,tmp,address[PARHELIA_M9_FAA_GRAPH_OWNERS],i;u64 point=0;bool posted=false;int idx,ret;
 static_assert(sizeof(args->layout)==sizeof(input.layout));
 static_assert(sizeof(*args)==136);
 if(!state||!args->graph_id||!args->init_point||!args->upload||args->lock_point||args->flags||args->state||args->pad||
  !args->declaration_count||args->declaration_count>32||!args->upload_bytes||args->upload_bytes>0x42000)return -EINVAL;
 if(check_add_overflow(args->layout.program_bytes,args->layout.constants_bytes,&sum)||
  check_add_overflow(sum,args->layout.geometry_bytes,&sum)||
  check_mul_overflow(args->declaration_count,4u,&tmp)||check_add_overflow(sum,tmp,&sum)||
  sum!=args->upload_bytes)return -EINVAL;
 if(!m9x_faa_graph_profile_enabled())return -EOPNOTSUPP;
 if(!drm_dev_enter(drm,&idx))return -ENODEV;
 ret=m9x_faa_graph_continuation_acquire(mdev,file,args->graph_id,args->generation,args->init_point,&binding);
 if(ret)goto out_dev;
 mutex_lock(&state->lock);graph=state->graph;
 if(graph!=binding->graph||!m9_faa_graph_continuation_matches(graph,args->graph_id,args->generation,args->init_point)){ret=-ENOENT;goto out_binding;}
 upload=memdup_user(u64_to_user_ptr(args->upload),args->upload_bytes);
 if(IS_ERR(upload)){ret=PTR_ERR(upload);upload=NULL;goto out_binding;}
 memset(&input,0,sizeof(input));memcpy(&input.layout,&args->layout,sizeof(input.layout));
 input.program=upload;input.constants=input.program+args->layout.program_bytes;
 input.geometry=input.constants+args->layout.constants_bytes;
 input.declaration=input.geometry+args->layout.geometry_bytes;
 input.record_stride=args->record_stride;input.record_count=args->record_count;input.declaration_count=args->declaration_count;
 ret=m9x_faa_graph_color_prepare(mdev,binding,args->generation,args->init_point,&input);if(ret)goto out_binding;
 drm_exec_init(&exec,DRM_EXEC_INTERRUPTIBLE_WAIT|DRM_EXEC_IGNORE_DUPLICATES,PARHELIA_M9_FAA_GRAPH_OWNERS);
 drm_exec_until_all_locked(&exec){for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  ret=drm_exec_prepare_obj(&exec,graph->owner[i],1);drm_exec_retry_on_contention(&exec);if(ret)goto out_exec;}}
 ret=m9_faa_graph_live_addresses(mdev,graph,address);if(ret)goto out_exec;
 if(memcmp(binding->address,address,sizeof(address))){ret=-ESTALE;goto out_exec;}
 ret=m9x_faa_graph_color_submit(mdev,binding,args->generation,args->init_point,&point,&posted);
 if(!ret){if(!point||point!=args->init_point+(args->layout.program_count==4?0xe0:0xa0))ret=-EIO;
  else{args->lock_point=point;args->state=PARHELIA_M9_FAA_COLOR_LOCK_RETIRED;}}
out_exec:drm_exec_fini(&exec);
out_binding:kfree(upload);m9x_faa_graph_binding_put(binding);
mutex_unlock(&state->lock);
out_dev:drm_dev_exit(idx);return ret;
}

int m9x_faa_graph_public_vs_next_ioctl(struct drm_device *drm, void *data, struct drm_file *file)
{
 struct drm_parhelia_m9_faa_graph_public_vs_next *args=data;
 struct m9x_device *mdev=to_m9x(drm);struct m9_faa_file *state=file->driver_priv;
 struct m9_faa_prepared_graph *graph;struct m9_faa_graph_binding *binding=NULL;
 struct drm_exec exec;u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS],i;u64 point=0;bool posted=false;int idx,ret;
 if(!state||!args->graph_id||args->unlock_point<0x100||args->lock_point||args->flags||args->state||args->pad)return -EINVAL;
 if(!m9x_faa_graph_profile_enabled())return -EOPNOTSUPP;
 if(!drm_dev_enter(drm,&idx))return -ENODEV;
 mutex_lock(&state->lock);graph=state->graph;
 if(!m9_faa_graph_continuation_matches(graph,args->graph_id,args->generation,args->unlock_point-0x100)){ret=-ENOENT;goto out_state;}
 mutex_lock(&mdev->transport.lock);
 ret=m9x_faa_graph_public_vs_unlocked_get_locked(mdev,args->generation,args->unlock_point,&binding);
 if(!ret&&binding->graph!=graph)ret=-ESTALE;
 mutex_unlock(&mdev->transport.lock);if(ret)goto out_binding;
 ret=m9x_faa_graph_public_vs_next_prepare(mdev,binding,args->generation,args->unlock_point,args->vertices);
 if(ret)goto out_binding;
 drm_exec_init(&exec,DRM_EXEC_INTERRUPTIBLE_WAIT|DRM_EXEC_IGNORE_DUPLICATES,PARHELIA_M9_FAA_GRAPH_OWNERS);
 drm_exec_until_all_locked(&exec){for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  ret=drm_exec_prepare_obj(&exec,graph->owner[i],1);drm_exec_retry_on_contention(&exec);if(ret)goto out_exec;
 }}
 ret=m9_faa_graph_live_addresses(mdev,graph,address);if(ret)goto out_exec;
 if(memcmp(binding->address,address,sizeof(address))){ret=-ESTALE;goto out_exec;}
 ret=m9x_faa_graph_public_vs_next_submit(mdev,binding,args->generation,args->unlock_point,&point,&posted);
 if(!ret){if(!point||point!=args->unlock_point+0xd0)ret=-EIO;
  else{args->lock_point=point;args->state=PARHELIA_M9_FAA_PUBLIC_VS_NEXT_LOCK_RETIRED;}}
out_exec:drm_exec_fini(&exec);
out_binding:m9x_faa_graph_binding_put(binding);
out_state:mutex_unlock(&state->lock);drm_dev_exit(idx);return ret;
}

int m9x_faa_graph_public_vs_draw_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_public_vs_draw *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->head_point < 0xc0 ||
	    args->draw_point || args->flags || args->state || args->pad)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, args->graph_id,
			args->generation, args->head_point - 0x10)) {
		ret = -ENOENT;
		goto out_state;
	}
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_public_vs_head_get_locked(mdev, args->generation,
			args->head_point, &binding);
	if (!ret && binding->graph != graph)
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_binding;
	/* Firmware access and CPU image allocation precede caller reservations.
	 * The file mutex preserves the exact graph token across preparation. */
	ret = m9x_faa_graph_public_vs_draw_prepare(mdev, binding, args->generation,
			args->head_point);
	if (ret)
		goto out_binding;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(binding->address, address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	ret = m9x_faa_graph_public_vs_head_hazards(mdev,binding,args->generation,args->head_point);
	if(ret)goto out_exec;
	ret = m9x_faa_graph_public_vs_draw_submit(mdev, binding, args->generation,
			args->head_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->head_point + 0xa0)
			ret = -EIO;
		else {
			args->draw_point = point;
			args->state = PARHELIA_M9_FAA_PUBLIC_VS_DRAW_OBSERVED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_binding:
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_public_vs_lock_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_public_vs_lock *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->draw_point < 0x160 ||
	    args->lock_point || args->flags || args->state || args->pad)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, args->graph_id,
			args->generation, args->draw_point - 0xb0)) {
		ret = -ENOENT;
		goto out_state;
	}
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_public_vs_draw_get_locked(mdev, args->generation,
			args->draw_point, &binding);
	if (!ret && binding->graph != graph)
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_binding;
	/* Firmware access and CPU image allocation precede caller reservations.
	 * The file mutex preserves the exact graph token across preparation. */
	ret = m9x_faa_graph_public_vs_lock_prepare(mdev, binding, args->generation,
			args->draw_point);
	if (ret)
		goto out_binding;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(binding->address, address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	ret = m9x_faa_graph_public_vs_draw_hazards(mdev,binding,args->generation,args->draw_point);
	if(ret)goto out_exec;
	ret = m9x_faa_graph_public_vs_lock_submit(mdev, binding, args->generation,
			args->draw_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->draw_point + 0x30)
			ret = -EIO;
		else {
			args->lock_point = point;
			args->state = PARHELIA_M9_FAA_PUBLIC_VS_LOCK_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_binding:
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_public_vs_follow_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_public_vs_follow *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->prefix_point < 0x100 ||
	    args->follow_point || args->flags || args->state || args->pad)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, args->graph_id,
			args->generation, args->prefix_point - 0x50)) {
		ret = -ENOENT;
		goto out_state;
	}
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_public_vs_follow_get_locked(mdev, args->generation,
			args->prefix_point, &binding);
	if (!ret && binding->graph != graph)
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_binding;
	/* Firmware access and CPU image allocation precede caller reservations.
	 * The file mutex preserves the exact graph token across preparation. */
	ret = m9x_faa_graph_public_vs_follow_prepare(mdev, binding, args->generation,
			args->prefix_point);
	if (ret)
		goto out_binding;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(binding->address, address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(graph->owner[i]->resv, DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = m9x_faa_graph_public_vs_follow_submit(mdev, binding, args->generation,
			args->prefix_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->prefix_point + 0x30)
			ret = -EIO;
		else {
			args->follow_point = point;
			args->state = PARHELIA_M9_FAA_PUBLIC_VS_FOLLOW_PREFIX_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_binding:
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_next_draw_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_next_draw *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->unlock_point < 0xe0 ||
	    args->draw_point || args->flags || args->state || args->pad || args->reserved)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, args->graph_id,
			args->generation, graph ? graph->init_point : 0)) {
		ret = -ENOENT;
		goto out_state;
	}
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_next_get_locked(mdev, args->generation,
			args->unlock_point, &binding);
	if (!ret && binding->graph != graph)
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_binding;
	/* Firmware access and CPU image allocation precede caller reservations.
	 * The file mutex preserves the exact graph token across preparation. */
	ret = m9x_faa_graph_next_prepare(mdev, binding, args->generation,
			args->unlock_point, args->vertices);
	if (ret)
		goto out_binding;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(binding->address, address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(graph->owner[i]->resv, DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = m9x_faa_graph_next_submit(mdev, binding, args->generation,
			args->unlock_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->unlock_point + 0x50)
			ret = -EIO;
		else {
			args->draw_point = point;
			args->state = PARHELIA_M9_FAA_NEXT_PREFIX_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_binding:
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_next_finish_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_next_finish *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->prefix_point < 0x130 ||
	    args->finish_point || args->flags || args->state || args->pad)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, args->graph_id,
			args->generation, args->prefix_point - 0x130)) {
		ret = -ENOENT;
		goto out_state;
	}
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_next_finish_get_locked(mdev, args->generation,
			args->prefix_point, &binding);
	if (!ret && binding->graph != graph)
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_binding;
	/* Firmware access and CPU image allocation precede caller reservations.
	 * The file mutex preserves the exact graph token across preparation. */
	ret = m9x_faa_graph_next_finish_prepare(mdev, binding, args->generation,
			args->prefix_point);
	if (ret)
		goto out_binding;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(binding->address, address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(graph->owner[i]->resv, DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = m9x_faa_graph_next_finish_submit(mdev, binding, args->generation,
			args->prefix_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->prefix_point + 0x30)
			ret = -EIO;
		else {
			args->finish_point = point;
			args->state = PARHELIA_M9_FAA_NEXT_FINISH_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_binding:
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_next_lock_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_faa_graph_next_lock *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9_faa_file *state = file->driver_priv;
	struct m9_faa_graph_binding *binding = NULL;
	struct m9_faa_prepared_graph *graph;
	struct drm_exec exec;
	u32 address[PARHELIA_M9_FAA_GRAPH_OWNERS], i;
	u64 point = 0;
	bool posted = false;
	int idx, ret;

	if (!state || !args->graph_id || args->finish_point < 0x160 ||
	    args->lock_point || args->flags || args->state || args->pad)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&state->lock);
	graph = state->graph;
	if (!m9_faa_graph_continuation_matches(graph, args->graph_id,
			args->generation, args->finish_point - 0x160)) {
		ret = -ENOENT;
		goto out_state;
	}
	mutex_lock(&mdev->transport.lock);
	ret = m9x_faa_graph_next_lock_get_locked(mdev, args->generation,
			args->finish_point, &binding);
	if (!ret && binding->graph != graph)
		ret = -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_binding;
	/* Firmware access and CPU image allocation precede caller reservations.
	 * The file mutex preserves the exact graph token across preparation. */
	ret = m9x_faa_graph_next_lock_prepare(mdev, binding, args->generation,
			args->finish_point);
	if (ret)
		goto out_binding;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, PARHELIA_M9_FAA_GRAPH_OWNERS);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			ret = drm_exec_prepare_obj(&exec, graph->owner[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_faa_graph_live_addresses(mdev, graph, address);
	if (ret)
		goto out_exec;
	if (memcmp(binding->address, address, sizeof(address))) {
		ret = -ESTALE;
		goto out_exec;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		if (!dma_resv_test_signaled(graph->owner[i]->resv, DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out_exec;
		}
	}
	ret = m9x_faa_graph_next_lock_submit(mdev, binding, args->generation,
			args->finish_point, &point, &posted);
	if (!ret) {
		if (!point || point != args->finish_point + 0x30)
			ret = -EIO;
		else {
			args->lock_point = point;
			args->state = PARHELIA_M9_FAA_NEXT_LOCK_RETIRED;
		}
	}
out_exec:
	drm_exec_fini(&exec);
out_binding:
	m9x_faa_graph_binding_put(binding);
out_state:
	mutex_unlock(&state->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_faa_graph_color_next_ioctl(struct drm_device *drm,void *data,struct drm_file *file)
{
 struct drm_parhelia_m9_faa_graph_color_next *args=data;
 struct m9x_device *mdev=to_m9x(drm);struct m9_faa_file *state=file->driver_priv;
 struct m9_faa_prepared_graph *graph;struct m9_faa_graph_binding *binding=NULL;
 struct m9x_faa_color_input input;struct drm_exec exec;
 u8 *upload=NULL;u32 sum,tmp,address[PARHELIA_M9_FAA_GRAPH_OWNERS],i;u64 point=0;bool posted=false;int idx,ret;
 static_assert(sizeof(args->layout)==sizeof(input.layout));static_assert(sizeof(*args)==160);
 if(!state||!args->graph_id||!args->previous_unlock_point||!args->upload||args->lock_point||args->flags||args->state||args->pad||
  args->width!=320||args->height!=240||args->clear_flags>PARHELIA_M9_FAA_COLOR_CLEAR_TARGET||args->clear_color||
  args->depth_flags||args->raster_flags||args->layout.program_count!=4||
  !args->declaration_count||args->declaration_count>31||!args->upload_bytes||args->upload_bytes>0x42000)return -EINVAL;
 if(check_add_overflow(args->layout.program_bytes,args->layout.constants_bytes,&sum)||
  check_add_overflow(sum,args->layout.geometry_bytes,&sum)||check_mul_overflow(args->declaration_count,4u,&tmp)||
  check_add_overflow(sum,tmp,&sum)||sum!=args->upload_bytes)return -EINVAL;
 if(!m9x_faa_graph_profile_enabled())return -EOPNOTSUPP;
 if(!drm_dev_enter(drm,&idx))return -ENODEV;
 mutex_lock(&state->lock);graph=state->graph;
 if(!graph||!graph->initialized||!graph->init_point||graph->id!=args->graph_id||graph->generation!=args->generation){ret=-ENOENT;goto out_binding;}
 mutex_lock(&mdev->transport.lock);
 ret=m9x_faa_graph_color_unlocked_get_locked(mdev,args->generation,args->previous_unlock_point,&binding);
 mutex_unlock(&mdev->transport.lock);if(ret)goto out_binding;
 if(binding->graph!=graph){ret=-ESTALE;goto out_binding;}
 upload=memdup_user(u64_to_user_ptr(args->upload),args->upload_bytes);
 if(IS_ERR(upload)){ret=PTR_ERR(upload);upload=NULL;goto out_binding;}
 memset(&input,0,sizeof(input));memcpy(&input.layout,&args->layout,sizeof(input.layout));
 input.program=upload;input.constants=input.program+args->layout.program_bytes;
 input.geometry=input.constants+args->layout.constants_bytes;input.declaration=input.geometry+args->layout.geometry_bytes;
 input.record_stride=args->record_stride;input.record_count=args->record_count;input.declaration_count=args->declaration_count;
 ret=m9x_faa_graph_color_next_prepare(mdev,binding,args->generation,args->previous_unlock_point,&input,args->clear_flags);if(ret)goto out_binding;
 drm_exec_init(&exec,DRM_EXEC_INTERRUPTIBLE_WAIT|DRM_EXEC_IGNORE_DUPLICATES,PARHELIA_M9_FAA_GRAPH_OWNERS);
 drm_exec_until_all_locked(&exec){for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  ret=drm_exec_prepare_obj(&exec,graph->owner[i],1);drm_exec_retry_on_contention(&exec);if(ret)goto out_exec;}}
 ret=m9_faa_graph_live_addresses(mdev,graph,address);if(ret)goto out_exec;
 if(memcmp(binding->address,address,sizeof(address))){ret=-ESTALE;goto out_exec;}
 ret=m9x_faa_graph_color_next_submit(mdev,binding,args->generation,args->previous_unlock_point,&point,&posted);
 if(!ret){if(!point||point!=args->previous_unlock_point+0xd0+(args->clear_flags?0x10:0))ret=-EIO;
  else{args->lock_point=point;args->state=PARHELIA_M9_FAA_COLOR_NEXT_LOCK_RETIRED;}}
out_exec:drm_exec_fini(&exec);
out_binding:kfree(upload);m9x_faa_graph_binding_put(binding);mutex_unlock(&state->lock);drm_dev_exit(idx);return ret;
}
