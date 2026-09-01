------------------------------ MODULE Protocol50ZstdRoute ------------------------------
(***************************************************************************
Asymmetric first-product ZSTD_ROUTE transaction law.

There is no opaque CCtx/DCtx clone, promotion, rollback, or saved predecessor
codec.  C's sole usable encoder advances once and retains the exact encoded
BODY.  F's sole usable decoder stays at the committed predecessor until the
complete BODY and full operation identity validate.  Decoder touch is
irreversible: it ends in one atomic InputRecord/route/lastCommit commit or in
an identity-bound classified reset.  Codec continuity and input-operation
settlement are separate facts.

The model is deliberately finite: three distinct operations, two explicit
byte values, two fragments, one route reset, and one route state multiplied
by a configured maximum-active-route owner bound.  Product codec bytes,
throughput, and deployment remain executable gates.
***************************************************************************)

EXTENDS Naturals, FiniteSets, Sequences, TLC

CONSTANTS
    T0, T1, T2, NoTU,
    B0, B1,
    MaxFragments, MaxWriterTicks, MaxReaderTicks, MaxTerminalTicks,
    CodecUnits, MaxEncodedBytes, MaxRawBytes, MaxWindowBytes,
    MaxActiveRoutes, PerRouteCap, GlobalCap,
    MutantKind

MutantCommitBeforePublish       == MutantKind = 1
MutantDuplicateSettlement       == MutantKind = 2
MutantEarlyCObserve             == MutantKind = 3
MutantDropLastCommit            == MutantKind = 4
MutantOldRouteStart             == MutantKind = 5
MutantResetBeforeClassification == MutantKind = 6
MutantStateDigest               == MutantKind = 7
MutantResourceOverflow          == MutantKind = 8
MutantStalledWriter             == MutantKind = 9
MutantResetIdentity             == MutantKind = 10
MutantWrongTerminalIdentity     == MutantKind = 11
MutantFeedBeforeComplete        == MutantKind = 12
MutantContinueAfterTouch        == MutantKind = 13
MutantFrameEnd                  == MutantKind = 14
MutantEqualLengthCorruption     == MutantKind = 15
MutantWrongPostReset            == MutantKind = 16
MutantResendAfterCommit         == MutantKind = 17
MutantCollapsedTU               == MutantKind = 18
MutantSingleByteAlphabet        == MutantKind = 19
MutantInitialContextReuse       == MutantKind = 20
MutantSecondDecode              == MutantKind = 21
MutantCRollback                 == MutantKind = 22
MutantStaleAfterReset           == MutantKind = 23
MutantSameEpochABA              == MutantKind = 24
MutantLostLiability             == MutantKind = 25
MutantSecondCodec               == MutantKind = 26
MutantResetErasesSettlement     == MutantKind = 27
MutantStalledReader             == MutantKind = 28
MutantStalledTerminal           == MutantKind = 29
MutantFaultNeverCloses          == MutantKind = 30
MutantWrongCommitRetry          == MutantKind = 31
MutantActiveRoutesOverflow      == MutantKind = 32
MutantWrongBodyEventIdentity    == MutantKind = 33
MutantPreTouchReencode          == MutantKind = 34
MutantDropFJobAndReuse          == MutantKind = 35
MutantInstallFContextAfterCancel == MutantKind = 36
MutantDuplicateFLease           == MutantKind = 37
MutantDropCJobAndReuse          == MutantKind = 38
MutantInstallStaleCodecReturn   == MutantKind = 39

NoIdentity == <<"NO_IDENTITY">>

TUs == {T0, T1, T2}
Byte == {B0, B1}

TUAt(i) == CASE i = 0 -> T0 [] i = 1 -> T1 [] OTHER -> T2
TransactionAt(i) == CASE i = 0 -> "tx0" [] i = 1 -> "tx1" [] OTHER -> "tx2"

HistoryByte(h) == CASE h = 0 -> B0 [] h = 1 -> B1
                         [] h = 2 -> B0 [] OTHER -> B1
TUByte(tu) == CASE tu = T0 -> B0 [] tu = T1 -> B1 [] OTHER -> B0
FlipByte(b) == IF b = B0 THEN B1 ELSE B0

RawBytes(tu) == CASE tu = T0 -> <<B0>>
                     [] tu = T1 -> <<B1>>
                     [] OTHER -> <<B0, B1>>

Encode(history, tu) == <<B0, HistoryByte(history), TUByte(tu), B1>>
DecodeMatches(history, body, tu) == body = Encode(history, tu)

CorruptSameLength(body) ==
    [i \in 1..Len(body) |->
        IF i = Len(body) THEN FlipByte(body[i]) ELSE body[i]]

ByteSequences == UNION {[1..n -> Byte] : n \in 0..MaxEncodedBytes}
FullBodies == {body \in ByteSequences : Len(body) = MaxEncodedBytes}

Fragment(body, index) ==
    IF index = 0
       THEN SubSeq(body, 1, MaxEncodedBytes \div 2)
       ELSE SubSeq(body, (MaxEncodedBytes \div 2) + 1, MaxEncodedBytes)

CLaunch(e) == IF e = 0 THEN "c-launch-0" ELSE "c-launch-1"
CStore(e) == IF e = 0 THEN "c-store-0" ELSE "c-store-1"
FLaunch(e) == IF e = 0 THEN "f-launch-0" ELSE "f-launch-1"
FStore(e) == IF e = 0 THEN "f-store-0" ELSE "f-store-1"
Relationship(e) == IF e = 0 THEN "relationship-0" ELSE "relationship-1"
FenceId(e) == IF e = 0 THEN "fence-0" ELSE "fence-1"

RouteIdentity(e) == [
    profile |-> "ZSTD_ROUTE",
    cLaunch |-> CLaunch(e),
    cStore |-> CStore(e),
    fLaunch |-> FLaunch(e),
    fStore |-> FStore(e),
    epoch |-> e,
    relationship |-> Relationship(e),
    relSeq |-> e
]

WrongRouteIdentity(e) ==
    [RouteIdentity(e) EXCEPT !.cLaunch = "wrong-c-launch"]

NoRouteIdentity == [
    profile |-> "NO_PROFILE",
    cLaunch |-> "no-c-launch",
    cStore |-> "no-c-store",
    fLaunch |-> "no-f-launch",
    fStore |-> "no-f-store",
    epoch |-> 2,
    relationship |-> "no-relationship",
    relSeq |-> 2
]

StateDigest(route, cursor, history) ==
    <<"state", route, cursor, history>>
RawIdentity(tu) == [
    bytes |-> RawBytes(tu),
    digest |-> <<"raw", tu, RawBytes(tu)>>
]
EncodedIdentity(body) == [
    length |-> Len(body),
    bytes |-> body,
    digest |-> <<"encoded", body>>
]

OperationIdentity(e, index, tu, predecessorCursor, predecessorDigest, body) == [
    route |-> RouteIdentity(e),
    transaction |-> TransactionAt(index),
    tu |-> tu,
    tuSeq |-> index,
    predecessorCursor |-> predecessorCursor,
    predecessorDigest |-> predecessorDigest,
    raw |-> RawIdentity(tu),
    encoded |-> EncodedIdentity(body)
]

WrongOperationIdentity(op) ==
    [op EXCEPT !.route = WrongRouteIdentity(op.route.epoch)]

WrongBodyOperationIdentity(op) ==
    [op EXCEPT !.encoded = EncodedIdentity(
        CorruptSameLength(op.encoded.bytes))]

NoOperation == [
    route |-> NoRouteIdentity,
    transaction |-> "no-transaction",
    tu |-> NoTU,
    tuSeq |-> 3,
    predecessorCursor |-> 4,
    predecessorDigest |-> NoIdentity,
    raw |-> [bytes |-> <<>>, digest |-> NoIdentity],
    encoded |-> [length |-> 0, bytes |-> <<>>, digest |-> NoIdentity]
]

OperationDomain == {
    OperationIdentity(e, index, TUAt(index), predecessor,
                      StateDigest(RouteIdentity(e), predecessor, history), body) :
        e \in 0..1,
        index \in 0..2,
        predecessor \in 0..3,
        history \in 0..3,
        body \in FullBodies
}
WrongOperationDomain == {WrongOperationIdentity(op) : op \in OperationDomain}
OperationOrNone == OperationDomain \cup WrongOperationDomain \cup {NoOperation}

ContextGeneration(e) == e + 1
CodecJobId(side, op, generation) ==
    <<"zstd-route-codec-job", side, op, generation>>

PreparedId(op) ==
    <<"prepared-input", op.route, op.transaction, op.tuSeq,
      op.predecessorCursor, op.predecessorDigest,
      op.raw.digest, op.encoded.digest,
      ContextGeneration(op.route.epoch),
      CodecJobId("F", op, ContextGeneration(op.route.epoch))>>

PermitId(op, preparedId) ==
    <<"f-input-commit-permit", op.route, op.transaction, preparedId>>

NoCodecJobId == <<"NO_CODEC_JOB_ID">>

PreparedIdDomain == {PreparedId(op) : op \in OperationDomain}
PermitIdDomain == {
    PermitId(op, PreparedId(op)) : op \in OperationDomain
}
CodecJobIdDomain == {
    CodecJobId(side, op, generation) :
        side \in {"C", "F"}, op \in OperationDomain,
        generation \in 1..2
}

(***************************************************************************
 The finite domains above are authoritative.  These shape predicates are
 their non-enumerative form for state validation: they preserve the same
 field relations while avoiding repeated membership scans over the large
 record-valued domains during TLC invariant checking.
***************************************************************************)
OperationFieldNames == {
    "route", "transaction", "tu", "tuSeq", "predecessorCursor",
    "predecessorDigest", "raw", "encoded"
}

OperationShape(op) ==
    /\ DOMAIN op = OperationFieldNames
    /\ LET e == op.route.epoch
           base == RouteIdentity(e)
       IN /\ e \in 0..1
          /\ (op.route = base \/ op.route = WrongRouteIdentity(e))
          /\ op.tuSeq \in 0..2
          /\ op.tu = TUAt(op.tuSeq)
          /\ op.transaction = TransactionAt(op.tuSeq)
          /\ op.predecessorCursor \in 0..3
          /\ \E history \in 0..3 :
                op.predecessorDigest =
                    StateDigest(base, op.predecessorCursor, history)
          /\ op.raw = RawIdentity(op.tu)
          /\ op.encoded.bytes \in FullBodies
          /\ op.encoded = EncodedIdentity(op.encoded.bytes)

ProperOperationShape(op) ==
    /\ OperationShape(op)
    /\ op.route = RouteIdentity(op.route.epoch)

OperationOrNoneShape(op) ==
    op = NoOperation \/ OperationShape(op)

PreparedOperation(pid) ==
    LET body == pid[8][2]
    IN [
        route |-> pid[2],
        transaction |-> pid[3],
        tu |-> TUAt(pid[4]),
        tuSeq |-> pid[4],
        predecessorCursor |-> pid[5],
        predecessorDigest |-> pid[6],
        raw |-> RawIdentity(TUAt(pid[4])),
        encoded |-> EncodedIdentity(body)
    ]

PreparedIdShape(pid) ==
    /\ Len(pid) = 10
    /\ pid[1] = "prepared-input"
    /\ pid[2] \in {RouteIdentity(0), RouteIdentity(1)}
    /\ pid[4] \in 0..2
    /\ pid[3] = TransactionAt(pid[4])
    /\ pid[5] \in 0..3
    /\ \E history \in 0..3 :
          pid[6] = StateDigest(pid[2], pid[5], history)
    /\ pid[7] = RawIdentity(TUAt(pid[4])).digest
    /\ Len(pid[8]) = 2
    /\ pid[8][1] = "encoded"
    /\ pid[8][2] \in FullBodies
    /\ pid[9] = ContextGeneration(pid[2].epoch)
    /\ pid = PreparedId(PreparedOperation(pid))

PermitIdShape(permit) ==
    /\ Len(permit) = 4
    /\ permit[1] = "f-input-commit-permit"
    /\ PreparedIdShape(permit[4])
    /\ permit = PermitId(PreparedOperation(permit[4]), permit[4])

CodecJobIdShape(job) ==
    /\ Len(job) = 4
    /\ job[1] = "zstd-route-codec-job"
    /\ job[2] \in {"C", "F"}
    /\ ProperOperationShape(job[3])
    /\ job[4] \in 1..2
    /\ job = CodecJobId(job[2], job[3], job[4])

CodecJobSetShape(jobs) ==
    /\ IsFiniteSet(jobs)
    /\ \A job \in jobs : CodecJobIdShape(job)

PermitSetShape(permits) ==
    /\ IsFiniteSet(permits)
    /\ \A permit \in permits : PermitIdShape(permit)

OperationSetShape(operations) ==
    /\ IsFiniteSet(operations)
    /\ \A op \in operations : ProperOperationShape(op)

OperationSequenceShape(operations) ==
    /\ Len(operations) <= 3
    /\ \A i \in 1..Len(operations) :
          ProperOperationShape(operations[i])

JobSet(job) == IF job = NoCodecJobId THEN {} ELSE {job}
StaleCodecJob(side, x) ==
    CodecJobId(side, x.oldEpochOp,
               ContextGeneration(x.oldEpochOp.route.epoch))
UnpublishedRetiredCodecJobs(x) ==
    IF ProperOperationShape(x.oldEpochOp)
          /\ x.oldEpochOp \notin x.publishedOps
      THEN {StaleCodecJob("C", x), StaleCodecJob("F", x)}
                \intersect x.retiredCodecJobs
       ELSE {}

PreparedInput(x) == [
    route |-> RouteIdentity(x.epoch),
    operation |-> x.pendingOp,
    preparedId |-> PreparedId(x.pendingOp),
    predecessorCursor |-> x.preCursor,
    predecessorDigest |-> x.preDigest,
    successorCursor |-> x.preCursor + 1,
    successorTag |-> x.postTag,
    successorDigest |-> StateDigest(
        RouteIdentity(x.epoch), x.preCursor + 1, x.postTag),
    body |-> EncodedIdentity(x.retainedBody),
    raw |-> RawIdentity(x.pendingTU),
    contextGeneration |-> x.fContextGeneration,
    codecJobId |-> x.fCodecJobId,
    decoderTouched |-> x.decoderTouched
]

(***************************************************************************
 The profile core owns every identity which crosses the narrow F-input
 boundary.  The owner interface retains these values unchanged; it never
 reconstructs an InputRecord, Ready event, route successor, or lastCommit.

 ReadyEventId is deliberately a nonzero bounded owner sequence in this
 finite model.  The complete ReadyIdentity also binds that sequence to the
 exact operation and canonical InputRecord key.
***************************************************************************)
InputRecordKey(op, preparedId) ==
    <<"f-input-record", op.route.fStore, op.transaction, preparedId>>

ReadyEventId(op) == 1 + (3 * op.route.epoch) + op.tuSeq

ReadyIdentity(op, inputRecordKey) == [
    readyEventId |-> ReadyEventId(op),
    operation |-> op,
    inputRecordKey |-> inputRecordKey
]

CommittedPredecessor(x) == [
    route |-> x.lastCommitOp.route,
    cursor |-> x.lastCommitOp.predecessorCursor,
    digest |-> x.lastCommitOp.predecessorDigest
]

CommittedSuccessor(x) == [
    route |-> x.lastCommitOp.route,
    cursor |-> x.lastCommitOp.predecessorCursor + 1,
    tag |-> x.lastCommitSuccessorTag,
    digest |-> x.lastCommitSuccessorDigest,
    contextGeneration |-> x.lastCommitContextGeneration,
    codecJobId |-> x.lastCommitCodecJobId
]

