------------------------- MODULE Protocol50Reconnect -------------------------
EXTENDS Naturals, TLC

(***************************************************************************
Small reconnect-decision model for the boundary deliberately excluded from
Protocol50.tla's cache transaction state space.

It distinguishes:

  * initial cold establishment;
  * exact replay;
  * a witnessed lost final acknowledgement;
  * verified F_STORE_GUID replacement;
  * same-GUID namespace disappearance;
  * an unresolved same-incarnation route mismatch.

The last two are fail-closed.  They do not authorize generic history reset
while C retains active or possibly durable work.  One HISTORY_RESET is allowed
per reconciled session; further reset need closes/re-enters reconciliation.
***************************************************************************)

CONSTANTS MutantSameGuidMissingCold,
          MutantResetActiveMismatch,
          MutantSecondResetSameSession

ASSUME /\ MutantSameGuidMissingCold \in BOOLEAN
       /\ MutantResetActiveMismatch \in BOOLEAN
       /\ MutantSecondResetSameSession \in BOOLEAN

Reports == {"Missing", "Exact", "LostAck", "Mismatch"}
Phases ==
    {"Unclassified", "InitialCold", "IncarnationCold", "AcceptLostAck",
     "Replay", "ResetIdle", "NeedReissue", "Error", "Done"}

ScenarioRecords ==
    [everEstablished : BOOLEAN,
     report          : Reports,
     guidChanged     : BOOLEAN,
     active          : BOOLEAN,
     durable         : BOOLEAN]

Scenarios ==
    {q \in ScenarioRecords :
        /\ (q.durable => q.active)
        /\ (~q.everEstablished =>
              /\ ~q.guidChanged
              /\ q.report = "Missing"
              /\ ~q.durable)
        /\ (q.guidChanged =>
              /\ q.everEstablished
              /\ q.report = "Missing")
        /\ (q.report = "LostAck" =>
              /\ q.everEstablished
              /\ ~q.guidChanged
              /\ q.durable)
        /\ (q.report = "Exact" => ~q.durable)}

VARIABLE s
vars == <<s>>

Init ==
    \E q \in Scenarios :
        s = [everEstablished        |-> q.everEstablished,
             report                 |-> q.report,
             guidChanged            |-> q.guidChanged,
             initialActive          |-> q.active,
             initialDurable         |-> q.durable,
             active                 |-> q.active,
             durable                |-> q.durable,
             retryRetained          |-> FALSE,
             coldSelected           |-> FALSE,
             resetCount             |-> 0,
             acceptedLostAck        |-> FALSE,
             replayed               |-> FALSE,
             reissued               |-> FALSE,
             errored                |-> FALSE,
             badColdRetirement      |-> FALSE,
             badActiveReset         |-> FALSE,
             badSecondReset         |-> FALSE,
             phase                  |-> "Unclassified"]

Classify ==
    LET nextPhase ==
        IF ~s.everEstablished
        THEN "InitialCold"
        ELSE IF s.guidChanged
             THEN "IncarnationCold"
             ELSE IF s.report = "Missing"
                  THEN IF MutantSameGuidMissingCold
                       THEN "IncarnationCold"
                       ELSE "Error"
                  ELSE IF s.report = "LostAck"
                       THEN "AcceptLostAck"
                       ELSE IF s.report = "Exact"
                            THEN IF s.active THEN "Replay" ELSE "Done"
                            ELSE IF s.active
                                 THEN IF MutantResetActiveMismatch
                                      THEN "ResetIdle"
                                      ELSE "Error"
                                 ELSE "ResetIdle"
        cold == nextPhase \in {"InitialCold", "IncarnationCold"}
        unprovedCold ==
            /\ s.everEstablished
            /\ ~s.guidChanged
            /\ s.report = "Missing"
            /\ cold
    IN /\ s.phase = "Unclassified"
       /\ s' = [s EXCEPT
                    !.phase = nextPhase,
                    !.coldSelected = cold,
                    !.retryRetained = cold /\ s.active,
                    !.badColdRetirement =
                        s.badColdRetirement \/ unprovedCold]

