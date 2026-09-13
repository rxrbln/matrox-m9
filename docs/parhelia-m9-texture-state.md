# Matrox M9 texture and sampler state

This note joins Direct3D texture/sampler frontend state to the final M9 indexed
packet emitter in Windows `MxMD.dll`.  The emitter structure and constants are
cross-checked where possible against Linux `m9x_drv.so`.  `[BIN]` means direct
binary data flow, `[DER]` a deterministic decode, and `[INF]` a provisional
semantic name.

## Object path and final packet

Frontend parser `0xd0770` calls D3D texture-stage dispatcher `0x105340` at
`0xd0976`.  Fixed TSS objects begin at `ctx+0x2520 -> +0x3c`, with 28 stages at
stride `0x60`.  Programmable stages 257..272 use dispatcher `0x105cf0` and
samplers at `ctx+0x2518 -> +0x14`, stride `0x74`.  Fixed sampler dispatcher
`0x105850` instead uses `ctx+0x2520 -> +0x2c + stage*0x4a` [BIN].

Lowering `0x17faf0` enumerates units into `0x187860`.  Its working record binds
backend at `+0x0c`, sampler at `+0x24`, fixed TSS at `+0x28`, resource at
`+0x1c`, and input descriptor at `+0x14`.  Selector builder `0x187950`, resource
format join `0x187b50`, central lowerer `0x18b4e0`, and filter helper `0x18afb0`
produce the four 0x64-byte backend blocks initialized by `0xf4120` at batch
offsets `+0x4b4,+0x518,+0x57c,+0x5e0` [BIN].

Final emitter `0xf4350` maps each backend exactly:

| dirty group | final indices | backend fields |
|---:|---|---|
| common | dynamic `70 + unit` | `+0x10` |
| `0x02` | `78,79,7a` | `+0x4c,+0x50`, relocated resource `+0x44/+0x48` |
| `0x04` | `7c,7d,87` | `+0x14,+0x18,+0x54` |
| `0x08` | `82,83,84,85` | `+0x2c,+0x30,+0x34,+0x38` |
| `0x10` | `80,81,7e,7f` | `+0x24,+0x28,+0x1c,+0x20` |
| `0x20` | `c0` | resource `+0x3c/+0x40` |

It rewrites `f0` bits `14:13` with the unit and uses relocation selectors
`unit+0x0b` and `unit+0x0f`.  Comparator `0xf41f0` establishes dirty groups;
group `0x08` is conditional on backend `+0x10 & 0x3000 == 0x2000`.  Field
`+0x54`/index `87` is initialized, compared, and emitted, but the central
lowerer never writes it.  Keep it reserved.

## Direct3D sampler shadow

Dispatcher `0x105850` packs these public states [BIN]:

| sampler state | shadow field | dirty group |
|---|---|---:|
| ADDRESSU/V/W | `+0x08` bits `2:0/5:3/8:6` | `0x02` |
| BORDERCOLOR | raw `+0x2e`, clear `+0x3e` | `0x04` |
| MAGFILTER | `+0x08[18:15]` | `0x08` |
| MINFILTER | `+0x08[22:19]` | `0x08` |
| MIPFILTER | `+0x08[24:23]` | `0x08` |
| MIPMAPLODBIAS | float `+0x18` | `0x10` |
| MAXMIPLEVEL | `+0x1c` | `0x10` |
| MAXANISOTROPY | `+0x46[4:0]` | `0x08` |

Private state 29 sets `+0x46` bit 5; states 33/34 select a special border source
through `+0x08` bit 14 and `+0x42`; state 35 sets `+0x46` bit 10; state 36 sets
bit 11; and state 37 sets bit 12, stores low six payload bits at byte `+0x2c`,
bits `7:6` at byte `+0x2d`, and sets bit 13 when byte `+0x2c` is nonzero.
Programmable dispatcher `0x105cf0` mirrors these sampler fields.

## Final filter/wrap word: index `7c`

Backend `+0x14`, emitted at index `7c`, is packed exactly as follows [BIN][DER]:

| bits | source |
|---:|---|
| `2:0` | mapped MIN filter |
| `6:3` | mapped MAG filter |
| `8:7` | mapped MIP filter |
| `10` | target/fast-path condition, exact name unresolved |
| `12` | forced one |
| `13` | input-descriptor condition |
| `15:14` | resource conditions |
| `17:16` | mapped anisotropy |
| `21:18` | resource-input `+0x0c` low nibble |
| `24:22` | mapped ADDRESSU |
| `27:25` | mapped ADDRESSV |
| `30:28` | mapped ADDRESSW |
| `31` | border/color conversion condition |

The literal tables are:

```text
MIN/MAG D3D enum 0..8 -> 0,0,2,3,0,0,4,4,7
MIP     D3D enum 0..2 -> 0,1,2
WRAP    D3D enum 0..6 -> 0,0,4,1,7,5,6
ANISO   0..16          -> 0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2,3
```

Thus anisotropy 1..3 selects 0, 4..7 selects 1, 8..15 selects 2, and 16 selects
3.  Wrap values identify D3D WRAP=0, MIRROR=4, CLAMP=1, BORDER=7, and
MIRRORONCE=5.  Windows table addresses are `0x1f6300` MIN/MAG, `0x1f6348` MIP,
`0x1f639c` wrap, and `0x1f63b8` anisotropy.

Linux 1.15 contains byte-identical tables at file offsets near `0x10723a0`,
`0x1072480`, and `0x10724a0` (VA plus `0x200000`).  No usable live code xrefs
were found, so this corroborates constants rather than a Linux frontend path.

## LOD, border, bump, and compare

Backend `+0x18`/index `7d` is built by `0x1366f0` and `0x18ba27` [BIN]:

| bits | value |
|---:|---|
| `9:0` | signed LOD bias clamped to `[-512,511]` |
| `18:10` | first 9-bit bound clamped to `[0,511]` |
| `27:19` | second 9-bit bound clamped to `[0,511]` |

Backend `+0x10` bit 19 enables this control.  Sources include sampler LOD bias
`+0x18`, max level `+0x1c`, and derived bounds `+0x24/+0x28`; orientation of the
two bounds remains unresolved.

At `0x18be15`, border color from sampler `+0x2e` or special source `+0x42` is
converted through `0x187bc0/0x1897e0` into backend `+0x1c,+0x20,+0x24,+0x28`.
Those four dwords are emitted at indices `7e,7f,80,81`; wrap outputs at least 6
gate their comparison/emission.

Indices `82..85` are definitively `D3DTSS_BUMPENVMAT00/01/10/11`, converted at
`0x18c148` from fixed-stage floats `+0x08,+0x10,+0x0c,+0x14`.  They are not
dimensions.  `BUMPENVLSCALE/OFFSET` at stage `+0x18/+0x1c` are consumed by a
different path.

Shadow comparison at `0x18bf38` requires sampler `+0x46` bit 8 and sampler
`+0x08` bit 13, sets backend `+0x10` bit 26, and puts this mapped D3D comparison
in bits `29:27` [BIN][DER]:

```text
NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS
    0    1     3      2       6        4     5      7
```

## Target and resource descriptor

Selector builder `0x187950` puts target/resource-family values
`1,8,9,c,d` in backend `+0x10[11:8]`, based on input descriptor flags and
resource virtual method `+0x48`.  Depth lowering adjusts that nibble for format
variants.  Cube-versus-volume names cannot yet be assigned safely.

The live resource vtable is `0x1f3a90`, installed at `0xa3323/0xa347a` [BIN].
Relevant slots are:

| slot | function | result |
|---:|---:|---|
| `+0x38` | `0xa6660` | target class |
| `+0x3c` | `0xa6d30` | normalized format ID at resource `+0x90` |
| `+0x48` | `0xa6d40` | resource `+0xb0` kind mapped to `0..5` |
| `+0x54` | `0xa7a60` | resource `+0xb0` bit 7 |
| `+0x5c` | `0xa6740` | relocation plus descriptor pair, return `0x10` |
| `+0x64` | `0xa7a40` | resource `+0x68` |
| `+0x74` | `0xa5e80` | further resource property |
| `+0x78` | `0xa7a10` | resource `+0xd0` |
| `+0x7c` | `0xa7a20` | resource `+0xc8` |

Method `0xa6740(selector=2, reloc, idx78, idx79)` selects cached descriptor
triple 2.  Cache builder `0x467e0` and format helper `0x47350` establish the
ordinary descriptor layout exactly [BIN][DER]:

| output | bits | source |
|---|---:|---|
| `idx78` | `15:0` | cache source `+0x184` |
| `idx78` | `18:16` | source `+0x138[2:0]` |
| `idx78` | `20:19` | source `+0x13c[1:0]` when kind is 2 |
| `idx78` | `21` | source flags dword 0 bit 3 |
| `idx78` | `23` | source `+0x14c` bit 2 |
| `idx78` | `31:25` | source `+0x140[6:0]`, hardware format code |
| `idx79` | `27:16` | source `+0x130[11:0]`, or `+0x134` for format `0x3f/0x7f` |

Ordinary selector 2 leaves `idx78` bits 22/24 and `idx79` low 16 clear.  Kind 1
replaces `idx79[27:16]` with 1; kind 3 uses a special tiling/stride calculation.
The triple's first dword is the BO address with low-two attribute bits, followed
by `idx78` and `idx79`.  With no backing, the relocation fallback uses tag
`0x20000000 | resource+0x70`.

Fields `+0x184` and `+0x130/+0x134` are size/layout derived, while `+0x13c`
values 1..3 distinguish resource classes.  Static data flow does not yet prove
which is width, height, pitch, or which class is cube versus volume.  Defaults
before resource fill are `idx78=0xcc010001` and `idx79=0x00010001`; function
`0x18b7f4` may clear `idx78[20:19]` based on target family.

Compression is proven upstream.  Format helpers near `0x4ee70/0x4fd10` map
FourCC DXT1 to internal 0, DXT2 to 2, DXT3 to 3, DXT4 to 4, and DXT5 to 5.
Descriptor initialization near `0x4fdf0` assigns DXT1 type 1/code `0x10` and
auxiliary size field 4; DXT2/3 code `0x15` and auxiliary field 8; and DXT4/5
code `0x17` and auxiliary field 8.  Those values are not byte counts; they are
likely dword or half-block units [INF].
Layout helper `0xa3ba0` applies minimum-four block geometry and the format's
width/stride transforms.  Constructor `0x45eb0` stores the format descriptor's
code in cache source `+0x140`, and ordinary selector 2 copies it unchanged to
`idx78[31:25]`.  Thus DXT1 reaches the hardware field as `0x10`, DXT2/3 as
`0x15`, and DXT4/5 as `0x17`; the complete non-DXT format table is unresolved.

Constructor `0x45eb0` also stores two caller dimensions at source
`+0x128/+0x12c` and aligned/layout-adjusted forms at `+0x184/+0x188`.  The
simple path aligns the first dimension to eight and the second to a
format-dependent one/four boundary.  Selector 2 copies `+0x184` to `idx78`
low 16 but does not copy `+0x188` into ordinary `idx79`; selector 1 does.  The
custom value at `+0x130/+0x134` reaches `idx79[27:16]`; ordinary 2D constructor
caller `0x51bd2` supplies 1, strongly suggesting depth/layer count.  Width
versus pitch naming for `idx78` low 16 still requires capture confirmation.