InputRecord(x, inputRecordKey) == [
    key |-> inputRecordKey,
    operation |-> x.lastCommitOp,
    preparedId |-> x.lastCommitPreparedId,
    raw |-> RawIdentity(x.lastCommitOp.tu),
    body |-> EncodedIdentity(x.lastCommitBody)
]

LastCommitRecord(x, inputRecordKey, ready) == [
    operation |-> x.lastCommitOp,
    preparedId |-> x.lastCommitPreparedId,
    permitId |-> x.lastCommitPermitId,
    inputRecordKey |-> inputRecordKey,
    ready |-> ready,
    predecessor |-> CommittedPredecessor(x),
    successor |-> CommittedSuccessor(x),
    raw |-> RawIdentity(x.lastCommitOp.tu),
    body |-> EncodedIdentity(x.lastCommitBody)
]

DurableBundle(x) ==
    LET key == InputRecordKey(x.lastCommitOp, x.lastCommitPreparedId)
        ready == ReadyIdentity(x.lastCommitOp, key)
    IN [
        operation |-> x.lastCommitOp,
        preparedId |-> x.lastCommitPreparedId,
        permit |-> x.lastCommitPermitId,
        inputRecord |-> InputRecord(x, key),
        ready |-> ready,
        routeSuccessor |-> CommittedSuccessor(x),
        lastCommit |-> LastCommitRecord(x, key, ready),
        raw |-> RawIdentity(x.lastCommitOp.tu),
        body |-> EncodedIdentity(x.lastCommitBody),
        predecessor |-> CommittedPredecessor(x),
        successor |-> CommittedSuccessor(x),
        contextGeneration |-> x.fContextGeneration,
        codecJobId |-> x.fCodecJobId
    ]

ResetIdentity(e) == [
    oldRoute |-> RouteIdentity(e),
    newRoute |-> RouteIdentity(e + 1),
    fence |-> FenceId(e),
    baseDigest |-> StateDigest(RouteIdentity(e + 1), 0, 0)
]
WrongResetIdentity(e) ==
    [ResetIdentity(e) EXCEPT !.baseDigest = <<"wrong-reset-digest">>]
NoResetIdentity == [
    oldRoute |-> NoRouteIdentity,
    newRoute |-> NoRouteIdentity,
    fence |-> "no-fence",
    baseDigest |-> NoIdentity
]
ResetIdentityDomain ==
    {ResetIdentity(0), WrongResetIdentity(0), NoResetIdentity}

RouteIdentityDomain ==
    {RouteIdentity(e) : e \in 0..1}
        \cup {WrongRouteIdentity(e) : e \in 0..1}
        \cup {NoRouteIdentity}

EventKinds == {
    "init", "ambiguous-f-loss",
    "c-codec-lease-issued", "c-codec-may-touch",
    "c-codec-returned-prepared", "c-codec-drain-required",
    "c-codec-returned-after-cancel", "c-codec-incarnation-replaced",
    "f-codec-lease-issued", "f-codec-may-touch",
    "f-codec-returned-prepared", "f-codec-drain-required",
    "f-codec-returned-after-cancel", "f-codec-incarnation-replaced",
    "drop-f-codec-job-and-reuse", "install-f-context-after-cancel",
    "duplicate-f-codec-lease", "drop-c-codec-job-and-reuse",
    "stale-codec-return-rejected", "stale-codec-return-installed",
    "c-encode", "writer-stall",
    "c-writer-tick", "reader-stall", "f-reader-tick",
    "short-disconnect", "complete-body-validation",
    "pre-touch-refusal", "retry-same-body", "pre-touch-reset",
    "feed-before-complete", "f-decode",
    "second-decode", "decode-failure", "materialize", "commit-permit",
    "owner-cancel-after-touch",
    "f-atomic-commit", "terminal-stall", "terminal-writer-tick",
    "lost-final-observation", "retry-last-commit", "c-observe-commit",
    "duplicate-terminal", "early-c-observe", "resend-after-commit",
    "c-context-loss", "f-context-loss", "drop-liability",
    "old-route-start", "classify-committed", "classify-not-committed",
    "reconcile-stall", "reconcile-required", "reset-offer",
    "reset-ack", "reset-commit", "resource-overflow",
    "active-routes-overflow"
}

BoundEvent(kind, route, op, reset) == [
    kind |-> kind,
    route |-> route,
    operation |-> op,
    reset |-> reset
]

NoEvent == BoundEvent("init", NoRouteIdentity, NoOperation, NoResetIdentity)
OperationEvent(kind, op) ==
    BoundEvent(kind, op.route, op, NoResetIdentity)
RouteEvent(kind, route, op) ==
    BoundEvent(kind, route, op, NoResetIdentity)
ResetEvent(kind, reset) ==
    BoundEvent(kind, reset.oldRoute, NoOperation, reset)

Phases == {
    "aligned", "c-codec-leased", "c-codec-running",
    "c-codec-drain-required",
    "receiving", "body-complete", "validated",
    "f-codec-leased", "f-codec-running", "f-codec-drain-required",
    "pre-touch-terminal", "decoded",
    "materialized", "f-committed", "reset-required", "reset-classified",
    "reset-offered", "reset-acked", "reconcile-required", "terminal-failure"
}
ResetPhases == {
    "reset-required", "reset-classified", "reset-offered", "reset-acked"
}
DrainPhases == {"c-codec-drain-required", "f-codec-drain-required"}
CodecStates == {"available", "leased", "poisoned", "lost"}
CodecLeaseStates == {
    "none", "issued", "may-have-touched", "returned-prepared",
    "drain-required", "destroyed", "installed"
}
ContextOwners == {"route", "worker", "prepared", "none"}
Liabilities == {
    "none", "codec-only", "known-not-committed", "commit-may-have-happened"
}
SettlementClasses == {"none", "not-committed", "committed", "reconcile-required"}
FaultClosures == {"none", "reset-complete", "reconcile-required", "terminal-failure"}
PreTouchDispositions == {"none", "pending", "retry-same-body", "reset-required"}

ASSUME
    /\ (MutantCollapsedTU \/ (T0 # T1 /\ T0 # T2 /\ T1 # T2))
    /\ (MutantSingleByteAlphabet \/ B0 # B1)
    /\ NoTU \notin TUs
    /\ MaxFragments = 2
    /\ MaxEncodedBytes = 4
    /\ MaxRawBytes = 2
    /\ MaxWriterTicks > 0
    /\ MaxReaderTicks > 0
    /\ MaxTerminalTicks > 0
    /\ CodecUnits > 0
    /\ MaxWindowBytes >= CodecUnits
    /\ MaxActiveRoutes > 0
    /\ PerRouteCap > 0
    /\ GlobalCap >= MaxActiveRoutes * PerRouteCap
    /\ MutantKind \in 0..39

VARIABLE s
vars == <<s>>

PayloadResource(x) ==
    Len(x.retainedBody)
    + Len(x.wireFragment)
    + Len(x.fBuffer)
    + Len(x.materializedRaw)
    + (IF x.inputPublished THEN Len(x.materializedRaw) ELSE 0)
    + (IF x.lastCommitPresent
          THEN Len(x.lastCommitBody) + Len(x.lastCommitRaw)
          ELSE 0)
    + x.resourceLeak

ResourceOf(x) ==
    CodecUnits * (x.cCodecCopies + x.fCodecCopies) + PayloadResource(x)

SettledSet(x) == {x.settledOps[i] : i \in DOMAIN x.settledOps}

InitialState == [
    phase |-> "aligned",
    epoch |-> 0,
    nextIndex |-> 0,
    cLogical |-> 0,
    fLogical |-> 0,
    cCommittedTag |-> 0,
    fCommittedTag |-> 0,
    cTag |-> 0,
    fTag |-> 0,
    cHighWater |-> 0,
    cEncodeCount |-> 0,
    cDigest |-> StateDigest(RouteIdentity(0), 0, 0),
    fDigest |-> StateDigest(RouteIdentity(0), 0, 0),
    cCodec |-> "available",
    fCodec |-> "available",
    cCodecCopies |-> 1,
    fCodecCopies |-> 1,
    cLeaseState |-> "none",
    fLeaseState |-> "none",
    cContextOwner |-> "route",
    fContextOwner |-> "route",
    cContextGeneration |-> ContextGeneration(0),
    fContextGeneration |-> ContextGeneration(0),
    cCodecJobId |-> NoCodecJobId,
    fCodecJobId |-> NoCodecJobId,
    cLeaseOp |-> NoOperation,
    fLeaseOp |-> NoOperation,
    cBodyReady |-> FALSE,
    retiredCodecJobs |-> {},
    installedCodecJobs |-> {},
    outstandingCodecJobs |-> {},
    staleCodecReturnsRejected |-> {},
    activeRoutes |-> 1,
    pendingOp |-> NoOperation,
    pendingTU |-> NoTU,
    preCursor |-> 0,
    preTag |-> 0,
    preDigest |-> StateDigest(RouteIdentity(0), 0, 0),
    postTag |-> 0,
    retainedBody |-> <<>>,
    replayBody |-> <<>>,
    wireFragment |-> <<>>,
    fBuffer |-> <<>>,
    writerFragment |-> 0,
    readerFragments |-> 0,
    writerTicks |-> 0,
    readerTicks |-> 0,
    terminalTicks |-> 0,
    progressPulse |-> 0,
    replays |-> 0,
    bodySends |-> 0,
    sendsAtFCommit |-> 0,
    decoderAttempts |-> 0,
    decoderTouched |-> FALSE,
    materializedRaw |-> <<>>,
    inputPublished |-> FALSE,
    preparedPresent |-> FALSE,
    preparedOp |-> NoOperation,
    preparedId |-> NoIdentity,
    permitPresent |-> FALSE,
    permitOp |-> NoOperation,
    permitPreparedId |-> NoIdentity,
    permitId |-> NoIdentity,
    permitConsumed |-> FALSE,
    issuedPermits |-> {},
    consumedPermits |-> {},
    publishedOps |-> {},
    settledOps |-> <<>>,
    settlementHighWater |-> 0,
    lastCommitPresent |-> FALSE,
    lastCommitOp |-> NoOperation,
    previousCommitOp |-> NoOperation,
    lastCommitBody |-> <<>>,
    lastCommitRaw |-> <<>>,
    lastCommitSuccessorDigest |-> NoIdentity,
    lastCommitSuccessorTag |-> 0,
    lastCommitPreparedId |-> NoIdentity,
    lastCommitPermitId |-> NoIdentity,
    lastCommitContextGeneration |-> 0,
    lastCommitCodecJobId |-> NoCodecJobId,
    commitOwed |-> FALSE,
    terminalReady |-> FALSE,
    ackLost |-> FALSE,
    retryIssued |-> FALSE,
    lastFIdentity |-> NoOperation,
    lastCIdentity |-> NoOperation,
    fTerminalCount |-> 0,
    cTerminalCount |-> 0,
    duplicateObserved |-> FALSE,
    liability |-> "none",
    liabilityOp |-> NoOperation,
    oldEpochOp |-> NoOperation,
    resetRequired |-> FALSE,
    oldRouteFenced |-> FALSE,
    resetIdentity |-> NoResetIdentity,
    settlementClass |-> "none",
    lossObserved |-> FALSE,
    touchedFailurePending |-> FALSE,
    preTouchDisposition |-> "none",
    faultClosure |-> "none",
    retiredEpoch0 |-> FALSE,
    resourceBeforeRelease |-> 0,
    resourceLeak |-> 0,
    lastEvent |-> "init",
    eventIdentity |-> NoEvent
]

Init == s = InitialState

AmbiguousInitialState ==
    LET body == Encode(0, T0)
        op == OperationIdentity(0, 0, T0, 0,
                                StateDigest(RouteIdentity(0), 0, 0), body)
    IN [InitialState EXCEPT
        !.phase = "reset-required",
        !.cTag = 1,
        !.cHighWater = 1,
        !.cEncodeCount = 1,
        !.fCodec = "lost",
        !.fCodecCopies = 0,
        !.fContextOwner = "none",
        !.pendingOp = op,
        !.pendingTU = T0,
        !.postTag = 1,
        !.retainedBody = body,
        !.replayBody = body,
        !.liability = "commit-may-have-happened",
        !.liabilityOp = op,
        !.resetRequired = TRUE,
        !.lossObserved = TRUE,
        !.lastEvent = "ambiguous-f-loss",
        !.eventIdentity = OperationEvent("ambiguous-f-loss", op)]

AmbiguousInit == s = AmbiguousInitialState

CanStart ==
    /\ s.phase = "aligned"
    /\ s.nextIndex < 3
    /\ s.pendingOp = NoOperation
    /\ s.liability = "none"
    /\ ~s.resetRequired
    /\ s.cCodec = "available" /\ s.fCodec = "available"
    /\ s.cCodecCopies = 1 /\ s.fCodecCopies = 1
    /\ s.cLeaseState = "none" /\ s.fLeaseState = "none"
    /\ s.cContextOwner = "route" /\ s.fContextOwner = "route"
    /\ s.cLogical = s.fLogical
    /\ s.cCommittedTag = s.fCommittedTag
    /\ s.cTag = s.cCommittedTag
    /\ s.fTag = s.fCommittedTag

StartTU ==
    /\ CanStart
    /\ LET tu == TUAt(s.nextIndex)
           correctBody == Encode(s.cTag, tu)
           body == IF (MutantFrameEnd \/ MutantInitialContextReuse)
                     /\ s.nextIndex > 0
                  THEN Encode(0, tu)
                  ELSE correctBody
           post == s.cTag + 1
           op == OperationIdentity(s.epoch, s.nextIndex, tu,
                                   s.cLogical, s.cDigest, body)
       IN s' = [s EXCEPT
            !.phase = "c-codec-leased",
            !.pendingOp = op,
            !.pendingTU = tu,
            !.preCursor = s.cLogical,
            !.preTag = s.cTag,
            !.preDigest = s.cDigest,
            !.postTag = post,
            !.retainedBody = body,
            !.replayBody = body,
            !.cCodec = "leased",
            !.cLeaseState = "issued",
            !.cContextOwner = "worker",
            !.cCodecJobId = CodecJobId(
                "C", op, s.cContextGeneration),
            !.outstandingCodecJobs = @ \cup {
                CodecJobId("C", op, s.cContextGeneration)},
            !.cLeaseOp = op,
            !.cBodyReady = FALSE,
            !.cEncodeCount = 1,
            !.cCodecCopies = IF MutantSecondCodec THEN 2 ELSE @,
            !.wireFragment = <<>>,
            !.fBuffer = <<>>,
            !.writerFragment = 0,
            !.readerFragments = 0,
            !.writerTicks = 0,
            !.readerTicks = 0,
            !.terminalTicks = 0,
            !.progressPulse = 0,
            !.replays = 0,
            !.bodySends = 0,
            !.sendsAtFCommit = 0,
            !.decoderAttempts = 0,
            !.decoderTouched = FALSE,
            !.materializedRaw = <<>>,
            !.inputPublished = FALSE,
            !.preparedPresent = FALSE,
            !.preparedOp = NoOperation,
            !.preparedId = NoIdentity,
            !.permitPresent = FALSE,
            !.permitOp = NoOperation,
            !.permitPreparedId = NoIdentity,
            !.permitId = NoIdentity,
            !.permitConsumed = FALSE,
            !.terminalReady = FALSE,
            !.ackLost = FALSE,
            !.retryIssued = FALSE,
            !.lastFIdentity = NoOperation,
            !.lastCIdentity = NoOperation,
            !.fTerminalCount = 0,
            !.cTerminalCount = 0,
            !.duplicateObserved = FALSE,
            !.settlementClass = "none",
            !.faultClosure = "none",
            !.preTouchDisposition = "none",
            !.lastEvent = "c-codec-lease-issued",
            !.eventIdentity = OperationEvent(
                "c-codec-lease-issued", op)]

CCodecMayHaveTouched ==
    /\ s.phase = "c-codec-leased"
    /\ s.cCodec = "leased"
    /\ s.cLeaseState = "issued"
    /\ s.cContextOwner = "worker"
    /\ s.cLeaseOp = s.pendingOp
    /\ s.cCodecJobId = CodecJobId(
            "C", s.pendingOp, s.cContextGeneration)
    /\ s' = [s EXCEPT
        !.phase = "c-codec-running",
        !.cLeaseState = "may-have-touched",
        !.lastEvent = "c-codec-may-touch",
        !.eventIdentity = OperationEvent(
            "c-codec-may-touch", s.pendingOp)]

CCodecReturnedPrepared ==
    /\ s.phase = "c-codec-running"
    /\ s.cCodec = "leased"
    /\ s.cLeaseState = "may-have-touched"
    /\ s.cContextOwner = "worker"
    /\ s.cLeaseOp = s.pendingOp
    /\ s.cCodecJobId = CodecJobId(
            "C", s.pendingOp, s.cContextGeneration)
    /\ s.cCodecJobId \notin s.retiredCodecJobs
    /\ s' = [s EXCEPT
        !.phase = "receiving",
        !.cTag = s.postTag,
        !.cHighWater = s.postTag,
        !.cLeaseState = "returned-prepared",
        !.cContextOwner = "prepared",
        !.cBodyReady = TRUE,
        !.outstandingCodecJobs = @ \ {s.cCodecJobId},
        !.lastEvent = "c-codec-returned-prepared",
        !.eventIdentity = OperationEvent(
            "c-codec-returned-prepared", s.pendingOp)]

