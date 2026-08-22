--------------------- MODULE Protocol50AssignmentIdentity ---------------------
EXTENDS TLC

(***************************************************************************
Focused end-to-end assignment-identity refinement for the existing
Scheduler -> Client daemon -> Fulfillment daemon path.

The established job_id is the assignment wire_id.  Protocol 50 adds the
scheduler epoch and assignment nonce to UseCS and CompileFile.  Protocols
43, 48, and 49 retain job_id but carry neither added component.

The model deliberately separates:

  * an exact P50 identity:       (epoch, wire_id, nonce)
  * whole legacy compatibility: (absent, wire_id, absent)
  * an invalid partial identity: exactly one added component is absent

ENFORCING_COMPAT accepts the first two shapes.  STRICT_NONCE is a P50-only
operator promise and accepts only the exact full identity.  A local scheduler
decision is exempt because it creates no remote worker assignment.

The final transitions are a small correspondence lemma for the accepted
Protocol50AssignmentDelivery model: pre-claim Revoked releases immediately;
ClaimedOrLater retains the assignment until ordinary settlement.
***************************************************************************)

CONSTANTS Scenario, LocalDecision

Protocols == {"P43", "P48", "P49", "P50"}
LegacyProtocols == {"P43", "P48", "P49"}
Modes == {"EnforcingCompat", "StrictNonce"}
Scenarios == {
    "Normal",
    "StaleEpoch", "StaleWire", "StaleNonce",
    "EpochOnly", "NonceOnly",
    "DropP50Identity", "StrictAbsent",
    "DropOnReconnect", "ReleaseClaimed"
}

ASSUME /\ Scenario \in Scenarios
       /\ LocalDecision \in BOOLEAN

NoComponent == "NoComponent"
Epoch0 == "Epoch0"
Epoch1 == "Epoch1"
Wire0 == "Wire0"
Wire1 == "Wire1"
Nonce0 == "Nonce0"
Nonce1 == "Nonce1"

Identity(e, w, n) == [epoch |-> e, wire |-> w, nonce |-> n]

NoEnvelope == Identity(NoComponent, NoComponent, NoComponent)
LegacyEnvelope == Identity(NoComponent, Wire0, NoComponent)
ExactIdentity == Identity(Epoch0, Wire0, Nonce0)
StaleEpochIdentity == Identity(Epoch1, Wire0, Nonce0)
StaleWireIdentity == Identity(Epoch0, Wire1, Nonce0)
StaleNonceIdentity == Identity(Epoch0, Wire0, Nonce1)
EpochOnlyIdentity == Identity(Epoch0, Wire0, NoComponent)
NonceOnlyIdentity == Identity(NoComponent, Wire0, Nonce0)

Identities == {
    NoEnvelope, LegacyEnvelope, ExactIdentity,
    StaleEpochIdentity, StaleWireIdentity, StaleNonceIdentity,
    EpochOnlyIdentity, NonceOnlyIdentity
}

FullIdentity(i) ==
    /\ i.epoch # NoComponent
    /\ i.wire # NoComponent
    /\ i.nonce # NoComponent

WholeLegacyIdentity(i) == i = LegacyEnvelope

