----------------- MODULE AssignmentFenceEarlyCancelProgress -----------------
(***************************************************************************
Product-controlled progress theorem for the actor-local early-cancel model.

Weak fairness is assumed only for consuming a continuously enabled live-link
FIFO head.  Compiler completion, client progress, connection repair, and peer
failure are outside this theorem.
***************************************************************************)
EXTENDS AssignmentFenceEarlyCancel

FDrain == FConsumePrepare \/ FConsumeRevoke
SDrain == SConsumeReady \/ SConsumeRevoked

FairSpec ==
    /\ Spec
    /\ WF_vars(FDrain)
    /\ WF_vars(SDrain)

EarlyCancelEventuallySettles ==
    [](earlyCancel =>
       <>(/\ revokedReplyConsumed
          /\ released
          /\ ~schedulerReservation
          /\ ~workerSlot
          /\ sPhase = "Terminal"))

=============================================================================
