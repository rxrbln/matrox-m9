# M9 fragment shader specialisation on uniform values

The native fragment backend has no control flow: `compile_impl` refuses any
shader whose entry point is more than one block.  Two shapes that glmark2 and
glamor draw with are nevertheless reachable, because the value that decides
the control flow is a uniform and is therefore known by the time the draw is
submitted:

* an `if` whose condition reads only uniforms and literals, which folds away
  entirely once the values are substituted;
* a `for` loop whose bound is a uniform -- glmark2's
  `loop:fragment-uniform=true` is `for (int i = 0; i < FragmentLoops; i++)` --
  which unrolls once the trip count is constant.

`m9_fs_find_inlinable_uniforms` marks the constant-buffer dwords of both
shapes, `m9_fs_inline_uniforms` substitutes the values and then folds, and
`nir_opt_loop_unroll` replaces the loop with its body.  `m9_fs_nir_options`
caps the unroll at `max_unroll_iterations = 32`; a bound past what the cost
model will unroll leaves the loop standing and the shader is refused as
"control flow" rather than compiled to the wrong iteration count.

Measured offline with the glmark2 loop fragment shader
(`m9-fs-compile-test --glsl`, `M9_FS_INLINE=n`):

| `FragmentLoops` | result |
|---:|---|
| not specialised | refused, "control flow" |
| 0 | 9 slots |
| 5 | 19 slots |
| 31 | 71 slots |
| 32 | 73 slots |
| 33 | 75 slots |
| 100 | refused, "control flow" |

## The values do not reach the driver yet

MEASURED on the M9128 on 2026-09-18: every specialised variant compiles
against zeros, whatever the application set.  `m9-mesa-glsl loopuni:5` renders
the value the loop starts from, and the driver's own NIR dump under
`M9X_DEBUG=1` shows the loop already gone with no iteration in its place.

The cause is in `src/mesa/state_tracker`, not in the driver.  st reads
`prog->info.num_inlinable_uniforms` in `st_atom_constbuf.c` before it will
call `pipe_context::set_inlinable_constants`, and `prog->info` is copied from
`prog->nir->info` at link time (`st_glsl_to_nir.cpp`).  `screen->finalize_nir`
-- where `m9_fs_find_inlinable_uniforms` sets that field -- runs at link time
only when the screen sets `caps.call_finalize_nir_in_linker`; otherwise
`st_create_fp_variant` calls it after the copy, so the field is always zero in
`prog->info` and `set_inlinable_constants` is never called.

The driver keeps `ctx->fs_inline` zeroed in that case and hands the zeros to
`m9_fs_inline_uniforms`, so a specialised variant is silently wrong rather
than refused.  That affects the pre-existing `if` specialisation as much as
the loop one.

The fix is one line in `m9_screen.c`, next to the other caps:

```c
   caps->call_finalize_nir_in_linker = true;
```

Until it is applied, `m9-mesa-glsl loopuni:N` is dispatched but not listed in
the suite, and `loop:fragment-uniform=true` renders the wrong number of
iterations instead of being refused.
