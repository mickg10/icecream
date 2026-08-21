------------------------------ MODULE Protocol50 ------------------------------
EXTENDS Naturals, FiniteSets, TLC

(***************************************************************************
Protocol-50 cache-transaction safety core.

The model deliberately keeps one C-side active transaction and one F-side
pending overlay.  It covers the states that must stay exact when sessions are
replaced, callbacks arrive late, immutable objects are installed, or an object
cache evicts entries.  Compiler-job restart/result arbitration is modeled in
Protocol50JobLifecycle.tla and composes at INPUT_COMMITTED.
***************************************************************************)

CONSTANTS F0, F1, T0, T1, O0, O1, V0, V1,
          Tok0, Tok1, NoF, NoTU, NoToken, NoContent,
          CActor, FActor, MaxRel

Fs == {F0, F1}
TUs == {T0, T1}
Objects == {O0, O1}
Values == {V0, V1}
Tokens == {Tok0, Tok1}
Actors == {CActor, FActor}

TuObjects(t) == IF t = T0 THEN {O0} ELSE {O0, O1}
CanonicalContent(o) == IF o = O0 THEN V0 ELSE V1
Present(contentMap) == {o \in Objects : contentMap[o] # NoContent}

VARIABLES session, sessionToken, usedTokens,
          route, nonce, fRel, content, lastCommit,
          cF, cNonce, cRel, cActive,
          pending, pendingToken, dictDone, needRecorded, requested, missing,
          pinned, bodyDone, materialized,
          commitUnacked, commitDisconnected, badCommit,
          staleObjectSeen, staleAckSeen, staleCloseSeen, staleControlSeen

vars == <<session, sessionToken, usedTokens,
          route, nonce, fRel, content, lastCommit,
          cF, cNonce, cRel, cActive,
          pending, pendingToken, dictDone, needRecorded, requested, missing,
          pinned, bodyDone, materialized,
          commitUnacked, commitDisconnected, badCommit,
          staleObjectSeen, staleAckSeen, staleCloseSeen, staleControlSeen>>

CurrentSession(f, tok) ==
    /\ session = f
    /\ sessionToken = tok

Init ==
    /\ session = NoF
    /\ sessionToken = NoToken
    /\ usedTokens = [f \in Fs |-> {}]
    /\ route = [f \in Fs |-> FALSE]
    /\ nonce = [f \in Fs |-> 0]
    /\ fRel = [f \in Fs |-> 0]
    /\ content = [f \in Fs |-> [o \in Objects |-> NoContent]]
    /\ lastCommit = [f \in Fs |-> NoTU]
    /\ cF = NoF
    /\ cNonce = 0
    /\ cRel = 0
    /\ cActive = NoTU
    /\ pending = NoTU
    /\ pendingToken = NoToken
    /\ dictDone = FALSE
    /\ needRecorded = FALSE
    /\ requested = {}
    /\ missing = {}
    /\ pinned = {}
    /\ bodyDone = FALSE
    /\ materialized = FALSE
    /\ commitUnacked = FALSE
    /\ commitDisconnected = FALSE
    /\ badCommit = FALSE
    /\ staleObjectSeen = FALSE
    /\ staleAckSeen = FALSE
    /\ staleCloseSeen = FALSE
    /\ staleControlSeen = FALSE

ClearFOverlay ==
    /\ pending' = NoTU
    /\ pendingToken' = NoToken
    /\ dictDone' = FALSE
    /\ needRecorded' = FALSE
    /\ requested' = {}
    /\ missing' = {}
    /\ pinned' = {}
    /\ bodyDone' = FALSE
    /\ materialized' = FALSE

SESSION_OPENED(f, tok) ==
    /\ f \in Fs
    /\ tok \in Tokens \ usedTokens[f]
    /\ session = NoF
    /\ session' = f
    /\ sessionToken' = tok
    /\ usedTokens' = [usedTokens EXCEPT ![f] = @ \cup {tok}]
    /\ ClearFOverlay
    /\ UNCHANGED <<route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen,
                    staleControlSeen>>

SESSION_REPLACED(f, tok) ==
    /\ f \in Fs
    /\ tok \in Tokens \ usedTokens[f]
    /\ session = f
    /\ session' = f
    /\ sessionToken' = tok
    /\ usedTokens' = [usedTokens EXCEPT ![f] = @ \cup {tok}]
    /\ ClearFOverlay
    /\ commitDisconnected' = (commitDisconnected \/ commitUnacked)
    /\ UNCHANGED <<route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, commitUnacked,
                    badCommit, staleObjectSeen, staleAckSeen,
                    staleCloseSeen, staleControlSeen>>

SESSION_DISCONNECTED(f, tok) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ CurrentSession(f, tok)
    /\ session' = NoF
    /\ sessionToken' = NoToken
    /\ ClearFOverlay
    /\ commitDisconnected' = (commitDisconnected \/ commitUnacked)
    /\ UNCHANGED <<usedTokens, route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, commitUnacked,
                    badCommit, staleObjectSeen, staleAckSeen,
                    staleCloseSeen, staleControlSeen>>

STALE_CLOSE_CALLBACK(f, tok) ==
    /\ f \in Fs
    /\ tok \in usedTokens[f]
    /\ ~CurrentSession(f, tok)
    /\ staleCloseSeen' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleControlSeen>>

HISTORY_RESET(f, tok) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ CurrentSession(f, tok)
    /\ cActive = NoTU
    /\ pending = NoTU
    /\ ~commitUnacked
    /\ route' = [route EXCEPT ![f] = TRUE]
    /\ nonce' = [nonce EXCEPT ![f] = 1 - @]
    /\ fRel' = [fRel EXCEPT ![f] = 0]
    /\ lastCommit' = [lastCommit EXCEPT ![f] = NoTU]
    /\ cF' = f
    /\ cNonce' = 1 - nonce[f]
    /\ cRel' = 0
    /\ commitDisconnected' = FALSE
    /\ UNCHANGED <<session, sessionToken, usedTokens, content, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, badCommit, staleObjectSeen,
                    staleAckSeen, staleCloseSeen, staleControlSeen>>

STALE_CONTROL_CALLBACK(f, tok) ==
    /\ f \in Fs
    /\ tok \in usedTokens[f]
    /\ ~CurrentSession(f, tok)
    /\ staleControlSeen' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen>>

C_TX_BEGIN ==
    \E t \in TUs, f \in Fs:
        /\ session = f
        /\ cF = f
        /\ route[f]
        /\ nonce[f] = cNonce
        /\ fRel[f] = cRel
        /\ cActive = NoTU
        /\ ~commitUnacked
        /\ cActive' = t
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, content, lastCommit,
                        cF, cNonce, cRel,
                        pending, pendingToken, dictDone, needRecorded,
                        requested, missing, pinned, bodyDone, materialized,
                        commitUnacked, commitDisconnected, badCommit,
                        staleObjectSeen, staleAckSeen, staleCloseSeen,
                        staleControlSeen>>

F_TX_BEGIN ==
    \E f \in Fs:
        /\ session = f
        /\ cF = f
        /\ route[f]
        /\ nonce[f] = cNonce
        /\ fRel[f] = cRel
        /\ cActive \in TUs
        /\ pending = NoTU
        /\ pending' = cActive
        /\ pendingToken' = sessionToken
        /\ dictDone' = FALSE
        /\ needRecorded' = FALSE
        /\ requested' = {}
        /\ missing' = {}
        /\ pinned' = {}
        /\ bodyDone' = FALSE
        /\ materialized' = FALSE
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, content, lastCommit,
                        cF, cNonce, cRel, cActive,
                        commitUnacked, commitDisconnected, badCommit,
                        staleObjectSeen, staleAckSeen, staleCloseSeen,
                        staleControlSeen>>

TX_BEGIN(actor) ==
    /\ actor \in Actors
    /\ IF actor = CActor THEN C_TX_BEGIN ELSE F_TX_BEGIN

ACTIVE_REPLAYED == F_TX_BEGIN

TX_ABORTED ==
    /\ cActive \in TUs
    /\ pending = NoTU
    /\ cActive' = NoTU
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen,
                    staleControlSeen>>

DICT_COMPLETE ==
    /\ pending \in TUs
    /\ session = cF
    /\ pendingToken = sessionToken
    /\ ~dictDone
    /\ dictDone' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen,
                    staleControlSeen>>

NEED_RECORDED ==
    \E f \in Fs, t \in TUs:
        /\ session = f
        /\ cF = f
        /\ pending = t
        /\ pendingToken = sessionToken
        /\ dictDone
        /\ ~needRecorded
        /\ LET required == TuObjects(t)
               present == Present(content[f])
           IN /\ needRecorded' = TRUE
              /\ requested' = required \ present
              /\ missing' = required \ present
              /\ pinned' = required \cap present
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, content, lastCommit,
                        cF, cNonce, cRel, cActive, pending, pendingToken,
                        dictDone, bodyDone, materialized,
                        commitUnacked, commitDisconnected, badCommit,
                        staleObjectSeen, staleAckSeen, staleCloseSeen,
                        staleControlSeen>>

OBJECT_APPLIED(o) ==
    \E f \in Fs:
        /\ session = f
        /\ cF = f
        /\ pending \in TUs
        /\ pendingToken = sessionToken
        /\ needRecorded
        /\ o \in requested
        /\ content[f][o] \in {NoContent, CanonicalContent(o)}
        /\ content' = [content EXCEPT ![f][o] = CanonicalContent(o)]
        /\ missing' = missing \ {o}
        /\ pinned' = pinned \cup {o}
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, lastCommit,
                        cF, cNonce, cRel, cActive, pending, pendingToken,
                        dictDone, needRecorded, requested,
                        bodyDone, materialized,
                        commitUnacked, commitDisconnected, badCommit,
                        staleObjectSeen, staleAckSeen, staleCloseSeen,
                        staleControlSeen>>

STALE_OBJECT_CALLBACK(f, tok, o) ==
    /\ f \in Fs
    /\ tok \in usedTokens[f]
    /\ o \in Objects
    /\ ~CurrentSession(f, tok)
    /\ staleObjectSeen' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleAckSeen, staleCloseSeen, staleControlSeen>>

EVICT_OBJECT(f, o) ==
    /\ f \in Fs
    /\ o \in Objects
    /\ content[f][o] # NoContent
    /\ IF pending # NoTU /\ cF = f THEN o \notin pinned ELSE TRUE
    /\ content' = [content EXCEPT ![f][o] = NoContent]
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen,
                    staleControlSeen>>

BODY_COMPLETE ==
    /\ pending \in TUs
    /\ session = cF
    /\ pendingToken = sessionToken
    /\ ~bodyDone
    /\ bodyDone' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen,
                    staleControlSeen>>

INPUT_MATERIALIZED ==
    /\ pending \in TUs
    /\ session = cF
    /\ pendingToken = sessionToken
    /\ dictDone
    /\ needRecorded
    /\ missing = {}
    /\ pinned = TuObjects(pending)
    /\ bodyDone
    /\ ~materialized
    /\ materialized' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, pending, pendingToken,
                    dictDone, needRecorded, requested, missing, pinned,
                    bodyDone, commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleAckSeen, staleCloseSeen,
                    staleControlSeen>>

INPUT_COMMITTED ==
    \E f \in Fs, t \in TUs:
        /\ session = f
        /\ cF = f
        /\ pending = t
        /\ pendingToken = sessionToken
        /\ cActive = t
        /\ materialized
        /\ pinned = TuObjects(t)
        /\ fRel[f] = cRel
        /\ cRel < MaxRel
        /\ fRel' = [fRel EXCEPT ![f] = @ + 1]
        /\ lastCommit' = [lastCommit EXCEPT ![f] = t]
        /\ badCommit' = badCommit \/
                         ~(dictDone /\ needRecorded /\ missing = {} /\
                           pinned = TuObjects(t) /\ bodyDone)
        /\ ClearFOverlay
        /\ commitUnacked' = TRUE
        /\ commitDisconnected' = FALSE
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, content, cF, cNonce, cRel, cActive,
                        staleObjectSeen, staleAckSeen, staleCloseSeen,
                        staleControlSeen>>

AcceptCommit(f, tok) ==
    /\ f \in Fs
    /\ tok \in Tokens
    /\ CurrentSession(f, tok)
    /\ cF = f
    /\ cActive \in TUs
    /\ lastCommit[f] = cActive
    /\ fRel[f] = cRel + 1
    /\ commitUnacked
    /\ cRel' = cRel + 1
    /\ cActive' = NoTU
    /\ commitUnacked' = FALSE
    /\ commitDisconnected' = FALSE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    badCommit, staleObjectSeen, staleAckSeen,
                    staleCloseSeen, staleControlSeen>>

COMMIT_ACCEPTED(f, tok) ==
    /\ ~commitDisconnected
    /\ AcceptCommit(f, tok)

LOST_COMMIT_ACCEPTED(f, tok) ==
    /\ commitDisconnected
    /\ AcceptCommit(f, tok)

STALE_ACK_CALLBACK(f, tok) ==
    /\ f \in Fs
    /\ tok \in usedTokens[f]
    /\ ~CurrentSession(f, tok)
    /\ staleAckSeen' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive,
                    pending, pendingToken, dictDone, needRecorded,
                    requested, missing, pinned, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit,
                    staleObjectSeen, staleCloseSeen, staleControlSeen>>

Next ==
    \/ \E f \in Fs, tok \in Tokens : SESSION_OPENED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : SESSION_REPLACED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : SESSION_DISCONNECTED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : STALE_CLOSE_CALLBACK(f, tok)
    \/ \E f \in Fs, tok \in Tokens : HISTORY_RESET(f, tok)
    \/ \E f \in Fs, tok \in Tokens : STALE_CONTROL_CALLBACK(f, tok)
    \/ \E actor \in Actors : TX_BEGIN(actor)
    \/ ACTIVE_REPLAYED
    \/ TX_ABORTED
    \/ DICT_COMPLETE
    \/ NEED_RECORDED
    \/ \E o \in Objects : OBJECT_APPLIED(o)
    \/ \E f \in Fs, tok \in Tokens, o \in Objects :
           STALE_OBJECT_CALLBACK(f, tok, o)
    \/ \E f \in Fs, o \in Objects : EVICT_OBJECT(f, o)
    \/ BODY_COMPLETE
    \/ INPUT_MATERIALIZED
    \/ INPUT_COMMITTED
    \/ \E f \in Fs, tok \in Tokens : COMMIT_ACCEPTED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : LOST_COMMIT_ACCEPTED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : STALE_ACK_CALLBACK(f, tok)

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ session \in Fs \cup {NoF}
    /\ sessionToken \in Tokens \cup {NoToken}
    /\ usedTokens \in [Fs -> SUBSET Tokens]
    /\ route \in [Fs -> BOOLEAN]
    /\ nonce \in [Fs -> 0..1]
    /\ fRel \in [Fs -> 0..MaxRel]
    /\ content \in [Fs -> [Objects -> Values \cup {NoContent}]]
    /\ lastCommit \in [Fs -> TUs \cup {NoTU}]
    /\ cF \in Fs \cup {NoF}
    /\ cNonce \in 0..1
    /\ cRel \in 0..MaxRel
    /\ cActive \in TUs \cup {NoTU}
    /\ pending \in TUs \cup {NoTU}
    /\ pendingToken \in Tokens \cup {NoToken}
    /\ dictDone \in BOOLEAN
    /\ needRecorded \in BOOLEAN
    /\ requested \subseteq Objects
    /\ missing \subseteq Objects
    /\ pinned \subseteq Objects
    /\ bodyDone \in BOOLEAN
    /\ materialized \in BOOLEAN
    /\ commitUnacked \in BOOLEAN
    /\ commitDisconnected \in BOOLEAN
    /\ badCommit \in BOOLEAN
    /\ staleObjectSeen \in BOOLEAN
    /\ staleAckSeen \in BOOLEAN
    /\ staleCloseSeen \in BOOLEAN
    /\ staleControlSeen \in BOOLEAN

OneActive == pending = NoTU \/ pending = cActive

SessionFence ==
    pending = NoTU \/
        /\ session = cF
        /\ pendingToken = sessionToken
        /\ sessionToken \in usedTokens[session]

InstalledContentExact ==
    \A f \in Fs, o \in Objects :
        content[f][o] = NoContent \/ content[f][o] = CanonicalContent(o)

NeedIsExact ==
    IF pending \in TUs /\ needRecorded
    THEN /\ cF \in Fs
         /\ requested \subseteq TuObjects(pending)
         /\ missing = requested \ Present(content[cF])
    ELSE /\ requested = {}
         /\ missing = {}

PinnedObjectsPresent ==
    IF pending = NoTU
    THEN pinned = {}
    ELSE /\ cF \in Fs
         /\ pinned \subseteq Present(content[cF])

PinsAreExact ==
    IF pending \in TUs /\ needRecorded
    THEN pinned = TuObjects(pending) \ missing
    ELSE pinned = {}

CommitOnlyAfterExactMaterialization == badCommit = FALSE

AtMostOneAhead ==
    IF cF \in Fs
    THEN ~route[cF] \/ fRel[cF] = cRel \/ fRel[cF] = cRel + 1
    ELSE TRUE

=============================================================================