CancelCCodecInFlight ==
    /\ s.phase \in {"c-codec-leased", "c-codec-running"}
    /\ s.cLeaseState \in {"issued", "may-have-touched"}
    /\ s.cCodec = "leased"
    /\ s.cContextOwner = "worker"
    /\ s.cCodecJobId # NoCodecJobId
    /\ s' = [s EXCEPT
        !.phase = "c-codec-drain-required",
        !.cLeaseState = "drain-required",
        !.liability = "known-not-committed",
        !.liabilityOp = s.pendingOp,
        !.resetRequired = TRUE,
        !.oldRouteFenced = FALSE,
        !.lossObserved = TRUE,
        !.touchedFailurePending = TRUE,
        !.lastEvent = "c-codec-drain-required",
        !.eventIdentity = OperationEvent(
            "c-codec-drain-required", s.pendingOp)]

CCodecReturnAfterCancel ==
    /\ s.phase = "c-codec-drain-required"
    /\ s.cLeaseState = "drain-required"
    /\ s.cContextOwner = "worker"
    /\ s.cCodecJobId # NoCodecJobId
    /\ s' = [s EXCEPT
        !.phase = "reset-required",
        !.cCodec = "lost",
        !.cCodecCopies = 0,
        !.cLeaseState = "destroyed",
        !.cContextOwner = "none",
        !.cBodyReady = FALSE,
        !.retiredCodecJobs = @ \cup {s.cCodecJobId},
        !.outstandingCodecJobs = @ \ {s.cCodecJobId},
        !.lastEvent = "c-codec-returned-after-cancel",
        !.eventIdentity = OperationEvent(
            "c-codec-returned-after-cancel", s.pendingOp)]

ReplaceCIncarnationAfterStall ==
    /\ s.phase = "c-codec-drain-required"
    /\ s.cLeaseState = "drain-required"
    /\ s.cContextOwner = "worker"
    /\ s.cCodecJobId # NoCodecJobId
    /\ s' = [s EXCEPT
        !.phase = "reset-required",
        !.cCodec = "lost",
        !.cCodecCopies = 0,
        !.cLeaseState = "destroyed",
        !.cContextOwner = "none",
        !.cBodyReady = FALSE,
        !.retiredCodecJobs = @ \cup {s.cCodecJobId},
        !.outstandingCodecJobs = @ \ {s.cCodecJobId},
        !.lastEvent = "c-codec-incarnation-replaced",
        !.eventIdentity = OperationEvent(
            "c-codec-incarnation-replaced", s.pendingOp)]

DropCCodecJobAndReuse ==
    /\ MutantDropCJobAndReuse
    /\ s.phase = "c-codec-drain-required"
    /\ s.cCodecJobId \in s.outstandingCodecJobs
    /\ s' = [s EXCEPT
        !.phase = "aligned",
        !.cCodec = "available",
        !.cCodecCopies = 1,
        !.cLeaseState = "none",
        !.cContextOwner = "route",
        !.resetRequired = FALSE,
        !.touchedFailurePending = FALSE,
        !.lossObserved = FALSE,
        !.liability = "none",
        !.liabilityOp = NoOperation,
        !.lastEvent = "drop-c-codec-job-and-reuse",
        !.eventIdentity = OperationEvent(
            "drop-c-codec-job-and-reuse", s.pendingOp)]

CWriterTick ==
    /\ s.phase = "receiving"
    /\ s.cBodyReady
    /\ s.cLeaseState = "returned-prepared"
    /\ s.cContextOwner = "prepared"
    /\ s.wireFragment = <<>>
    /\ s.writerFragment < MaxFragments
    /\ IF MutantStalledWriter
          THEN s' = [s EXCEPT
                    !.writerTicks = IF @ < MaxWriterTicks THEN @ + 1 ELSE @,
                    !.progressPulse = 1 - @,
                    !.lastEvent = "writer-stall",
                    !.eventIdentity = OperationEvent(
                        "writer-stall", s.pendingOp)]
          ELSE LET sourceBody == IF MutantEqualLengthCorruption
                                    THEN CorruptSameLength(s.retainedBody)
                                    ELSE s.retainedBody
                   fragment == Fragment(sourceBody, s.writerFragment)
               IN s' = [s EXCEPT
                    !.wireFragment = fragment,
                    !.writerFragment = @ + 1,
                    !.writerTicks = IF @ < MaxWriterTicks THEN @ + 1 ELSE @,
                    !.bodySends = @ + 1,
                    !.lastEvent = "c-writer-tick",
                    !.eventIdentity = OperationEvent(
                        "c-writer-tick", s.pendingOp)]

FReaderTick ==
    /\ s.phase = "receiving"
    /\ s.wireFragment # <<>>
    /\ s.readerFragments < MaxFragments
    /\ IF MutantStalledReader
          THEN s' = [s EXCEPT
                    !.readerTicks = IF @ < MaxReaderTicks THEN @ + 1 ELSE @,
                    !.progressPulse = 1 - @,
                    !.lastEvent = "reader-stall",
                    !.eventIdentity = OperationEvent(
                        "reader-stall", s.pendingOp)]
          ELSE LET newBuffer == s.fBuffer \o s.wireFragment
               IN s' = [s EXCEPT
                    !.fBuffer = newBuffer,
                    !.wireFragment = <<>>,
                    !.readerFragments = @ + 1,
                    !.readerTicks = IF @ < MaxReaderTicks THEN @ + 1 ELSE @,
                    !.phase = IF Len(newBuffer) = MaxEncodedBytes
                                  THEN "body-complete" ELSE @,
                    !.lastEvent = "f-reader-tick",
                    !.eventIdentity = OperationEvent(
                        "f-reader-tick", s.pendingOp)]

ShortDisconnect ==
    /\ s.phase = "receiving"
    /\ Len(s.fBuffer) > 0
    /\ Len(s.fBuffer) < MaxEncodedBytes
    /\ s.replays < 1
    /\ s' = [s EXCEPT
        !.wireFragment = <<>>,
        !.fBuffer = <<>>,
        !.writerFragment = 0,
        !.readerFragments = 0,
        !.writerTicks = 0,
        !.readerTicks = 0,
        !.replays = @ + 1,
        !.lastEvent = "short-disconnect",
        !.eventIdentity = OperationEvent("short-disconnect", s.pendingOp)]

CompleteBodyValidation ==
    /\ s.phase = "body-complete"
    /\ s.wireFragment = <<>>
    /\ Len(s.fBuffer) = s.pendingOp.encoded.length
    /\ s.pendingOp.route = RouteIdentity(s.epoch)
    /\ s.pendingOp.predecessorCursor = s.preCursor
    /\ s.pendingOp.predecessorDigest = s.preDigest
    /\ s.fBuffer = s.retainedBody \/ MutantEqualLengthCorruption
    /\ LET eventOp == IF MutantWrongBodyEventIdentity
                         THEN WrongBodyOperationIdentity(s.pendingOp)
                         ELSE s.pendingOp
       IN s' = [s EXCEPT
            !.phase = "validated",
            !.lastEvent = "complete-body-validation",
            !.eventIdentity = OperationEvent(
                "complete-body-validation", eventOp)]

PreTouchRefusal ==
    /\ s.phase \in {"receiving", "body-complete", "validated"}
    /\ s.decoderAttempts = 0
    /\ s' = [s EXCEPT
        !.phase = "pre-touch-terminal",
        !.cTag = IF MutantCRollback THEN s.preTag ELSE @,
        !.liability = "known-not-committed",
        !.liabilityOp = s.pendingOp,
        !.resetRequired = FALSE,
        !.oldRouteFenced = FALSE,
        !.preTouchDisposition = "pending",
        !.lastEvent = "pre-touch-refusal",
        !.eventIdentity = OperationEvent(
            "pre-touch-refusal", s.pendingOp)]

RetrySameBodyAfterPreTouchTerminal ==
    /\ s.phase = "pre-touch-terminal"
    /\ s.preTouchDisposition = "pending"
    /\ s.decoderAttempts = 0 /\ ~s.decoderTouched
    /\ s.fCodec = "available" /\ s.fTag = s.preTag
    /\ s.pendingOp # NoOperation
    /\ s.pendingOp.encoded = EncodedIdentity(s.retainedBody)
    /\ s.replays < 1
    /\ s' = [s EXCEPT
        !.phase = "receiving",
        !.wireFragment = <<>>,
        !.fBuffer = <<>>,
        !.writerFragment = 0,
        !.readerFragments = 0,
        !.writerTicks = 0,
        !.readerTicks = 0,
        !.terminalTicks = 0,
        !.replays = @ + 1,
        !.liability = "none",
        !.liabilityOp = NoOperation,
        !.cEncodeCount = IF MutantPreTouchReencode THEN @ + 1 ELSE @,
        !.preTouchDisposition = "retry-same-body",
        !.lastEvent = "retry-same-body",
        !.eventIdentity = OperationEvent(
            "retry-same-body", s.pendingOp)]

EscalatePreTouchTerminalToReset ==
    /\ s.phase = "pre-touch-terminal"
    /\ s.preTouchDisposition = "pending"
    /\ s.liability = "known-not-committed"
    /\ s.liabilityOp = s.pendingOp
    /\ s' = [s EXCEPT
        !.phase = "reset-required",
        !.cCodec = "lost",
        !.cCodecCopies = 0,
        !.cLeaseState = "destroyed",
        !.cContextOwner = "none",
        !.cBodyReady = FALSE,
        !.retiredCodecJobs = @ \cup {s.cCodecJobId},
        !.resetRequired = TRUE,
        !.preTouchDisposition = "reset-required",
        !.lastEvent = "pre-touch-reset",
        !.eventIdentity = OperationEvent(
            "pre-touch-reset", s.pendingOp)]

FeedDecoderBeforeComplete ==
    /\ MutantFeedBeforeComplete
    /\ s.phase = "receiving"
    /\ Len(s.fBuffer) > 0
    /\ Len(s.fBuffer) < MaxEncodedBytes
    /\ s' = [s EXCEPT
        !.decoderAttempts = 1,
        !.decoderTouched = TRUE,
        !.fTag = s.postTag,
        !.lastEvent = "feed-before-complete",
        !.eventIdentity = OperationEvent(
            "feed-before-complete", s.pendingOp)]

IssueFCodecLease ==
    /\ s.phase = "validated"
    /\ s.decoderAttempts = 0
    /\ s.fCodec = "available"
    /\ s.fCodecCopies = 1
    /\ s.fLeaseState = "none"
    /\ s.fContextOwner = "route"
    /\ s.fTag = s.preTag
    /\ s.fBuffer = s.retainedBody
    /\ DecodeMatches(s.preTag, s.fBuffer, s.pendingTU)
    /\ s' = [s EXCEPT
        !.phase = "f-codec-leased",
        !.fCodec = "leased",
        !.fLeaseState = "issued",
        !.fContextOwner = "worker",
        !.fCodecJobId = CodecJobId(
            "F", s.pendingOp, s.fContextGeneration),
        !.outstandingCodecJobs = @ \cup {
            CodecJobId("F", s.pendingOp, s.fContextGeneration)},
        !.fLeaseOp = s.pendingOp,
        !.lastEvent = "f-codec-lease-issued",
        !.eventIdentity = OperationEvent(
            "f-codec-lease-issued", s.pendingOp)]

FCodecMayHaveTouched ==
    /\ s.phase = "f-codec-leased"
    /\ s.decoderAttempts = 0
    /\ s.fCodec = "leased"
    /\ s.fLeaseState = "issued"
    /\ s.fContextOwner = "worker"
    /\ s.fLeaseOp = s.pendingOp
    /\ s.fCodecJobId = CodecJobId(
            "F", s.pendingOp, s.fContextGeneration)
    /\ s' = [s EXCEPT
        !.phase = "f-codec-running",
        !.decoderAttempts = 1,
        !.decoderTouched = TRUE,
        !.fLeaseState = "may-have-touched",
        !.lastEvent = "f-codec-may-touch",
        !.eventIdentity = OperationEvent(
            "f-codec-may-touch", s.pendingOp)]

FCodecReturnedPrepared ==
    /\ s.phase = "f-codec-running"
    /\ s.decoderAttempts = 1
    /\ s.decoderTouched
    /\ s.fCodec = "leased"
    /\ s.fLeaseState = "may-have-touched"
    /\ s.fContextOwner = "worker"
    /\ s.fLeaseOp = s.pendingOp
    /\ s.fCodecJobId = CodecJobId(
            "F", s.pendingOp, s.fContextGeneration)
    /\ s.fCodecJobId \notin s.retiredCodecJobs
    /\ s.fBuffer = s.retainedBody
    /\ DecodeMatches(s.preTag, s.fBuffer, s.pendingTU)
    /\ s' = [s EXCEPT
        !.phase = "decoded",
        !.fTag = s.postTag,
        !.fLeaseState = "returned-prepared",
        !.fContextOwner = "prepared",
        !.outstandingCodecJobs = @ \ {s.fCodecJobId},
        !.lastEvent = "f-codec-returned-prepared",
        !.eventIdentity = OperationEvent(
            "f-codec-returned-prepared", s.pendingOp)]

FDecodeSuccess == FCodecReturnedPrepared

CancelFCodecInFlight ==
    /\ s.phase \in {"f-codec-leased", "f-codec-running"}
    /\ s.fLeaseState \in {"issued", "may-have-touched"}
    /\ s.fCodec = "leased"
    /\ s.fContextOwner = "worker"
    /\ s.fCodecJobId # NoCodecJobId
    /\ s' = [s EXCEPT
        !.phase = "f-codec-drain-required",
        !.cCodec = "lost",
        !.cCodecCopies = 0,
        !.cLeaseState = "destroyed",
        !.cContextOwner = "none",
        !.cBodyReady = FALSE,
        !.fLeaseState = "drain-required",
        !.retiredCodecJobs = @ \cup {s.cCodecJobId},
        !.liability = "known-not-committed",
        !.liabilityOp = s.pendingOp,
        !.resetRequired = TRUE,
        !.oldRouteFenced = FALSE,
        !.lossObserved = TRUE,
        !.touchedFailurePending = TRUE,
        !.lastEvent = "f-codec-drain-required",
        !.eventIdentity = OperationEvent(
            "f-codec-drain-required", s.pendingOp)]

