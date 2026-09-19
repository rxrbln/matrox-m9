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
(`m9-fs-compile-test --glsl`, `M9_FS_INLINE=<the dword st sends>`, which is
the float encoding of the bound -- see the next section):

| `FragmentLoops` | dword | result |
|---:|---|---|
| not specialised | -- | refused, "control flow" |
| 0 | `0x00000000` | 9 slots |
| 5 | `0x40a00000` | 19 slots |
| 31 | `0x41f80000` | 71 slots |
| 32 | `0x42000000` | 73 slots |
| 33 | `0x42040000` | 75 slots |
| 100 | `0x42c80000` | refused, "control flow" |

## What the uniform's value is, and what the shader reads

The screen advertises no integer support (`shader_caps.integers = false`,
`nir_shader_compiler_options.no_integers`), and Mesa answers that by storing
an integer uniform in constant buffer 0 **as a float**: `uniform_int_float`
in `src/mesa/main/uniform_query.cpp` writes `(float) *isrc`, so
`FragmentLoops = 5` is the dword `0x40a00000`.  A driver that declares
`no_integers` is the one that lowers its own NIR to match -- gallivm does it
for the vertex path the draw module runs (`lp_bld_nir_soa.c`,
`nir->options->no_integers`).  The fragment path did not, so its NIR still
compared with `ige` and read that dword as the integer 1084227584.

MEASURED on the M9128 on 2026-09-19, `M9X_FS_INLINE_TRACE=1` under
`loop:fragment-uniform=true`:

```text
m9x: fs variant: 1 inlinable uniform(s) dw0=40a00000
m9x: context 0: 3446 draws skipped: control flow
```

So the plumbing was already whole -- one inlinable uniform found, its value
delivered -- and the value was the float encoding of 5 while the loop wanted
the integer.  A trip count of 1084227584 is far past
`max_unroll_iterations`, so the loop survived and the shader was refused.

`m9_fs_lower_integers()` runs `nir_lower_int_to_float` on a fragment shader
whose options say `no_integers`, which is what closes it.  One repair goes
with it: NIR types a UBO load's element index as an integer, so the pass
converts the constant indices of a matrix uniform's columns too, and the
index is put back with `f2i32` and folded -- the same fixup gallivm carries
as `lp_nir_no_integer_intrinsic_fixup`.  Without it the `matuni` fixture
resolves constants a gigabyte past the bound buffer (MEASURED: suite 227 of
228, "fragment constants outside the bound constant buffer").

OFFLINE afterwards, `m9-fs-compile-test --glsl` on the glmark2 loop shader
with the value st actually sends:

| `M9_FS_INLINE` | meaning | result |
|---|---|---|
| not specialised | -- | refused, "control flow" |
| `0x40a00000` | float 5.0 | 19 slots |
| `0x41000000` | float 8.0 | 25 slots |
| `0` | float 0.0 | 9 slots |

`caps.call_finalize_nir_in_linker` is set, which is what gets
`prog->info.num_inlinable_uniforms` filled before `st_atom_constbuf.c` reads
it; `loopuni:0`, `loopuni:5` and `loopuni:24` are listed in the fixture
suite.
