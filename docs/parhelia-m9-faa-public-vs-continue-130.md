# M9128 public VS continuation after real 0x100

The original M9128 public VS DRAW was resumed only with the distinct real
0x100 hardware receipt from boot `46b862d5-5a0b-4342-aff7-63dc1567b55c`.
The receipt binds the exact 796-byte source input and original public VS
program, constants and geometry. The original caller advanced from its unread
0xf0 wait to another unread wait at 0x120; it did not return from DRAW or
invoke Lock. A separate, genuine vendor queue finalizer then emitted one END
and stopped before its status load at 0x130. Neither 0x120 nor 0x130 was
provided as a GPU response, and no pixels were supplied.

The CPU-produced continuation has owner 29 secondary bytes `[752, 1368)`:
600 bytes before the original 0x120 wait and 16 bytes from the independent
finalizer. Owner 27 primary bytes `[4992, 5040)` are a 48-byte END footer.
Its first 12 bytes contain a fixed indirect opcode and two endpoints into
owner 29, encoded with tags 1 and 2. The only prior command mutation changes
owner 29 word `+704` from `0x49d00000` to `0x49d00091`: the low field is
`(1284 - 704) / 4`, a same-owner relative target. This must be validated
and patched before posting the new END. The source writes three completion
tokens at owner 29 `+796`, `+1348` and `+1364`, for 0x110, 0x120 and 0x130
respectively, each encoded as the low point plus one. It writes the END
register once, ending at source primary offset 5040. The native cursor must
come from the retained live primary, including any earlier dynamic clear
delta; the source offset is only a profile reference.

The [typed plan](../captures/m9-faa-graph-public-vs-20261007/continued-prefix-plan.json)
and [stripped template](../captures/m9-faa-graph-public-vs-20261007/continued-prefix-template.bin)
separate 12 address fields, three timeline fields, two null fields and the
literal control word from source bytes. Native code must resolve each address
against its retained live owner, validate domain, capacity, alignment and
generation, and use the current timeline epoch. The owner 29 word at `+1320`
is the next deferred marker; source supplies no payload for it here. The
three `0x40004000` words at `+1292/+1296/+1300` are fixed instruction
immediates from the exact original M9128 DLL (`mov eax,0x40004000` at
`0xfa227`, stores at `0xfa22c/0xfa235/0xfa23e`), not owner addresses.

This is a bounded fixed-input CPU source plan for the retained public VS
graph. It does not qualify application state, the raster tail, Lock, 0x130
retirement or pixels. A native producer needs all six graph-owner write
fences before the old-word patch, new uploads and END post, then an actual
completion receipt before advancing the original caller. The source trace
is [archived](../captures/m9-faa-graph-public-vs-20261007/continued-actual100-prefix.json);
`check-continue-prefix.py` verifies the exact source image, rebuilt bytes,
four distinct live placements and refusal cases.

The follow controller accepts a *single new-boot* capsule for INIT, public
VS prefix 0x100 and follow 0x130. It requires the exact 796-byte input,
staged module/helper hashes supplied explicitly, the 664-byte follow
firmware, state 10 then state 11, both intermediate and final transport
snapshots, the native 5044-byte/four-post/relative-word line and final four
counter lanes ending in 0x131. It rejects the earlier 0x100-only capsule.
The host admission test uses a temporary parser-only synthetic fixture;
those bytes are never supplied to the emulator or treated as hardware proof.
After a genuine 0x130 receipt arrives, run:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
  tools/m9emu/m9_faa_graph_public_vs_continue.py \
  --init-receipt-dir tests/m9-faa-graph-init-hardware-20261007/confirmed-retirement \
  --follow-receipt-dir /absolute/path/to/new-boot-proof \
  --follow-module-sha256 STAGED_MODULE_SHA256 \
  --follow-helper-sha256 STAGED_HELPER_SHA256 \
  --out /tmp/m9-public-vs-actual130-source.json
```
