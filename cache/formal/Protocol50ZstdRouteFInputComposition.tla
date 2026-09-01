------------------------------ MODULE Protocol50ZstdRouteFInputComposition ------------------------------
(***************************************************************************
 Shared-state composition of the authoritative ZSTD_ROUTE profile core and
 the narrow executable F-input owner boundary.

 There is exactly one permit-selection action and exactly one durable commit
 action.  Profile commit and owner publication are conjoined over the same
 primed state; neither module constructs the other module's identities.
***************************************************************************)

EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    T0, T1, T2, NoTU, B0, B1,
    MaxFragments, MaxWriterTicks, MaxReaderTicks, MaxTerminalTicks,
    CodecUnits, MaxEncodedBytes, MaxRawBytes, MaxWindowBytes,
    MaxActiveRoutes, PerRouteCap, GlobalCap, MutantKind,
    NoOwner, NoPrepared, NoPermit, NoBundle

VARIABLE core, owner
vars == <<core, owner>>

Core == INSTANCE Protocol50ZstdRoute WITH s <- core
Owner == INSTANCE Protocol50ZstdRouteFInputInterface
    WITH i <- owner,
         NoOwner <- NoOwner,
         NoPrepared <- NoPrepared,
         NoPermit <- NoPermit,
         NoBundle <- NoBundle

CompositionInit ==
    /\ core = Core!InitialState
    /\ owner = Owner!InitialState

CompositionTerminal ==
    /\ core.phase = "aligned"
    /\ core.nextIndex = 3
    /\ owner.phase = "closed"
    /\ owner.stage = "committed"
    /\ owner.durable # NoBundle
    /\ Owner!CompleteDurableBundle(owner.durable)
    /\ owner.durable = owner.lastDurable
    /\ Cardinality(owner.durableBundles) = 3
    /\ core.pendingOp = Core!NoOperation
    /\ core.pendingTU = NoTU
    /\ ~core.preparedPresent
    /\ ~core.permitPresent
    /\ ~core.permitConsumed
    /\ core.cCodec = "available"
    /\ core.fCodec = "available"
    /\ core.cCodecCopies = 1
    /\ core.fCodecCopies = 1
    /\ core.cLeaseState = "none"
    /\ core.fLeaseState = "none"
    /\ core.cContextOwner = "route"
    /\ core.fContextOwner = "route"
    /\ core.cCodecJobId = Core!NoCodecJobId
    /\ core.fCodecJobId = Core!NoCodecJobId
    /\ core.outstandingCodecJobs = {}
    /\ ~core.resetRequired
    /\ ~core.commitOwed
    /\ core.liability = "none"
    /\ core.liabilityOp = Core!NoOperation
    /\ ~core.touchedFailurePending
    /\ core.settlementHighWater = 3

TerminalResidualCore ==
    /\ CompositionTerminal
    /\ Core!DuplicateTerminalObservation
    /\ UNCHANGED owner

FinalCancelledResetRequiredState ==
    /\ owner.phase = "closed"
    /\ owner.stage = "cancelled"
    /\ core.epoch = 1
    /\ core.phase = "reset-required"
    /\ core.resetRequired

FinalCancelledResetQuiescence ==
    /\ FinalCancelledResetRequiredState
    /\ UNCHANGED vars

(***************************************************************************
 Core-only transport/codec steps never change owner state.  The core's
 TransportNext deliberately excludes context loss and reset completion:
 either transition can invalidate or retire owner-retained state and is
 synchronized below.
***************************************************************************)
CoreIndependent ==
    /\ ~CompositionTerminal
    \* At the bounded final epoch, owner cancellation is already the
    \* terminal operation outcome.  Do not let an independent core turn that
    \* outcome into ClassifyFault (or any other transport step) while the
    \* route is still reset-required.  Non-final epochs retain their ordinary
    \* classify -> offer -> ack -> reset-commit path below.
    /\ ~FinalCancelledResetRequiredState
    /\ Core!TransportNext
    /\ UNCHANGED owner

ContextLossWithoutPreparedOwner ==
    /\ ~(owner.phase = "open" /\ owner.stage = "prepared")
    /\ core.phase # "aligned"
    /\ (Core!LoseCContext \/ Core!LoseFContext)
    /\ UNCHANGED owner

ContextLossAfterPreparedSync ==
    /\ core.phase = "materialized"
    /\ owner.phase = "open"
    /\ owner.stage = "prepared"
    /\ owner.owner = core.pendingOp
    /\ owner.prepared = Core!PreparedInput(core)
    /\ (Core!LoseCContext \/ Core!LoseFContext)
    /\ Owner!Cancel

ObservePreparedSync ==
    /\ core.phase = "materialized"
    /\ core.preparedPresent
    /\ owner.phase = "closed"
    /\ Owner!ObservePrepared(Core!PreparedInput(core))
    /\ UNCHANGED core

CancelAfterTouchSync ==
    /\ core.phase = "materialized"
    /\ owner.phase = "open"
    /\ owner.stage = "prepared"
    /\ owner.owner = core.pendingOp
    /\ owner.prepared = Core!PreparedInput(core)
    /\ Core!OwnerCancelAfterTouch
    /\ Owner!Cancel

SelectCommitSync ==
    LET exactPermit == Core!PermitId(core.pendingOp, core.preparedId)
    IN /\ core.phase = "materialized"
       /\ owner.phase = "open"
       /\ owner.stage = "prepared"
       /\ owner.owner = core.pendingOp
       /\ owner.prepared = Core!PreparedInput(core)
       /\ Core!AcceptCommitPermit
       /\ core'.permitId = exactPermit
       /\ Owner!SelectCommit(exactPermit)

