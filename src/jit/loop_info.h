#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"

// 2.1  Loop Canonicalization Framework.
//
// Before this header, "is this block in a loop" was a guess: find_loops()
// treated any jump whose target block came earlier in the layout as a back
// edge. That is a structural heuristic, not a definition -- a forward branch
// that happens to target a lower-indexed block is not a loop, and a loop whose
// header is laid out after its latch is missed. Every later SSA pass (LICM,
// SCEV, Phi placement) needs the real thing, so this file builds it once:
//
//   CFG          -> successors/predecessors from the branch targets
//   dominators   -> Cooper-Harvey-Kennedy fixpoint over the dominator sets
//   natural loops-> a back edge is an edge A->B with B dominating A; the loop
//                   body is every block that reaches A without passing through B
//   preheader    -> the unique predecessor of the header that is outside the loop
//   latch        -> the block(s) holding the back edge
//   exits        -> every edge leaving the body
//
// The legacy find_loops()/LoopSpan flat-span view is intentionally kept for
// the linear liveness and the two-block passes that predate SSA. It is NOT
// derived from compute_loop_info() yet: replacing it makes live ranges more
// precise, and that exposes a latent miscompile (see find_loops below). The
// new analysis is the canonical one for tools and future SSA passes; the flat
// spans are migrated when SSA liveness replaces the linear allocator.

namespace lithon::jit {

// Trims surrounding whitespace from a branch-target label. Needed because
// Branch encodes both targets as one comma-separated name ("then, else") --
// without trimming, a label with a leading space after the split silently
// fails lookup, and a real loop is missed entirely.
inline std::string trim_label(const std::string& raw) {
    std::string label = raw;
    label.erase(0, label.find_first_not_of(" \t"));
    size_t last = label.find_last_not_of(" \t");
    label.erase(last == std::string::npos ? 0 : last + 1);
    return label;
}

// Labels a Jump/Branch can transfer control to (empty for other ops).
inline std::vector<std::string> branch_targets(const lithon::ir::Instr& in) {
    std::vector<std::string> out;
    if (in.op == lithon::ir::Op::Jump) {
        out.push_back(trim_label(in.name));
    } else if (in.op == lithon::ir::Op::Branch) {
        size_t comma = in.name.find(',');
        if (comma == std::string::npos) {
            out.push_back(trim_label(in.name));
        } else {
            out.push_back(trim_label(in.name.substr(0, comma)));
            out.push_back(trim_label(in.name.substr(comma + 1)));
        }
    }
    return out;
}

constexpr size_t kNoBlock = static_cast<size_t>(-1);

// ---------------------------------------------------------------------------
// CFG
// ---------------------------------------------------------------------------

struct Cfg {
    std::vector<std::string> labels;                       // block index -> label
    std::unordered_map<std::string, size_t> index;         // label -> block index
    std::vector<std::vector<size_t>> succ;                 // deduped, target order
    std::vector<std::vector<size_t>> pred;

    size_t size() const { return labels.size(); }
};

inline Cfg build_cfg(const lithon::ir::Function& fn) {
    using lithon::ir::Op;
    Cfg g;
    g.labels.reserve(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        g.labels.push_back(fn.blocks[b].label);
        g.index[fn.blocks[b].label] = b;
    }
    g.succ.resize(fn.blocks.size());
    g.pred.resize(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        std::vector<size_t> targets;
        const auto& block = fn.blocks[b];
        if (block.instrs.empty()) {
            // An empty block falls through to the next one, like any block
            // without an explicit terminator.
            if (b + 1 < fn.blocks.size()) targets.push_back(b + 1);
        } else {
            const auto& term = block.instrs.back();
            if (term.op == Op::Jump || term.op == Op::Branch) {
                for (const auto& t : branch_targets(term)) {
                    auto it = g.index.find(t);
                    if (it != g.index.end()) targets.push_back(it->second);
                }
            } else if (term.op != Op::Return) {
                // No terminator: execution falls into the textually next block,
                // which is exactly how the emitter lays them out.
                if (b + 1 < fn.blocks.size()) targets.push_back(b + 1);
            }
        }
        for (size_t t : targets) {
            if (std::find(g.succ[b].begin(), g.succ[b].end(), t) != g.succ[b].end()) continue;
            g.succ[b].push_back(t);
            g.pred[t].push_back(b);
        }
    }
    return g;
}

// ---------------------------------------------------------------------------
// Dominators (Cooper-Harvey-Kennedy style fixpoint on dominator sets)
// ---------------------------------------------------------------------------

struct DomInfo {
    std::vector<std::unordered_set<size_t>> dom;   // dom[b] = blocks dominating b
    std::vector<size_t> idom;                      // immediate dominator, kNoBlock for entry
    std::vector<bool> reachable;

