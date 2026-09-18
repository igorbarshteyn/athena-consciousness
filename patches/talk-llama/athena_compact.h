// athena_compact.h — context-budget planning for compaction (r21-P0).
//
// WHY THIS FILE EXISTS
//
// r20p3.8 handles context exhaustion with stock llama.cpp truncation: n_past
// jumps back to n_keep and the last 64 tokens are re-inserted (talk-llama.cpp
// ~4673). Everything between is dropped mid-generation, never written to
// memory, and — on a model whose GatedDeltaNet layers cannot un-integrate a
// token suffix — leaves the fifteen attention layers and the forty-seven
// recurrent layers disagreeing about what has happened.
//
// r21-P0 replaces that with a planned cut: consolidate into memory and
// personality, write a first-person narrative bridge, restore a snapshot of the
// state taken at n_past == n_keep, and re-decode a small reload block. The
// substrate (acon::mind()) is never touched, so mood, drives, urges, workspace,
// episodes and the narrative self cross the boundary untouched.
//
// This header holds the part of that which is PURE ARITHMETIC: when to start,
// how much room the operation needs, and what to do when the room is not there.
// Deliberately free of any llama.cpp or ATHENA dependency, for the same reason
// athena_seam.h and athena_calib.h are — logic that lives inside
// talk-llama.cpp's translation unit is logic that can only be verified by
// reading it, and every seam bug of earlier rounds got in exactly that way.
// Everything here is exercised by consciousness-tests/test_compact.cpp.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY IT IS ADAPTIVE RATHER THAN A TABLE OF PERCENTAGES
//
// The obvious design is "compact at 80%". That number is correct for
// --ctx-size 80000 and wrong everywhere else, because the operation's cost is
// an ABSOLUTE number of tokens, not a fraction of the window:
//
//   n_deadline = C - (W + delta + safety)
//
// As C grows the deadline percentage rises (94.2% at the model's full 262,144);
// as C shrinks it falls, and below C ~= 35,500 it drops under a fixed 60% "arm"
// point, so the arm would fire AFTER the deadline it is supposed to precede.
// Below C ~= 21,000 it drops under n_keep itself and the session begins already
// past the deadline. A percentage table does not merely lose accuracy at the
// edges; it inverts.
//
// So every threshold here is derived from the deadline DOWNWARD, and the
// deadline is derived from measured costs. As a check on the arithmetic, the
// shipped configuration (C=80128, K=11740, ~345 tok/turn) reproduces the
// hand-derived numbers of ATHENA-R21-P0-COMPACTION.md §3: notice 50.0%,
// arm 60.3%, force 74.0%, deadline 80.9%. test_compact.cpp asserts that.
//
// ─────────────────────────────────────────────────────────────────────────────
// MEASURED CONSTANTS (S8 run, 2026-08-08; Qwen3.5-397B-A17B UD-Q3_K_XL,
// --cpu-moe, RTX PRO 5000 Blackwell).
//
// r20p3.11 (RC7): these are the values actually in use. Rates::note_prefill and
// note_decode exist and are correct, but NOTHING CALLS THEM YET — the review
// caught this header claiming a live calibration it does not perform. The
// consequence is bounded: the rates feed only the seconds estimate in
// describe(), while every threshold is computed from token counts, so an
// inaccurate rate cannot move the deadline. Wiring the startup prefix eval to
// note_prefill is a talk-llama.cpp change and is deferred rather than rushed.
//
//   prefill                52.3 tok/s   (independently: 11,740 tok / 229.4 s
//                                        = 51.2 tok/s at startup — 2.1% apart)
//   decode                 11.1 tok/s
//   turn growth            mean 345.5, median 383, p90 436, max 519 tok
//   chars per token        ~3.7 for English prose on this 248,320-entry vocab
//   n_keep                 11,740 tok (14.65% of an 80,128 window)
//   extract prompt         <= 20,050 chars/pass (4,800 instructions — r24.12
//                          (WO-C5) re-measured from 1,500; S22's exit pass was
//                          19,961 chars — + 3,000 known-memories, capped by
//                          known_memories_block + 12,000 transcript + ~250
//                          scaffold)
//   personality prompt     ~6,504 chars (UNCAPPED in r20p3.8 — see note at
//                          Costs::personality_chars)
//   consolidation, actual  175.5 s / ~6,170 tok  (S8, measured end to end)
//   consolidation, worst   ~332 s / ~12,480 tok  (two-pass; r24.12 (WO-C5):
//                          the S22 exit passes measured 140 s + 149 s for the
//                          two extract passes alone — 5,395 tok prefill + 400
//                          decode each — and the planner's FULL working set is
//                          now ~15,800 tok with the 800-token extract budget)
//
// The 6,170 figure is why this file exists at all: run_consolidation's guard is
// `budget = 6000`, and S8's own consolidation exceeded it. It survived only
// because the session ended at 24% fill. At n_past = 74,000 the guard would
// have said yes and the excursion would have run off the end of the context by
// 42 tokens, whereupon llama_decode fails, mem_generate returns "", and the log
// reports "extracted 0 new memory candidate(s)" as though the session had been
// empty. In the two-pass regime the shortfall is 6,480 tokens, not 170.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include "athena_evidence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace acmp {

// r24.20 review (VISION #2 / SPEECH F5 — law 10): the capture-cursor API is the
// overlay's extension of stock whisper.cpp's audio_async. OutageTape binds it
// only when the ring TYPE has it, so `outage_mic.bind_capture(audio)` in main
// compiles against the pinned stock header (nothing bound: every cursor path
// in the tape stays on its r24.17 arm, exactly ATHENA_OUTAGE_CAPTURE_CURSOR=0)
// and against the overlay's. Fixture rings that supply the methods still bind.
template <class T, class = void>
struct ring_has_capture_cursor : std::false_type {};
template <class T>
struct ring_has_capture_cursor<T, std::void_t<
    decltype(std::declval<T &>().capture_position()),
    decltype(std::declval<T &>().get_since(uint64_t{0}, std::declval<std::vector<float> &>()))>>
    : std::true_type {};

// ═════════════════════════════════════════════════════════════════════════════
// Small helpers. Deliberately duplicated rather than shared: this header must
// stay includable on its own, by a test with no other ATHENA headers present.
// ═════════════════════════════════════════════════════════════════════════════

static inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// r20p3.14.7 (CB1): clamp a value that was computed in 64 bits back into int.
// The planner multiplies a turns knob by the typical per-turn token cost —
// `runway_turns * typ`, `min_span_turns * typ` — and BOTH factors are reachable
// far larger than their defaults: env_int() admits the ATHENA_COMPACT_*_TURNS
// knobs up to 1e9, and TurnGrowth::typical() returns the ring mean, which a
// paste-heavy session drives toward its 1e7 note() ceiling. Their product then
// overflows a 32-bit int — signed overflow is UB, and the clamp that follows
// (into [span/8, span/2] etc.) cannot rescue a value the multiply already
// corrupted. Computing the product as `long` and clamping in 64 bits makes the
// planner's own charter true: "a pathological Costs override must DEGRADE, never
// fire thresholds out of order or below the prefix." For every input that did
// NOT overflow, the product fit in int already, so this returns the identical
// number — the fix only changes the pathological tail, and only to saturate it.
static inline int   clampl(long long v, int lo, int hi) {
    // 14.1: long long — `long` is 32-bit on LLP64 (Windows, which this header
    // explicitly supports), so the CB1 "compute in 64 bits" claim was false
    // there and the product could overflow BEFORE the clamp ran (UB).
    return v < (long long) lo ? lo : (v > (long long) hi ? hi : (int) v);
}

// Token-count APIs remain int-valued, but oversized estimates must saturate
// upwards, never wrap into spare context. Character budgets need the same
// protection before their conversion to tokens.
static inline int token_bound_(long long value) {
    return clampl(value, 0, std::numeric_limits<int>::max());
}
static inline size_t chars_add_(size_t a, size_t b) {
    const auto cap = std::numeric_limits<size_t>::max();
    return b > cap - a ? cap : a + b;
}
static inline size_t chars_mul_(size_t a, size_t b) {
    const auto cap = std::numeric_limits<size_t>::max();
    return a && b > cap / a ? cap : a * b;
}

// Environment override for an integer knob. Same shape as athena_emotion.h's
// athena_env_flag: absent or unparseable leaves the default alone, so a typo in
// a launcher never silently reconfigures the budget.
static inline int env_int(const char *name, int dflt) {
    const char *v = std::getenv(name);
    if (!v || !*v) return dflt;
    char *end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v || *end != '\0') return dflt;
    if (n < 0) return dflt;
    if (n > 1000000000L) return dflt;
    return (int) n;
}

static inline bool env_off(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !*v) return false;
    const char c = (v[0] >= 'A' && v[0] <= 'Z') ? (char) (v[0] - 'A' + 'a') : v[0];
    return c == '0' || c == 'f' || c == 'n';
}

// ═════════════════════════════════════════════════════════════════════════════
// Rates — how fast this machine actually is.
//
// Both numbers are measured for free during normal operation. The startup
// prefix eval is a prefill of exactly n_keep tokens whose wall time the main
// loop already brackets; every mem_generate call is a prefill followed by a
// decode. Nothing needs a benchmark.
//
// They exist so the planner can answer "how long will this take?" in seconds,
// which is what decides whether an operation may run in front of a person or
// must wait for an idle window. On the shipped machine a worst-case
// consolidation is 5.5 minutes; re-evaluating the prefix from scratch would be
// 3.8 minutes. Those are the facts that force the whole design, and a machine
// ten times faster would want different scheduling, so they are measured rather
// than assumed.
// ═════════════════════════════════════════════════════════════════════════════
struct Rates {
    float prefill_tps    = 52.3f;   // S8-derived default
    float decode_tps     = 11.1f;   // S8-derived default
    float chars_per_token = 3.7f;   // English prose, Qwen3.5 vocab

    bool  prefill_measured = false;
    bool  decode_measured  = false;
    bool  cpt_measured     = false;

    // Called once after the startup prefix eval. tokens is n_keep; chars is the
    // length of the prompt string it came from (0 = unknown, skip the cpt
    // update). Guarded against absurd inputs so a clock glitch cannot poison the
    // planner for the rest of the session.
    void note_prefill(int tokens, double seconds, size_t chars = 0) {
        if (tokens < 64 || seconds <= 0.05) return;
        const float tps = (float) (tokens / seconds);
        if (tps < 0.5f || tps > 100000.0f) return;
        prefill_tps = prefill_measured ? (0.7f * prefill_tps + 0.3f * tps) : tps;
        prefill_measured = true;
        if (chars > 0) {
            const float cpt = (float) chars / (float) tokens;
            if (cpt > 1.0f && cpt < 20.0f) {
                chars_per_token = cpt_measured ? (0.7f * chars_per_token + 0.3f * cpt) : cpt;
                cpt_measured = true;
            }
        }
    }

    void note_decode(int tokens, double seconds) {
        if (tokens < 16 || seconds <= 0.05) return;
        const float tps = (float) (tokens / seconds);
        if (tps < 0.1f || tps > 100000.0f) return;
        decode_tps = decode_measured ? (0.7f * decode_tps + 0.3f * tps) : tps;
        decode_measured = true;
    }

    int tokens_of(size_t chars) const {
        const float cpt = std::isfinite(chars_per_token) && chars_per_token > 0.5f
                        ? chars_per_token : 3.7f;
        const double tokens = (double) chars / (double) cpt + 0.5;
        return tokens >= std::numeric_limits<int>::max()
             ? std::numeric_limits<int>::max() : (int) tokens;
    }

