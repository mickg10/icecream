----------------------------- MODULE Protocol50 -----------------------------
EXTENDS Naturals, Integers, Sequences, FiniteSets, TLC

(***************************************************************************
Protocol-50 safety model.

This model is intentionally smaller than the implementation.  It models the
state transitions that are easy to get subtly wrong:

  * one C namespace and one or two independent F arenas;
  * one active cache transaction per C/F relationship;
  * immutable object publication independent of input/history commit;
  * F-authoritative object presence, pinning, and eviction;
  * whole-current-TU replay after disconnect;
  * lost-final-commit acknowledgement reconciliation;
  * F-store restart and history-only reset;
  * cache and legacy compiler attempts, including job restart/fallback;
  * at most one accepted result for one logical job.

Framing bytes, compression, scheduler policy, compiler-environment transfer,
and timing are deliberately outside this model.  They are parser, integration,
and simulator concerns.  Environment readiness is one external Boolean gate.
***************************************************************************)

CONSTANTS
    F0, F1,
    T0, T1,
    O0, O1,
    J0,
    V0, V1,
    UseF1,
    MaxRel,
    MaxAttempts,
    EpochMod

ASSUME /\ UseF1 \in BOOLEAN
       /\ MaxRel \in Nat
       /\ MaxRel > 0
       /\ MaxAttempts \in Nat
       /\ MaxAttempts > 0
       /\ EpochMod \in Nat
       /\ EpochMod >= 3
       /\ F0 # F1
       /\ T0 # T1
       /\ O0 # O1
       /\ V0 # V1

Fs == IF UseF1 THEN {F0, F1} ELSE {F0}
Tus == {T0, T1}
Objs == {O0, O1}
Jobs == {J0}
Values == {V0, V1}
Profiles == {"ZSTD", "P29", "GRZ"}
Modes == {"None", "Cache", "Legacy"}
Attempts == 0..MaxAttempts
Epochs == 0..(EpochMod - 1)

NoTx == "NoTx"
NoCommit == "NoCommit"
NoValue == "NoValue"
NoDigest == "NoDigest"
NoWorker == "NoWorker"
NoAttempt == -1

Required(t) == IF t = T0 THEN {O0} ELSE {O0, O1}
Canonical(o) == IF o = O0 THEN V0 ELSE V1
JobTu(j) == T0
RawDigest(t) == <<"raw", t>>

CanAdvanceEpoch(e) == e < (EpochMod - 1)
NextEpoch(e) == e + 1

IsHistory(h) == /\ h \in Seq(Tus)
                /\ Len(h) <= MaxRel

IsPrefix(a, b) ==
    IF Len(a) > Len(b)
    THEN FALSE
    ELSE IF Len(a) = 0
         THEN TRUE
         ELSE SubSeq(b, 1, Len(a)) = a

TxDigest(cg, fg, nonce, rel, tu, profile, pre) ==
    <<cg, fg, nonce, rel, tu, profile, pre>>

IsTx(tx) ==
    IF tx = NoTx
    THEN TRUE
    ELSE /\ tx.cguid \in Epochs
         /\ tx.fguid \in Epochs
         /\ tx.nonce \in Epochs
         /\ tx.tu \in Tus
         /\ tx.profile \in Profiles
         /\ IsHistory(tx.pre)
         /\ tx.rel = Len(tx.pre)
         /\ tx.rel < MaxRel
         /\ tx.digest = TxDigest(tx.cguid,
                                 tx.fguid,
                                 tx.nonce,
                                 tx.rel,
                                 tx.tu,
                                 tx.profile,
                                 tx.pre)

IsCommit(c) ==
    IF c = NoCommit
    THEN TRUE
    ELSE /\ IsTx(c.tx)
         /\ c.tx # NoTx
         /\ c.post = Append(c.tx.pre, c.tx.tu)
         /\ IsHistory(c.post)

MakeTx(st, f, t, profile) ==
    [cguid   |-> st.cGuid,
     fguid   |-> st.fGuid[f],
     nonce   |-> st.cNonce[f],
     rel     |-> Len(st.cHistory[f]),
     tu      |-> t,
     profile |-> profile,
     pre     |-> st.cHistory[f],
     digest  |-> TxDigest(st.cGuid,
                           st.fGuid[f],
                           st.cNonce[f],
                           Len(st.cHistory[f]),
                           t,
                           profile,
                           st.cHistory[f])]

