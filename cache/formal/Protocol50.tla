------------------------------ MODULE Protocol50 ------------------------------
EXTENDS Naturals, FiniteSets, TLC

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
          bodyDone, materialized,
          commitUnacked, commitDisconnected, badCommit, staleSeen

vars == <<session, sessionToken, usedTokens,
          route, nonce, fRel, content, lastCommit,
          cF, cNonce, cRel, cActive,
          pending, pendingToken, dictDone, needRecorded, requested, missing,
          bodyDone, materialized,
          commitUnacked, commitDisconnected, badCommit, staleSeen>>

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
    /\ bodyDone = FALSE
    /\ materialized = FALSE
    /\ commitUnacked = FALSE
    /\ commitDisconnected = FALSE
    /\ badCommit = FALSE
    /\ staleSeen = FALSE

ClearFOverlay ==
    /\ pending' = NoTU
    /\ pendingToken' = NoToken
    /\ dictDone' = FALSE
    /\ needRecorded' = FALSE
    /\ requested' = {}
    /\ missing' = {}
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
                    commitUnacked, commitDisconnected, badCommit, staleSeen>>

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
                    badCommit, staleSeen>>

SESSION_DISCONNECTED ==
    /\ session # NoF
    /\ session' = NoF
    /\ sessionToken' = NoToken
    /\ ClearFOverlay
    /\ commitDisconnected' = (commitDisconnected \/ commitUnacked)
    /\ UNCHANGED <<usedTokens, route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, commitUnacked,
                    badCommit, staleSeen>>

HISTORY_RESET ==
    \E f \in Fs:
        /\ session = f
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
                        requested, missing, bodyDone, materialized,
                        commitUnacked, badCommit, staleSeen>>

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
                        cF, cNonce, cRel, pending, pendingToken,
                        dictDone, needRecorded, requested, missing,
                        bodyDone, materialized, commitUnacked,
                        commitDisconnected, badCommit, staleSeen>>

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
        /\ bodyDone' = FALSE
        /\ materialized' = FALSE
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, content, lastCommit,
                        cF, cNonce, cRel, cActive,
                        commitUnacked, commitDisconnected, badCommit, staleSeen>>

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
                    cF, cNonce, cRel, pending, pendingToken,
                    dictDone, needRecorded, requested, missing,
                    bodyDone, materialized, commitUnacked,
                    commitDisconnected, badCommit, staleSeen>>

DICT_COMPLETE ==
    /\ pending \in TUs
    /\ ~dictDone
    /\ dictDone' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, pending, pendingToken,
                    needRecorded, requested, missing, bodyDone, materialized,
                    commitUnacked, commitDisconnected, badCommit, staleSeen>>

NEED_RECORDED ==
    \E f \in Fs, t \in TUs:
        /\ session = f
        /\ pending = t
        /\ pendingToken = sessionToken
        /\ dictDone
        /\ ~needRecorded
        /\ needRecorded' = TRUE
        /\ requested' = TuObjects(t) \ Present(content[f])
        /\ missing' = TuObjects(t) \ Present(content[f])
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, content, lastCommit,
                        cF, cNonce, cRel, cActive, pending, pendingToken,
                        dictDone, bodyDone, materialized,
                        commitUnacked, commitDisconnected, badCommit, staleSeen>>

OBJECT_APPLIED(o) ==
    \E f \in Fs:
        /\ session = f
        /\ pending \in TUs
        /\ pendingToken = sessionToken
        /\ needRecorded
        /\ o \in requested
        /\ content' = [content EXCEPT ![f][o] = CanonicalContent(o)]
        /\ missing' = missing \ {o}
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, lastCommit,
                        cF, cNonce, cRel, cActive, pending, pendingToken,
                        dictDone, needRecorded, requested, bodyDone, materialized,
                        commitUnacked, commitDisconnected, badCommit, staleSeen>>

STALE_OBJECT_CALLBACK(f, tok, o) ==
    /\ f \in Fs
    /\ tok \in usedTokens[f]
    /\ o \in Objects
    /\ \/ session # f
       \/ sessionToken # tok
    /\ staleSeen' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, pending, pendingToken,
                    dictDone, needRecorded, requested, missing,
                    bodyDone, materialized, commitUnacked,
                    commitDisconnected, badCommit>>

