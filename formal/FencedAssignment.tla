--------------------------- MODULE FencedAssignment ---------------------------
(***************************************************************************
Worker-side authorization/fencing model for proposed protocol 49/50.

p49 gives S-F prepare/revoke. p50 adds a C-echoed nonce. LegacyClaim is
intentionally weaker: because an old C presents only wire id, a delayed claim
from another epoch can be mistaken for the current assignment when the id is
reused. NonceClaim excludes that counterexample.
***************************************************************************)
EXTENDS Naturals, FiniteSets, TLC

CONSTANTS Assignments, Epochs, WireIds, Nonces, Workers, NoWorker, NoNonce,
          EpochOf, WireOf, NonceOf, WorkerOf

ASSUME /\ Assignments # {}
       /\ Epochs # {}
       /\ WireIds # {}
       /\ Nonces # {}
       /\ NoWorker \notin Workers
       /\ NoNonce \notin Nonces
       /\ EpochOf \in [Assignments -> Epochs]
       /\ WireOf \in [Assignments -> WireIds]
       /\ NonceOf \in [Assignments -> Nonces]
       /\ WorkerOf \in [Assignments -> Workers]

Phases == {"Absent", "Preparing", "Reserved", "Claimed", "Started",
           "Revoked", "Terminal"}

VARIABLES currentEpoch, phase, claimedBy, tombstone

vars == <<currentEpoch, phase, claimedBy, tombstone>>

TypeOK ==
    /\ currentEpoch \in Epochs
    /\ phase \in [Assignments -> Phases]
    /\ claimedBy \in [Assignments -> (Assignments \cup {NoNonce})]
    /\ tombstone \subseteq Assignments

Init ==
    /\ currentEpoch \in Epochs
    /\ phase = [a \in Assignments |-> "Absent"]
    /\ claimedBy = [a \in Assignments |-> NoNonce]
    /\ tombstone = {}

Prepare(a) ==
    /\ EpochOf[a] = currentEpoch
    /\ phase[a] = "Absent"
    /\ phase' = [phase EXCEPT ![a] = "Preparing"]
    /\ UNCHANGED <<currentEpoch, claimedBy, tombstone>>

Ready(a) ==
    /\ phase[a] = "Preparing"
    /\ phase' = [phase EXCEPT ![a] = "Reserved"]
    /\ UNCHANGED <<currentEpoch, claimedBy, tombstone>>

(***************************************************************************
`arriving` denotes the logical assignment that issued the client's UseCS.
An old client does not carry EpochOf/NonceOf; F can compare only WireOf.
***************************************************************************)
LegacyClaim(current, arriving) ==
    /\ phase[current] = "Reserved"
    /\ WireOf[current] = WireOf[arriving]
    /\ current \notin tombstone
    /\ phase' = [phase EXCEPT ![current] = "Claimed"]
    /\ claimedBy' = [claimedBy EXCEPT ![current] = arriving]
    /\ UNCHANGED <<currentEpoch, tombstone>>

NonceClaim(current, arriving) ==
    /\ phase[current] = "Reserved"
    /\ WireOf[current] = WireOf[arriving]
    /\ NonceOf[current] = NonceOf[arriving]
    /\ current \notin tombstone
    /\ phase' = [phase EXCEPT ![current] = "Claimed"]
    /\ claimedBy' = [claimedBy EXCEPT ![current] = arriving]
    /\ UNCHANGED <<currentEpoch, tombstone>>

Start(a) ==
    /\ phase[a] = "Claimed"
    /\ phase' = [phase EXCEPT ![a] = "Started"]
    /\ UNCHANGED <<currentEpoch, claimedBy, tombstone>>

Revoke(a) ==
    /\ phase[a] \in {"Preparing", "Reserved"}
    /\ phase' = [phase EXCEPT ![a] = "Revoked"]
    /\ tombstone' = tombstone \cup {a}
    /\ UNCHANGED <<currentEpoch, claimedBy>>

Done(a) ==
    /\ phase[a] = "Started"
    /\ phase' = [phase EXCEPT ![a] = "Terminal"]
    /\ UNCHANGED <<currentEpoch, claimedBy, tombstone>>

Restart(e) ==
    /\ e \in Epochs
    /\ e # currentEpoch
    /\ currentEpoch' = e
    /\ UNCHANGED <<phase, claimedBy, tombstone>>

NextLegacy ==
    \/ \E a \in Assignments : Prepare(a)
    \/ \E a \in Assignments : Ready(a)
    \/ \E c \in Assignments, a \in Assignments : LegacyClaim(c, a)
    \/ \E a \in Assignments : Start(a)
    \/ \E a \in Assignments : Revoke(a)
    \/ \E a \in Assignments : Done(a)
    \/ \E e \in Epochs : Restart(e)

NextNonce ==
    \/ \E a \in Assignments : Prepare(a)
    \/ \E a \in Assignments : Ready(a)
    \/ \E c \in Assignments, a \in Assignments : NonceClaim(c, a)
    \/ \E a \in Assignments : Start(a)
    \/ \E a \in Assignments : Revoke(a)
    \/ \E a \in Assignments : Done(a)
    \/ \E e \in Epochs : Restart(e)

AuthorizationSafety ==
    \A a \in Assignments :
        phase[a] \in {"Claimed", "Started", "Terminal"} => claimedBy[a] # NoNonce

ExactGenerationSafety ==
    \A a \in Assignments :
        phase[a] \in {"Claimed", "Started", "Terminal"} =>
            /\ EpochOf[claimedBy[a]] = EpochOf[a]
            /\ NonceOf[claimedBy[a]] = NonceOf[a]

RevocationSafety ==
    \A a \in tombstone : phase[a] \notin {"Claimed", "Started"}

LegacySpec == Init /\ [][NextLegacy]_vars
NonceSpec  == Init /\ [][NextNonce]_vars

(***************************************************************************
With two assignments having equal WireOf but different EpochOf/NonceOf,
LegacySpec admits a counterexample to ExactGenerationSafety. NonceSpec is
intended to satisfy it, subject to the static mapping assumptions above.
***************************************************************************)
=============================================================================