PartialIdentity(i) ==
    /\ i.wire # NoComponent
    /\ ((i.epoch = NoComponent) # (i.nonce = NoComponent))

StaleIdentity(i) == FullIdentity(i) /\ i # ExactIdentity

VARIABLES schedulerProtocol, clientProtocol, workerProtocol, mode,
          phase, clientIdentity, workerIdentity,
          usecsSent, compileFileSent, clientReconnected,
          admission, revokeResult, released, ordinarySettled

AllP50 ==
    /\ schedulerProtocol = "P50"
    /\ clientProtocol = "P50"
    /\ workerProtocol = "P50"

StrictConfigurationAvailable ==
    /\ schedulerProtocol = "P50"
    /\ clientProtocol = "P50"
    /\ (LocalDecision \/ workerProtocol = "P50")

RemoteClaimAccepted == admission \in {"Exact", "LegacyCompat", "Invalid"}

vars == <<schedulerProtocol, clientProtocol, workerProtocol, mode,
          phase, clientIdentity, workerIdentity,
          usecsSent, compileFileSent, clientReconnected,
          admission, revokeResult, released, ordinarySettled>>

Init ==
    /\ schedulerProtocol \in Protocols
    /\ clientProtocol \in Protocols
    /\ workerProtocol \in Protocols
    /\ mode \in Modes
    /\ phase = "Start"
    /\ clientIdentity = NoEnvelope
    /\ workerIdentity = NoEnvelope
    /\ usecsSent = FALSE
    /\ compileFileSent = FALSE
    /\ clientReconnected = FALSE
    /\ admission = "None"
    /\ revokeResult = "None"
    /\ released = FALSE
    /\ ordinarySettled = FALSE

Configure ==
    /\ phase = "Start"
    /\ phase' =
           IF mode = "StrictNonce" /\ ~StrictConfigurationAvailable
           THEN "Refused"
           ELSE "Configured"
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    admission, revokeResult, released, ordinarySettled>>

RouteLocal ==
    /\ phase = "Configured"
    /\ LocalDecision
    /\ phase' = "LocalComplete"
    /\ admission' = "LocalExempt"
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    revokeResult, released, ordinarySettled>>

UseCSIdentity ==
    IF schedulerProtocol = "P50" /\ clientProtocol = "P50"
    THEN ExactIdentity
    ELSE LegacyEnvelope

PublishUseCS ==
    LET normal == UseCSIdentity
        sent == IF Scenario \in {"DropP50Identity", "StrictAbsent"}
                        /\ normal = ExactIdentity
                THEN LegacyEnvelope
                ELSE normal
    IN /\ phase = "Configured"
       /\ ~LocalDecision
       /\ phase' = "UseCSPublished"
       /\ clientIdentity' = sent
       /\ usecsSent' = TRUE
       /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                       workerIdentity, compileFileSent, clientReconnected,
                       admission, revokeResult, released, ordinarySettled>>

ReconnectClient ==
    /\ phase = "UseCSPublished"
    /\ ~clientReconnected
    /\ clientReconnected' = TRUE
    /\ clientIdentity' =
           IF Scenario = "DropOnReconnect"
           THEN LegacyEnvelope
           ELSE clientIdentity
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    phase, workerIdentity, usecsSent, compileFileSent,
                    admission, revokeResult, released, ordinarySettled>>

RelayedIdentity ==
    IF clientProtocol = "P50" /\ workerProtocol = "P50"
            /\ clientIdentity = ExactIdentity
    THEN ExactIdentity
    ELSE LegacyEnvelope

ScenarioIdentity(i) ==
    IF Scenario = "StaleEpoch" THEN StaleEpochIdentity
    ELSE IF Scenario = "StaleWire" THEN StaleWireIdentity
    ELSE IF Scenario = "StaleNonce" THEN StaleNonceIdentity
    ELSE IF Scenario = "EpochOnly" THEN EpochOnlyIdentity
    ELSE IF Scenario = "NonceOnly" THEN NonceOnlyIdentity
    ELSE i

RelayCompileFile ==
    LET sent == ScenarioIdentity(RelayedIdentity)
    IN /\ phase = "UseCSPublished"
       /\ ~LocalDecision
       /\ phase' = "ClaimPresented"
       /\ workerIdentity' = sent
       /\ compileFileSent' = TRUE
       /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                       clientIdentity, usecsSent, clientReconnected,
                       admission, revokeResult, released, ordinarySettled>>

InvalidAcceptanceScenario ==
    Scenario \in {"StaleEpoch", "StaleWire", "StaleNonce",
                   "EpochOnly", "NonceOnly"}

NormallyAcceptable(i) ==
    \/ i = ExactIdentity
    \/ /\ mode = "EnforcingCompat"
          /\ WholeLegacyIdentity(i)

MutantAcceptable(i) ==
    \/ /\ InvalidAcceptanceScenario
          /\ (StaleIdentity(i) \/ PartialIdentity(i))
    \/ /\ Scenario = "StrictAbsent"
          /\ mode = "StrictNonce"
          /\ WholeLegacyIdentity(i)

ClaimAcceptable(i) == NormallyAcceptable(i) \/ MutantAcceptable(i)

AdmissionOf(i) ==
    IF i = ExactIdentity THEN "Exact"
    ELSE IF WholeLegacyIdentity(i) THEN "LegacyCompat"
    ELSE "Invalid"

AcceptRemoteClaim ==
    /\ phase = "ClaimPresented"
    /\ ClaimAcceptable(workerIdentity)
    /\ phase' = "Claimed"
    /\ admission' = AdmissionOf(workerIdentity)
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    revokeResult, released, ordinarySettled>>

RejectRemoteClaim ==
    /\ phase = "ClaimPresented"
    /\ ~ClaimAcceptable(workerIdentity)
    /\ phase' = "Rejected"
    /\ admission' = "Rejected"
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    revokeResult, released, ordinarySettled>>

RequestRevokeBeforeClaim ==
    /\ phase \in {"Configured", "UseCSPublished", "ClaimPresented", "Rejected"}
    /\ ~LocalDecision
    /\ phase' = "RevokedPending"
    /\ revokeResult' = "Revoked"
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    admission, released, ordinarySettled>>

DeliverRevokedResult ==
    /\ phase = "RevokedPending"
    /\ revokeResult = "Revoked"
    /\ phase' = "Released"
    /\ released' = TRUE
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    admission, revokeResult, ordinarySettled>>

RequestRevokeClaimed ==
    /\ phase = "Claimed"
    /\ phase' = "ClaimedRevokePending"
    /\ revokeResult' = "ClaimedOrLater"
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    admission, released, ordinarySettled>>

DeliverClaimedResult ==
    /\ phase = "ClaimedRevokePending"
    /\ revokeResult = "ClaimedOrLater"
    /\ phase' = "Retained"
    /\ released' = (released \/ Scenario = "ReleaseClaimed")
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    admission, revokeResult, ordinarySettled>>

OrdinarySettle ==
    /\ phase = "Retained"
    /\ revokeResult = "ClaimedOrLater"
    /\ phase' = "Settled"
    /\ released' = TRUE
    /\ ordinarySettled' = TRUE
    /\ UNCHANGED <<schedulerProtocol, clientProtocol, workerProtocol, mode,
                    clientIdentity, workerIdentity,
                    usecsSent, compileFileSent, clientReconnected,
                    admission, revokeResult>>

Next ==
    \/ Configure
    \/ RouteLocal
    \/ PublishUseCS
    \/ ReconnectClient
    \/ RelayCompileFile
    \/ AcceptRemoteClaim
    \/ RejectRemoteClaim
    \/ RequestRevokeBeforeClaim
    \/ DeliverRevokedResult
    \/ RequestRevokeClaimed
    \/ DeliverClaimedResult
    \/ OrdinarySettle

Spec == Init /\ [][Next]_vars

Phases == {
    "Start", "Configured", "Refused", "LocalComplete",
    "UseCSPublished", "ClaimPresented", "Claimed", "Rejected",
    "RevokedPending", "ClaimedRevokePending", "Retained",
    "Released", "Settled"
}
Admissions == {"None", "Exact", "LegacyCompat", "Invalid",
                "Rejected", "LocalExempt"}
RevokeResults == {"None", "Revoked", "ClaimedOrLater"}

TypeOK ==
    /\ schedulerProtocol \in Protocols
    /\ clientProtocol \in Protocols
    /\ workerProtocol \in Protocols
    /\ mode \in Modes
    /\ phase \in Phases
    /\ clientIdentity \in Identities
    /\ workerIdentity \in Identities
    /\ usecsSent \in BOOLEAN
    /\ compileFileSent \in BOOLEAN
    /\ clientReconnected \in BOOLEAN
    /\ admission \in Admissions
    /\ revokeResult \in RevokeResults
    /\ released \in BOOLEAN
    /\ ordinarySettled \in BOOLEAN

ConfigurationModeSafety ==
    phase \notin {"Start", "Refused"} /\ mode = "StrictNonce" =>
        StrictConfigurationAvailable

P50PathPreservesIdentity ==
    AllP50 /\ ~LocalDecision =>
        /\ (usecsSent => clientIdentity = ExactIdentity)
        /\ (compileFileSent => workerIdentity = ExactIdentity)

LegacyHopsOmitIdentity ==
    /\ (usecsSent /\
         (schedulerProtocol \in LegacyProtocols \/
          clientProtocol \in LegacyProtocols) =>
            WholeLegacyIdentity(clientIdentity))
    /\ (compileFileSent /\
         (clientProtocol \in LegacyProtocols \/
          workerProtocol \in LegacyProtocols \/
          WholeLegacyIdentity(clientIdentity)) =>
            WholeLegacyIdentity(workerIdentity))

AcceptedIdentityShape ==
    RemoteClaimAccepted =>
        \/ /\ admission = "Exact"
              /\ workerIdentity = ExactIdentity
        \/ /\ admission = "LegacyCompat"
              /\ mode = "EnforcingCompat"
              /\ WholeLegacyIdentity(workerIdentity)

StaleIdentityRejected ==
    RemoteClaimAccepted => ~StaleIdentity(workerIdentity)

PartialIdentityRejected ==
    RemoteClaimAccepted => ~PartialIdentity(workerIdentity)

StrictClaimsExact ==
    mode = "StrictNonce" /\ RemoteClaimAccepted =>
        /\ admission = "Exact"
        /\ workerIdentity = ExactIdentity
        /\ AllP50

EnforcingCompatClaimsExactOrWholeLegacy ==
    mode = "EnforcingCompat" /\ RemoteClaimAccepted =>
        \/ workerIdentity = ExactIdentity
        \/ WholeLegacyIdentity(workerIdentity)

LocalExemptionHasNoRemoteAssignment ==
    phase = "LocalComplete" =>
        /\ LocalDecision
        /\ admission = "LocalExempt"
        /\ ~usecsSent
        /\ ~compileFileSent
        /\ clientIdentity = NoEnvelope
        /\ workerIdentity = NoEnvelope
        /\ revokeResult = "None"
        /\ ~released

ReconnectPreservesIdentity ==
    clientReconnected /\ AllP50 => clientIdentity = ExactIdentity

ReleaseHasMatchingSettlement ==
    released =>
        \/ revokeResult = "Revoked"
        \/ ordinarySettled

ClaimedResultRetains ==
    revokeResult = "ClaimedOrLater" /\ ~ordinarySettled => ~released

OrdinarySettlementHasClaim ==
    ordinarySettled =>
        /\ revokeResult = "ClaimedOrLater"
        /\ admission \in {"Exact", "LegacyCompat"}

Safety ==
    /\ TypeOK
    /\ ConfigurationModeSafety
    /\ P50PathPreservesIdentity
    /\ LegacyHopsOmitIdentity
    /\ AcceptedIdentityShape
    /\ StaleIdentityRejected
    /\ PartialIdentityRejected
    /\ StrictClaimsExact
    /\ EnforcingCompatClaimsExactOrWholeLegacy
    /\ LocalExemptionHasNoRemoteAssignment
    /\ ReconnectPreservesIdentity
    /\ ReleaseHasMatchingSettlement
    /\ ClaimedResultRetains
    /\ OrdinarySettlementHasClaim

(***************************************************************************
State constraints for the explicit P43/P50 compatibility cells.
***************************************************************************)
CellP43P43P43 ==
    schedulerProtocol = "P43" /\ clientProtocol = "P43" /\
    workerProtocol = "P43"
CellP43P43P50 ==
    schedulerProtocol = "P43" /\ clientProtocol = "P43" /\
    workerProtocol = "P50"
CellP43P50P43 ==
    schedulerProtocol = "P43" /\ clientProtocol = "P50" /\
    workerProtocol = "P43"
CellP43P50P50 ==
    schedulerProtocol = "P43" /\ clientProtocol = "P50" /\
    workerProtocol = "P50"
CellP50P43P43 ==
    schedulerProtocol = "P50" /\ clientProtocol = "P43" /\
    workerProtocol = "P43"
CellP50P43P50 ==
    schedulerProtocol = "P50" /\ clientProtocol = "P43" /\
    workerProtocol = "P50"
CellP50P50P43 ==
    schedulerProtocol = "P50" /\ clientProtocol = "P50" /\
    workerProtocol = "P43"
CellP50P50P50 ==
    schedulerProtocol = "P50" /\ clientProtocol = "P50" /\
    workerProtocol = "P50"

EnforcingOnly == mode = "EnforcingCompat"
StrictOnly == mode = "StrictNonce"

(***************************************************************************
Deletion-sensitive reachability predicates.  Each corresponding config
checks the negation and must fail only after the intended state is reached.
***************************************************************************)
ExpectedCellOutcomeReached ==
    /\ phase = "Claimed"
    /\ IF AllP50
          THEN admission = "Exact" /\ workerIdentity = ExactIdentity
          ELSE admission = "LegacyCompat" /\
               WholeLegacyIdentity(workerIdentity)

StrictExactClaimReached ==
    /\ mode = "StrictNonce"
    /\ AllP50
    /\ phase = "Claimed"
    /\ admission = "Exact"
    /\ workerIdentity = ExactIdentity

StrictMixedRefusalReached ==
    /\ mode = "StrictNonce"
    /\ ~LocalDecision
    /\ ~AllP50
    /\ phase = "Refused"

LocalExemptionReached ==
    /\ LocalDecision
    /\ phase = "LocalComplete"
    /\ admission = "LocalExempt"
    /\ clientIdentity = NoEnvelope
    /\ workerIdentity = NoEnvelope

ReconnectExactClaimReached ==
    /\ clientReconnected
    /\ AllP50
    /\ phase = "Claimed"
    /\ admission = "Exact"
    /\ workerIdentity = ExactIdentity

RevokedReleaseReached ==
    /\ phase = "Released"
    /\ revokeResult = "Revoked"
    /\ released
    /\ ~RemoteClaimAccepted

ClaimedOrdinarySettlementReached ==
    /\ phase = "Settled"
    /\ revokeResult = "ClaimedOrLater"
    /\ ordinarySettled
    /\ released
    /\ RemoteClaimAccepted

NoExpectedCellOutcome == ~ExpectedCellOutcomeReached
NoStrictExactClaimWitness == ~StrictExactClaimReached
NoStrictMixedRefusalWitness == ~StrictMixedRefusalReached
NoLocalExemptionWitness == ~LocalExemptionReached
NoReconnectExactClaimWitness == ~ReconnectExactClaimReached
NoRevokedReleaseWitness == ~RevokedReleaseReached
NoClaimedOrdinarySettlementWitness == ~ClaimedOrdinarySettlementReached

=============================================================================
