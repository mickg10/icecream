------------------------ MODULE CompatibilityProjection ------------------------
(***************************************************************************
Concrete P43 projection for the Icecream 1.4.0/current compile path.

This model is deliberately codec-shaped.  It names the fields added after
protocol 43 and the one message kind introduced after P43.  The C++ OLD/NEW
socketpair fixture remains the byte-order/length authority; this module checks
trace filtering, explicit decode defaults, idempotence, and preservation of
assignment/client correlation fields.

Mutant switches model the four load-bearing compatibility failures.
***************************************************************************)
EXTENDS Naturals, Sequences, TLC

CONSTANTS MutantEmitJobTimingAt43,
          MutantKeepCommandSummaryAt43,
          MutantKeepLocalFieldsAt43,
          MutantKeepStatsClientCount

ASSUME /\ MutantEmitJobTimingAt43 \in BOOLEAN
       /\ MutantKeepCommandSummaryAt43 \in BOOLEAN
       /\ MutantKeepLocalFieldsAt43 \in BOOLEAN
       /\ MutantKeepStatsClientCount \in BOOLEAN

NoField == "NO_FIELD"

P43Kinds ==
    {"GET_CS", "USE_CS", "NO_CS", "JOB_BEGIN", "JOB_DONE",
     "JOB_LOCAL_BEGIN", "JOB_LOCAL_DONE", "STATS"}

AllKinds == P43Kinds \cup {"JOB_TIMING"}

Message(kind, job, client, commandSummary, fulljob,
        localReason, cmdline, localFlags, clientCount) ==
    [kind           |-> kind,
     job            |-> job,
     client         |-> client,
     commandSummary |-> commandSummary,
     fulljob        |-> fulljob,
     localReason    |-> localReason,
     cmdline        |-> cmdline,
     localFlags     |-> localFlags,
     clientCount    |-> clientCount]

MCTrace ==
    <<Message("GET_CS",          0,   11, "cc -c x.c", FALSE, "",       "",          0, NoField),
      Message("USE_CS",          100, 11, "",           FALSE, "",       "",          0, NoField),
      Message("JOB_BEGIN",       100, 11, "",           FALSE, "",       "",          0, 9),
      Message("JOB_DONE",        100, 11, "",           FALSE, "",       "",          0, 9),
      Message("JOB_LOCAL_BEGIN", 0,   12, "",           TRUE,  "policy", "cc -E x.c", 1, NoField),
      Message("JOB_LOCAL_DONE",  0,   12, "",           FALSE, "",       "",          0, NoField),
      Message("JOB_TIMING",      100, 11, "",           FALSE, "",       "",          0, NoField),
      Message("STATS",           0,   0,  "",           FALSE, "",       "",          0, 9)>>

IsP43Message(m) == m.kind \in P43Kinds

CandidateIsP43Message(m) ==
    IsP43Message(m)
    \/ (MutantEmitJobTimingAt43 /\ m.kind = "JOB_TIMING")

CorrectProject43(m) ==
    CASE m.kind = "GET_CS" ->
             [m EXCEPT !.commandSummary = ""]
      [] m.kind = "JOB_LOCAL_BEGIN" ->
             [m EXCEPT !.fulljob = FALSE,
                       !.localReason = "",
                       !.cmdline = "",
                       !.localFlags = 0]
      [] m.kind = "STATS" ->
             [m EXCEPT !.clientCount = NoField]
      [] OTHER -> m

CandidateProject43(m) ==
    CASE m.kind = "GET_CS" ->
             IF MutantKeepCommandSummaryAt43
             THEN m
             ELSE [m EXCEPT !.commandSummary = ""]
      [] m.kind = "JOB_LOCAL_BEGIN" ->
             IF MutantKeepLocalFieldsAt43
             THEN m
             ELSE [m EXCEPT !.fulljob = FALSE,
                             !.localReason = "",
                             !.cmdline = "",
                             !.localFlags = 0]
      [] m.kind = "STATS" ->
             IF MutantKeepStatsClientCount
             THEN m
             ELSE [m EXCEPT !.clientCount = NoField]
      [] OTHER -> m

SeqElems(s) == {s[i] : i \in 1..Len(s)}

CorrectTrace43(s) ==
    LET kept == SelectSeq(s, IsP43Message)
    IN [i \in 1..Len(kept) |-> CorrectProject43(kept[i])]

VARIABLES cursor, output

vars == <<cursor, output>>

Init ==
    /\ cursor = 1
    /\ output = <<>>

Emit ==
    /\ cursor <= Len(MCTrace)
    /\ LET m == MCTrace[cursor]
       IN output' =
            IF CandidateIsP43Message(m)
            THEN Append(output, CandidateProject43(m))
            ELSE output
    /\ cursor' = cursor + 1

Next == Emit

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ cursor \in 1..(Len(MCTrace) + 1)
    /\ output \in Seq([kind           : AllKinds,
                       job            : Nat,
                       client         : Nat,
                       commandSummary : STRING,
                       fulljob        : BOOLEAN,
                       localReason    : STRING,
                       cmdline        : STRING,
                       localFlags     : Nat,
                       clientCount    : Nat \cup {NoField}])

ProjectionPrefix ==
    output = CorrectTrace43(SubSeq(MCTrace, 1, cursor - 1))

NoUnknownKind ==
    \A i \in 1..Len(output) : output[i].kind \in P43Kinds

ProjectionIdempotent ==
    \A m \in SeqElems(MCTrace) :
        IsP43Message(m)
        => CorrectProject43(CorrectProject43(m)) = CorrectProject43(m)

CorrelationPreserved ==
    \A m \in SeqElems(MCTrace) :
        IsP43Message(m)
        => /\ CorrectProject43(m).job = m.job
           /\ CorrectProject43(m).client = m.client

ExplicitP43Defaults ==
    /\ \A m \in SeqElems(MCTrace) :
          m.kind = "GET_CS" => CorrectProject43(m).commandSummary = ""
    /\ \A m \in SeqElems(MCTrace) :
          m.kind = "JOB_LOCAL_BEGIN"
          => /\ CorrectProject43(m).fulljob = FALSE
             /\ CorrectProject43(m).localReason = ""
             /\ CorrectProject43(m).cmdline = ""
             /\ CorrectProject43(m).localFlags = 0
    /\ \A m \in SeqElems(MCTrace) :
          m.kind = "STATS" => CorrectProject43(m).clientCount = NoField

ProjectionInvariant ==
    /\ TypeOK
    /\ ProjectionPrefix
    /\ NoUnknownKind
    /\ ProjectionIdempotent
    /\ CorrelationPreserved
    /\ ExplicitP43Defaults

=============================================================================