Present(st, f) == {o \in Objs : st.objValue[f][o] # NoValue}

Healthy(st, f) ==
    /\ st.cSeenF[f] = st.fGuid[f]
    /\ st.cNonce[f] = st.fNonce[f]

CurrentSession(st, f, epoch) ==
    /\ f \in st.sessionOpen
    /\ epoch = st.sessionEpoch[f]

VARIABLE s
vars == <<s>>

Init ==
    s = [prepared         |-> {},
         cGuid            |-> 0,
         fGuid            |-> [f \in Fs |-> 0],
         sessionEpoch     |-> [f \in Fs |-> 0],
         sessionOpen      |-> {},
         cSeenF           |-> [f \in Fs |-> 0],
         cNonce           |-> [f \in Fs |-> 0],
         fNonce           |-> [f \in Fs |-> 0],
         cHistory         |-> [f \in Fs |-> <<>>],
         fHistory         |-> [f \in Fs |-> <<>>],
         cActive          |-> [f \in Fs |-> NoTx],
         fActive          |-> [f \in Fs |-> NoTx],
         fActiveEpoch     |-> [f \in Fs |-> 0],
         dictDone         |-> [f \in Fs |-> FALSE],
         needComputed     |-> [f \in Fs |-> FALSE],
         bodyDone         |-> [f \in Fs |-> FALSE],
         needSet          |-> [f \in Fs |-> {}],
         objValue         |-> [f \in Fs |-> [o \in Objs |-> NoValue]],
         pins             |-> [f \in Fs |-> {}],
         lastCommit       |-> [f \in Fs |-> NoCommit],
         committedInputs  |-> [f \in Fs |-> {}],
         inputDigest      |-> [f \in Fs |-> [t \in Tus |-> NoDigest]],
         envReady         |-> {},
         nextAttempt      |-> [j \in Jobs |-> 0],
         attemptMode      |-> [j \in Jobs |-> [a \in Attempts |-> "None"]],
         attemptWorker    |-> [j \in Jobs |-> [a \in Attempts |-> NoWorker]],
         attemptLive      |-> {},
         attemptStarted   |-> {},
         acceptedAttempt  |-> [j \in Jobs |-> NoAttempt]]

Prepare(t) ==
    /\ t \in Tus
    /\ t \notin s.prepared
    /\ s' = [s EXCEPT !.prepared = @ \cup {t}]

OpenSession(f) ==
    /\ f \in Fs
    /\ CanAdvanceEpoch(s.sessionEpoch[f])
    /\ s' = [s EXCEPT
                 !.sessionEpoch[f] = NextEpoch(@),
                 !.sessionOpen = @ \cup {f},
                 !.cSeenF[f] = s.fGuid[f],
                 !.fActive[f] = NoTx,
                 !.fActiveEpoch[f] = NextEpoch(s.sessionEpoch[f]),
                 !.dictDone[f] = FALSE,
                 !.needComputed[f] = FALSE,
                 !.bodyDone[f] = FALSE,
                 !.needSet[f] = {},
                 !.pins[f] = {}]

Route(f, t, profile) ==
    /\ f \in Fs
    /\ t \in s.prepared
    /\ profile \in Profiles
    /\ f \in s.sessionOpen
    /\ Healthy(s, f)
    /\ s.cHistory[f] = s.fHistory[f]
    /\ Len(s.cHistory[f]) < MaxRel
    /\ s.cActive[f] = NoTx
    /\ s.fActive[f] = NoTx
    /\ LET tx == MakeTx(s, f, t, profile)
       IN s' = [s EXCEPT
                    !.cActive[f] = tx,
                    !.fActive[f] = tx,
                    !.fActiveEpoch[f] = s.sessionEpoch[f],
                    !.dictDone[f] = FALSE,
                    !.needComputed[f] = FALSE,
                    !.bodyDone[f] = FALSE,
                    !.needSet[f] = {},
                    !.pins[f] = {}]

ReceiveDict(f, epoch) ==
    LET tx == s.fActive[f]
    IN IF tx = NoTx
       THEN FALSE
       ELSE /\ CurrentSession(s, f, epoch)
            /\ s.fActiveEpoch[f] = epoch
            /\ ~s.dictDone[f]
            /\ s' = [s EXCEPT !.dictDone[f] = TRUE]

