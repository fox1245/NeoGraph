# Opt-in benchmark schema for ProgramTransitionPublication.
#
# This is deliberately a transport envelope, not a replacement persistence
# contract. Each nested immutable Program record remains canonical JSON so
# its existing parser, validation, and SHA-256 identity contract stay intact.

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("neograph::bench::capnp_poc");

@0xd749ac2e047a6a4c;

struct ProgramPublicationEnvelope {
  runRecord @0 :Data;
  journalRecord @1 :Data;
  events @2 :List(Data);
  effects @3 :List(Data);
  migrationPlan @4 :Data;
  hasMigrationPlan @5 :Bool;
}
