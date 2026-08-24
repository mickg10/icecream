# Protocol-50 committed-input FD attachment

`p50_input_fd_attachment.{h,cpp}` is a bounded adapter between a committed
`InputRecord` and one compiler consumer. It is deliberately not a routing
selector or an end-to-end compiler mode.

The client connects to a private AF_UNIX endpoint under one absolute
`steady_clock` deadline, verifies the service credentials, and performs the
existing local-transport identity handshake. Its exact version-1 `DATA`
operation envelope identifies `InputFdAttachment`, the local identity,
128-bit `CStoreGuid`, `TuSeq`, and nonzero request ID. The service checks the
same identity and queues `P50ServerEndpoint::attach_input` on the sidecar's
endpoint-owner io_context; materialization happens on the bounded control
worker after the independently-owned cursor is returned.

Before any descriptor is passed over SCM_RIGHTS, the service drains the
immutable cursor into a complete Linux sealed memfd, verifies its digest and
size, reopens that memfd `O_RDONLY|O_CLOEXEC`, rewinds it to byte zero, and
transfers it with the existing authenticated one-shot FD handoff. The receiver
rechecks read-only/CLOEXEC/regular-file invariants before exposing it to the
compiler. Unsupported platforms and every malformed, stale, missing,
disconnected, or timed-out path return without a descriptor. A fresh
memfd/open-file-description is created for every request, so consumers have
independent offsets; after handoff the sealed snapshot remains readable even
if the service process exits.
