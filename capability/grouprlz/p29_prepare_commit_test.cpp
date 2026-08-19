// Gates 2-6 for OnlineS1's prepare/commit/abort state machine.
// (Gate 1 is p29_online_s1_test.cpp -- the existing admit() equivalence, kept unmodified.)
//
// A prepared TU is APPLIED immediately, because the plan has to be built against real
// matcher state; the journal is what makes it reversible.  These gates check that the undo
// restores head VALUES in reverse mutation order and not merely vector sizes, which is the
// failure a size-only rollback hides.  Predecessor VALUES are deliberately NOT restored --
// see the reasoning in undo(); there is no gate for that because the state is unreachable,
// and inventing one would be a check that cannot fail.
//
// The two matchers take different paths on purpose: GLOBAL_S1 uses the direct, always-
// committed admit(), and the SELECTED ROUTE uses prepare()/commit()/abort().  Gate 2 proves
// the two produce the same plan and the same resulting state; gate 5 proves they cannot be
// interleaved.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror p29_prepare_commit_test.cpp -o t && ./t
#include "p29_online_s1.h"

#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
// Expand a Root to the Region sequence it represents.  Comparing RAW canonical Block ids
// across an abort is wrong by design: the catalogue is deliberately NOT rolled back, so an
// aborted TU's minted ids survive and the retry legitimately sees different ids (and
// canonical_was_new flips true->false).  What must be exact is the Root's IDENTITY -- the
// same Regions in the same order -- and the matcher history.
std::vector<uint32_t> expand(const p29::TuPlan& p, const p29::BlockCatalogue& c) {
    std::vector<uint32_t> out;
    for (const p29::Ref& r : p.root) {
        if (r.kind == p29::RefKind::Region) out.push_back(r.id);
        else for (uint32_t k : c.block(r.id).regions) out.push_back(k);
    }
    return out;
}
// Everything the MATCHER determines must be exact after an abort -- including
// source_position, which is matcher-local and is precisely what a stale hash head would
// change.  Only the canonical Block id and canonical_was_new may differ, because the
// catalogue is deliberately not rolled back.
bool sameShape(const p29::TuPlan& a, const p29::TuPlan& b) {
    if (a.root.size() != b.root.size() || a.block_uses.size() != b.block_uses.size()) return false;
    if (a.occurrence_begin != b.occurrence_begin || a.occurrence_end != b.occurrence_end) return false;
    for (size_t i = 0; i < a.root.size(); ++i) {
        if (a.root[i].kind != b.root[i].kind) return false;
        if (a.root[i].kind == p29::RefKind::Region && a.root[i].id != b.root[i].id) return false;
    }
    for (size_t i = 0; i < a.block_uses.size(); ++i) {
        const p29::BlockUse& x = a.block_uses[i]; const p29::BlockUse& y = b.block_uses[i];
        if (x.root_index != y.root_index || x.source_position != y.source_position ||
            x.length != y.length || x.source_precedes_current_tu != y.source_precedes_current_tu) return false;
    }
    return true;
}
bool same(const p29::TuPlan& a, const p29::TuPlan& b) {
    if (a.root.size() != b.root.size() || a.block_uses.size() != b.block_uses.size()) return false;
    if (a.occurrence_begin != b.occurrence_begin || a.occurrence_end != b.occurrence_end) return false;
    for (size_t i = 0; i < a.root.size(); ++i) if (a.root[i] != b.root[i]) return false;
    for (size_t i = 0; i < a.block_uses.size(); ++i) {
        const p29::BlockUse& x = a.block_uses[i]; const p29::BlockUse& y = b.block_uses[i];
        if (x.root_index != y.root_index || x.block_id != y.block_id ||
            x.source_position != y.source_position || x.length != y.length ||
            x.source_precedes_current_tu != y.source_precedes_current_tu) return false;
    }
    return true;
}
// min_match 3 with a short reach, so boundary anchors genuinely straddle TUs
const p29::OnlineS1::Config kCfg{3, 1024, 12};

