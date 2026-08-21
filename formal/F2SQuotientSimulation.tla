---------------------- MODULE F2SQuotientSimulation ----------------------
(***************************************************************************
Lockstep validation of the finite F-to-S quotient.

The concrete side carries synthetic absolute sequence values.  The abstract
side carries only payload order.  Enqueue and selected-position consumption
must preserve projection.  The fixed model consumes the FIFO head; the mutant
selects position two when possible and is rejected by FIFOOnly.
***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANTS Messages, MaxDepth, MaxSeq, MutantBypass

ASSUME /\ Messages # {}
       /\ MaxDepth \in Nat \ {0}
       /\ MaxSeq \in Nat
       /\ MutantBypass \in BOOLEAN

Frame(message, sequence) ==
    [message |-> message, sequence |-> sequence]

Project(concrete) ==
    [i \in 1..Len(concrete) |-> concrete[i].message]

RemoveAt(sequence, index) ==
    [i \in 1..(Len(sequence) - 1) |->
        IF i < index THEN sequence[i] ELSE sequence[i + 1]]

VARIABLES concreteQ,
          abstractQ,
          nextSequence,
          concreteLastIndex,
          abstractLastIndex,
          enqueueCount,
          consumeCount

vars ==
    <<concreteQ, abstractQ, nextSequence,
      concreteLastIndex, abstractLastIndex,
      enqueueCount, consumeCount>>

Init ==
    /\ concreteQ = <<>>
    /\ abstractQ = <<>>
    /\ nextSequence = 0
    /\ concreteLastIndex = 0
    /\ abstractLastIndex = 0
    /\ enqueueCount = 0
    /\ consumeCount = 0

Enqueue(message) ==
    /\ message \in Messages
    /\ Len(concreteQ) < MaxDepth
    /\ nextSequence <= MaxSeq
    /\ concreteQ' = Append(concreteQ, Frame(message, nextSequence))
    /\ abstractQ' = Append(abstractQ, message)
    /\ nextSequence' = nextSequence + 1
    /\ enqueueCount' = enqueueCount + 1
    /\ UNCHANGED <<concreteLastIndex, abstractLastIndex, consumeCount>>

ChosenIndex ==
    IF MutantBypass /\ Len(abstractQ) >= 2 THEN 2 ELSE 1

Consume ==
    /\ Len(concreteQ) > 0
    /\ ChosenIndex <= Len(concreteQ)
    /\ concreteQ' = RemoveAt(concreteQ, ChosenIndex)
    /\ abstractQ' = RemoveAt(abstractQ, ChosenIndex)
    /\ concreteLastIndex' = ChosenIndex
    /\ abstractLastIndex' = ChosenIndex
    /\ consumeCount' = consumeCount + 1
    /\ UNCHANGED <<nextSequence, enqueueCount>>

Next ==
    \/ \E message \in Messages : Enqueue(message)
    \/ Consume

Spec == Init /\ [][Next]_vars

ConcreteType ==
    Seq([message : Messages, sequence : 0..MaxSeq])

TypeOK ==
    /\ concreteQ \in ConcreteType
    /\ abstractQ \in Seq(Messages)
    /\ Len(concreteQ) <= MaxDepth
    /\ Len(abstractQ) <= MaxDepth
    /\ nextSequence \in 0..(MaxSeq + 1)
    /\ concreteLastIndex \in 0..MaxDepth
    /\ abstractLastIndex \in 0..MaxDepth
    /\ enqueueCount \in 0..(MaxSeq + 1)
    /\ consumeCount \in 0..(MaxSeq + 1)

ProjectionAgreement == abstractQ = Project(concreteQ)
IndexAgreement == abstractLastIndex = concreteLastIndex
CountAgreement == enqueueCount = nextSequence

SimulationInvariant ==
    /\ TypeOK
    /\ ProjectionAgreement
    /\ IndexAgreement
    /\ CountAgreement

FIFOOnly == abstractLastIndex <= 1

NoTwoFrameQueue == Len(abstractQ) < 2
NoBypassObserved == abstractLastIndex # 2

=============================================================================