FCommitAndPublish ==
    /\ core.phase = "materialized"
    /\ owner.phase = "open"
    /\ owner.stage = "permit-issued"
    /\ owner.owner = core.pendingOp
    /\ owner.prepared = Core!PreparedInput(core)
    /\ owner.permit = core.permitId
    /\ Core!FCommit
    /\ Owner!CommitDurable(Core!DurableBundle(core'))

OwnerLateClose ==
    /\ owner.phase = "open"
    /\ owner.stage = "committed"
    /\ Owner!LateClose
    /\ UNCHANGED core

OwnerNormalClose ==
    /\ owner.phase = "open"
    /\ owner.stage = "committed"
    /\ Owner!CloseCommitted
    /\ UNCHANGED core

ResetCommitAfterOwnerCancel ==
    /\ owner.phase = "closed"
    /\ owner.stage = "cancelled"
    /\ Core!ResetCommit
    /\ Owner!RetireCancelled

ResetCommitWithoutOwner ==
    /\ owner.stage # "cancelled"
    /\ Core!ResetCommit
    /\ UNCHANGED owner

RejectStaleOwnerEvent ==
    /\ core.lastCommitPresent
    /\ core.pendingOp # Core!NoOperation
    /\ core.lastCommitOp # core.pendingOp
    /\ owner.phase = "open"
    /\ owner.owner = core.pendingOp
    /\ owner.staleEventsRejected = 0
    /\ Owner!RejectStaleObservation(
          core.lastCommitOp, Owner!CancelObservation(core.lastCommitOp))
    /\ UNCHANGED core

CompositionCoreNext ==
    CoreIndependent
    \/ ContextLossWithoutPreparedOwner
    \/ ContextLossAfterPreparedSync
    \/ ObservePreparedSync
    \/ CancelAfterTouchSync
    \/ SelectCommitSync
    \/ FCommitAndPublish
    \/ OwnerLateClose
    \/ OwnerNormalClose
    \/ ResetCommitAfterOwnerCancel
    \/ ResetCommitWithoutOwner
    \/ RejectStaleOwnerEvent
    \/ IF MutantKind = 5 THEN TerminalResidualCore ELSE FALSE

TerminalStutterWidened ==
    /\ core.phase = "c-codec-running"
    /\ UNCHANGED vars

TerminalStutter ==
    IF MutantKind = 4
    THEN TerminalStutterWidened
    ELSE
        /\ CompositionTerminal
        /\ UNCHANGED vars

TerminalFailureStutter ==
    /\ core.phase = "terminal-failure"
    /\ core.epoch = 1
    /\ core.resetRequired
    /\ core.oldRouteFenced
    /\ core.faultClosure = "terminal-failure"
    /\ UNCHANGED vars

CompositionNext ==
    CompositionCoreNext
    \/ TerminalStutter
    \/ TerminalFailureStutter
    \/ FinalCancelledResetQuiescence

CompositionBaseSpec ==
    CompositionInit
    /\ [][CompositionNext]_vars

CompositionSpec == CompositionBaseSpec

OrdinaryCommitSelectFair ==
    /\ core.phase = "materialized"
    /\ owner.stage = "prepared"
    /\ SelectCommitSync

OrdinaryCommitPublishFair ==
    /\ core.phase = "materialized"
    /\ owner.stage = "permit-issued"
    /\ FCommitAndPublish

CancelResetFair ==
    /\ owner.stage = "cancelled"
    /\ ResetCommitAfterOwnerCancel

TerminalObservationFair ==
    /\ core.phase = "f-committed"
    /\ core.terminalReady
    /\ Core!TerminalWriterTick
    /\ UNCHANGED owner

ResourceReleaseFair ==
    /\ core.lastEvent = "f-atomic-commit"
    /\ Core!CObserveCommit
    /\ UNCHANGED owner

UnrelatedOperationFair ==
    /\ core.nextIndex < 3
    /\ Core!StartTU
    /\ UNCHANGED owner

CompositionProgressSpec ==
    CompositionBaseSpec
    /\ WF_vars(OrdinaryCommitSelectFair)
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(CancelResetFair)
    /\ WF_vars(TerminalObservationFair)
    /\ WF_vars(ResourceReleaseFair)
    /\ WF_vars(UnrelatedOperationFair)

(***************************************************************************
 Progress fairness is action-specific.  The four actions below are the
 enabled protocol hand-offs named by the progress claims; the aggregate
 fairness assumption is retained only for independent transport steps.
***************************************************************************)
ProgressFairnessJustification ==
    /\ WF_vars(OrdinaryCommitSelectFair)
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(CancelResetFair)
    /\ WF_vars(TerminalObservationFair)
    /\ WF_vars(ResourceReleaseFair)
    /\ WF_vars(UnrelatedOperationFair)

FairnessDeletedOrdinaryCommitSpec ==
    CompositionBaseSpec
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(CancelResetFair)
    /\ WF_vars(TerminalObservationFair)
    /\ WF_vars(ResourceReleaseFair)
    /\ WF_vars(UnrelatedOperationFair)

FairnessDeletedCancelResetSpec ==
    CompositionBaseSpec
    /\ WF_vars(OrdinaryCommitSelectFair)
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(TerminalObservationFair)
    /\ WF_vars(ResourceReleaseFair)
    /\ WF_vars(UnrelatedOperationFair)

FairnessDeletedTerminalObservationSpec ==
    CompositionBaseSpec
    /\ WF_vars(OrdinaryCommitSelectFair)
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(CancelResetFair)
    /\ WF_vars(ResourceReleaseFair)
    /\ WF_vars(UnrelatedOperationFair)

FairnessDeletedResourceReleaseSpec ==
    CompositionBaseSpec
    /\ WF_vars(OrdinaryCommitSelectFair)
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(CancelResetFair)
    /\ WF_vars(TerminalObservationFair)
    /\ WF_vars(UnrelatedOperationFair)

FairnessDeletedUnrelatedOperationSpec ==
    CompositionBaseSpec
    /\ WF_vars(OrdinaryCommitSelectFair)
    /\ WF_vars(OrdinaryCommitPublishFair)
    /\ WF_vars(CancelResetFair)
    /\ WF_vars(TerminalObservationFair)
    /\ WF_vars(ResourceReleaseFair)

TypeOK ==
    /\ Core!TypeOK
    /\ Owner!OwnerTypeOK

PreparedSharedExact ==
    owner.phase = "open" /\ owner.stage \in {"prepared", "permit-issued"} =>
        /\ core.phase = "materialized"
        /\ core.preparedPresent
        /\ owner.owner = core.pendingOp
        /\ owner.prepared = Core!PreparedInput(core)

PermitSharedExact ==
    /\ (owner.stage = "permit-issued" <=> core.permitPresent)
    /\ (owner.stage = "permit-issued" =>
        /\ owner.permit = core.permitId
        /\ owner.permit = Core!PermitId(core.pendingOp, core.preparedId)
        /\ owner.permit \notin owner.consumedPermits
        /\ owner.permit \notin core.consumedPermits)

AtomicDurableBundle ==
    core.phase = "f-committed" =>
        /\ owner.stage = "committed"
        /\ owner.owner = core.lastCommitOp
        /\ owner.durable = Core!DurableBundle(core)
        /\ owner.durable \in owner.durableBundles
        /\ owner.durable.inputRecord.key =
              owner.durable.ready.inputRecordKey
        /\ owner.durable.ready.readyEventId > 0
        /\ owner.durable.ready.readyEventId \in owner.readyEventIds
        /\ owner.durable.permit \in owner.consumedPermits
        /\ core.inputPublished
        /\ core.lastCommitPresent
        /\ core.lastCommitOp \in core.publishedOps

DurableLedgerExact ==
    /\ Owner!DurableOperations(owner) \subseteq core.publishedOps
    /\ owner.issuedPermits = core.issuedPermits
    /\ owner.consumedPermits = core.consumedPermits
    /\ Cardinality(owner.readyEventIds) =
          Cardinality(owner.durableBundles)

CancelFirstNoDurability ==
    /\ owner.cancelledOperations \cap core.publishedOps = {}
    /\ owner.cancelledOperations \cap
          Owner!DurableOperations(owner) = {}
    /\ (owner.cancelRequested =>
        /\ owner.stage = "cancelled"
        /\ owner.owner \notin core.publishedOps
        /\ owner.owner \notin Owner!DurableOperations(owner))

TouchedCancelRequiresReset ==
    owner.stage = "cancelled" =>
        /\ core.resetRequired
        /\ core.phase \in Core!ResetPhases

CommitFirstPersists ==
    owner.lateCloseSuppressed =>
        /\ owner.stage = "committed"
        /\ owner.durable \in owner.durableBundles
        /\ owner.durable.operation \in core.publishedOps
        /\ owner.durable.ready.readyEventId \in owner.readyEventIds

NoPermitReuse ==
    /\ Owner!NoPermitReuse
    /\ Core!NoPermitReuse
    /\ owner.issuedPermits = core.issuedPermits
    /\ owner.consumedPermits = core.consumedPermits

CommitRequiresIssuedPermit ==
    /\ Core!CommitRequiresExactPermit
    /\ owner.consumedPermits \subseteq owner.issuedPermits
    /\ Owner!DurablePermits(owner) \subseteq owner.issuedPermits
    /\ owner.issuedPermits = core.issuedPermits

OwnerObservationExact ==
    owner.stage = "cancelled" =>
        owner.lastObservation = Owner!CancelObservation(owner.owner)

(***************************************************************************
 The composed state retains the core's exact profile predicates.  These are
 explicit refinement aliases rather than a weaker TypeOK approximation.
***************************************************************************)
CoreTerminalIdentityExact == Core!TerminalIdentityExact
CoreEventIdentityExact == Core!EventIdentityExact
CoreSettlementLedgerExact == Core!SettlementLedgerExact
CoreResourceBounds == Core!ResourceBounds
CorePayloadReleasedOnSettlement == Core!PayloadReleasedOnSettlement

TerminalSelfLoop == TerminalStutter

(***************************************************************************
 Terminal state safety is deliberately state-local.  In particular, the
 unrestricted base relation permits legal nonterminal stuttering, so terminal
 reachability is not asserted here.  The exact self-loop is the only action
 enabled at a terminal state in the positive relation.
***************************************************************************)
TerminalStutterEnabled ==
    CompositionTerminal => ENABLED TerminalStutter

TerminalLoopInNext ==
    CompositionTerminal => ENABLED (CompositionNext /\ TerminalStutter)

TerminalWitnessFinalState ==
    CompositionTerminal => TerminalLoopInNext

TerminalStutterGuardExact ==
    ENABLED TerminalStutter => CompositionTerminal

TerminalCoreQuiescent ==
    CompositionTerminal => ~ENABLED CompositionCoreNext

TerminalClosure ==
    /\ TerminalStutterEnabled
    /\ TerminalLoopInNext
    /\ TerminalStutterGuardExact
    /\ TerminalCoreQuiescent

(***************************************************************************
 Terminal witness properties are checked only on this progress relation.
 The driver uses the ordinary composed actions and action-local fairness;
 there is no WF on an aggregate next action and no fairness on TerminalStutter.
***************************************************************************)
TerminalWitnessStart ==
    /\ core.phase = "aligned"
    /\ core.nextIndex < 3
    /\ Core!StartTU
    /\ UNCHANGED owner

TerminalWitnessCCodecTouch ==
    /\ core.phase = "c-codec-leased"
    /\ Core!CCodecMayHaveTouched
    /\ UNCHANGED owner
TerminalWitnessCCodecReturn ==
    /\ core.phase = "c-codec-running"
    /\ Core!CCodecReturnedPrepared
    /\ UNCHANGED owner
TerminalWitnessCWriterTick ==
    /\ core.phase = "receiving"
    /\ core.wireFragment = <<>>
    /\ Core!CWriterTick
    /\ UNCHANGED owner
TerminalWitnessFReaderTick ==
    /\ core.phase = "receiving"
    /\ core.wireFragment # <<>>
    /\ Core!FReaderTick
    /\ UNCHANGED owner
TerminalWitnessBodyComplete ==
    /\ core.phase = "body-complete"
    /\ Core!CompleteBodyValidation
    /\ UNCHANGED owner
TerminalWitnessFLease ==
    /\ core.phase = "validated"
    /\ Core!IssueFCodecLease
    /\ UNCHANGED owner
TerminalWitnessFCodecTouch ==
    /\ core.phase = "f-codec-leased"
    /\ Core!FCodecMayHaveTouched
    /\ UNCHANGED owner
TerminalWitnessFCodecReturn ==
    /\ core.phase = "f-codec-running"
    /\ Core!FDecodeSuccess
    /\ UNCHANGED owner
TerminalWitnessMaterialize ==
    /\ core.phase = "decoded"
    /\ Core!Materialize
    /\ UNCHANGED owner
TerminalWitnessObserve == ObservePreparedSync
TerminalWitnessSelect == SelectCommitSync
TerminalWitnessCommit == FCommitAndPublish
TerminalWitnessClose == OwnerNormalClose
TerminalWitnessTerminalTick ==
    /\ core.phase = "f-committed"
    /\ ~core.terminalReady
    /\ Core!TerminalWriterTick
    /\ UNCHANGED owner
TerminalWitnessRelease ==
    /\ core.phase = "f-committed"
    /\ core.terminalReady
    /\ Core!CObserveCommit
    /\ UNCHANGED owner

TerminalWitnessDriverNext ==
    IF core.phase = "aligned"
       THEN TerminalWitnessStart
       ELSE IF core.phase = "c-codec-leased"
          THEN TerminalWitnessCCodecTouch
          ELSE IF core.phase = "c-codec-running"
             THEN TerminalWitnessCCodecReturn
             ELSE IF core.phase = "receiving"
                THEN IF core.wireFragment = <<>>
                     THEN TerminalWitnessCWriterTick
                     ELSE TerminalWitnessFReaderTick
                ELSE IF core.phase = "body-complete"
                   THEN TerminalWitnessBodyComplete
                   ELSE IF core.phase = "validated"
                      THEN TerminalWitnessFLease
                      ELSE IF core.phase = "f-codec-leased"
                         THEN TerminalWitnessFCodecTouch
                         ELSE IF core.phase = "f-codec-running"
                            THEN TerminalWitnessFCodecReturn
                            ELSE IF core.phase = "decoded"
                               THEN TerminalWitnessMaterialize
                               ELSE IF core.phase = "materialized"
                                  THEN IF owner.phase = "closed"
                                       THEN TerminalWitnessObserve
                                       ELSE IF owner.stage = "prepared"
                                          THEN TerminalWitnessSelect
                                          ELSE TerminalWitnessCommit
                                  ELSE IF core.phase = "f-committed"
                                     THEN IF owner.phase = "open"
                                          THEN TerminalWitnessClose
                                          ELSE IF ~core.terminalReady
                                             THEN TerminalWitnessTerminalTick
                                             ELSE TerminalWitnessRelease
                                     ELSE FALSE

(***************************************************************************
 The witness driver is a single deterministic phase-dispatch action.  Its
 action-local weak fairness is applied to that driver only; TerminalStutter
 is deliberately outside the fairness assumption.
***************************************************************************)

TerminalWitnessNext ==
    TerminalWitnessDriverNext \/ TerminalStutter

TerminalWitnessNoStutterNext ==
    TerminalWitnessDriverNext

TerminalWitnessSpec ==
    CompositionInit
    /\ [][TerminalWitnessNext]_vars
    /\ WF_vars(TerminalWitnessDriverNext)

TerminalSelfLoopSpec == TerminalWitnessSpec

TerminalWitnessReached ==
    <> CompositionTerminal

TerminalSelfLoopObserved ==
    <>[] (CompositionTerminal /\
          ENABLED (CompositionNext /\ TerminalStutter))

(***************************************************************************
 The negative terminal/deadlock controls use the same current-identity
 witness actions as the positive driver.  The named antecedents below are
 executable state expressions, so their reachability can be attributed in
 TLC coverage rather than inferred from a prose marker.
***************************************************************************)
TerminalLoopDeletionAntecedent == CompositionTerminal

TerminalGuardWideningAntecedent ==
    /\ ~CompositionTerminal
    /\ ENABLED TerminalStutter

TerminalGuardWideningWitness ==
    /\ TerminalGuardWideningAntecedent
    /\ TerminalStutterWidened

TerminalLoopDeletedNonterminalFallback ==
    /\ ~CompositionTerminal
    /\ ~ENABLED TerminalWitnessDriverNext
    /\ UNCHANGED vars

TerminalLoopDeletedNext ==
    TerminalWitnessDriverNext
    \/ TerminalLoopDeletedNonterminalFallback

TerminalLoopDeletedEnabledExact ==
    ENABLED TerminalLoopDeletedNext <=> ~TerminalLoopDeletionAntecedent

TerminalGuardWideningSpec ==
    CompositionInit
    /\ [][TerminalWitnessNoStutterNext \/ TerminalGuardWideningWitness]_vars

TerminalCoreResidualSpec ==
    CompositionInit
    /\ [][TerminalWitnessNoStutterNext \/ TerminalResidualCore]_vars

NoFairnessProgressSpec ==
    CompositionInit
    /\ [][CompositionNext]_vars

TerminalLoopDeletedSpec ==
    CompositionInit
    /\ [][TerminalLoopDeletedNext]_vars
    /\ <> CompositionTerminal

DeadlockExitDeletedDriverNext ==
    /\ ~CompositionTerminal
    /\ TerminalWitnessDriverNext

DeadlockExitDeletedNext ==
    DeadlockExitDeletedDriverNext

DeadlockExitDeletedSpec ==
    CompositionInit
    /\ [][DeadlockExitDeletedNext]_vars

OrdinaryCommitProgress ==
    (core.phase = "materialized" /\ owner.stage = "prepared")
        ~> core.phase = "f-committed"

CancelResetProgress ==
    owner.stage = "cancelled" ~> owner.stage = "receiving"

TerminalObservationProgress ==
    (core.phase = "f-committed" /\ core.terminalReady)
        ~> core.phase = "aligned"

ResourceReleaseProgress ==
    core.lastEvent = "f-atomic-commit" ~>
        (core.retainedBody = <<>> \/ core.phase # "f-committed")

UnrelatedOperationProgress ==
    core.nextIndex < 3 ~> core.nextIndex = 3

CompositionSafety ==
    /\ TypeOK
    /\ PreparedSharedExact
    /\ PermitSharedExact
    /\ AtomicDurableBundle
    /\ DurableLedgerExact
    /\ CancelFirstNoDurability
    /\ TouchedCancelRequiresReset
    /\ CommitFirstPersists
    /\ CommitRequiresIssuedPermit
    /\ NoPermitReuse
    /\ OwnerObservationExact
    /\ CoreTerminalIdentityExact
    /\ CoreEventIdentityExact
    /\ CoreSettlementLedgerExact
    /\ CoreResourceBounds
    /\ CorePayloadReleasedOnSettlement

(***************************************************************************
 Non-vacuous history-dependent witness programs.  T0 is committed through
 the shared owner action first; only then may T1 exercise cancel-first or
 commit-first arbitration at H1 -> H2.
***************************************************************************)
WitnessStart ==
    /\ Core!StartTU
    /\ core.epoch = 0
    /\ core.nextIndex <= 1
    /\ UNCHANGED owner

WitnessCoreProgress ==
    /\ ( \/ TerminalWitnessCCodecTouch
         \/ TerminalWitnessCCodecReturn
         \/ TerminalWitnessCWriterTick
         \/ TerminalWitnessFReaderTick
         \/ TerminalWitnessBodyComplete
         \/ TerminalWitnessFLease
         \/ TerminalWitnessFCodecTouch
         \/ TerminalWitnessFCodecReturn
         \/ TerminalWitnessMaterialize
         \/ TerminalWitnessTerminalTick
         \/ TerminalWitnessRelease )
    /\ UNCHANGED owner

WitnessCoreHappy == WitnessStart \/ WitnessCoreProgress

WitnessObserve == ObservePreparedSync
WitnessT0Select == SelectCommitSync /\ core.nextIndex = 0
WitnessT0Commit == FCommitAndPublish /\ core.nextIndex = 0
OwnerTUSeq == IF owner.owner = NoOwner THEN 3 ELSE owner.owner.tuSeq
WitnessT0Close ==
    /\ OwnerTUSeq = 0
    /\ OwnerNormalClose

WitnessT1Cancel ==
    /\ core.epoch = 0
    /\ core.nextIndex = 1
    /\ CancelAfterTouchSync

WitnessT1Select == SelectCommitSync /\ core.nextIndex = 1
WitnessT1Commit == FCommitAndPublish /\ core.nextIndex = 1
WitnessT1LateClose ==
    /\ OwnerTUSeq = 1
    /\ OwnerLateClose

CompositionSafetyPrefix ==
    WitnessStart
    \/ WitnessCoreProgress
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close

CompositionSafetyDriver ==
    CompositionSafetyPrefix
    \/ WitnessT1Select

CompositionCancelSafetyDriver ==
    CompositionSafetyPrefix
    \/ WitnessT1Cancel

CompositionCommitSafetyDriver ==
    CompositionSafetyPrefix
    \/ WitnessT1Select
    \/ WitnessT1Commit
    \/ WitnessT1LateClose

(***************************************************************************
 Witness fairness is expanded at the action boundary.  No aggregate next
 action, duplicate/stale/no-op branch, or TerminalStutter is made fair.
***************************************************************************)
WitnessNormalCoreFairness ==
    /\ WF_vars(WitnessStart)
    /\ WF_vars(TerminalWitnessCCodecTouch)
    /\ WF_vars(TerminalWitnessCCodecReturn)
    /\ WF_vars(TerminalWitnessCWriterTick)
    /\ WF_vars(TerminalWitnessFReaderTick)
    /\ WF_vars(TerminalWitnessBodyComplete)
    /\ WF_vars(TerminalWitnessFLease)
    /\ WF_vars(TerminalWitnessFCodecTouch)
    /\ WF_vars(TerminalWitnessFCodecReturn)
    /\ WF_vars(TerminalWitnessMaterialize)
    /\ WF_vars(TerminalWitnessTerminalTick)
    /\ WF_vars(TerminalWitnessRelease)

WitnessCancelFairness ==
    /\ WitnessNormalCoreFairness
    /\ WF_vars(WitnessObserve)
    /\ WF_vars(WitnessT0Select)
    /\ WF_vars(WitnessT0Commit)
    /\ WF_vars(WitnessT0Close)
    /\ WF_vars(WitnessT1Cancel)

WitnessCommitFairness ==
    /\ WitnessNormalCoreFairness
    /\ WF_vars(WitnessObserve)
    /\ WF_vars(WitnessT0Select)
    /\ WF_vars(WitnessT0Commit)
    /\ WF_vars(WitnessT0Close)
    /\ WF_vars(WitnessT1Select)
    /\ WF_vars(WitnessT1Commit)
    /\ WF_vars(WitnessT1LateClose)

WitnessStaleOwnerFairness ==
    /\ WitnessNormalCoreFairness
    /\ WF_vars(WitnessObserve)
    /\ WF_vars(WitnessT0Select)
    /\ WF_vars(WitnessT0Commit)
    /\ WF_vars(WitnessT0Close)
    /\ WF_vars(RejectStaleOwnerEvent)
    /\ WF_vars(WitnessT1Select)
    /\ WF_vars(WitnessT1Commit)
    /\ WF_vars(WitnessT1LateClose)

CancelWitnessNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Cancel

CancelWitnessSpec ==
    CompositionInit
    /\ [][CancelWitnessNext]_vars
    /\ WitnessCancelFairness

CancelWitnessReached ==
    <> ( /\ owner.stage = "cancelled"
         /\ core.nextIndex = 1
         /\ core.phase = "reset-required"
         /\ Cardinality(owner.durableBundles) = 1 )

WitnessT1ContextLoss ==
    /\ core.epoch = 0
    /\ core.nextIndex = 1
    /\ ContextLossAfterPreparedSync

WitnessContextLossFairness ==
    /\ WitnessNormalCoreFairness
    /\ WF_vars(WitnessObserve)
    /\ WF_vars(WitnessT0Select)
    /\ WF_vars(WitnessT0Commit)
    /\ WF_vars(WitnessT0Close)
    /\ WF_vars(WitnessT1ContextLoss)

ContextLossWitnessNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1ContextLoss

ContextLossWitnessSpec ==
    CompositionInit
    /\ [][ContextLossWitnessNext]_vars
    /\ WitnessContextLossFairness

ContextLossWitnessReached ==
    <> ( /\ owner.stage = "cancelled"
         /\ core.nextIndex = 1
         /\ core.phase = "reset-required"
         /\ core.lastEvent \in {"c-context-loss", "f-context-loss"}
         /\ Cardinality(owner.durableBundles) = 1 )

WitnessClassifyCancelled ==
    /\ owner.stage = "cancelled"
    /\ core.epoch = 0
    /\ Core!ClassifyFault
    /\ UNCHANGED owner

WitnessResetOfferCancelled ==
    /\ owner.stage = "cancelled"
    /\ Core!ResetOffer
    /\ UNCHANGED owner

WitnessResetAckCancelled ==
    /\ owner.stage = "cancelled"
    /\ Core!ResetAck
    /\ UNCHANGED owner

WitnessResetCommitAndRetire == ResetCommitAfterOwnerCancel

WitnessFreshStart ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ owner.phase = "closed"
    /\ owner.stage = "receiving"
    /\ Core!StartTU
    /\ UNCHANGED owner

WitnessFreshT1Select ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ SelectCommitSync

WitnessFreshT1Commit ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ FCommitAndPublish

WitnessCancelResetReuseFairness ==
    /\ WitnessNormalCoreFairness
    /\ WF_vars(WitnessObserve)
    /\ WF_vars(WitnessT0Select)
    /\ WF_vars(WitnessT0Commit)
    /\ WF_vars(WitnessT0Close)
    /\ WF_vars(WitnessT1Cancel)
    /\ WF_vars(WitnessClassifyCancelled)
    /\ WF_vars(WitnessResetOfferCancelled)
    /\ WF_vars(WitnessResetAckCancelled)
    /\ WF_vars(WitnessResetCommitAndRetire)
    /\ WF_vars(WitnessFreshStart)
    /\ WF_vars(WitnessFreshT1Select)
    /\ WF_vars(WitnessFreshT1Commit)

CancelResetReuseWitnessNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Cancel
    \/ WitnessClassifyCancelled
    \/ WitnessResetOfferCancelled
    \/ WitnessResetAckCancelled
    \/ WitnessResetCommitAndRetire
    \/ WitnessFreshStart
    \/ WitnessCoreProgress
    \/ WitnessFreshT1Select
    \/ WitnessFreshT1Commit

CancelResetReuseWitnessSpec ==
    CompositionInit
    /\ [][CancelResetReuseWitnessNext]_vars
    /\ WitnessCancelResetReuseFairness

CancelResetReuseWitnessReached ==
    <> ( /\ core.epoch = 1
         /\ core.nextIndex = 1
         /\ core.phase = "f-committed"
         /\ owner.stage = "committed"
         /\ OwnerTUSeq = 1
         /\ Cardinality(owner.durableBundles) = 2
         /\ Cardinality(owner.consumedPermits) = 2
         /\ Cardinality(owner.readyEventIds) = 2 )

(***************************************************************************
 Epoch-one cancel regression.  This reuses the real T0 cancel/reset path,
 then starts the fresh epoch-one TU and cancels it after materialization.  The
 final reset-required state must remain quiescent: CoreIndependent must not
 classify it into terminal-failure while the owner is cancelled.
***************************************************************************)
FinalCancelFreshStart ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ core.phase = "aligned"
    /\ owner.phase = "closed"
    /\ owner.stage = "receiving"
    /\ Core!StartTU
    /\ UNCHANGED owner

FinalCancelFreshCoreProgress ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ ( \/ TerminalWitnessCCodecTouch
         \/ TerminalWitnessCCodecReturn
         \/ TerminalWitnessCWriterTick
         \/ TerminalWitnessFReaderTick
         \/ TerminalWitnessBodyComplete
         \/ TerminalWitnessFLease
         \/ TerminalWitnessFCodecTouch
         \/ TerminalWitnessFCodecReturn
         \/ TerminalWitnessMaterialize )
    /\ UNCHANGED owner

FinalCancelObserve ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ ObservePreparedSync

FinalCancelAfterTouch ==
    /\ core.epoch = 1
    /\ core.nextIndex = 1
    /\ CancelAfterTouchSync

FinalCancelResetPrefixNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Cancel
    \/ WitnessClassifyCancelled
    \/ WitnessResetOfferCancelled
    \/ WitnessResetAckCancelled
    \/ WitnessResetCommitAndRetire

FinalCancelWitnessNext ==
    FinalCancelResetPrefixNext
    \/ FinalCancelFreshStart
    \/ FinalCancelFreshCoreProgress
    \/ FinalCancelObserve
    \/ FinalCancelAfterTouch
    \/ FinalCancelledResetQuiescence

FinalCancelWitnessSpec ==
    CompositionInit
    /\ [][FinalCancelWitnessNext]_vars
    /\ WitnessNormalCoreFairness
    /\ WF_vars(WitnessObserve)
    /\ WF_vars(WitnessT0Select)
    /\ WF_vars(WitnessT0Commit)
    /\ WF_vars(WitnessT0Close)
    /\ WF_vars(WitnessT1Cancel)
    /\ WF_vars(WitnessClassifyCancelled)
    /\ WF_vars(WitnessResetOfferCancelled)
    /\ WF_vars(WitnessResetAckCancelled)
    /\ WF_vars(WitnessResetCommitAndRetire)
    /\ WF_vars(FinalCancelFreshStart)
    /\ WF_vars(FinalCancelFreshCoreProgress)
    /\ WF_vars(FinalCancelObserve)
    /\ WF_vars(FinalCancelAfterTouch)

FinalCancelWitnessReached ==
    <> FinalCancelledResetRequiredState

CommitWitnessNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Select
    \/ WitnessT1Commit
    \/ WitnessT1LateClose

CommitWitnessSpec ==
    CompositionInit
    /\ [][CommitWitnessNext]_vars
    /\ WitnessCommitFairness

CommitWitnessReached ==
    <> ( /\ owner.phase = "closed"
         /\ owner.stage = "committed"
         /\ owner.lateCloseSuppressed
         /\ OwnerTUSeq = 1
         /\ Cardinality(owner.durableBundles) = 2
         /\ Cardinality(owner.consumedPermits) = 2
         /\ Cardinality(owner.readyEventIds) = 2 )

StaleOwnerWitnessNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ RejectStaleOwnerEvent
    \/ (WitnessT1Select /\ owner.staleEventsRejected = 1)
    \/ (WitnessT1Commit /\ owner.staleEventsRejected = 1)
    \/ (WitnessT1LateClose /\ owner.staleEventsRejected = 1)

StaleOwnerWitnessSpec ==
    CompositionInit
    /\ [][StaleOwnerWitnessNext]_vars
    /\ WitnessStaleOwnerFairness

StaleOwnerEventRejectedReached ==
    <> (owner.staleEventsRejected = 1)

(***************************************************************************
 Branch-specific deletion controls.  Each driver has a named state
 antecedent and omits only the required transition.  Explicit nonterminal
 fallbacks make an ordinary deadlock attributable to that antecedent.
***************************************************************************)
CancelResetRetirementAntecedent ==
    /\ owner.phase = "closed"
    /\ owner.stage = "cancelled"
    /\ core.epoch = 0
    /\ core.phase = "reset-acked"
    /\ core.resetRequired
    /\ core.oldRouteFenced
    /\ core.liability = "none"

CancelResetRetirementDeleted ==
    FALSE

CancelResetRetirementDriverNext ==
    /\ ~CancelResetRetirementAntecedent
    /\ ( \/ WitnessCoreHappy
         \/ WitnessObserve
         \/ WitnessT0Select
         \/ WitnessT0Commit
         \/ WitnessT0Close
         \/ WitnessT1Cancel
         \/ WitnessClassifyCancelled
         \/ WitnessResetOfferCancelled
         \/ WitnessResetAckCancelled )

CancelResetRetirementNonterminalFallback ==
    /\ ~CancelResetRetirementAntecedent
    /\ ~ENABLED CancelResetRetirementDriverNext
    /\ UNCHANGED vars

CancelResetRetirementDeletedNext ==
    CancelResetRetirementDriverNext
    \/ CancelResetRetirementNonterminalFallback
    \/ CancelResetRetirementDeleted

CancelResetRetirementEnabledExact ==
    ENABLED CancelResetRetirementDeletedNext <=>
        ~CancelResetRetirementAntecedent

CommitSettlementReleaseAntecedent ==
    /\ core.phase = "aligned"
    /\ core.nextIndex > 0
    /\ core.lastEvent = "c-observe-commit"
    /\ owner.phase = "closed"
    /\ owner.stage = "committed"
    /\ owner.durable # NoBundle

CommitSettlementReleaseDeleted ==
    FALSE

CommitSettlementReleaseDriverNext ==
    /\ ~CommitSettlementReleaseAntecedent
    /\ ( \/ WitnessCoreHappy
         \/ WitnessObserve
         \/ WitnessT0Select
         \/ WitnessT0Commit
         \/ WitnessT0Close )

CommitSettlementReleaseNonterminalFallback ==
    /\ ~CommitSettlementReleaseAntecedent
    /\ ~ENABLED CommitSettlementReleaseDriverNext
    /\ UNCHANGED vars

CommitSettlementReleaseDeletedNext ==
    CommitSettlementReleaseDriverNext
    \/ CommitSettlementReleaseNonterminalFallback
    \/ CommitSettlementReleaseDeleted

CommitSettlementReleaseEnabledExact ==
    ENABLED CommitSettlementReleaseDeletedNext <=>
        ~CommitSettlementReleaseAntecedent

ContextLossDrainAntecedent ==
    /\ owner.phase = "closed"
    /\ owner.stage = "cancelled"
    /\ core.phase = "reset-required"
    /\ core.resetRequired
    /\ core.lossObserved
    /\ core.lastEvent \in {"c-context-loss", "f-context-loss"}

ContextLossDrainReplacementDriverNext ==
    /\ ~ContextLossDrainAntecedent
    /\ ( \/ WitnessCoreHappy
         \/ WitnessObserve
         \/ WitnessT0Select
         \/ WitnessT0Commit
         \/ WitnessT0Close
         \/ WitnessT1ContextLoss )

ContextLossDrainReplacementNonterminalFallback ==
    /\ ~ContextLossDrainAntecedent
    /\ ~ENABLED ContextLossDrainReplacementDriverNext
    /\ UNCHANGED vars

ContextLossDrainReplacementDeletedNext ==
    ContextLossDrainReplacementDriverNext
    \/ ContextLossDrainReplacementNonterminalFallback

ContextLossDrainReplacementEnabledExact ==
    ENABLED ContextLossDrainReplacementDeletedNext <=>
        ~ContextLossDrainAntecedent

NonterminalDeadlockAntecedent ==
    /\ core.phase = "c-codec-running"
    /\ core.cLeaseState = "may-have-touched"
    /\ core.pendingOp # Core!NoOperation

NonterminalDeadlockDriverNext ==
    /\ ~NonterminalDeadlockAntecedent
    /\ WitnessCoreHappy

NonterminalDeadlockNonterminalFallback ==
    /\ ~NonterminalDeadlockAntecedent
    /\ ~ENABLED NonterminalDeadlockDriverNext
    /\ UNCHANGED vars

NonterminalDeadlockNext ==
    NonterminalDeadlockDriverNext
    \/ NonterminalDeadlockNonterminalFallback

NonterminalDeadlockEnabledExact ==
    ENABLED NonterminalDeadlockNext <=> ~NonterminalDeadlockAntecedent

CancelResetRetirementDeletedSpec ==
    CompositionInit
    /\ [][CancelResetRetirementDeletedNext]_vars

CommitSettlementReleaseDeletedSpec ==
    CompositionInit
    /\ [][CommitSettlementReleaseDeletedNext]_vars

ContextDrainReplacementDeletedSpec ==
    CompositionInit
    /\ [][ContextLossDrainReplacementDeletedNext]_vars

NonterminalDeadlockSpec ==
    CompositionInit
    /\ [][NonterminalDeadlockNext]_vars


(***************************************************************************
 Independent composition mutants.  These negative-only actions are not part
 of CompositionNext or any positive witness.  They execute the forbidden
 terminal state directly so safety invariants, rather than disabled positive
 actions or temporal non-reachability, must turn each configuration red.
***************************************************************************)
MutantCancelWinsAndPublishes ==
    /\ core.nextIndex = 1
    /\ core.phase = "materialized"
    /\ owner.phase = "open"
    /\ owner.stage = "prepared"
    /\ owner.owner = core.pendingOp
    /\ owner.prepared = Core!PreparedInput(core)
    /\ LET forbiddenPermit ==
              Core!PermitId(core.pendingOp, core.preparedId)
           badCore == [core EXCEPT
              !.phase = "f-committed",
              !.fCodec = "available",
              !.fLeaseState = "installed",
              !.fContextOwner = "route",
              !.installedCodecJobs = @ \cup {core.fCodecJobId},
              !.fLogical = core.preCursor + 1,
              !.fCommittedTag = core.postTag,
              !.fDigest = owner.prepared.successorDigest,
              !.inputPublished = TRUE,
              !.publishedOps = @ \cup {core.pendingOp},
              !.permitPresent = FALSE,
              !.permitOp = core.pendingOp,
              !.permitPreparedId = core.preparedId,
              !.permitId = forbiddenPermit,
              !.permitConsumed = TRUE,
              !.consumedPermits = @ \cup {forbiddenPermit},
              !.previousCommitOp = core.lastCommitOp,
              !.lastCommitPresent = TRUE,
              !.lastCommitOp = core.pendingOp,
              !.lastCommitBody = core.retainedBody,
              !.lastCommitRaw = core.materializedRaw,
              !.lastCommitSuccessorDigest = owner.prepared.successorDigest,
              !.lastCommitSuccessorTag = owner.prepared.successorTag,
              !.lastCommitPreparedId = core.preparedId,
              !.lastCommitPermitId = forbiddenPermit,
              !.lastCommitContextGeneration =
                    owner.prepared.contextGeneration,
              !.lastCommitCodecJobId = owner.prepared.codecJobId,
              !.commitOwed = TRUE,
              !.terminalReady = FALSE,
              !.ackLost = FALSE,
              !.retryIssued = FALSE,
              !.sendsAtFCommit = core.bodySends,
              !.fTerminalCount = 1,
              !.lastFIdentity = core.pendingOp,
              !.lastEvent = "f-atomic-commit",
              !.eventIdentity =
                    Core!OperationEvent("f-atomic-commit", core.pendingOp)]
           badBundle == Core!DurableBundle(badCore)
       IN /\ core' = badCore
          /\ owner' = [owner EXCEPT
              !.stage = "committed",
              !.permit = NoPermit,
              !.durable = badBundle,
              !.lastDurable = badBundle,
              !.durableBundles = @ \cup {badBundle},
              !.consumedPermits = @ \cup {forbiddenPermit},
              !.readyEventIds =
                    @ \cup {badBundle.ready.readyEventId},
              !.cancelRequested = TRUE,
              !.deliveryState = "closed",
              !.lastObservation = Owner!CancelObservation(owner.owner),
              !.lastEvent = "mutant-cancel-wins-late-commit"]

PermitBypassMutantNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ MutantCancelWinsAndPublishes

PermitBypassMutantSpec ==
    CompositionInit
    /\ [][PermitBypassMutantNext]_vars

MutantUnsynchronizedContextLoss ==
    /\ core.nextIndex = 1
    /\ core.phase = "materialized"
    /\ owner.phase = "open"
    /\ owner.stage = "prepared"
    /\ (Core!LoseCContext \/ Core!LoseFContext)
    /\ UNCHANGED owner

UnsynchronizedContextLossMutantNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ MutantUnsynchronizedContextLoss

UnsynchronizedContextLossMutantSpec ==
    CompositionInit
    /\ [][UnsynchronizedContextLossMutantNext]_vars

MutantResetWithoutOwnerRetire ==
    /\ owner.stage = "cancelled"
    /\ Core!ResetCommit
    /\ UNCHANGED owner

ResetWithoutOwnerRetireMutantNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Cancel
    \/ WitnessClassifyCancelled
    \/ WitnessResetOfferCancelled
    \/ WitnessResetAckCancelled
    \/ MutantResetWithoutOwnerRetire

ResetWithoutOwnerRetireMutantSpec ==
    CompositionInit
    /\ [][ResetWithoutOwnerRetireMutantNext]_vars

MutantReadyOmissionCommit ==
    /\ core.nextIndex = 1
    /\ core.phase = "materialized"
    /\ owner.stage = "permit-issued"
    /\ owner.permit = core.permitId
    /\ Core!FCommit
    /\ LET exactBundle == Core!DurableBundle(core')
           badBundle == [exactBundle EXCEPT
               !.ready.readyEventId = 0,
               !.lastCommit.ready.readyEventId = 0]
       IN owner' = [owner EXCEPT
            !.stage = "committed",
            !.permit = NoPermit,
            !.durable = badBundle,
            !.lastDurable = badBundle,
            !.durableBundles = @ \cup {badBundle},
            !.consumedPermits = @ \cup {owner.permit},
            !.lastEvent = "mutant-ready-omitted"]

MutantReadySplitCommit ==
    /\ core.nextIndex = 1
    /\ core.phase = "materialized"
    /\ owner.stage = "permit-issued"
    /\ owner.permit = core.permitId
    /\ Core!FCommit
    /\ LET exactBundle == Core!DurableBundle(core')
       IN owner' = [owner EXCEPT
            !.stage = "committed",
            !.permit = NoPermit,
            !.durable = exactBundle,
            !.lastDurable = exactBundle,
            !.durableBundles = @ \cup {exactBundle},
            !.consumedPermits = @ \cup {owner.permit},
            !.lastEvent = "mutant-ready-split-later"]

ReadyOmissionMutantNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Select
    \/ MutantReadyOmissionCommit

ReadyOmissionMutantSpec ==
    CompositionInit
    /\ [][ReadyOmissionMutantNext]_vars

ReadySplitMutantNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ WitnessT1Select
    \/ MutantReadySplitCommit

ReadySplitMutantSpec ==
    CompositionInit
    /\ [][ReadySplitMutantNext]_vars

MutantStaleOwnerCancelsCurrent ==
    /\ core.nextIndex = 1
    /\ core.lastCommitPresent
    /\ core.lastCommitOp # core.pendingOp
    /\ owner.phase = "open"
    /\ owner.stage = "prepared"
    /\ Core!OwnerCancelAfterTouch
    /\ Owner!CancelWithObservation(
          Owner!CancelObservation(core.lastCommitOp))

StaleOwnerMutantNext ==
    WitnessCoreHappy
    \/ WitnessObserve
    \/ WitnessT0Select
    \/ WitnessT0Commit
    \/ WitnessT0Close
    \/ MutantStaleOwnerCancelsCurrent

StaleOwnerMutantSpec ==
    CompositionInit
    /\ [][StaleOwnerMutantNext]_vars

(***************************************************************************
 Dedicated composed safety deletion/weakening controls.  Each action is
 reachable after a bounded witness prefix and changes only the field that
 its named composed refinement predicate protects.
***************************************************************************)
MutantDeleteCoreTerminalIdentity ==
    /\ core.pendingOp # Core!NoOperation
    /\ core.phase # "aligned"
    /\ core' = [core EXCEPT
          !.fTerminalCount = 1,
          !.lastFIdentity = Core!WrongOperationIdentity(core.pendingOp)]
    /\ UNCHANGED owner

CoreTerminalIdentityDeletionNext ==
    WitnessCoreHappy \/ MutantDeleteCoreTerminalIdentity

CoreTerminalIdentityDeletionSpec ==
    CompositionInit
    /\ [][CoreTerminalIdentityDeletionNext]_vars

MutantDeleteCoreEventIdentity ==
    /\ core.pendingOp # Core!NoOperation
    /\ core.phase # "aligned"
    /\ core' = [core EXCEPT
          !.lastEvent = "c-codec-lease-issued",
          !.eventIdentity = Core!BoundEvent(
              "c-codec-lease-issued", Core!NoRouteIdentity,
              Core!NoOperation, Core!NoResetIdentity)]
    /\ UNCHANGED owner