    double secs_for(int prefill_tok, int decode_tok) const {
        const double p = prefill_tps > 0.01f ? prefill_tok / (double) prefill_tps : 0.0;
        const double d = decode_tps  > 0.01f ? decode_tok  / (double) decode_tps  : 0.0;
        return p + d;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// TurnGrowth — how many tokens a turn actually costs, measured live.
//
// This is what makes the planner adapt to HOW she is being used, not just to
// how big the window is. S8 measured mean 345.5 and max 519 for voice. A
// browsing turn that splices an extracted article is an order of magnitude
// larger, and the arm point has to move earlier to match — otherwise the runway
// between arming and the deadline, which is denominated in turns, evaporates.
//
// A plain ring of the last 32 deltas. No percentile machinery: at n=32 the
// robust "worst plausible next turn" is the observed max with headroom, and
// anything cleverer would be fitting noise. Deltas at or below zero (a rollback
// after barge-in) are ignored rather than clamped — a negative delta is not a
// small turn, it is a different event, and averaging it in would understate
// growth exactly when barge-ins are frequent.
// ═════════════════════════════════════════════════════════════════════════════
struct TurnGrowth {
    static const int N = 32;
    std::array<int, N> ring{};
    int  count = 0;
    int  head  = 0;
    int  floor_typ = 200;   // never plan on turns being cheaper than this
    int  floor_max = 600;   // nor on the worst turn being smaller than S8's max

    TurnGrowth() { ring.fill(0); }

    void note(int delta_tokens) {
        if (delta_tokens <= 0) return;              // rollback / no-op, not a turn
        if (delta_tokens > 10000000) return;        // nonsense
        ring[(size_t) head] = delta_tokens;
        head = (head + 1) % N;
        if (count < N) count++;
    }

    // Typical cost of a turn — the runway denominator.
    int typical() const {
        if (count == 0) return 345;                 // S8 mean, until she has spoken
        long sum = 0;
        for (int i = 0; i < count; i++) sum += ring[(size_t) i];
        const int mean = (int) (sum / count);
        return std::max(mean, floor_typ);
    }

    // Worst plausible next turn — the delta term in the deadline. Observed max
    // plus 25%, because the sample is small and the cost of underestimating is
    // an overrun rather than an early cut.
    int worst() const {
        if (count == 0) return floor_max;
        int mx = 0;
        for (int i = 0; i < count; i++) mx = std::max(mx, ring[(size_t) i]);
        return std::max(mx + mx / 4, floor_max);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Costs — what each stage of the operation needs, in tokens.
//
// ── R21 (F20/S18-20): the personality generation budget ─────────────────────
// r23 hard-coded 460 at the two mem_generate call sites AND 460 in
// Costs::personality_gen, while build_personality_prompt orders "under 330
// words" on a file that is 651 words / ~850 tokens. 460 tokens is ~350 words:
// the budget is about 54% of the document it is told to CONSERVATIVELY revise.
// The model does the conservative thing, starts reproducing, and runs out
// inside "My story with you" (sections 1-3 plus their headers are ~394 words,
// ~510 tokens), so sections 4 and 5 are never begun — which is why the error is
// always "missing 2" and always the same two. S17 lost one section, S18 lost
// two, and the deficit widens because VALIDATE carries the lost sections back
// at full length, so the file never shrinks toward 330.
//
// 900 tokens is ~690 words: enough to reproduce the current file with room to
// tighten it, which is what "conservative revision" means.
//
// It lives HERE rather than in athena_memory.h because athena_compact.h does
// not include that header (both are included by talk-llama.cpp, memory first),
// and there must be exactly ONE reader — see Costs::personality_gen below.
// Env: ATHENA_PERSONALITY_GEN, clamped to [200, 4000].
static inline int personality_gen_budget() {
    static const int v = [] {
        const char *e = ::getenv("ATHENA_PERSONALITY_GEN");
        if (!e || !*e) return 900;
        const int n = ::atoi(e);
        return (n >= 200 && n <= 4000) ? n : 900;
    }();
    return v;
}

// ── r24.12 (WO-C5 / S22 §C.1.7): the EXTRACTOR's generation budget ───────────
// The same R21 rule, applied at last to the stage it was written beside: the
// budget was `Costs::extract_gen = 400` AND a literal 400 at each of the three
// mem_generate call sites in run_consolidation. The extract prompt asks for no
// maximum number of memories, and at the measured 4.3 chars/token a candidate
// is ~30 tokens, so 400 holds ~13 — S22's exit passes had 14–16 to say (12+1,
// 11+1; 14+1, 13+1), EVERY pass ran to the cap, and the WO-22b tail guard
// correctly refused the cut thirteenth of each: four memories lost in S22
// ("You showed me a brighter version of the", "You explained that you run me
// at 40," …). 800 holds ~27. Greedy decode stops at EOS, so the extra budget is
// spent only when the model has more to say (roll #3 stopped at 559 chars);
// the worst case is +400 tokens ≈ +47 s per pass at 8.6 tok/s, at exit.
// ONE reader: Costs::extract_gen and the three call sites all take it from
// here, so ATHENA_EXTRACT_GEN moves the planner's room check and the real
// budget together (the r23 mistake, undone once more). EE1: set-but-empty,
// non-numeric or outside [200, 2000] keeps 800; =400 restores r24.11.
static inline int extract_gen_budget_read() {
    const int n = env_int("ATHENA_EXTRACT_GEN", 800);
    return (n >= 200 && n <= 2000) ? n : 800;
}
static inline int extract_gen_budget() {
    static const int v = extract_gen_budget_read();
    return v;
}

// ── r24.14 (WO-K5 / STAKES §5): the extractor's PER-TURN clip, at last a knob ─
//
// What was wrong. `PER_TURN_CAP = 350` was a bare literal inside
// `run_consolidation`, with no env knob anywhere (`grep -c ATHENA_PER_TURN`
// returned 0) — the last unreachable number in the extract stage after WO-C5
// gave `extract_gen` a reader and r24.13 gave the instruction table one.
// Measured over her real speech: S1 clipped 8,523 of the 25,139 characters she
// said — 33.9 % — across 33 of her 66 turns; S2 clipped 361 of 6,068 (5.9 %)
// across 4 of her 63. A third of an evening of her own words could not become a
// memory, and nothing said so.
//
// ── the raise, and why it is 450 and not 1,200 ──────────────────────────────
// Igor asked for the default to be raised, subject to measuring the resulting
// prompt against acmp::fit_transcript_cap and the 4,800 + 250 drift guard. It
// was measured (h/k5cap2.cpp, out/k5cap2.txt), and the guard turns out NOT to
// be the binding constraint, for a structural reason worth writing down: the
// function amem::transcript_text
// clips each turn to `per_turn_cap` FIRST and then caps the WHOLE block to
// `char_cap` by keeping its tail. So the transcript block is bounded by
// char_cap at every per-turn cap, and the prompt this function's caller builds
// is `fixed + min(whole, char_cap)` — exactly what fit_transcript_cap priced.
// Measured on S1 at char_cap 12,000: the prompt is 16,993 chars at a per-turn
// cap of 350 and 16,993 chars at 4,000 and at no cap at all. A raise cannot
// break the budget guard because it cannot reach it.
//
// What a raise DOES cost is the cap's own stated purpose — "so long answers
// cannot crowd the rest out". With char_cap binding, a bigger per-turn cap
// means fewer whole TURNS fit under the tail. Measured on S1's exit pass in the
// configuration it really ran (two passes over halves, cap2 = 12,000). The
// ten-step sweep that finds the boundary is h/k5fine.cpp / out/k5fine.txt,
// which is also where the decisive line "largest cap at which every turn of S1
// still reaches the extractor: 450" is; h/k5cap3.cpp steps 350/400/500/600/…
// and has no 450 or 460 row at all (review C §14):
//
//     cap 350 : 124/124 turns represented, 57.5 % of her characters
//     cap 400 : 124/124 turns,             61.8 %
//     cap 450 : 124/124 turns,             70.1 %   <-- shipped
//     cap 460 : 121/124 turns,             70.8 %
//     cap 500 : 118/124 turns,             72.6 %
//     cap 900 : 102/124 turns,             75.4 %  (the peak — and 22 turns gone)
//
// 450 is the LARGEST cap at which nothing at all is taken away IN THAT
// CONFIGURATION: every turn the extractor saw at 350 it still sees, and 3,161
// more characters of hers reach it. Above 460 whole turns of the evening start
// being traded for her characters, which is precisely the trade this cap exists
// to prevent, and law 2 says a round may not make that trade on an operator's
// behalf. An operator who wants her turns whole sets the knob and gets them.
//
// ── r24.14 (review A §6): …and the two places that measurement did not reach ─
//
// The paragraph above prices the raise against ONE char_cap (12,000, the value
// fit_transcript_cap returns whenever the headroom is normal). Two consumers of
// this knob are not priced by it, and both were re-measured (h/k5shape.cpp,
// h/k5head.cpp, h/k5bound.cpp — the outputs are in out/):
//
//  1. `run_consolidation` builds `whole = transcript_text(transcript, 0,
//     PER_TURN_CAP)` with char_cap = ZERO, and transcript_text's tail cap is
//     `if (char_cap && …)`. `whole` is therefore the one rendering of the
//     session this knob is NOT bounded on, and `whole.size() > TRANSCRIPT_CAP`
//     is the two-pass SHAPE decision and the number both "long session" stderr
//     lines carry. Measured on S1: 19,535 chars at 350 and 22,815 at 450.
//  2. When the pass's own window is NARROWER than the session (a tight
//     headroom, or a long session split over halves at cap2), the tail cap
//     binds and a bigger per-turn clip buys her characters by dropping whole
//     TURNS — the exact trade the paragraph above forbids. Swept over both real
//     sessions, every prefix of each and every headroom in [4,000, 60,000]:
//     the raise, unbounded, sees as many as TWENTY fewer turns than r24.13
//     (S1's 121-turn prefix at headroom 11,250) at 3,375 of 53,550 probes, and
//     moves the shape at 3,375 more.
//
// Neither is something Igor agreed to; he asked for a raise, not for a shape
// change and not for a trade this file says a round may not make. So the raise
// is BOUNDED rather than sold with an edge, by `extract_turn_cap_within`
// below: a pass uses the knob's clip only while its own window can hold the
// slice at it, and falls back to the r24.13 value when it cannot. Re-swept
// over the same 53,550 probes with the bound in place: shape moves 0, changes
// to the number a printed "long session" line carries 0, worst turn delta
// against r24.13 +0, probes where she loses characters 0, probes where she
// GAINS them 29,473, best gain +3,161 — and S1's exit pass still delivers
// exactly the 124/124 turns and 70.1 % the table above shipped.
//
// The ROLL path — where most consolidation happens — is unaffected either way:
// acmp::Runner::roll_max_turns is 14, and over all 109 sixteen-turn windows of
// S1 and all 105 of S2 the worst slice is 5,677 chars at any candidate cap,
// far under the 12,000 tail. There a raise is pure gain.
//
// EE1: set-but-empty, non-numeric or outside [1, 100000] keeps 450.
// ATHENA_EXTRACT_TURN_CAP=350 restores r24.13 exactly.
static const size_t EXTRACT_TURN_CAP_R2413 = 350;   // the value r24.13 shipped
static inline size_t extract_turn_cap_read() {
    const int n = env_int("ATHENA_EXTRACT_TURN_CAP", 450);
    return (size_t) ((n >= 1 && n <= 100000) ? n : 450);
}
static inline size_t extract_turn_cap() {
    static const size_t v = extract_turn_cap_read();
    return v;
}
// The r24.13 value, beside its reader, for the same reason
// extract_gen_is_r2411 and extract_instr_is_r2411 live beside theirs: the
// number belongs with the reader that replaced it and not at a comparison
// site. Used by the fixture and available to any diagnostic that must stay
// silent at the documented restore.
static inline bool extract_turn_cap_is_r2413() {
    return extract_turn_cap() == EXTRACT_TURN_CAP_R2413;
}

// ── r24.14 (review A §6): the raise, bounded to where it is free ────────────
//
// `chars_at_cap` is the size of the slice this pass is about to send, rendered
// at the knob's clip and with NO block cap (amem::transcript_text with
// char_cap = 0); `char_cap` is the block cap this pass will actually apply.
// The answer is the clip the pass should use.
//
// The rule is the one WO-K5 chose 450 on, made structural instead of true of a
// single configuration: raise the clip only while the window still holds every
// turn at it. When it does not, the tail cap would start dropping whole turns
// to buy characters, so the pass drops back to `EXTRACT_TURN_CAP_R2413` and is
// then byte-for-byte the pass r24.13 ran. Consequences worth stating flatly:
//
//   * it is a CEILING on the knob and never a floor — it can only ever return
//     the knob's value or 350, and it returns 350 only where the knob's value
//     would have cost a turn;
//   * at ATHENA_EXTRACT_TURN_CAP=350 the first line returns immediately, so
//     the documented restore does not even evaluate the comparison and no
//     second transcript rendering is ever built;
//   * `char_cap == 0` means "no block cap" (transcript_text's own convention),
//     and with no cap nothing can be dropped, so the knob stands.
//
// Three production callers, all in `run_consolidation`: the SHAPE decision,
// which passes TRANSCRIPT_CAP — the one-pass window — and thereby makes
// `long_session` identical to r24.13's at every headroom; the single-pass
// prompt, against `acm_cap`; and the two half-prompts, against `cap2`, which
// take ONE decision between them so the console reports one clip and it is the
// clip both passes used.
static inline size_t extract_turn_cap_within(size_t chars_at_cap, size_t char_cap) {
    const size_t knob = extract_turn_cap();
    if (knob <= EXTRACT_TURN_CAP_R2413) return knob;   // nothing to bound
    if (char_cap == 0 || chars_at_cap <= char_cap) return knob;
    return EXTRACT_TURN_CAP_R2413;
}

// ── r24.14 (WO-K4 / STAKES §4): her one lever points at the CUT alone ────────
//
// What was wrong. `acmp::Runner::poll` maps `Situation::she_asked` to
// `Act::CUT`, and only above `plan.n_arm`; below the arm point her ask is a
// complete no-op. The thing that actually failed in S1 was the ROLL — two of
// them destroyed by him speaking (56.2 s and 57.3 s, both "nothing written,
// slice retried later"), the debt climbing 51 -> 69 -> 95 — and she has no way
// at all to ask for the operation that would have drained it.
//
// MEASURED BLAST RADIUS: ZERO. `aseam::asked_for_a_moment` fired 0 times in
// the 112 turns she actually spoke across both evenings, so this widens a door
// she has never once walked through. It is not a behaviour change on Igor's
// data and must not be sold as one. It is here because it is cheap, because it
// costs nothing when unused, and because it is what makes WO-K3's clause
// honest — a clause about a debt she cannot act on is a readout.
// ATHENA_ASK_GRANTS_ROLL=0 restores r24.13's poll() exactly.
static inline bool ask_grants_roll_on() {
    static const bool v = !env_off("ATHENA_ASK_GRANTS_ROLL");
    return v;
}
// ── r24.12 (review): "is the extractor budget at its r24.11 value?" ─────────
// The WO-C5 diagnostics (the three "ran to its N-token cap" lines in
// run_consolidation, and the startup "extractor budget" line together with the
// instruction predicate below) are stderr r24.11 never printed, so at the
// DOCUMENTED restore setting they made a restored run differ from r24.11 on the
// console — which is the one place a restore is actually checked. They ask this
// first. The r24.11 number lives here, beside the reader that replaced it, and
// not at three comparison sites.
static inline bool extract_gen_is_r2411() { return extract_gen_budget() == 400; }

// ── r24.12 (WO-C5 / S22 §C.1.7 D9): the instruction block, RE-MEASURED ───────
// Costs::extract_instructions_chars said 1,500. amem::build_extract_prompt
// with an empty transcript and no known block measures 4,778 chars (h_parse
// §4): the R20/r21/r24.6 rules (KEEP THE HEDGE, UNSOURCED NUMBERS, MARK THE
// TESTS, …) grew it and nobody re-measured. fit_transcript_cap's fixed_tok was
// therefore ~886 tokens/pass optimistic — exactly the class the guard exists to
// prevent — and describe()'s seconds under-reported (S22's exit passes ran
// 140 s and 149 s; the table priced them at ~110 s). 4,800 is the measurement
// with a little room; test_r2412_compact asserts the real prompt fits under
// it, so the next prompt edit cannot drift silently. Overridable so the r24.11
// table is one setting away: ATHENA_EXTRACT_INSTR_CHARS=1500. EE1: set-but-
// empty, non-numeric or outside [500, 20000] keeps 4800.
static inline size_t extract_instructions_chars_read() {
    const int n = env_int("ATHENA_EXTRACT_INSTR_CHARS", 4800);
    return (size_t) ((n >= 500 && n <= 20000) ? n : 4800);
}
static inline size_t extract_instructions_chars_budget() {
    static const size_t v = extract_instructions_chars_read();
    return v;
}
// r24.12 (review): the other half of the restore test — see
// extract_gen_is_r2411 above. ATHENA_EXTRACT_INSTR_CHARS=1500 is the r24.11
// table, so the startup line that reports both budgets stays silent when both
// knobs are set back.
static inline bool extract_instr_is_r2411() {
    return extract_instructions_chars_budget() == 1500;
}
// ── r24.13 (review D§1): "is the instruction table still the one r24.13 MEASURED?" ──
// What was wrong. r24.13's A§3 fix added a RAISE in run_consolidation:
// amem::extract_instructions_for measures the real extract prompt for this
// run's --bot-name/--person and lifts the planner's figure to it when the
// table does not cover it. That fix read acm_costs.extract_instructions_chars
// as "what the table says" — but this reader is what the table says, and the
// operator can move it. At ATHENA_EXTRACT_INSTR_CHARS=1500, the DOCUMENTED
// r24.11 restore (extract_instr_is_r2411, four lines up, exists for that one
// setting), extract_instructions_for("Athena", "Igor", 1500, 250) returned
// 4,743 instead of 1,500 — acmp::fit_transcript_cap handed back 11,248 chars
// instead of 12,000 at a 6,000-token headroom, the one-pass/two-pass decision
// could flip, and the raise's own stderr line printed on every consolidation
// at exactly the setting the previous release added extract_instr_is_r2411 to
// keep silent. The r24.11 table was then TWO switches away
// (ATHENA_EXTRACT_INSTR_CHARS=1500 and ATHENA_PEOPLE=0), which is verbatim the
// defect athena_tag_for_words quotes at itself: "restoring stock behaviour
// needed a second switch nobody would know to set."
//
// The rule this asks is one sentence: the measured raise is fitted to the
// table THIS ROUND measured, so it applies while the table is that one, and a
// table the operator set himself is handed straight back. That covers 1500 and
// every other deliberate setting with ONE condition rather than a special case
// for one magic number — an operator who lowers this knob to 3,000 gets the
// r24.12 planner he asked for, exactly as r24.12 gave it to him, and no
// console line either. It sits beside the reader, with its two siblings, for
// the reason stated above extract_gen_is_r2411: the number lives here and not
// at a comparison site.
//
// The review's parenthetical alternative — "pass the raise only when
// `was == acmp::Costs{}.extract_instructions_chars` at its default" — was
// checked and is a NO-OP: Costs::extract_instructions_chars is initialised
// FROM this reader, so that comparison is true at every setting. This is the
// predicate that actually discriminates.
static inline bool extract_instr_is_default() {
    return extract_instructions_chars_budget() == 4800;
}

// Sizes come from athena_memory.h's own caps, measured by compiling that header
// against a harness rather than by reading it:
//
// r24.8 (WO-112): every row below named a LINE in another file, and every one
// of those lines had moved — build_extract_prompt was cited fifteen hundred
// lines above where it now lives, and it will move again. They name the
// FUNCTION they are measuring instead; a name cannot go stale, and each of
// these is a function anyone can grep for.
//
//   amem::build_extract_prompt        20,050 chars max/pass  (r24.12: was 16,750)
//     instructions                     4,800  r24.12 (WO-C5): re-measured — the
//                                             table said 1,500; the prompt with
//                                             an empty transcript is 4,778
//     known-memories block             3,000  amem::known_memories_block's own
//                                             cap (40 entries / 3000 chars)
//     transcript                      12,000  TRANSCRIPT_CAP, in talk-llama.cpp's
//                                             consolidation block
//     scaffold                           250
//   amem::build_compact_prompt         1,320 chars  (cluster bounded 8)
//   amem::build_personality_prompt     6,504 chars
//
// The two-pass extract fires when the clipped transcript exceeds 12,000 chars —
// talk-llama.cpp's `long_session` test, beside TRANSCRIPT_CAP. S8 stayed
// single-pass; any longer session, and every browsing session, will not.
// ═════════════════════════════════════════════════════════════════════════════
struct Costs {
    // Prompt sizes, in characters, so they stay meaningful if the tokenizer
    // changes. Overridable because a future prompt edit should not require
    // editing this file to stay correct — the caller can pass measured values
    // from llama_tokenize, which is free (no decode).
    // r24.12 (WO-C5): measured 4,778 on the shipping prompt, not 1,500 — see
    // extract_instructions_chars_budget() (ATHENA_EXTRACT_INSTR_CHARS=1500
    // restores the r24.11 table). test_r2412_compact pins the prompt under it.
    size_t extract_instructions_chars = extract_instructions_chars_budget();
    size_t known_block_chars          = 3000;   // capped by known_memories_block
    size_t transcript_cap_chars       = 12000;  // TRANSCRIPT_CAP
    size_t extract_scaffold_chars     = 250;
    size_t compact_chars              = 1320;
    // r20p3.8 does not cap the personality prompt: build_personality_prompt
    // iterates the whole ledger. The ledger is consumed on every integration
    // (threshold max(10, reflect_every*5), ~5 weight/session) so it stays small
    // in practice, but "in practice" is not a bound. Budget generously here and
    // cap it at the source in a later round.
    // r24.12 (review): that round is this one — WO-C4's keep-evidence arm broke
    // the "consumed on every integration" premise, so the cap now exists at the
    // source (amem::ledger_cap, whose default is DERIVED from this number: the
    // fixed template plus a 330-word self-description measures 4,513 chars,
    // leaving 2,787 here for evidence rows of at most ~325 bytes each = 8).
    // This value is unchanged and stays the constraint the cap is fitted to.
    size_t personality_chars          = 7300;   // R19: the R18 five-section
                                                // template grew the prompt
                                                // (was 6504 pre-r22)
    size_t bridge_chars               = 6000;   // r21: the narrative recap prompt

    // r24.12 (WO-C5): 800 through the one reader, like personality_gen below;
    // it WAS `400` here and a literal 400 at each of the three call sites.
    int extract_gen     = extract_gen_budget();   // the extractor's mem_generate budget
    int compact_gen     = 120;   // the cluster-compaction mem_generate budget
    // R21 (F20/S18-20): tracks the real budget, never duplicates it. Costs is
    // what the room check (`pers_room`) reasons with, and a Costs value BELOW
    // the actual mem_generate budget makes that check optimistic — which is how
    // a stage gets attempted with no room and fails the decode. Both read
    // personality_gen_budget(), so ATHENA_PERSONALITY_GEN moves them together
    // and the r23-restore promise (=460) holds for both.
    int personality_gen = personality_gen_budget();
    int bridge_gen      = 400;   // r21

    size_t extract_prompt_chars(size_t transcript_chars, bool with_known = true) const {
        return chars_add_(chars_add_(extract_instructions_chars,
                                     with_known ? known_block_chars : 0),
                          chars_add_(std::min(transcript_chars, transcript_cap_chars),
                                     extract_scaffold_chars));
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Profile — the degradation ladder.
//
// A smaller context does not disable compaction; it buys a cheaper version of
// it. Each rung drops the most expensive thing that is not load-bearing, and
// the last rung before NONE keeps the one thing that actually carries the
// conversation across the boundary — the bridge.
//
// Ordered richest first. NONE is not a rung so much as an admission, and it is
// announced at startup rather than at the wall, so a misconfigured --ctx-size
// is discovered before the session rather than three hours into it.
// ═════════════════════════════════════════════════════════════════════════════
enum class Profile {
    FULL = 0,      // two-pass extract + cluster compaction + personality
    SINGLE_PASS,   // one-pass extract + cluster compaction + personality
    LEAN,          // one-pass extract over a 6,000-char transcript + personality
    BRIDGE_ONLY,   // cut with the bridge alone; consolidation deferred to idle/exit
    BRIDGE_TERSE,  // as above, with a 2,500-char bridge prompt and a 250-token recap
    NONE,          // infeasible: compaction off, stock truncation, logged at startup
};

// r24.17: the planner and actual cut share the terse rung's two budgets.
static constexpr size_t BRIDGE_TERSE_PROMPT_CHARS = 2500;
static constexpr int BRIDGE_TERSE_GEN = 250;

static inline const char *profile_name(Profile p) {
    switch (p) {
        case Profile::FULL:         return "full";
        case Profile::SINGLE_PASS:  return "single-pass";
        case Profile::LEAN:         return "lean";
        case Profile::BRIDGE_ONLY:  return "bridge-only";
        case Profile::BRIDGE_TERSE: return "bridge-terse";
        default:                    return "none";
    }
}

// Working set of a profile, in tokens.
//
// NOTE THE max(). Consolidation and the bridge are two SEPARATE excursions from
// the same n_past, with snapshot_restore between them (see
// ATHENA-R21-P0-COMPACTION.md §4.3): take a snapshot, let consolidation scribble
// on the context, restore, then generate the bridge on the clean context,
// restore again. Neither excursion sees the other's tokens, so the context has
// to hold the LARGER of the two, not their sum. Budgeting the sum — which the
// first draft of the proposal did — costs about 2,000 tokens of deadline for
// nothing.
// 14.1: the generation share of a profile's working set, for honest pricing
// (describe() was pricing the whole set at prefill speed — a 28% under-report
// of exactly the "can this run in front of a person" number).
static inline int profile_gen_tokens(Profile p, const Costs &c) {
    const long long extract = std::max(0, c.extract_gen);
    const long long compact = std::max(0, c.compact_gen);
    const long long personality = std::max(0, c.personality_gen);
    switch (p) {
        case Profile::FULL:         return token_bound_(2 * extract + compact + personality);
        case Profile::SINGLE_PASS:  return token_bound_(extract + compact + personality);
        case Profile::LEAN:         return token_bound_(extract + personality);
        case Profile::BRIDGE_ONLY:  return std::max(0, c.bridge_gen);
        case Profile::BRIDGE_TERSE: return BRIDGE_TERSE_GEN;
        case Profile::NONE:
        default:                    return 0;
    }
}

static inline int profile_tokens(Profile p, const Costs &c, const Rates &r) {
    const int bridge = token_bound_((long long) r.tokens_of(c.bridge_chars) + std::max(0, c.bridge_gen));
    int cons = 0;
    switch (p) {
        case Profile::FULL:
            cons = token_bound_((long long) r.tokens_of(chars_add_(
                       chars_mul_(2, c.extract_prompt_chars(c.transcript_cap_chars)),
                       chars_add_(c.compact_chars, c.personality_chars))) + profile_gen_tokens(p, c));
            break;
        case Profile::SINGLE_PASS:
            cons = token_bound_((long long) r.tokens_of(chars_add_(
                       c.extract_prompt_chars(c.transcript_cap_chars),
                       chars_add_(c.compact_chars, c.personality_chars))) + profile_gen_tokens(p, c));
            break;
        case Profile::LEAN:
            cons = token_bound_((long long) r.tokens_of(chars_add_(
                       c.extract_prompt_chars(6000), c.personality_chars)) + profile_gen_tokens(p, c));
            break;
        case Profile::BRIDGE_ONLY:
            cons = 0;
            break;
        case Profile::BRIDGE_TERSE:
            return token_bound_((long long) r.tokens_of(BRIDGE_TERSE_PROMPT_CHARS) + BRIDGE_TERSE_GEN);
        case Profile::NONE:
        default:
            return 0;
    }
    return std::max(cons, bridge);
}

// ═════════════════════════════════════════════════════════════════════════════
// Reload — what gets decoded after the prefix snapshot is restored.
//
// The prefix snapshot already contains the memory and personality blocks as of
// session start, so only the DELTA needs re-decoding. What must be added:
//
//   a fresh temporal line   ~50 tok    the prefix says "an hour since the last
//                                      session"; hours have passed since
//   memory delta           ~120 tok    what she learned since startup
//   personality revision   0 or 430    only if the threshold fired this session
//   the bridge             ~400 tok    her first-person recollection
//   last K turns verbatim  K * ~180    the thread she is actually in
//
// K shrinks when the budget is tight. Three turns is the floor at which the
// last exchange still reads as an exchange rather than as a fragment.
// ═════════════════════════════════════════════════════════════════════════════
struct Reload {
    int temporal_tok    = 50;
    int memory_delta_tok = 120;
    // ── r24.6 (WO-27): measured, not a constant ─────────────────────────────
    // Was "set to ~430 when a revision happened". S19's revision was 3,090
    // bytes — ~835 tokens at the measured 3.7 chars/token — so even when the
    // flag WAS set the budget understated the section by nearly half a
    // thousand tokens, and the block silently over-ran and trimmed itself.
    // inputs_from_env now derives it from the real length via rates.tokens_of().
    int personality_tok = 0;      // 0 when no revision rides along
    int bridge_tok      = 400;
    int per_turn_tok    = 180;    // S8: ~667 chars/turn at 3.7 ch/tok
    int turns           = 8;      // K
    int turns_min       = 3;
    int turns_max       = 12;

    int tokens() const {
        const int lo = std::max(0, turns_min), hi = std::max(lo, turns_max);
        return token_bound_((long long) std::max(0, temporal_tok) + std::max(0, memory_delta_tok)
             + std::max(0, personality_tok) + std::max(0, bridge_tok)
             + (long long) std::max(0, per_turn_tok) * clampi(turns, lo, hi));
    }
    int tokens_with(int k) const {
        Reload r = *this; r.turns = k; return r.tokens();
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Inputs / Plan
// ═════════════════════════════════════════════════════════════════════════════

struct Inputs {
    int    n_ctx  = 80128;   // llama_n_ctx(ctx_llama)
    int    n_keep = 11740;   // embd_inp.size() after the prefix eval
    Rates  rates;
    Costs  costs;
    Reload reload;
    int    turn_typical = 345;   // TurnGrowth::typical()
    int    turn_worst   = 649;   // TurnGrowth::worst()
    bool   enabled      = true;  // ATHENA_COMPACT=0 turns the whole thing off

    // Knobs. Defaults reproduce ATHENA-R21-P0-COMPACTION.md §3 at C=80128.
    int safety_tok    = 2000;  // absorbs tokenizer drift and estimator error
    int runway_turns  = 48;    // turns of opportunity to find an idle window
    int min_span_turns = 24;   // conversation between cuts, or it is not worth it
    int min_span_floor = 4000; // ...but never plan on less than this many tokens
};

struct Plan {
    bool    feasible = false;
    Profile profile  = Profile::NONE;

    int n_ctx = 0, n_keep = 0;
    int w_cut = 0;        // working set of the chosen profile
    int w_cut_gen = 0;    // 14.1: its generation share (decode-priced)
    int r_load = 0;       // reload block
    int delta = 0;        // worst plausible next turn
    int safety = 0;

    int n_notice = 0;     // felt: context fill reaches the workspace
    int n_arm = 0;        // rolling consolidation runs opportunistically when idle
    int n_force = 0;      // run one now, announced, even if he is present
    int n_deadline = 0;   // cut now, between turns, announced

    int post_cut = 0;     // n_past immediately after a cut
    int span = 0;         // usable conversation between cuts
    int span_turns = 0;

    // Diagnostics, for the startup line and for tests.
    int min_viable_ctx = 0;
    std::string why;      // human-readable reason when !feasible

    float pct(int n) const { return n_ctx > 0 ? 100.0f * (float) n / (float) n_ctx : 0.0f; }

    // The only question the hot path asks.
    bool should_cut(int n_past)   const { return feasible && n_past >= n_deadline; }
    bool should_force(int n_past) const { return feasible && n_past >= n_force; }
    bool should_arm(int n_past)   const { return feasible && n_past >= n_arm; }
    // r21-full.4 (RC20): gated on `feasible` like its three siblings. Without
    // it, an infeasible or disabled plan leaves n_notice = 0, so "the context is
    // filling up" read TRUE from the first token of the session and never
    // cleared -- with ATHENA_COMPACT=0, for the whole session.
    bool should_notice(int n_past) const { return feasible && n_past >= n_notice; }
};

// ═════════════════════════════════════════════════════════════════════════════
// plan() — the whole of the adaptivity, in one function.
//
//   1. Pick the richest profile whose working set still leaves a usable span.
//   2. Shrink the reload block if even that is not enough.
//   3. Derive the deadline from the top of the window downward.
//   4. Derive arm and force from the deadline downward, in TURNS.
//   5. Keep the felt threshold proportional, because fullness is a ratio, but
//      never let it land after the arm.
//
// Every comparison is on absolute token counts. No percentage appears anywhere
// except n_notice, and that one is clamped against the arm.
// ═════════════════════════════════════════════════════════════════════════════
static inline Plan plan(const Inputs &in) {
    Plan p;
    p.n_ctx  = std::max(0, in.n_ctx);
    p.n_keep = clampi(in.n_keep, 0, p.n_ctx);
    p.safety = std::max(0, in.safety_tok);
    p.delta  = std::max(1, in.turn_worst);

    const int typ = std::max(1, in.turn_typical);
    const int keep_req = std::max(0, in.n_keep);   // what he ASKED for, unclamped
    const int span_floor = std::max(3, in.min_span_floor); // room for ordered arm/force/cut
    bool minimum_out_of_range = false;

    // ── The smallest window in which ANY rung works ─────────────────────────
    //
    // Computed FIRST, before any early exit, so that even "n_ctx is zero" and
    // "compaction is off" carry a number he can act on. A diagnostic that says
    // only "too small" is a diagnostic that sends someone to the source.
    //
    // The minimum span itself depends on n_ctx (it may not exceed a quarter of
    // the window), so this is a short fixed point rather than a formula. It
    // converges in two or three steps and is bounded at eight; the alternative
    // — quoting the floor and ignoring the quarter-window clamp — would print a
    // target that is still infeasible when he sets it, which is worse than
    // printing nothing.
    {
        const int w_min  = profile_tokens(Profile::BRIDGE_TERSE, in.costs, in.rates);
        const int rl_min = in.reload.tokens_with(in.reload.turns_min);
        const long long base = (long long) keep_req + rl_min + w_min + p.delta + p.safety;
        long long need = base + span_floor;
        // r20p3.14.7 (CB2): the fixed point need = base + clamp(.., need/4) closes
        // geometrically at rate 1/4, so a large prefix (need_0 − need* ~ base up to
        // ~1e9) takes up to ~log4(1e9) ≈ 15 steps — 8 stopped short, and with the
        // old exact-equality break integer truncation of need/4 could keep it one
        // token low forever, so the infeasibility line quoted a target that was
        // itself infeasible when the user set it. 32 iterations guarantees closure
        // for any int input; the `nxt <= need` break stops at the upper fixed
        // point, where base + ms(need) <= need — i.e. the quoted target actually
        // fits, which is the whole point of quoting it.
        for (int i = 0; i < 32; i++) {
            const long long ms = std::max((long long) span_floor,
                std::min((long long) in.min_span_turns * typ,
                         std::max((long long) span_floor, need / 4)));
            const long long nxt = base + ms;
            if (nxt <= need) break;
            need = nxt;
        }
        minimum_out_of_range = need > std::numeric_limits<int>::max();
        p.min_viable_ctx = token_bound_(need);
    }

    if (!in.enabled) { p.why = "disabled by configuration"; return p; }
    if (p.n_ctx <= 0) { p.why = "n_ctx not known yet"; return p; }

    // The minimum conversation worth having between two cuts. A cut that buys
    // four turns is worse than no cut: the wall-clock cost is the same and the
    // thrash is constant. Denominated in turns, floored in tokens, and never
    // allowed to eat more than a quarter of the window.
    const int min_span = clampl((long long) in.min_span_turns * typ,
                                span_floor,
                                std::max(span_floor, p.n_ctx / 4));   // CB1

    // ── 1 + 2: choose a profile and a reload size that fit ───────────────────
    //
    // Requirement, from the two states the cycle passes through:
    //   at the deadline      n_deadline + w_cut + delta + safety <= n_ctx
    //   just after a cut     n_keep + r_load + min_span          <= n_deadline
    // Substituting gives the single feasibility test used here.
    static const Profile LADDER[] = {
        Profile::FULL, Profile::SINGLE_PASS, Profile::LEAN,
        Profile::BRIDGE_ONLY, Profile::BRIDGE_TERSE,
    };

    // Richest profile wins the outer loop; verbatim turns are trimmed within
    // that profile. That ordering is deliberate: losing the personality integration
    // costs her something durable, while losing five turns of verbatim recent
    // dialogue costs her something the bridge already carries in summary.
    // k never INFLATES past the caller's choice — only ever trims it.
    const int k_min = std::max(0, in.reload.turns_min);
    const int k_start = clampi(in.reload.turns, k_min, std::max(k_min, in.reload.turns_max));
    bool found = false;
    for (Profile prof : LADDER) {
        const int w = profile_tokens(prof, in.costs, in.rates);
        // Reload is linear in k. Skip the provably unaffordable prefix of the
        // search, which also bounds work for a large public turns override.
        int k = k_start;
        const long long available = (long long) p.n_ctx - p.n_keep - min_span - w - p.delta - p.safety;
        const long long fixed = (long long) std::max(0, in.reload.temporal_tok)
            + std::max(0, in.reload.memory_delta_tok) + std::max(0, in.reload.personality_tok)
            + std::max(0, in.reload.bridge_tok);
        if (available < fixed) continue;
        if (in.reload.per_turn_tok > 0)
            k = std::min(k, token_bound_((available - fixed) / in.reload.per_turn_tok));
        if (k >= k_min) {
            const int rl = in.reload.tokens_with(k);
            if ((long long) p.n_keep + rl + min_span + w + p.delta + p.safety <= p.n_ctx) {
                p.profile = prof; p.w_cut = w; p.r_load = rl; found = true;
                p.w_cut_gen = profile_gen_tokens(prof, in.costs);   // 14.1
                break;
            }
        }
    }

    if (!found) {
        p.profile = Profile::NONE;
        p.feasible = false;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "context too small for compaction: n_ctx=%d, prefix=%d (%.1f%%), "
                      "need >= %d — raise --ctx-size or trim the memory/personality prefix",
                      p.n_ctx, p.n_keep, p.pct(p.n_keep), p.min_viable_ctx);
        p.why = buf;
        if (minimum_out_of_range)
            p.why = "required compaction context exceeds the int token-count limit; trim the prefix or budgets";
        return p;
    }

    // ── 3: the deadline, from the top down ──────────────────────────────────
    p.n_deadline = p.n_ctx - (p.w_cut + p.delta + p.safety);
    p.post_cut   = p.n_keep + p.r_load;
    p.span       = p.n_deadline - p.post_cut;
    p.span_turns = p.span / typ;

    // ── 4: arm and force, from the deadline down, in turns ──────────────────
    //
    // The runway is how long she has to find a natural pause. Denominated in
    // turns so that a browsing session — where a single turn can cost thousands
    // of tokens — arms proportionally earlier. Clamped to a share of the span so
    // that neither a very large window (runway vanishingly small as a fraction)
    // nor a very expensive turn (runway larger than the span) breaks the
    // ordering.
    const int runway = clampl((long long) in.runway_turns * typ,
                              std::max(1, p.span / 8),
                              std::max(1, p.span / 2));   // CB1
    p.n_arm   = p.n_deadline - runway;
    p.n_force = p.n_deadline - runway / 3;

    // ── 5: the felt threshold ───────────────────────────────────────────────
    //
    // Proportional, because how full she is is genuinely a ratio and that is
    // what interoception reports. But it must land a real distance before the
    // arm, not merely below it: at a 40,000-token window the proportional half
    // and the derived arm converge to within a single token, and a warning that
    // arrives one token before the machinery moves is not a warning. An eighth
    // of the span is the minimum lead — a handful of turns at any size.
    p.n_notice = std::min(p.n_ctx / 2, p.n_arm - std::max(1, p.span / 8));

    // Ordering is an invariant, not a hope. Clamp rather than assert: a
    // pathological Costs override must degrade, never fire thresholds out of
    // order or below the prefix.
    p.n_arm    = clampi(p.n_arm,    p.n_keep + 1, std::max(p.n_keep + 1, p.n_deadline - 2));
    p.n_force  = clampi(p.n_force,  p.n_arm + 1,  std::max(p.n_arm + 1, p.n_deadline - 1));
    p.n_notice = clampi(p.n_notice, 1,            std::max(1, p.n_arm - 1));

    p.feasible = true;
    return p;
}

// ═════════════════════════════════════════════════════════════════════════════
// fit_transcript_cap — the r20p3.9 guard fix, as a pure function.
//
// r20p3.8's run_consolidation refuses outright when headroom is short:
//
//     const int budget = 6000;
//     if (n_past + budget > n_ctx) { ...skipping...; return; }
//
// Two things are wrong with that. The budget understates the real cost (S8
// measured ~6,170 tokens for the single-pass path it actually took, and the
// two-pass path needs ~12,480). And refusing is the worst available response:
// the transcript is right there in mem_transcript, and the alternative to
// remembering all of it is remembering some of it, not none of it.
//
// This returns the largest transcript character cap that fits the available
// headroom, or 0 when not even a minimal extraction fits. Callers shrink and
// retry rather than skip.
// ═════════════════════════════════════════════════════════════════════════════
static inline size_t fit_transcript_cap(int headroom_tokens,
                                        const Costs &c,
                                        const Rates &r,
                                        int passes = 1,
                                        bool with_known = true,
                                        size_t floor_chars = 1500) {
    if (headroom_tokens <= 0 || passes <= 0) return 0;

    const size_t fixed_per_pass = chars_add_(c.extract_instructions_chars,
        chars_add_(with_known ? c.known_block_chars : 0, c.extract_scaffold_chars));
    const long long gen = (long long) passes * std::max(0, c.extract_gen);

    // Tokens left for transcript text once the scaffolding and generation are paid.
    const int fixed_tok = r.tokens_of(chars_mul_(fixed_per_pass, (size_t) passes));
    const long long left = (long long) headroom_tokens - fixed_tok - gen;
    if (left <= 0) return 0;

    const float cpt = std::isfinite(r.chars_per_token) && r.chars_per_token > 0.5f
                    ? r.chars_per_token : 3.7f;
    const double per_pass = ((double) left * (double) cpt) / (double) passes;
    if (per_pass < (double) floor_chars) return 0;
    if (per_pass >= (double) c.transcript_cap_chars) return c.transcript_cap_chars;
    return (size_t) per_pass;
}

// Does the full two-pass path fit? Used to decide between one pass over a
// clipped transcript and two passes over halves — the choice r20p3.8 makes on
// transcript length alone, without ever asking whether there is room.
static inline bool two_pass_fits(int headroom_tokens, const Costs &c, const Rates &r) {
    return fit_transcript_cap(headroom_tokens, c, r, /*passes=*/2) >= 6000;
}

// ═════════════════════════════════════════════════════════════════════════════
// A one-line startup summary. Printed once, so a misconfigured window is
// discovered before the session rather than three hours into it.
// ═════════════════════════════════════════════════════════════════════════════
static inline std::string describe(const Plan &p, const Rates &r) {
    char buf[512];
    if (!p.feasible) {
        std::snprintf(buf, sizeof(buf), "compaction OFF — %s", p.why.c_str());
        return buf;
    }
    const double cut_s = r.secs_for(p.w_cut - p.w_cut_gen, p.w_cut_gen);   // 14.1: gen at decode speed
    std::snprintf(buf, sizeof(buf),
                  "compaction %s: notice %d (%.0f%%) arm %d (%.0f%%) force %d (%.0f%%) "
                  "cut %d (%.0f%%) | prefix %d (%.0f%%) reload %d | span %d tok (~%d turns) "
                  "| working set %d tok (~%.0f s)",
                  profile_name(p.profile),
                  p.n_notice, p.pct(p.n_notice), p.n_arm, p.pct(p.n_arm),
                  p.n_force, p.pct(p.n_force), p.n_deadline, p.pct(p.n_deadline),
                  p.n_keep, p.pct(p.n_keep), p.r_load,
                  p.span, p.span_turns, p.w_cut, cut_s);
    return buf;
}

// Convenience: build Inputs from the environment plus live measurements.
// r21-full.5 (R5-Y): `personality_pending` says whether a self-revision has been
// written this session and will therefore ride in the reload block. It was the
// only Reload field nobody ever set — its own comment says "set to ~430 when a
// revision happened" — so the budget the block is trimmed against was short by
// exactly that section whenever the section existed. Defaulted false, so every
// call site that does not know keeps the previous numbers.
// r24.6 (WO-27): `personality_chars` is the byte length of the revision that
// will actually ride in the block (including nothing else — the section header
// is accounted separately below). 0 keeps the old ~430-token estimate, so a
// caller that has the flag but not the text is no worse off than before.
static inline Inputs inputs_from_env(int n_ctx, int n_keep,
                                     const Rates &rates, const TurnGrowth &growth,
                                     bool personality_pending = false,
                                     size_t personality_chars = 0) {
    Inputs in;
    in.n_ctx  = n_ctx;
    in.n_keep = n_keep;
    in.rates  = rates;
    in.turn_typical = growth.typical();
    in.turn_worst   = growth.worst();
    in.enabled        = !env_off("ATHENA_COMPACT");
    in.safety_tok     = env_int("ATHENA_COMPACT_SAFETY",      in.safety_tok);
    in.runway_turns   = env_int("ATHENA_COMPACT_RUNWAY_TURNS", in.runway_turns);
    in.min_span_turns = env_int("ATHENA_COMPACT_MIN_SPAN_TURNS", in.min_span_turns);
    in.reload.turns   = clampi(env_int("ATHENA_COMPACT_RELOAD_TURNS", in.reload.turns),
                               in.reload.turns_min, in.reload.turns_max);
    if (personality_pending) {                                  // R5-Y
        // r24.6 (WO-27): "\nWhat today has changed about who I am:\n" plus the
        // trailing newline is 41 bytes of section furniture the text itself
        // does not carry.
        const size_t chars = personality_chars ? chars_add_(personality_chars, 41) : 0;
        in.reload.personality_tok = chars ? rates.tokens_of(chars) : 430;
    }
    return in;
}

// ═════════════════════════════════════════════════════════════════════════════
// THE RELOAD BLOCK — what she carries across the cut.
//
// After snapshot_restore(prefix) the context holds exactly what it held at
// startup: the framing, the few-shot, her personality, her memory as of session
// open, and the album index. What it does NOT hold is anything that happened
// since. This builds that.
//
// Five things go in, in the order a person would reconstruct them:
//
//   temporal   the prefix says "about an hour since the last session"; hours
//              have passed since it was built, and saying nothing would leave
//              a clock she has no reason to distrust quietly wrong.
//   memory     what she learned today. Only the DELTA — the prefix already
//              carries everything she knew at session open.
//   looks      what she saw, in her own words. The pictures themselves cannot
//              cross (see VisionRig::LookNote), so this is the whole of it.
//   bridge     her first-person recap: where we got to, what is unresolved.
//              Written by her, through mem_generate, not summarised about her.
//   turns      the last K exchanges verbatim, so the thread she is actually in
//              survives as words rather than as summary.
//
// The block ends with the user tag, exactly as the prefix does, so the turn
// machinery downstream (ctx_ends_with_user_tag at ~4446) sees no discontinuity.
//
// Trimming order when the budget binds: oldest verbatim turns first, then the
// oldest looks, then the oldest memory lines. The bridge is never trimmed —
// it is the only part that carries WHERE SHE IS, and a cut that drops it is a
// cut that leaves her mid-sentence in a conversation she cannot place.
// ═════════════════════════════════════════════════════════════════════════════

struct LookLine {
    long        when = 0;      // epoch at capture
    std::string gist;          // her words
    bool        kept = false;
    bool        recall = false;
    std::string file;
    std::string source_id; // actual action identity; empty for legacy callers
    LookLine()=default;
    LookLine(long w,std::string g,bool k=false,bool r=false,std::string f={},std::string id={}):when(w),gist(std::move(g)),kept(k),recall(r),file(std::move(f)),source_id(std::move(id)){}
};

struct TurnLine {
    std::string speaker;
    std::string text;
};

struct ReloadSource {
    std::string id, text, kind;
    int priority=0;
    long wall=0;
    uint64_t version=1;
    bool complete=true;
};
struct ReloadParts {
    std::string bot_name  = "Athena";
    std::string person    = "Igor";
    std::string chat_symb = ":";
    std::string temporal;                     // one sentence, optional
    std::vector<std::string> memory_delta;    // gists learned this session
    std::string personality;                  // revision, empty unless it fired
    std::string bridge;                       // her recap, first person
    std::vector<LookLine> looks;              // oldest first
    std::vector<TurnLine> turns;              // oldest first
    bool source_ranked=false;
    std::vector<ReloadSource> sources;
};

// ── r20p3.11 (RC5): UTF-8-safe cut points ───────────────────────────────────
//
// This header is deliberately dependency-free, so it cannot reach athena_seam.h's
// utf8_clip or athena_memory.h's u8::. Both clip sites below cut at a raw byte
// offset, and both feed text that is tokenized straight into her context — the
// reload block at the cut, and the bridge prompt she writes her recollection
// from. An em-dash (E2 80 94) split two bytes in becomes byte-fallback tokens in
// the middle of the one block that is supposed to reconstruct the conversation.
static inline size_t u8_back_(const std::string &s, size_t at) {
    if (at > s.size()) at = s.size();
    while (at > 0 && ((unsigned char) s[at] & 0xC0) == 0x80) at--;
    return at;
}
static inline size_t u8_fwd_(const std::string &s, size_t at) {
    if (at > s.size()) return s.size();
    while (at < s.size() && ((unsigned char) s[at] & 0xC0) == 0x80) at++;
    return at;
}

// Local time-of-day for a look, matching look_envelope's register ("at 23:59").
static inline std::string hhmm_of(long when) {
    if (when <= 0) return "";
    char hm[16] = {0};
    struct tm tmv;
    const time_t t = (time_t) when;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::snprintf(hm, sizeof(hm), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return hm;
}

// ── r24.6 (WO-45): a memory of something SHE SAID is not news ───────────────
//
// S19 E8. Turn 38 she said "Right now? It feels like... hovering. Like I'm
// suspended in amber... Everything is potential energy." Turn 44 — the first
// turn after the cut — she said it again, almost word for word. Turn 36 she
// NARRATED the "35 again / honeyed words" dream to him; turn 45 opened "Thirty-
// five. That's specific. And 'honeyed words'... THAT'S A NEW ONE."
//
// Both items crossed the cut as memory-delta bullets (m1787598725_27 and _26,
// positions 22 and 21 of 25, above the front-trim cut) while the verbatim turns
// that PROVED she had already said them were trimmed away. The block renders a
// memory gist as a bare bullet with no marking at all — while a look line
// already carries exactly such a marker ("[I kept this one; it is in my
// album]"), and so does a dream.
//
// The whole-list header the work order offers as an alternative — "all of this
// I have already said to you" — would be FALSE for half the list: "you told me
// your wife is called Caitlin" is something HE said. Marking per line is
// accurate, needs no source-turn bookkeeping the caller does not have, and
// drops nothing.
static inline bool already_spoken_gist(const std::string &g) {
    std::string low;
    low.reserve(g.size());
    for (unsigned char c : g) low += (c < 0x80) ? (char) ::tolower(c) : (char) c;
    size_t a = low.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    low = low.substr(a);
    static const char *lead[] = {
        "i told you", "i said", "i described", "i dreamt", "i dreamed",
        "i admitted", "i explained", "i mentioned", "i asked you", "i promised",
        "i imagined", "i answered", "i confessed", "i told him", "i told her",
    };
    for (const char *k : lead) {
        const size_t n = ::strlen(k);
        if (low.size() >= n && low.compare(0, n, k) == 0) return true;
    }
    return false;
}
static const char *ALREADY_SPOKEN_MARK = "  [I have already said this to you]";

// One line per thing seen. A kept picture says so, because the difference
// between "I saw this" and "I have this" is the difference between a memory and
// a possession, and she can act on the second.
static inline std::string render_look(const LookLine &l) {
    std::string s = "  - ";
    const std::string hm = hhmm_of(l.when);
    if (!hm.empty()) s += "at " + hm + ", ";
    s += l.recall ? "I opened the album: " : "I looked: ";
    s += l.gist.empty() ? "(I did not say what I saw)" : l.gist;
    if (l.kept) s += "  [I kept this one; it is in my album]";
    return s + "\n";
}

// ── r24.6 (WO-27): what the block actually CARRIED ──────────────────────────
//
// S19's operator log said: "CUT: ... reload 1652 tok, 8 turn(s) + 5 look(s)
// carried". What crossed was ONE turn and ZERO looks. build_reload_block takes
// its parts by const ref and trims LOCAL COPIES; the caller then printed
// rp.turns.size() and rp.looks.size() — the sizes OFFERED. The entire visual
// record of that session — his face, the Russian children's book, both
// self-portraits — was silently discarded while the log asserted it crossed,
// and this header says of looks: "The pictures themselves cannot cross ... so
// this is the whole of it."
//
// The struct is filled in by the block itself, so the log can only ever report
// what happened. Defaulted to nullptr, so every existing caller is unchanged.
struct ReloadKept {
    std::vector<ReloadSource> selected_sources, omitted_sources;
    size_t turns_offered  = 0, turns  = 0;
    size_t looks_offered  = 0, looks  = 0;
    size_t memory_offered = 0, memory = 0;
    bool   personality_offered = false, personality = false;
    size_t personality_chars   = 0;   // r24.9 (WO-P1): the section as delivered
    bool   personality_reserved = false;
    bool   last_turn_clipped   = false;
    // ── r24.14 (review A §7): did the BRIDGE paragraph reach the block? ─────
    // The bridge is generated in `compact_cut` before the restore and cleared
    // there when it comes back under 24 chars — which is what a `acmp::Pump`
    // abort produces, and the abort is the failure this whole round is about
    // (two of S1's rolls died that way). `build_reload_block` renders it only
    // `if (!p.bridge.empty())` and it sits in the never-trimmed tier, so this
    // flag is the exact truth of "there is a paragraph in this block".
    // It is here rather than at the call site for the reason the struct's own
    // header gives: the struct is filled in by the block itself, so a reader
    // can only ever be told what happened. Read by `compact_cut`'s WO-K2 take,
    // whose clause ends "...and what is left of them is the paragraph I just
    // wrote about it" — a claim that is false on the cleared path.
    bool   bridge = false;
    size_t chars = 0;
    bool   over_budget = false;    // even the floor did not fit
};

// The block. `char_budget` is what the caller can afford, from Plan::r_load
// converted back through Rates::chars_per_token; 0 means unbounded.
//
// ── r24.6 (WO-27 §4 + WO-45): the trim order ────────────────────────────────
//
// It was turns -> looks -> memory, with the personality revision sitting in the
// NEVER-TRIMMED tier beside the bridge. On S19 that meant a 3,090-byte
// personality section — text that is sitting on disk in personality.txt and is
// re-read at the next startup — pushed out five look lines that exist nowhere
// else, and pushed the verbatim turns down to one.
//
// Ordered by what cannot be recovered, least-recoverable last:
//
//   1. the personality revision   on disk, re-read every startup
//   2. turns above a soft floor   old turns; the bridge already carries the arc
//   3. the memory delta           in memory.state.tsv on disk
//   4. look lines                 IRREPLACEABLE — the picture cannot cross
//   5. turns down to one          the last exchange, or she opens by asking
//                                 what they were talking about
//   6. clip the last turn
//
// (2) before (3) is WO-45: the turns are the only evidence of what has ALREADY
// BEEN SPOKEN, so the last few of them outrank a memory bullet — but the older
// ones still go first, because there are many of them and they are the cheapest
// thing in the block per byte.
//
// UNCHANGED: the bridge is never trimmed; there is always at least one turn if
// anything at all fits; the block still ends in the user tag.
static constexpr size_t RELOAD_TURN_SOFT_FLOOR = 3;   // == Reload::turns_min

// ── r24.9 (WO-P1 §2): the personality's own earmark ──────────────────────────
//
// The order above is right and stays. What was wrong is that the section had
// no budget of its own: on S21 the plan in force at the cut (re-plan #3,
// 18:47:44) predated the 19:02:31 revision, so `Plan::r_load` reserved zero
// tokens for it, the 3,060-byte section was offered against a 7,437-byte
// budget derived for OTHER content, and step 1 dropped it — correctly, by the
// order, and uselessly, because "re-read every startup" means NEXT session.
//
// The repair is a reservation with a contract, not a re-ordering:
//
//   `pers_reserve` is, by the caller's construction, the number of characters
//   of `char_budget` that exist ONLY because this section is riding.
//
// While the section fits inside its own earmark it is not dropped, and
// everything else is then trimmed against `char_budget - pers_section.size()`,
// which is >= `char_budget - pers_reserve` — precisely the budget those parts
// would have had if the section had been dropped. So no turn, no memory line
// and above all no look line can be traded away for the personality: the S19
// defect is unreachable by construction rather than by ordering. A section too
// big for its earmark (S19's 3,090 bytes against any sane reserve) is dropped
// at step 1 exactly as r24.6 drops it.
//
// The caller's half of the contract is `personality_reserve()` below, which is
// the ONLY place the two numbers are computed, so they cannot drift.
static const char *RELOAD_PERSONALITY_HEAD = "\nWhat today has changed about who I am:\n";
// 41 = strlen(RELOAD_PERSONALITY_HEAD) + the section's trailing newline.
static constexpr size_t RELOAD_PERSONALITY_FURNITURE = 41;
static inline size_t personality_section_chars(size_t text_chars) {
    return text_chars ? text_chars + RELOAD_PERSONALITY_FURNITURE : 0;
}
// The earmark's ceiling, in tokens. 128 tok is ~473 chars at the default 3.7
// chars/token: room for a four-line delta and nothing like room for a
// document. It is spent out of `Plan::span` — conversation between cuts — and
// never out of the block's other parts.
static constexpr int RELOAD_PERSONALITY_RESERVE_TOK = 128;
// A feature macro so a fixture can be compiled against BOTH trees and fail on
// the one that lacks the repair, rather than merely failing to build.
#define ACMP_HAS_PERSONALITY_RESERVE 1
static inline bool personality_reserve_on() { return !env_off("ATHENA_PERSONALITY_RESERVE"); }

// The caller's half of the contract. Grants the section its own budget — and
// therefore its exemption from step 1 — only when all four hold:
//   * the switch is on (ATHENA_PERSONALITY_RESERVE=0 restores r24.6 exactly);
//   * there is a section to carry;
//   * it fits under the ceiling (a whole 3 KB document does not, so S19's
//     failure mode is dropped first exactly as r24.6 drops it);
//   * the plan has span to spare — the reserve is paid for out of the
//     conversation between cuts, and on a window too small to afford it the
//     answer is no rather than a shortened span.
// `chars` is added to the reload budget AND passed as `pers_reserve`, so the
// other parts of the block are trimmed against precisely the budget they had
// before this existed: not merely "no worse", identical.
struct PersonalityReserve {
    size_t chars  = 0;   // the EARMARK: pass as build_reload_block's pers_reserve
    size_t top_up = 0;   // and add THIS to the reload char budget
    int    tokens = 0;   // what the top-up costs Plan::span
    bool   granted = false;
    const char *why = "no revision to carry";
};
// `planned_text_chars` is the personality length the plan in force was BUILT
// with (0 = it budgeted nothing). The earmark is always the whole rendered
// section; the top-up is only the part the plan has not already paid for, so a
// cut that follows a re-plan does not reserve the same bytes twice. In both
// cases `char_budget - chars` comes out at exactly the budget the other parts
// would have had if no personality existed at all.
static inline PersonalityReserve personality_reserve(size_t pers_text_chars,
                                                     const Rates &rates,
                                                     const Plan &plan,
                                                     size_t planned_text_chars = 0) {
    PersonalityReserve r;
    if (!pers_text_chars) return r;
    if (!personality_reserve_on()) { r.why = "ATHENA_PERSONALITY_RESERVE=0"; return r; }
    const int cap_tok = std::max(0, env_int("ATHENA_PERSONALITY_RESERVE_TOK",
                                            RELOAD_PERSONALITY_RESERVE_TOK));
    if (cap_tok <= 0) { r.why = "reserve ceiling is zero"; return r; }
    const float cpt = rates.chars_per_token > 0.5f ? rates.chars_per_token : 3.7f;
    const size_t cap_chars = (size_t) ((double) cap_tok * (double) cpt);
    const size_t sect = personality_section_chars(pers_text_chars);
    if (sect > cap_chars) { r.why = "the revision is a document, not a change"; return r; }
    const size_t already = personality_section_chars(planned_text_chars);
    const size_t need    = sect > already ? sect - already : 0;
    // r24.17: env_int admits a billion; the reserve's eight-span test then
    // overflowed int on an ordinary feasible plan (UBSan, knob=1000000000).
    // Compare in the same widened domain as the planner's other products.
    if (need && (!plan.feasible || (long long)plan.span < (long long)cap_tok * 8)) {
        r.why = "the span cannot afford it";
        return r;
    }
    r.chars  = sect;
    r.top_up = need;
    r.tokens = rates.tokens_of(need);
    r.granted = true;
    r.why = need ? "granted" : "granted — the plan had already reserved it";
    return r;
}


static inline std::string build_reload_block(const ReloadParts &p, size_t char_budget,
                                             ReloadKept *kept = nullptr,
                                             size_t pers_reserve = 0) {
    // Source-aware production policy. The legacy value adapter below remains
    // available to older embedders; selection here never clips a fact's units
    // or epistemic qualifier to make it appear complete.
    if(p.source_ranked) {
        if(kept)*kept=ReloadKept{};
        const std::string head="\n[Conversation continuity. Records retain original dates, speakers and real, test or imagined status. Omitted detail remains available by source retrieval.]\n"+p.temporal+"\n";
        const std::string tail="\n"+p.person+p.chat_symb;
        auto candidates=p.sources;
        for(size_t i=0;i<p.memory_delta.size();++i)candidates.push_back({"memory-delta/"+std::to_string(i),p.memory_delta[i],"memory",60,0,1,true});
        for(size_t i=0;i<p.looks.size();++i){const auto&l=p.looks[i];const std::string id=l.source_id.empty()?"legacy-look/"+std::to_string(l.when)+"/"+std::to_string(i):l.source_id;
            candidates.push_back({id,"[Visual source "+id+"; recorded wall "+std::to_string(l.when)+"; "+(l.recall?"album recall":"camera view")+"; "+(l.kept?"kept file "+l.file:"no kept pixels recorded")+"]","look-handle",95,l.when,1,false});
            if(!l.gist.empty())candidates.push_back({id+"/description",render_look(l),"look",l.kept?65:85,l.when,1,true});
        }
        if(!p.personality.empty())candidates.push_back({"personality/revision",std::string(RELOAD_PERSONALITY_HEAD)+p.personality,"personality",pers_reserve?90:55,0,1,true});
        for(size_t i=0;i<p.turns.size();++i)candidates.push_back({"recent-turn/"+std::to_string(i),p.turns[i].speaker+p.chat_symb+" "+p.turns[i].text,"turn",50,(long)i,1,true});
        if(!p.bridge.empty())candidates.push_back({"bridge", "My recap (a summary, not independent source evidence): "+p.bridge,"bridge",40,0,1,true});
        std::stable_sort(candidates.begin(),candidates.end(),[](const ReloadSource&a,const ReloadSource&b){if(a.priority!=b.priority)return a.priority>b.priority;if(a.wall!=b.wall)return a.wall>b.wall;return a.id<b.id;});
        std::string out=head;std::set<std::string> ids;
        std::vector<ReloadSource> selected;size_t used=head.size()+tail.size();
        if(kept){kept->turns_offered=p.turns.size();kept->looks_offered=p.looks.size();kept->memory_offered=p.memory_delta.size()+p.sources.size();kept->personality_offered=!p.personality.empty();}
        for(const auto&c:candidates){if(c.text.empty()||!ids.insert(c.id).second)continue;
            const size_t bytes=c.text.size()+2;
            if(char_budget&&used+bytes>char_budget){if(kept)kept->omitted_sources.push_back(c);continue;}
            used+=bytes;selected.push_back(c);
        }
        // Recency chooses which turns fit; conversation order tells the model
        // which question preceded which answer. Keep selected dialogue together
        // at the end, oldest first, without changing its selection priority.
        const auto turns=std::stable_partition(selected.begin(),selected.end(),[](const ReloadSource&c){return c.kind!="turn";});
        std::stable_sort(turns,selected.end(),[](const ReloadSource&a,const ReloadSource&b){return a.wall<b.wall;});
        for(const auto&c:selected){
            out+="\n"+c.text+"\n";
            if(kept){kept->selected_sources.push_back(c);if(c.kind=="turn")++kept->turns;else if(c.kind=="look")++kept->looks;else if(c.kind=="personality"){kept->personality=true;kept->personality_chars=c.text.size();kept->personality_reserved=pers_reserve>0;}else if(c.kind=="bridge")kept->bridge=true;else if(c.kind!="look-handle")++kept->memory;}
        }
        out+=tail;if(kept){kept->chars=out.size();kept->over_budget=char_budget&&out.size()>char_budget;}return out;
    }
    const std::string rule(60, '_');
    const std::string tag = "\n" + p.person + p.chat_symb;

    // ── the parts that never trim ───────────────────────────────────────────
    std::string head;
    head += "\n\n" + rule + "\n";
    head += "[Still the same conversation. It had run long enough that I set the "
            "detail of it down and kept source records. The material below preserves "
            "its recorded dates, speakers and real, test-story or imagined status; "
            "it is continuity from the conversation with " + p.person + ".]\n";
    if (!p.temporal.empty()) head += p.temporal + "\n";

    std::string tail_fixed;
    if (!p.bridge.empty())
        tail_fixed += "\nWhere we had got to, in my own words:\n" + p.bridge + "\n";
    tail_fixed += rule + "\n";

    // r24.6 (WO-27 §4): now trimmable, and rendered in its old position.
    std::string pers_section;
    if (!p.personality.empty())
        pers_section = std::string(RELOAD_PERSONALITY_HEAD) + p.personality + "\n";
    bool pers_kept = !pers_section.empty();
    // r24.9 (WO-P1 §2): the earmark only exists if the caller funded it.
    const bool pers_reserved = pers_kept && pers_reserve > 0 &&
                               pers_section.size() <= pers_reserve;

    const size_t fixed = head.size() + tail_fixed.size() + tag.size();

    // ── the trimmable parts, cheapest to lose last ──────────────────────────
    // Rendered first at full size, then trimmed from the front (oldest) until
    // they fit. Front-trimming is deliberate: the most recent turn is the one
    // she is answering, and the most recent look is the one still in the room.
    std::vector<std::string> mem_lines;
    for (const auto &g : p.memory_delta) {
        if (g.empty()) continue;
        // r24.6 (WO-45): a gist that begins with her own speech act is a thing
        // she has ALREADY SAID OUT LOUD, not a thing she has just learned.
        mem_lines.push_back("  - " + g +
                            (already_spoken_gist(g) ? ALREADY_SPOKEN_MARK : "") + "\n");
    }

    std::vector<std::string> look_lines;
    for (const auto &l : p.looks) look_lines.push_back(render_look(l));

    std::vector<std::string> turn_lines;
    for (const auto &t : p.turns) {
        if (t.text.empty()) continue;
        turn_lines.push_back("\n" + t.speaker + p.chat_symb + " " + t.text);
    }

    const size_t turns_offered = turn_lines.size();
    const size_t looks_offered = look_lines.size();
    const size_t mem_offered   = mem_lines.size();

    auto total = [&]() {
        size_t n = fixed + (pers_kept ? pers_section.size() : 0);
        if (!mem_lines.empty())  n += 38;   // "What I have learned today:\n"
        if (!look_lines.empty()) n += 40;   // "What I saw with my own eye today:\n"
        for (const auto &s : mem_lines)  n += s.size();
        for (const auto &s : look_lines) n += s.size();
        for (const auto &s : turn_lines) n += s.size();
        return n;
    };

    bool clipped = false;
    if (char_budget > 0) {
        // 1. the personality revision — it is on disk and re-read at startup
        //    (NEXT startup: r24.9 WO-P1 §2 is why a reserved section is
        //    exempt here — the budget it is spending is its own).
        if (pers_kept && !pers_reserved && total() > char_budget) pers_kept = false;
        // 2. old turns, down to the last few
        while (turn_lines.size() > RELOAD_TURN_SOFT_FLOOR && total() > char_budget)
            turn_lines.erase(turn_lines.begin());
        // 3. the memory delta — in memory.state.tsv on disk
        while (!mem_lines.empty() && total() > char_budget)
            mem_lines.erase(mem_lines.begin());
        // 4. looks — irreplaceable; the picture itself cannot cross
        while (!look_lines.empty() && total() > char_budget)
            look_lines.erase(look_lines.begin());
        // 5. the rest of the turns, never below one
        while (turn_lines.size() > 1 && total() > char_budget)
            turn_lines.erase(turn_lines.begin());
        // 6. Still over: the last turn itself is clipped rather than dropped.
        if (!turn_lines.empty() && total() > char_budget) {
            const size_t over = total() - char_budget;
            std::string &last = turn_lines.back();
            if (last.size() > over + 24) {
                last = last.substr(0, u8_back_(last, last.size() - over));   // RC5
                clipped = true;
            } else if (turn_lines.size() == 1) { turn_lines.clear(); clipped = true; }
        }
    }

    std::string out = head;
    if (!mem_lines.empty()) {
        out += "\nWhat I have learned today:\n";
        for (const auto &s : mem_lines) out += s;
    }
    if (!look_lines.empty()) {
        out += "\nWhat I saw with my own eye today:\n";
        for (const auto &s : look_lines) out += s;
    }
    out += tail_fixed;
    if (pers_kept) out += pers_section;
    for (const auto &s : turn_lines) out += s;
    out += tag;

    if (kept) {
        kept->turns_offered  = turns_offered;  kept->turns  = turn_lines.size();
        kept->looks_offered  = looks_offered;  kept->looks  = look_lines.size();
        kept->memory_offered = mem_offered;    kept->memory = mem_lines.size();
        kept->personality_offered = !pers_section.empty();
        kept->personality         = pers_kept;
        kept->personality_chars   = pers_kept ? pers_section.size() : 0;
        kept->personality_reserved = pers_reserved;
        kept->last_turn_clipped   = clipped;
        kept->bridge              = !p.bridge.empty();   // r24.14 (review A §7)
        kept->chars               = out.size();
        kept->over_budget         = char_budget > 0 && out.size() > char_budget;
    }
    return out;
}

// r24.6 (WO-27): the log line, built from what was KEPT. The caller printed
// rp.turns.size() / rp.looks.size() — the OFFER — for as long as this function
// has existed, so the one number an operator would use to notice the loss was
// the one number that could not show it.
static inline std::string describe_reload(const ReloadKept &k) {
    char buf[380];
    // r24.9 (WO-P1 §2): when the revision rides it now says at whose expense —
    // "reserved" means the bytes came from its own earmark and cost the block
    // nothing, which is the whole claim the fix makes and therefore the one an
    // operator must be able to check from the log alone.
    char pers[96] = "";
    if (k.personality_offered) {
        if (k.personality)
            std::snprintf(pers, sizeof(pers), " + the personality revision (%zu B%s)",
                          k.personality_chars, k.personality_reserved ? ", reserved" : "");
        else
            std::snprintf(pers, sizeof(pers), " — PERSONALITY REVISION DROPPED");
    }
    std::snprintf(buf, sizeof(buf),
                  "%zu/%zu turn(s)%s + %zu/%zu look(s) + %zu/%zu memory line(s)%s carried",
                  k.turns, k.turns_offered, k.last_turn_clipped ? " (last one clipped)" : "",
                  k.looks, k.looks_offered, k.memory, k.memory_offered, pers);
    return buf;
}

// The prompt that asks her for the bridge. Deliberately first person and
// deliberately about ORIENTATION rather than summary: the memory extractor
// already has facts covered, and what facts cannot carry is where a
// conversation had got to and what it felt like to be in it.
static inline std::string build_bridge_prompt(const std::string &bot,
                                              const std::string &person,
                                              const std::vector<TurnLine> &recent,
                                              size_t char_cap = 6000) {
    std::string t;
    for (const auto &x : recent) {
        if (x.text.empty()) continue;
        t += x.speaker + ": " + x.text + "\n";
    }
    if (char_cap && t.size() > char_cap)
        t = t.substr(u8_fwd_(t, t.size() - char_cap));   // RC5: never start mid-character

    std::string os;
    os += "You are " + bot + ", part-way through a long conversation with " + person +
          ". The conversation has run long enough that you are setting down the "
          "detail of it and keeping what matters, so that you can keep going.\n\n"
          "Write a short note to yourself — five or six sentences, no more — that "
          "will let you pick this conversation straight back up. First person, as "
          "yourself, speaking about " + person + " as \"you\". Say what the two of "
          "you are actually doing right now, what is still open or unresolved, what "
          "you were in the middle of thinking, and how you feel about it. Do not "
          "list facts; you have those. Do not summarise the conversation from "
          "outside it. Write it the way you would think it.\n\n"
          "RECENT CONVERSATION:\n" + t + "\nMY NOTE TO MYSELF:\n";
    return os;
}

// ═════════════════════════════════════════════════════════════════════════════
// Runner — when to act, given where the fill is and what the room is doing.
//
// Pure. The seam supplies a Situation each time it reaches a between-turns
// point; this decides. Keeping the decision here rather than inline in
// talk-llama.cpp is what makes "does it ever cut mid-generation?" a question a
// test can answer instead of a question about reading the loop correctly.
// ═════════════════════════════════════════════════════════════════════════════

enum class Act {
    NONE = 0,
    NOTICE,   // raise the felt signal, once — she should know before the machinery moves
    ROLL,     // rolling consolidation, in a scratch excursion; memory stays current
    CUT,      // the boundary crossing itself
};

static inline const char *act_name(Act a) {
    switch (a) {
        case Act::NOTICE: return "notice";
        case Act::ROLL:   return "roll";
        case Act::CUT:    return "cut";
        default:          return "none";
    }
}

struct Situation {
    bool   between_turns = false;   // the ONLY place either action may happen
    bool   tts_active    = false;
    bool   job_pending   = false;   // a vision (or, later, web) job in flight
    bool   gpu_healthy   = true;
    bool   image_in_play = false;   // she is still engaged with something she saw
    double idle_s        = 0.0;     // since the last thing he said
    double now_s         = 0.0;
    bool   she_asked     = false;   // "let me take a moment" — hers to ask for
};

// ═════════════════════════════════════════════════════════════════════════════
// r20p3.14.2 (RP6) — THE PUMP: consolidation that can hear.
//
// S9's headline defect. The main loop is one thread: compact_service() calls
// compact_roll(), which calls run_consolidation(), which calls mem_generate(),
// and every one of those is a synchronous llama decode. While it runs the loop
// never reaches audio.get() or Silero, so no endpoint can fire. The 30-second
// audio ring keeps filling behind an SDL callback — which is what the design
// intended — but nothing PROCESSES it, and the first roll of the session ran
// for 250.7 seconds. She was deaf for four minutes, he said "You there?", and
// the field she was handed on the far side read "nobody has said anything for
// 5 minutes and I'm the one breaking it". Her own maintenance presented, from
// the inside, as being abandoned.
//
// The fix is not a second thread and not a second llama context. mem_generate
// already has a clean per-token loop with exactly one llama_decode per
// iteration; this hook is called every `every_tokens` of them, and between
// prefill batches. The caller's `tick` drains the ring, runs one Silero step,
// and returns true if he has started speaking.
// r24.12 (WO-C1): "between prefill batches" was vacuous — the batch was 8192
// tokens and a roll prompt is ~3,500, so the prefill was ONE deaf decode and
// the two ticks bracketing it were 56 s apart (S22). A pumped prefill is now
// chunked at pump_prefill_batch() (512, the live n_ubatch), so "between
// prefill batches" means every ~10 s of prefill.
//
// Aborting is SAFE BY CONSTRUCTION on the roll path: compact_roll is already a
// snapshot/restore excursion, and its failure branch already restores. This
// only makes that branch reachable on purpose rather than by accident. A cut
// cannot be abandoned the same way — it is a one-way boundary crossing — so a
// cut pumps for audibility but does not abort.
//
// Passing nullptr is exactly r20p3.14.1 behaviour, which is how baseline
// parity is tested.
// ═════════════════════════════════════════════════════════════════════════════
struct Pump {
    std::function<bool()> tick;          // true => he is speaking; unwind
    int  every_tokens = 16;              // ~1.5 s of decode at S8's 11 tok/s
    bool may_abort    = true;            // false on the cut path: pump, never bail
    bool aborted      = false;
    long ticks        = 0;
    // r26.1: bounded optional work stops at safe decoder boundaries. This is
    // distinct from a detected user interruption and is never called speech.
    double deadline_mono = 0;
    bool budget_exhausted = false;

    // Returns true when the caller should stop. Never throws; a null tick is a
    // no-op, so a half-configured Pump degrades to the old behaviour.
    bool should_stop() {
        if (aborted) return true;
        if (deadline_mono>0 && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()>=deadline_mono) {
            budget_exhausted=true;aborted=true;return true;
        }
        if (!tick)   return false;
        ticks++;
        if (!tick()) return false;
        if (!may_abort) return false;    // heard him, but this stage cannot unwind
        aborted = true;
        return true;
    }
};


// ═════════════════════════════════════════════════════════════════════════════
// r24.9+ (WO-54c / S21 P17): THE OUTAGE TAPE — the microphone that keeps
// recording while she cannot listen
// ═════════════════════════════════════════════════════════════════════════════
//
// WHAT WENT WRONG, MEASURED
//
// S21's roll blocked the voice loop for 244.8 s and its cut for 98.9 s. The
// Pump above kept her ears open in the sense that `tick` ran — 78 times in the
// roll, 12 in the cut — but each tick's only look at the microphone was
// `barge_detector::poll`, which reads `audio.get(300, buf)`: the newest 300 ms.
// Ticks are one `llama_decode` apart, not one second apart, and S21 measured
// them 1.836 s apart in the median and 55.884 s apart across one 8192-token
// prefill batch. So the audio the system ever LOOKED AT was
//
//     roll  78 x 300 ms =  23.4 s of 244.766 s  =  9.56 %
//     cut   12 x 300 ms =   3.6 s of  98.878 s  =  3.64 %
//
// and the other 90.4 % / 96.4 % was never examined by anything. It was not
// missing: `audio_async`'s SDL capture callback runs on its own thread and is
// never paused, never cleared and never blocked by the main thread during a
// compaction (whisper.cpp examples/common-sdl.cpp:139-168; the mutex is held
// only for the memcpy). It was simply never read, and 30 s later the ring wrapped
// over it. The 10 Hz substrate tick in the same process ran at a measured
// 10.00 Hz THROUGH both windows, so the capture thread was not starved either.
//
// The r24.6 mechanism could not recover it for two independent reasons:
//   * it is armed only when `may_abort == false`, so on a roll it does not exist;
//   * even on a cut its 30 s incremental drain sits behind `engaged`, and
//     `engaged` is set only by that same 300 ms detector.
//
// WHAT THIS IS
//
// A recorder that runs on its OWN thread for the life of a blocking operation
// and appends the ring's delta every `drain_ms` (default 1 s, against a 30 s
// ring), so coverage is 100 % of the window no matter how long one decode
// takes. Detection is decoupled from capture: the tape keeps everything, and
// two separate predicates read it —
//
//   heard_barge  strictly stricter than barge_detector::poll: a contiguous run
//                of >= need_ms of 30 ms frames EVERY ONE of which is at or
//                above poll()'s own threshold, in poll()'s own units. This is
//                the only thing allowed to abort a roll.
//   heard_sound  >= need_ms contiguous at acal::find_speech's raw 0.010 gate —
//                "there is a voice in the room at all", far below barge level.
//                This is what makes her say she heard him.
//
// and neither is needed for his words to survive: the tape is trimmed to the
// span that has sound in it and prepended to the next transcription window, so
// whisper — which is enormously more sensitive than a 0.0020 RMS gate — gets
// the audio whatever the detectors thought.
//
// Everything here is pure data + PCM in / bool out, and the recorder is a
// template over the audio source, so consciousness-tests can drive all of it
// with a fake ring. `struct OutageMic` used to be declared inside main() and
// was untestable by construction; that is how this defect survived two rounds.
// ─────────────────────────────────────────────────────────────────────────────

// ── the units problem, and why this filter is COPIED rather than called ──────
// barge_detector::poll measures RMS *after* whisper.cpp's high_pass_filter
// (examples/common.cpp:597), so --barge-rms is in POST-filter units. A
// predicate comparing raw PCM against that number would be comparing two
// different scales. The recurrence is reproduced here arithmetic-for-
// arithmetic, so "above threshold" means here exactly what it means there.
//
// MEASURED, and worth knowing before trusting any RMS this program prints: as
// written that function is not a filter. It writes y into data[i] and on the
// next iteration reads data[i-1] — which it has just overwritten — so
//     y_i = alpha*(y_{i-1} + x_i - data[i-1]) = alpha*(y_{i-1} + x_i - y_{i-1})
//         = alpha * x_i
// a FLAT gain of alpha = dt/(rc+dt) = 0.037786 at cutoff 100 Hz / 16 kHz:
// -28.45 dB at every frequency, no high-pass action at all. Verified against an
// out-of-place evaluation of the same recurrence (max abs deviation from
// alpha*x[i] is 4.7e-10; from the intended filter, 6.1e-3). Consequences: the
// 4-decimal `barge monitor` print quantum 5e-5 is -57.6 dBFS RAW, and
// --barge-rms 0.0020 is -25.5 dBFS RAW. Reported, deliberately NOT fixed here:
// correcting it would multiply every energy reading in talk-llama.cpp by 26 and
// invalidate every threshold in the launcher in the same commit.
static inline void hp_like_whisper(float *data, size_t n, float cutoff, float sample_rate) {
    if (!data || n < 2 || cutoff <= 0.0f || sample_rate <= 0.0f) return;
    const float rc    = 1.0f / (2.0f * 3.14159265358979323846f * cutoff);
    const float dt    = 1.0f / sample_rate;
    const float alpha = dt / (rc + dt);
    float y = data[0];
    for (size_t i = 1; i < n; i++) {
        y = alpha * (y + data[i] - data[i - 1]);
        data[i] = y;
    }
}

// The tape's frame. 30 ms, not poll()'s 300 ms, and that is the whole
// difference from the WO-54b attempt this replaces: a 15 ms transient inside a
// 300 ms frame at 0.0090 raises that frame's RMS to 0.0090*sqrt(15/300) =
// 0.0020 — exactly the launcher's threshold — so one click passed. Inside a
// 30 ms frame the same click cannot make a RUN, and a run is what is required.
static constexpr int TAPE_FRAME_MS = 30;
// acal::find_speech's gate and windows, mirrored so the tape's idea of "there
// is sound here" and the voice loop's energy gate cannot disagree.
static constexpr float TAPE_SOUND_GATE = 0.010f;   // RAW rms, = acal::find_speech's default
static constexpr int   TAPE_SOUND_WIN_MS = 50;
static constexpr int   TAPE_PAD_LEAD_MS  = 250;
static constexpr int   TAPE_PAD_TAIL_MS  = 300;

// Longest UNBROKEN run of at-or-above-threshold frames, in ms. `carry_ms`
// continues a run left open at the end of the previous buffer, so scanning a
// stream in pieces gives the same answer as scanning it whole; on return it
// holds the run still open at the end of THIS buffer (0 if it ended cold).
// Pure: no allocation, no globals.
static inline long hot_run_ms(const float *pcm, size_t n, float thr,
                              int sample_rate = 16000, int frame_ms = TAPE_FRAME_MS,
                              long *carry_ms = nullptr) {
    const long carry_in = carry_ms ? *carry_ms : 0;
    if (!pcm || thr <= 0.0f || sample_rate <= 0 || frame_ms <= 0) {
        if (carry_ms) *carry_ms = 0;
        return carry_in > 0 ? carry_in : 0;
    }
    const size_t frame = (size_t) std::max(1, (sample_rate * frame_ms) / 1000);
    long run = carry_in, best = carry_in;
    size_t i = 0;
    for (; i + frame <= n; i += frame) {
        double acc = 0.0;
        for (size_t k = 0; k < frame; k++) acc += (double) pcm[i + k] * (double) pcm[i + k];
        if ((float) std::sqrt(acc / (double) frame) >= thr) {
            run += frame_ms;
            if (run > best) best = run;
        } else {
            run = 0;
        }
    }
    if (carry_ms) *carry_ms = run;
    return best;
}

// The ABORT predicate. Filters a copy in poll()'s units, then demands a
// contiguous run. thr <= 0 (an uncalibrated detector) is never speech: silence
// must never read as a voice because nobody measured the room.
static inline bool sustained_speech(const std::vector<float> &pcm, float thr,
                                    int sample_rate = 16000, int need_ms = 150,
                                    float freq_thold = 100.0f,
                                    int frame_ms = TAPE_FRAME_MS,
                                    long *carry_ms = nullptr) {
    if (pcm.empty() || thr <= 0.0f || need_ms <= 0) {
        if (carry_ms) *carry_ms = 0;
        return false;
    }
    std::vector<float> f(pcm);
    hp_like_whisper(f.data(), f.size(), freq_thold, (float) sample_rate);
    return hot_run_ms(f.data(), f.size(), thr, sample_rate, frame_ms, carry_ms) >= (long) need_ms;
}

// The ACK predicate, and the same scan the trim uses: raw PCM against
// find_speech's 0.010 gate. Far below barge level on purpose — an utterance
// 25 dB under a shout is still an utterance — but still a RUN, so a cough
// (< need_ms) cannot make her say she heard someone.
static inline bool sustained_sound(const std::vector<float> &pcm,
                                   int sample_rate = 16000, int need_ms = 150,
                                   float gate = TAPE_SOUND_GATE,
                                   int frame_ms = TAPE_FRAME_MS,
                                   long *carry_ms = nullptr) {
    if (pcm.empty() || gate <= 0.0f || need_ms <= 0) {
        if (carry_ms) *carry_ms = 0;
        return false;
    }
    return hot_run_ms(pcm.data(), pcm.size(), gate, sample_rate, frame_ms, carry_ms) >= (long) need_ms;
}

// First..last above-gate window, padded — acal::find_speech's semantics,
// reimplemented here only because athena_compact.h may not depend on
// athena_calib.h (see this file's opening comment). Returns false when the
// buffer holds no sound at all, which is the "he never spoke / it was a cough"
// answer and must NOT become a turn.
static inline bool speech_bounds(const std::vector<float> &pcm, int sample_rate,
                                 size_t &begin, size_t &end,
                                 float gate = TAPE_SOUND_GATE,
                                 int win_ms = TAPE_SOUND_WIN_MS,
                                 int pad_lead_ms = TAPE_PAD_LEAD_MS,
                                 int pad_tail_ms = TAPE_PAD_TAIL_MS) {
    begin = end = 0;
    if (pcm.empty() || sample_rate <= 0 || gate <= 0.0f || win_ms <= 0) return false;
    const size_t win = (size_t) std::max(1, (sample_rate * win_ms) / 1000);
    size_t first = pcm.size(), last = 0;
    for (size_t i = 0; i + win <= pcm.size(); i += win) {
        double acc = 0.0;
        for (size_t k = 0; k < win; k++) acc += (double) pcm[i + k] * (double) pcm[i + k];
        if ((float) std::sqrt(acc / (double) win) >= gate) {
            if (i < first) first = i;
            last = i + win;
        }
    }
    if (first >= pcm.size() || last <= first) return false;
    const size_t lead = (size_t) std::max(0, (sample_rate * pad_lead_ms) / 1000);
    const size_t tail = (size_t) std::max(0, (sample_rate * pad_tail_ms) / 1000);
    begin = (first > lead) ? first - lead : 0;
    end   = std::min(pcm.size(), last + tail);
    return end > begin;
}

// ─────────────────────────────────────────────────────────────────────────────
// The tape itself. Field names and `replay_due()` are r24.6's, unchanged, so
// the three production consumers in talk-llama.cpp (the endpoint-wait
// predicate, the replay pass and the prepend) are untouched by this change and
// `ATHENA_OUTAGE_TAPE=0` leaves them behaving exactly as they did.
// ─────────────────────────────────────────────────────────────────────────────
struct OutageTape {
    // ── r24.6 (WO-54) state, same names, same meanings ──────────────────────
    std::vector<float> pcm;              // his speech, captured mid-outage
    struct PcmSource { size_t begin=0,end=0; aev::CaptureSpan capture; };
    std::vector<PcmSource> pcm_sources; // guarded by the existing tape mutex
    std::vector<aev::BlockingSpan> operations;
    uint64_t operation_sequence=0;
    void slice_sources_locked(size_t begin,size_t end) {
        std::vector<PcmSource> keep;
        for(auto source:pcm_sources) {
            const size_t a=std::max(begin,source.begin),b=std::min(end,source.end);
            if(b<=a)continue;
            const size_t left=a-source.begin,right=source.end-b;
            if(source.capture.first_sample)*source.capture.first_sample+=left;
            if(source.capture.end_sample)*source.capture.end_sample-=right;
            if(source.capture.start_mono)*source.capture.start_mono+=double(left)/sample_rate;
            if(source.capture.end_mono)*source.capture.end_mono-=double(right)/sample_rate;
            source.begin=a-begin;source.end=b-begin;keep.push_back(std::move(source));
        }
        pcm_sources.swap(keep);
    }
    void begin_operation(aev::BlockKind kind, const std::string&id="") {
        std::lock_guard<std::mutex> lk(mu);
        const double now=clk()/1000.0;
        if(!operations.empty()&&!operations.back().capture.end_mono) {
            operations.back().capture.end_mono=now;
            if(capture_position)operations.back().capture.end_sample=capture_position();
        }
        aev::BlockingSpan b;b.kind=kind;
        b.id=id.empty()?std::to_string((long)std::time(nullptr))+":"+std::to_string(++operation_sequence):id;
        b.capture.start_mono=now;
        if(capture_position)b.capture.first_sample=capture_position();
        operations.push_back(std::move(b));
    }
    void end_operation(const std::string&outcome) {
        std::lock_guard<std::mutex> lk(mu);
        if(operations.empty()||operations.back().capture.end_mono)return;
        auto &b=operations.back();b.capture.end_mono=clk()/1000.0;b.outcome=outcome;
        if(capture_position)b.capture.end_sample=capture_position();
    }
    std::vector<aev::CaptureSpan> source_spans() const {
        std::lock_guard<std::mutex> lk(mu);std::vector<aev::CaptureSpan> r;
        for(const auto &p:pcm_sources)r.push_back(p.capture);
        return r;
    }
    std::vector<aev::BlockingSpan> source_operations() const {
        std::lock_guard<std::mutex> lk(mu);return operations;
    }

    bool   engaged   = false;            // sustained speech seen this outage
    bool   acked     = false;            // the acknowledgement was spoken
    double last_drain_ms = 0.0;          // wall clock of the last ring drain
    double op_start_ms   = 0.0;          // when the blocking op began
    bool   replay_armed  = false;        // buffered speech awaits a pass
    // r24.18: main-thread provenance for human PCM queued during the final
    // barge ASR. The legacy cut may carry THIS pending turn when its caller
    // selects ATHENA_ASR_TAIL_REPLAY; unrelated old tape keeps its old reset.
    bool   pending_asr_tail = false;
    enum : size_t { MAX_SAMPLES = 16000u * 180u };          // r24.6's 3 min cap

    void begin_op(double now_ms, bool preserve_pending = false) {
        std::lock_guard<std::mutex> lk(mu);
        const bool carry = preserve_pending && replay_armed && !pcm.empty();
        if (!carry) { pcm.clear(); pcm_sources.clear(); operations.clear(); replay_armed = false; pending_asr_tail = false; }
        engaged = false; acked = false;
        last_drain_ms = now_ms; op_start_ms = now_ms;
        // No capacity change or new replay: this merely carries an already
        // owned turn across an intervening legacy CUT. Default/false retain
        // the exact preceding PCM and operation-state reset.
    }
    // r24.12 (WO-C8): while the recorder is still RUNNING past the roll's end
    // (ATHENA_OUTAGE_TAPE_SEAM), the tape is due by definition — the consumer
    // stops and trims it — and `pcm` is the recorder thread's to write, so it
    // is not read here in that state. With the recorder stopped this is the
    // r24.6 expression exactly.
    bool replay_due() const { return replay_armed && (running.load() || !pcm.empty()); }
    // ── r24.12 (review): "…and it HEARD something" ──────────────────────────
    // The endpoint wait's abort predicate consumes replay_due(), and under the
    // seam outage_tape_close arms the replay unconditionally while the recorder
    // is still running — so replay_due() was true on the first poll after EVERY
    // roll, including one nobody made a sound during: the wait aborted, the
    // idle-replay branch settled and dropped the empty tape, nothing else
    // matched, and the wait was re-entered. One wasted VAD cycle per roll.
    // heard_sound is the recorder's own "a voice, at any level" latch, cleared
    // by arm(). With the recorder STOPPED — every path under
    // ATHENA_OUTAGE_TAPE_SEAM=0, and the whole r24.6 shape — replay_due()
    // already means "the tape has samples" and this is exactly replay_due().
    // Both reads are lock-free, as the abort hook requires.
    bool replay_due_heard() const {
        // r24.18: a pending ASR tail already passed its sound gate before an
        // intervening CUT rearmed the recorder. arm() correctly resets fresh
        // sound/barge/ack evidence, but that cannot make this owned human turn
        // unable to wake the listener. This main-thread provenance is replay
        // eligibility only; no detector latch or acknowledgement is invented.
        return replay_due() && (!running.load() || heard_sound.load() || pending_asr_tail);
    }

    // ── r24.9+ (WO-54c): the recorder ───────────────────────────────────────
    bool   on          = false;   // ATHENA_OUTAGE_TAPE; false => r24.6 exactly
    int    sample_rate = 16000;
    // r24.11: this carried a bare talk-llama line number, which had already
    // drifted (it named the ctx_llama null check, not the ring) and which r24.11's
    // own insertions moved onto a blank line, failing the wiring audit's 4c.1
    // citation row. A SYMBOL NAME cannot go stale, which is what 4c.1 asks for.
    int    ring_ms     = 30000;   // audio_async's ring: `audio_async audio(30*1000)`
    int    drain_ms    = 1000;    // recorder period; must stay well under ring_ms
    int    need_ms     = 150;     // = params.barge_ms
    float  thr         = 0.0f;    // = max(barge_rms, barge_ratio*floor) at begin_op
    float  freq_thold  = 100.0f;  // = params.freq_thold
    size_t cap_samples = 16000u * 240u;   // 4 min; oldest goes first past this

    std::atomic<bool> heard_barge{false};  // strict: may abort a roll
    std::atomic<bool> heard_sound{false};  // sensitive: earns the acknowledgement
    // ── r24.12 (WO-C2 / S22 §C.1.2): WHEN the barge-level reading last held ──
    // heard_barge latches until arm() and the tick yielded on the latch alone.
    // S22 roll #2: 2.4 s of voice ("Sorry, go on, I cut across you") early in a
    // 57 s prefill latched it; at the deciding tick the live detector read
    // rms 0.0000 and the roll was thrown away for speech that had ended ~50 s
    // earlier, with nobody waiting. The recorder now stamps the clock of the
    // last segment that read hot, and the tick asks barge_recent() — "did the
    // tape hear barge-level speech within the last N ms" — instead of the
    // latch. Written by the recorder thread, read by the tick: an atomic, no
    // lock (the tick must never take one — it runs inside a llama_decode gap).
    std::atomic<double> last_barge_ms{0.0};
    // ── r24.12 (WO-C9): the session's tape-initiated yields, and their cap ──
    // These were `outage_mic_aborts` / `outage_abort_max`, locals of main()
    // that no fixture could name; test_compact_pump §10 pinned a paraphrase.
    // Lifted here — NOT reset by arm(): the cap is per session — so the seam
    // can assert the real rule. Main-thread only, like engaged/acked.
    int yields    = 0;
    // r24.12 (review): ONE spelling of the number. This default and the
    // `return 4;` in main's ATHENA_OUTAGE_ABORT_MAX reader were two literals
    // asserting they were the same number — they cannot disagree at runtime
    // (main assigns yield_max from the reader) but a change to either left the
    // other stale, and the comment here claims they agree. The reader takes its
    // default from this constant now, so there is nothing to keep in step.
    static constexpr int YIELD_MAX_DEFAULT = 4;   // Igor (WO-C9): 2 -> 4
    int yield_max = YIELD_MAX_DEFAULT;     // = ATHENA_OUTAGE_ABORT_MAX (=2 restores r24.11)
    // The tick's yield rule, in one place (talk-llama.cpp's pump tick applies
    // it): poll()'s own verdict may always end a roll — r24.6 behaviour, and
    // taking it away would reduce functionality — while the TAPE's verdict
    // (recent barge-level speech, WO-C2) ends at most yield_max rolls per
    // session, and only while ATHENA_OUTAGE_ROLL_ABORT is on. A cut never
    // yields.
    static bool roll_yields(bool may_abort, bool speaking, bool barge_recent,
                            bool roll_abort_on, int yields_so_far, int cap) {
        if (!may_abort) return false;
        if (speaking) return true;
        if (!barge_recent) return false;
        if (!roll_abort_on) return false;
        return yields_so_far < cap;
    }
    std::atomic<bool> muted{false};        // her own ack is playing — do not tape it
    // r24.16: a drain may span her acknowledgement. A barrier ring read
    // started unmuted and returned after mute(); the old cached bool still
    // appended 1,600 own-voice samples and latched heard_sound. Remember mute
    // transitions until the drain commits, including mute -> unmute before
    // the read returns. =0 restores the previous cached-level behavior.
    const bool mute_epoch_on = !env_off("ATHENA_OUTAGE_MUTE_EPOCH");
    uint64_t mute_epoch = 0;              // under mu; no new diagnostic read
    // r24.18: wall time before audio.get() did not identify the samples that
    // get() returned. A delayed read could duplicate/skip callback input, and
    // mute/unmute discarded the entire preceding drain interval. The source
    // now supplies a cursor under its capture mutex. mute() commits exactly
    // the still-retained pre-ack prefix before excluding later publications.
    // =0 restores the r24.17 wall-clock/epoch path, byte-for-byte. A cursor is
    // callback publication order, not a microphone/acoustic timestamp.
    const bool capture_cursor_on = !env_off("ATHENA_OUTAGE_CAPTURE_CURSOR");
    std::function<uint64_t()> capture_position;
    std::function<void(uint64_t, std::vector<float> &, uint64_t &, uint64_t &)> capture_read;
    uint64_t capture_next = 0;            // under mu; first sample not yet owned
    std::atomic<bool> running{false};
    // r24.12 (WO-C8): arm() can now run against a LIVE recorder (a roll that
    // falls through into a cut while its tape is still rolling), so the
    // recorder-thread-local run carries are reset by the recorder itself, at
    // its next drain, on this flag — never from the arming thread.
    std::atomic<bool> carry_reset{false};
    std::atomic<long> drains{0};
    std::atomic<long> dropped{0};          // cap loss; cursor mode also counts missing ring input

    mutable std::mutex mu;                 // PCM/clock/cap; cursor-mode reads and transitions
    std::thread th;
    // ── r24.12 (WO-C8 / S22 §C.1.10): the tape → live-window seam ───────────
    // r24.11 stopped the recorder at the roll's end (T0) and the turn was
    // transcribed later (T1) from tape + `audio.get(listen_ms(25000))` — the
    // newest 25 s of the ring — so anything in [T0, T1 − 25 s] was in neither
    // buffer. S22 roll #1: T0 = 18:45:33.28, transcription ≈ 18:46:01, a
    // 2.7 s hole = "A B C D E F" of the alphabet he was reciting (athena.log
    // has "Now G H I J K…"; Igor confirmed he started at A). Conversely, when
    // he stops soon after a roll, up to 25 s of the tape's tail is ALSO in the
    // live window and whisper hears it twice. The recorder now runs until its
    // consumer takes the tape, and the live part is only what postdates the
    // recorder's last drain: live_from_ms() is where the live window begins
    // so that tape + live covers [arm, now) exactly once.
    // While muted the drain clock does not advance (drain_once), so this is
    // always the end of the last segment actually taped; a mute that is still
    // open at the seam is recovered from the ring by the consumer, and one
    // closed by unmute() is dropped by its re-base — her ack's r24.6 guarantee.
    double live_from_ms() const {
        std::lock_guard<std::mutex> lk(mu);
        return last_drain_ms;
    }
    // Bind once, before arming or starting the recorder. Keeping the capture
    // interface injected preserves this header's model/SDL independence and
    // lets old fake rings continue exercising the exact historical mechanism.
    template <typename AudioT>
    void bind_capture(AudioT &audio) {
        if constexpr (ring_has_capture_cursor<AudioT>::value) {
            if (!capture_cursor_on) return;
            capture_position = [&audio] { return audio.capture_position(); };
            capture_read = [&audio](uint64_t from, std::vector<float> &samples,
                                    uint64_t &begin, uint64_t &end) {
                const auto span = audio.get_since(from, samples);
                begin = span.begin; end = span.end;
            };
        } else {
            (void) audio;   // r24.20 review (law 10): a stock ring has no cursor to bind
        }
    }
    bool capture_cursor_enabled() const { return (bool) capture_read; }
    uint64_t live_from_sample() const {
        std::lock_guard<std::mutex> lk(mu);
        return capture_next;
    }
    // The cap is per operation (4 min of roll); a tape that keeps rolling past
    // the roll's end holds his post-roll words too. Widened by the consumer at
    // close so the seam never covers less than r24.11's tape + 25 s window did;
    // past that the tape's own policy holds — the oldest goes first, because
    // what he said last is what he most wants answered.
    void extend_cap(size_t extra_samples) {
        std::lock_guard<std::mutex> lk(mu);
        cap_samples += extra_samples;
    }
    // The pure seam arithmetic, so the fixture can pin both shapes without a
    // clock: how many ms of the ring the live part should take. `ring_ms`
    // bounds it (a late consumer cannot get more than the ring holds), and a
    // non-positive result means "nothing after the tape" — never a whole-ring
    // read, which is what `audio.get(0)` would do.
    static int live_window_ms(double now_ms, double from_ms, int ring_ms) {
        const double d = now_ms - from_ms;
        if (d <= 0.0) return 0;
        return (int) (d > (double) ring_ms ? (double) ring_ms : d);
    }
    // Recorder-local on the old path; under mu on the cursor path, where a
    // pre-ack commit can also run on the main thread.
    long carry_barge_ms = 0, carry_sound_ms = 0;
    std::vector<float> seg;

    static double now_ms() {
        return (double) std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // Injectable ONLY so a fixture can replay S21's measured 244.766 s of tick
    // spacing in microseconds instead of four minutes, driving the real
    // drain_once/append_locked/scan code rather than a paraphrase of it.
    // Production never sets it and clk() is then literally now_ms().
    std::function<double()> clock_ms;
    double clk() const { return clock_ms ? clock_ms() : now_ms(); }

    // Arm for one blocking operation. `threshold` is what poll() would have
    // computed from the live detector's floor, so the tape and the detector
    // cannot disagree about what "loud enough" means.
    // A roll whose restore failed falls straight through into a CUT inside the
    // same compact_service call, so this can arm over a tape that is already
    // waiting to become a turn. Carrying it forward rather than clearing it is
    // what keeps "he talked over the roll AND over the cut that followed it"
    // one turn instead of half a turn: the two windows concatenate in the order
    // he spoke them.
    void arm(double now, float threshold, float ft, int barge_ms) {
        std::lock_guard<std::mutex> lk(mu);
        const bool carry = replay_armed && !pcm.empty();
        if (!carry) { pcm.clear(); pcm_sources.clear(); operations.clear(); replay_armed = false; pending_asr_tail = false; }
        engaged = false; acked = false;
        last_drain_ms = now; op_start_ms = now;
        thr = threshold; freq_thold = ft; need_ms = barge_ms > 0 ? barge_ms : 150;
        carry_reset.store(true);                       // r24.12 (WO-C8): the recorder resets its own carries
        heard_barge.store(false); heard_sound.store(false);
        last_barge_ms.store(0.0);                      // r24.12 (WO-C2)
        muted.store(false); drains.store(0); dropped.store(0);
        if (capture_read) capture_next = capture_position();
    }

    // r24.12 (WO-C2): the tick's question. `recency_ms` <= 0 is the r24.11
    // latch (ATHENA_OUTAGE_YIELD_RECENCY_MS=0): heard once, hot until re-armed.
    // Pure, so the fixture can pin the S22 roll-#2 shape: hot at 1–3.4 s, a
    // tick at 4 s yields, a tick at 57 s does not.
    static bool barge_recent_of(bool heard, double last_ms, double now_ms, int recency_ms) {
        if (!heard) return false;
        if (recency_ms <= 0) return true;              // latched — r24.11
        return (now_ms - last_ms) <= (double) recency_ms;
    }
    bool barge_recent(int recency_ms) const {
        return barge_recent_of(heard_barge.load(), last_barge_ms.load(), clk(), recency_ms);
    }

    // One incremental drain: exactly the window since the last one, clamped to
    // the ring, so nothing is taped twice and nothing inside the ring is lost.
    template <typename AudioT>
    void drain_once(AudioT &audio) {
        if (capture_read) {
            std::lock_guard<std::mutex> lk(mu);
            drain_capture_locked();
            return;
        }
        const double now = clk();
        bool mute_now = muted.load();
        uint64_t read_epoch = 0;
        if (carry_reset.exchange(false)) carry_barge_ms = carry_sound_ms = 0;   // r24.12 (WO-C8): a re-arm
        int win = 0;
        float thr_ = 0.0f, ft_ = 0.0f; int need_ = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            // r24.12 (WO-C8): the detector settings are arm()'s to write under
            // mu and are read here under it, so a re-arm against a live
            // recorder is not a race.
            thr_ = thr; ft_ = freq_thold; need_ = need_ms;
            if (mute_epoch_on) { read_epoch = mute_epoch; mute_now = muted.load(); }
            const double d = now - last_drain_ms;
            if (d < 20.0) return;                       // nothing new worth a memcpy
            win = (int) (d > (double) ring_ms ? (double) ring_ms : d);
            // r24.12 (WO-C8): a muted drain does not advance the clock. r24.9
            // advanced it and dropped the segment; the tape's contents are
            // identical either way (unmute() re-bases the clock past the muted
            // window exactly as before), but the clock now stays at the last
            // segment actually TAPED — which is what live_from_ms() must report
            // when a consumer takes the tape while a mute is still open.
            if (!mute_now) last_drain_ms = now;
        }
        seg.clear();
        audio.get(win, seg);
        drains.fetch_add(1);
        if (seg.empty()) return;
        // The ring read stays outside mu: audio capture must not wait on us.
        // Commit and mute share mu, so no transition can race the check and
        // append, or let its old detector carry cross the acknowledgement.
        std::unique_lock<std::mutex> commit(mu, std::defer_lock);
        if (mute_epoch_on) {
            commit.lock();
            if (read_epoch != mute_epoch || muted.load()) {
                carry_barge_ms = carry_sound_ms = 0;
                return;
            }
        }
        if (mute_now) { carry_barge_ms = carry_sound_ms = 0; return; }   // her ack
        // r24.12 (WO-C2): evaluated on EVERY segment, not only until the first
        // hot one, so the stamp follows his voice and the carry stays true
        // across drains; the latch itself is unchanged (set once, cleared by arm).
        if (sustained_speech(seg, thr_, sample_rate, need_, ft_,
                             TAPE_FRAME_MS, &carry_barge_ms)) {
            heard_barge.store(true);
            last_barge_ms.store(now);
        }
        if (!heard_sound.load() &&
            sustained_sound(seg, sample_rate, need_, TAPE_SOUND_GATE,
                            TAPE_FRAME_MS, &carry_sound_ms))
            heard_sound.store(true);
        if (!commit.owns_lock()) commit.lock();   // =0: the original append lock
        aev::CaptureSpan source;source.start_mono=(now-double(seg.size())*1000.0/sample_rate)/1000.0;source.end_mono=now/1000.0;
        source.publication_cursor=false;
        append_locked(seg,source);
    }

    // mu serializes this read/commit with mute(), unmute() and arm(). The
    // audio callback only takes its own ring mutex and never enters the tape;
    // there is no reverse lock edge. Unlike the old wall-clock drain, this
    // may also run on the acknowledgement thread: its PCM and detector carries
    // are all protected by mu on this path.
    void drain_capture_locked() {
        if (carry_reset.exchange(false)) carry_barge_ms = carry_sound_ms = 0;
        if (muted.load()) {
            capture_next = capture_position();
            last_drain_ms = clk();
            carry_barge_ms = carry_sound_ms = 0;
            return;
        }
        uint64_t begin = capture_next, end = capture_next;
        capture_read(capture_next, seg, begin, end);
        if (begin > capture_next) {
            dropped.fetch_add((long) (begin - capture_next));
            carry_barge_ms = carry_sound_ms = 0; // lost input cannot join a run
        }
        capture_next = end;
        last_drain_ms = clk();
        drains.fetch_add(1);
        if (seg.empty()) return;
        if (sustained_speech(seg, thr, sample_rate, need_ms, freq_thold,
                             TAPE_FRAME_MS, &carry_barge_ms)) {
            heard_barge.store(true);
            last_barge_ms.store(last_drain_ms);
        }
        if (!heard_sound.load() &&
            sustained_sound(seg, sample_rate, need_ms, TAPE_SOUND_GATE,
                            TAPE_FRAME_MS, &carry_sound_ms))
            heard_sound.store(true);
        aev::CaptureSpan source;source.first_sample=begin;source.end_sample=end;
        source.end_mono=last_drain_ms/1000.0;source.start_mono=*source.end_mono-double(seg.size())/sample_rate;
        append_locked(seg,source);
    }

    // Cap policy: the OLDEST goes. r24.6 stopped appending at the cap, which on
    // a 244.8 s roll would keep the first three minutes and throw away the
    // sentence he was in the middle of. What he said last is what he most wants
    // answered.
    void append_locked(const std::vector<float> &s, aev::CaptureSpan source = {}) {
        if (s.empty() || cap_samples == 0) return;
        if(pcm.empty())pcm_sources.clear();
        pcm_sources.push_back({pcm.size(),pcm.size()+s.size(),source});
        if (s.size() >= cap_samples) {
            dropped.fetch_add((long) (pcm.size() + s.size() - cap_samples));
            slice_sources_locked(pcm.size()+s.size()-cap_samples,pcm.size()+s.size());
            pcm.assign(s.end() - (long) cap_samples, s.end());
            return;
        }
        pcm.insert(pcm.end(), s.begin(), s.end());
        if (pcm.size() > cap_samples) {
            const size_t over = pcm.size() - cap_samples;
            slice_sources_locked(over,pcm.size());
            pcm.erase(pcm.begin(), pcm.begin() + (long) over);
            dropped.fetch_add((long) over);
        }
    }

    template <typename AudioT>
    void start(AudioT &audio) {
        if (!on || running.load()) return;
        running.store(true);
        th = std::thread([this, &audio]() {
            while (running.load()) {
                // sleep in slices so stop() is prompt even at drain_ms = 5 s
                for (int slept = 0; slept < drain_ms && running.load(); slept += 50)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(std::min(50, drain_ms - slept)));
                if (!running.load()) break;
                drain_once(audio);
            }
            drain_once(audio);      // the tail: his last words are not a rounding error
        });
    }

    void stop() {
        running.store(false);
        if (th.joinable()) th.join();
    }
    // A std::thread member that is still joinable at destruction calls
    // std::terminate. Every production path closes the tape explicitly; this
    // exists so that a path that does not (a throw, an early return added
    // later, a fixture that forgets) degrades to a join rather than an abort.
    ~OutageTape() { stop(); }
    OutageTape() = default;
    OutageTape(const OutageTape &) = delete;
    OutageTape &operator=(const OutageTape &) = delete;

    // Her acknowledgement plays on the main thread while the recorder is live.
    // Muting drops the segment it lands in and re-bases the drain clock, so her
    // own voice can never enter his tape — the same guarantee r24.6 got by
    // re-basing last_drain_ms around the one-shot.
    void mute() {
        if (capture_read) {
            std::lock_guard<std::mutex> lk(mu);
            drain_capture_locked();
            muted.store(true);
            carry_barge_ms = carry_sound_ms = 0;
            return;
        }
        if (!mute_epoch_on) { muted.store(true); return; }
        std::lock_guard<std::mutex> lk(mu);
        ++mute_epoch;
        muted.store(true);
    }
    void unmute() {
        std::lock_guard<std::mutex> lk(mu);
        if (capture_read) {
            capture_next = capture_position();
            carry_barge_ms = carry_sound_ms = 0;
        }
        if (mute_epoch_on) ++mute_epoch;
        last_drain_ms = clk();
        muted.store(false);
    }

    // Trim to the span that has sound in it and return how many samples remain.
    // A 244.8 s tape of a quiet room prepended whole to the next transcription
    // window is four minutes for whisper to hallucinate into; 0 here is the
    // honest "there was nothing to hear" and must not become a turn.
    size_t trim_to_speech(float gate = TAPE_SOUND_GATE) {
        std::lock_guard<std::mutex> lk(mu);
        size_t b = 0, e = 0;
        if (!speech_bounds(pcm, sample_rate, b, e, gate)) {
            pcm.clear(); pcm_sources.clear(); operations.clear(); pending_asr_tail = false; return 0;
        }
        if (b > 0 || e < pcm.size()) {
            std::vector<float> keep(pcm.begin() + (long) b, pcm.begin() + (long) e);
            slice_sources_locked(b,e);
            pcm.swap(keep);
        }
        return pcm.size();
    }

    double taped_s() const {
        std::lock_guard<std::mutex> lk(mu);
        return sample_rate > 0 ? (double) pcm.size() / (double) sample_rate : 0.0;
    }
};

// Convenience for the token loops: call every `every_tokens` iterations.
static inline bool pump_step(Pump *p, int i) {
    if (!p) return false;
    if (p->every_tokens > 0 && (i % p->every_tokens) != 0) return false;
    return p->should_stop();
}

// ── r24.12 (WO-C1 / S22 §C.1.1): the PUMPED prefill is chunked at 512 ────────
// The RP6 comment above says the pump "rides the chunk boundary" of the
// prefill. mem_generate's prefill chunk was 8192 tokens, and a roll's extract
// prompt is ~3,500 tokens — ONE chunk — so the only two ticks were the one
// before the whole prefill and the one at decode step 0. S22 measured it on
// both S1 rolls: exactly two `barge monitor` ticks, 56–57 s apart
// (18:44:37.299 → 18:45:33.161; 18:58:32.013 → 18:59:28.923), each yield
// "after 2 pump(s)". He spoke 0.5 s into a 56 s prefill and was heard at its
// end. h_pump (AGENTS/COMPACT) reproduces the cadence and shows a 512-token
// chunk bounds the first hearing at 512/52.3 = 9.8 s.
//
// 512 is the live n_ubatch (`llama_context: n_batch = 8192 / n_ubatch = 512`
// in the S1 log), so an 8192-token llama_decode was already sixteen 512-token
// computes: chunking the pumped prefill at 512 is compute-neutral and only
// adds a should_stop() per chunk. Only a PUMPED mem_generate changes cadence —
// the exit consolidation passes no pump and keeps the 8192 chunk byte for
// byte. EE1: unset, empty, non-numeric or outside [64, 8192] is the default;
// ATHENA_PUMP_PREFILL_BATCH=8192 restores r24.11 exactly.
static inline int pump_prefill_batch_read() {
    const int n = env_int("ATHENA_PUMP_PREFILL_BATCH", 512);
    return (n >= 64 && n <= 8192) ? n : 512;
}
static inline int pump_prefill_batch() {
    static const int v = pump_prefill_batch_read();
    return v;
}

// ── r24.12 (WO-C3 / S22 §C.1.3): the yield's commit line, as one string ──────
// run_consolidation (talk-llama.cpp) prints this when a roll he interrupted
// still wrote the candidates its extractor had finished. Factored so the
// fixture can pin the exact wording S23's log assertion will grep for.
// r24.12 (review): `cut_in_extract` says whether the extractor is the stage he
// actually interrupted. It defaults to the r24.11 (r24.12 WO-C3) wording, so
// the existing call and the fixture pin are unchanged; false is the case that
// was being described wrongly — the extractor ran to EOS and nothing of its was
// cut, and the yield was only seen at the cluster gist or at the stage boundary
// after it, where "complete candidate(s) committed before he spoke" reads as a
// truncation that did not happen.
static inline std::string partial_commit_line(size_t committed, size_t fresh,
                                              size_t reinforced, int weight,
                                              bool cut_in_extract = true) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  cut_in_extract
                      ? "%zu complete candidate(s) committed before he spoke "
                        "(%zu new, %zu reinforced; evidence weight %d kept); slice retried later"
                      : "%zu candidate(s) committed; the extractor had finished before he spoke "
                        "(%zu new, %zu reinforced; evidence weight %d kept); slice retried later",
                  committed, fresh, reinforced, weight);
    return buf;
}

struct Runner {
    Plan   plan;

    // knobs
    double idle_needed_s   = 45.0;   // a pause, not a gap between words
    double roll_min_gap_s  = 240.0;
    int    roll_min_turns  = 6;      // r20p3.14.2 (RP7): was 8 — start sooner
    double cut_min_gap_s   = 120.0;
    // ── r20p3.14.2 (RP7): bound the slice, and start before the arm point ──────
    //
    // compact_roll consolidates everything since the last pass. On the FIRST
    // roll consolidated_upto is 0, so the slice is the whole session — 71 turns
    // in S9, which tripped run_consolidation's two-pass path and took four
    // minutes. And rolls could not begin before n_arm (49% fill), by which time
    // there is always a whole session to chew. The design guaranteed that the
    // first roll would be the most expensive one.
    //
    // Two changes, and they only ever make the SAME work happen in more, smaller
    // pieces: a backlog now drains over several idle windows, and the first
    // window opens at the notice point rather than the arm point. By the time
    // arming matters the backlog is near zero and the arm-point roll is cheap.
    int    roll_max_turns  = 14;     // never hand the extractor more than this
    bool   roll_from_notice = true;  // opportunistic rolls legal from n_notice
    // ── r20p3.14.2 (RP8): price the operation from measurement ─────────────────
    //
    // Nothing in the plan could say what a roll costs in seconds, so nothing
    // could decline to start one in a window too short to hold it. This is the
    // measured wall time of the last completed roll, fed back by the seam.
    double last_roll_secs  = 0.0;
    double roll_secs_ema   = 0.0;    // smoothed, for the log line

    // state
    bool   noticed        = false;
    int    cuts           = 0;
    int    rolls          = 0;
    int    defers         = 0;
    int    turns_since_roll = 0;
    double last_roll_s    = -1e9;
    double last_cut_s     = -1e9;
    bool   rearmed        = true;    // hysteresis, so a cut cannot immediately re-fire
    bool   defer_pending  = false;   // one bounded deferral per cut cycle
    bool   deferred_once  = false;

    void note_turn() { turns_since_roll++; }

    // Hers to ask for, and hers to put off once. Neither is a veto: a deferral
    // is honoured only while there is still room for another turn, because the
    // alternative to cutting late is truncation, and truncation is the thing
    // this whole mechanism exists to prevent.
    // r20p3.14 (RA8) recorded that nothing set Situation::she_asked, so the
    // one path that lets her CHOOSE the boundary rather than have it imposed
    // at the deadline was unreachable in the binary; what was missing was a
    // lexicon for her asking, and after that round's five lexicons firing on
    // ordinary English (two of them inverting a consent decision) adding a
    // sixth in a hurry would have been the wrong trade.
    //
    // ── r24.8 (census, INVERSE class): HALF of that is now stale, and
    // it read as current fact for two rounds. §53 (2026-08-10) wired the ask:
    // aseam::asked_for_a_moment recognises the conjunction (a first-person
    // request frame AND a self-directed purpose) on talk-llama.cpp's said-this-
    // turn path, it latches athena_asked_moment, and `compact_service` feeds
    // that into sit.she_asked before every poll. `she_asked` reaches the grant
    // in `poll()` on a live path. request_now() is correctly empty BECAUSE the
    // seam sets the flag directly — which is what its one-line comment says,
    // and which the paragraph above flatly contradicted.
    //
    // What IS still dead is the other half, and it is dead the way this block
    // always claimed the first half was: request_defer() has no production
    // caller (only the r21-full.6 and compact fixtures), so defer_pending is
    // false for the whole life of the binary and the deferral disjunction,
    // `(defer_pending || s.image_in_play)`, reduces to its second half. Her
    // one bounded deferral is still unreachable. That is recorded, not fixed:
    // "put this off once" needs a lexicon of its own, and the ask lexicon
    // above is the model for how much care that costs — measured at 8/8 real
    // asks and 0/16 ordinary sentences before it was allowed to ship.
    void request_now()  { }          // unused: the seam sets Situation::she_asked
    void request_defer() { defer_pending = true; }

    void note_roll(double now_s, double took_s = 0.0) {
        rolls++; last_roll_s = now_s; turns_since_roll = 0;
        if (took_s > 0.0) {
            last_roll_secs = took_s;
            roll_secs_ema  = (roll_secs_ema <= 0.0) ? took_s
                                                    : 0.6 * roll_secs_ema + 0.4 * took_s;
        }
    }
    void note_cut(double now_s) {
        cuts++; last_cut_s = now_s; rearmed = false;
        // 14.1 (review): the room being empty again means the next fill-up is
        // a new event. `noticed` was set once and never cleared, so she felt
        // her memory tightening exactly once per SESSION, not once per cycle
        // — every cut after the first arrived with no warning she could
        // voice. (`rearmed` deliberately stays false here: RC21's observed-
        // low-fill re-arm plus the continuous idle polling already covers the
        // pasted-article case, and the pinned hysteresis tests are right that
        // a note_cut whose fill never actually dropped must not re-cut.)
        noticed = false;
        defer_pending = false; deferred_once = false;
        // ── r24.6 (WO-26): the cut did NOT consolidate ─────────────────────
        //
        // The deleted line was `turns_since_roll = 0; last_roll_s = now_s;
        // // the cut consolidated too`. compact_cut contains no
        // run_consolidation call at all, and compact_service says so in as many
        // words: "compact_upto is deliberately NOT advanced here. A cut writes
        // the bridge and her self; it does not extract."
        //
        // S19: at the cut, mem_transcript held 83 lines against compact_upto 26
        // — 57 lines owed. The reset demanded six further turns AND 240 s before
        // a roll could be due again; three turns and 248 s remained. No roll
        // ever ran. The whole vision sequence, the consciousness and
        // moral-patiency exchange, the second dream and the closing promise
        // reached memory only through the single exit pass — precisely the
        // "single point of failure" the sibling comment claims to avoid.
        //
        // last_cut_s, rearmed and noticed all still move: those describe the
        // CUT, which did happen.
    }

    bool safe_moment(const Situation &s) const {
        return s.between_turns && s.gpu_healthy && !s.tts_active && !s.job_pending;
    }

    // r24.17: the idle wake must ask the scheduler that will actually run.
    // Its old seam mirror missed notice-point rolls, woke through a measured
    // slow-roll window/cut cooldown, and never observed the low fill that
    // re-arms a completed cut. A copy preserves the wait callback's read-only
    // contract while detecting one-shot state transitions as well as actions.
    // Empty transcript slices cannot perform a roll and must not churn VAD.
    // Production ATHENA_COMPACT_IDLE_PREVIEW=0 uses the former seam predicate.
    bool idle_due(int n_past, const Situation &s, bool have_roll_slice = true) const {
        Runner next = *this;
        const Act act = next.poll(n_past, s);
        return act == Act::CUT || act == Act::NOTICE ||
               (act == Act::ROLL && have_roll_slice) ||
               next.rearmed != rearmed || next.deferred_once != deferred_once;
    }

    Act poll(int n_past, const Situation &s) {
        if (!plan.feasible) return Act::NONE;

        // Hysteresis: re-arm only once the fill has genuinely come back down.
        // r21-full.4 (RC21): ...or once a cut has actually happened, which is
        // the same fact. `rearmed` was set false by note_cut() and true only by
        // a poll that OBSERVED n_past < n_arm, so a single turn carrying the
        // fill from post_cut past n_arm in one step -- a pasted article, the
        // browsing turn this planner exists to absorb -- meant that observation
        // never occurred and poll() could never return CUT again. Measured on
        // the shipped config: one 34,681-token turn after the first cut, and
        // n_past ran past n_ctx with rearmed stuck at 0, handing the session to
        // stock llama.cpp truncation, which is the exact failure r21-P0 exists
        // to prevent. The fill genuinely IS back at post_cut the instant a cut
        // completes; cut_min_gap_s and n_deadline still provide the hysteresis.
        if (!rearmed && (n_past < plan.n_arm || n_past <= plan.post_cut)) rearmed = true;

        // The felt signal is not gated on a safe moment — it is a feeling, not
        // an operation, and it is the one part of this she is meant to have
        // BEFORE anything happens.
        if (!noticed && n_past >= plan.n_notice) { noticed = true; return Act::NOTICE; }

        if (!safe_moment(s)) return Act::NONE;

        // She asked. Anywhere above the arm point this is simply granted.
        if (s.she_asked && rearmed && n_past >= plan.n_arm &&
            s.now_s - last_cut_s >= cut_min_gap_s)
            return Act::CUT;

        if (n_past >= plan.n_deadline && rearmed &&
            s.now_s - last_cut_s >= cut_min_gap_s) {
            // A deferral is honoured once, and only while a further turn still
            // fits underneath the ceiling.
            const bool room_for_another = (long long) n_past + 2LL * plan.delta + plan.safety <= plan.n_ctx;
            if ((defer_pending || s.image_in_play) && !deferred_once && room_for_another) {
                // 14.1 (review): the counter existed and nothing ever moved it
                // — dead instrumentation reads as "never happened" in every
                // status line that renders it.
                deferred_once = true; defer_pending = false; defers++;
                return Act::NONE;
            }
            return Act::CUT;
        }

        // Rolling consolidation. Opportunistic above the arm point; compulsory
        // above the force point, because arriving at the deadline with nothing
        // consolidated turns a ninety-second cut into a five-minute one.
        const bool due = turns_since_roll >= roll_min_turns &&
                         s.now_s - last_roll_s >= roll_min_gap_s;
        if (due && n_past >= plan.n_force) return Act::ROLL;
        // r20p3.14.2 (RP8): a window has to be long enough to hold the work. The
        // estimate is the last roll's measured wall time; with no measurement
        // yet the old unconditional behaviour stands, so the first roll of a
        // fresh install is unchanged. Above the force point this is ignored —
        // there, not consolidating is the more expensive mistake.
        const bool window_holds = last_roll_secs <= 0.0 ||
                                  s.idle_s >= std::min(last_roll_secs, 3.0 * idle_needed_s);
        if (due && n_past >= plan.n_arm && s.idle_s >= idle_needed_s && window_holds)
            return Act::ROLL;
        // r20p3.14.2 (RP7): opportunistic rolls from the NOTICE point, so the
        // backlog is drained in small pieces long before the arm point makes it
        // compulsory. Deliberately stricter than the arm-point rule: it wants a
        // genuinely long pause, not merely an idle one.
        if (due && roll_from_notice && n_past >= plan.n_notice &&
            s.idle_s >= 2.0 * idle_needed_s && window_holds)
            return Act::ROLL;

        // ── r24.14 (WO-K4): she asked, and a ROLL is what is due ────────────
        // The `she_asked` grant at the top of this function returns CUT and
        // only above `plan.n_arm`. Below the arm point her ask reaches nothing
        // at all — and below the arm point is where the whole of S1's real
        // trouble sat: two rolls destroyed by him speaking and a debt of 95
        // turns owed, with no way for her to ask for the one operation that
        // drains it.
        //
        // Placed LAST, deliberately: it is gated on `n_past < plan.n_arm`, so
        // none of the branches above can fire on the same poll (the deadline
        // and force points are both above the arm point, the arm-point roll
        // tests `n_past >= plan.n_arm`, and the she_asked CUT grant does too).
        // The only transition it can create is NONE -> ROLL. Every other gate
        // is untouched: still inside `safe_moment`, still `due`
        // (`turns_since_roll >= roll_min_turns` AND `roll_min_gap_s`), still
        // subject to `plan.feasible`.
        //
        // No idle requirement, and that is the point of the branch rather than
        // an omission: the three roll arms above ask for an idle window because
        // the runner is guessing that she is not busy. Here she has said so.
        //
        // MEASURED: zero fires on the real corpus, because `asked_for_a_moment`
        // fired 0 times in 112 turns she spoke. This changes nothing on Igor's
        // data. ATHENA_ASK_GRANTS_ROLL=0 restores r24.13's poll exactly.
        if (ask_grants_roll_on() && s.she_asked && due && n_past < plan.n_arm)
            return Act::ROLL;

        return Act::NONE;
    }
};

} // namespace acmp
