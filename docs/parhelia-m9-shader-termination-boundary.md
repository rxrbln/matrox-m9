# Matrox M9 native shader termination boundary

This note isolates the last unknown in M9 native-program execution.  The saved
compiler and submission paths determine what does **not** stop execution and
identify one invariant endpoint selector.  They do not distinguish whether the
hardware stops on that selector alone or on the complete final-MOV operation.

## Bounds that do not reach hardware execution

The MxMD native emitter maintains an explicit slot count at output offset
`+0x04`.  Packer `0x1db98d..0x1db9a5` refuses an append beyond 256 slots,
writes two dwords at `+0x08 + 8*count`, and increments the count.  Compiler
clone and rewrite passes also iterate against that count.  No native sentinel
is inserted by the packer [BIN].

After linking, Windows program state `+0xbb0` and Linux state `+0xbd8` hold an
aligned byte length.  Both uploaders divide it by 32 for command reservation,
then use it only to bound selector-`68`, ports-`60..67` writes.  The source is
advanced by 32 bytes per line.  Zero slots produced by alignment are therefore
transport padding [BIN][DER].

The final inline bind has only two 16-bit instruction starts.  Windows
`0xf7819..0xf78ad` and all sixteen saved Linux `m9x_drv.so` variants emit the
same choice [BIN]:

```text
packed: 69 <- 4; 6a <- primary_start | companion_start << 16
split:  6d <- companion_start; 69 <- 4; 6a <- primary_start
```

The Linux inline-bind core is byte-identical across releases `1_9_0` through
`1_15_0` and `6_9_0` through `7_5_99`: each 165-byte region has SHA-256
`6cd8fc0db33496a09e55a813626f95ed1ce74908feaf67282713caa56bc64031`.
It reads starts at `+0xbd0/+0xbd4` and contains no reference to byte length
`+0xbd8` or CPU pointer `+0xbe0`.  The external-resource form likewise writes
`69`, `6a`, and `6d` from the relocated program resource and start fields,
without emitting a length or end address [BIN].

Consequently these values cannot be hardware execution bounds:

* the compiler slot count;
* the aligned upload byte count;
* zero-filled upload padding;
* the next program's entry address;
* compiler property `+0x397` bit 29, whose only joined consumer is a rewrite
  barrier.

Literal `69 <- 4` selects the inline-program form; it is constant while program
length varies and cannot encode the instruction count [DER].

## The invariant final endpoint

The mandatory output finalizer at `0x1c8590` bridges a normal stage-relative
final result to an internal endpoint before returning.  For the one-output
case its last generic instruction is exactly [BIN]:

```text
opcode       1                    D3D MOV
destination  0x3001fffe
source       0x0000fff8
```

The multi-result form uses destination `0x3003fffe` and source `0x0044fff8`.
The alternate component path uses destination `0x3002fffe`.  Thus all three
endpoint destinations have low seven bits `0x7e`, while the stage-relative
source has low seven bits `0x78`.

Generic lowerer `0x1b5390` masks off component bits while preserving the
destination's low seven bits, installs it in native object `+0x1c`, and sets
format gate `0x04`.  Packer `0x1db902..0x1db914` responds to that gate by
copying `+0x1c & 0x7f` into native `W0[14:8]`.  Therefore the generated final
operation writes physical selector `0x7e` [BIN][DER].  The finalizer returns
immediately after appending this bridge; it adds no separate `END` slot.

This narrows the native stop rule to two possibilities:

1. completion is implicit when an instruction writes endpoint selector
   `0x7e`; or
2. completion is encoded by the full operation/control combination of that
   final MOV, with selector `0x7e` as its destination.

Raw appearances of `W0[14:8] == 0x7e` in fixed fragments cannot choose between
them because the packer's fields overlap by format.  A raw bit slice is a
destination only when the compiler set gate `0x04`.  The fixed-fragment tails
also have no universal bit or universal 64-bit slot.  Calling `0x7e` an `END`
opcode would therefore exceed the static evidence.

## Smallest decisive capture

Capture one passthrough vertex shader and one solid-color fragment shader at
the native packer, recording each append at `0x1db99b` and the output count at
`+0x04`.  Also record compiler context `+0x1f0/+0x1f4`, because those values
select the two possible MOV lowerings.  Stop before the linker pads the stream
to four-slot upload lines.  For each stage retain:

```text
stage, unpadded slot count, every (W0,W1), final endpoint slot, entry start
```

Those two traces provide the exact terminal sequence needed for a first
open-driver shader without assigning speculative bit names.  To distinguish
the two hardware rules later, take the captured final MOV, preserve all of its
operation/control fields, retarget only its gate-proven destination from
`0x7e` to a known scratch selector, and append the original endpoint MOV one
slot later.  Successful completion at the later slot proves endpoint-triggered
termination; completion before it proves an operation/control trigger.  Run
that mutation only under the recoverable opt-in test module.

`tools/m9-shader-termination-evidence.py` verifies the compiler endpoint,
counted host stream, Windows bind, and the byte-identical bind/upload evidence
in every saved Linux render-driver variant.
