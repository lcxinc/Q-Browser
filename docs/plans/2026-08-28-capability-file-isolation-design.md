# Capability and File Isolation Design

## Goal

Bind every asynchronous capability result, native file operation, and IPC
response to the exact tab/runtime/session authority that requested it. A
background or retired tab must not receive a result, keep a dialog open, or
block a sibling tab's heartbeat and capability lane.

## Scope and non-goals

This design covers the browser-shell Task16 boundary: file selection,
capability-result routing, bounded asynchronous IPC submission, and same-root
storage initialization. Existing synchronous network, clipboard, and storage
broker policy validation remains in `CapabilityBroker` and its services; only
the delivery boundary becomes asynchronous where necessary. Browser session
restore (Task17), UI Automation (Task18), and deployment attestation (Task19)
remain outside this change.

## Ownership

`HostApplication` owns one process-wide `FileDialogCoordinator`. It does not
know `BrowserTabModel` details; it receives an opaque operation token and an
immutable callback. Each `AppTabRuntimeController` injects the coordinator
into its `HostCapabilityRuntime`. The runtime maps

```
operation token -> TabCapabilityAuthority + requestId + session generation
```

and owns the one pending capability request for that session. The callback is
delivered to the GUI thread, then queued to the owning session only if the full
authority and admission token still match. Closing, rollback, token revoke, or
runtime-incarnation change invalidates the operation and drops late results.

## File flow

`CapabilityBroker::prepareFileRequest()` performs all request and policy
validation without showing UI. It returns a move-only `PreparedFileRequest`
containing the bounded policy snapshot and request metadata. The host starts
`FileDialogCoordinator::openAsync()` with an opaque token and a cancellation
handle. The coordinator runs `IFileOpenDialog` on a dedicated STA, creates a
message-only cancellation window before `Show()`, and retains the exact dialog
instance. Cancellation posts to that window, whose procedure calls
`IFileOpenDialog::Close(ERROR_CANCELLED)` while the COM modal loop pumps.

The selected stable handle remains on the STA. The coordinator validates
non-reparse ancestry, file identity, size (maximum plus one), and reads/encodes
owned bytes before sending a value-only `FileDialogSelection` callback. No
`QIODevice` crosses threads. The host invokes
`CapabilityBroker::completeFileRequest()` and sends only a bounded immutable
`BrokerResult` through the final authority gate.

The process-wide coordinator allows at most one native dialog. A second
owner receives stable `file.busy`; a background tab is rejected before a
dialog is created. Closing the owner invokes exact cancellation and marks the
token terminal, so success/cancel/failure from the old dialog cannot reach a
reopened tab or a new runtime incarnation.

## IPC flow

`IpcSession::submitSend()` takes an owned serialized frame and completion
callback. `WinPipeTransport` maintains a bounded per-session FIFO (64 frames,
4 MiB), one ordered writer, and exactly-once completion on success, failure, or
cancellation. Submission only enqueues; GUI and lifecycle code never waits for
the peer. Worker-side synchronous helpers may wait with a bounded timeout only
on the dedicated session IO thread.

The host send context retains `AuthorityAdmissionToken::UseGuard` until the
transport completion callback. At dequeue/send time, the complete authority
and session generation are compared and `publishIfStillAdmitted()` is the
decisive publication gate. Work queued before revocation is dropped at that
gate. Queue-full and cancellation are stable errors and do not reorder or
discard already accepted frames.

## Storage and lifecycle

`StorageBroker` serializes initialization for one canonical application root
with a stable inter/intra-process lock and revalidates the root before each
DACL transaction. A sibling runtime can initialize and update the same root
concurrently; retiring one broker cannot release another broker's root. All
async network/storage/clipboard/file completions use the same final authority
and token gate.

`HostApplication` cancels all file operations before closing runtimes and
destroys the coordinator only after callbacks are quiescent. Per-tab runtime
retirement, transport close, and file cancellation are independent: a blocked
writer or dialog cannot delay a sibling heartbeat or GUI event loop.

## Verification strategy

RED tests first establish owner-only busy state, tab switching and reopen
isolation, exact cancellation, late-result dropping, same-root storage races,
IPC FIFO/limits/exactly-once completion, revocation at the final send gate,
and GUI responsiveness while a dialog or writer is blocked. GREEN implements
one boundary at a time, followed by the existing broker, host, IPC, worker,
and end-to-end suites in Debug and a Release host build.
