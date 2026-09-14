# Matrox M9 requester rollover and the `0x404` doorbell

This note separates two register interfaces which use related command-buffer
addresses but are not one ring descriptor.  Evidence comes from the saved
Linux `KernelLibraries.o_shipped` and Windows `MxMM.sys`; addresses below are
ELF text offsets or PE virtual addresses.  `[BIN]` denotes direct disassembly
evidence and `[DER]` its narrow consequence.  No hardware was used.

## `0x404` is an endpoint doorbell, not a limit

Linux `m9x109` at `0x2d150..0x2d1bd` translates the command object's CPU start
pointer, stores the resulting GPU address in the primary allocation object at
`+0x50`, clears its low two bits, adds the backing class from resource `+0x34`,
and writes the result to MMIO `0x400` [BIN].  At submission, `m9x1039`
`0x2d7d7..0x2d83f` translates the final CPU producer, stores that endpoint in
the same `+0x50`, and writes it to MMIO `0x404` [BIN].  Thus the two registers
are a tagged start/control value and a final executable address.  There is no
length or upper-bound value written to `0x404` [DER].

The compare immediately before the Linux doorbell is software allocation
bookkeeping, not an address comparison [BIN]:

```
primary = command->allocator->primary       // command +48, allocator +10
primary->submitted_endpoint = translate(command->producer) // primary +50
if (primary->active_nodes <= command->node_allowance)
        MMIO[0x404] = primary->submitted_endpoint;
command->node_allowance = 0;
```

The field names describe only the demonstrated roles. `m9x669` initializes
the primary object's `+0x54` to zero, and allocation methods `m9x110` and
`m9x599` increment it once per node linked into the circular list rooted at
`+0x68`; teardown methods return it to zero.  The command constructors
`m9x654` and `m9x1086` initialize command `+0x58` to zero, and `m9x1039`
clears it after the comparison.  The saved ELF has no direct nonzero
immediate-displacement store to this command field.  Static evidence does not
identify the path, if any, that grants a nonzero node allowance.  A new driver
must not translate this comparison into a hardware ring-size rule [DER].

Windows independently fixes the endpoint meaning without this Linux-only
allocator policy.  `MxMM.sys` `0x224c8` copies CPU start to current producer
and writes object `+0x24` to MMIO `0x400`.  Function `0x224f3` computes
`current_producer - cpu_start + gpu_base` from object `+0x2c/+0x20/+0x24` and
writes exactly that address to `0x404` [BIN].  It performs no access to
`0x500..0x524` and no size comparison.

## The `0x500..0x524` requester rolls independently

The requester descriptor's software-owned words have a different producer
chain [BIN]:

- Linux `m9x759` `0x2b290..0x2b380` writes a tagged transition/base address to
  `0x500`, zero to `0x508`, and the raw live pointer to `0x504`.
- Linux `m9x620` `0x285d0..0x28799` appends either a 16-byte or 32-byte
  descriptor record when required, then publishes the translated software
  producer through `0x504`.
- Linux `m9x1200` `0x2b1c0..0x2b27a` publishes and quiesces, writes the
  two-dword transition record `{2, resource_gpu_base}`, resets the software
  start/producer and end guard to that resource's CPU mapping, republishes
  `0x504`, and quiesces again.
- The Windows equivalents `0x145a5` and `0x146bc` derive `0x500/0x504` from
  their requester object's aligned start, CPU mapping and GPU base.  They do
  not access `0x400/0x404`.

Neither rollover path writes `0x404`, and neither `0x404` submitter reads or
writes the descriptor words.  The only demonstrated relationship is that both
interfaces carry GPU addresses belonging to command transport [DER].  In
particular, `0x50c/0x510` cannot be named as mirrors or limits of `0x404`.

The exhaustive direct-register audit remains the static boundary for the five
hardware-owned descriptor words: outside restore, neither saved kernel binary
has a software write to `0x50c`, `0x510`, `0x514`, `0x520`, or `0x524`.
Quiescence samples `0x500` and `0x50c`; save/restore preserves all eight words;
Windows conditionally restores the `50c/514/510` triple when
`(saved_50c ^ saved_510) & ~0xf` is nonzero.  These facts show address-like
tagging and rollover-sensitive state, but do not reveal which word is a fetch
pointer, boundary, or pending transition [DER].

## Smallest capture that resolves the remaining fields

Instrument MMIO reads/writes and force exactly one `m9x1200` requester wrap
with a fence after it.  Four timestamped samples are sufficient:

1. quiescent state before the final pre-wrap descriptor;
2. immediately after its `0x504` publication;
3. immediately after the transition record and post-wrap `0x504` publication;
4. quiescent state after the fence retires.

Each sample must contain `0x400`, the most recent value written to `0x404`, all
eight words `0x500..0x524`, the software CPU start/producer/end-guard pointers,
the backing GPU base/class, and the two dwords at the old-ring transition
record.  Record the two `0x504` writes and any `0x404` write in order.  A single
wrap distinguishes stable limits from moving fetch/current pointers; the
pre-publication and post-fence samples distinguish software publication from
hardware consumption.  No rendering workload or second wrap is needed unless
the first wrap leaves `0x50c` equal to `0x510`; in that case repeat once with
the ring base chosen across a different low-nibble tag boundary.

Until that capture, preserve `0x50c..0x524` as opaque saved hardware state and
keep requester rollover separate from construction of the `0x404` endpoint.
