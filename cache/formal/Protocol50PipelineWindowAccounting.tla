------------------------- MODULE Protocol50PipelineWindowAccounting -------------------------
EXTENDS Naturals, TLC

(***************************************************************************
Accounting-only W30 reachability model.  StageSendOne deliberately abstracts
the per-job and codec transitions modeled at W2: it represents one complete,
ordered source bundle entering the bounded speculative window.  It does not
model P29/ZSTD transforms or prove a W30 codec/worker implementation.

The metrics are explicit per-slot budgets: retained raw input until C verifies
the commit, encoded bytes until F publishes input, unresolved receipt bytes
until F processes the cumulative ACK, and per-TU speculative journal/metadata
bytes for the P-A suffix. It does not charge one cloned entropy snapshot per
slot: the proposed design keeps one working codec state and replays retained
raw suffix bytes after confirmed reset. Per-slot byte sizes are assumptions,
not measured codec
output sizes.  The model exists to check cursor arithmetic and cap accounting
at W30, not the C++ representation.
***************************************************************************)

CONSTANTS Window,
          RawBytesPerJob, MaxRawBytes,
          EncodedBytesPerBundle, MaxEncodedBytes,
          ReceiptBytesPerInput, MaxReceiptBytes,
          JournalBytesPerSpecSlot, MaxJournalBytes

ASSUME /\ Window = 30
       /\ RawBytesPerJob \in Nat \ {0}
       /\ MaxRawBytes \in Nat \ {0}
       /\ EncodedBytesPerBundle \in Nat \ {0}
       /\ MaxEncodedBytes \in Nat \ {0}
       /\ ReceiptBytesPerInput \in Nat \ {0}
       /\ MaxReceiptBytes \in Nat \ {0}
       /\ JournalBytesPerSpecSlot \in Nat \ {0}
       /\ MaxJournalBytes \in Nat \ {0}

VARIABLES A, K, P, Q, S, ackSent
vars == <<A, K, P, Q, S, ackSent>>

Init ==
    /\ A = 0
    /\ K = 0
    /\ P = 0
    /\ Q = 0
    /\ S = 0
    /\ ackSent = 0

StageSendOne ==
    /\ P - A < Window
    /\ P < Window
    /\ S = P
    /\ P' = P + 1
    /\ S' = S + 1
    /\ UNCHANGED <<A, K, Q, ackSent>>

FPublishInput ==
    /\ K < S
    /\ K - Q < Window
    /\ K' = K + 1
    /\ UNCHANGED <<A, P, Q, S, ackSent>>

ObserveReceipt ==
    /\ A < K
    /\ A' = A + 1
    /\ UNCHANGED <<K, P, Q, S, ackSent>>

TransmitAck ==
    /\ ackSent < A
    /\ ackSent' = A
    /\ UNCHANGED <<A, K, P, Q, S>>

ProcessAckAtF ==
    /\ Q < ackSent
    /\ ackSent <= A
    /\ ackSent <= K
    /\ Q' = ackSent
    /\ UNCHANGED <<A, K, P, S, ackSent>>

Next ==
    \/ StageSendOne
    \/ FPublishInput
    \/ ObserveReceipt
    \/ TransmitAck
    \/ ProcessAckAtF
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

CursorAndWindowBounds ==
    /\ Q <= A
    /\ A <= K
    /\ K <= S
    /\ S <= P
    /\ P - A <= Window
    /\ K - Q <= Window
    /\ ackSent <= A
    /\ ackSent <= K

RawBytesWithinCap == (P - A) * RawBytesPerJob <= MaxRawBytes
EncodedBytesWithinCap == (S - K) * EncodedBytesPerBundle <= MaxEncodedBytes
ReceiptBytesWithinCap == (K - Q) * ReceiptBytesPerInput <= MaxReceiptBytes
SpeculativeJournalBytesWithinCap ==
    (P - A) * JournalBytesPerSpecSlot <= MaxJournalBytes
FullWindowNotReached == P - A < Window

=============================================================================
