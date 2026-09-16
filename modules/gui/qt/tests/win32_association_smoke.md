# Real Windows association acceptance

`win32_association_smoke.cpp` is an opt-in integration check, not an unattended
CI test. It uses the production transaction writers without fault hooks. The
runner is not VLC, so it validates the supplied installed VLC path explicitly
rather than exercising the public entry point's running-executable lookup.

From `modules/gui/qt`, build with MinGW (native or cross-compiler):

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror -static -municode \
  tests/win32_association_smoke.cpp -I../../../src \
  ../../../src/win32/associations/userchoice_hash.cpp \
  ../../../src/win32/associations/association_batch.cpp \
  ../../../src/win32/associations/userchoice_latest_hash/HashCodec.cpp ../../../src/win32/associations/userchoice_latest_hash/HashTables.cpp \
  -ladvapi32 -lcrypt32 -lshell32 -lshlwapi -lole32 -luuid -o win32_association_smoke.exe
```

Run that executable on Windows as the ordinary user, with
`--run "C:\Program Files\VideoLAN\VLC\vlc.exe"` (adjust to the registered install).
The printed extension is unique to this run; existing fixture roots are refused.
Only its HKCU FileExts/recovery entry and three Classes entries are written.
Commands pointing to Notepad/VLC are registered but never launched by this check.
Normal and exception exits remove and verify those owned entries. If forcibly
interrupted, inspect/remove only the entries for the printed extension, not
the FileExts, Classes or AssociationRecovery parent directories.

Each resolver assertion runs in a fresh process and asks Windows through
[AssocQueryStringW](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-assocquerystringw),
not the backend's hash verifier. Expected sequence: non-VLC fallback, VLC after
the transaction, fallback after deliberately corrupting the hash, VLC after
refusal to modify the ignored invalid record, a fresh transaction after removing
that owned record, and fallback after removing the test choice. In-process
checks of the production effective-default query must agree with the fresh
resolver before/after the valid and invalid writes. The negative control prevents
counting a resolver that simply trusts the written ProgId as acceptance evidence.

For a separate document-open smoke test, use a small local media file whose
extension already resolves to VLC; `--probe .wav "<installed vlc.exe>"` is a
read-only check. With VLC closed, open the file through Explorer or PowerShell
`Start-Process -FilePath "<sample.wav>"`, without specifying a player. Verify
the newly launched executable is the registered VLC and its command line contains
the sample path. Close only that test instance and remove the sample. Do not
change an unrelated default just to make this step pass.

On 2026-09-17, Windows build 26200.9457 (latest format) passed all five resolver
assertions and fixture restoration/cleanup; snapshots of 529 existing extension
choice records were unchanged. A separate already-associated WAV document-open
launched the installed VLC with the sample path; that new instance was closed
and the sample removed. Activation of the synthetic extension was not observed
in an earlier ShellExecuteEx attempt, so it is not counted as a file-opening pass.
The resolver check and existing-WAV launch are distinct evidence, not an
automated playback or all-extension/Windows-build acceptance matrix.

The backend also checks ProgId, Hash and key timestamps before publishing a
hash or restoring/removing a choice. The registry suite injects external writes
at calculation/ownership boundaries, including same-ProgId changes. These are
conflict-detection tests, not atomicity evidence: Settings can still write between
the final snapshot check and registry mutation. The per-user mutex serializes
VLC instances only. No registry-transaction availability is required.