CoreEventIdentityDeletionNext ==
    WitnessCoreHappy \/ MutantDeleteCoreEventIdentity

CoreEventIdentityDeletionSpec ==
    CompositionInit
    /\ [][CoreEventIdentityDeletionNext]_vars

MutantDeleteCoreSettlementLedger ==
    /\ core.nextIndex > 0
    /\ core' = [core EXCEPT !.settledOps = <<>>]
    /\ UNCHANGED owner

CoreSettlementLedgerDeletionNext ==
    CompositionSafetyPrefix \/ MutantDeleteCoreSettlementLedger

CoreSettlementLedgerDeletionSpec ==
    CompositionInit
    /\ [][CoreSettlementLedgerDeletionNext]_vars

MutantDeleteCoreResourceBounds ==
    /\ core' = [core EXCEPT !.resourceLeak = PerRouteCap]
    /\ UNCHANGED owner

CoreResourceBoundsDeletionNext ==
    WitnessCoreHappy \/ MutantDeleteCoreResourceBounds

CoreResourceBoundsDeletionSpec ==
    CompositionInit
    /\ [][CoreResourceBoundsDeletionNext]_vars

MutantDeleteCorePayloadReleasedOnSettlement ==
    /\ core.phase = "f-committed"
    /\ core.lastCommitPresent
    /\ core' = [core EXCEPT
          !.lastEvent = "c-observe-commit",
          !.eventIdentity = Core!OperationEvent(
              "c-observe-commit", core.lastCommitOp),
          !.resourceBeforeRelease = 0]
    /\ UNCHANGED owner