BODY_COMPLETE ==
    /\ pending \in TUs
    /\ ~bodyDone
    /\ bodyDone' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, pending, pendingToken,
                    dictDone, needRecorded, requested, missing, materialized,
                    commitUnacked, commitDisconnected, badCommit, staleSeen>>

INPUT_MATERIALIZED ==
    /\ pending \in TUs
    /\ dictDone /\ needRecorded /\ missing = {} /\ bodyDone
    /\ ~materialized
    /\ materialized' = TRUE
    /\ UNCHANGED <<session, sessionToken, usedTokens,
                    route, nonce, fRel, content, lastCommit,
                    cF, cNonce, cRel, cActive, pending, pendingToken,
                    dictDone, needRecorded, requested, missing, bodyDone,
                    commitUnacked, commitDisconnected, badCommit, staleSeen>>

INPUT_COMMITTED ==
    \E f \in Fs, t \in TUs:
        /\ session = f
        /\ pending = t
        /\ cActive = t
        /\ materialized
        /\ fRel[f] = cRel
        /\ cRel < MaxRel
        /\ fRel' = [fRel EXCEPT ![f] = @ + 1]
        /\ lastCommit' = [lastCommit EXCEPT ![f] = t]
        /\ badCommit' = badCommit \/
                         ~(dictDone /\ needRecorded /\ missing = {} /\ bodyDone)
        /\ ClearFOverlay
        /\ commitUnacked' = TRUE
        /\ commitDisconnected' = FALSE
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, content, cF, cNonce, cRel, cActive,
                        staleSeen>>

AcceptCommit ==
    \E f \in Fs, t \in TUs:
        /\ cF = f
        /\ cActive = t
        /\ lastCommit[f] = t
        /\ fRel[f] = cRel + 1
        /\ commitUnacked
        /\ cRel' = cRel + 1
        /\ cActive' = NoTU
        /\ commitUnacked' = FALSE
        /\ commitDisconnected' = FALSE
        /\ UNCHANGED <<session, sessionToken, usedTokens,
                        route, nonce, fRel, content, lastCommit,
                        cF, cNonce, pending, pendingToken,
                        dictDone, needRecorded, requested, missing,
                        bodyDone, materialized, badCommit, staleSeen>>

COMMIT_ACCEPTED ==
    /\ ~commitDisconnected
    /\ AcceptCommit

LOST_COMMIT_ACCEPTED ==
    /\ commitDisconnected
    /\ session = cF
    /\ AcceptCommit

Next ==
    \/ \E f \in Fs, tok \in Tokens : SESSION_OPENED(f, tok)
    \/ \E f \in Fs, tok \in Tokens : SESSION_REPLACED(f, tok)
    \/ SESSION_DISCONNECTED
    \/ HISTORY_RESET
    \/ \E actor \in Actors : TX_BEGIN(actor)
    \/ ACTIVE_REPLAYED
    \/ TX_ABORTED
    \/ DICT_COMPLETE
    \/ NEED_RECORDED
    \/ \E o \in Objects : OBJECT_APPLIED(o)
    \/ \E f \in Fs, tok \in Tokens, o \in Objects :
           STALE_OBJECT_CALLBACK(f, tok, o)
    \/ BODY_COMPLETE
    \/ INPUT_MATERIALIZED
    \/ INPUT_COMMITTED
    \/ COMMIT_ACCEPTED
    \/ LOST_COMMIT_ACCEPTED

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
    /\ requested \subseteq Objects
    /\ missing \subseteq Objects

OneActive == pending = NoTU \/ pending = cActive
SessionFence == pending = NoTU \/
                (session = cF /\ pendingToken = sessionToken /\
                 sessionToken \in usedTokens[session])
InstalledContentExact ==
    \A f \in Fs, o \in Objects :
        content[f][o] = NoContent \/ content[f][o] = CanonicalContent(o)
NeedIsExact ==
    IF pending \in TUs /\ needRecorded
    THEN /\ requested \subseteq TuObjects(pending)
         /\ missing = requested \ Present(content[session])
    ELSE requested = {} /\ missing = {}
CommitOnlyAfterExactMaterialization == badCommit = FALSE
AtMostOneAhead == IF cF \in Fs
                  THEN ~route[cF] \/ fRel[cF] = cRel \/ fRel[cF] = cRel + 1
                  ELSE TRUE

=============================================================================