FCodecReturnAfterCancel ==
    /\ s.phase = "f-codec-drain-required"
    /\ s.fLeaseState = "drain-required"
    /\ s.fContextOwner = "worker"
    /\ s.fCodecJobId # NoCodecJobId
    /\ s' = [s EXCEPT
        !.phase = "reset-required",
        !.fCodec = "lost",
        !.fCodecCopies = 0,
        !.fLeaseState = "destroyed",
        !.fContextOwner = "none",
        !.retiredCodecJobs = @ \cup {s.fCodecJobId},
        !.outstandingCodecJobs = @ \ {s.fCodecJobId},
        !.lastEvent = "f-codec-returned-after-cancel",
        !.eventIdentity = OperationEvent(
            "f-codec-returned-after-cancel", s.pendingOp)]

ReplaceFIncarnationAfterStall ==
    /\ s.phase = "f-codec-drain-required"
    /\ s.fLeaseState = "drain-required"
    /\ s.fContextOwner = "worker"
    /\ s.fCodecJobId # NoCodecJobId
    /\ s' = [s EXCEPT
        !.phase = "reset-required",
        !.fCodec = "lost",
        !.fCodecCopies = 0,
        !.fLeaseState = "destroyed",
        !.fContextOwner = "none",
        !.retiredCodecJobs = @ \cup {s.fCodecJobId},
        !.outstandingCodecJobs = @ \ {s.fCodecJobId},
        !.lastEvent = "f-codec-incarnation-replaced",
        !.eventIdentity = OperationEvent(
            "f-codec-incarnation-replaced", s.pendingOp)]

StaleCodecReturnCanArrive(side) ==
    /\ side \in {"C", "F"}
    /\ s.epoch = 1
    /\ ProperOperationShape(s.oldEpochOp)
    /\ ProperOperationShape(s.pendingOp)
    /\ s.oldEpochOp.route # s.pendingOp.route
    /\ StaleCodecJob(side, s) \in s.retiredCodecJobs
    /\ StaleCodecJob(side, s) \notin s.staleCodecReturnsRejected
    /\ IF side = "C"
          THEN /\ s.cContextOwner = "worker"
               /\ s.cLeaseOp = s.pendingOp
               /\ s.cCodecJobId \in s.outstandingCodecJobs
          ELSE /\ s.fContextOwner = "worker"
               /\ s.fLeaseOp = s.pendingOp
               /\ s.fCodecJobId \in s.outstandingCodecJobs

RejectStaleCodecReturn(side) ==
    /\ StaleCodecReturnCanArrive(side)
    /\ LET staleJob == StaleCodecJob(side, s)
       IN s' = [s EXCEPT
            !.staleCodecReturnsRejected = @ \cup {staleJob},
            !.lastEvent = "stale-codec-return-rejected",
            !.eventIdentity = OperationEvent(
                "stale-codec-return-rejected", s.oldEpochOp)]

InstallStaleCodecReturn ==
    /\ MutantInstallStaleCodecReturn
    /\ StaleCodecReturnCanArrive("F")
    /\ LET staleJob == StaleCodecJob("F", s)
       IN s' = [s EXCEPT
            !.installedCodecJobs = @ \cup {staleJob},
            !.lastEvent = "stale-codec-return-installed",
            !.eventIdentity = OperationEvent(
                "stale-codec-return-installed", s.oldEpochOp)]

DropFCodecJobAndReuse ==
    /\ MutantDropFJobAndReuse
    /\ s.phase = "f-codec-drain-required"
    /\ s.fCodecJobId \in s.outstandingCodecJobs
    /\ s' = [s EXCEPT
        !.phase = "aligned",
        !.cCodec = "available",
        !.cCodecCopies = 1,
        !.cLeaseState = "none",
        !.cContextOwner = "route",
        !.fCodec = "available",
        !.fCodecCopies = 1,
        !.fLeaseState = "none",
        !.fContextOwner = "route",
        !.resetRequired = FALSE,
        !.touchedFailurePending = FALSE,
        !.lossObserved = FALSE,
        !.liability = "none",
        !.liabilityOp = NoOperation,
        !.lastEvent = "drop-f-codec-job-and-reuse",
        !.eventIdentity = OperationEvent(
            "drop-f-codec-job-and-reuse", s.pendingOp)]

InstallFContextAfterCancel ==
    /\ MutantInstallFContextAfterCancel
    /\ s.phase = "f-codec-drain-required"
    /\ s.fCodecJobId \in s.outstandingCodecJobs
    /\ s' = [s EXCEPT
        !.phase = "f-committed",
        !.fCodec = "available",
        !.fCodecCopies = 1,
        !.fLeaseState = "installed",
        !.fContextOwner = "route",
        !.fLogical = s.preCursor + 1,
        !.fCommittedTag = s.postTag,
        !.fTag = s.postTag,
        !.fDigest = StateDigest(RouteIdentity(s.epoch),
                                s.preCursor + 1, s.postTag),
        !.installedCodecJobs = @ \cup {s.fCodecJobId},
        !.outstandingCodecJobs = @ \ {s.fCodecJobId},
        !.lastEvent = "install-f-context-after-cancel",
        !.eventIdentity = OperationEvent(
            "install-f-context-after-cancel", s.pendingOp)]

DuplicateFCodecLease ==
    /\ MutantDuplicateFLease
    /\ s.phase = "f-codec-leased"
    /\ s.fLeaseState = "issued"
    /\ s.fCodecCopies = 1
    /\ s' = [s EXCEPT
        !.fCodecCopies = 2,
        !.lastEvent = "duplicate-f-codec-lease",
        !.eventIdentity = OperationEvent(
            "duplicate-f-codec-lease", s.pendingOp)]

SecondDecode ==
    /\ MutantSecondDecode
    /\ s.phase \in {"decoded", "materialized"}
    /\ s.decoderAttempts = 1
    /\ s' = [s EXCEPT
        !.decoderAttempts = 2,
        !.lastEvent = "second-decode",
        !.eventIdentity = OperationEvent("second-decode", s.pendingOp)]

FDecodeFailure ==
    /\ s.phase = "f-codec-running"
    /\ s.decoderAttempts = 1
    /\ s.decoderTouched
    /\ s.fCodec = "leased"
    /\ s.fLeaseState = "may-have-touched"
    /\ s.fContextOwner = "worker"
    /\ s' = [s EXCEPT
        !.phase = IF MutantContinueAfterTouch THEN "aligned" ELSE "reset-required",
        !.cCodec = IF MutantContinueAfterTouch THEN "available" ELSE "lost",
        !.cCodecCopies = IF MutantContinueAfterTouch THEN 1 ELSE 0,
        !.cLeaseState = IF MutantContinueAfterTouch THEN "none" ELSE "destroyed",
        !.cContextOwner = IF MutantContinueAfterTouch THEN "route" ELSE "none",
        !.cBodyReady = FALSE,
        !.fCodec = IF MutantContinueAfterTouch THEN "available" ELSE "poisoned",
        !.fCodecCopies = IF MutantContinueAfterTouch THEN 1 ELSE 0,
        !.fLeaseState = IF MutantContinueAfterTouch THEN "none" ELSE "destroyed",
        !.fContextOwner = IF MutantContinueAfterTouch THEN "route" ELSE "none",
        !.retiredCodecJobs = @ \cup {s.cCodecJobId, s.fCodecJobId},
        !.outstandingCodecJobs = @ \ {s.fCodecJobId},
        !.liability = "known-not-committed",
        !.liabilityOp = s.pendingOp,
        !.resetRequired = TRUE,
        !.oldRouteFenced = FALSE,
        !.touchedFailurePending = TRUE,
        !.lastEvent = "decode-failure",
        !.eventIdentity = OperationEvent("decode-failure", s.pendingOp)]

Materialize ==
    /\ s.phase = "decoded"
    /\ s.decoderAttempts = 1
    /\ s.fCodec = "leased"
    /\ s.fLeaseState = "returned-prepared"
    /\ s.fContextOwner = "prepared"
    /\ s.fTag = s.postTag
    /\ s' = [s EXCEPT
        !.phase = "materialized",
        !.materializedRaw = RawBytes(s.pendingTU),
        !.preparedPresent = TRUE,
        !.preparedOp = s.pendingOp,
        !.preparedId = PreparedId(s.pendingOp),
        !.lastEvent = "materialize",
        !.eventIdentity = OperationEvent("materialize", s.pendingOp)]

AcceptCommitPermit ==
    /\ s.phase = "materialized"
    /\ s.preparedPresent
    /\ s.preparedOp = s.pendingOp
    /\ s.preparedId = PreparedId(s.pendingOp)
    /\ ~s.permitPresent
    /\ ~s.permitConsumed
    /\ LET permit == PermitId(s.pendingOp, s.preparedId)
       IN /\ permit \notin s.consumedPermits
          /\ permit \notin s.issuedPermits
          /\ s' = [s EXCEPT
                !.permitPresent = TRUE,
                !.permitOp = s.pendingOp,
                !.permitPreparedId = s.preparedId,
                !.permitId = permit,
                !.issuedPermits = @ \cup {permit},
                !.lastEvent = "commit-permit",
                !.eventIdentity = OperationEvent(
                    "commit-permit", s.pendingOp)]

OwnerCancelAfterTouch ==
    /\ s.phase = "materialized"
    /\ s.decoderTouched /\ s.decoderAttempts = 1
    /\ s.preparedPresent /\ s.preparedOp = s.pendingOp
    /\ ~s.permitPresent /\ ~s.permitConsumed
    /\ s.fLeaseState = "returned-prepared"
    /\ s.fContextOwner = "prepared"
    /\ s' = [s EXCEPT
        !.phase = "reset-required",
        !.cCodec = "lost",
        !.cCodecCopies = 0,
        !.cLeaseState = "destroyed",
        !.cContextOwner = "none",
        !.cBodyReady = FALSE,
        !.fCodec = "poisoned",
        !.fCodecCopies = 0,
        !.fLeaseState = "destroyed",
        !.fContextOwner = "none",
        !.retiredCodecJobs = @ \cup {s.cCodecJobId, s.fCodecJobId},
        !.preparedPresent = FALSE,
        !.preparedOp = NoOperation,
        !.preparedId = NoIdentity,
        !.liability = "known-not-committed",
        !.liabilityOp = s.pendingOp,
        !.resetRequired = TRUE,
        !.oldRouteFenced = FALSE,
        !.touchedFailurePending = TRUE,
        !.lossObserved = TRUE,
        !.lastEvent = "owner-cancel-after-touch",
        !.eventIdentity = OperationEvent(
            "owner-cancel-after-touch", s.pendingOp)]

FCommit ==
    /\ s.phase = "materialized"
    /\ s.materializedRaw = RawBytes(s.pendingTU)
    /\ s.decoderAttempts = 1
    /\ s.fLogical = s.preCursor
    /\ s.fCodec = "leased"
    /\ s.fLeaseState = "returned-prepared"
    /\ s.fContextOwner = "prepared"
    /\ s.fCodecJobId = CodecJobId(
            "F", s.pendingOp, s.fContextGeneration)
    /\ s.fCodecJobId \notin s.retiredCodecJobs
    /\ s.preparedPresent
    /\ s.preparedOp = s.pendingOp
    /\ s.preparedId = PreparedId(s.pendingOp)
    /\ s.permitPresent
    /\ ~s.permitConsumed
    /\ s.permitOp = s.pendingOp
    /\ s.permitPreparedId = s.preparedId
    /\ s.permitId = PermitId(s.pendingOp, s.preparedId)
    /\ s.permitId \in s.issuedPermits
    /\ s.permitId \notin s.consumedPermits
    /\ LET exactDigest == StateDigest(RouteIdentity(s.epoch),
                                      s.fLogical + 1, s.postTag)
           terminalOp ==
              IF MutantWrongTerminalIdentity
                 THEN WrongOperationIdentity(s.pendingOp)
              ELSE IF MutantSameEpochABA /\ s.lastCommitPresent
                 THEN s.lastCommitOp
              ELSE IF MutantStaleAfterReset /\ s.epoch = 1
                      /\ s.oldEpochOp # NoOperation
                 THEN s.oldEpochOp
              ELSE s.pendingOp
       IN s' = [s EXCEPT
            !.phase = "f-committed",
            !.fCodec = "available",
            !.fLeaseState = "installed",
            !.fContextOwner = "route",
            !.installedCodecJobs = @ \cup {s.fCodecJobId},
            !.fLogical = @ + 1,
            !.fCommittedTag = s.postTag,
            !.fDigest = IF MutantStateDigest
                           THEN <<"bad-state-digest">> ELSE exactDigest,
            !.inputPublished = ~MutantCommitBeforePublish,
            !.publishedOps = IF MutantCommitBeforePublish
                                THEN @ ELSE @ \cup {s.pendingOp},
            !.permitPresent = FALSE,
            !.permitConsumed = TRUE,
            !.consumedPermits = @ \cup {s.permitId},
            !.previousCommitOp = IF s.lastCommitPresent
                                    THEN s.lastCommitOp ELSE @,
            !.lastCommitPresent = ~MutantDropLastCommit,
            !.lastCommitOp = terminalOp,
            !.lastCommitBody = IF MutantDropLastCommit
                                  THEN CorruptSameLength(s.retainedBody)
                                  ELSE s.retainedBody,
            !.lastCommitRaw = s.materializedRaw,
            !.lastCommitSuccessorDigest = exactDigest,
            !.lastCommitSuccessorTag = s.postTag,
            !.lastCommitPreparedId = s.preparedId,
            !.lastCommitPermitId = s.permitId,
            !.lastCommitContextGeneration = s.fContextGeneration,
            !.lastCommitCodecJobId = s.fCodecJobId,
            !.commitOwed = TRUE,
            !.terminalReady = FALSE,
            !.ackLost = FALSE,
            !.retryIssued = FALSE,
            !.sendsAtFCommit = s.bodySends,
            !.fTerminalCount = 1,
            !.lastFIdentity = terminalOp,
            !.lastEvent = "f-atomic-commit",
            !.eventIdentity = OperationEvent(
                "f-atomic-commit", terminalOp)]

TerminalWriterTick ==
    /\ s.phase = "f-committed"
    /\ s.commitOwed
    /\ ~s.terminalReady
    /\ IF MutantStalledTerminal
          THEN s' = [s EXCEPT
                    !.terminalTicks = IF @ < MaxTerminalTicks THEN @ + 1 ELSE @,
                    !.progressPulse = 1 - @,
                    !.lastEvent = "terminal-stall",
                    !.eventIdentity = OperationEvent(
                        "terminal-stall", s.lastCommitOp)]
          ELSE s' = [s EXCEPT
                    !.terminalReady = TRUE,
                    !.terminalTicks = IF @ < MaxTerminalTicks THEN @ + 1 ELSE @,
                    !.lastEvent = "terminal-writer-tick",
                    !.eventIdentity = OperationEvent(
                        "terminal-writer-tick", s.lastCommitOp)]

LoseFinalObservation ==
    /\ s.phase = "f-committed"
    /\ s.terminalReady
    /\ s.commitOwed
    /\ ~s.ackLost
    /\ s' = [s EXCEPT
        !.terminalReady = FALSE,
        !.ackLost = TRUE,
        !.lastEvent = "lost-final-observation",
        !.eventIdentity = OperationEvent(
            "lost-final-observation", s.lastCommitOp)]

