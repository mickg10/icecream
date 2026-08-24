# Protocol-50 accepted-FD handoff

This document specifies the bounded generic ownership primitive in
`p50_fd_handoff.{h,cpp}`.  It is a transport-only checkpoint for a future
ordinary-link `CACHE_SESSION`; it is not a cache protocol or a service
integration.

## Preconditions and wire record

Both callers use one move-only `Connection`, and each must have successfully
verified the peer credentials on that exact connection.  The next operation on
the stream is a fixed 40-byte big-endian `P50F` record:

| Bytes | Field |
| ---: | --- |
| 0..3 | magic `P50F` |
| 4..5 | version `1` |
| 6..7 | request (`1`), ACK (`2`), or NACK (`3`) |
| 8..11 | exact record length `40` |
| 12..19 | generation |
| 20..27 | attempt |
| 28..35 | nonzero request id |
| 36..39 | result code (zero in a request) |

A request has exactly one `SCM_RIGHTS` control message containing exactly one
descriptor.  AF_UNIX is deliberately `SOCK_STREAM`, so both request and reply
are accumulated across arbitrary positive fragments under one absolute
deadline.  A positive short `sendmsg` transfers the rights only once and all
remaining record bytes are sent without repeating the ancillary data.  The
reader consumes exactly 40 bytes and never reads into the next record.  Because
this exchange is stop-and-wait, any byte already queued in the same direction
before the reply is forbidden pipelining and is rejected without consumption.

The receiver also rejects any missing/extra fd or ancillary message, bad cmsg
length/type, `MSG_TRUNC`, `MSG_CTRUNC`, malformed record, stale generation,
wrong attempt/request identity, duplicate, disconnect, or expired absolute
deadline.  Terminal poll bits are classified before requested readiness, so a
combined `POLLIN|POLLHUP` result fails closed without consuming queued bytes.
There is no allocation based on a wire length.

## Ownership state machine

The sender starts `Prepared`, sends once, then enters `Sent`.  ACK moves it to
`Acked` and closes its local descriptor.  NACK, timeout, disconnect, malformed
response, or any other terminal failure moves it to a terminal rejection state
and closes the descriptor; a sent request is never retried.

The receiver closes every rejected descriptor.  For the valid expected request
it first proves `CLOEXEC` (`MSG_CMSG_CLOEXEC` and a safe `fcntl` fallback), moves
the descriptor into its adopted owner, and only then sends ACK.  If ACK cannot
be delivered the adopted owner remains responsible for the descriptor, while
the sender fails closed because ownership cannot be confirmed.

The raw exchange is one-shot and must not be interleaved with framed control
traffic.  One receiver instance admits one request.  No listener, endpoint,
advertisement, daemon/service loop, cache store, session codec, or ordinary-link
behavior is included here.
