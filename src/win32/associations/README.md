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
