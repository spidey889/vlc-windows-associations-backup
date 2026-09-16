# UserChoiceLatest hash implementation

The original association backend, legacy hash implementation, and their tests
are MIT-licensed; see `../LICENSE`. VLC-specific UI/worker integration remains
GPL-2.0-or-later. The vendored codec retains its short source copyright notices
and one full MIT license in `LICENSE`, which also records its source provenance.

## Provenance and scope

This is an adaptation of the existing codec, not a new hash algorithm. `LICENSE`
records the source URL and pinned revision. Lookup data, token tags, seeds and
mixing constants retain their imported values. Local adaptations cover Windows
API/resource handling, thread-safe table initialization, alias-safe UTF-16/DWORD
buffers and checked encoding/length boundaries. `ComputeMd5Words` is also used
by the legacy hash implementation.

`ComputeHash` performs no registry I/O. The backend in
`../win32_associations.cpp` selects the format and assembles the class-specific
salt, field order and timestamp. The codec lowercases that concatenation with
explicit ASCII casing, preserving the pinned codec's C-locale bytes regardless
of the process CRT locale. Non-ASCII UTF-16 units are unchanged; Unicode-wide
lowercasing produces different ProgId hashes and must not be substituted.
Registry callers pass `lowercase_output = false`; the optional lowercase result
is retained for codec compatibility, not as a normalization of Base64 hashes.

## Byte-level invariants

- Input consists of Windows UTF-16 code units, not UTF-8 bytes or Unicode scalar
  values. Integer widths are checked in `HashCommon.h`; packed WORD bytes used
  by MD5 rely on Windows little-endian representation.
- The two encoding passes are deliberately distinct. The first requires full
  tokens and can emit non-repeat history references using lookahead. The second
  handles the remaining tail with fallback codes and flushes pending repeats.
  Both share one encoder state. Their similar-looking code is not interchangeable.
- The handoff offset is the sum of first-pass token lengths, not its final
  cursor position after rewinds. That count must stay within the original input
  so the second-pass slice cannot underflow or extend past the buffer. The
  629-unit repeating-input regression exercises this rejection boundary.
- First-pass history lookahead can request a rewind larger than the consumed
  prefix. Check the distance before forming the pointer and reject that input;
  clamping the rewind would silently change the encoded bytes.
- Entry zero holds encoder metadata. Entries are flattened into nonzero 16-bit
  chunks, low-to-high, followed by a terminator. MD5 hashes that entire stream;
  the two mixers process only its complete 8-byte blocks. Padding a DWORD buffer
  must not extend either input. The final hash is the Base64 encoding of the
  little-endian XOR of the two mixer result pairs.
- Mixer arithmetic wraps at 32 bits. Constants, intermediate truncation and
  operation order must stay byte-compatible; a mathematically similar rewrite
  is not sufficient evidence of equivalence.

`GetTables` publishes one immutable set via function-local static initialization;
each call owns its mutable encoder state. `ComputeHash` assigns `out_hash` only
after successful encoding, mixing and Base64 conversion. A returned failure
leaves it unchanged, allowing the caller to decide recovery without a partial
hash. Allocation exceptions propagate to the association caller; CryptoAPI
handles are released during unwinding. Optional `DebugData` exposes packed
words, MD5 words and mixer pairs for comparison, and may be partial on failure.

## What the tests establish

`../../tests/test_userchoice_latest_hash.cpp` checks:

- Exact hashes for fixed canonical strings representing all three machine
  classes. These are codec vectors, not a test of the backend's class selection.
- Concurrent first use, shared complete lookup tables and independent full hash
  calculations with a fixed expected result.
- Successful encoding/12-character results at selected input lengths, exact
  empty and long-repeat snapshots, input case folding, and optional debug/output
  modes. Length-only checks are not additional byte-compatibility vectors.
- Rejection of the repeating pattern that previously over-counted the first-pass
  slice and hung, with the caller's previous output preserved. This test expects
  failure, not a substitute hash for that input.
- Rejection/output preservation for the first-pass rewind-underflow pattern;
  fixed pre-fix hashes for lowercase non-ASCII, surrogate pairs, an isolated
  surrogate and embedded NUL. C, Turkish and Azerbaijani CRT locale checks verify
  C-locale byte preservation and the existing canonical vector; unavailable locales are
  explicitly reported as skipped, not counted as successful locale runs.

`../../tests/test_userchoice_hash.cpp` checks the shared MD5 helper through a
fixed legacy-format vector. Format selection and transaction/recovery behavior
are separately covered by the backend's decision and disposable-registry tests.
Codec tests do not write associations or establish Shell acceptance.

Both Autotools and Meson register the tests. Cross-compiled executables must run
on Windows. When changing the encoder or mixers, compare packed words, MD5 words,
both mixer pairs and final hashes with the pinned codec on representative ASCII,
UTF-16, boundary and repeated inputs; a final-output match alone can hide a
divergence in a path the fixed vectors do not exercise.