    bool dominates(size_t a, size_t b) const { return dom[b].count(a) != 0; }
};

inline DomInfo compute_dominators(const Cfg& g) {
    const size_t n = g.size();
    DomInfo d;
    d.dom.assign(n, {});
    d.idom.assign(n, kNoBlock);
    d.reachable.assign(n, false);
    if (n == 0) return d;

    // Only the reachable subgraph participates; an unreachable block is its
    // own dominator and nothing else's.
    std::vector<size_t> order;
    std::vector<bool> seen(n, false);
    std::vector<size_t> stack{0};
    seen[0] = true;
    while (!stack.empty()) {
        size_t b = stack.back();
        stack.pop_back();
        order.push_back(b);
        for (size_t s : g.succ[b]) {
            if (!seen[s]) { seen[s] = true; stack.push_back(s); }
        }
    }
    for (size_t b : order) d.reachable[b] = true;

    std::unordered_set<size_t> all(order.begin(), order.end());
    for (size_t b = 0; b < n; ++b) d.dom[b] = d.reachable[b] ? all : std::unordered_set<size_t>{b};
    d.dom[0].clear();
    d.dom[0].insert(0);   // the entry dominates only itself; the fixpoint skips it

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t b : order) {
            if (b == 0) continue;
            std::unordered_set<size_t> next;
            bool first = true;
            for (size_t p : g.pred[b]) {
                if (!d.reachable[p]) continue;
                if (first) { next = d.dom[p]; first = false; }
                else {
                    std::unordered_set<size_t> inter;
                    for (size_t x : next) if (d.dom[p].count(x)) inter.insert(x);
                    next.swap(inter);
                }
            }
            next.insert(b);
            if (next != d.dom[b]) { d.dom[b].swap(next); changed = true; }
        }
    }

    // The immediate dominator is the strict dominator with the largest
    // dominator set: on the dominator tree all strict dominators form a chain,
    // so the one dominating every other strict dominator has the most members.
    for (size_t b : order) {
        if (b == 0) continue;
        size_t best = kNoBlock;
        size_t best_size = 0;
        for (size_t x : d.dom[b]) {
            if (x == b) continue;
            if (best == kNoBlock || d.dom[x].size() > best_size) { best = x; best_size = d.dom[x].size(); }
        }
        d.idom[b] = best;
    }
    return d;
}

// ---------------------------------------------------------------------------
// Natural loops
// ---------------------------------------------------------------------------

struct LoopExit {
    size_t from;   // in-loop block holding the edge
    size_t to;     // outside block
};

struct Loop {
    size_t header = kNoBlock;
    size_t preheader = kNoBlock;         // kNoBlock if none exists yet (must be created)
    std::vector<size_t> latches;         // blocks with a back edge to the header
    std::vector<size_t> blocks;          // sorted; includes the header
    std::vector<LoopExit> exits;         // one per in->out edge
    std::vector<size_t> exit_blocks;     // unique, sorted `to` above
    size_t parent = kNoBlock;            // index into LoopInfo::loops, kNoBlock if outermost

