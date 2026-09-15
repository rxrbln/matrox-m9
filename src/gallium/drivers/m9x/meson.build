# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 René Rebe <rene@exactco.de>

m9x_batch_files = files(
  'm9_batch.c', 'm9_draw.c', 'm9_draw_params.c', 'm9_format.c', 'm9_state.c',
)

libm9x_batch = static_library(
  'm9x_batch',
  m9x_batch_files,
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

libm9x = static_library(
  'm9x',
  files('m9_screen.c', 'm9_context.c', 'm9_fs_compile.c'),
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil, idep_nir],
  link_with : libm9x_batch,
  gnu_symbol_visibility : 'hidden',
)

driver_m9x = declare_dependency(
  compile_args : '-DGALLIUM_M9X',
  link_with : [libm9x, libm9x_batch],
)
