# SPDX-License-Identifier: GPL-2.0-only

m9x_drm-y := m9x_drv.o m9_gem.o m9_requester.o m9_address.o m9_submit.o m9_triangle.o m9_faa_native.o m9_faa_graph.o m9_faa_graph_draw_encode.o m9_faa_graph_draw_tail_encode.o m9_faa_graph_lock_encode.o m9_faa_graph_next_encode.o m9_faa_graph_next_finish_encode.o m9_faa_graph_public_vs_encode.o m9_faa_graph_public_vs_follow_encode.o m9_faa_graph_public_vs_draw_encode.o m9_faa_graph_public_vs_lock_encode.o m9_faa_graph_public_vs_next_encode.o m9_faa_graph_color_encode.o m9_faa_graph_color_four_encode.o m9_faa_graph_color_next_encode.o m9_faa_graph_texture_bind.o m9_faa_graph_texture_place.o m9_faa_graph_depth_encode.o m9_faa_graph_depth_draw_encode.o m9_faa_graph_raster_encode.o m9_faa_graph_depth_raster_encode.o m9_faa_graph_sample_clear_encode.o m9_faa_graph_first_depth_encode.o m9_faa_graph_first_depth_two_encode.o m9_faa_graph_first_depth_two_append.o m9_faa_depth_mode_reset.o m9_faa_depth_mode_append.o m9_faa_depth_mode_later.o m9_faa_depth_mode_clear_resume.o m9_faa_graph_later_depth_encode.o m9_faa_graph_later_depth_two_encode.o m9_faa_graph_later_depth_two_append.o m9_faa_graph_program_arena.o m9_faa_graph_present_encode.o m9_faa_graph_frame_batch_input.o m9_faa_graph_frame_first_append_encode.o m9_kms.o m9_ddc.o m9_dp.o m9_mode.o m9_surface.o

obj-$(CONFIG_DRM_M9X) += m9x_drm.o

# Retained M9128 texture views and the source-qualified two-unit packet motif.
m9x_drm-y += m9_faa_graph_texture_dual.o m9_faa_graph_texture_dual_link.o m9_faa_graph_dual_first_encode.o m9_faa_graph_dual_colorpass_encode.o m9_faa_graph_dual_first_append.o m9_faa_graph_dual_batch_input.o m9_faa_graph_texture_owner.o

# Pure source-qualified counted secondary link and live two-slot lifetime.
m9x_drm-y += m9_faa_counted_secondary_encode.o m9_faa_secondary_pool.o m9_faa_primary_pool.o

ccflags-$(CONFIG_DRM_M9X_FAA_CONSTANT_SHADER_CONTROL) += -DM9_FAA_CONSTANT_SHADER_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_CONSTANT_RED_CONTROL) += -DM9_FAA_CONSTANT_RED_CONTROL
ccflags-$(CONFIG_DRM_M9X_ORDINARY_SYSTEM_CONTROL) += -DM9_FAA_ORDINARY_SYSTEM_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_INIT_ONLY_CONTROL) += -DM9_FAA_INIT_ONLY_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_GRAPH_INIT_CONTROL) += -DM9_FAA_GRAPH_INIT_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PRECLEAR_CONTROL) += -DM9_FAA_PRECLEAR_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PREPARED_PRECLEAR_CONTROL) += -DM9_FAA_PREPARED_PRECLEAR_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PREPARED_FLOW_CONTROL) += -DM9_FAA_PREPARED_FLOW_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PRECLEAR_DRAW_CONTROL) += -DM9_FAA_PRECLEAR_DRAW_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PRECLEAR_OUTPUT_CONTROL) += -DM9_FAA_PRECLEAR_OUTPUT_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL) += -DM9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL) += -DM9_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PRECLEAR_INPLACE_COPY_CONTROL) += -DM9_FAA_PRECLEAR_INPLACE_COPY_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PUBLIC_SHARED_DEPTH_FLUSH_CONTROL) += -DM9_FAA_PUBLIC_SHARED_DEPTH_FLUSH_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PUBLIC_SHARED_DEPTH_RESOLVE_CONTROL) += -DM9_FAA_PUBLIC_SHARED_DEPTH_RESOLVE_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PUBLIC_SHARED_DEPTH_RETURNED_CONTROL) += -DM9_FAA_PUBLIC_SHARED_DEPTH_RETURNED_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_CONTROL) += -DM9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_SUBMIT_CONTROL) += -DM9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_SUBMIT_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_RETURNED_CONTROL) += -DM9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_RETURNED_CONTROL
ccflags-$(CONFIG_DRM_M9X_FAA_POST_TRIANGLE_CONTROL) += -DM9_FAA_POST_TRIANGLE_CONTROL
