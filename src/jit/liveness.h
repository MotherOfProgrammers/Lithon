#pragma once
#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir/ir.h"
#include "loop_info.h"

namespace lithon::jit {

// One value's extent of existence.
//
// The block-granular fields (def_block/def_pos/lo/hi) are the ones allocation
// uses. birth/last_use remain as flat instruction indices because they are
// what a diagnostic can actually point at, and because two of them still mean
// something the block form does not: birth is where the value is produced, and
// last_use is the last textual reference.
//
// lo/hi are REVERSE POSTORDER indices, not block-list positions. RPO is used
// because it is a topological order of the CFG, so "defined earlier in RPO"
// implies "cannot depend on a later block" -- which is what makes the single
// [lo, hi] interval a sound over-approximation of a value's live blocks. Block
// list position has neither property, and is the reason the previous
// linear-instruction model could not be trusted across a loop.
struct LiveRange {
    int birth = -1;
    int last_use = -1;
    int def_block = -1;      // RPO index of the defining block
    int def_pos = -1;        // instruction offset inside that block
    int lo = 0;              // first RPO index where the value is live
    int hi = 0;              // last RPO index where the value is live
    bool spans_call = false; // must survive a call, so: callee-saved or spill
};

// Values with NO run-time existence, as decided by plan_function
// (compile_function.h). Each one is generated as an immediate operand,
// as a copy-free reference to a promoted variable's register, or fused
// into the single instruction that consumes it -- so no register and no
// stack slot is ever allocated for it. plan_function already proves this
// for EVERY such value (Const, Alias, FusedCmp, FusedStore), which is why
// the set lives here rather than in a second, narrower analysis that
// could only ever agree with codegen by coincidence.
using VirtualTemps = std::unordered_set<lithon::ir::ValueId>;

// Liveness over the CFG, by backward dataflow.
//
// This replaces a linear scan over the flat instruction order plus a
// "drag every range that overlaps a loop out to the loop's flat end" fixup.
// That model had one assumption it could not state: that the flat order is a
// legal execution order. Inside a loop it is not -- the header is listed once
// and runs many times -- so a value born before the loop and read in it could
// be given an interval ending before its own last read. The fixup hid that for
// one direction of the problem and had no way to see the other.
//
// Working from the CFG removes the assumption instead of patching it. Liveness
// is a property of edges, not of text, so a value that a back edge carries is
// live exactly where it is live.
//
// The transfer function is the textbook one:
//
//     live_out[B] = union over successors S of live_in[S]
//     live_in[B]  = use[B] union (live_out[B] minus def[B])
//
// iterated over RPO to a fixpoint. `use[B]` is "referenced before defined
// inside B", so a value defined and consumed entirely within one block is not
// forced live at the block's entry.
class LivenessAnalysis {
public:
    // `virtual_temps` must be plan_function's set, passed in at construction
    // -- NOT filtered afterwards. A virtual temp gets NO live range here: not
    // at its definition, and it is not counted as a "use" at its references
    // either. That matters because a virtual temp would otherwise be dragged
    // across whatever blocks its (non-existent) extent spans, charging it
    // against a small register pool for instructions it never generates.
    explicit LivenessAnalysis(const lithon::ir::Function& fn,
                              const VirtualTemps& virtual_temps = VirtualTemps{}) {
        const Cfg g = build_cfg(fn);
        const std::vector<size_t> rpo = reverse_postorder(g);

        // RPO index -> block index. -1 marks a block the search never reached
        // (unreachable from the entry). Such a block's values are treated as
        // dead outside it rather than being given a position, because there
        // is no order to be consistent with.
        rpo_pos_.assign(g.size(), -1);
        for (size_t i = 0; i < rpo.size(); ++i) rpo_pos_[rpo[i]] = static_cast<int>(i);

        compute_use_def(fn, virtual_temps);
        compute_edge_uses(fn, g);
        solve_fixpoint(g, rpo);
        build_ranges(fn, g, virtual_temps);

        // Call-clobbering last: it needs def positions from every range.
        mark_call_spanners(fn, g);
        build_interference(fn, g);
    }

