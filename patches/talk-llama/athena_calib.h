// athena_calib.h — the emotion-floor calibration mathematics, pure and testable.
//
// r19.6. The interactive calibrator used to be 450 lines of Python embedded in a
// shell script, which violated ATHENA's one hard architectural rule: zero Python
// at runtime. This header holds everything the calibrator DECIDES — separation,
// floors, adaptivity, take quality — with no I/O, no audio, no ONNX and no
// terminal, so all of it is exercised by the standalone battery exactly like the
// rest of the substrate. `athena-emotion-calibrate.cpp` is thin I/O around it.
//
// What is being calibrated, restated so the code below reads as what it is:
// emotion2vec returns nine probabilities per utterance; ATHENA picks the highest
// NON-neutral class and emits a tag only if that class clears its own floor. So
// there is exactly one dial per class, and calibration is the question "where,
// on THIS voice through THIS microphone, does the intended register separate
// from everything else?" — answered per class, from labelled takes.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

namespace acal {

// A floor of exactly this retires a class: a probability can never reach it.
static constexpr float RETIRED = 1.01f;
// Below this margin between the true and false distributions, a floor placed
// between them is fitting noise rather than finding a boundary.
static constexpr float MIN_MARGIN = 0.04f;
// Clearance left under the weakest true take, so a slightly flatter delivery on
// the day still lands. Bounded by the margin so it can never cross max(false).
static constexpr float TRUE_HEADROOM = 0.02f;

// ── trimming a take to the speech inside it ──────────────────────────────────
//
// The capture buffer spans from "ENTER was pressed" to "they stopped", so most
// of it is the person deciding to start. Grading THAT as the take is wrong in
// both directions: a two-second sentence inside a nine-second buffer passes the
// duration gate it should have failed, and the model is handed seven seconds of
// room tone to average over.
//
// A windowed RMS scan over the buffer itself, rather than the live poll loop,
// so the result does not depend on how punctually the poll thread was scheduled.
// Margins on both sides: speech onsets are gradual, and clipping the first
// consonant changes what the model hears.
struct SpeechSpan {
    size_t begin = 0, end = 0;      // sample indices, end exclusive
    float  voiced_s = 0.0f;         // how much of it was above the gate
    bool   found = false;
};

inline SpeechSpan find_speech(const std::vector<float> &pcm, int sample_rate,
                              float gate = 0.010f, int win_ms = 50,
                              int pad_lead_ms = 250, int pad_tail_ms = 300) {
    SpeechSpan s;
    if (pcm.empty() || sample_rate <= 0) return s;
    const size_t win = (size_t) std::max(1, (sample_rate * win_ms) / 1000);
    size_t first = pcm.size(), last = 0, voiced_windows = 0;
    for (size_t i = 0; i + win <= pcm.size(); i += win) {
        double acc = 0.0;
        for (size_t k = 0; k < win; k++) acc += (double) pcm[i + k] * (double) pcm[i + k];
        const float r = (float) std::sqrt(acc / (double) win);
        if (r >= gate) {
            if (i < first) first = i;
            last = i + win;
            voiced_windows++;
        }
    }
    if (voiced_windows == 0) return s;
    const size_t lead = (size_t) ((sample_rate * pad_lead_ms) / 1000);
    const size_t tail = (size_t) ((sample_rate * pad_tail_ms) / 1000);
    s.begin    = first > lead ? first - lead : 0;
    s.end      = std::min(pcm.size(), last + tail);
    s.voiced_s = (float) (voiced_windows * win) / (float) sample_rate;
    s.found    = s.end > s.begin;
    return s;
}

// ── take quality ─────────────────────────────────────────────────────────────

//
// The gates exist because a bad take poisons the distribution it lands in, and
// the person cannot hear that it was bad. Every threshold is the one ATHENA
// herself uses where she has an opinion: MIN_SAMPLES (0.3 s) is the tagger's own
// floor, and the S6 false positive was on a 1.86 s utterance, which is why the
// useful band starts above that rather than at the tagger's hard minimum.
struct TakeQuality {
    bool  ok         = false;
    bool  too_short  = false;
    bool  too_long   = false;
    bool  too_quiet  = false;
    bool  clipped    = false;
    const char *why  = "";
};

// `dur_s` is the VOICED duration (see SpeechSpan::voiced_s), not the padded span
// and not the length of the buffer.
//
// r21-full.5 (R5-X): it used to be the padded span, which is 550 ms of silence
// longer than the speech by construction and which stretches across internal
// pauses. So 0.70 s of real speech passed the 1.2 s gate, and a 0.4 s false
// start plus a 3 s pause plus a 0.5 s take was graded a 4.55 s take — right in
// the middle of the "3 to 6 seconds" target — and its nine probabilities were
// filed as a TRUE reading of how that emotion sounds. `voiced_s` is the honest
// measure, it was already computed, and nothing read it.
// `no_audio` is the case worth separating loudly: zero captured samples is a
// broken capture path, not a person who spoke too briefly, and telling them
// "too short — aim for 3 to 6 seconds" when the microphone delivered nothing at
// all sends them to re-record instead of to the device list.
inline TakeQuality grade_take(float dur_s, float rms, float clip_frac, bool no_audio = false) {
    TakeQuality q;
    if (no_audio)            { q.too_quiet = true; q.why = "no audio captured at all"; return q; }
    // r24.17: one nonfinite microphone sample propagates into RMS, and NaN
    // made every old rejection comparison false. Reject before ONNX/file-in.
    static const bool finite_takes = [] {
        const char *e = std::getenv("ATHENA_CALIB_FINITE_TAKES");
        return !(e && e[0] == '0');
    }();
    if (finite_takes && (!std::isfinite(dur_s) || !std::isfinite(rms) || !std::isfinite(clip_frac))) {
        q.why = "invalid audio samples — check the capture device";
        return q;
    }
    if (dur_s < 1.2f)        { q.too_short = true; q.why = "too brief — say the whole line, around 3 to 6 seconds"; return q; }
    // R5-X: the capture cap is 20 s and this gate is 15, so an over-long take
    // can be OBSERVED and rejected with the message that already exists. At a
    // 15 s cap the buffer could never exceed 15.000 s, the test is strictly
    // greater, and a swept 15,001 reachable durations fired this branch zero
    // times — so the fifteen-second monologue the gate exists to reject was
    // graded "good".
    if (dur_s > 15.0f)       { q.too_long  = true; q.why = "too long — one sentence is enough"; return q; }
    if (rms  < 0.010f)       { q.too_quiet = true; q.why = "too quiet — move closer or turn the input gain up"; return q; }
    if (clip_frac > 0.005f)  { q.clipped   = true; q.why = "clipping — back off the mic or lower the gain"; return q; }
    q.ok = true; q.why = "good";
    return q;
}


// Fraction of samples at or beyond full scale.
inline float clipped_fraction(const std::vector<float> &pcm) {
    if (pcm.empty()) return 0.0f;
    size_t n = 0;
    for (float s : pcm) if (s >= 0.999f || s <= -0.999f) n++;
    return (float) n / (float) pcm.size();
}

inline float rms_of(const std::vector<float> &pcm) {
    if (pcm.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : pcm) acc += (double) s * (double) s;
    return (float) std::sqrt(acc / (double) pcm.size());
}

// ── separation ───────────────────────────────────────────────────────────────
//
// TRUE  = this class's probability on the takes where the person WAS in that
//         register.
// FALSE = the same class's probability on every other take in the session —
//         the neutral takes, and the takes aimed at other classes.
//
// A floor can separate them only if the weakest true reading sits above the
// strongest false one. If it does not, no threshold exists that admits the real
// register without also admitting something else, and the honest outcome is to
// retire the class rather than leave it to fire wrongly. That is not a loss: a
// channel that is wrong is worse than a channel that is silent, which is exactly
// what S6 demonstrated when a single false `disgusted` wrote a permanent
// valence -0.60 episode onto a neutral sentence.
struct Separation {
    bool   separable   = false;
    float  floor       = RETIRED;
    float  min_true    = 0.0f;
    float  max_false   = 0.0f;
    float  margin      = 0.0f;   // min_true - max_false
    size_t n_true      = 0;
    size_t n_false     = 0;
    bool   enough_data = false;
};

inline Separation separate(const std::vector<float> &trues,
                           const std::vector<float> &falses,
                           size_t min_true_takes = 2) {
    Separation s;
    s.n_true  = trues.size();
    s.n_false = falses.size();
    s.enough_data = trues.size() >= min_true_takes;
    if (trues.empty()) return s;                       // nothing to admit

    s.min_true  = *std::min_element(trues.begin(), trues.end());
    s.max_false = falses.empty() ? 0.0f : *std::max_element(falses.begin(), falses.end());
    s.margin    = s.min_true - s.max_false;

    if (!s.enough_data) return s;                      // not yet decidable
    if (s.margin <= MIN_MARGIN) return s;              // overlapping -> retire

    // Sit just under the weakest true reading, but never at or below the
    // strongest false one. With a margin above MIN_MARGIN both bounds are
    // satisfiable; the midpoint is the fallback when the headroom would cross.
    float f = s.min_true - TRUE_HEADROOM;
    if (f <= s.max_false) f = 0.5f * (s.min_true + s.max_false);
    // A floor below the base default admits more than ATHENA's own baseline
    // would; that is legitimate (it is measured), but never let it go negative
    // or reach 1.0, where it would silently mean "retired".
    if (f < 0.01f) f = 0.01f;
    if (f > 0.99f) f = 0.99f;

    s.separable = true;
    s.floor     = f;
    return s;
}

// ── r24.6 (WO-04): SHARE space ───────────────────────────────────────────────
//
// THE DEFECT. `separate()` above is pure boundary mathematics and is correct in
// any space. What was wrong is the space its INPUTS were measured in. The
// calibrator fed it the raw per-class posterior from acted takes, where the
// neutral mass is small, so a class's absolute posterior is close to its
// confidence. `EmotionTagger::decide()` applies the resulting number as a SHARE
// threshold on conversational speech, where the median neutral posterior over
// 81 live utterances is 0.996. Those are two different quantities wearing one
// name (the R8-A comment in athena_emotion.h diagnoses it at length and fixes
// the runtime; the calibrator was never brought along). Running a calibration
// pass today therefore derives floors that are unreachable at runtime, i.e. it
// SILENCES the channel — the exact failure mode S12 shipped with.
//
// THE SHAPE OF THE FIX. `separate()` is deliberately left byte-identical, and
// so is every existing call site: the change is that the caller now measures in
// share space before calling it. `to_share()` is the transform, and it is the
// same arithmetic decide() performs — including WO-02's exclusion of retired
// mass from the denominator. `shares_of()` maps a set of recorded takes.
//
// WHY THE TAKES MUST NOW BE RECORDED AS 9-VECTORS. A share cannot be computed
// from a scalar. The calibrator kept only `probs[gi]` per take
// (`std::vector<std::vector<float>> trues/falses`), which is why this could not
// simply be patched inside separate(): the information was thrown away at
// capture time. See athena-emotion-calibrate.cpp.
using Posterior = std::array<float, 9>;

// Indices, mirrored from EmotionTagger so this header stays dependency-free.
// Pinned equal to the tagger's enum by a fixture arm.
enum { CAL_NEUTRAL = 4, CAL_OTHER = 5, CAL_UNK = 8 };

// The runtime rule's share, for ONE named class — decide() computes it only for
// the class it picked, and calibration needs it for the class under test.
// `retired[i] == true` means "excluded from the pick", i.e. floor > 1.0.
inline float to_share(const Posterior &p, const bool retired[9], int cls) {
    if (cls < 0 || cls > 8) return 0.0f;
    float denom = 1.0f - p[CAL_NEUTRAL] - p[CAL_OTHER] - p[CAL_UNK];
    if (denom < 0.0f) denom = 0.0f;
    for (int i = 0; i < 9; ++i) {
        if (i == CAL_NEUTRAL || i == CAL_OTHER || i == CAL_UNK) continue;
        if (retired[i]) denom -= p[i];                 // WO-02: not a candidate,
    }                                                  // so not evidence for one
    if (denom < 0.0f) denom = 0.0f;
    if (denom <= 1e-6f) return 0.0f;
    float sh = p[cls] / denom;
    if (sh > 1.0f) sh = 1.0f;
    return sh;
}

inline std::vector<float> shares_of(const std::vector<Posterior> &takes,
                                    const bool retired[9], int cls) {
    std::vector<float> out;
    out.reserve(takes.size());
    for (const auto &t : takes) out.push_back(to_share(t, retired, cls));
    return out;
}

// The evidence gate decide() applies before the share question is asked at all.
inline float evidence_of(const Posterior &p) {
    float e = 1.0f - p[CAL_NEUTRAL] - p[CAL_OTHER] - p[CAL_UNK];
    return e < 0.0f ? 0.0f : e;
}

// ── the retirement fixpoint ──────────────────────────────────────────────────
//
// Excluding retired mass from the denominator (WO-02) raises every share, which
// can make a class separable that was not — and retirement is decided FROM the
// separation. The dependency is circular, so it is resolved by iteration:
// derive with the current mask, recompute the mask, repeat until it stops
// moving. It is not monotone in either direction (excluding mass lifts the true
// AND the false readings, so the margin can move either way), so this both caps
// the iterations and detects a two-cycle, falling back to the first pass rather
// than picking an arbitrary member of the cycle. `iterations` and `converged`
// are reported so the tool can print them.
struct ShareDerivation {
    Separation sep[9];                 // per class id, in share space
    bool       retired[9] = {false,false,false,false,false,false,false,false,false};
    int        iterations = 0;
    bool       converged  = true;
    int        dropped_no_evidence = 0;   // takes below the runtime evidence gate
};

// ── the evidence gate belongs in the derivation too ──────────────────────────
//
// This is the arm the work order does not mention and without which share-space
// calibration RETIRES EVERYTHING. Round 1 records a FLAT take per class
// (`one_take(SCRIPT[c], -1, ...)`), filed as a FALSE reading for every class. A
// flat delivery reads neutral ~0.98, so a 0.010 stray on it has a SHARE of
// 0.010/0.025 = 0.40. Feed that in as a false reading and max_false jumps from
// 0.01 to 0.40 for every class at once, margins collapse, and the pass retires
// classes that separate perfectly well.
//
// It is also simply wrong: the runtime would never emit on that take, because
// decide() gates on `evidence >= evidence_floor` BEFORE it looks at the share.
// A reading the runtime cannot act on is not a false positive; it is out of
// scope. So the derivation applies the same gate the runtime does, and a take
// below it is dropped from BOTH the true and the false sets. `usable_evidence`
// is exported so the tool can say how many takes that removed.
inline bool passes_evidence(const Posterior &p, float evidence_floor) {
    return evidence_of(p) >= evidence_floor;
}

// `class_takes[i]` = the takes whose intended register was class i (TRUE for i,
// FALSE for every other class). `extra_falses` are takes with no intended class
// — the flat references — which are FALSE readings for every class. Classes with
// no script entry pass an empty set.
// r24.16: now consumed by the interactive calibrator. It retains complete
// nine-class takes and derives in the same space as the runtime. The old
// scalar derivation remains behind ATHENA_CALIB_SHARE=0 in that tool.
inline ShareDerivation derive_share_floors(const std::vector<Posterior> class_takes[9],
                                           const bool in_script[9],
                                           const std::vector<Posterior> &extra_falses = {},
                                           size_t min_true_takes = 2,
                                           float evidence_floor = 0.10f) {
    ShareDerivation d;
    bool mask[9] = {false,false,false,false,false,false,false,false,false};
    bool seen[10][9]; int n_seen = 0;
    for (int it = 0; it < 9; ++it) {
        d.iterations = it + 1;
        for (int c = 0; c < 9; ++c) {
            if (!in_script[c]) { d.sep[c] = Separation(); continue; }
            std::vector<Posterior> tr, fa;
            for (const auto &t : class_takes[c])
                if (passes_evidence(t, evidence_floor)) tr.push_back(t);
            for (int o = 0; o < 9; ++o) if (o != c && in_script[o])
                for (const auto &t : class_takes[o])
                    if (passes_evidence(t, evidence_floor)) fa.push_back(t);
            for (const auto &t : extra_falses)
                if (passes_evidence(t, evidence_floor)) fa.push_back(t);
            d.sep[c] = separate(shares_of(tr, mask, c), shares_of(fa, mask, c), min_true_takes);
        }
        d.dropped_no_evidence = 0;
        for (const auto &t : extra_falses) if (!passes_evidence(t, evidence_floor)) d.dropped_no_evidence++;
        for (int c = 0; c < 9; ++c) if (in_script[c])
            for (const auto &t : class_takes[c]) if (!passes_evidence(t, evidence_floor)) d.dropped_no_evidence++;
        bool next[9];
        for (int c = 0; c < 9; ++c) next[c] = in_script[c] ? !d.sep[c].separable : false;
        bool same = true;
        for (int c = 0; c < 9; ++c) if (next[c] != mask[c]) same = false;
        for (int c = 0; c < 9; ++c) mask[c] = next[c];
        if (same) { for (int c = 0; c < 9; ++c) d.retired[c] = mask[c]; return d; }
        for (int k = 0; k < n_seen; ++k) {          // two-cycle (or longer) detected
            bool eq = true;
            for (int c = 0; c < 9; ++c) if (seen[k][c] != mask[c]) eq = false;
            if (eq) {
                d.converged = false;
                bool none[9] = {false,false,false,false,false,false,false,false,false};
                for (int c = 0; c < 9; ++c) {        // fall back to the first pass
                    if (!in_script[c]) { d.sep[c] = Separation(); continue; }
                    std::vector<Posterior> tr, fa;
                    for (const auto &t : class_takes[c])
                        if (passes_evidence(t, evidence_floor)) tr.push_back(t);
                    for (int o = 0; o < 9; ++o) if (o != c && in_script[o])
                        for (const auto &t : class_takes[o])
                            if (passes_evidence(t, evidence_floor)) fa.push_back(t);
                    for (const auto &t : extra_falses)
                        if (passes_evidence(t, evidence_floor)) fa.push_back(t);
                    d.sep[c] = separate(shares_of(tr, none, c), shares_of(fa, none, c), min_true_takes);
                    d.retired[c] = !d.sep[c].separable;
                }
                return d;
            }
        }
        if (n_seen < 10) { for (int c = 0; c < 9; ++c) seen[n_seen][c] = mask[c]; n_seen++; }
    }
    d.converged = false;
    for (int c = 0; c < 9; ++c) d.retired[c] = mask[c];
    return d;
}

// ── the self-check the absence of which let the current block ship ───────────
//
// Replay the derived floor set against the takes that produced it, UNDER THE
// RUNTIME RULE, and report what she would actually have said. The calibrator
// already had a "validation" pass, but it built ONE-HOT score vectors
// (`float s[9]={0}; s[gi]=pr;`), which makes evidence exactly 1.0 and share
// exactly the raw posterior — so it silently re-derives the absolute rule and
// is structurally incapable of seeing the neutral-dominance problem it exists
// to catch. This replays the real recorded posteriors.
//
// `share_floor` is passed because decide()'s bar is max(class floor, global
// share floor): a derived floor BELOW the global floor is discarded exactly the
// way MIN_HAPPY=0.80 was discarded by share_floor=0.90 (WO-03). A floor set
// that only emits because the caller forgot to lower the global floor is a
// floor set that will be silent in production.
struct ReplayResult {
    int emit_on_true = 0, n_true = 0;      // recall
    int emit_on_false = 0, n_false = 0;    // false positives
    int lost_to_evidence = 0;              // cleared the share bar, failed the evidence gate
    int lost_to_share_floor = 0;           // cleared its own floor, failed the GLOBAL floor
    bool would_be_silent = false;          // the refusal condition
};

// r24.16: the calibrator consumes this replay, then cross-checks it against
// EmotionTagger::decide on the actual vectors and rounded exported floors.
// Count every intended take in the recall denominator, even when the model
// picks a different class. A wrong emitted class is also a false positive.
inline ReplayResult replay_under_runtime_rule(const std::vector<Posterior> class_takes[9],
                                              const bool in_script[9],
                                              const float floors[9],
                                              float share_floor,
                                              const std::vector<Posterior> &extra_falses = {},
                                              float evidence_floor = 0.10f,
                                              float noise_floor    = 0.02f) {
    ReplayResult rr;
    for (int intended = -1; intended < 9; ++intended) {
        if (intended >= 0 && !in_script[intended]) continue;
        const std::vector<Posterior> &set = intended < 0 ? extra_falses : class_takes[intended];
        for (const auto &p : set) {
            // decide(): pick the best non-retired real class
            bool retired[9];
            for (int i = 0; i < 9; ++i) retired[i] = floors[i] > 1.0f;
            int cls = -1; float best = 0.0f;
            for (int i = 0; i < 9; ++i) {
                if (i == CAL_NEUTRAL || i == CAL_OTHER || i == CAL_UNK) continue;
                if (retired[i]) continue;
                if (p[i] > best || cls < 0) { best = p[i]; cls = i; }
            }
            const float ev  = evidence_of(p);
            const float sh  = to_share(p, retired, cls);
            const float fl  = cls >= 0 ? floors[cls] : 1.0f;
            const float bar = fl > share_floor ? fl : share_floor;
            const bool emit = cls >= 0 && ev >= evidence_floor && sh >= bar && best >= noise_floor;
            if (intended >= 0) { rr.n_true++; if (emit && intended == cls) rr.emit_on_true++; }
            else rr.n_false++;
            if (emit && intended != cls) rr.emit_on_false++;
            if (!emit && cls >= 0 && sh >= fl && best >= noise_floor) {
                if (ev < evidence_floor)   rr.lost_to_evidence++;
                else if (sh < share_floor) rr.lost_to_share_floor++;
            }
        }
    }
    rr.would_be_silent = (rr.emit_on_true == 0);
    return rr;
}

// ── adaptivity ───────────────────────────────────────────────────────────────
//
// How many more takes is this class worth? The point is to spend the person's
// voice where it buys something. A class that separated cleanly on the first two
// takes needs nothing more; a class that is close to the boundary is worth
// pushing on, because one more sample may settle it either way; a class whose
// true and false readings are thoroughly interleaved is not going to be rescued
// by a fourth take and asking for one wastes the session.
inline int extra_takes_wanted(const Separation &s, int taken, int budget_per_class) {
    if (taken >= budget_per_class) return 0;
    if (!s.enough_data) return budget_per_class - taken;      // still collecting the minimum
    if (s.separable && s.margin >= 0.25f) return 0;           // clean and wide — done
    if (s.separable) return 1;                                // separable but tight — confirm it
    if (s.margin <= -0.30f) return 0;                         // hopeless; do not spend more voice
    return 1;                                                 // close to the line — one more
}

// Is the class worth offering at all, given what the neutral round already
// showed? A class whose FALSE readings already sit near the top of the range
// cannot be separated by anything the person does next.
inline bool worth_eliciting(float max_false_so_far) { return max_false_so_far < 0.90f; }

// ── the export block ─────────────────────────────────────────────────────────
struct ClassResult {
    std::string label;
    Separation  sep;
    bool        retired = false;
};

// The exact lines to paste into launch-athena-397b.sh. Deliberately emits a line
// for EVERY class, including retired ones: an absent export means "inherit the
// base floor", which is how DISGUSTED came to sit at 0.50 in S6 and produce the
// session's only, false, emit. Silence in a config file is not a decision.
// r24.6 (WO-04): stamped into the export so a block derived in the OLD space is
// detectable on sight. A block with no `# calib:` line predates WO-04 and its
// floors are absolute-posterior floors being read as share floors.
static constexpr const char *CALIB_VERSION = "r24.6";
static constexpr const char *CALIB_SPACE   = "share";

// `share_floor` is emitted too, and this is not cosmetic: decide()'s bar is
// max(per-class floor, ATHENA_EMOTION_SHARE), so a derived floor below the
// global one is silently discarded — the WO-03 defect, reintroduced by the
// calibrator. Pass the value the self-check was run at.
// r24.6 (WO-04, verifier pass): the stamp records the space the CALLER
// actually derived in — it must not assert "share" unconditionally. The
// legacy calibration branch (ATHENA_CALIB_SHARE=0) records
// scalar posteriors and derives on them, i.e. in ABSOLUTE space; an
// unconditional share stamp would have put a FALSE provenance claim on
// exactly the artifact this stamp exists to make detectable. Default
// "absolute" keeps that untouched caller truthful; a caller that derived via
// derive_share_floors()/replay_under_runtime_rule() passes CALIB_SPACE.
inline std::string export_block(const std::vector<ClassResult> &results, float base,
                                float share_floor = -1.0f,
                                const char *derived_space = "absolute") {
    std::string out;
    char buf[240];
    const bool share_space = derived_space && std::string(derived_space) == CALIB_SPACE;
    std::snprintf(buf, sizeof(buf),
                  share_space
                  ? "# calib: version=%s space=%s   # floors below are SHARE thresholds\n"
                    "#        (p / non-neutral non-retired mass), the quantity\n"
                    "#        EmotionTagger::decide() actually compares against.\n"
                  : "# calib: version=%s space=%s   # floors below are ABSOLUTE-posterior\n"
                    "#        thresholds (the pre-WO-04 space); decide() applies floors as\n"
                    "#        SHARE thresholds — re-derive via derive_share_floors().\n",
                  CALIB_VERSION, derived_space ? derived_space : "unknown");
    out += buf;
    if (share_floor >= 0.0f) {
        std::snprintf(buf, sizeof(buf), "export ATHENA_EMOTION_SHARE=%.2f\n", share_floor);
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "export ATHENA_EMOTION_MIN=%.2f\n", base);
    out += buf;
    for (const auto &r : results) {
        std::string up;
        for (char c : r.label) up += (char) ((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
        if (r.retired || !r.sep.separable) {
            std::snprintf(buf, sizeof(buf),
                          "export ATHENA_EMOTION_MIN_%s=1.01   # retired: %s\n", up.c_str(),
                          r.sep.n_true == 0 ? "never elicited"
                                            : (r.sep.enough_data ? "true and false readings overlap"
                                                                 : "too few usable takes"));
        } else {
            std::snprintf(buf, sizeof(buf),
                          "export ATHENA_EMOTION_MIN_%s=%.2f   # true >= %.3f, false <= %.3f (margin %.3f)\n",
                          up.c_str(), r.sep.floor, r.sep.min_true, r.sep.max_false, r.sep.margin);
        }
        out += buf;
    }
    return out;
}

// Every class retired means the channel is honestly silent on this voice. That
// is a legitimate outcome and the launcher should say so explicitly rather than
// leave floors that will occasionally fire.
inline bool all_retired(const std::vector<ClassResult> &results) {
    if (results.empty()) return false;
    for (const auto &r : results) if (r.sep.separable && !r.retired) return false;
    return true;
}

} // namespace acal