RetryLastCommit ==
    /\ s.phase = "f-committed"
    /\ s.ackLost
    /\ s.commitOwed
    /\ s.lastCommitPresent
    /\ s.lastCommitBody = s.retainedBody
    /\ s.lastCommitRaw = s.materializedRaw
    /\ LET retryIdentity == IF MutantWrongCommitRetry
                               THEN WrongOperationIdentity(s.lastCommitOp)
                               ELSE s.lastCommitOp
       IN s' = [s EXCEPT
            !.terminalReady = TRUE,
            !.ackLost = FALSE,
            !.retryIssued = TRUE,
            !.lastFIdentity = retryIdentity,
            !.lastEvent = "retry-last-commit",
            !.eventIdentity = OperationEvent(
                "retry-last-commit", retryIdentity)]

CObserveCommit ==
    /\ s.phase = "f-committed"
    /\ s.terminalReady
    /\ s.commitOwed
    /\ s.lastCommitPresent
    /\ s.lastCommitOp = s.pendingOp
    /\ s.lastCommitBody = s.retainedBody
    /\ s.lastCommitRaw = s.materializedRaw
    /\ s.lastCommitSuccessorDigest = s.fDigest
    /\ s.pendingOp \in s.publishedOps
    /\ s.cCodec = "leased"
    /\ s.cLeaseState = "returned-prepared"
    /\ s.cContextOwner = "prepared"
    /\ s.cBodyReady
    /\ s.fCodec = "available"
    /\ s.fLeaseState = "installed"
    /\ s.fContextOwner = "route"
    /\ s.nextIndex < 3
    /\ s' = [s EXCEPT
        !.phase = "aligned",
        !.nextIndex = @ + 1,
        !.cLogical = s.fLogical,
        !.cCommittedTag = s.postTag,
        !.cDigest = s.fDigest,
        !.cCodec = "available",
        !.cLeaseState = "none",
        !.fLeaseState = "none",
        !.cContextOwner = "route",
        !.fContextOwner = "route",
        !.installedCodecJobs = @ \cup {s.cCodecJobId},
        !.cCodecJobId = NoCodecJobId,
        !.fCodecJobId = NoCodecJobId,
        !.cLeaseOp = NoOperation,
        !.fLeaseOp = NoOperation,
        !.cBodyReady = FALSE,
        !.settledOps = Append(@, s.pendingOp),
        !.settlementHighWater = s.nextIndex + 1,
        !.lastCIdentity = s.lastCommitOp,
        !.cTerminalCount = 1,
        !.pendingOp = NoOperation,
        !.pendingTU = NoTU,
        !.cEncodeCount = 0,
        !.retainedBody = <<>>,
        !.replayBody = <<>>,
        !.wireFragment = <<>>,
        !.fBuffer = <<>>,
        !.writerFragment = 0,
        !.readerFragments = 0,
        !.materializedRaw = <<>>,
        !.inputPublished = FALSE,
        !.preparedPresent = FALSE,
        !.preparedOp = NoOperation,
        !.preparedId = NoIdentity,
        !.permitPresent = FALSE,
        !.permitOp = NoOperation,
        !.permitPreparedId = NoIdentity,
        !.permitId = NoIdentity,
        !.permitConsumed = FALSE,
        !.commitOwed = FALSE,
        !.terminalReady = FALSE,
        !.ackLost = FALSE,
        !.retryIssued = FALSE,
        !.resourceBeforeRelease = PayloadResource(s),
        !.lastEvent = "c-observe-commit",
        !.eventIdentity = OperationEvent(
            "c-observe-commit", s.lastCommitOp)]

DuplicateTerminalObservation ==
    /\ s.phase = "aligned"
    /\ s.lastCommitPresent
    /\ ~s.commitOwed
    /\ s' = [s EXCEPT
        !.duplicateObserved = TRUE,
        !.nextIndex = IF MutantDuplicateSettlement /\ s.nextIndex < 3
                          THEN @ + 1 ELSE @,
        !.lastEvent = "duplicate-terminal",
        !.eventIdentity = OperationEvent(
            "duplicate-terminal", s.lastCommitOp)]

EarlyCObserve ==
    /\ MutantEarlyCObserve
    /\ s.phase = "materialized"
    /\ s.nextIndex < 3
    /\ s' = [s EXCEPT
        !.phase = "aligned",
        !.settledOps = Append(@, s.pendingOp),
        !.nextIndex = @ + 1,
        !.pendingOp = NoOperation,
        !.pendingTU = NoTU,
        !.cEncodeCount = 0,
        !.retainedBody = <<>>,
        !.fBuffer = <<>>,
        !.materializedRaw = <<>>,
        !.lastEvent = "early-c-observe",
        !.eventIdentity = OperationEvent(
            "early-c-observe", s.pendingOp)]

ResendBodyAfterCommit ==
    /\ MutantResendAfterCommit
    /\ s.phase = "f-committed"
    /\ s.commitOwed
    /\ s' = [s EXCEPT
        !.bodySends = @ + 1,
        !.lastEvent = "resend-after-commit",
        !.eventIdentity = OperationEvent(
            "resend-after-commit", s.lastCommitOp)]

LoseCContext ==
    /\ s.epoch < 1
    /\ s.phase \notin ResetPhases
    /\ s.phase \notin {"reconcile-required", "terminal-failure"}
    \* SelectCommit is the owner race winner.  Once its exact permit exists,
    \* context installation and durable commit are one indivisible owner turn;
    \* a later loss/cancel transition cannot revoke that selected authority.
    /\ ~s.permitPresent
    /\ s.cCodec \in {"available", "leased"}
    /\ s.cContextOwner # "worker"
    /\ s.fContextOwner # "worker"
    /\ LET hasPending == s.pendingOp # NoOperation
           op == IF hasPending THEN s.pendingOp ELSE s.lastCommitOp
           mayCommit == s.phase = "f-committed" /\ s.commitOwed
       IN s' = [s EXCEPT
            !.phase = "reset-required",
            !.cCodec = "lost",
            !.cCodecCopies = 0,
            !.cLeaseState = IF s.cCodecJobId # NoCodecJobId
                                THEN "destroyed" ELSE "none",
            !.cContextOwner = "none",
            !.cBodyReady = FALSE,
            !.retiredCodecJobs = @ \cup JobSet(s.cCodecJobId),
            !.preparedPresent = FALSE,
            !.preparedOp = NoOperation,
            !.preparedId = NoIdentity,
            !.liability = IF mayCommit THEN "commit-may-have-happened"
                           ELSE IF hasPending THEN "known-not-committed"
                           ELSE "codec-only",
            !.liabilityOp = IF hasPending \/ mayCommit THEN op ELSE NoOperation,
            !.resetRequired = TRUE,
            !.oldRouteFenced = FALSE,
            !.lossObserved = TRUE,
            !.lastEvent = "c-context-loss",
            !.eventIdentity = RouteEvent(
                "c-context-loss", RouteIdentity(s.epoch),
                IF hasPending \/ mayCommit THEN op ELSE NoOperation)]

LoseFContext ==
    /\ s.epoch < 1
    /\ s.phase \notin ResetPhases
    /\ s.phase \notin {"reconcile-required", "terminal-failure"}
    /\ ~s.permitPresent
    /\ s.fCodec \in {"available", "leased", "poisoned"}
    /\ s.fContextOwner # "worker"
    /\ s.cContextOwner # "worker"
    /\ LET hasPending == s.pendingOp # NoOperation
           op == IF hasPending THEN s.pendingOp ELSE s.lastCommitOp
           mayCommit == s.phase = "f-committed" /\ s.commitOwed
       IN s' = [s EXCEPT
            !.phase = "reset-required",
            !.cCodec = IF s.cCodec = "leased" THEN "lost" ELSE @,
            !.cCodecCopies = IF s.cCodec = "leased" THEN 0 ELSE @,
            !.cLeaseState = IF s.cCodec = "leased" THEN "destroyed" ELSE @,
            !.cContextOwner = IF s.cCodec = "leased" THEN "none" ELSE @,
            !.cBodyReady = IF s.cCodec = "leased" THEN FALSE ELSE @,
            !.fCodec = "lost",
            !.fCodecCopies = 0,
            !.fLeaseState = IF s.fCodecJobId # NoCodecJobId
                                THEN "destroyed" ELSE "none",
            !.fContextOwner = "none",
            !.retiredCodecJobs = @
                \cup JobSet(s.cCodecJobId) \cup JobSet(s.fCodecJobId),
            !.preparedPresent = FALSE,
            !.preparedOp = NoOperation,
            !.preparedId = NoIdentity,
            !.liability = IF mayCommit THEN "commit-may-have-happened"
                           ELSE IF hasPending THEN "known-not-committed"
                           ELSE "codec-only",
            !.liabilityOp = IF hasPending \/ mayCommit THEN op ELSE NoOperation,
            !.resetRequired = TRUE,
            !.oldRouteFenced = FALSE,
            !.lossObserved = TRUE,
            !.lastEvent = "f-context-loss",
            !.eventIdentity = RouteEvent(
                "f-context-loss", RouteIdentity(s.epoch),
                IF hasPending \/ mayCommit THEN op ELSE NoOperation)]

DropLiabilityAfterLoss ==
    /\ MutantLostLiability
    /\ s.phase = "reset-required"
    /\ s.lossObserved
    /\ s.liability # "none"
    /\ s' = [s EXCEPT
        !.phase = "aligned",
        !.liability = "none",
        !.liabilityOp = NoOperation,
        !.lastCommitPresent = FALSE,
        !.commitOwed = FALSE,
        !.resetRequired = FALSE,
        !.oldEpochOp = s.liabilityOp,
        !.lastEvent = "drop-liability",
        !.eventIdentity = RouteEvent(
            "drop-liability", RouteIdentity(s.epoch), s.liabilityOp)]

OldRouteStartAfterLoss ==
    /\ MutantOldRouteStart
    /\ s.phase = "reset-required"
    /\ s.lossObserved
    /\ LET body == Encode(s.cCommittedTag, TUAt(s.nextIndex))
           wrong == WrongOperationIdentity(
                       OperationIdentity(s.epoch, s.nextIndex,
                                         TUAt(s.nextIndex), s.cLogical,
                                         s.cDigest, body))
       IN s' = [s EXCEPT
            !.phase = "receiving",
            !.pendingOp = wrong,
            !.pendingTU = TUAt(s.nextIndex),
            !.retainedBody = body,
            !.lastEvent = "old-route-start",
            !.eventIdentity = OperationEvent("old-route-start", wrong)]

ClassifyFault ==
    /\ s.phase = "reset-required"
    /\ s.resetRequired
    /\ s.liability \in {"codec-only", "known-not-committed",
                         "commit-may-have-happened"}
    /\ IF s.liability = "commit-may-have-happened"
          THEN /\ s.lastCommitPresent
               /\ s.lastCommitOp = s.liabilityOp
               /\ s.lastCommitBody = s.retainedBody
               /\ s.lastCommitRaw = s.materializedRaw
               /\ s' = [s EXCEPT
                    !.phase = IF s.epoch = 1
                                  THEN "terminal-failure"
                                  ELSE "reset-classified",
                    !.liability = "none",
                    !.oldRouteFenced = TRUE,
                    !.settlementClass = "committed",
                    !.settledOps = IF s.pendingOp \in SettledSet(s)
                                      THEN @ ELSE Append(@, s.pendingOp),
                    !.nextIndex = IF s.pendingOp \in SettledSet(s)
                                      THEN @ ELSE @ + 1,
                    !.settlementHighWater = IF s.pendingOp \in SettledSet(s)
                                               THEN @ ELSE s.nextIndex + 1,
                    !.faultClosure = IF s.epoch = 1
                                        THEN "terminal-failure" ELSE @,
                    !.cLogical = s.fLogical,
                    !.cCommittedTag = s.fCommittedTag,
                    !.cDigest = s.fDigest,
                    !.commitOwed = FALSE,
                    !.terminalReady = FALSE,
                    !.ackLost = FALSE,
                    !.oldEpochOp = s.pendingOp,
                    !.pendingOp = NoOperation,
                    !.pendingTU = NoTU,
                    !.cEncodeCount = 0,
                    !.preparedPresent = FALSE,
                    !.preparedOp = NoOperation,
                    !.preparedId = NoIdentity,
                    !.permitPresent = FALSE,
                    !.permitOp = NoOperation,
                    !.permitPreparedId = NoIdentity,
                    !.permitId = NoIdentity,
                    !.permitConsumed = FALSE,
                    !.lastEvent = "classify-committed",
                    !.eventIdentity = RouteEvent(
                        "classify-committed", RouteIdentity(s.epoch),
                        s.liabilityOp)]
          ELSE s' = [s EXCEPT
                    !.phase = IF s.epoch = 1
                                  THEN "terminal-failure"
                                  ELSE "reset-classified",
                    !.liability = "none",
                    !.oldRouteFenced = TRUE,
                    !.settlementClass = IF s.liability = "codec-only"
                                           THEN "none" ELSE "not-committed",
                    !.faultClosure = IF s.epoch = 1
                                        THEN "terminal-failure" ELSE @,
                    !.oldEpochOp = IF s.liabilityOp # NoOperation
                                      THEN s.liabilityOp ELSE s.lastCommitOp,
                    !.lastEvent = "classify-not-committed",
                    !.eventIdentity = RouteEvent(
                        "classify-not-committed", RouteIdentity(s.epoch),
                        IF s.liabilityOp # NoOperation
                           THEN s.liabilityOp ELSE s.lastCommitOp)]

EnterReconcileRequired ==
    /\ s.phase = "reset-required"
    /\ s.liability = "commit-may-have-happened"
    /\ (~s.lastCommitPresent \/ s.lastCommitOp # s.liabilityOp)
    /\ IF MutantFaultNeverCloses
          THEN s' = [s EXCEPT
                    !.progressPulse = 1 - @,
                    !.lastEvent = "reconcile-stall",
                    !.eventIdentity = RouteEvent(
                        "reconcile-stall", RouteIdentity(s.epoch),
                        s.liabilityOp)]
          ELSE s' = [s EXCEPT
                    !.phase = "reconcile-required",
                    !.settlementClass = "reconcile-required",
                    !.faultClosure = "reconcile-required",
                    !.lastEvent = "reconcile-required",
                    !.eventIdentity = RouteEvent(
                        "reconcile-required", RouteIdentity(s.epoch),
                        s.liabilityOp)]

ResetOffer ==
    /\ (s.phase = "reset-classified" /\ s.liability = "none"
            /\ s.oldRouteFenced)
       \/ (MutantResetBeforeClassification /\ s.phase = "reset-required")
    /\ s.epoch < 1
    /\ s.outstandingCodecJobs = {}
    /\ LET resetObs == IF MutantResetIdentity
                          THEN WrongResetIdentity(s.epoch)
                          ELSE ResetIdentity(s.epoch)
       IN s' = [s EXCEPT
            !.phase = "reset-offered",
            !.resetIdentity = resetObs,
            !.lastEvent = "reset-offer",
            !.eventIdentity = ResetEvent("reset-offer", resetObs)]

ResetAck ==
    /\ s.phase = "reset-offered"
    /\ s.resetIdentity = ResetIdentity(s.epoch)
    /\ s' = [s EXCEPT
        !.phase = "reset-acked",
        !.lastEvent = "reset-ack",
        !.eventIdentity = ResetEvent("reset-ack", s.resetIdentity)]