    const std::unordered_map<lithon::ir::ValueId, LiveRange>& ranges() const {
        return ranges_;
    }

    int instruction_count() const { return instruction_count_; }

    // Blocks in reverse postorder, as CFG block indices. Exposed so a caller
    // that builds its own ordering (IRC's move list, say) can reuse the
    // analysis's notion of execution order instead of inventing a second one.
    const std::vector<size_t>& reverse_postorder() const { return rpo_; }

    // Two values INTERFERE when some program point could need both in
    // registers at once. That is the whole safety condition for handing one
    // register to two values, and the allocator's only reason to keep them
    // apart.
    //
    // This is deliberately not `their block intervals overlap`. Block
    // granularity answers the right question at block BOUNDARIES and the
    // wrong one everywhere inside a block: twenty instructions in a single
    // block all share one interval, so a straight-line chain of adds looks
    // like twenty simultaneously live values and spills for no reason. The
    // intervals in LiveRange are kept for ordering and diagnostics; the
    // edges below are what allocation actually consumes.
    bool interferes(lithon::ir::ValueId a, lithon::ir::ValueId b) const {
        auto it = interference_.find(a);
        return it != interference_.end() && it->second.count(b) != 0;
    }

    // Definition order: RPO block, then position inside the block. Colouring
    // in this order is what makes the greedy choice close to optimal -- for a
    // straight-line region the resulting graph is chordal, and chordal graphs
    // colour perfectly greedily in reverse postorder of their tree.
    const std::vector<lithon::ir::ValueId>& allocation_order() const { return order_; }

private:
    std::unordered_map<lithon::ir::ValueId, LiveRange> ranges_;
    std::vector<int> rpo_pos_;
    std::vector<std::unordered_set<lithon::ir::ValueId>> use_, def_;
    std::vector<std::unordered_set<lithon::ir::ValueId>> live_in_, live_out_;
    // Values a SUCCESSOR's Phi reads on the edge out of this block. Distinct
    // from live_out_ on purpose: it is an input to the fixpoint, not an output
    // of it, and it is edge-specific where live_out_ is not.
    std::vector<std::unordered_set<lithon::ir::ValueId>> edge_live_;
    std::vector<size_t> rpo_;
    std::unordered_map<lithon::ir::ValueId, std::unordered_set<lithon::ir::ValueId>> interference_;
    std::vector<lithon::ir::ValueId> order_;
    int instruction_count_ = 0;

    // Reference before definition within a block. Walking forwards and only
    // consulting `def` after adding to `use` is what makes this correct in one
    // pass: a value used twice in the same block, defined between the two uses,
    // is a use only the first time.
    void compute_use_def(const lithon::ir::Function& fn, const VirtualTemps& virtual_temps) {
        const size_t n = fn.blocks.size();
        use_.assign(n, {});
        def_.assign(n, {});
        for (size_t b = 0; b < n; ++b) {
            std::unordered_set<lithon::ir::ValueId> defined_here;
            for (const auto& instr : fn.blocks[b].instrs) {
                if (instr.result != lithon::ir::kInvalidValue && !virtual_temps.count(instr.result))
                    defined_here.insert(instr.result);
                // A Phi's operands belong to the incoming edges, not to this
                // block, and treating them as ordinary uses here is wrong in
                // both directions at once. It inflates: the operand is not live
                // inside the join, so counting it as live-in drags its range
                // backwards across the whole join. And it misses: what the copy
                // actually needs is the value alive at the END of the
                // predecessor, which use_[b] says nothing about -- so a value
                // whose last real use precedes the branch could look dead and
                // have its register reused before the copy reads it. That is a
                // silent wrong answer, so edge_live_ below carries these instead.
                if (instr.op == lithon::ir::Op::Phi) continue;
                for (auto arg : instr.args) {
                    // A virtual temp reference is not a real use: no register
                    // ever holds the value, so it cannot extend anything.
                    if (virtual_temps.count(arg)) continue;
                    if (!defined_here.count(arg)) use_[b].insert(arg);
                }
            }
            def_[b] = std::move(defined_here);
        }
    }

