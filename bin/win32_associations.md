# Early Windows association command

```text
vlc.exe --win32-set-default-associations=.mp3,.mp4
vlc.exe --win32-set-default-associations-once=.mp3,.mp4
```

This exclusive Windows maintenance operation is handled by the installed launcher
immediately after Unicode argument parsing, before crash reporting, configuration,
libVLC/Qt initialization and instance forwarding. It does not play media or open
a dialog or Settings. The executable retains its asInvoker manifest; this command
never requests elevation or credentials.

Supply one nonempty comma-separated list. Each extension follows the engine's
existing rules: a leading dot and 1-63 ASCII letters, digits, plus, minus or
underscore characters. Input order, case and duplicates are preserved. Empty
entries, paths, whitespace, additional arguments and repeated switches are
rejected before the first write. The complete list is validated up front.
Caller inventories remain outside the engine. An installer must skip invocation
for empty selection and prevent its own argument truncation.

## Installer success history

NSIS and MSI use the exclusive `-once` form. After the ordinary-user context is
verified, the worker holds the existing per-user writer mutex across the history
check, selected batch, final read-only verification and completion write. Nested
extension transactions take the same recursive Windows mutex on this thread.
Only a nonempty, entirely successful selection with no pending selected-extension
journals and a successful final effective-default check can record
`HKCU\Software\VideoLAN\VLC\InstallerDefaults\Completed` as DWORD 1.
Already-correct defaults count toward success. Completion is shared by installer
formats and architectures, with no version/path/selection-specific reset.

An existing valid record causes an immediate no-op after validation/context/lock
checks. It records historical success, not a claim about the user's current
defaults. Never reassert or clear it when the user changes players. Preferences
and the ordinary command remain unrestricted and do not consult/write this record.
The record is outside VLC configuration files and installer-owned HKLM resources;
existing uninstall/configuration cleanup does not remove it. Unreadable or malformed
records fail closed. Partial, busy, rejected or unverified work leaves no success.
Completion write/flush errors are nonfatal command failures, with a failed flush
removing the new value on a best-effort basis. There is no automatic retry.

MSI does not wait for or record the async worker's result. The worker may persist
verified success after MSI closes, or after the NSIS broker stops waiting. Such
success is based on the worker's own completed verification, not the parent's
unconfirmed status. An interrupted worker leaves the existing per-extension
journals for a later explicit attempt. A crash before recording completion can
leave verified defaults but no history; a later installer may try again.

MSI also registers the NSIS-style `VLC.ext` identifiers alongside its own
`VLC.ext.Document` identifiers. The generated compatibility components follow
the existing Audio/Video/Others features and reuse their names, icons and open
commands. This restores the registrations removed by the NSIS uninstaller,
keeping an existing Windows choice usable without another defaults operation.
It runs as normal MSI registration, independently of `Completed`; neither
UserChoice nor completion history is changed by these compatibility components.

## User context and handoff

The desktop shell comes from GetShellWindow, never arbitrary Explorer enumeration.
The command reads both process and shell tokens and requires the same nonzero
session, an ordinary medium-integrity shell, and stable shell identity. Ordinary
callers and private workers must also have the shell user's SID. Only a verified
elevated high-integrity broker may have a different SID: over-the-shoulder UAC
changes the installer account, not the intended interactive desktop user.
Unknown, inaccessible or changing contexts return 3. There is no administrator-
profile fallback, impersonation, linked-token downgrade or user-hive loading.

If opening the verified desktop's shell process/token is denied, only the high-
integrity broker may temporarily enable its existing SeDebugPrivilege for one
additional handle-acquisition attempt. Its prior state must be restored before
launching a worker. Missing privilege, denied access or failed restoration refuses
without a worker. This changes no account/policy and never retries associations.

An ordinary medium-integrity caller runs the synchronous batch directly. A
verified elevated high-integrity caller acts only as a broker: it launches
one ordinary worker using the held shell process as the explicit parent. The
worker is the same absolute executable, with a fresh environment built from the
verified shell token, an explicit executable directory and no inherited broker
handles. Arguments use Windows quoting, and an overlong command is rejected
instead of truncating the list. There is no RunAs, credential UI or retry.

The worker's process/thread DACLs explicitly grant access to that shell user and
SYSTEM. Leaving these attributes null would use the elevated creator's default
DACL, which can deny a different original user access to its own process/token.
The held shell supplies the primary token; the administrator supplies neither
the target identity nor the worker's environment, defaults or completion history.

