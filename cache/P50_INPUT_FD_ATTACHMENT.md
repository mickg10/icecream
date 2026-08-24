# Protocol-50 committed-input FD attachment

`p50_input_fd_attachment.{h,cpp}` is a bounded adapter between a committed
`InputRecord` and one compiler consumer. It is deliberately not a routing
selector or an end-to-end compiler mode.

The client connects to a private AF_UNIX endpoint under one absolute
`steady_clock` deadline, verifies the service credentials, and performs the
existing local-transport identity handshake. Its `DATA` request contains the
exact 128-bit `CStoreGuid`, `TuSeq`, and nonzero request ID. The service checks
the same identity and calls its cursor provider synchronously; the
`for_endpoint()` helper binds that provider to `P50ServerEndpoint::attach_input`
on the endpoint owner thread.

Before any descriptor is passed over SCM_RIGHTS, the service drains the
immutable cursor into a complete Linux sealed memfd, verifies its digest and
size, rewinds it to byte zero, and transfers it with the existing authenticated
one-shot FD handoff. Unsupported platforms and every malformed, stale,
missing, disconnected, or timed-out path return without a descriptor. A
fresh memfd/open-file-description is created for every request, so consumers
have independent offsets; after handoff the sealed snapshot remains readable
even if the service process exits.
