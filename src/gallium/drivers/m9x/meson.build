# SPDX-License-Identifier: GPL-2.0-only

libm9x_batch = static_library(
  'm9x_batch',
  files('m9_batch.c', 'm9_format.c', 'm9_state.c'),
  include_directories : [inc_include, inc_src, inc_gallium],
  dependencies : [idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

libm9x = static_library(
  'm9x',
  files('m9_screen.c'),
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_libdrm, idep_mesautil],
  link_with : libm9x_batch,
  gnu_symbol_visibility : 'hidden',
)

driver_m9x = declare_dependency(
  compile_args : '-DGALLIUM_M9X',
  link_with : [libm9x, libm9x_batch],
)