ComputeNeed(f, epoch) ==
    LET tx == s.fActive[f]
    IN IF tx = NoTx
       THEN FALSE
       ELSE /\ CurrentSession(s, f, epoch)
            /\ s.fActiveEpoch[f] = epoch
            /\ s.dictDone[f]
            /\ ~s.needComputed[f]
            /\ s' = [s EXCEPT
                          !.needComputed[f] = TRUE,
                          !.needSet[f] = Required(tx.tu) \ Present(s, f)]

ReceiveBody(f, epoch) ==
    LET tx == s.fActive[f]
    IN IF tx = NoTx
       THEN FALSE
       ELSE /\ CurrentSession(s, f, epoch)
            /\ s.fActiveEpoch[f] = epoch
            /\ ~s.bodyDone[f]
            /\ s' = [s EXCEPT !.bodyDone[f] = TRUE]

ApplyObject(f, epoch, o, value) ==
    /\ f \in Fs
    /\ o \in Objs
    /\ value \in Values
    /\ CurrentSession(s, f, epoch)
    /\ s.needComputed[f]
    /\ o \in s.needSet[f]
    /\ value = Canonical(o)
    /\ s.objValue[f][o] \in {NoValue, value}
    /\ s' = [s EXCEPT !.objValue[f][o] = value]

RejectConflictingObject(f, epoch, o, value) ==
    /\ f \in Fs
    /\ o \in Objs
    /\ value \in Values
    /\ CurrentSession(s, f, epoch)
    /\ value # Canonical(o)
    /\ s' = [s EXCEPT
                 !.sessionOpen = @ \ {f},
                 !.fActive[f] = NoTx,
                 !.dictDone[f] = FALSE,
                 !.needComputed[f] = FALSE,
                 !.bodyDone[f] = FALSE,
                 !.needSet[f] = {},
                 !.pins[f] = {}]

PinClosure(f, epoch) ==
    LET tx == s.fActive[f]
    IN IF tx = NoTx
       THEN FALSE
       ELSE /\ CurrentSession(s, f, epoch)
            /\ s.fActiveEpoch[f] = epoch
            /\ s.dictDone[f]
            /\ s.needComputed[f]
            /\ s.bodyDone[f]
            /\ Required(tx.tu) \subseteq Present(s, f)
            /\ s.pins[f] = {}
            /\ s' = [s EXCEPT !.pins[f] = Required(tx.tu)]

CommitInput(f, epoch) ==
    LET tx == s.fActive[f]
    IN IF tx = NoTx
       THEN FALSE
       ELSE LET post == Append(s.fHistory[f], tx.tu)
            IN /\ CurrentSession(s, f, epoch)
               /\ s.fActiveEpoch[f] = epoch
               /\ s.cActive[f] = tx
               /\ tx.cguid = s.cGuid
               /\ tx.fguid = s.fGuid[f]
               /\ tx.nonce = s.fNonce[f]
               /\ tx.pre = s.fHistory[f]
               /\ tx.rel = Len(s.fHistory[f])
               /\ Len(s.fHistory[f]) < MaxRel
               /\ s.pins[f] = Required(tx.tu)
               /\ Required(tx.tu) \subseteq Present(s, f)
               /\ s' = [s EXCEPT
                            !.fHistory[f] = post,
                            !.lastCommit[f] = [tx |-> tx, post |-> post],
                            !.committedInputs[f] = @ \cup {tx.tu},
                            !.inputDigest[f][tx.tu] = RawDigest(tx.tu),
                            !.fActive[f] = NoTx,
                            !.dictDone[f] = FALSE,
                            !.needComputed[f] = FALSE,
                            !.bodyDone[f] = FALSE,
                            !.needSet[f] = {},
                            !.pins[f] = {}]

AckCommit(f) ==
    LET tx == s.cActive[f]
        lc == s.lastCommit[f]
    IN IF tx = NoTx \/ lc = NoCommit
       THEN FALSE
       ELSE /\ f \in s.sessionOpen
            /\ Healthy(s, f)
            /\ lc.tx = tx
            /\ lc.post = s.fHistory[f]
            /\ Len(s.fHistory[f]) = Len(s.cHistory[f]) + 1
            /\ IsPrefix(s.cHistory[f], s.fHistory[f])
            /\ s' = [s EXCEPT
                          !.cHistory[f] = lc.post,
                          !.cActive[f] = NoTx]