CorePayloadReleasedOnSettlementDeletionNext ==
    CompositionSafetyPrefix \/ MutantDeleteCorePayloadReleasedOnSettlement

CorePayloadReleasedOnSettlementDeletionSpec ==
    CompositionInit
    /\ [][CorePayloadReleasedOnSettlementDeletionNext]_vars

(***************************************************************************
 One-at-a-time composition seam weakenings.  These controls exercise the
 owner arbitration and shared publication predicates directly, without
 relying on the five core aliases above.
***************************************************************************)
MutantDeletePermitBinding ==
    /\ owner.stage = "prepared"
    /\ owner.phase = "open"
    /\ owner' = [owner EXCEPT !.stage = "permit-issued"]
    /\ UNCHANGED core

PermitBindingDeletionNext == CompositionSafetyDriver \/ MutantDeletePermitBinding
PermitBindingDeletionSpec ==
    CompositionInit
    /\ [][PermitBindingDeletionNext]_vars

MutantDeleteCancelClosure ==
    /\ owner.stage = "cancelled"
    /\ owner.cancelRequested
    /\ owner' = [owner EXCEPT !.stage = "committed"]
    /\ UNCHANGED core

CancelClosureDeletionNext == CompositionCancelSafetyDriver \/ MutantDeleteCancelClosure
CancelClosureDeletionSpec ==
    CompositionInit
    /\ [][CancelClosureDeletionNext]_vars

