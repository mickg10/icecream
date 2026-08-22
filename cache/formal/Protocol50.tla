------------------------------ MODULE Protocol50 ------------------------------
EXTENDS Naturals, FiniteSets, Sequences, TLC

(***************************************************************************
Protocol-50 cache-transaction safety core.

The model deliberately has one C-side active transaction and one F-side
pending overlay.  It covers the states that must stay exact across immutable
object publication, eviction, session replacement, replay, and the
F-durable/C-unacknowledged commit window.

Compiler/job restart begins at INPUT_COMMITTED and is modeled separately in
Protocol50JobLifecycle.tla.

Callbacks carry an abstract operation identity:
    <<F, HISTORY_NONCE, REL_SEQ, TU, TX_DIGEST_VARIANT>>
The final field is a bounded representative of the production transaction
digest, which additionally binds profile, component descriptors, raw digest,
and route pre-state.  Only one cursor/TU tuple receives a second digest variant
in this model, so the same-session abort/re-encode ABA case is explored without
doubling the complete state space.
***************************************************************************)

CONSTANTS F0, F1, T0, T1, O0, O1, V0, V1,
          Tok0, Tok1, NoF, NoTU, NoToken, NoContent,
          MaxRel,
          MutantAbortAfterCommit,
          MutantBeginAtMaxRel,
          MutantIgnoreTxDigest

ASSUME /\ MaxRel \in Nat
       /\ MaxRel > 0
       /\ MutantAbortAfterCommit \in BOOLEAN
       /\ MutantBeginAtMaxRel \in BOOLEAN
       /\ MutantIgnoreTxDigest \in BOOLEAN
       /\ F0 # F1
       /\ T0 # T1
       /\ O0 # O1
       /\ V0 # V1

Fs == {F0, F1}
TUs == {T0, T1}
Objects == {O0, O1}
Values == {V0, V1}
Tokens == {Tok0, Tok1}
Nonces == 0..1
Rels == 0..MaxRel
DigestVariants == 0..1

TuObjects(t) == IF t = T0 THEN {O0} ELSE {O0, O1}
CanonicalContent(o) == IF o = O0 THEN V0 ELSE V1

Op(f, n, r, t, d) == <<f, n, r, t, d>>
NoOp == <<NoF, 2, MaxRel + 1, NoTU, 2>>
BaseOps == Fs \X Nonces \X Rels \X TUs
PrimaryOps == {Append(op, 0) : op \in BaseOps}
RetryDigestOp == Op(F0, 1, 0, T0, 1)
RealOps == PrimaryOps \cup {RetryDigestOp}
Ops == RealOps \cup {NoOp}

OpF(op) == op[1]
OpNonce(op) == op[2]
OpRel(op) == op[3]
OpTu(op) == op[4]
OpDigestVariant(op) == op[5]

SameCursorWithoutDigest(left, right) ==
    /\ left \in RealOps
    /\ right \in RealOps
    /\ OpF(left) = OpF(right)
    /\ OpNonce(left) = OpNonce(right)
    /\ OpRel(left) = OpRel(right)
    /\ OpTu(left) = OpTu(right)
    /\ OpDigestVariant(left) # OpDigestVariant(right)

Present(st, f) == {o \in Objects : st.content[f][o] # NoContent}
CurrentSession(st, f, tok) ==
    /\ st.session = f
    /\ st.sessionToken = tok

ClearOverlay(st) ==
    [st EXCEPT
        !.pendingOp = NoOp,
        !.pendingToken = NoToken,
        !.dictDone = FALSE,
        !.needRecorded = FALSE,
        !.requested = {},
        !.missing = {},
        !.pinned = {},
        !.bodyDone = FALSE,
        !.materialized = FALSE]

VARIABLE s
vars == <<s>>

Init ==
    s = [session             |-> NoF,
         sessionToken        |-> NoToken,
         usedTokens          |-> [f \in Fs |-> {}],
         route               |-> [f \in Fs |-> FALSE],
         nonce               |-> [f \in Fs |-> 0],
         fRel                |-> [f \in Fs |-> 0],
         content             |-> [f \in Fs |->
                                     [o \in Objects |-> NoContent]],
         lastCommitOp        |-> [f \in Fs |-> NoOp],
         cF                  |-> NoF,
         cNonce              |-> 0,
         cRel                |-> 0,
         cActiveOp           |-> NoOp,
         pendingOp           |-> NoOp,
         pendingToken        |-> NoToken,
         dictDone            |-> FALSE,
         needRecorded        |-> FALSE,
         requested           |-> {},
         missing             |-> {},
         pinned              |-> {},
         bodyDone            |-> FALSE,
         materialized        |-> FALSE,
         commitUnacked       |-> FALSE,
         commitDisconnected  |-> FALSE,
         badCommit           |-> FALSE,
         staleSessionSeen    |-> FALSE,
         staleOperationSeen  |-> FALSE]

SESSION_OPENED(f, tok) ==
    LET base == ClearOverlay(s)
    IN /\ f \in Fs
       /\ tok \in Tokens \ s.usedTokens[f]
       /\ s.session = NoF
       /\ s' = [base EXCEPT
                    !.session = f,
                    !.sessionToken = tok,
                    !.usedTokens[f] = s.usedTokens[f] \cup {tok}]

