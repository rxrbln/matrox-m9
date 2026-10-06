# Ordinary FAA output CPU-access preparation

The actual internal output helper reaches the ordinary SYSTEM mapper before
its first missing GPU input. Passive source hooks on the retained `101f8`
frame observe this sequence:

```
afbe0 -> 4d2b0 -> aff90 -> 2cb40 -> 555c0 -> 4e250
                                           -> 4e2fc common+20 read STOP
```

The last read is stopped before it executes. No completion, pixel value,
successful mapper return, CPU-copy return or temporary release is supplied.
This remains the private internal `ac1f0` experiment below unclosed public
`a74b0` admission. It does not establish an application Lock contract.

The retained temporary is logical format6, hardware format100 (`64`),
allocation kind1, 320x240 with 320x256/`50000` backing. At actual `afbe0`
entry its resource fields `268`, `2a4`, `328`, `32c`, `3d0` and `428` are
zero. Its copy direction is1, flags are0, and destination CPU stride is1280.
The actual `4d2b0` call returns before entering `aff90`; its optional access
layer (`268`) and resource-copy collection (`328/32c`) are absent. Thus those
conditional resource synchronization operations do not occur on this
temporary before the strict wait boundary.

The live queue has flags `+58=0`, so `aff90` skips its conditional vtable
`+58` call. The genuine constructor vtable `1f2c60` selects `249a0` for that
slot. Static decoding shows command-buffer context push/pop logic, rather
than a pixel decoder. No such call is executed by this observed CPU-copy
entry.

The following statements describe **static post-wait code**, not executed
continuation. `2cb40` uses the allocation-kind1 arm at `2cc97`, which selects
the retained CPU alias from resource `+1f0`. The eventual kind1 barrier call
at `2cc86` enters `1e5020`: a CPU `mfence`/serialization helper. The
host-aperture `27100/26c00` preparation at `2cbcd..2cc2e` is conditional on
allocation kind0 or6 and does not apply to this allocation-kind1 destination.
`2c980` likewise selects its kind1 `2ca7c` CPU-alias arm. These source branches
support the existing retained DMA-alias read design, but the capture does
not prove a returned copy, all subsequent wait behavior, or pixel visibility.

The first native ordinary output image remains black after real retirement.
Its FAA fragment and companion hashes match the preceding run exactly, so
this proof does not revive the disproved vertex-delivery hypothesis. The
ordinary output's raw DMA contents and source-owned command delivery remain
separate checks; this evidence supplies no new GPU rendering state.

The [access proof](../captures/m9128-device-inputs-20261006/normal-faa-output-access-preparation-101f8.json)
retains the exact source fields, six observed calls, live queue join, first
common-buffer input boundary and pinned instruction bytes. Its accompanying
source and contract files preserve actual CPU/MDL chronology. The checker
verifies their hashes, exact DLL instruction bytes, owner/shape joins and
absence of supplied GPU input. Observed resource fields are not promoted to
universally initialized bytes or physical-adapter capabilities.

Reproduce with `tools/m9emu/m9_faa_output_access_preparation.py`, passing
`--contract-driver tools/m9-vendor-faa-read-contract.py`, `--access-out`, and
the read-contract producer's actual input/tool/output arguments. Its passive
hooks forward the existing strict `Boundary`; they do not modify vendor code,
resource state, admission flags or the interrupted call's return. Verify with:

```
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-output-access-preparation.py \
  captures/m9128-device-inputs-20261006/normal-faa-output-access-preparation-101f8.json
```
