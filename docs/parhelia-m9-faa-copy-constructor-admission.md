# Earlier mode admission for the PDEV conversion constructor

The original mode caller and selection consumer now execute with source-known
CPU inputs. The real retained mode collection has **zero modes**, so its
selection returns error11 and the fixture deliberately leaves `86660`
uncalled. This is an emulator initialization boundary, not a demonstrated
cause of the native FAA image failure. No GPU completion or pixels were
supplied. Existing hardware1d0/1f0 retirement results remain separate.

The shipping image hashes and eight decoded spans are checked by
`tools/check-m9-faa-copy-constructor-admission.py`. The source ordering is:

1. Original GDI caller `105f0` reads DEVMODE bpp/width/height/Hz at
   `10618/10631/10627/1063b`. Its32-bpp table branch at `1067b` emits code64.
   The fixture executes this caller and stops at its actual `2dea0` call before
   another PDEV initializes. It retains the produced CPU request before a
   later emulator call can reuse its stack.
2. `2dea0` calls `64bd0` at `2df09`, before mode setup and the module loop.
   `64bd0` requests `2334d8`, prepending `ffffffff` to the actual six-DWORD
   caller request. `MxMM.sys:60263/60268` registers `6053c` for this request;
   its `6056d` call enters the real `6e049` mode selector.
3. `6e061` obtains the original mode count via `6dd52`. The observer at
   `6e066` sees AL1/count0. The `6e0bf` loop therefore visits no modes and
   `6e16f` produces error11. The real reply is status11/index0; transport
   success0 and returned8 bytes do **not** indicate selection success.
   `64bd0` propagates11. Mode setup and the constructor stay uncalled.
4. Only a successful selection may feed its live index to `2ff50`. The latter
   issues real `232014`, `2334cc`, and `2334c8` requests before publishing mode
   fields. `300e5..300f6` derives `+78=4` bytes for32 storage bits;
   `3024a/3024f` publishes numeric format66 to `+80`. It is not a pointer.
   Only successful mode setup permits the real `86660` constructor, whose
   three wrappers must be retained before original `b1290` loads them.

The mode selector consumes only the first four caller request DWORDs
(width,height,format-enum,Hz). The final two remain the original live caller
stack bytes; the fixture does not replace them with captured values. It sends
the source-produced packet through actual `6c755` dispatch, preserves the real
result and output-byte count, and propagates only actual CPU stores to existing
shared owners. It never bulk-refreshes a shared allocation over MD writes.

`tools/faaemu/m9_pdev_copy_resources.py` refuses existing mode/conversion slots
or a published queue. It restores the previous I/O handler on every exit.
There are no forced d0/d4/d8, wrapper readiness, conversion result, mode count
or mode records. The observed request changes CPU allocations and startup
history, so prefix/resource-effect equivalence is explicitly **not proved**.
The old hardware receipt is not applicable to this fixture. A successful future
constructor requires a separately exported and qualified whole fixture.

## Reproduction

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-vendor-faa-copy-constructor.py \
  --out /tmp/m9-faa-copy-constructor-20261007.json --require-constructor
# Expected exit1: genuine selection error11, constructor not admitted.
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-copy-constructor-admission.py \
  --proof docs/traces/m9-faa-copy-constructor-admission-20261007.json
```

The retained proof hashes its implementation modules, vendor images and actual
PCI input artifacts. It records unchanged zero mode/conversion slots and zero
mode-device MMIO events. The checker rejects six contradictory admission or
receipt claims. These controls verify source ordering and the failed admission;
they do not stand for successful constructor execution or pixel readback.

The next missing prerequisite is the **genuine producer of the nonempty mode
collection** consumed by `6dd52/6ddef`, with its original caller inputs and
earlier startup order. Supplying a fabricated count, injecting a format code,
or creating wrappers after the pending wait would bypass that prerequisite.
