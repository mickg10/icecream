------------------- MODULE AssignmentFenceCoreProgress -------------------
(***************************************************************************
Progress claims for the abstract assignment-fence core.

The safety module deliberately exposes both product transitions and external
outcomes.  A compiler completing and a worker/session being lost are facts
about the environment; weak fairness for those actions is not a product
progress guarantee.

The accepted core liveness claim begins only after F has linearized REVOKED.
At that point phase[a] = "Revoked" records a complete revocation result waiting
for S.  Weak fairness is assumed only for S consuming that exact result.

ExternalOutcomeStep and FullEnvironmentFairSpec are named below solely to make
the stronger availability assumption explicit for future experiments.  They
are not selected by the acceptance configuration.
***************************************************************************)
EXTENDS AssignmentFenceCore

RevokedResultFairSpec ==
    /\ Spec
    /\ \A a \in Assignments : WF_vars(ConsumeRevoked(a))

RevokedEventuallyConsumed ==
    \A a \in Assignments :
        [](phase[a] = "Revoked" => <> (phase[a] = "Terminal"))

ExternalOutcomeStep(a) == Complete(a) \/ WorkerLost(a)

FullEnvironmentFairSpec ==
    /\ RevokedResultFairSpec
    /\ \A a \in Assignments : WF_vars(ExternalOutcomeStep(a))

=============================================================================
