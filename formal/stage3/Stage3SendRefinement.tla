------------------------ MODULE Stage3SendRefinement ------------------------
EXTENDS Naturals

LocalStates == {"Delivered", "Sending", "Started", "Closing", "Cleaned"}
WireStates == {"Empty", "Prefix", "Full", "Decoded"}
RemoteStates == {"Assigned", "Begun", "Done", "Cancelled"}
LinkStates == {"Open", "Closed"}
TerminalKinds == {"None", "Done", "Cleanup"}

VARIABLES localState, wireState, remoteState, clientOpen, schedulerLink,
          slotCharged, terminalKind, releaseCount, everStarted,
          doneAccepted, doneQueued

vars ==
  <<localState, wireState, remoteState, clientOpen, schedulerLink,
    slotCharged, terminalKind, releaseCount, everStarted,
    doneAccepted, doneQueued>>

Init ==
  /\ localState = "Delivered"
  /\ wireState = "Empty"
  /\ remoteState = "Assigned"
  /\ clientOpen = TRUE
  /\ schedulerLink = "Open"
  /\ slotCharged = TRUE
  /\ terminalKind = "None"
  /\ releaseCount = 0
  /\ everStarted = FALSE
  /\ doneAccepted = FALSE
  /\ doneQueued = FALSE

StartSend ==
  /\ localState = "Delivered" /\ schedulerLink = "Open"
  /\ localState' = "Sending"
  /\ UNCHANGED <<wireState, remoteState, clientOpen, schedulerLink,
                 slotCharged, terminalKind, releaseCount, everStarted,
                 doneAccepted, doneQueued>>

WritePrefix ==
  /\ localState = "Sending" /\ wireState = "Empty"
  /\ wireState' = "Prefix"
  /\ UNCHANGED <<localState, remoteState, clientOpen, schedulerLink,
                 slotCharged, terminalKind, releaseCount, everStarted,
                 doneAccepted, doneQueued>>

CommitFullFrame ==
  /\ localState = "Sending" /\ wireState \in {"Empty", "Prefix"}
  /\ wireState' = "Full"
  /\ UNCHANGED <<localState, remoteState, clientOpen, schedulerLink,
                 slotCharged, terminalKind, releaseCount, everStarted,
                 doneAccepted, doneQueued>>

DecodeBegin ==
  /\ wireState = "Full" /\ remoteState = "Assigned"
  /\ wireState' = "Decoded" /\ remoteState' = "Begun"
  /\ UNCHANGED <<localState, clientOpen, schedulerLink, slotCharged,
                 terminalKind, releaseCount, everStarted, doneAccepted,
                 doneQueued>>

ReturnSuccess ==
  /\ localState = "Sending" /\ wireState \in {"Full", "Decoded"}
  /\ schedulerLink = "Open"
  /\ localState' = "Started" /\ everStarted' = TRUE
  /\ UNCHANGED <<wireState, remoteState, clientOpen, schedulerLink,
                 slotCharged, terminalKind, releaseCount,
                 doneAccepted, doneQueued>>

ReturnFailureNoCommit ==
  /\ localState = "Sending" /\ wireState \in {"Empty", "Prefix"}
  /\ localState' = "Closing"
  /\ clientOpen' = FALSE /\ schedulerLink' = "Closed"
  /\ UNCHANGED <<wireState, remoteState, slotCharged, terminalKind,
                 releaseCount, everStarted, doneAccepted, doneQueued>>

ReturnFailureAfterCommit ==
  /\ localState = "Sending" /\ wireState \in {"Full", "Decoded"}
  /\ localState' = "Closing"
  /\ clientOpen' = FALSE /\ schedulerLink' = "Closed"
  /\ UNCHANGED <<wireState, remoteState, slotCharged, terminalKind,
                 releaseCount, everStarted, doneAccepted, doneQueued>>

AcceptClientDone ==
  /\ localState = "Started" /\ clientOpen /\ terminalKind = "None"
  /\ doneAccepted' = TRUE /\ doneQueued' = TRUE
  /\ terminalKind' = "Done" /\ slotCharged' = FALSE
  /\ releaseCount' = releaseCount + 1
  /\ UNCHANGED <<localState, wireState, remoteState, clientOpen,
                 schedulerLink, everStarted>>