// GATE 4's fixture (local-oracle's).  A chain budget of ONE, and a hash space small enough
// (2^4 slots) that the aborted TU is certain to leave a COLLIDING head in front of the
// genuine committed source.  With max_chain == 1 that one stale candidate consumes the whole
// search budget, so the real match is never reached -- which is what makes the restoration
// observable.  The earlier blind fixtures failed for the opposite reason: their stale head
// pointed at another EXACT occurrence, so content verification accepted it and the match came
// out valid anyway.
const p29::OnlineS1::Config kBudgetCfg{3, 1, 4};
const std::vector<uint32_t> kBudgetA{10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
const std::vector<uint32_t> kBudgetB{132, 121, 106, 141, 118, 129, 109, 120, 163, 162, 112, 133};

// The fixture that makes a stale head CHANGE THE ANSWER.  An earlier attempt put the shared
// content at the START of both the aborted TU and the retry, and could not discriminate:
// the stale head pointed at an index at or past the retry's own end, so following it was an
// out-of-bounds read whose result happened not to matter.  What is needed is a stale head
// that is IN BOUNDS, points FORWARD of the position being matched, and terminates the chain.
//
// Let H be the slot of the 3-gram (50,51,52) and let the retry begin at occurrence 9.
//   kA  installs H at position 0, where a SIX-symbol match is available.
//   kB  is the aborted TU.  It writes H twice, at positions 10 and 16 -- late, so both are
//       forward of where the retry looks, and twice, so only a REVERSE-order restore lands
//       the earliest recorded value.  It is longer than 3 so 10 and 16 both exceed 9.
//   kC  is the retry, and is LONGER than kB (12 > 10) so the stale index 10 or 16 is inside
//       the retry's own freshly-kNone predecessor range instead of past the end.
// Correct undo  -> heads_[H] == 0,  0 < 9, content matches six deep -> root[0] is a Block.
// Stale head    -> heads_[H] == 16 (no restore) or 10 (forward-order restore); both are
//                  >= 9 so the candidate is skipped, and predecessors_[that] is kNone, so
//                  the chain ends and root[0] is a bare Region.
const std::vector<uint32_t> kA{50, 51, 52, 53, 54, 55, 7, 8, 9};
const std::vector<uint32_t> kB{90, 50, 51, 52, 93, 94, 95, 50, 51, 52};
const std::vector<uint32_t> kC{50, 51, 52, 53, 54, 55, 60, 61, 62, 63, 64, 65};
}  // namespace