ResetCommit ==
    /\ s.phase = "reset-acked"
    /\ s.resetIdentity = ResetIdentity(s.epoch)
    /\ s.liability = "none"
    /\ s.oldRouteFenced
    /\ s.epoch < 1
    /\ LET newEpoch == s.epoch + 1
           erasedCount == IF s.nextIndex > 0 THEN s.nextIndex - 1 ELSE 0
       IN s' = [s EXCEPT
            !.phase = "aligned",
            !.epoch = newEpoch,
            !.nextIndex = IF MutantResetErasesSettlement THEN erasedCount ELSE @,
            !.cLogical = 0,
            !.fLogical = 0,
            !.cCommittedTag = 0,
            !.fCommittedTag = 0,
            !.cTag = 0,
            !.fTag = IF MutantWrongPostReset THEN 1 ELSE 0,
            !.cHighWater = 0,
            !.cEncodeCount = 0,
            !.cDigest = StateDigest(RouteIdentity(newEpoch), 0, 0),
            !.fDigest = IF MutantWrongPostReset
                           THEN <<"wrong-reset-digest">>
                           ELSE StateDigest(RouteIdentity(newEpoch), 0, 0),
            !.cCodec = "available",
            !.fCodec = "available",
            !.cCodecCopies = 1,
            !.fCodecCopies = 1,
            !.cLeaseState = "none",
            !.fLeaseState = "none",
            !.cContextOwner = "route",
            !.fContextOwner = "route",
            !.cContextGeneration = ContextGeneration(newEpoch),
            !.fContextGeneration = ContextGeneration(newEpoch),
            !.cCodecJobId = NoCodecJobId,
            !.fCodecJobId = NoCodecJobId,
            !.cLeaseOp = NoOperation,
            !.fLeaseOp = NoOperation,
            !.cBodyReady = FALSE,
            !.retiredCodecJobs = @
                \cup JobSet(s.cCodecJobId) \cup JobSet(s.fCodecJobId),
            !.pendingOp = NoOperation,
            !.pendingTU = NoTU,
            !.preCursor = 0,
            !.preTag = 0,
            !.preDigest = StateDigest(RouteIdentity(newEpoch), 0, 0),
            !.postTag = 0,
            !.retainedBody = <<>>,
            !.replayBody = <<>>,
            !.wireFragment = <<>>,
            !.fBuffer = <<>>,
            !.writerFragment = 0,
            !.readerFragments = 0,
            !.writerTicks = 0,
            !.readerTicks = 0,
            !.terminalTicks = 0,
            !.decoderAttempts = 0,
            !.decoderTouched = FALSE,
            !.materializedRaw = <<>>,
            !.inputPublished = FALSE,
            !.preparedPresent = FALSE,
            !.preparedOp = NoOperation,
            !.preparedId = NoIdentity,
            !.permitPresent = FALSE,
            !.permitOp = NoOperation,
            !.permitPreparedId = NoIdentity,
            !.permitId = NoIdentity,
            !.permitConsumed = FALSE,
            !.lastCommitPresent = FALSE,
            !.lastCommitOp = NoOperation,
            !.lastCommitBody = <<>>,
            !.lastCommitRaw = <<>>,
            !.lastCommitSuccessorDigest = NoIdentity,
            !.lastCommitSuccessorTag = 0,
            !.lastCommitPreparedId = NoIdentity,
            !.lastCommitPermitId = NoIdentity,
            !.lastCommitContextGeneration = 0,
            !.lastCommitCodecJobId = NoCodecJobId,
            !.commitOwed = FALSE,
            !.terminalReady = FALSE,
            !.ackLost = FALSE,
            !.lastFIdentity = NoOperation,
            !.lastCIdentity = NoOperation,
            !.fTerminalCount = 0,
            !.cTerminalCount = 0,
            !.duplicateObserved = FALSE,
            !.liability = "none",
            !.liabilityOp = NoOperation,
            !.resetRequired = FALSE,
            !.oldRouteFenced = FALSE,
            !.resetIdentity = NoResetIdentity,
            !.lossObserved = FALSE,
            !.touchedFailurePending = FALSE,
            !.preTouchDisposition = "none",
            !.faultClosure = "reset-complete",
            !.retiredEpoch0 = TRUE,
            !.resourceBeforeRelease = PayloadResource(s),
            !.lastEvent = "reset-commit",
            !.eventIdentity = ResetEvent("reset-commit", s.resetIdentity)]

OverflowResources ==
    /\ MutantResourceOverflow
    /\ s.phase # "terminal-failure"
    /\ s' = [s EXCEPT
        !.resourceLeak = PerRouteCap + 1,
        !.lastEvent = "resource-overflow",
        !.eventIdentity = RouteEvent(
            "resource-overflow", RouteIdentity(s.epoch), NoOperation)]

OverflowActiveRoutes ==
    /\ MutantActiveRoutesOverflow
    /\ s.activeRoutes <= MaxActiveRoutes
    /\ s' = [s EXCEPT
        !.activeRoutes = MaxActiveRoutes + 1,
        !.lastEvent = "active-routes-overflow",
        !.eventIdentity = RouteEvent(
            "active-routes-overflow", RouteIdentity(s.epoch), NoOperation)]

TerminalFailureStutter ==
    /\ s.phase = "terminal-failure"
    /\ s.epoch = 1
    /\ s.resetRequired
    /\ s.oldRouteFenced
    /\ s.faultClosure = "terminal-failure"
    /\ UNCHANGED s

Next ==
    \/ StartTU
    \/ CCodecMayHaveTouched
    \/ CCodecReturnedPrepared
    \/ CancelCCodecInFlight
    \/ CCodecReturnAfterCancel
    \/ ReplaceCIncarnationAfterStall
    \/ DropCCodecJobAndReuse
    \/ CWriterTick
    \/ FReaderTick
    \/ ShortDisconnect
    \/ CompleteBodyValidation
    \/ PreTouchRefusal
    \/ RetrySameBodyAfterPreTouchTerminal
    \/ EscalatePreTouchTerminalToReset
    \/ FeedDecoderBeforeComplete
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ FDecodeSuccess
    \/ FDecodeFailure
    \/ CancelFCodecInFlight
    \/ FCodecReturnAfterCancel
    \/ ReplaceFIncarnationAfterStall
    \/ RejectStaleCodecReturn("C")
    \/ RejectStaleCodecReturn("F")
    \/ InstallStaleCodecReturn
    \/ DropFCodecJobAndReuse
    \/ InstallFContextAfterCancel
    \/ DuplicateFCodecLease
    \/ SecondDecode
    \/ Materialize
    \/ AcceptCommitPermit
    \/ OwnerCancelAfterTouch
    \/ FCommit
    \/ TerminalWriterTick
    \/ LoseFinalObservation
    \/ RetryLastCommit
    \/ CObserveCommit
    \/ DuplicateTerminalObservation
    \/ EarlyCObserve
    \/ ResendBodyAfterCommit
    \/ LoseCContext
    \/ LoseFContext
    \/ DropLiabilityAfterLoss
    \/ OldRouteStartAfterLoss
    \/ ClassifyFault
    \/ EnterReconcileRequired
    \/ ResetOffer
    \/ ResetAck
    \/ ResetCommit
    \/ TerminalFailureStutter
    \/ OverflowResources
    \/ OverflowActiveRoutes

\* The composition module may advance transport/codec state without changing
\* its FInput owner state.  Owner arbitration, durable commit, context loss,
\* and reset completion are excluded: each can invalidate or retire owner-
\* retained state and must be synchronized by the composition module.
TransportNext ==
    \/ StartTU
    \/ CCodecMayHaveTouched
    \/ CCodecReturnedPrepared
    \/ CCodecReturnAfterCancel
    \/ ReplaceCIncarnationAfterStall
    \/ CWriterTick
    \/ FReaderTick
    \/ ShortDisconnect
    \/ CompleteBodyValidation
    \/ PreTouchRefusal
    \/ RetrySameBodyAfterPreTouchTerminal
    \/ EscalatePreTouchTerminalToReset
    \/ FeedDecoderBeforeComplete
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ FDecodeSuccess
    \/ FDecodeFailure
    \/ FCodecReturnAfterCancel
    \/ ReplaceFIncarnationAfterStall
    \/ RejectStaleCodecReturn("C")
    \/ RejectStaleCodecReturn("F")
    \/ InstallStaleCodecReturn
    \/ SecondDecode
    \/ Materialize
    \/ TerminalWriterTick
    \/ LoseFinalObservation
    \/ RetryLastCommit
    \/ CObserveCommit
    \/ DuplicateTerminalObservation
    \/ EarlyCObserve
    \/ ResendBodyAfterCommit
    \/ DropLiabilityAfterLoss
    \/ OldRouteStartAfterLoss
    \/ ClassifyFault
    \/ EnterReconcileRequired
    \/ ResetOffer
    \/ ResetAck
    \/ OverflowResources
    \/ OverflowActiveRoutes

Spec == Init /\ [][Next]_vars

PositiveStart == StartTU /\ s.nextIndex < 2 /\ s.epoch = 0

PositiveNext ==
    \/ PositiveStart
    \/ CCodecMayHaveTouched
    \/ CCodecReturnedPrepared
    \/ CWriterTick
    \/ FReaderTick
    \/ CompleteBodyValidation
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ FDecodeSuccess
    \/ Materialize
    \/ AcceptCommitPermit
    \/ FCommit
    \/ TerminalWriterTick
    \/ CObserveCommit

