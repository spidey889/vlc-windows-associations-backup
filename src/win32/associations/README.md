# Private Windows association support

This static library owns the current-user transaction engine, hash codecs and
synchronous batch operation. It depends on Windows APIs and standard C++, not Qt
or libvlccore, and exposes no installed/public libVLC API. Its original engine and
codec licensing/provenance are retained beside the sources. Batch orchestration
and Qt integration retain their GPL-2.0-or-later license.

The caller supplies explicit user intent and the exact requested extensions;
extension inventories remain frontend policy. The API does not select users,
elevate, register VLC or open Settings. It preserves running-executable ownership
validation, effective-default verification, journals, per-extension rollback and
concurrent-change checks. A batch is not atomic: completed extensions remain
completed after another fails. A writer-lock timeout marks the remaining entries
unattempted instead of waiting repeatedly. Calls must stay off the UI thread.

Preferences uses thin asynchronous Qt wrappers.
Backend regression fixtures live in test/src/win32 and use disposable test
extensions; the batch fixture supplies a fake backend and performs no registry I/O.
Qt lifecycle/selection fixtures remain with the interface. The separate opt-in
Shell smoke test must not be confused with these regression fixtures.

The batch exposes a typed stop reason (`none` or `writerBusy`) and skipped count.
Skipped extensions remain in the failure list so existing Qt presentation and
retry selection stay unchanged. A busy final extension can have zero skipped
entries. Completed transactions are not rolled back when a later item fails.

The Windows executable's early command is documented in bin/win32_associations.md.
It checks the complete payload and current desktop identity before calling this
library. Elevated callers broker one independently verified desktop-user worker,
including different-administrator UAC elevation; that handoff is outside this
library. The worker's current token owns all defaults and completion history.

Installer-only batches recover existing journals for the requested extensions
under the same writer lock before consulting persistent completion history.
Historical success still skips all new default requests. Recovery preserves
later user choices through the normal ownership/snapshot checks; a failed
cleanup retains its journal and does not report an already-completed operation.