SESSION_REPLACED(f, tok) ==
    LET base == ClearOverlay(s)
    IN /\ f \in Fs
       /\ tok \in Tokens \ s.usedTokens[f]
       /\ s.session = f
       /\ s' = [base EXCEPT
                    !.sessionToken = tok,
                    !.usedTokens[f] = s.usedTokens[f] \cup {tok},
                    !.commitDisconnected =
                        s.commitDisconnected \/ s.commitUnacked]

SESSION_DISCONNECTED(f, tok) ==
    LET base == ClearOverlay(s)
    IN /\ f \in Fs
       /\ tok \in Tokens
       /\ CurrentSession(s, f, tok)
       /\ s' = [base EXCEPT
                    !.session = NoF,
                    !.sessionToken = NoToken,
                    !.commitDisconnected =
                        s.commitDisconnected \/ s.commitUnacked]

STALE_SESSION_CALLBACK(f, tok) ==
    /\ f \in Fs
    /\ tok \in s.usedTokens[f]
    /\ ~CurrentSession(s, f, tok)
    /\ s' = [s EXCEPT !.staleSessionSeen = TRUE]

HISTORY_RESET(f, tok) ==
    LET nextNonce == 1 - s.nonce[f]
    IN /\ f \in Fs
       /\ tok \in Tokens
       /\ CurrentSession(s, f, tok)
       /\ s.cActiveOp = NoOp
       /\ s.pendingOp = NoOp
       /\ ~s.commitUnacked
       /\ s' = [s EXCEPT
                    !.route[f] = TRUE,
                    !.nonce[f] = nextNonce,
                    !.fRel[f] = 0,
                    !.lastCommitOp[f] = NoOp,
                    !.cF = f,
                    !.cNonce = nextNonce,
                    !.cRel = 0,
                    !.commitDisconnected = FALSE]

C_TX_BEGIN(f, t, d) ==
    LET op == Op(f, s.cNonce, s.cRel, t, d)
    IN /\ f \in Fs
       /\ t \in TUs
       /\ d \in DigestVariants
       /\ op \in RealOps
       /\ s.session = f
       /\ s.cF = f
       /\ s.route[f]
       /\ s.nonce[f] = s.cNonce
       /\ s.fRel[f] = s.cRel
       /\ s.cActiveOp = NoOp
       /\ ~s.commitUnacked
       /\ (MutantBeginAtMaxRel \/ s.cRel < MaxRel)
       /\ s' = [s EXCEPT !.cActiveOp = op]

F_TX_BEGIN(op) ==
    LET f == OpF(op)
    IN /\ op \in RealOps
       /\ op = s.cActiveOp
       /\ s.pendingOp = NoOp
       /\ s.session = f
       /\ s.route[f]
       /\ OpNonce(op) = s.nonce[f]
       /\ OpRel(op) = s.fRel[f]
       /\ s' = [s EXCEPT
                    !.pendingOp = op,
                    !.pendingToken = s.sessionToken,
                    !.dictDone = FALSE,
                    !.needRecorded = FALSE,
                    !.requested = {},
                    !.missing = {},
                    !.pinned = {},
                    !.bodyDone = FALSE,
                    !.materialized = FALSE]

ACTIVE_REPLAYED(op) == F_TX_BEGIN(op)

