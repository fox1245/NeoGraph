---------------------------- MODULE BarrierJoin ----------------------------
(***************************************************************************)
(* BSP super-steps with one AND join whose wait_for slots may hold several *)
(* any_of alternatives: the approved scope of #363 (modes are deferred).   *)
(*                                                                         *)
(* Nodes are 1..n. Node 1 is the entry and node n is the join. A node      *)
(* routes statically to every target in its set (an empty set is an        *)
(* implicit end), or conditionally to one of two targets, chosen           *)
(* nondeterministically like a condition's label. Target 0 is __end__. One *)
(* node (g.ender) may instead return Command.goto(__end__), which preempts *)
(* every route of the super-step.                                          *)
(*                                                                         *)
(* Join semantics:                                                         *)
(*  - a slot is filled when any of its alternatives signals the join; the  *)
(*    join fires when every slot is filled, and then all signals clear;    *)
(*  - signals from non-members are recorded and never count;               *)
(*  - when nothing is ready the run ends; it is stalled, and Failed, when  *)
(*    the join still holds signals (from members or not), unless the final *)
(*    super-step reached __end__ by an explicit route: a static edge or a  *)
(*    conditional route to __end__, or Command.goto(__end__). An implicit  *)
(*    end (a node without successors) never suppresses the stall, and an   *)
(*    explicit end in an earlier super-step does not either.               *)
(*                                                                         *)
(* Init ranges over every graph of the bounded family (at most MaxNodes    *)
(* nodes) and over named graphs: the #357 shape with its legacy barrier    *)
(* and with an any_of slot, two non-exclusive variants (the leftover       *)
(* alternative arrives with the join's exit, or one super-step later), and *)
(* an explicit end by Command.goto.                                        *)
(* tests/test_join_exhaustive.cpp checks the same rules against the real   *)
(* Scheduler and GraphEngine for larger graphs.                            *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    MaxNodes,   \* family graphs have 2..MaxNodes nodes
    MaxCond,    \* at most this many conditional nodes per family graph
    MaxSteps,   \* super-step budget; a run still going after it hits the limit
    AnyOf       \* TRUE: family joins also get every any_of partition of their members

ASSUME MaxNodes \in 2..4 /\ MaxCond \in 0..2 /\ MaxSteps \in 1..20 /\ AnyOf \in BOOLEAN

End  == 0
Go   == -1   \* choice of a static node: signal every target
Stop == -2   \* choice of the ender: Command.goto(__end__)

NodesOf(n)   == 1..n
TargetsOf(n) == NodesOf(n) \cup {End}

Routes(n) ==
    {[cond |-> FALSE, to |-> S] : S \in {S \in SUBSET TargetsOf(n) : Cardinality(S) <= 2}}
    \cup {[cond |-> TRUE, to |-> S] : S \in {S \in SUBSET TargetsOf(n) : Cardinality(S) = 2}}

Predecessors(n, route) == {u \in 1..(n - 1) : n \in route[u].to}

Partitions(S) ==
    {P \in SUBSET (SUBSET S \ {{}}) :
        /\ UNION P = S
        /\ \A x, y \in P : x # y => x \cap y = {}}

Singletons(S) == {{m} : m \in S}

SlotSpecs(n, route) ==
    UNION {IF AnyOf THEN Partitions(M) ELSE {Singletons(M)} :
               M \in (SUBSET Predecessors(n, route)) \ {{}}}

FamilyOf(n) ==
    LET routes == {r \in [NodesOf(n) -> Routes(n)] :
                       Cardinality({u \in NodesOf(n) : r[u].cond}) <= MaxCond}
    IN UNION {UNION {{[n |-> n, route |-> r, slots |-> sl, ender |-> e] : e \in 0..n} :
                         sl \in SlotSpecs(n, r)} :
                  r \in routes}

Family == UNION {FamilyOf(n) : n \in 2..MaxNodes}

\* The #357 shape: A=1 fans out to B=2 and C=3; B routes to D=5 (pass) or to
\* FIX=4 (fail); FIX and C go to D; D exits.
Issue357Route ==
    [u \in 1..5 |-> CASE u = 1 -> [cond |-> FALSE, to |-> {2, 3}]
                      [] u = 2 -> [cond |-> TRUE,  to |-> {5, 4}]
                      [] u = 3 -> [cond |-> FALSE, to |-> {5}]
                      [] u = 4 -> [cond |-> FALSE, to |-> {5}]
                      [] u = 5 -> [cond |-> FALSE, to |-> {End}]]

\* B signals D and FIX: the alternatives are not route-exclusive. FIX's
\* signal arrives in the super-step in which D exits.
NonExclusiveRoute == [Issue357Route EXCEPT ![2] = [cond |-> FALSE, to |-> {5, 4}]]

\* As above, but B reaches FIX=5 through X=4, so FIX's signal arrives one
\* super-step after D=6 exits, with nothing else ready.
DelayedLeftoverRoute ==
    [u \in 1..6 |-> CASE u = 1 -> [cond |-> FALSE, to |-> {2, 3}]
                      [] u = 2 -> [cond |-> FALSE, to |-> {6, 4}]
                      [] u = 3 -> [cond |-> FALSE, to |-> {6}]
                      [] u = 4 -> [cond |-> FALSE, to |-> {5}]
                      [] u = 5 -> [cond |-> FALSE, to |-> {6}]
                      [] u = 6 -> [cond |-> FALSE, to |-> {End}]]

Named ==
    { [n |-> 5, route |-> Issue357Route,     slots |-> {{2}, {3}},    ender |-> 0],
      [n |-> 5, route |-> Issue357Route,     slots |-> {{2, 4}, {3}}, ender |-> 0],
      [n |-> 5, route |-> NonExclusiveRoute, slots |-> {{2, 4}, {3}}, ender |-> 0],
      [n |-> 6, route |-> DelayedLeftoverRoute, slots |-> {{2, 5}, {3}}, ender |-> 0],
      [n |-> 5, route |-> Issue357Route,     slots |-> {{2}, {3}},    ender |-> 4] }

VARIABLES
    g,             \* the graph; fixed for the whole behavior
    ready,         \* nodes that run in the current super-step
    pending,       \* signals the join holds (members or not)
    steps,         \* super-steps executed
    phase,         \* "run" or "done"
    outcome,       \* "none", "completed", "failed" (barrier stalled) or "limit"
    explicitEnd,   \* the last super-step reached __end__ by an explicit route or Command.goto
    firedNow,      \* the join fired in the last super-step (it is ready now)
    firedBefore,   \* the join has fired at least once
    leftover,      \* an any_of alternative arrived after a fire and is still waiting
    roundArrivals, \* ghost: arrivals at the join since its last fire
    badFire        \* ghost: the join fired with an empty slot

vars == <<g, ready, pending, steps, phase, outcome, explicitEnd, firedNow,
          firedBefore, leftover, roundArrivals, badFire>>

Join == g.n

Multi == UNION {s \in g.slots : Cardinality(s) > 1}

Filled(P) == \A s \in g.slots : s \cap P # {}

Choices(u) ==
    (IF g.route[u].cond THEN g.route[u].to ELSE {Go})
    \cup (IF g.ender = u THEN {Stop} ELSE {})

Picks == {p \in [ready -> UNION {Choices(u) : u \in ready}] : \A u \in ready : p[u] \in Choices(u)}

Signals(u, c) == IF c = Go THEN g.route[u].to ELSE IF c = Stop THEN {} ELSE {c}

Init ==
    /\ g \in Family \cup Named
    /\ ready = {1}
    /\ pending = {}
    /\ steps = 0
    /\ phase = "run"
    /\ outcome = "none"
    /\ explicitEnd = FALSE
    /\ firedNow = FALSE
    /\ firedBefore = FALSE
    /\ leftover = FALSE
    /\ roundArrivals = {}
    /\ badFire = FALSE

Step ==
    /\ phase = "run"
    /\ \E p \in Picks :
        LET stop     == \E u \in ready : p[u] = Stop
            arrivals == IF stop THEN {} ELSE {u \in ready : Join \in Signals(u, p[u])}
            others   == IF stop THEN {} ELSE UNION {Signals(u, p[u]) \ {End, Join} : u \in ready}
            \* An implicit end (no successors) signals nothing, so it is never explicit.
            explicit == stop \/ \E u \in ready : End \in Signals(u, p[u])
            held     == pending \cup arrivals
            fire     == arrivals # {} /\ Filled(held)
            nready   == others \cup (IF fire THEN {Join} ELSE {})
            nheld    == IF fire THEN {} ELSE held
        IN /\ ready' = nready
           /\ pending' = nheld
           /\ steps' = steps + 1
           /\ explicitEnd' = explicit
           /\ firedNow' = fire
           /\ firedBefore' = (firedBefore \/ fire)
           /\ leftover' = IF fire THEN FALSE
                          ELSE leftover \/ (firedBefore /\ arrivals \cap Multi # {})
           /\ roundArrivals' = IF fire THEN {} ELSE roundArrivals \cup arrivals
           /\ badFire' = (badFire \/ (fire /\ ~Filled(roundArrivals \cup arrivals)))
           /\ IF nready = {}
                 THEN /\ phase' = "done"
                      /\ outcome' = IF nheld # {} /\ ~explicit THEN "failed" ELSE "completed"
                 ELSE IF steps + 1 = MaxSteps
                      THEN /\ phase' = "done"
                           /\ outcome' = "limit"
                      ELSE /\ phase' = "run"
                           /\ outcome' = "none"
    /\ UNCHANGED g

Terminated == phase = "done" /\ UNCHANGED vars

Next == Step \/ Terminated

Spec == Init /\ [][Next]_vars /\ WF_vars(Step)

-----------------------------------------------------------------------------

TypeOK ==
    /\ ready \subseteq NodesOf(g.n)
    /\ pending \subseteq NodesOf(g.n)
    /\ steps \in 0..MaxSteps
    /\ phase \in {"run", "done"}
    /\ outcome \in {"none", "completed", "failed", "limit"}
    /\ (phase = "run") = (outcome = "none")

\* No double fire per round: every fire consumes arrivals that fill every
\* slot since the previous fire, and the join is ready only right after one.
NoDoubleFirePerRound == ~badFire
JoinRunsOnlyWhenFired == Join \in ready => firedNow
FireClearsSignals == firedNow => pending = {}

\* A stall fails the run, and only an explicit end in the final super-step
\* suppresses it.
StallImpliesFailed ==
    (phase = "done" /\ ready = {} /\ pending # {} /\ ~explicitEnd) => outcome = "failed"
OnlyExplicitEndSuppressesStall ==
    (outcome = "completed" /\ pending # {}) => explicitEnd
FailedOnlyOnStall ==
    outcome = "failed" => (ready = {} /\ pending # {} /\ ~explicitEnd)

\* A non-exclusive alternative that arrives after the join fired leads to a
\* stall (fail-closed) unless the join fires again or the final super-step
\* ends explicitly.
LeftoverAlternativeStalls ==
    (phase = "done" /\ ready = {} /\ leftover /\ ~explicitEnd) => outcome = "failed"

\* The owner's examples: FIX's leftover signal arriving with D's exit
\* completes; arriving one super-step after it fails.
NamedOutcomes ==
    phase = "done" =>
        /\ (g.n = 5 /\ g.route = NonExclusiveRoute) => outcome = "completed"
        /\ (g.n = 6 /\ g.route = DelayedLeftoverRoute) => outcome = "failed"

\* With singleton slots the slot rule is today's AND barrier: wait_for is a
\* subset of the signals received.
LegacyEquivalence ==
    (\A s \in g.slots : Cardinality(s) = 1)
        => \A P \in SUBSET NodesOf(g.n) : Filled(P) <=> (UNION g.slots \subseteq P)

Invariants ==
    /\ TypeOK
    /\ NoDoubleFirePerRound
    /\ JoinRunsOnlyWhenFired
    /\ FireClearsSignals
    /\ StallImpliesFailed
    /\ OnlyExplicitEndSuppressesStall
    /\ FailedOnlyOnStall
    /\ LeftoverAlternativeStalls
    /\ NamedOutcomes
    /\ LegacyEquivalence

\* Every run ends: completed, failed or at the step limit.
Termination == <>(phase = "done")
=============================================================================
