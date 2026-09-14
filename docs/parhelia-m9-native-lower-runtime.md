# M9 native lowerer execution corpus

This note records a controlled execution of the saved Windows M9 native
lowerers.  It complements the static packer analysis: the static code proves
each field and call path, while this corpus gives exact packed slots for
specific compiler inputs without requiring an M9 board or an MMIO capture.

## Isolation and pinned input

`tools/m9-native-lower-run.py` accepts only the saved `MxMD.dll` with SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
It builds a non-PIE i386 helper that maps the PE at its preferred base, supplies
the allocator vtable used by the audited compiler functions, and constructs
the exact nested output-vector ownership chain at compiler context `+0x41c`.
The helper exposes only these five known entry points [BIN][RUN]:

| source operation | lowerer | calling convention | packed slots |
|---:|---:|---|---:|
| MOV `1` | `0x1b5390` | two stack arguments | 2 |
| RCP `6` | `0x1b0840` | context in `ecx`, IR on stack | 6 |
| RSQ `7` | `0x1b1210` | context in `ecx`, IR on stack | 7 |
| EXP `14` | `0x1b1ba0` | context in `ecx`, IR on stack | 6 |
| LOG `15` | `0x1b2550` | two stack arguments | 8 |

The i386 helper must be built with `-O0`: these old routines use mixed custom
calling conventions and depend on the caller's literal stack layout.  The
launcher enforces that build mode.

## Canonical input

Every run starts with a zeroed `0x88`-byte backend IR record and sets:

```text
IR +0x0c = source operation
IR +0x14 = 0x000f0000       full xyzw destination mask
IR +0x1c = 0x10e40000       identity-swizzled source 0
IR +0x24 = 0x10e40002       identity-swizzled source 1
IR +0x2c = 0x10e40004       identity-swizzled source 2
IR +0x34 = 0x10e40006       identity-swizzled source 3
context +0x1f4 = 1          live Windows mode-6 selection
```

The destination and sources are deliberately synthetic internal descriptors.
The corpus proves the backend's output for this exact input.  It must not be
treated as a claim that every mask, descriptor class, predicate, or modifier
uses the same sequence.

## Exact native sequences

Each pair is `(W0, W1)` in execution order [RUN].

```text
MOV:
  ffc0001c a2803f1f
  ffc0811c a2803f1f

RCP:
  1fff801c ae203000
  000301bc 8c26ac10
  0040801c ae728300
  000301bc 8c26ac10
  0040801c ae728300
  0040811c ae728300

RSQ:
  ffc0001d a2803c0f
  002600bc 9e26b090
  003f811c ac200d00
  004080fd a2727f00
  003f811c ac200d00
  004080fc a2727f00
  004081fc a2727f00

EXP:
  1fff801c 9e851d40
  0027011c 982082d0
  0040811e a273ccf0
  0040811e a2740cf0
  007f801c ac2000c0
  007f811c ac2000c0

LOG:
  ffc0001d a2803c0f
  1fff801c 96852ec0
  1fc3001c ac800d10
  0029011c 944041e0
  0040821d a274c8a0
  0041021d a27508a0
  0041021d a27548a0
  0080011c 9c451f00
```

This closes the raw-byte question for four complex scalar lowerings and the
ordinary live MOV under one canonical descriptor configuration.  A first Mesa
backend can use these results as differential fixtures while its descriptor
builder is developed.  Driver emission should still be generated from typed
lowering rules rather than copying these literal sequences for arbitrary IR.

## Remaining compiler boundary

ADD, SUB, MAD, MUL, DP3, DP4, MIN, MAX, comparison, and texture operations do
not enter the generic MOV-style lowerer directly.  The saved whole-program
compiler first rewrites their IR and dispatches through additional operation
specific paths.  The next execution target is the common post-optimizer
operation dispatcher, or individually audited entry points with their precise
calling conventions and context fields.  Controlled mask, swizzle, modifier,
predicate, destination-class, and texture-unit matrices are still required to
turn this five-operation corpus into a complete semantic ISA table.