TX_ABORTED(op) ==
    /\ op \in RealOps
    /\ op = s.cActiveOp
    /\ s.pendingOp = NoOp
    /\ (MutantAbortAfterCommit \/ ~s.commitUnacked)
    /\ s' = [s EXCEPT !.cActiveOp = NoOp]

DICT_COMPLETE(op) ==
    /\ op \in RealOps
    /\ op = s.pendingOp
    /\ CurrentSession(s, OpF(op), s.pendingToken)
    /\ ~s.dictDone
    /\ s' = [s EXCEPT !.dictDone = TRUE]

NEED_RECORDED(op) ==
    LET f == OpF(op)
        required == TuObjects(OpTu(op))
        present == Present(s, f)
    IN /\ op \in RealOps
       /\ op = s.pendingOp
       /\ CurrentSession(s, f, s.pendingToken)
       /\ s.dictDone
       /\ ~s.needRecorded
       /\ s' = [s EXCEPT
                    !.needRecorded = TRUE,
                    !.requested = required \ present,
                    !.missing = required \ present,
                    !.pinned = required \cap present]

BODY_COMPLETE(op) ==
    /\ op \in RealOps
    /\ op = s.pendingOp
    /\ CurrentSession(s, OpF(op), s.pendingToken)
    /\ ~s.bodyDone
    /\ s' = [s EXCEPT !.bodyDone = TRUE]

OBJECT_APPLIED(op, o) ==
    LET f == OpF(op)
    IN /\ op \in RealOps
       /\ op = s.pendingOp
       /\ o \in Objects
       /\ CurrentSession(s, f, s.pendingToken)
       /\ s.needRecorded
       /\ o \in s.requested
       /\ s.content[f][o] \in {NoContent, CanonicalContent(o)}
       /\ s' = [s EXCEPT
                    !.content[f][o] = CanonicalContent(o),
                    !.missing = @ \ {o},
                    !.pinned = @ \cup {o}]

REJECT_CONFLICTING_OBJECT(op, o, value) ==
    LET f == OpF(op)
        base == ClearOverlay(s)
    IN /\ op \in RealOps
       /\ op = s.pendingOp
       /\ o \in Objects
       /\ value \in Values
       /\ value # CanonicalContent(o)
       /\ CurrentSession(s, f, s.pendingToken)
       /\ s' = [base EXCEPT
                    !.session = NoF,
                    !.sessionToken = NoToken]

STALE_OPERATION_CALLBACK(f, tok, op) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ op \in RealOps
    /\ CurrentSession(s, f, tok)
    /\ op # s.pendingOp
    /\ s' = [s EXCEPT !.staleOperationSeen = TRUE]

EVICT_OBJECT(f, o) ==
    /\ f \in Fs
    /\ o \in Objects
    /\ s.content[f][o] # NoContent
    /\ IF s.pendingOp # NoOp /\ OpF(s.pendingOp) = f
          THEN o \notin s.pinned
          ELSE TRUE
    /\ s' = [s EXCEPT !.content[f][o] = NoContent]

INPUT_MATERIALIZED(op) ==
    LET f == OpF(op)
        required == TuObjects(OpTu(op))
    IN /\ op \in RealOps
       /\ op = s.pendingOp
       /\ CurrentSession(s, f, s.pendingToken)
       /\ s.dictDone
       /\ s.needRecorded
       /\ s.bodyDone
       /\ s.missing = {}
       /\ s.pinned = required
       /\ required \subseteq Present(s, f)
       /\ ~s.materialized
       /\ s' = [s EXCEPT !.materialized = TRUE]

INPUT_COMMITTED(callbackOp) ==
    LET current == s.pendingOp
        f == OpF(current)
        base == ClearOverlay(s)
    IN /\ callbackOp \in RealOps
       /\ current \in RealOps
       /\ (callbackOp = current \/
             /\ MutantIgnoreTxDigest
             /\ SameCursorWithoutDigest(callbackOp, current))
       /\ current = s.cActiveOp
       /\ CurrentSession(s, f, s.pendingToken)
       /\ s.materialized
       /\ OpNonce(current) = s.nonce[f]
       /\ OpRel(current) = s.fRel[f]
       /\ OpRel(current) = s.cRel
       /\ s.cRel < MaxRel
       /\ s' = [base EXCEPT
                    !.fRel[f] = s.fRel[f] + 1,
                    !.lastCommitOp[f] = current,
                    !.commitUnacked = TRUE,
                    !.commitDisconnected = FALSE,
                    !.badCommit =
                        s.badCommit \/
                        callbackOp # current \/
                        ~(s.dictDone /\ s.needRecorded /\
                          s.bodyDone /\ s.missing = {} /\
                          s.materialized)]