ResolveLostAck ==
    /\ s.phase = "AcceptLostAck"
    /\ s.active
    /\ s.durable
    /\ s.report = "LostAck"
    /\ ~s.guidChanged
    /\ s' = [s EXCEPT
                 !.active = FALSE,
                 !.durable = FALSE,
                 !.acceptedLostAck = TRUE,
                 !.phase = "Done"]

ReplayExact ==
    /\ s.phase = "Replay"
    /\ s.active
    /\ ~s.durable
    /\ s' = [s EXCEPT
                 !.replayed = TRUE,
                 !.phase = "Done"]

ResetRoute ==
    LET discardsActive == s.phase = "ResetIdle" /\ s.active
        coldNeedsRetry ==
            s.phase \in {"InitialCold", "IncarnationCold"} /\ s.active
    IN /\ s.phase \in {"InitialCold", "IncarnationCold", "ResetIdle"}
       /\ s.resetCount = 0
       /\ s' = [s EXCEPT
                    !.active = IF discardsActive THEN FALSE ELSE s.active,
                    !.durable =
                        IF s.phase = "IncarnationCold" \/ discardsActive
                        THEN FALSE ELSE s.durable,
                    !.resetCount = 1,
                    !.badActiveReset =
                        s.badActiveReset \/ discardsActive,
                    !.phase = IF coldNeedsRetry
                              THEN "NeedReissue" ELSE "Done"]

ReissueCold ==
    /\ s.phase = "NeedReissue"
    /\ s.coldSelected
    /\ s.retryRetained
    /\ s.active
    /\ ~s.durable
    /\ s' = [s EXCEPT
                 !.reissued = TRUE,
                 !.phase = "Done"]

FinishError ==
    /\ s.phase = "Error"
    /\ s' = [s EXCEPT
                 !.errored = TRUE,
                 !.phase = "Done"]

SecondHistoryReset ==
    /\ MutantSecondResetSameSession
    /\ s.phase = "Done"
    /\ s.resetCount = 1
    /\ s' = [s EXCEPT
                 !.resetCount = 2,
                 !.badSecondReset = TRUE]

Next ==
    \/ Classify
    \/ ResolveLostAck
    \/ ReplayExact
    \/ ResetRoute
    \/ ReissueCold
    \/ FinishError
    \/ SecondHistoryReset

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ s.everEstablished \in BOOLEAN
    /\ s.report \in Reports
    /\ s.guidChanged \in BOOLEAN
    /\ s.initialActive \in BOOLEAN
    /\ s.initialDurable \in BOOLEAN
    /\ s.active \in BOOLEAN
    /\ s.durable \in BOOLEAN
    /\ s.retryRetained \in BOOLEAN
    /\ s.coldSelected \in BOOLEAN
    /\ s.resetCount \in 0..2
    /\ s.acceptedLostAck \in BOOLEAN
    /\ s.replayed \in BOOLEAN
    /\ s.reissued \in BOOLEAN
    /\ s.errored \in BOOLEAN
    /\ s.badColdRetirement \in BOOLEAN
    /\ s.badActiveReset \in BOOLEAN
    /\ s.badSecondReset \in BOOLEAN
    /\ s.phase \in Phases

ColdRetirementHasProof ==
    ~s.badColdRetirement

UnresolvedActiveNotDiscarded ==
    ~s.badActiveReset

AtMostOneResetPerSession ==
    /\ ~s.badSecondReset
    /\ s.resetCount <= 1

LostAckAcceptanceHasWitness ==
    s.acceptedLostAck =>
        /\ s.initialActive
        /\ s.initialDurable
        /\ s.report = "LostAck"
        /\ ~s.guidChanged

UnverifiedDurableStateRetained ==
    (~s.guidChanged /\ s.initialDurable /\
     s.report # "LostAck" /\ ~s.acceptedLostAck) =>
        /\ s.active
        /\ s.durable

ColdRetryPreserved ==
    s.coldSelected /\ s.initialActive =>
        /\ s.retryRetained
        /\ s.active

ErrorRetainsUnresolvedActive ==
    s.errored /\ s.initialActive => s.active

=============================================================================