    // Operand i of a Phi in block S is read on the edge out of pred(S)[i], so
    // that is the block that has to hold it to its end. The pairing is by
    // predecessor index, which is exact because validate_ssa() requires a Phi's
    // arity to equal pred(S).size() and Cfg::pred is deduped -- a block that
    // branches to the same target twice contributes one edge and therefore one
    // operand, not two.
    void compute_edge_uses(const lithon::ir::Function& fn, const Cfg& g) {
        edge_live_.assign(fn.blocks.size(), {});
        for (size_t s = 0; s < fn.blocks.size() && s < g.size(); ++s) {
            for (const auto& instr : fn.blocks[s].instrs) {
                if (instr.op != lithon::ir::Op::Phi) continue;
                for (size_t i = 0; i < instr.args.size() && i < g.pred[s].size(); ++i) {
                    const auto arg = instr.args[i];
                    if (arg != lithon::ir::kInvalidValue) edge_live_[g.pred[s][i]].insert(arg);
                }
            }
        }
    }

    // Reverse postorder of the CFG, entry first. Unreachable blocks are
    // appended in block order so `rpo_pos_` covers every block: excluding them
    // would leave def_block == -1 for their values and quietly turn an
    // allocation decision into "always safe".
    std::vector<size_t> reverse_postorder(const Cfg& g) const {
        std::vector<size_t> order;
        std::vector<char> seen(g.size(), 0);
        // Iterative postorder on the reverse graph, so an entry block whose
        // predecessor is itself (a self-loop) cannot overflow the C++ stack.
        std::vector<std::pair<size_t, size_t>> stack;
        if (g.size() != 0) {
            seen[0] = 1;
            stack.push_back({0, 0});
        }
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < g.pred[node].size()) {
                const size_t p = g.pred[node][next++];
                if (!seen[p]) {
                    seen[p] = 1;
                    stack.push_back({p, 0});
                }
            } else {
                order.push_back(node);
                stack.pop_back();
            }
        }
        std::reverse(order.begin(), order.end());
        for (size_t b = 0; b < g.size(); ++b)
            if (!seen[b]) order.push_back(b);
        return order;
    }

    void solve_fixpoint(const Cfg& g, const std::vector<size_t>& rpo) {
        const size_t n = g.size();
        live_in_.assign(n, {});
        live_out_.assign(n, {});
        bool changed = true;
        // Bounded, not unbounded. One backwards sweep over RPO suffices for a
        // reducible CFG and this converges in a couple of rounds for the rest,
        // but the cap means a lattice bug degrades into a conservative answer
        // instead of a hang.
        for (int iter = 0; iter < 1000 && changed; ++iter) {
            changed = false;
            for (auto it = rpo.rbegin(); it != rpo.rend(); ++it) {
                const size_t b = *it;
                std::unordered_set<lithon::ir::ValueId> out;
                for (size_t s : g.succ[b]) {
                    out.insert(live_in_[s].begin(), live_in_[s].end());
                }
                // A successor's Phi reads its operand at the instant this
                // block's edge is taken, which is past every ordinary use here.
                out.insert(edge_live_[b].begin(), edge_live_[b].end());
                std::unordered_set<lithon::ir::ValueId> in = use_[b];
                for (const auto& v : out)
                    if (!def_[b].count(v)) in.insert(v);
                if (out != live_out_[b] || in != live_in_[b]) {
                    live_out_[b] = std::move(out);
                    live_in_[b] = std::move(in);
                    changed = true;
                }
            }
        }
    }

    void build_ranges(const lithon::ir::Function& fn, const Cfg& g,
                      const VirtualTemps& virtual_temps) {
        // Marker for a block reverse postorder never reached. Chosen to be
        // below every real RPO position (which start at 0) so an unreachable
        // value's interval is empty of real blocks.
        const int unreachable = -1;
        int idx = 0;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            for (size_t pos = 0; pos < fn.blocks[b].instrs.size(); ++pos) {
                const auto& instr = fn.blocks[b].instrs[pos];
                if (instr.result != lithon::ir::kInvalidValue &&
                    !virtual_temps.count(instr.result)) {
                    LiveRange& r = ranges_[instr.result];
                    r.birth = idx;
                    r.def_block = rpo_pos_[b];
                    r.def_pos = static_cast<int>(pos);
                    if (r.last_use < idx) r.last_use = idx;
                }
                for (auto arg : instr.args) {
                    if (virtual_temps.count(arg)) continue;
                    auto it = ranges_.find(arg);
                    if (it != ranges_.end() && it->second.last_use < idx) it->second.last_use = idx;
                }
                ++idx;
            }
        }
        instruction_count_ = idx;

        for (auto& kv : ranges_) {
            LiveRange& r = kv.second;
            if (r.def_block < 0) {
                // Defined in a block RPO never reached, so it has no place in
                // any execution order. Confined to lo == hi == -1, which sits
                // below every real block's position: such a value therefore
                // never overlaps anything and is emitted in a register it may
                // reuse freely. Safe because the block cannot execute.
                r.lo = r.hi = unreachable;
                continue;
            }
            int lo = r.def_block;
            int hi = r.def_block;
            for (size_t b = 0; b < g.size(); ++b) {
                if (live_in_[b].count(kv.first) || live_out_[b].count(kv.first)) {
                    const int p = rpo_pos_[b];
                    if (p < 0) continue;
                    lo = std::min(lo, p);
                    hi = std::max(hi, p);
                }
            }
            r.lo = lo;
            r.hi = hi;
        }
    }

    // A value must hold a callee-saved register (or a stack slot) if any Call
    // can execute between the value's availability and its last need.
    //
    // Both halves are position-aware, and both have to be. A value is
    // endangered by the call at (b, p) when
    //
    //   available at (b,p)   AND   still needed after (b,p)
    //
    // where "available" is: defined in an earlier block, defined earlier in
    // this same block, or live in at the block's entry (the loop-carried case,
    // whose definition is textually LATER and is exactly why the old flat
    // index comparison could not be trusted). "Still needed" is: live out of
    // the block, or referenced by some instruction after position p.
    //
    // Using block-level live_in/live_out alone looks sufficient and is not.
    // It misses a value defined AND consumed entirely within one block that
    // merely straddles a call, because such a value appears in neither set:
    // it is defined before its own uses, so it is not live-in, and it never
    // leaves the block, so it is not live-out. `fib` is exactly that shape --
    // %7 is the result of the first recursive call and an operand of the add
    // after the second -- and letting it into a caller-saved register made
    // fib(4) come back as 2.
    void mark_call_spanners(const lithon::ir::Function& fn, const Cfg& g) {
        using lithon::ir::Op;
        for (size_t b = 0; b < g.size() && b < fn.blocks.size(); ++b) {
            if (rpo_pos_[b] < 0) continue;
            const int block_rpo = rpo_pos_[b];

            // Last reference to each value inside this block. -1 means the block
            // never mentions it.
            std::unordered_map<lithon::ir::ValueId, int> last_use_here;
            for (size_t pos = 0; pos < fn.blocks[b].instrs.size(); ++pos)
                for (auto arg : fn.blocks[b].instrs[pos].args)
                    last_use_here[arg] = static_cast<int>(pos);

            for (size_t pos = 0; pos < fn.blocks[b].instrs.size(); ++pos) {
                if (Op::Call != fn.blocks[b].instrs[pos].op) continue;
                const int call_pos = static_cast<int>(pos);
                for (auto& kv : ranges_) {
                    LiveRange& r = kv.second;
                    if (r.def_block < 0) continue;

                    const bool defined_earlier_block = r.def_block < block_rpo;
                    const bool defined_earlier_here =
                        r.def_block == block_rpo && r.def_pos <= call_pos;
                    const bool available = defined_earlier_block || defined_earlier_here ||
                                           live_in_[b].count(kv.first) != 0;
                    if (!available) continue;

                    auto lu = last_use_here.find(kv.first);
                    const bool used_after_here =
                        lu != last_use_here.end() && lu->second > call_pos;
                    if (used_after_here || live_out_[b].count(kv.first) != 0)
                        r.spans_call = true;
                }
            }
        }
    }

    // Interference, as a sweep inside each block.
    //
    // Within one block every value has a presence interval: from where it
    // first exists there to where it last is needed. The start is -1 for a
    // value live in at the block's entry -- which is exactly a loop-carried
    // value, whose definition sits LATER in the block than its first use and
    // is the case a flat instruction range cannot represent -- and the end is
    // kPastEnd for one live out, because the block's successor will read it.
    //
    // Two values interfere when their presence intervals overlap, since that
    // names an instruction where both would be needed. Sweeping by start and
    // keeping the still-open intervals builds every such pair without the
    // quadratic pair loop being visible at the call site.
    static constexpr int kPastEnd = std::numeric_limits<int>::max();

    void build_interference(const lithon::ir::Function& fn, const Cfg& g) {
        std::vector<std::pair<lithon::ir::ValueId, std::pair<int, int>>> present;
        for (size_t b = 0; b < g.size() && b < fn.blocks.size(); ++b) {
            present.clear();

            // Every value that exists at some point in this block.
            std::unordered_map<lithon::ir::ValueId, std::pair<int, int>> span;
            auto note = [&](lithon::ir::ValueId v, int lo, int hi) {
                auto it = span.find(v);
                if (it == span.end()) {
                    span.emplace(v, std::make_pair(lo, hi));
                } else {
                    it->second.first = std::min(it->second.first, lo);
                    it->second.second = std::max(it->second.second, hi);
                }
            };

            for (size_t pos = 0; pos < fn.blocks[b].instrs.size(); ++pos) {
                const auto& instr = fn.blocks[b].instrs[pos];
                if (instr.result != lithon::ir::kInvalidValue) note(instr.result, int(pos), int(pos));
                // A Phi's operands are absent from this block. Noting them here
                // would claim they are needed at the top of the join, where the
                // copy has long since happened -- which both inflates the range
                // and invents interference that does not exist, including
                // between the operands of two different incoming edges. The
                // predecessor's own live_out_ is what says they are still alive.
                if (instr.op == lithon::ir::Op::Phi) continue;
                for (auto arg : instr.args) note(arg, int(pos), int(pos));
            }
            for (const auto& v : live_in_[b]) note(v, -1, -1);
            for (const auto& v : live_out_[b]) note(v, kPastEnd, kPastEnd);
            if (rpo_pos_[b] < 0) continue;

            for (const auto& kv : span) {
                if (ranges_.count(kv.first) == 0) continue;   // virtual temp: no location
                present.push_back({kv.first, kv.second});
            }
            std::sort(present.begin(), present.end(),
                      [](const auto& a, const auto& c) {
                          return a.second.first != c.second.first ? a.second.first < c.second.first
                                                                 : a.first < c.first;
                      });

            // Open intervals, in start order. An interval stays open while its
            // end reaches the next interval's start.
            std::vector<std::pair<lithon::ir::ValueId, int>> open;
            for (const auto& entry : present) {
                open.erase(std::remove_if(open.begin(), open.end(),
                                          [&](const auto& o) { return o.second < entry.second.first; }),
                           open.end());
                for (const auto& o : open) {
                    interference_[entry.first].insert(o.first);
                    interference_[o.first].insert(entry.first);
                }
                open.push_back({entry.first, entry.second.second});
            }
        }

        order_.reserve(ranges_.size());
        for (const auto& kv : ranges_) {
            if (kv.second.def_block < 0) continue;   // unreachable: never allocated
            order_.push_back(kv.first);
        }
        std::sort(order_.begin(), order_.end(), [&](lithon::ir::ValueId a, lithon::ir::ValueId b) {
            const LiveRange& ra = ranges_.at(a);
            const LiveRange& rb = ranges_.at(b);
            if (ra.def_block != rb.def_block) return ra.def_block < rb.def_block;
            if (ra.def_pos != rb.def_pos) return ra.def_pos < rb.def_pos;
            return a < b;
        });
    }
};

}  // namespace lithon::jit