Disconnect(f) ==
    /\ f \in Fs
    /\ f \in s.sessionOpen
    /\ s' = [s EXCEPT
                 !.sessionOpen = @ \ {f},
                 !.fActive[f] = NoTx,
                 !.dictDone[f] = FALSE,
                 !.needComputed[f] = FALSE,
                 !.bodyDone[f] = FALSE,
                 !.needSet[f] = {},
                 !.pins[f] = {}]

ReplayActive(f) ==
    LET tx == s.cActive[f]
    IN IF tx = NoTx
       THEN FALSE
       ELSE /\ f \in s.sessionOpen
            /\ Healthy(s, f)
            /\ s.cHistory[f] = s.fHistory[f]
            /\ s.fActive[f] = NoTx
            /\ tx.cguid = s.cGuid
            /\ tx.fguid = s.fGuid[f]
            /\ tx.nonce = s.cNonce[f]
            /\ tx.pre = s.cHistory[f]
            /\ tx.rel = Len(s.cHistory[f])
            /\ s' = [s EXCEPT
                          !.fActive[f] = tx,
                          !.fActiveEpoch[f] = s.sessionEpoch[f],
                          !.dictDone[f] = FALSE,
                          !.needComputed[f] = FALSE,
                          !.bodyDone[f] = FALSE,
                          !.needSet[f] = {},
                          !.pins[f] = {}]

ReconcileLostAck(f) ==
    LET tx == s.cActive[f]
        lc == s.lastCommit[f]
    IN IF tx = NoTx \/ lc = NoCommit
       THEN FALSE
       ELSE /\ f \in s.sessionOpen
            /\ Healthy(s, f)
            /\ lc.tx = tx
            /\ lc.post = s.fHistory[f]
            /\ Len(s.fHistory[f]) = Len(s.cHistory[f]) + 1
            /\ IsPrefix(s.cHistory[f], s.fHistory[f])
            /\ s' = [s EXCEPT
                          !.cHistory[f] = lc.post,
                          !.cActive[f] = NoTx]

AbandonUncommitted(f) ==
    /\ f \in Fs
    /\ s.cActive[f] # NoTx
    /\ s.fActive[f] = NoTx
    /\ s.cHistory[f] = s.fHistory[f]
    /\ s' = [s EXCEPT !.cActive[f] = NoTx]

RestartF(f) ==
    /\ f \in Fs
    /\ CanAdvanceEpoch(s.fGuid[f])
    /\ CanAdvanceEpoch(s.fNonce[f])
    /\ CanAdvanceEpoch(s.sessionEpoch[f])
    /\ s' = [s EXCEPT
                 !.fGuid[f] = NextEpoch(@),
                 !.fNonce[f] = NextEpoch(@),
                 !.fHistory[f] = <<>>,
                 !.sessionEpoch[f] = NextEpoch(@),
                 !.sessionOpen = @ \ {f},
                 !.fActive[f] = NoTx,
                 !.dictDone[f] = FALSE,
                 !.needComputed[f] = FALSE,
                 !.bodyDone[f] = FALSE,
                 !.needSet[f] = {},
                 !.objValue[f] = [o \in Objs |-> NoValue],
                 !.pins[f] = {},
                 !.lastCommit[f] = NoCommit,
                 !.committedInputs[f] = {},
                 !.inputDigest[f] = [t \in Tus |-> NoDigest]]

AcceptColdArena(f) ==
    /\ f \in Fs
    /\ f \in s.sessionOpen
    /\ s.cSeenF[f] = s.fGuid[f]
    /\ s.fHistory[f] = <<>>
    /\ s.lastCommit[f] = NoCommit
    /\ Present(s, f) = {}
    /\ s.fActive[f] = NoTx
    /\ s' = [s EXCEPT
                 !.cNonce[f] = s.fNonce[f],
                 !.cHistory[f] = <<>>,
                 !.cActive[f] = NoTx]

ResetHistory(f) ==
    /\ f \in Fs
    /\ f \in s.sessionOpen
    /\ s.fActive[f] = NoTx
    /\ CanAdvanceEpoch(s.fNonce[f])
    /\ LET nonce == NextEpoch(s.fNonce[f])
       IN s' = [s EXCEPT
                    !.cSeenF[f] = s.fGuid[f],
                    !.cNonce[f] = nonce,
                    !.fNonce[f] = nonce,
                    !.cHistory[f] = <<>>,
                    !.fHistory[f] = <<>>,
                    !.cActive[f] = NoTx,
                    !.lastCommit[f] = NoCommit,
                    !.dictDone[f] = FALSE,
                    !.needComputed[f] = FALSE,
                    !.bodyDone[f] = FALSE,
                    !.needSet[f] = {},
                    !.pins[f] = {}]

