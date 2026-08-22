--------------------- MODULE Protocol50AssignmentConfig ---------------------
EXTENDS Protocol50Assignment

(***************************************************************************
TLC configuration files cannot assign function literals directly.  This
wrapper supplies named operators for the bounded two-assignment model while
leaving Protocol50Assignment.tla itself parameterized.
***************************************************************************)

CONSTANTS A0, A1, E0, E1, W0, N0, N1

EpochOfDef == [A0 |-> E0, A1 |-> E1]
WireOfDef == [A0 |-> W0, A1 |-> W0]
NonceOfDef == [A0 |-> N0, A1 |-> N1]
AssignmentAtDef == [E0 |-> [W0 |-> A0],
                    E1 |-> [W0 |-> A1]]

=============================================================================