ConsumeQueuedDone ==
  /\ doneQueued /\ remoteState = "Begun"
  /\ remoteState' = "Done" /\ doneQueued' = FALSE
  /\ UNCHANGED <<localState, wireState, clientOpen, schedulerLink,
                 slotCharged, terminalKind, releaseCount, everStarted,
                 doneAccepted>>

BeginCloseAfterStart ==
  /\ localState = "Started" /\ terminalKind = "None"
  /\ localState' = "Closing"
  /\ clientOpen' = FALSE /\ schedulerLink' = "Closed"
  /\ UNCHANGED <<wireState, remoteState, slotCharged, terminalKind,
                 releaseCount, everStarted, doneAccepted, doneQueued>>

CleanupLocal ==
  /\ localState = "Closing" /\ terminalKind = "None"
  /\ localState' = "Cleaned"
  /\ terminalKind' = "Cleanup" /\ slotCharged' = FALSE
  /\ releaseCount' = releaseCount + 1
  /\ UNCHANGED <<wireState, remoteState, clientOpen, schedulerLink,
                 everStarted, doneAccepted, doneQueued>>

CleanupRemote ==
  /\ schedulerLink = "Closed"
  /\ remoteState \in {"Assigned", "Begun"} /\ ~doneQueued
  /\ remoteState' = "Cancelled"
  /\ UNCHANGED <<localState, wireState, clientOpen, schedulerLink,
                 slotCharged, terminalKind, releaseCount, everStarted,
                 doneAccepted, doneQueued>>

CloseCompleted ==
  /\ terminalKind = "Done" /\ remoteState = "Done"
  /\ localState = "Started"
  /\ localState' = "Cleaned"
  /\ clientOpen' = FALSE /\ schedulerLink' = "Closed"
  /\ UNCHANGED <<wireState, remoteState, slotCharged, terminalKind,
                 releaseCount, everStarted, doneAccepted, doneQueued>>

Quiesce ==
  /\ localState = "Cleaned" /\ remoteState \in {"Done", "Cancelled"}
  /\ UNCHANGED vars

Next ==
  \/ StartSend
  \/ WritePrefix
  \/ CommitFullFrame
  \/ DecodeBegin
  \/ ReturnSuccess
  \/ ReturnFailureNoCommit
  \/ ReturnFailureAfterCommit
  \/ AcceptClientDone
  \/ ConsumeQueuedDone
  \/ BeginCloseAfterStart
  \/ CleanupLocal
  \/ CleanupRemote
  \/ CloseCompleted
  \/ Quiesce

SafetySpec == Init /\ [][Next]_vars

Fairness ==
  /\ WF_vars(DecodeBegin)
  /\ WF_vars(ConsumeQueuedDone)
  /\ WF_vars(CleanupLocal)
  /\ WF_vars(CleanupRemote)
  /\ WF_vars(CloseCompleted)

Spec == SafetySpec /\ Fairness

TypeOK ==
  /\ localState \in LocalStates
  /\ wireState \in WireStates
  /\ remoteState \in RemoteStates
  /\ clientOpen \in BOOLEAN
  /\ schedulerLink \in LinkStates
  /\ slotCharged \in BOOLEAN
  /\ terminalKind \in TerminalKinds
  /\ releaseCount \in 0..1
  /\ everStarted \in BOOLEAN
  /\ doneAccepted \in BOOLEAN
  /\ doneQueued \in BOOLEAN

NoDoneBeforeStarted == doneAccepted => everStarted
SingleRelease == releaseCount <= 1
ChargeConservation == slotCharged = (terminalKind = "None")
RemoteDoneHasLocalDone == remoteState = "Done" => terminalKind = "Done"
CleanupExcludesDone == terminalKind = "Cleanup" => ~doneAccepted
FailedAttemptClosesClient ==
  localState \in {"Closing", "Cleaned"} => ~clientOpen
RemoteBeginRequiresDecode ==
  remoteState \in {"Begun", "Done"} => wireState = "Decoded"

ClosingEventuallyCleaned ==
  localState = "Closing" ~> localState = "Cleaned"

=============================================================================