PositiveSpec ==
    Init /\ [][PositiveNext]_vars
        /\ WF_vars(PositiveStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(CCodecReturnedPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(FDecodeSuccess)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)

WitnessStart ==
    StartTU
    /\ ((s.epoch = 0 /\ s.nextIndex <= 2)
        \/ (s.epoch = 1 /\ s.nextIndex = 2))
WitnessDecodeSuccess ==
    FDecodeSuccess /\ ~(s.epoch = 0 /\ s.nextIndex = 2)
WitnessDecodeFailure == FDecodeFailure /\ s.epoch = 0 /\ s.nextIndex = 2

WitnessNext ==
    \/ WitnessStart
    \/ CCodecMayHaveTouched
    \/ CCodecReturnedPrepared
    \/ CWriterTick
    \/ FReaderTick
    \/ CompleteBodyValidation
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ WitnessDecodeSuccess
    \/ WitnessDecodeFailure
    \/ Materialize
    \/ AcceptCommitPermit
    \/ FCommit
    \/ TerminalWriterTick
    \/ CObserveCommit
    \/ ClassifyFault
    \/ ResetOffer
    \/ ResetAck
    \/ ResetCommit

WitnessSpec ==
    Init /\ [][WitnessNext]_vars
        /\ WF_vars(WitnessStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(CCodecReturnedPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(WitnessDecodeSuccess)
        /\ WF_vars(WitnessDecodeFailure)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)
        /\ WF_vars(ClassifyFault)
        /\ WF_vars(ResetOffer)
        /\ WF_vars(ResetAck)
        /\ WF_vars(ResetCommit)

AmbiguousNext == EnterReconcileRequired
AmbiguousSpec ==
    AmbiguousInit /\ [][AmbiguousNext]_vars
        /\ WF_vars(EnterReconcileRequired)

RetryStart == StartTU /\ s.epoch = 0 /\ s.nextIndex = 0
RetryRefusal ==
    PreTouchRefusal /\ s.phase = "validated" /\ s.replays = 0
RetryDecodeSuccess == FDecodeSuccess /\ s.replays = 1

RetryNext ==
    \/ RetryStart
    \/ CCodecMayHaveTouched
    \/ CCodecReturnedPrepared
    \/ CWriterTick
    \/ FReaderTick
    \/ CompleteBodyValidation
    \/ RetryRefusal
    \/ RetrySameBodyAfterPreTouchTerminal
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ RetryDecodeSuccess
    \/ Materialize
    \/ AcceptCommitPermit
    \/ FCommit
    \/ TerminalWriterTick
    \/ CObserveCommit

RetrySpec ==
    Init /\ [][RetryNext]_vars
        /\ WF_vars(RetryStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(CCodecReturnedPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(RetryRefusal)
        /\ WF_vars(RetrySameBodyAfterPreTouchTerminal)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(RetryDecodeSuccess)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)

LeaseWitnessStart ==
    StartTU /\ s.epoch = 0 /\ s.nextIndex <= 1
LeaseWitnessFReturnPrepared ==
    FCodecReturnedPrepared /\ s.nextIndex = 0
LeaseWitnessFCancel ==
    CancelFCodecInFlight
        /\ s.epoch = 0 /\ s.nextIndex = 1
        /\ s.phase = "f-codec-running"
LeaseWitnessFReturnAfterCancel ==
    FCodecReturnAfterCancel /\ s.nextIndex = 1
LeaseWitnessFReplaceAfterCancel ==
    ReplaceFIncarnationAfterStall /\ s.nextIndex = 1

FLeaseReturnNext ==
    \/ LeaseWitnessStart
    \/ CCodecMayHaveTouched
    \/ CCodecReturnedPrepared
    \/ CWriterTick
    \/ FReaderTick
    \/ CompleteBodyValidation
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ LeaseWitnessFReturnPrepared
    \/ Materialize
    \/ AcceptCommitPermit
    \/ FCommit
    \/ TerminalWriterTick
    \/ CObserveCommit
    \/ LeaseWitnessFCancel
    \/ LeaseWitnessFReturnAfterCancel
    \/ ClassifyFault
    \/ ResetOffer
    \/ ResetAck
    \/ ResetCommit

FLeaseReturnSpec ==
    Init /\ [][FLeaseReturnNext]_vars
        /\ WF_vars(LeaseWitnessStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(CCodecReturnedPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(LeaseWitnessFReturnPrepared)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)
        /\ WF_vars(LeaseWitnessFCancel)
        /\ WF_vars(LeaseWitnessFReturnAfterCancel)
        /\ WF_vars(ClassifyFault)
        /\ WF_vars(ResetOffer)
        /\ WF_vars(ResetAck)
        /\ WF_vars(ResetCommit)

FLeaseReplaceNext ==
    (FLeaseReturnNext /\ ~FCodecReturnAfterCancel)
    \/ LeaseWitnessFReplaceAfterCancel

FLeaseReplaceSpec ==
    Init /\ [][FLeaseReplaceNext]_vars
        /\ WF_vars(LeaseWitnessStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(CCodecReturnedPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(LeaseWitnessFReturnPrepared)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)
        /\ WF_vars(LeaseWitnessFCancel)
        /\ WF_vars(LeaseWitnessFReplaceAfterCancel)
        /\ WF_vars(ClassifyFault)
        /\ WF_vars(ResetOffer)
        /\ WF_vars(ResetAck)
        /\ WF_vars(ResetCommit)

LeaseWitnessCCancel ==
    CancelCCodecInFlight
        /\ s.epoch = 0 /\ s.nextIndex = 1
        /\ s.phase = "c-codec-running"
LeaseWitnessCReturnAfterCancel ==
    CCodecReturnAfterCancel /\ s.nextIndex = 1
LeaseWitnessCReturnPrepared ==
    CCodecReturnedPrepared /\ s.nextIndex = 0

CLeaseReturnNext ==
    \/ LeaseWitnessStart
    \/ CCodecMayHaveTouched
    \/ LeaseWitnessCReturnPrepared
    \/ CWriterTick
    \/ FReaderTick
    \/ CompleteBodyValidation
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ LeaseWitnessFReturnPrepared
    \/ Materialize
    \/ AcceptCommitPermit
    \/ FCommit
    \/ TerminalWriterTick
    \/ CObserveCommit
    \/ LeaseWitnessCCancel
    \/ LeaseWitnessCReturnAfterCancel
    \/ ClassifyFault
    \/ ResetOffer
    \/ ResetAck
    \/ ResetCommit

CLeaseReturnSpec ==
    Init /\ [][CLeaseReturnNext]_vars
        /\ WF_vars(LeaseWitnessStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(LeaseWitnessCReturnPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(LeaseWitnessFReturnPrepared)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)
        /\ WF_vars(LeaseWitnessCCancel)
        /\ WF_vars(LeaseWitnessCReturnAfterCancel)
        /\ WF_vars(ClassifyFault)
        /\ WF_vars(ResetOffer)
        /\ WF_vars(ResetAck)
        /\ WF_vars(ResetCommit)

StaleWitnessStart ==
    StartTU
        /\ ((s.epoch = 0 /\ s.nextIndex <= 1)
             \/ (s.epoch = 1 /\ s.nextIndex = 1))
StaleWitnessCReturnPrepared ==
    CCodecReturnedPrepared
        /\ (s.epoch = 0
             \/ StaleCodecJob("C", s)
                    \in s.staleCodecReturnsRejected)
StaleWitnessFReturnPrepared ==
    FCodecReturnedPrepared
        /\ ((s.epoch = 0 /\ s.nextIndex = 0)
             \/ (s.epoch = 1 /\ s.nextIndex = 1
                   /\ StaleCodecJob("F", s)
                          \in s.staleCodecReturnsRejected))
StaleWitnessFCancelA ==
    CancelFCodecInFlight
        /\ s.epoch = 0 /\ s.nextIndex = 1
        /\ s.phase = "f-codec-running"
StaleWitnessFReplaceA ==
    ReplaceFIncarnationAfterStall
        /\ s.epoch = 0 /\ s.nextIndex = 1
StaleWitnessRejectC ==
    RejectStaleCodecReturn("C")
        /\ s.epoch = 1 /\ s.nextIndex = 1
        /\ s.phase = "c-codec-running"
StaleWitnessRejectF ==
    RejectStaleCodecReturn("F")
        /\ s.epoch = 1 /\ s.nextIndex = 1
        /\ s.phase = "f-codec-running"

StaleReturnWitnessNext ==
    \/ StaleWitnessStart
    \/ CCodecMayHaveTouched
    \/ StaleWitnessRejectC
    \/ StaleWitnessCReturnPrepared
    \/ CWriterTick
    \/ FReaderTick
    \/ CompleteBodyValidation
    \/ IssueFCodecLease
    \/ FCodecMayHaveTouched
    \/ StaleWitnessFReturnPrepared
    \/ Materialize
    \/ AcceptCommitPermit
    \/ FCommit
    \/ TerminalWriterTick
    \/ CObserveCommit
    \/ StaleWitnessFCancelA
    \/ StaleWitnessFReplaceA
    \/ ClassifyFault
    \/ ResetOffer
    \/ ResetAck
    \/ ResetCommit
    \/ StaleWitnessRejectF

StaleReturnWitnessSpec ==
    Init /\ [][StaleReturnWitnessNext]_vars
        /\ WF_vars(StaleWitnessStart)
        /\ WF_vars(CCodecMayHaveTouched)
        /\ WF_vars(StaleWitnessRejectC)
        /\ WF_vars(StaleWitnessCReturnPrepared)
        /\ WF_vars(CWriterTick)
        /\ WF_vars(FReaderTick)
        /\ WF_vars(CompleteBodyValidation)
        /\ WF_vars(IssueFCodecLease)
        /\ WF_vars(FCodecMayHaveTouched)
        /\ WF_vars(StaleWitnessFReturnPrepared)
        /\ WF_vars(Materialize)
        /\ WF_vars(AcceptCommitPermit)
        /\ WF_vars(FCommit)
        /\ WF_vars(TerminalWriterTick)
        /\ WF_vars(CObserveCommit)
        /\ WF_vars(StaleWitnessFCancelA)
        /\ WF_vars(StaleWitnessFReplaceA)
        /\ WF_vars(ClassifyFault)
        /\ WF_vars(ResetOffer)
        /\ WF_vars(ResetAck)
        /\ WF_vars(ResetCommit)
        /\ WF_vars(StaleWitnessRejectF)

StaleWitnessInstallF ==
    InstallStaleCodecReturn
        /\ s.epoch = 1 /\ s.nextIndex = 1
        /\ s.phase = "f-codec-running"
StaleInstallMutantNext ==
    (StaleReturnWitnessNext /\ ~StaleWitnessRejectF)
    \/ StaleWitnessInstallF
StaleInstallMutantSpec ==
    Init /\ [][StaleInstallMutantNext]_vars

FLeaseCancelReached ==
    <> (s.lastEvent = "f-codec-drain-required"
            /\ s.pendingOp.tuSeq = 1)
FLeaseReturnDestroyedReached ==
    <> (s.lastEvent = "f-codec-returned-after-cancel"
            /\ s.fLeaseState = "destroyed")
FLeaseReplacementReached ==
    <> (s.lastEvent = "f-codec-incarnation-replaced"
            /\ s.fLeaseState = "destroyed")
CLeaseCancelReached ==
    <> (s.lastEvent = "c-codec-drain-required"
            /\ s.pendingOp.tuSeq = 1)
CLeaseReturnDestroyedReached ==
    <> (s.lastEvent = "c-codec-returned-after-cancel"
            /\ s.cLeaseState = "destroyed")
LeaseFreshEpochRecovery ==
    <> (s.epoch = 1 /\ s.nextIndex = 1 /\ s.phase = "aligned"
            /\ s.outstandingCodecJobs = {})
StaleCCodecReturnRejectedReached ==
    <> (StaleCodecJob("C", s) \in s.staleCodecReturnsRejected)
StaleFCodecReturnRejectedReached ==
    <> (StaleCodecJob("F", s) \in s.staleCodecReturnsRejected)
StaleCurrentBCommittedReached ==
    <> (s.epoch = 1 /\ s.nextIndex = 2 /\ s.phase = "aligned"
            /\ Cardinality(s.staleCodecReturnsRejected) = 2)

LiveOperationEvents == {
    "c-codec-lease-issued", "c-codec-may-touch",
    "c-codec-returned-prepared", "c-codec-drain-required",
    "c-codec-returned-after-cancel", "c-codec-incarnation-replaced",
    "f-codec-lease-issued", "f-codec-may-touch",
    "f-codec-returned-prepared", "f-codec-drain-required",
    "f-codec-returned-after-cancel", "f-codec-incarnation-replaced",
    "drop-f-codec-job-and-reuse", "install-f-context-after-cancel",
    "duplicate-f-codec-lease", "drop-c-codec-job-and-reuse",
    "c-encode", "writer-stall", "c-writer-tick", "reader-stall",
    "f-reader-tick", "short-disconnect", "complete-body-validation",
    "pre-touch-refusal", "retry-same-body", "pre-touch-reset",
    "feed-before-complete", "f-decode",
    "second-decode", "decode-failure", "materialize", "commit-permit",
    "owner-cancel-after-touch",
    "f-atomic-commit", "terminal-stall", "terminal-writer-tick",
    "lost-final-observation", "retry-last-commit",
    "resend-after-commit", "old-route-start"
}

PostCommitEvents == {"c-observe-commit", "duplicate-terminal"}
StaleCodecReturnEvents == {
    "stale-codec-return-rejected", "stale-codec-return-installed"
}
ContextLossEvents == {"c-context-loss", "f-context-loss"}
ClassificationEvents == {"classify-committed", "classify-not-committed"}
ReconcileEvents == {"reconcile-stall", "reconcile-required"}
RouteOnlyEvents == {"resource-overflow", "active-routes-overflow"}

TypeOK ==
    /\ s.phase \in Phases
    /\ s.epoch \in 0..1
    /\ s.nextIndex \in 0..3
    /\ s.cLogical \in 0..3 /\ s.fLogical \in 0..3
    /\ s.cCommittedTag \in 0..3 /\ s.fCommittedTag \in 0..3
    /\ s.cTag \in 0..3 /\ s.fTag \in 0..3 /\ s.cHighWater \in 0..3
    /\ s.cEncodeCount \in 0..2
    /\ s.cCodec \in CodecStates /\ s.fCodec \in CodecStates
    /\ s.cCodecCopies \in 0..2 /\ s.fCodecCopies \in 0..2
    /\ s.cLeaseState \in CodecLeaseStates
    /\ s.fLeaseState \in CodecLeaseStates
    /\ s.cContextOwner \in ContextOwners
    /\ s.fContextOwner \in ContextOwners
    /\ s.cContextGeneration \in 1..2
    /\ s.fContextGeneration \in 1..2
    /\ (s.cCodecJobId = NoCodecJobId
            \/ CodecJobIdShape(s.cCodecJobId))
    /\ (s.fCodecJobId = NoCodecJobId
            \/ CodecJobIdShape(s.fCodecJobId))
    /\ OperationOrNoneShape(s.cLeaseOp)
    /\ OperationOrNoneShape(s.fLeaseOp)
    /\ s.cBodyReady \in BOOLEAN
    /\ CodecJobSetShape(s.retiredCodecJobs)
    /\ CodecJobSetShape(s.installedCodecJobs)
    /\ CodecJobSetShape(s.outstandingCodecJobs)
    /\ CodecJobSetShape(s.staleCodecReturnsRejected)
    /\ s.activeRoutes \in 0..(MaxActiveRoutes + 1)
    /\ OperationOrNoneShape(s.pendingOp)
    /\ s.pendingTU \in TUs \cup {NoTU}
    /\ s.preCursor \in 0..3 /\ s.preTag \in 0..3 /\ s.postTag \in 0..3
    /\ s.retainedBody \in ByteSequences /\ s.replayBody \in ByteSequences
    /\ s.wireFragment \in ByteSequences /\ s.fBuffer \in ByteSequences
    /\ s.writerFragment \in 0..MaxFragments
    /\ s.readerFragments \in 0..MaxFragments
    /\ s.writerTicks \in 0..MaxWriterTicks
    /\ s.readerTicks \in 0..MaxReaderTicks
    /\ s.terminalTicks \in 0..MaxTerminalTicks
    /\ s.progressPulse \in 0..1 /\ s.replays \in 0..1
    /\ s.bodySends \in Nat /\ s.sendsAtFCommit \in Nat
    /\ s.decoderAttempts \in 0..2 /\ s.decoderTouched \in BOOLEAN
    /\ s.materializedRaw \in ByteSequences
    /\ s.inputPublished \in BOOLEAN
    /\ s.preparedPresent \in BOOLEAN
    /\ OperationOrNoneShape(s.preparedOp)
    /\ (s.preparedId = NoIdentity \/ PreparedIdShape(s.preparedId))
    /\ s.permitPresent \in BOOLEAN
    /\ OperationOrNoneShape(s.permitOp)
    /\ (s.permitPreparedId = NoIdentity
            \/ PreparedIdShape(s.permitPreparedId))
    /\ (s.permitId = NoIdentity \/ PermitIdShape(s.permitId))
    /\ s.permitConsumed \in BOOLEAN
    /\ PermitSetShape(s.issuedPermits)
    /\ PermitSetShape(s.consumedPermits)
    /\ OperationSetShape(s.publishedOps)
    /\ OperationSequenceShape(s.settledOps)
    /\ s.settlementHighWater \in 0..3
    /\ s.lastCommitPresent \in BOOLEAN
    /\ OperationOrNoneShape(s.lastCommitOp)
    /\ OperationOrNoneShape(s.previousCommitOp)
    /\ s.lastCommitBody \in ByteSequences /\ s.lastCommitRaw \in ByteSequences
    /\ s.lastCommitSuccessorTag \in 0..3
    /\ (s.lastCommitPreparedId = NoIdentity
            \/ PreparedIdShape(s.lastCommitPreparedId))
    /\ (s.lastCommitPermitId = NoIdentity
            \/ PermitIdShape(s.lastCommitPermitId))
    /\ s.lastCommitContextGeneration \in 0..2
    /\ (s.lastCommitCodecJobId = NoCodecJobId
            \/ CodecJobIdShape(s.lastCommitCodecJobId))
    /\ s.commitOwed \in BOOLEAN /\ s.terminalReady \in BOOLEAN
    /\ s.ackLost \in BOOLEAN /\ s.retryIssued \in BOOLEAN
    /\ OperationOrNoneShape(s.lastFIdentity)
    /\ OperationOrNoneShape(s.lastCIdentity)
    /\ s.fTerminalCount \in 0..1 /\ s.cTerminalCount \in 0..1
    /\ s.duplicateObserved \in BOOLEAN
    /\ s.liability \in Liabilities /\ OperationOrNoneShape(s.liabilityOp)
    /\ OperationOrNoneShape(s.oldEpochOp)
    /\ s.resetRequired \in BOOLEAN /\ s.oldRouteFenced \in BOOLEAN
    /\ s.resetIdentity \in ResetIdentityDomain
    /\ s.settlementClass \in SettlementClasses
    /\ s.lossObserved \in BOOLEAN /\ s.touchedFailurePending \in BOOLEAN
    /\ s.preTouchDisposition \in PreTouchDispositions
    /\ s.faultClosure \in FaultClosures /\ s.retiredEpoch0 \in BOOLEAN
    /\ s.resourceBeforeRelease \in Nat /\ s.resourceLeak \in Nat
    /\ s.eventIdentity.kind \in EventKinds
    /\ s.eventIdentity.route \in RouteIdentityDomain
    /\ OperationOrNoneShape(s.eventIdentity.operation)
    /\ s.eventIdentity.reset \in ResetIdentityDomain

DistinctOperationTUs == T0 # T1 /\ T0 # T2 /\ T1 # T2
ByteAlphabetHasTwoValues == Cardinality(Byte) >= 2

OneUsableCodecPerSide ==
    /\ s.cCodecCopies <= 1
    /\ s.fCodecCopies <= 1
    /\ (s.cCodec \in {"available", "leased"}
            <=> s.cCodecCopies = 1)
    /\ (s.fCodec \in {"available", "leased"}
            <=> s.fCodecCopies = 1)

ExclusiveContextLease ==
    /\ (s.cContextOwner = "route" <=> s.cCodec = "available")
    /\ (s.fContextOwner = "route" <=> s.fCodec = "available")
    /\ (s.cContextOwner \in {"worker", "prepared"}
            <=> s.cCodec = "leased")
    /\ (s.fContextOwner \in {"worker", "prepared"}
            <=> s.fCodec = "leased")
    /\ (s.cContextOwner = "worker" <=>
            s.cLeaseState \in {"issued", "may-have-touched",
                                "drain-required"})
    /\ (s.fContextOwner = "worker" <=>
            s.fLeaseState \in {"issued", "may-have-touched",
                                "drain-required"})
    /\ (s.cContextOwner = "worker" <=>
            s.cCodecJobId \in s.outstandingCodecJobs)
    /\ (s.fContextOwner = "worker" <=>
            s.fCodecJobId \in s.outstandingCodecJobs)
    /\ (s.cContextOwner = "prepared" <=>
            s.cLeaseState = "returned-prepared")
    /\ (s.fContextOwner = "prepared" <=>
            s.fLeaseState = "returned-prepared")
    /\ (s.cCodecJobId # NoCodecJobId =>
            /\ s.cLeaseOp \in {s.pendingOp, s.lastCommitOp, s.oldEpochOp}
            /\ s.cCodecJobId = CodecJobId(
                    "C", s.cLeaseOp, s.cContextGeneration))
    /\ (s.fCodecJobId # NoCodecJobId =>
            /\ s.fLeaseOp \in {s.pendingOp, s.lastCommitOp, s.oldEpochOp}
            /\ s.fCodecJobId = CodecJobId(
                    "F", s.fLeaseOp, s.fContextGeneration))
    /\ (s.cLeaseState = "drain-required" =>
            /\ s.phase = "c-codec-drain-required"
            /\ s.resetRequired)
    /\ (s.fLeaseState = "drain-required" =>
            /\ s.phase = "f-codec-drain-required"
            /\ s.resetRequired)
    /\ (s.cLeaseState = "destroyed" =>
            /\ s.cContextOwner = "none"
            /\ s.cCodecJobId \in s.retiredCodecJobs)
    /\ (s.fLeaseState = "destroyed" =>
            /\ s.fContextOwner = "none"
            /\ s.fCodecJobId \in s.retiredCodecJobs)
    /\ (s.fLeaseState = "installed" =>
            /\ s.fContextOwner = "route"
            /\ s.fCodecJobId \in s.installedCodecJobs)
    /\ s.cContextGeneration = ContextGeneration(s.epoch)
    /\ s.fContextGeneration = ContextGeneration(s.epoch)

OutstandingCodecJobsBlockReuse ==
    /\ Cardinality(s.outstandingCodecJobs) <= 1
    /\ (s.outstandingCodecJobs # {} =>
            /\ s.phase \in {"c-codec-leased", "c-codec-running",
                             "c-codec-drain-required",
                             "f-codec-leased", "f-codec-running",
                             "f-codec-drain-required"}
            /\ s.phase # "aligned")
    /\ (s.phase \in {"reset-classified", "reset-offered", "reset-acked"}
            => s.outstandingCodecJobs = {})

StaleCodecReturnFenced ==
    /\ s.staleCodecReturnsRejected \subseteq s.retiredCodecJobs
    /\ s.staleCodecReturnsRejected \intersect
           s.outstandingCodecJobs = {}
    /\ UnpublishedRetiredCodecJobs(s) \intersect
           s.installedCodecJobs = {}

SingleUnresolvedTU ==
    /\ (s.pendingOp = NoOperation <=> s.pendingTU = NoTU)
    /\ (s.pendingOp # NoOperation => s.pendingTU \in TUs)

PendingIdentityExact ==
    s.pendingOp # NoOperation =>
        /\ s.pendingOp = OperationIdentity(
                s.epoch, s.pendingOp.tuSeq, s.pendingTU,
                s.preCursor, s.preDigest, s.retainedBody)
        /\ s.pendingOp.route = RouteIdentity(s.epoch)
        /\ s.pendingOp.transaction = TransactionAt(s.pendingOp.tuSeq)
        /\ s.pendingOp.raw = RawIdentity(s.pendingTU)
        /\ s.pendingOp.encoded = EncodedIdentity(s.retainedBody)
        /\ s.replayBody = s.retainedBody

CNoRollbackWithinEpoch ==
    /\ (s.cCodec \in {"available", "leased"} =>
            s.cTag = s.cHighWater)
    /\ (s.pendingOp # NoOperation
            /\ s.cLeaseState = "returned-prepared" =>
            s.cTag = s.postTag /\ s.postTag = s.preTag + 1)
    /\ (s.cLeaseState \in {"issued", "may-have-touched"} =>
            s.cTag = s.preTag /\ s.cHighWater = s.preTag)
    /\ (s.phase = "aligned" /\ s.pendingOp = NoOperation =>
            s.cTag = s.cCommittedTag)

SingleCEncodePerTU ==
    /\ (s.pendingOp # NoOperation => s.cEncodeCount = 1)
    /\ (s.pendingOp = NoOperation => s.cEncodeCount = 0)

FUntouchedBeforeCompleteBody ==
    s.phase \in {"receiving", "body-complete", "validated",
                  "pre-touch-terminal"} =>
        /\ s.fCodec = "available"
        /\ s.fTag = s.preTag
        /\ s.fCommittedTag = s.preTag
        /\ s.decoderAttempts = 0
        /\ ~s.decoderTouched

ValidatedBodyExact ==
    s.phase \in {"validated", "decoded", "materialized", "f-committed"} =>
        s.fBuffer = s.retainedBody

HistorySensitiveEncoding ==
    /\ Encode(0, T1) # Encode(1, T1)
    /\ (s.pendingOp # NoOperation =>
            s.retainedBody = Encode(s.preTag, s.pendingTU))
    /\ (s.pendingTU = T1 /\ s.pendingOp.route.epoch = 0 => s.preTag = 1)

EqualLengthCorruptionIsDistinct ==
    /\ Len(CorruptSameLength(Encode(0, T0))) = Len(Encode(0, T0))
    /\ CorruptSameLength(Encode(0, T0)) # Encode(0, T0)

DecodeAtMostOnce == s.decoderAttempts <= 1

TouchedImpliesCommitOrReset ==
    s.touchedFailurePending =>
        /\ s.resetRequired
        /\ s.phase \in ResetPhases \cup DrainPhases
                        \cup {"reconcile-required", "terminal-failure"}

PoisonedOrLostCannotContinue ==
    s.fCodec \in {"poisoned", "lost"} =>
        s.phase \in ResetPhases \cup {"reconcile-required", "terminal-failure"}

MaterializedRawExact ==
    s.phase \in {"materialized", "f-committed"} =>
        s.materializedRaw = RawBytes(s.pendingTU)

PreparedInputExact ==
    /\ (s.preparedPresent <=> s.preparedOp # NoOperation)
    /\ (s.preparedPresent =>
            /\ s.pendingOp # NoOperation
            /\ s.preparedOp = s.pendingOp
            /\ s.preparedId = PreparedId(s.pendingOp)
            /\ s.decoderTouched /\ s.decoderAttempts = 1
            /\ IF s.phase = "f-committed"
                  THEN /\ s.fLeaseState = "installed"
                       /\ s.fContextOwner = "route"
                  ELSE /\ s.fLeaseState = "returned-prepared"
                       /\ s.fContextOwner = "prepared"
            /\ s.materializedRaw = RawBytes(s.pendingTU)
            /\ PreparedInput(s).operation = s.pendingOp
            /\ PreparedInput(s).preparedId = s.preparedId
            /\ PreparedInput(s).contextGeneration =
                  s.fContextGeneration
            /\ PreparedInput(s).codecJobId = s.fCodecJobId
            /\ s.fCodecJobId = CodecJobId(
                  "F", s.pendingOp, s.fContextGeneration))
    /\ (~s.preparedPresent => s.preparedId = NoIdentity)

CommitRequiresExactPermit ==
    /\ (s.permitPresent =>
            /\ s.phase = "materialized"
            /\ s.preparedPresent
            /\ ~s.permitConsumed
            /\ s.permitOp = s.pendingOp
            /\ s.permitPreparedId = s.preparedId
            /\ s.permitId = PermitId(s.pendingOp, s.preparedId)
            /\ s.permitId \in s.issuedPermits
            /\ s.permitId \notin s.consumedPermits)
    /\ (s.phase = "f-committed" =>
            /\ s.permitConsumed
            /\ ~s.permitPresent
            /\ s.lastCommitPreparedId = PreparedId(s.lastCommitOp)
            /\ s.lastCommitPermitId =
                  PermitId(s.lastCommitOp, s.lastCommitPreparedId)
            /\ s.lastCommitPermitId \in s.issuedPermits
            /\ s.lastCommitPermitId \in s.consumedPermits)

NoPermitReuse ==
    /\ s.consumedPermits \subseteq s.issuedPermits
    /\ (s.permitPresent =>
            /\ s.permitId \in s.issuedPermits
            /\ s.permitId \notin s.consumedPermits)
    /\ (s.lastCommitPermitId # NoIdentity =>
            /\ s.lastCommitPermitId \in s.issuedPermits
            /\ s.lastCommitPermitId \in s.consumedPermits)

FCommitAtomic ==
    s.phase = "f-committed" =>
        /\ s.inputPublished
        /\ s.pendingOp \in s.publishedOps
        /\ s.preparedPresent
        /\ s.preparedOp = s.pendingOp
        /\ s.fLogical = s.preCursor + 1
        /\ s.fCommittedTag = s.postTag
        /\ s.fTag = s.postTag
        /\ s.fCodec = "available"
        /\ s.fLeaseState = "installed"
        /\ s.fContextOwner = "route"
        /\ s.fCodecJobId \in s.installedCodecJobs
        /\ s.fDigest = StateDigest(RouteIdentity(s.epoch),
                                   s.fLogical, s.fCommittedTag)

LastCommitRetention ==
    s.commitOwed =>
        /\ s.lastCommitPresent
        /\ s.lastCommitOp = s.pendingOp
        /\ s.lastCommitBody = s.retainedBody
        /\ s.lastCommitRaw = s.materializedRaw
        /\ s.lastCommitSuccessorTag = s.postTag
        /\ s.lastCommitSuccessorDigest = s.fDigest
        /\ s.lastCommitPreparedId = PreparedId(s.lastCommitOp)
        /\ s.lastCommitPermitId =
              PermitId(s.lastCommitOp, s.lastCommitPreparedId)
        /\ s.lastCommitPermitId \in s.consumedPermits
        /\ s.lastCommitContextGeneration = s.fContextGeneration
        /\ s.lastCommitCodecJobId = CodecJobId(
              "F", s.lastCommitOp, s.lastCommitContextGeneration)
        /\ s.phase \in {"f-committed"} \cup ResetPhases

TerminalIdentityExact ==
    /\ (s.fTerminalCount > 0 => s.lastFIdentity = s.lastCommitOp)
    /\ (s.cTerminalCount > 0 => s.lastCIdentity = s.lastCommitOp)
    /\ (s.fTerminalCount > 0 /\ s.pendingOp # NoOperation =>
            /\ s.lastCommitOp = s.pendingOp
            /\ s.lastFIdentity = s.pendingOp)
    /\ (s.retryIssued => s.lastFIdentity = s.lastCommitOp)
    /\ (s.retryIssued /\ s.pendingOp # NoOperation =>
            s.lastCommitOp = s.pendingOp)

EventIdentityExact ==
    /\ s.eventIdentity.kind = s.lastEvent
    /\ (s.eventIdentity.operation # NoOperation =>
            s.eventIdentity.route = s.eventIdentity.operation.route)
    /\ (s.lastEvent = "init" => s.eventIdentity = NoEvent)
    /\ (s.lastEvent = "ambiguous-f-loss" =>
            /\ s.eventIdentity.operation = s.pendingOp
            /\ s.eventIdentity.route = RouteIdentity(s.epoch))
    /\ (s.lastEvent \in LiveOperationEvents =>
            /\ s.pendingOp # NoOperation
            /\ s.eventIdentity.operation = s.pendingOp
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.reset = NoResetIdentity)
    /\ (s.lastEvent \in PostCommitEvents =>
            /\ s.lastCommitOp # NoOperation
            /\ s.eventIdentity.operation = s.lastCommitOp
            /\ s.eventIdentity.route = RouteIdentity(s.epoch))
    /\ (s.lastEvent = "early-c-observe" =>
            /\ Len(s.settledOps) > 0
            /\ s.eventIdentity.operation =
                  s.settledOps[Len(s.settledOps)]
            /\ s.eventIdentity.route = RouteIdentity(s.epoch))
    /\ (s.lastEvent \in StaleCodecReturnEvents =>
            /\ ProperOperationShape(s.oldEpochOp)
            /\ ProperOperationShape(s.pendingOp)
            /\ s.eventIdentity.operation = s.oldEpochOp
            /\ s.eventIdentity.route = s.oldEpochOp.route
            /\ s.eventIdentity.route # s.pendingOp.route)
    /\ (s.lastEvent \in ContextLossEvents =>
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.operation =
                  IF s.liabilityOp # NoOperation
                     THEN s.liabilityOp ELSE NoOperation)
    /\ (s.lastEvent = "drop-liability" =>
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.operation = s.oldEpochOp)
    /\ (s.lastEvent \in ClassificationEvents =>
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.operation = s.oldEpochOp)
    /\ (s.lastEvent \in ReconcileEvents =>
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.operation = s.liabilityOp)
    /\ (s.lastEvent \in {"reset-offer", "reset-ack"} =>
            /\ s.eventIdentity.reset = s.resetIdentity
            /\ s.eventIdentity.reset = ResetIdentity(s.epoch)
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.operation = NoOperation)
    /\ (s.lastEvent = "reset-commit" =>
            /\ s.epoch = 1
            /\ s.eventIdentity.reset = ResetIdentity(s.epoch - 1)
            /\ s.eventIdentity.route = RouteIdentity(s.epoch - 1)
            /\ s.eventIdentity.operation = NoOperation)
    /\ (s.lastEvent \in RouteOnlyEvents =>
            /\ s.eventIdentity.route = RouteIdentity(s.epoch)
            /\ s.eventIdentity.operation = NoOperation
            /\ s.eventIdentity.reset = NoResetIdentity)

NoBodyResendAfterCommit ==
    s.commitOwed => s.bodySends = s.sendsAtFCommit

SettlementLedgerExact ==
    /\ Len(s.settledOps) = s.nextIndex
    /\ Cardinality(SettledSet(s)) = Len(s.settledOps)
    /\ SettledSet(s) \subseteq s.publishedOps
    /\ Len(s.settledOps) >= s.settlementHighWater

LiabilityRetainedAndBound ==
    /\ (s.liability # "none" /\ s.liability # "codec-only" =>
            /\ s.liabilityOp # NoOperation
            /\ (s.liabilityOp = s.pendingOp \/ s.liabilityOp = s.lastCommitOp))
    /\ (s.lossObserved /\ ~s.oldRouteFenced =>
            s.liability # "none" \/ s.phase = "reconcile-required")

ContextLossFencesOldRoute ==
    s.lossObserved =>
        s.phase \in ResetPhases \cup DrainPhases
                    \cup {"reconcile-required", "terminal-failure"}

ResetAfterClassification ==
    s.phase \in {"reset-offered", "reset-acked"} =>
        /\ s.liability = "none"
        /\ s.oldRouteFenced
        /\ s.resetRequired

ResetIdentityBound ==
    s.phase \in {"reset-offered", "reset-acked"} =>
        s.resetIdentity = ResetIdentity(s.epoch)

PostResetExact ==
    s.lastEvent = "reset-commit" =>
        /\ s.epoch = 1 /\ s.retiredEpoch0
        /\ s.cLogical = 0 /\ s.fLogical = 0
        /\ s.cCommittedTag = 0 /\ s.fCommittedTag = 0
        /\ s.cTag = 0 /\ s.fTag = 0
        /\ s.cCodec = "available" /\ s.fCodec = "available"
        /\ s.cDigest = StateDigest(RouteIdentity(1), 0, 0)
        /\ s.fDigest = StateDigest(RouteIdentity(1), 0, 0)
        /\ ~s.resetRequired /\ s.liability = "none"

ResourceBounds ==
    /\ Len(s.retainedBody) <= MaxEncodedBytes
    /\ Len(s.wireFragment) + Len(s.fBuffer) <= MaxEncodedBytes
    /\ Len(s.materializedRaw) <= MaxRawBytes
    /\ CodecUnits * s.cCodecCopies <= MaxWindowBytes
    /\ CodecUnits * s.fCodecCopies <= MaxWindowBytes
    /\ ResourceOf(s) <= PerRouteCap
    /\ s.activeRoutes <= MaxActiveRoutes
    /\ s.activeRoutes * ResourceOf(s) <= GlobalCap

PayloadReleasedOnSettlement ==
    s.lastEvent \in {"c-observe-commit", "reset-commit"} =>
        /\ (s.resourceBeforeRelease = 0
            \/ PayloadResource(s) < s.resourceBeforeRelease)
        /\ s.retainedBody = <<>> /\ s.wireFragment = <<>>
        /\ s.fBuffer = <<>> /\ s.materializedRaw = <<>>

TwoTUExactProgress == s.nextIndex < 2 ~> s.nextIndex >= 2

PositiveOperationProgress ==
    \A op \in OperationDomain :
        (s.pendingOp = op) ~>
            (s.phase = "aligned" /\ op \in SettledSet(s))

CombinedTwoTUResetRecovery ==
    s.epoch = 0 /\ s.nextIndex = 0 ~>
        s.epoch = 1 /\ s.nextIndex = 3 /\ s.phase = "aligned"

FaultEventuallyClosed ==
    s.liability # "none" ~>
        s.faultClosure \in {"reset-complete", "reconcile-required", "terminal-failure"}

PreTouchSameBodyRetryProgress ==
    s.lastEvent = "retry-same-body" ~>
        s.nextIndex >= 1 /\ s.phase = "aligned"

=============================================================================