EvictObject(f, o) ==
    /\ f \in Fs
    /\ o \in Objs
    /\ s.objValue[f][o] # NoValue
    /\ o \notin s.pins[f]
    /\ s' = [s EXCEPT !.objValue[f][o] = NoValue]

EnvironmentReady(f) ==
    /\ f \in Fs
    /\ f \notin s.envReady
    /\ s' = [s EXCEPT !.envReady = @ \cup {f}]

StartAttempt(j, f, mode) ==
    /\ j \in Jobs
    /\ f \in Fs
    /\ mode \in {"Cache", "Legacy"}
    /\ s.acceptedAttempt[j] = NoAttempt
    /\ s.nextAttempt[j] \in Attempts
    /\ LET a == s.nextAttempt[j]
       IN /\ <<j, a>> \notin s.attemptLive
          /\ IF mode = "Cache" THEN JobTu(j) \in s.prepared ELSE TRUE
          /\ s' = [s EXCEPT
                       !.attemptMode[j][a] = mode,
                       !.attemptWorker[j][a] = f,
                       !.attemptLive = @ \cup {<<j, a>>},
                       !.nextAttempt[j] = @ + 1]

DetachAttempt(j, a) ==
    /\ j \in Jobs
    /\ a \in Attempts
    /\ <<j, a>> \in s.attemptLive
    /\ s' = [s EXCEPT
                 !.attemptLive = @ \ {<<j, a>>},
                 !.attemptStarted = @ \ {<<j, a>>}]

StartCompiler(j, a) ==
    /\ j \in Jobs
    /\ a \in Attempts
    /\ <<j, a>> \in s.attemptLive
    /\ <<j, a>> \notin s.attemptStarted
    /\ LET f == s.attemptWorker[j][a]
           mode == s.attemptMode[j][a]
       IN /\ f \in Fs
          /\ f \in s.envReady
          /\ mode \in {"Cache", "Legacy"}
          /\ IF mode = "Cache"
             THEN /\ JobTu(j) \in s.committedInputs[f]
                  /\ s.inputDigest[f][JobTu(j)] = RawDigest(JobTu(j))
             ELSE TRUE
          /\ s' = [s EXCEPT
                       !.attemptStarted = @ \cup {<<j, a>>}]