COMMIT_ACCEPTED(f, tok, op) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ op \in RealOps
    /\ CurrentSession(s, f, tok)
    /\ op = s.cActiveOp
    /\ op = s.lastCommitOp[f]
    /\ OpF(op) = s.cF
    /\ OpNonce(op) = s.cNonce
    /\ OpRel(op) = s.cRel
    /\ s.fRel[f] = s.cRel + 1
    /\ s.commitUnacked
    /\ ~s.commitDisconnected
    /\ s' = [s EXCEPT
                 !.cRel = @ + 1,
                 !.cActiveOp = NoOp,
                 !.commitUnacked = FALSE,
                 !.commitDisconnected = FALSE]

LOST_COMMIT_ACCEPTED(f, tok, op) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ op \in RealOps
    /\ CurrentSession(s, f, tok)
    /\ op = s.cActiveOp
    /\ op = s.lastCommitOp[f]
    /\ OpF(op) = s.cF
    /\ OpNonce(op) = s.cNonce
    /\ OpRel(op) = s.cRel
    /\ s.fRel[f] = s.cRel + 1
    /\ s.commitUnacked
    /\ s.commitDisconnected
    /\ s' = [s EXCEPT
                 !.cRel = @ + 1,
                 !.cActiveOp = NoOp,
                 !.commitUnacked = FALSE,
                 !.commitDisconnected = FALSE]