int main() {
    // GATE 2: prepare+commit is admit, in plan AND in resulting state.
    {
        p29::BlockCatalogue c1, c2;
        p29::OnlineS1 viaAdmit(kCfg, c1), viaPrepare(kCfg, c2);
        viaAdmit.admit(kA); viaPrepare.prepare(kA); viaPrepare.commit();
        const p29::TuPlan pa = viaAdmit.admit(kC);
        const p29::TuPlan pb = [&]{ const p29::TuPlan& r = viaPrepare.prepare(kC); p29::TuPlan cp = r; viaPrepare.commit(); return cp; }();
        check(same(pa, pb), "gate 2: prepare+commit plan differs from admit");
        check(viaAdmit.occurrences() == viaPrepare.occurrences(), "gate 2: occurrence history differs");
        check(c1.size() == c2.size(), "gate 2: catalogue sizes differ");
    }

    // GATE 4: the canonical rollback fixture.
    //
    // MECHANISM, stated precisely because it is easy to describe wrongly: prepare(B)
    // SUCCEEDS.  Nothing throws.  The caller then EXPLICITLY calls abort(), and it is
    // undo()'s head restoration that is under test -- restoring every touched slot in REVERSE
    // mutation order, so a slot written several times inside one TU ends at the value it held
    // before that TU.  The NEXT admitted TU must then see no stale head.  This is NOT a
    // build()-throw-induced undo, and it is not affected by admit() taking the direct
    // non-journalled path: abort() belongs to the ROUTE path, which still journals.
    {
        p29::BlockCatalogue cc, ct;
        p29::OnlineS1 control(kBudgetCfg, cc), tested(kBudgetCfg, ct);
        control.admit(kBudgetA);
        tested.admit(kBudgetA);
        tested.prepare(kBudgetB);
        tested.abort();
        const p29::TuPlan a = control.admit(kBudgetA);
        const p29::TuPlan b = [&]{ const p29::TuPlan& r = tested.prepare(kBudgetA); p29::TuPlan cp = r; tested.commit(); return cp; }();
        check(!a.block_uses.empty(),
              "gate 4: the control matched nothing, so the fixture cannot discriminate");
        check(sameShape(a, b),
              "gate 4: a stale head changed bounded-chain match selection after abort");
        check(expand(a, cc) == expand(b, ct),
              "gate 4c: Root expands to a different Region sequence after the aborted TU");
        check(control.occurrences() == tested.occurrences(), "gate 4: history diverged");
    }

    // GATE 3 + GATE 4c: abort clears the pending flag and restores the history (gate 3), and
    // a second, independently derived rollback fixture (gate 4c) -- kept because it
    // discriminates through a DIFFERENT mechanism from gate 4's: an in-bounds stale head
    // pointing FORWARD of the position being matched, whose successor link is kNone, so the
    // chain ENDS rather than being exhausted by a budget of one.  Two fixtures failing for
    // two different reasons is worth more than one, and neither is load-bearing alone.
    // The control never sees the aborted TU; the test prepares and aborts it in between.
    {
        p29::BlockCatalogue cc, ct;
        p29::OnlineS1 control(kCfg, cc), tested(kCfg, ct);
        control.admit(kA);
        tested.admit(kA);
        tested.prepare(kB);
        check(tested.has_pending(), "gate 4c: prepare did not mark a pending transaction");
        tested.abort();
        check(!tested.has_pending(), "gate 4c: abort left the transaction pending");
        check(control.occurrences() == tested.occurrences(), "gate 3: occurrence history not restored");
        const p29::TuPlan a = control.admit(kC);
        const p29::TuPlan b = [&]{ const p29::TuPlan& r = tested.prepare(kC); p29::TuPlan cp = r; tested.commit(); return cp; }();
        // The fixture is only a gate if the correct answer is one a stale head would LOSE.
        // Without this the check would pass on a fixture where neither side matches anything.
        check(!a.root.empty() && a.root[0].kind == p29::RefKind::Block,
              "gate 4c: the control found no Block to match back into, so the fixture cannot discriminate");
        check(!a.block_uses.empty() && a.block_uses[0].source_position < b.occurrence_begin &&
              a.block_uses[0].length == 6,
              "gate 4c: the control's match is not the six-deep one reachable only via the restored head");
        check(sameShape(a, b), "gate 4c: Root shape after abort differs -- head values were not restored");
        check(expand(a, cc) == expand(b, ct),
              "gate 4c: Root expands to a different Region sequence after the aborted TU");
        check(control.occurrences() == tested.occurrences(), "gate 4c: history diverged after the aborted TU");
    }

    // GATE 4b: the same property over a randomised space, so the gate does not rest on one
    // hand-built shape.  A control matcher is fed only the TUs that were kept; the tested
    // matcher additionally prepares and aborts a ghost TU before some of them.  Ghosts are
    // drawn from the same small alphabet as the real TUs, so their anchors land in the same
    // hash slots -- which is what makes a stale head reachable at all.  Every kept TU's plan
    // must be identical in everything the MATCHER decides, and the histories must agree.
    {
        std::mt19937 rng(20260819u);
        size_t ghosts = 0, blocks = 0;
        for (int trial = 0; trial < 4000 && !failures; ++trial) {
            p29::BlockCatalogue cc, ct;
            p29::OnlineS1 control(kCfg, cc), tested(kCfg, ct);
            const int tus = 2 + int(rng() % 8);
            for (int t = 0; t < tus; ++t) {
                if (rng() % 3 == 0) {                       // a ghost, prepared then aborted
                    std::vector<uint32_t> ghost(1 + rng() % 24);
                    for (uint32_t& v : ghost) v = 40 + rng() % 6;
                    tested.prepare(ghost);
                    tested.abort();
                    ++ghosts;
                }
                std::vector<uint32_t> tu(1 + rng() % 24);
                for (uint32_t& v : tu) v = 40 + rng() % 6;
                const p29::TuPlan a = control.admit(tu);
                const p29::TuPlan b = tested.admit(tu);
                for (const p29::Ref& r : a.root) if (r.kind == p29::RefKind::Block) ++blocks;
                check(sameShape(a, b), "gate 4b: a plan differs after an aborted ghost TU");
                check(expand(a, cc) == expand(b, ct), "gate 4b: Root expands differently after a ghost TU");
                check(control.occurrences() == tested.occurrences(), "gate 4b: history diverged");
            }
        }
        // Coverage guards, only meaningful if the loop ran to completion -- the trial loop
        // stops at the first failure, so checking them after a real failure would report a
        // thin corpus that is merely the early exit.
        if (!failures) {
            check(ghosts > 1000, "gate 4b: too few aborted transactions to be a net");
            check(blocks > 1000, "gate 4b: too few Block matches, so stale heads would go unexercised");
        }
    }

    // GATE 5: one pending transaction, and no commit/abort without one.
    {
        p29::BlockCatalogue c; p29::OnlineS1 s(kCfg, c);
        bool threw = false;
        try { s.commit(); } catch (const std::logic_error&) { threw = true; }
        check(threw, "gate 5: commit without a pending transaction was accepted");
        threw = false;
        try { s.abort(); } catch (const std::logic_error&) { threw = true; }
        check(threw, "gate 5: abort without a pending transaction was accepted");
        s.prepare(kA);
        threw = false;
        try { s.prepare(kB); } catch (const std::logic_error&) { threw = true; }
        check(threw, "gate 5: a second prepare was accepted while one was pending");
        // The load-bearing half.  admit() is a SECOND entrance to the state machine and takes
        // the direct, non-journalled path: an admit() landing inside a live route transaction
        // would mutate heads_ and the occurrence stream without recording them, and that
        // route's later abort() would roll back to a state that never existed.  The rejection
        // is what makes the two paths safe to coexist.
        threw = false;
        try { s.admit(kB); } catch (const std::logic_error&) { threw = true; }
        check(threw, "gate 5: a direct admit was accepted while a transaction was pending");
        const std::vector<uint32_t> occDuringPending = s.occurrences();
        s.abort();
        check(s.occurrences().size() < occDuringPending.size(),
              "gate 5: the rejected admit fixture never had a pending TU to protect");
    }

    // GATE 6: an aborted prepare that minted a Block KEEPS the canonical id -- catalogue ids
    // are monotonic and never rewound -- while the route matcher is restored.  The id is
    // simply never marked known-on-F.
    {
        p29::BlockCatalogue c; p29::OnlineS1 s(kCfg, c);
        s.admit(kA);
        s.admit(kA);                       // second sight interns a Block
        const size_t afterCommit = c.size();
        check(afterCommit > 0, "gate 6: fixture minted no Block, so it cannot discriminate");
        const std::vector<uint32_t> occBefore = s.occurrences();
        s.prepare(kB);
        const size_t afterPrepare = c.size();          // EXACT size, so losing even one id shows
        const bool mintedInPrepare = afterPrepare > afterCommit;
        s.abort();
        check(mintedInPrepare, "gate 6: the prepared TU minted nothing, so it cannot discriminate");
        check(c.size() == afterPrepare, "gate 6: the catalogue was rolled back with the matcher");
        check(s.occurrences() == occBefore, "gate 6: the matcher was not restored");
    }

    std::printf("P29 prepare/commit/abort gates 2-6 %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