AcceptResult(j, a) ==
    /\ j \in Jobs
    /\ a \in Attempts
    /\ <<j, a>> \in s.attemptStarted
    /\ s.acceptedAttempt[j] = NoAttempt
    /\ s' = [s EXCEPT
                 !.acceptedAttempt[j] = a,
                 !.attemptLive = {p \in s.attemptLive : p[1] # j},
                 !.attemptStarted = {p \in s.attemptStarted : p[1] # j}]

RestartCQuiescent ==
    /\ CanAdvanceEpoch(s.cGuid)
    /\ s.attemptLive = {}
    /\ s.attemptStarted = {}
    /\ \A f \in Fs : /\ s.cActive[f] = NoTx
                       /\ s.fActive[f] = NoTx
    /\ s' = [s EXCEPT
                 !.prepared = {},
                 !.cGuid = NextEpoch(@),
                 !.sessionOpen = {},
                 !.cSeenF = [f \in Fs |-> s.fGuid[f]],
                 !.cNonce = [f \in Fs |-> 0],
                 !.fNonce = [f \in Fs |-> 0],
                 !.cHistory = [f \in Fs |-> <<>>],
                 !.fHistory = [f \in Fs |-> <<>>],
                 !.cActive = [f \in Fs |-> NoTx],
                 !.fActive = [f \in Fs |-> NoTx],
                 !.dictDone = [f \in Fs |-> FALSE],
                 !.needComputed = [f \in Fs |-> FALSE],
                 !.bodyDone = [f \in Fs |-> FALSE],
                 !.needSet = [f \in Fs |-> {}],
                 !.objValue = [f \in Fs |-> [o \in Objs |-> NoValue]],
                 !.pins = [f \in Fs |-> {}],
                 !.lastCommit = [f \in Fs |-> NoCommit],
                 !.committedInputs = [f \in Fs |-> {}],
                 !.inputDigest = [f \in Fs |-> [t \in Tus |-> NoDigest]]]

Next ==
    \/ \E t \in Tus : Prepare(t)
    \/ \E f \in Fs : OpenSession(f)
    \/ \E f \in Fs, t \in Tus, p \in Profiles : Route(f, t, p)
    \/ \E f \in Fs, e \in Epochs : ReceiveDict(f, e)
    \/ \E f \in Fs, e \in Epochs : ComputeNeed(f, e)
    \/ \E f \in Fs, e \in Epochs : ReceiveBody(f, e)
    \/ \E f \in Fs, e \in Epochs, o \in Objs, v \in Values :
           ApplyObject(f, e, o, v)
    \/ \E f \in Fs, e \in Epochs, o \in Objs, v \in Values :
           RejectConflictingObject(f, e, o, v)
    \/ \E f \in Fs, e \in Epochs : PinClosure(f, e)
    \/ \E f \in Fs, e \in Epochs : CommitInput(f, e)
    \/ \E f \in Fs : AckCommit(f)
    \/ \E f \in Fs : Disconnect(f)
    \/ \E f \in Fs : ReplayActive(f)
    \/ \E f \in Fs : ReconcileLostAck(f)
    \/ \E f \in Fs : AbandonUncommitted(f)
    \/ \E f \in Fs : RestartF(f)
    \/ \E f \in Fs : AcceptColdArena(f)
    \/ \E f \in Fs : ResetHistory(f)
    \/ \E f \in Fs, o \in Objs : EvictObject(f, o)
    \/ \E f \in Fs : EnvironmentReady(f)
    \/ \E j \in Jobs, f \in Fs, m \in {"Cache", "Legacy"} :
           StartAttempt(j, f, m)
    \/ \E j \in Jobs, a \in Attempts : DetachAttempt(j, a)
    \/ \E j \in Jobs, a \in Attempts : StartCompiler(j, a)
    \/ \E j \in Jobs, a \in Attempts : AcceptResult(j, a)
    \/ RestartCQuiescent

Spec == Init /\ [][Next]_vars

(***************************************************************************
Safety invariants
***************************************************************************)

TypeOK ==
    /\ s.prepared \subseteq Tus
    /\ s.cGuid \in Epochs
    /\ s.fGuid \in [Fs -> Epochs]
    /\ s.sessionEpoch \in [Fs -> Epochs]
    /\ s.sessionOpen \subseteq Fs
    /\ s.cSeenF \in [Fs -> Epochs]
    /\ s.cNonce \in [Fs -> Epochs]
    /\ s.fNonce \in [Fs -> Epochs]
    /\ \A f \in Fs : /\ IsHistory(s.cHistory[f])
                       /\ IsHistory(s.fHistory[f])
                       /\ IsTx(s.cActive[f])
                       /\ IsTx(s.fActive[f])
                       /\ IsCommit(s.lastCommit[f])
    /\ s.fActiveEpoch \in [Fs -> Epochs]
    /\ s.dictDone \in [Fs -> BOOLEAN]
    /\ s.needComputed \in [Fs -> BOOLEAN]
    /\ s.bodyDone \in [Fs -> BOOLEAN]
    /\ s.needSet \in [Fs -> SUBSET Objs]
    /\ s.objValue \in [Fs -> [Objs -> Values \cup {NoValue}]]
    /\ s.pins \in [Fs -> SUBSET Objs]
    /\ s.committedInputs \in [Fs -> SUBSET Tus]
    /\ \A f \in Fs, t \in Tus :
           s.inputDigest[f][t] \in {NoDigest, RawDigest(t)}
    /\ s.envReady \subseteq Fs
    /\ s.nextAttempt \in [Jobs -> 0..(MaxAttempts + 1)]
    /\ s.attemptMode \in [Jobs -> [Attempts -> Modes]]
    /\ s.attemptWorker \in [Jobs -> [Attempts -> Fs \cup {NoWorker}]]
    /\ s.attemptLive \subseteq (Jobs \X Attempts)
    /\ s.attemptStarted \subseteq (Jobs \X Attempts)
    /\ s.acceptedAttempt \in [Jobs -> ({NoAttempt} \cup Attempts)]

ObjectIdentityImmutable ==
    \A f \in Fs, o \in Objs :
        s.objValue[f][o] \in {NoValue, Canonical(o)}

PinnedObjectsPresent ==
    \A f \in Fs : s.pins[f] \subseteq Present(s, f)

PinsBelongToActiveTx ==
    \A f \in Fs : s.pins[f] # {} => s.fActive[f] # NoTx

FencedSessionsCannotOwnActiveTx ==
    \A f \in Fs :
        s.fActive[f] # NoTx =>
            /\ f \in s.sessionOpen
            /\ s.fActiveEpoch[f] = s.sessionEpoch[f]

ActiveTxAgreement ==
    \A f \in Fs :
        IF s.fActive[f] = NoTx
        THEN TRUE
        ELSE /\ s.cActive[f] = s.fActive[f]
             /\ s.fActive[f].cguid = s.cGuid
             /\ s.fActive[f].fguid = s.fGuid[f]
             /\ s.fActive[f].nonce = s.fNonce[f]
             /\ s.fActive[f].pre = s.fHistory[f]
             /\ s.fActive[f].rel = Len(s.fHistory[f])

CActiveLegalWhenHealthy ==
    \A f \in Fs :
        IF s.cActive[f] = NoTx \/ ~Healthy(s, f)
        THEN TRUE
        ELSE /\ s.cActive[f].cguid = s.cGuid
             /\ s.cActive[f].fguid = s.fGuid[f]
             /\ s.cActive[f].nonce = s.cNonce[f]
             /\ s.cActive[f].pre = s.cHistory[f]
             /\ s.cActive[f].rel = Len(s.cHistory[f])

HealthyRouteHasAtMostOneUnackedCommit ==
    \A f \in Fs :
        IF ~Healthy(s, f)
        THEN TRUE
        ELSE /\ IsPrefix(s.cHistory[f], s.fHistory[f])
             /\ Len(s.fHistory[f]) <= Len(s.cHistory[f]) + 1
             /\ IF Len(s.fHistory[f]) = Len(s.cHistory[f]) + 1
                THEN /\ s.cActive[f] # NoTx
                     /\ s.lastCommit[f] # NoCommit
                     /\ s.lastCommit[f].tx = s.cActive[f]
                     /\ s.lastCommit[f].post = s.fHistory[f]
                ELSE TRUE

LastCommitWellFormed ==
    \A f \in Fs :
        IF s.lastCommit[f] = NoCommit
        THEN TRUE
        ELSE /\ s.lastCommit[f].post =
                    Append(s.lastCommit[f].tx.pre,
                           s.lastCommit[f].tx.tu)
             /\ s.lastCommit[f].tx.rel =
                    Len(s.lastCommit[f].tx.pre)

CommittedInputHasExactDigest ==
    \A f \in Fs, t \in Tus :
        t \in s.committedInputs[f] =>
            s.inputDigest[f][t] = RawDigest(t)

CompilerStartsOnlyWithValidInput ==
    \A pair \in s.attemptStarted :
        LET j == pair[1]
            a == pair[2]
            f == s.attemptWorker[j][a]
            mode == s.attemptMode[j][a]
        IN /\ pair \in s.attemptLive
           /\ f \in Fs
           /\ f \in s.envReady
           /\ mode \in {"Cache", "Legacy"}
           /\ IF mode = "Cache"
              THEN /\ JobTu(j) \in s.committedInputs[f]
                   /\ s.inputDigest[f][JobTu(j)] = RawDigest(JobTu(j))
              ELSE TRUE

AcceptedResultIsUniqueAndAllocated ==
    \A j \in Jobs :
        IF s.acceptedAttempt[j] = NoAttempt
        THEN TRUE
        ELSE /\ s.acceptedAttempt[j] \in Attempts
             /\ s.acceptedAttempt[j] < s.nextAttempt[j]
             /\ s.attemptMode[j][s.acceptedAttempt[j]] \in
                    {"Cache", "Legacy"}

Safety ==
    /\ TypeOK
    /\ ObjectIdentityImmutable
    /\ PinnedObjectsPresent
    /\ PinsBelongToActiveTx
    /\ FencedSessionsCannotOwnActiveTx
    /\ ActiveTxAgreement
    /\ CActiveLegalWhenHealthy
    /\ HealthyRouteHasAtMostOneUnackedCommit
    /\ LastCommitWellFormed
    /\ CommittedInputHasExactDigest
    /\ CompilerStartsOnlyWithValidInput
    /\ AcceptedResultIsUniqueAndAllocated

=============================================================================