    bool contains(size_t b) const {
        return std::binary_search(blocks.begin(), blocks.end(), b);
    }
};

struct LoopInfo {
    std::vector<Loop> loops;
    std::vector<size_t> inner_most_of;   // block -> innermost containing loop, kNoBlock if none

    const Loop* by_header(size_t header) const {
        for (const auto& l : loops) if (l.header == header) return &l;
        return nullptr;
    }
};

// A back edge is any edge A->B with B dominating A (a self-edge A->A counts:
// A dominates itself). The natural loop of one back edge is the header B plus
// every block that can reach the latch A without passing through B; merging
// the bodies of all back edges into the same header gives the loop.
inline LoopInfo compute_loop_info(const lithon::ir::Function& fn) {
    LoopInfo info;
    const Cfg g = build_cfg(fn);
    const size_t n = g.size();
    info.inner_most_of.assign(n, kNoBlock);
    if (n == 0) return info;
    const DomInfo dom = compute_dominators(g);

    std::unordered_map<size_t, std::vector<size_t>> latches_of;   // header -> latches
    std::unordered_map<size_t, std::unordered_set<size_t>> body_of;

    for (size_t a = 0; a < n; ++a) {
        for (size_t b : g.succ[a]) {
            if (!dom.dominates(b, a)) continue;        // not a back edge
            latches_of[b].push_back(a);
            auto& body = body_of[b];
            body.insert(b);                            // header
            std::vector<size_t> work{a};               // start at the latch
            while (!work.empty()) {
                size_t x = work.back();
                work.pop_back();
                if (!body.insert(x).second) continue;  // already in the loop
                for (size_t p : g.pred[x])
                    if (dom.reachable[p]) work.push_back(p);   // stops at header (already inserted)
            }
        }
    }

    for (auto& kv : latches_of) {
        const size_t header = kv.first;
        Loop loop;
        loop.header = header;
        loop.latches = kv.second;
        std::sort(loop.latches.begin(), loop.latches.end());
        loop.latches.erase(std::unique(loop.latches.begin(), loop.latches.end()), loop.latches.end());

        const auto& body = body_of[header];
        loop.blocks.assign(body.begin(), body.end());
        std::sort(loop.blocks.begin(), loop.blocks.end());

        // Exits: any edge from inside to outside.
        std::unordered_set<size_t> exit_set;
        for (size_t u : loop.blocks) {
            for (size_t s : g.succ[u]) {
                if (body.count(s)) continue;
                loop.exits.push_back({u, s});
                exit_set.insert(s);
            }
        }
        loop.exit_blocks.assign(exit_set.begin(), exit_set.end());
        std::sort(loop.exit_blocks.begin(), loop.exit_blocks.end());

        // Preheader: the unique predecessor of the header that is outside the
        // loop. A preheader sitting in an *enclosing* loop is fine -- for an
        // inner loop it runs once per outer iteration, which is exactly the
        // once-before-the-head definition. Unreachable predecessors do not
        // count (a dead block that happens to jump into the header must not
        // defeat the unique-preheader test).
        size_t outside_pred = kNoBlock;
        size_t outside_preds = 0;
        for (size_t p : g.pred[header]) {
            if (body.count(p)) continue;
            if (!dom.reachable[p]) continue;
            outside_pred = p;
            ++outside_preds;
        }
        loop.preheader = (outside_preds == 1) ? outside_pred : kNoBlock;

        info.loops.push_back(std::move(loop));
    }

    // Deterministic order, by header position. Passes that walk loops and
    // rewrite the function rely on processing outer/before loops first, and
    // `changed`/`skip_headers` logic would otherwise see a different order on
    // every run.
    std::sort(info.loops.begin(), info.loops.end(),
              [](const Loop& a, const Loop& b) { return a.header < b.header; });

    // Nesting: the parent is the smallest loop whose body strictly contains
    // this loop's body. Bodies are disjoint or nested in a reducible CFG.
    for (size_t i = 0; i < info.loops.size(); ++i) {
        size_t best = kNoBlock;
        size_t best_size = SIZE_MAX;
        for (size_t j = 0; j < info.loops.size(); ++j) {
            if (i == j) continue;
            const auto& a = info.loops[i].blocks;
            const auto& b = info.loops[j].blocks;
            if (b.size() <= a.size()) continue;
            if (!std::includes(b.begin(), b.end(), a.begin(), a.end())) continue;
            if (b.size() < best_size) { best = j; best_size = b.size(); }
        }
        info.loops[i].parent = best;
    }

    for (size_t i = 0; i < info.loops.size(); ++i)
        for (size_t b : info.loops[i].blocks)
            info.inner_most_of[b] = i;   // later (smaller) loops overwrite; see sort below

    // inner_most_of must pick the *smallest* loop containing a block.
    for (size_t b = 0; b < n; ++b) {
        size_t best = kNoBlock;
        size_t best_size = SIZE_MAX;
        for (size_t i = 0; i < info.loops.size(); ++i) {
            if (info.loops[i].contains(b) && info.loops[i].blocks.size() < best_size) {
                best = i;
                best_size = info.loops[i].blocks.size();
            }
        }
        info.inner_most_of[b] = best;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Canonicalization: guarantee the shape LICM/SCEV rely on.
//
//   * one preheader  -- a block that dominates the whole loop and is its only
//                       entry from outside, so a hoisted instruction runs once
//   * one latch      -- the single block holding the backward edge
//
// Every control transfer is first made explicit (see materialize_fallthroughs)
// so a new block can be appended anywhere without disturbing an implicit
// fallthrough. Inserted blocks are appended at the end; all edges are labels,
// so physical order does not matter.
// ---------------------------------------------------------------------------

struct CanonStats {
    int preheaders = 0;
    int latches_merged = 0;
    int fallthroughs = 0;
    int blocks_added = 0;
};

inline std::string make_unique_label(const lithon::ir::Function& fn, const std::string& base) {
    std::unordered_set<std::string> used;
    for (const auto& b : fn.blocks) used.insert(b.label);
    if (!used.count(base)) return base;
    for (int i = 1;; ++i) {
        std::string candidate = base + "." + std::to_string(i);
        if (!used.count(candidate)) return candidate;
    }
}

// A block with no explicit terminator falls into the next block. Materialize
// that as a Jump so later edits can move blocks freely.
inline int materialize_fallthroughs(lithon::ir::Function& fn) {
    using namespace lithon::ir;
    int n = 0;
    for (size_t b = 0; b + 1 < fn.blocks.size(); ++b) {
        auto& ins = fn.blocks[b].instrs;
        if (!ins.empty()) {
            Op last = ins.back().op;
            if (last == Op::Jump || last == Op::Branch || last == Op::Return) continue;
        }
        Instr j;
        j.op = Op::Jump;
        j.result = kInvalidValue;
        j.name = fn.blocks[b + 1].label;
        ins.push_back(std::move(j));
        ++n;
    }
    return n;
}

// Redirect one terminator edge of `block_label` from `from` to `to`.
// Returns true if an edge was changed.
inline bool rewrite_terminator_target(lithon::ir::Function& fn, const std::string& block_label,
                                      const std::string& from, const std::string& to) {
    using namespace lithon::ir;
    for (auto& block : fn.blocks) {
        if (block.label != block_label || block.instrs.empty()) continue;
        Instr& term = block.instrs.back();
        if (term.op == Op::Jump) {
            if (trim_label(term.name) != from) return false;
            term.name = to;
            return true;
        }
        if (term.op == Op::Branch) {
            auto targets = branch_targets(term);
            bool changed = false;
            for (auto& t : targets)
                if (t == from) { t = to; changed = true; }
            if (!changed) return false;
            term.name = targets.size() == 2 ? targets[0] + ", " + targets[1] : targets[0];
            return true;
        }
        return false;
    }
    return false;
}

// Give every edge into the header from outside the body a single new entry
// block. `outside` are the predecessor labels to redirect.
inline bool insert_preheader(lithon::ir::Function& fn, const Loop& loop) {
    using namespace lithon::ir;
    if (loop.header == 0) return false;   // entry has no room for a block before it
    const Cfg g = build_cfg(fn);
    const std::unordered_set<size_t> inside(loop.blocks.begin(), loop.blocks.end());
    std::vector<std::string> outside;
    for (size_t p : g.pred[loop.header])
        if (!inside.count(p)) outside.push_back(fn.blocks[p].label);
    if (outside.empty()) return false;

    const std::string hdr = fn.blocks[loop.header].label;
    const std::string label = make_unique_label(fn, hdr + ".preheader");
    BasicBlock ph;
    ph.label = label;
    Instr j;
    j.op = Op::Jump;
    j.result = kInvalidValue;
    j.name = hdr;
    ph.instrs.push_back(std::move(j));

    for (const auto& pl : outside) rewrite_terminator_target(fn, pl, hdr, label);
    fn.blocks.push_back(std::move(ph));
    return true;
}

// Funnel every backward edge through one fresh latch block.
inline bool merge_latches(lithon::ir::Function& fn, const Loop& loop) {
    using namespace lithon::ir;
    if (loop.latches.size() <= 1) return false;
    const std::string hdr = fn.blocks[loop.header].label;
    const std::string label = make_unique_label(fn, hdr + ".latch");
    BasicBlock lb;
    lb.label = label;
    Instr j;
    j.op = Op::Jump;
    j.result = kInvalidValue;
    j.name = hdr;
    lb.instrs.push_back(std::move(j));

    for (size_t l : loop.latches) rewrite_terminator_target(fn, fn.blocks[l].label, hdr, label);
    fn.blocks.push_back(std::move(lb));
    return true;
}

// Iterate to a fixpoint, canonicalizing innermost loops first (smallest body),
// recomputing the analysis after each edit because new blocks change it.
struct ExitSplitStats {
    int blocks_added = 0;
    int edges_split = 0;
};

// 2.6. Give every loop-exit edge its own block.
//
// A loop's exit used to land directly on whatever followed the loop -- often a
// block holding the entire remainder of the function. Two things go wrong with
// that shape. The loop's blocks stop being contiguous, so nothing that reasons
// about "the loop body" can name it as a range; and the edge out is
// indistinguishable from any other edge into that block, so a pass that wants
// to move or duplicate the exit has no unit to move.
//
// The split makes both true: the loop becomes a contiguous run of blocks, and
// each exit is a named, single-predecessor edge block.
//
// This was UNSOUND to do before 2.7. The liveness model it lived next to
// measured a loop as a contiguous flat instruction range and dragged any
// overlapping value out to that range's end; splitting an exit out of the body
// punches a hole in the range, and a value used after the hole came back
// looking dead. CFG liveness has no such assumption -- a value is live on the
// edges that carry it -- so the split is now just a CFG edit.
//
// Skipped where it would be pure overhead: an edge out of a block that already
// jumps unconditionally to a block nothing else reaches needs no second block,
// and adding one would only lengthen the jump chain.
inline ExitSplitStats synthesize_loop_exits(lithon::ir::Function& fn) {
    using namespace lithon::ir;
    ExitSplitStats stats;

    // Each round: re-derive the loop structure, decide the edges to split, then
    // commit. Block indices shift on every insertion, so the structure has to
    // be recomputed rather than carried across rounds. Two rounds suffice in
    // practice -- an inner loop's exit edge only becomes an outer loop's once
    // the inner split has given it a block of its own -- and the bound turns a
    // future mistake into a wrong answer rather than a hang.
    struct Split {
        size_t from;             // block index holding the edge
        size_t to;               // block index the edge leaves the loop for
        std::string label;       // fresh block's label
    };

    for (int round = 0; round < 8; ++round) {
        const LoopInfo info = compute_loop_info(fn);
        const Cfg g = build_cfg(fn);
        std::vector<Split> splits;

        for (const auto& loop : info.loops) {
            for (const auto& exit : loop.exits) {
                if (exit.from >= fn.blocks.size() || exit.to >= fn.blocks.size()) continue;

                // Already dedicated. An edge into a block nothing else reaches
                // needs no second block: adding one would only lengthen the
                // jump chain, and the loop would still not own that code.
                bool sole_predecessor = true;
                for (size_t p : g.pred[exit.to])
                    if (p != exit.from) sole_predecessor = false;
                if (sole_predecessor) continue;

                splits.push_back({exit.from, exit.to,
                                  make_unique_label(fn, fn.blocks[exit.to].label + "_exit")});
            }
        }
        if (splits.empty()) break;

        // Redirect the terminator. The edge is identified by the LABEL of the
        // block it currently goes to, read from that block rather than
        // reconstructed from the fresh name -- make_unique_label may have
        // appended a disambiguating suffix, and matching on a shared prefix
        // would silently retarget the wrong half of a branch.
        for (const auto& sp : splits) {
            auto& ins = fn.blocks[sp.from].instrs;
            if (ins.empty()) continue;
            Instr& term = ins.back();
            const std::string target = fn.blocks[sp.to].label;
            if (term.op == Op::Jump) {
                if (term.name == target) term.name = sp.label;
            } else if (term.op == Op::Branch) {
                const auto halves = branch_targets(term);
                if (halves.size() != 2) continue;
                const std::string& t = halves.front();
                const std::string& f = halves.back();
                if (t == target) term.name = sp.label + "," + f;
                else if (f == target) term.name = t + "," + sp.label;
            }
        }

        // Commit from the back so the earlier indices stay valid. Each new block
        // goes immediately after the block holding the edge, which keeps the
        // loop's own blocks contiguous and puts the exit just past them rather
        // than scattering it through the middle of the function.
        std::sort(splits.begin(), splits.end(),
                  [](const Split& a, const Split& b) { return a.from > b.from; });
        for (const auto& sp : splits) {
            BasicBlock nb;
            nb.label = sp.label;
            Instr j;
            j.op = Op::Jump;
            j.result = kInvalidValue;
            j.name = fn.blocks[sp.to].label;
            nb.instrs.push_back(std::move(j));
            const size_t at = std::min(sp.from + 1, fn.blocks.size());
            fn.blocks.insert(fn.blocks.begin() + static_cast<long>(at), std::move(nb));
            ++stats.blocks_added;
        }
        stats.edges_split += static_cast<int>(splits.size());
    }
    return stats;
}

inline CanonStats canonicalize_loops(lithon::ir::Function& fn) {
    CanonStats stats;
    stats.fallthroughs = materialize_fallthroughs(fn);
    for (size_t guard = 0; guard < 1000; ++guard) {
        const LoopInfo info = compute_loop_info(fn);
        const Loop* target = nullptr;
        bool want_preheader = false;
        for (const auto& l : info.loops) {
            if (l.header == 0 || l.preheader != kNoBlock) continue;
            if (!target || l.blocks.size() < target->blocks.size()) { target = &l; want_preheader = true; }
        }
        if (!target) {
            for (const auto& l : info.loops) {
                if (l.latches.size() <= 1) continue;
                if (!target || l.blocks.size() < target->blocks.size()) { target = &l; want_preheader = false; }
            }
        }
        if (!target) break;
        const Loop loop = *target;   // copy: fn is mutated in place
        const bool did = want_preheader ? insert_preheader(fn, loop) : merge_latches(fn, loop);
        if (!did) break;             // e.g. entry-header loop: nothing we can do
        ++stats.blocks_added;
        if (want_preheader) ++stats.preheaders;
        else ++stats.latches_merged;
    }
    return stats;
}


} // namespace lithon::jit