STALE_ACK_CALLBACK(f, tok, op) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ op \in RealOps
    /\ CurrentSession(s, f, tok)
    /\ (op # s.cActiveOp \/ op # s.lastCommitOp[f])
    /\ s' = [s EXCEPT !.staleOperationSeen = TRUE]

Next ==
    \/ \E f \in Fs, tok \in Tokens : SESSION_OPENED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : SESSION_REPLACED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : SESSION_DISCONNECTED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : STALE_SESSION_CALLBACK(f, tok)
    \/ \E f \in Fs, tok \in Tokens : HISTORY_RESET(f, tok)
    \/ \E f \in Fs, t \in TUs, d \in DigestVariants : C_TX_BEGIN(f, t, d)
    \/ \E op \in RealOps : F_TX_BEGIN(op)
    \/ \E op \in RealOps : ACTIVE_REPLAYED(op)
    \/ \E op \in RealOps : TX_ABORTED(op)
    \/ \E op \in RealOps : DICT_COMPLETE(op)
    \/ \E op \in RealOps : NEED_RECORDED(op)
    \/ \E op \in RealOps : BODY_COMPLETE(op)
    \/ \E op \in RealOps, o \in Objects : OBJECT_APPLIED(op, o)
    \/ \E op \in RealOps, o \in Objects, v \in Values :
           REJECT_CONFLICTING_OBJECT(op, o, v)
    \/ \E f \in Fs, tok \in Tokens, op \in RealOps :
           STALE_OPERATION_CALLBACK(f, tok, op)
    \/ \E f \in Fs, o \in Objects : EVICT_OBJECT(f, o)
    \/ \E op \in RealOps : INPUT_MATERIALIZED(op)
    \/ \E op \in RealOps : INPUT_COMMITTED(op)
    \/ \E f \in Fs, tok \in Tokens, op \in RealOps :
           COMMIT_ACCEPTED(f, tok, op)
    \/ \E f \in Fs, tok \in Tokens, op \in RealOps :
           LOST_COMMIT_ACCEPTED(f, tok, op)
    \/ \E f \in Fs, tok \in Tokens, op \in RealOps :
           STALE_ACK_CALLBACK(f, tok, op)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ s.session \in Fs \cup {NoF}
    /\ s.sessionToken \in Tokens \cup {NoToken}
    /\ s.usedTokens \in [Fs -> SUBSET Tokens]
    /\ s.route \in [Fs -> BOOLEAN]
    /\ s.nonce \in [Fs -> Nonces]
    /\ s.fRel \in [Fs -> Rels]
    /\ s.content \in [Fs -> [Objects -> Values \cup {NoContent}]]
    /\ s.lastCommitOp \in [Fs -> Ops]
    /\ s.cF \in Fs \cup {NoF}
    /\ s.cNonce \in Nonces
    /\ s.cRel \in Rels
    /\ s.cActiveOp \in Ops
    /\ s.pendingOp \in Ops
    /\ s.pendingToken \in Tokens \cup {NoToken}
    /\ s.dictDone \in BOOLEAN
    /\ s.needRecorded \in BOOLEAN
    /\ s.requested \subseteq Objects
    /\ s.missing \subseteq Objects
    /\ s.pinned \subseteq Objects
    /\ s.bodyDone \in BOOLEAN
    /\ s.materialized \in BOOLEAN
    /\ s.commitUnacked \in BOOLEAN
    /\ s.commitDisconnected \in BOOLEAN
    /\ s.badCommit \in BOOLEAN
    /\ s.staleSessionSeen \in BOOLEAN
    /\ s.staleOperationSeen \in BOOLEAN

InstalledContentExact ==
    \A f \in Fs, o \in Objects :
        s.content[f][o] \in {NoContent, CanonicalContent(o)}

OneActive ==
    s.pendingOp = NoOp \/ s.pendingOp = s.cActiveOp

SessionFence ==
    s.pendingOp = NoOp \/
        CurrentSession(s, OpF(s.pendingOp), s.pendingToken)

ActiveOperationMatchesCursor ==
    s.cActiveOp = NoOp \/
        /\ OpF(s.cActiveOp) = s.cF
        /\ OpNonce(s.cActiveOp) = s.cNonce
        /\ OpRel(s.cActiveOp) = s.cRel

PendingOperationMatchesRoute ==
    s.pendingOp = NoOp \/
        /\ s.pendingOp = s.cActiveOp
        /\ s.route[OpF(s.pendingOp)]
        /\ OpNonce(s.pendingOp) = s.nonce[OpF(s.pendingOp)]
        /\ OpRel(s.pendingOp) = s.fRel[OpF(s.pendingOp)]

NeedIsExact ==
    IF s.pendingOp # NoOp /\ s.needRecorded
    THEN LET required == TuObjects(OpTu(s.pendingOp))
         IN /\ s.requested \subseteq required
            /\ s.missing \subseteq s.requested
            /\ s.pinned = required \ s.missing
    ELSE /\ s.requested = {}
         /\ s.missing = {}
         /\ s.pinned = {}

PinnedObjectsPresent ==
    s.pendingOp = NoOp \/
        s.pinned \subseteq Present(s, OpF(s.pendingOp))

CommitOnlyAfterExactMaterialization ==
    s.badCommit = FALSE

AtMostOneAhead ==
    IF s.cF \in Fs /\ s.route[s.cF] /\ s.cNonce = s.nonce[s.cF]
    THEN s.fRel[s.cF] = s.cRel \/ s.fRel[s.cF] = s.cRel + 1
    ELSE TRUE

CommitReconciliationWitness ==
    ~s.commitUnacked \/
        /\ s.cF \in Fs
        /\ s.cActiveOp \in RealOps
        /\ s.pendingOp = NoOp
        /\ s.lastCommitOp[s.cF] = s.cActiveOp
        /\ OpF(s.cActiveOp) = s.cF
        /\ OpNonce(s.cActiveOp) = s.cNonce
        /\ OpRel(s.cActiveOp) = s.cRel
        /\ s.nonce[s.cF] = s.cNonce
        /\ s.fRel[s.cF] = s.cRel + 1

ActiveSequenceHasRoom ==
    /\ (s.cActiveOp # NoOp => s.cRel < MaxRel)
    /\ (s.pendingOp # NoOp => OpRel(s.pendingOp) < MaxRel)

LastCommitOperationWellFormed ==
    \A f \in Fs :
        s.lastCommitOp[f] = NoOp \/
            /\ OpF(s.lastCommitOp[f]) = f
            /\ OpRel(s.lastCommitOp[f]) + 1 = s.fRel[f]

=============================================================================