MutantDeleteTouchedReset ==
    /\ owner.stage = "cancelled"
    /\ core.resetRequired
    /\ core' = [core EXCEPT !.resetRequired = FALSE]
    /\ UNCHANGED owner

TouchedResetDeletionNext == CompositionCancelSafetyDriver \/ MutantDeleteTouchedReset
TouchedResetDeletionSpec ==
    CompositionInit
    /\ [][TouchedResetDeletionNext]_vars

MutantDeleteAtomicReadyBundle ==
    /\ core.phase = "f-committed"
    /\ owner.stage = "committed"
    /\ owner' = [owner EXCEPT !.readyEventIds = {}]
    /\ UNCHANGED core

AtomicReadyBundleDeletionNext == CompositionSafetyDriver \/ MutantDeleteAtomicReadyBundle
AtomicReadyBundleDeletionSpec ==
    CompositionInit
    /\ [][AtomicReadyBundleDeletionNext]_vars

MutantDeleteCommitPersistence ==
    /\ owner.lateCloseSuppressed
    /\ owner' = [owner EXCEPT !.stage = "closed"]
    /\ UNCHANGED core

CommitPersistenceDeletionNext == CompositionCommitSafetyDriver \/ MutantDeleteCommitPersistence
CommitPersistenceDeletionSpec ==
    CompositionInit
    /\ [][CommitPersistenceDeletionNext]_vars

=============================================================================