The private worker invocation carries the expected canonical SID, session and
extension list. These arguments are expectations, not authority. The worker
independently verifies its actual token, ordinary integrity, session and desktop
shell against those expectations before calling the engine. An elevated worker
refuses and cannot recurse into another handoff. The broker never calls the
writer. The backend retains its running-executable/registered-VLC ownership checks,
current-account-only writes, locking, recovery and verification.

The broker waits once on the real worker process handle, with a five-minute
timeout. A signaled process with a readable result forwards codes 0-4. Launch
failures, unexpected/crash codes, wait failures/timeouts and an unreadable final
status return 1, with no fallback execution or automatic retry. There is no custom
IPC, result file, log parsing or polling protocol.

| Exit | Meaning |
| --- | --- |
| 0 | Every requested extension verified, or installer history already completed (no changes) |
| 1 | Failure/incomplete or unconfirmed completion, including required user confirmation |
| 2 | Invalid request; no association writes attempted |
| 3 | Unsafe/unverifiable user context; no association writes attempted |
| 4 | Writer lock busy; earlier transactions may have completed |

Busy takes precedence over ordinary incomplete results and can occur with zero
skipped entries. No result implies batch atomicity or rollback of earlier success.
The additive typed batch stop reason (none/writerBusy) and skipped count keep
skipped entries in the existing failure list, preserving Preferences presentation
and explicit retry behavior. Exit codes are never inferred from error text.

Diagnostics use stderr when captured. Direct callers get per-extension batch
details; brokers report setup failures and the numeric worker status. Worker
stderr is not forwarded to the broker. GUI executables do not promise an attached
terminal, and this command does not allocate a console or add diagnostic files.

## Completion and timeout policy

The five-minute limit bounds the broker's wait after worker creation, not the
lifetime of an association transaction. Timeout or wait failure reports
"completion unconfirmed", the worker PID and wait/error information, then returns
1. An unreadable result after process completion also returns 1 and is explicitly
unconfirmed. Only a signaled process with a readable result can report success.
The broker never samples an exit code after timeout, including a boundary race
where the worker exits immediately afterwards.

Timeout stops waiting only. The broker releases its own handles and leaves the
worker untouched; the worker may finish the remaining batch normally, even after
the broker exits. No termination, cancellation signal, automatic retry, extended
grace wait or repeated polling is introduced. This preserves transaction recovery
and the shared batch/Preferences behavior without adding a cancellation API or
another cross-account communication channel.

The PID is diagnostic context, not a durable completion token: it can be reused
after exit. No later code guesses completion from that PID or automatically retries
an uncertain request. Verify defaults later before deciding on a manual retry.

This is one finite Windows wait, not a wall-clock deadline spanning startup,
system suspension or synchronous environment/process setup. On Windows 8 and
newer, the wait interval excludes time spent in low-power states. Ordinary direct
invocation remains synchronous, with the existing engine behavior and no new
transaction deadline.

NSIS must use the verified elevated broker path to obtain this bounded wait;
there is no synchronous ordinary-user fallback. Schedule associations after file
and registration work, and treat code 1 as failed/incomplete or unconfirmed,
never success or permission to retry. Allow installation to finish without
claiming default setup succeeded. Do not kill a potentially live worker or remove
its files as part of timeout handling. Worker stderr is still not relayed.

Windows API references:
[WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject),
[process termination and child lifetime](https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process).

The user reports the previous NSIS and MSI packages passed real Windows testing.
The new first-success path has focused mocked token/launch/wait responses and
disposable-registry verification. It still needs new installed-package acceptance.
Different-administrator UAC handoff has deterministic token/launch regressions,
including real parsing of the worker DACLs, exact selection and original-user-only
worker/history access. Real two-account UAC acceptance remains required: confirm
the worker's SID/token/environment, original-user defaults and Completed record,
and unchanged administrator defaults/history. No mock proves Windows acceptance.

## NSIS adapter

The existing File Type Associations sections keep their labels, defaults and
Memento identifiers. Each executed checkbox section appends its exact extension
to one command list, in section order. It never assigns a machine-wide HKCR
extension default or backup: the verified current-user engine exclusively owns
new default changes. ProgID/capability registration remains, and SupportedTypes
uses the actual extension rather than a scratch register. Legacy uninstall
backup restoration remains for older installations. All selected types are
included, including optional skin/archive entries; there is no new inventory,
select-all fallback, filtering or per-extension launch.

