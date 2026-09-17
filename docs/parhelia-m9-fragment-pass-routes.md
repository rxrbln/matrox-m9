# M9 inter-pass record: descriptor and route word field layout

`docs/parhelia-m9-fragment-pass-splitting.md` section 4 establishes that a
fragment shader split across raster passes hands values over through a
per-fragment record in memory, and leaves the field boundaries inside the
route word **OPEN**, advising a scheduler to emit the captured triples
verbatim.  This note closes those boundaries.  The model below reproduces
**every** record descriptor and route word in both independent captures --
the seven two-pass GLSL cases and the five-sampler schedule, twenty values in
all -- and is checked by `test_record_routes` in
`src/gallium/drivers/m9x/m9_batch_test.c` (OFFLINE).

Labels: **CAPTURED** read from a saved stream, **DERIVED** a join, **OPEN**
not decoded.

## 1. The descriptor

A record descriptor appears as the producing pass's `b1` and as the consuming
route's `0x79`.  It is not an ordinary surface descriptor: its low sixteen
bits hold no row span (DERIVED).

```text
bits 31:25  0x77   record format class, every captured value (CAPTURED)
bits 24:21  0
bits 20:19  form, below
bits 18:16  1      the layout every captured value carries (CAPTURED)
bits 15:8   the dword inside the record the operand starts at
bits  7:0   the record's stride in dwords
```

so `m9_record_desc(form, stride, offset) = 0xee000000 | form << 19 |
0x00010000 | offset << 8 | stride`.

The `form` field takes three of its four values across the captures
(DERIVED; the fourth is unobserved):

| form | descriptor prefix | where it appears |
|---:|---|---|
| `1` | `ee09` | a two-dword operand: the two-scalar producers (`fs-tex2d-swizzle`, `fs-texrect`) and every five-sampler consumer |
| `3` | `ee19` | a four-dword producer slot, and a consumer whose third component is a **sampler modifier** -- the LOD bias of `fs-tex2d-bias`, the projector of `fs-tex2d-proj3` |
| `2` | `ee11` | a consumer whose third component **addresses the resource** -- the comparison reference of `fs-shadow2d`, the `r` of `fs-tex3d`, the major axis of `fs-texcube` |

The pass-splitting note reads bit 20 as moving with the record size.  That
holds for producers but not for consumers: the five-sampler consumers read a
sixteen-dword record with bit 20 **clear** (CAPTURED), so the field is a form
selector, not a size (DERIVED).

## 2. The route word

The word emitted at `0x74 + k`, immediately before that route's `0x78` base
and `0x79` descriptor:

```text
bits 31:24  component 3 source
bits 23:16  component 2 source
bits 15:8   component 1 source
bits  7:0   0x10 a coordinate route, 0x0c a value route, 0x00 idle
```

Component 0 comes from the descriptor's own start dword; components 1 to 3
are **record dword indices**, and `0xff` drops a component (CAPTURED, the
`00ff` of every two-component coordinate).  An idle route carries the
captured `0x00000100`.

A coordinate route (`0x10`) feeds the full sampler slot it precedes; a value
route (`0x0c`) arrives in the consuming pass as the register pair a texel
would, `(0x00 + 4k, 0x02 + 4k)`.

Reading the captures with that model:

| case | descriptor | route | components |
|---|---|---|---|
| `fs-tex2d-swizzle`, `fs-texrect` consumer | `ee090002` | `00ff0110` | `(d0, d1)` |
| `fs-tex2d-bias`, `fs-tex2d-proj3` consumer | `ee190204` | `01000310` | `(d2, d3, d0)` |
| `fs-shadow2d`, `fs-tex3d` consumer | `ee110204` | `01000310` | `(d2, d3, d0)` |
| `fs-texcube` consumer | `ee110004` | `03020110` | `(d0, d1, d2)` |
| five-sampler passes 1..4 | `ee0900{00,08,04,0c}10` | `00ff0{1,9,5,d}10` | `(dn, dn+1)` |
| five-sampler final, route 1 | `ee190210` | `0706030c` | `(d2, d3, d6, d7)` |
| five-sampler final, route 2 | `ee190a10` | `0f0e0b0c` | `(d10, d11, d14, d15)` |

The five-sampler record is sixteen dwords wide and each of its four producing
passes owns a four-dword slot, which is why the final pass can still gather
`a` from dwords 2..3 and `b` from dwords 10..11 after four passes have run
(DERIVED, and it is what the shader needs: `a.r + d.r`, `b.g + e.g`, `c.b`).

## 3. What stays open

* How the engine addresses the record **per fragment** -- linear in fragment
  order, or by screen position with an implied pitch -- is **OPEN**.  The
  descriptor carries no pitch and no extent, and the five-sampler capture
  draws only 64x64, so no capture separates the two.  A driver must measure
  this before it can size the record for a full-screen target; until then it
  allocates a bound that covers both walks.
* The unobserved fourth `form` value is **OPEN**.
* Whether a consuming pass may declare interpolated inputs *and* a route at
  the same time is **OPEN**, as the pass-splitting note already records.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