On initialization, the adapter latches existing InstallDir, Version or uninstall
markers from both registry views, including empty existing values. A predecessor
recognized by the existing NSIS InstallDir/Version detection can lift that initial
skip when version comparison says same/newer installer, including a recognized
32-bit NSIS predecessor. Re-latch markers in each visited registry view before
applying its version decision: an earlier other-architecture allow must not bypass
the main view's own skip. Marker-only/MSI or unknown predecessors and downgrades
remain excluded from this optional operation; existing installation behavior is
unchanged. Explicit /update does not block a recognized upgrade. Memento and the
existing page-skipping/selection behavior remain unchanged. Silent installs and
empty selections never invoke the once-only command. Registration still runs;
extension fallback defaults
are preserved. Ignored extraction errors and failed output-directory setup also
latch a skip before later logging can clear the error flag, so the success callback
cannot launch a stale executable after an unsuccessful copy.

The .OnInstSuccess callback invokes the optional operation once, after all
sections (including -Post registration) and Memento saving. It checks executable
presence, the installed product/App Paths registration and an elevated installer
token before calling vlc.exe directly with ExecWait. No Explorer/RunAs fallback
exists. The command/worker independently retain their stronger SID/session checks.

Both list accumulation and the entire quoted command are checked against
NSIS_MAX_STRLEN before string expansion. Overflow skips the whole optional
operation; no truncated subset or split batch is executed. The current 136-entry
inventory is 699 comma-list characters, but a long custom install path can still
make the full command too large for a standard NSIS build.

ExecWait uses a separate output variable and checks its launch-error flag before
reading that variable. Codes 0-4 and unexpected exits are recorded in the existing
details log. Every failure is nonfatal, and code 1 explicitly includes unconfirmed
completion with a possibly live worker. There is no retry, termination, cleanup of
worker files, new dialog, Settings launch or change to installer exit status. An
attempt latch also prevents a repeated success callback from launching again.

NSIS also registers `VLC.ext.Document` compatibility identifiers for the 110
file types shared with MSI, using the current executable, native NSIS metadata
and open command. Both build systems generate `helpers/msi-compat.nsh` with
`msi-associations.py --nsis-extensions`, deriving the intersection from MSI's
declared ProgIDs and the NSIS registration inventory. This includes `.mid`
without selecting it by default, excludes `.bin`, and retains
`VLC.amrec.Document` despite MSI's different capability mapping. Capabilities
still select `VLC.ext`. Registration does not write UserChoice or `Completed`,
and invalid hashes still fail the backend's backup-safety checks. Uninstall
removes these aliases only while their open command matches this installation.

Manual regression fixture (no registry/default writes or VLC launch):

```text
python3 test/src/win32/association_installer.py /absolute/test-output
```

Run the resulting asInvoker association-installer-test.exe from an ordinary
Windows shell, and again with /all. It writes results beside itself, uses the real
section macros/adapter with mocked external reads/registration/command execution,
and exercises subset/all selection, gates, error handling and string boundaries.
The optional win64 builder argument tests the real 32-bit NSIS stub with the
64-bit VLC installer branch. The amd64 argument additionally tests a native 64-bit
NSIS VM. Full package compilation and installed-VLC acceptance remain separate
gates. Existing macro/mocked primitive regressions detect a restored global-default
callback, the wrong SupportedTypes variable, and a missing extraction-failure latch.

## Remaining installer lifecycle gate

The optional post-install command never kills its worker. The pre-existing startup
CheckRunningProcesses path is not worker-aware: nsProcess matches every vlc.exe
by basename and NiceTerminate can call TerminateProcess. A later installation,
especially a silent one, can therefore kill a worker that survived an earlier
timeout while an association transaction is active. The original timeout return
does not invoke that cleanup, but the next installer run can.

Before installed acceptance, make startup process cleanup cooperate with the
association writer lock. The decision and close need synchronization to avoid
killing a writer that starts between a liveness check and termination. Do not use
PID logs or an extra sleep as proof that termination is safe. The process-close
policy is left unchanged for review, rather than introducing unreviewed cross-
process coordination in the audit. Avoid starting another installer while an
unconfirmed worker may be active until this gate is resolved.
