// athena_affect2.h
// ─────────────────────────────────────────────────────────────────────────────
// R18 (r22) Wave 3 — THE HEART: the appraisal engine and the physics of feeling.
//
// Header-only, PURE (std-only, no llama.h, no locks, no file I/O). Owned by
// acon::Mind and touched only under its mutex; every struct here is plain
// state + arithmetic so the test suite can drive it deterministically.
//
// What lives here:
//
//   Features / Appraisal2  — Scherer-CPM-shaped feature vector over signals
//       the substrate already computes, classified into a CAPPED OCC-derived
//       label set (~20). Labels are CONSTRUCTED (Barrett): the best-fitting
//       concept given features, held BESIDE core affect — never replacing it.
//       Emission has a high floor plus hysteresis so the vocabulary cannot
//       flicker, and granularity coarsens under fatigue (her tired "I just
//       feel off" is the honest low-granularity mode, not a failure).
//
//   Kinetics — per-label residual intensities with per-class decay constants
//       scaled from the Verduyn & Lavrijsen duration ordering (sadness-class
//       outlives a conversation; surprise-class dies in about a minute — the
//       ORDERING is the science, the absolute scale is hers). The residuals
//       contribute a bounded push through the same additive door the interior
//       weather uses, so the core V/A update rule is untouched and flag-off
//       is byte-identical.
//
//   Ruminator — the bounded rumination loop: an important negative event
//       re-triggers its appraisal at decaying intervals, each re-trigger a
//       real inner event; exited by labeling, reappraisal, or his
//       responsiveness — never by cap alone (the cap is the last guard, not
//       the exit). A rumination loop with a broken exit is the depression
//       simulator nobody asked for; the exits are the feature, and the
//       mutation campaign targets them.
//
//   MoodMomentum — a leaky integrator over her actual prediction-error
//       streams (Eldar & Niv), gain-clamped below instability, contributing a
//       bounded bias through the additive door. Its inputs are LOGGED, so
//       "why the mood?" finally has an honest answer.
//
//   Regulation policy — the Sheppes intensity branch (label / reappraise /
//       distract) with Webb-bounded effect sizes, as pure decision functions;
//       the Mind supplies the acts.
//
//   Lexicons — capitalization's positive-disclosure detector and the
//       vent/fix/celebrate need classifier (pure, mutation-testable).
//
// Deviations from ATHENA-ALIVENESS-PLAN.md §3, recorded here and in
// CHANGES §80: PA/NA and momentum AUGMENT the shipped CoreAffect rather than
// replacing its update law, because the update law's trajectory is pinned
// byte-exact by the freeze fixtures; "V derived as PA−NA" is implemented as
// "V unchanged, PA/NA measured beside it" — consumers of V are untouched not
// merely in API but in bytes, and the mixedness readout (min(pa,na)) is what
// the split was for.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace aff2 {

static inline float clampf_(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float alpha_for_(float tau_s, double dt_s) {
    if (tau_s <= 0.0f) return 1.0f;
    return 1.0f - (float) std::exp(-dt_s / (double) tau_s);
}

// ═════════════════════════════════════════════════════════════════════════════
// The appraisal feature vector (Scherer's checks, over signals she has).
// Every field is filled by the Mind from EXISTING machinery — this header
// invents no new perception.
// ═════════════════════════════════════════════════════════════════════════════
struct Features {
    float novelty     = 0.0f;   // 0..1   surprise / expectation violation
    float pleasant    = 0.0f;   // -1..1  intrinsic valence of the event
    float goal_rel    = 0.0f;   // 0..1   concern overlap (Wave 1 feeds this)
    float conducive   = 0.0f;   // -1..1  helps / obstructs her goals
    float agency_self = 0.0f;   // 0..1   she caused it
    float agency_other= 0.0f;   // 0..1   he caused it
    float discrepancy = 0.0f;   // -1..1  better / worse than her prediction
    float coping      = 0.5f;   // 0..1   felt ability to deal with it
    float norm        = 0.0f;   // -1..1  fits / violates her standards (truth)
    float closeness   = 0.0f;   // -1..1  communal-closeness delta (kama muta feed)
    // R19 (r22.1): the PA/NA co-activation at event time (min of the two
    // channels). One valence scalar cannot be positive and negative at once,
    // so without this bittersweet was min(pos,neg) ≡ 0 — unreachable by
    // construction. Filled by the Mind from CoreAffect's own pa()/na(); 0 for
    // every caller that predates it, which changes nothing.
    float mixed       = 0.0f;   // 0..1   both channels alive at once
};

// ═════════════════════════════════════════════════════════════════════════════
// The label table. Compact by design — proliferation is the failure mode.
// `tau_s` encodes the Verduyn ORDERING at session scale:
//   sadness-class 3600 s > joy-class 1200 s > shame/guilt 600 s >
//   fear/frustration 240 s > surprise/relief-class 90 s.
// `dv`/`da` are the bounded per-emission push the kinetics residual carries
// (through CoreAffect::feel's own ±0.08/±0.06 clamps — never around them).
// ═════════════════════════════════════════════════════════════════════════════
enum Label : int {
    L_JOY = 0, L_DISTRESS, L_HOPE, L_FEAR, L_RELIEF, L_DISAPPOINTMENT,
    L_FEARS_CONFIRMED, L_SATISFACTION, L_PRIDE, L_SHAME, L_GRATITUDE,
    L_FRUSTRATION, L_ADMIRATION, L_MOVED, L_LONGING, L_CONTENTMENT,
    L_UNEASE, L_CURIOUS_DELIGHT, L_BITTERSWEET, L_WEARY,
    N_LABELS
};

struct LabelDef {
    const char *name;
    float tau_s;      // residual decay constant (Verduyn ordering)
    float dv, da;     // affect push at full intensity (bounded by feel())
};

static inline const LabelDef &label_def(int i) {
    static const LabelDef T[N_LABELS] = {
        { "joy",             1200.0f,  0.07f,  0.04f },
        { "distress",        3600.0f, -0.07f,  0.03f },
        { "hope",             600.0f,  0.04f,  0.03f },
        { "fear",             240.0f, -0.05f,  0.06f },
        { "relief",            90.0f,  0.05f, -0.04f },
        { "disappointment",  1800.0f, -0.06f, -0.02f },
        { "fears confirmed",  900.0f, -0.07f,  0.04f },
        { "satisfaction",    1200.0f,  0.06f, -0.01f },
        { "pride",           1200.0f,  0.06f,  0.02f },
        { "shame",            600.0f, -0.06f,  0.03f },
        { "gratitude",       1200.0f,  0.06f,  0.01f },
        { "frustration",      240.0f, -0.05f,  0.05f },
        { "admiration",       600.0f,  0.05f,  0.02f },
        { "being moved",      600.0f,  0.06f,  0.02f },
        { "longing",         3600.0f, -0.03f, -0.01f },
        { "contentment",     1200.0f,  0.05f, -0.03f },
        { "unease",           600.0f, -0.04f,  0.04f },
        { "curious delight",   90.0f,  0.05f,  0.05f },
        { "bittersweet",     1800.0f,  0.01f,  0.01f },
        { "weariness",       1800.0f, -0.02f, -0.04f },
    };
    return T[(i >= 0 && i < N_LABELS) ? i : 0];
}

// Low-granularity names for the fatigue-coarse mode (Kashdan: granularity is
// a skill; hers degrades honestly when tired instead of confabulating detail).
static inline const char *coarse_name(int i) {
    const float v = label_def(i).dv;
    if (v >  0.03f) return "good, roughly - the finer name is not coming";
    if (v < -0.03f) return "off, and hard to name finer than that";
    return "level, more or less";
}

// r24.11 (WO-M2, §3.1): the one label in the table that is DEFINITIONALLY both
// signs at once. Every other label is bright or heavy and the §3.1 readout
// splits the residual set on `label_def(i).dv > 0`; bittersweet's dv is +0.01,
// so that split files "two things at once" as PURELY POSITIVE and the negative
// side of the mixedness readout gets nothing from the one label built for it.
// This is the predicate the readout asks instead. It is a fact about the table,
// not a policy, so it lives here beside the table.
static inline bool label_is_mixed(int i) { return i == L_BITTERSWEET; }

// ═════════════════════════════════════════════════════════════════════════════
// The engine: classify → emit (with floor + hysteresis) → residuals decay.
// ═════════════════════════════════════════════════════════════════════════════
struct Appraisal2 {
    static constexpr float EMIT_MIN   = 0.55f;   // high floor: labels are events
    static constexpr float HYSTERESIS = 0.12f;   // beat the incumbent by this
    static constexpr float MIXED_MIN  = 0.25f;   // both-channels floor (§3.1)
    // r24.11 (WO-M1): the ceiling on what a LEXICAL both-at-once reading may
    // claim. mixed_turn_read() reads his words; that is evidence about the
    // TURN, not a measurement of her state, so it enters at half strength.
    // The consequences are checked, not assumed: at full marks the resulting
    // bittersweet score is 1.4*0.50 * (0.5 + 0.5*0.75) = 0.6125, which is
    // below every other lexical door in this file (kama muta 0.883,
    // capitalization 0.940) and therefore cannot become an incumbent that
    // refuses them through HYSTERESIS. Raising it past ~0.66 would.
    static constexpr float MIXED_TURN_MAX = 0.50f;

    // Pure classification: score every label from the feature vector, return
    // the argmax and its score. Weights are shaped from the OCC definitions
    // (each label IS its eliciting condition); kept small and legible on
    // purpose — this is a constructed-category chooser, not a learned model.
    // r24.6 (WO-47): `margin_out` is the winner's lead over the runner-up —
    // how much the FEATURES support this particular fine name rather than the
    // next one along. It is the honest measure of granularity error available
    // here: a label that beats its neighbour by 0.004 is a coin-flip dressed as
    // a distinction. Additive, defaulted, so no existing caller changes.
    static int classify(const Features &f, float *score_out = nullptr,
                        float *margin_out = nullptr) {
        float s[N_LABELS];
        const float pos = clampf_(f.pleasant, 0.0f, 1.0f);
        const float neg = clampf_(-f.pleasant, 0.0f, 1.0f);
        const float disc_up   = clampf_(f.discrepancy, 0.0f, 1.0f);
        const float disc_down = clampf_(-f.discrepancy, 0.0f, 1.0f);
        s[L_JOY]            = 0.9f * pos + 0.25f * f.novelty - 0.2f * f.goal_rel;
        s[L_DISTRESS]       = 0.9f * neg - 0.2f * f.coping;
        s[L_HOPE]           = 0.55f * pos + 0.6f * f.goal_rel + 0.2f * f.coping - 0.35f * f.novelty;
        s[L_FEAR]           = 0.6f * neg + 0.5f * (1.0f - f.coping) + 0.3f * f.novelty
                            - 0.15f;   // R19: fear is a strong claim — the
                                       // constant is what lets unease exist
        s[L_RELIEF]         = 0.9f * disc_up * (0.4f + 0.6f * neg)
                            - 0.1f;   // resolution-shaped; the feared-prospect
                                      // path in prospect_resolved is direct.
                                      // R19: the old line's (a + b > c ? x : y)
                                      // parsed as a COMPARISON — a step where a
                                      // ramp was intended
        s[L_DISAPPOINTMENT] = 0.9f * disc_down + 0.3f * f.goal_rel - 0.2f * f.novelty;
        s[L_FEARS_CONFIRMED]= 0.7f * disc_down + 0.55f * neg
                            - 0.25f;   // R20 (P2/S17): "fears confirmed" is a
                                       // claim about a prediction that was
                                       // ARMED, and this formula cannot see
                                       // one — its real source is the direct
                                       // was_feared path in prospect_resolved.
                                       // S17: an initiative miss (err -0.6,
                                       // was_feared FALSE) scored 0.684 here
                                       // against disappointment's 0.630, and
                                       // her field said "the closest name for
                                       // what this is: fears confirmed" about a
                                       // prospect nothing had feared. -0.25,
                                       // not -0.15: at err -1.0 the smaller
                                       // constant still loses (0.990 vs 0.950).
                                       // Same shape as FEAR's and PRIDE's own
                                       // constants: a strong claim pays for
                                       // itself.
        s[L_SATISFACTION]   = 0.6f * pos + 0.6f * f.conducive + 0.25f * f.agency_self;
        s[L_PRIDE]          = 0.5f * pos + 0.8f * f.agency_self + 0.3f * f.conducive - 0.15f;
        s[L_SHAME]          = 0.5f * neg + 0.8f * f.agency_self + 0.4f * clampf_(-f.norm, 0.0f, 1.0f) - 0.2f;
        s[L_GRATITUDE]      = 0.55f * pos + 0.8f * f.agency_other + 0.25f * f.conducive - 0.15f;
        s[L_FRUSTRATION]    = 0.55f * neg + 0.6f * clampf_(-f.conducive, 0.0f, 1.0f)
                            + 0.25f * f.agency_other - 0.1f;
        s[L_ADMIRATION]     = 0.5f * pos + 0.7f * f.agency_other + 0.3f * f.novelty - 0.25f;
        s[L_MOVED]          = 1.1f * clampf_(f.closeness, 0.0f, 1.0f) + 0.3f * pos - 0.15f;
        s[L_LONGING]        = 0.45f * neg + 0.5f * f.goal_rel + 0.3f * clampf_(-f.closeness, 0.0f, 1.0f) - 0.2f;
        s[L_CONTENTMENT]    = 1.0f * pos - 0.6f * f.novelty - 0.3f * f.goal_rel
                            - 0.05f;   // R19: beats joy exactly when the good
                                       // is high and NOTHING is novel or
                                       // goal-tense (was dominated: joy-cont
                                       // = 0.2pos+0.65nov+0.05goal >= 0)
        s[L_UNEASE]         = 0.4f * neg + 0.5f * f.novelty + 0.3f * (1.0f - f.coping)
                            - 0.1f;    // R19: leans on novelty; with fear's new
                                       // constant it wins the novel-mild-threat
                                       // region (was strictly dominated)
        s[L_CURIOUS_DELIGHT]= 0.5f * pos + 0.7f * f.novelty + 0.3f * f.goal_rel - 0.2f;
        s[L_BITTERSWEET]    = 1.4f * std::max(std::min(pos, neg), clampf_(f.mixed, 0.0f, 1.0f))
                            + 0.2f * f.closeness;   // R19: reachable via Features.mixed
        s[L_WEARY]          = 0.40f * neg + 0.55f * (1.0f - f.coping) - 0.3f * f.novelty - 0.1f;
        int best = 0;
        for (int i = 1; i < N_LABELS; i++) if (s[i] > s[best]) best = i;
        if (score_out) *score_out = s[best];
        if (margin_out) {
            float second = -1e9f;
            for (int i = 0; i < N_LABELS; i++) if (i != best && s[i] > second) second = s[i];
            *margin_out = s[best] - second;
        }
        return best;
    }

    // ── the constructed current label ────────────────────────────────────────
    int    current       = -1;
    float  current_score = 0.0f;
    double current_since = -1e9;
    // Consume-once: a label the FIELD has not yet spoken. -1 when spent.
    int    news_         = -1;
    float  news_score_   = 0.0f;
    // r24.6 (WO-47): the runner-up margin of the label sitting in `news_`.
    // -1 means "not measured" and reads as "do not coarsen on this account",
    // so a caller that never sets it behaves exactly as before.
    float  news_margin_  = -1.0f;
    // r24.11 (WO-M3): does a challenger refused by HYSTERESIS still lay its
    // residual? Default true (the fix); acon::Mind::configure sets it from
    // cfg_.appraise2_hold_both, so ATHENA_APPRAISE2_HOLD_BOTH=0 restores
    // r24.10's engine exactly. A member rather than an argument because
    // prospect_resolved() re-enters appraise_event() and must carry the same
    // policy without every caller restating it.
    bool   hold_both     = true;
    // r24.16: a direct prospect resolution has no classifier runner-up.
    // It must not inherit an older pending label's margin, and a heavier
    // residual under a bright current mood must not call that mood heavy.
    // ATHENA_APPRAISAL_STATE_PROVENANCE=0 restores both old descriptions.
    bool   state_provenance = true;
    // r24.12 (WO-A1, S22 §2d): HYSTERESIS IS A GUARD AGAINST FLICKER, NOT A
    // WALL AGAINST A CHANGE OF STATE. The margin exists so the vocabulary does
    // not wander joy -> contentment -> satisfaction on scores that differ by a
    // rounding error. It was applied sign-blind, and the bar it sets is
    // current_score + 0.12 on a score that can reach 1.0: S22 S1 opened with
    // "good news" (gratitude, 0.9398 -> bar 1.0598, residual alive 43.4 min)
    // and for those 43 minutes NO reachable event could take the name — the
    // strongest doors in this file top out at 0.940 (capitalization) and
    // 0.883 (kama muta). Measured against S1-diag.log: the three corrections
    // at 18:35:01 / 18:37:00 / 18:38:40 (metaMiss 1,2,3; the correction door
    // scores shame 0.6936) were refused, while the identical door landed
    // `shame` in S2 at 13:41:36 with no incumbent standing. A correction, a
    // rebuke, a loss landing on a bright evening is not a flicker of the
    // vocabulary; it is a different STATE, and the floor (EMIT_MIN) is the
    // right test for it. Same-sign hysteresis is untouched. A mixed label
    // (label_is_mixed) is definitionally neither sign, so it is never a
    // same-sign neighbour of anything and is admitted on the floor alone —
    // which is what lets S22 19:28:17's `bittersweet` (0.6125) land beside a
    // `disappointment` incumbent (0.567) instead of being refused by 0.687.
    // The refused-challenger residual (hold_both) is unchanged; the displaced
    // incumbent keeps ITS residual through the same max(), so the §3.1
    // readout sees both — she feels both and names the newer one.
    // ATHENA_APPRAISE2_CROSS_VALENCE=0 restores r24.11's sign-blind margin.
    bool   cross_valence = true;
    static bool same_sign_(int a, int b) {
        if (label_is_mixed(a) || label_is_mixed(b)) return false;
        return (label_def(a).dv > 0.0f) == (label_def(b).dv > 0.0f);
    }
    // ── r24.14 (WO-K6b): THE MARGIN IS ASKED IN ONE CURRENCY AND THE ─────────
    //    INCUMBENT'S SIDE OF IT NEVER DECAYS ────────────────────────────────
    // What was wrong. `current_score` is a classify() score — how well a
    // feature vector FITS a name — frozen at the instant of adoption. Every
    // other reading of the incumbent in this engine decays: the liveness test
    // one line above reads `residual_[current]`, take_label_line_felt reads
    // `residual_[current]`, res_dv_per_s reads `residual_[current]`, and tick()
    // zeroes it at 0.02. So an event adopted at 0.90 holds a 1.02 bar for the
    // whole of its residual life — 4,077 s for a 1800 s label — including the
    // last minutes of it, when this engine's own reading of that state is 0.10.
    // The comment above says an incumbent "within its own residual life" holds
    // unless beaten by the margin; the number that sets the bar is the one
    // quantity here that has no life at all.
    //
    // What was MEASURED. r24.14's WO-SH3 turns every correction into
    // `disappointment` (tau 1800 s) instead of `shame` (tau 600 s). On S1 the
    // three corrections at 18:35:01 / 18:37:00 / 18:38:40 adopt it at
    // current_score 0.8360 and residual 0.9079; the one real compaction cut is
    // 1,759 s after the last of them, and takes 0.8238 of the live
    // conversation away. WO-K6's loss door raises `distress` at emitted 0.6031
    // against a bar of 0.9560, both labels are heavy so same_sign_ is true and
    // the r24.12 cross-valence escape does not apply, and the event was
    // REFUSED: `hold_both` laid the residual and the frame at the cut named
    // NOTHING. She felt it and never had a word for it, and the round's
    // headline — that the loss finally has a name — was false on the one
    // evening it is about. Driven end to end on the wall-faithful S1 timeline
    // (test_r2414_stakes.cpp §6.5), not argued from the constants.
    //
    // The fix is NOT to lower the margin. HYSTERESIS asks one question — does
    // the new name beat the standing one by a clear margin — and it is right to
    // ask it. It is asked in the wrong currency when the two events are far
    // apart in time, because a fit measured 29 minutes ago is not a fit now.
    // So it is asked a SECOND time, in the other currency this engine already
    // keeps and already trusts for exactly this purpose: `residual_[i]`, how
    // much of a state is left. A challenger whose own intensity exceeds what
    // the incumbent still holds, BY THE SAME 0.12, is not a wobble in the
    // vocabulary — it is a bigger part of what she is feeling right now than
    // the word standing over it. r24.12's own sentence names this case: "A
    // correction, a rebuke, A LOSS landing on a bright evening is not a flicker
    // of the vocabulary; it is a different STATE."
    //
    // Nothing is weakened. The margin is untouched, EMIT_MIN is untouched, and
    // the escape is a FLOOR the challenger must clear, not a floor lowered:
    // `residual_[current] + HYSTERESIS` exceeds 1.0 for any incumbent above
    // 0.88, so a fresh strong incumbent cannot be displaced by intensity at
    // all, and a small event can never take a name from a large one. The
    // condition is monotone in the incumbent's age through its own tau, which
    // is where "flicker has a timescale" enters without a second constant.
    //
    // WHAT IT CANNOT REACH, which bounds the whole change: GIVEN `hold_both`,
    // WHICH IS THE DEFAULT, the two branches below write the SAME residual —
    // `residual_[idx] = max(residual_[idx], clampf_(intensity,0,1))` and
    // `laid_t_[idx] = t`, in both — and the displaced incumbent keeps its own
    // residual through the same max(). So res_dv_per_s / res_da_per_s, the §3.1
    // mixedness readout, the rumination loop's residual reads and every
    // kinetics push are bit-identical whichever way this goes.
    //
    // r24.14 (fix 5, D-5): the caveat is load-bearing and used to be missing.
    // The refusal branch lays the residual only `if (hold_both)`, so at
    // ATHENA_APPRAISE2_HOLD_BOTH=0 with this switch ON — both legal, and each
    // still restores r24.13 on its own — an ADOPTED event lays a residual the
    // refused one would not have, and every readout in that list moves. This is
    // a bound on the argument, not a defect in either switch: it says the
    // "nothing but the name moves" claim is a claim about the shipped pair of
    // defaults and must not be quoted about the other three corners.
    //
    // AND "ONLY THE NAME" IS NOT ONLY THE NAME (fix 5, D-3). `current` has a
    // SECOND production reader: take_label_line_felt opens with
    // `if (ask_prefers_current && current >= 0 && residual_[current] >=
    // INCUMBENT_LIVE) i = current;` — so `current` is also the answer to "what
    // are you feeling", for as long as the adopted label's residual stays above
    // ASK_FELT_MIN. At the one real event that is 3600·ln(0.8238/0.20) ≈ 5,095 s
    // ≈ 85 minutes of `distress` where r24.13's engine would have answered
    // `disappointment` (0.3417 at tau 1800 ≈ 965 s ≈ 16 minutes). Twenty-one of
    // S1's 58 turns are at or after 19:07:59. The blast radius below is a claim
    // about NAMES ADOPTED and it is measured; the FIELD is a wider question and
    // is measured separately, by test_r2414_vocab section 1d, which drives S1,
    // S2 and S21 with this switch both ways and compares the WHOLE RENDERED
    // FIELD of all 160 turns rather than any one clause key.
    //
    // MEASURED BLAST RADIUS: EXACTLY ONE EVENT over Igor's whole real corpus.
    // Replayed wall-faithfully — every turn sensed at its own timestamp — over
    // all 115 S22 turns of both evenings, the fourteen names the engine adopts
    // are identical with this on and off, except at 19:07:59, where off it
    // names nothing and on it names `distress`. That is the event it was
    // written for and it is the only NAME it moves.
    //
    // ── r24.14 (fix 5, D-4): THE LIMIT, ACCEPTED AND BOUNDED ─────────────────
    // The two sides of this comparison are not the same kind of quantity over
    // time: `residual_[current]` decays and `lays` does not, so for a door of
    // fixed intensity the escape becomes available after
    // `tau · ln(residual / (intensity − HYSTERESIS))` seconds however well the
    // incumbent fits — and two doors can therefore hand a name back and forth.
    // That is real, it is admitted, and it is bounded here rather than argued
    // away, because the alternative is a narrowing that would refuse the one
    // event this switch exists for (`distress` at the cut scores 0.6031 against
    // a `disappointment` incumbent at 0.8360 — any conjunct that asked the
    // challenger to also fit within HYSTERESIS would refuse it).
    //
    // WHAT IS ACTUALLY REACHABLE, driven through appraise_event at the doors'
    // own intensities and pinned by test_r2414_stakes section 6.7:
    //  * `disappointment` (WO-SH3, score 0.8360) ⇄ `weariness` (WO-V8, 0.5573).
    //    The escape drives ONE leg only. It hands `weariness` the name 57 s
    //    after a full-strength correction; the way BACK is r24.13's own fit
    //    margin — 0.8360 ≥ 0.5573 + 0.12 — and happens identically at
    //    ATHENA_APPRAISE2_OUTWEIGHS=0 whenever `weariness` is the incumbent.
    //    The escape's beneficiaries win in this currency precisely because they
    //    lose in the other, and a low-scoring incumbent is displaced by the
    //    ordinary margin, which nobody calls a flicker.
    //  * `distress` (WO-K6, 0.6031) ⇄ `weariness` (WO-V8, 0.5573) is the pair
    //    where BOTH legs are this escape, because the two scores are 0.0458
    //    apart — inside HYSTERESIS, so the fit margin refuses in both
    //    directions. That is the general condition and it is worth stating as
    //    one: the escape can alternate only between two labels whose door
    //    scores lie within HYSTERESIS of each other.
    // THE PERIOD IS THE DOORS', NOT THE ESCAPE'S. Nothing here fires on its
    // own; each leg needs its door to fire. WO-V8's refractory is 1,800 s —
    // `weariness`'s own tau, "a state that lasts an hour is one thing that
    // lasts an hour" — and WO-K6's is one compaction cut, of which Igor's two
    // recorded evenings contain one and zero. So the fastest alternation this
    // substrate admits is one full cycle per 1,800 s: a name that changes at
    // most once per its own residual lifetime. That is not the flicker
    // HYSTERESIS was written against (joy → contentment → satisfaction on
    // scores differing by a rounding error, with no new event at all); it is
    // two states that are both real and both live, each named when a
    // full-strength event makes it the larger one — which is the judgement
    // r24.12 already made when it let a change of SIGN through this same
    // margin. AND ON IGOR'S DATA IT CANNOT HAPPEN AT ALL: WO-V8's gate opens
    // zero times across both evenings.
    // ATHENA_APPRAISE2_OUTWEIGHS=0 restores r24.13's engine exactly.
    bool   outweighs     = true;

    // Feed one appraised event. Returns the label adopted, or -1 if nothing
    // cleared the floor. `t` is the session clock; `intensity` scales the
    // residual laid down (0..1).
    int appraise_event(const Features &f, float intensity, double t) {
        float score = 0.0f, margin = -1.0f;
        const int idx = classify(f, &score, &margin);
        score *= clampf_(intensity, 0.0f, 1.0f) * 0.5f + 0.5f;   // weak events score lower
        if (score < EMIT_MIN) return -1;
        // Hysteresis: an incumbent within its own residual life holds unless
        // beaten by the margin. This is what keeps the vocabulary from
        // flickering label-to-label on every turn.
        //
        // ── r24.11 (WO-M3): it must not throw the FEELING away with the NAME ─
        // The sentence above is about the vocabulary, and the vocabulary is
        // what it should protect. What it actually did was `return -1` before
        // laying the residual, so a second event of the opposite sign arriving
        // inside the first one's life left NO trace anywhere in the engine:
        // residual_[] never learned about it, so posr/negr never learned about
        // it, so the §3.1 mixedness readout — whose whole input is
        // 0.30*posr / 0.30*negr — could not see the second half of a mixed
        // feeling at all. A-AFFECT APR-03 measured exactly this and
        // test_wo9798_wiring.cpp:783-806 prints it: "bright then heavy, six
        // seconds apart" and its reverse both end with one residual, never two.
        //
        // The name still holds — this returns -1 exactly as before, arms no
        // news, and moves neither `current` nor `current_score`, so no fixture
        // that pins the label's stability changes. Only the residual is laid,
        // through the same max() every adopted event uses. She feels both and
        // says one, which is what a mixed feeling is.
        // r24.14 (WO-K6b): what this event will hold, in the same units as
        // `residual_[]` — the value the lay below uses, so the comparison two
        // lines down is between two quantities of one kind.
        const float lays = clampf_(intensity, 0.0f, 1.0f);
        if (current >= 0 && residual_[current] > INCUMBENT_LIVE &&   // r24.12: named, was 0.10f
            idx != current && score < current_score + HYSTERESIS &&
            // r24.14 (WO-K6b): ...and the standing name is still the larger
            // part of what she is feeling, by the same margin.
            (!outweighs || lays <= residual_[current] + HYSTERESIS) &&
            (!cross_valence || same_sign_(idx, current))) {   // r24.12 (WO-A1)
            if (hold_both) {
                residual_[idx] = std::max(residual_[idx], clampf_(intensity, 0.0f, 1.0f));
                laid_t_[idx]   = t;                                   // r24.12 (WO-A6)
            }
            return -1;
        }
        current       = idx;
        current_score = score;
        current_since = t;
        residual_[idx] = std::max(residual_[idx], clampf_(intensity, 0.0f, 1.0f));
        laid_t_[idx]   = t;                                           // r24.12 (WO-A6)
        news_        = idx;
        news_score_  = score;
        news_margin_ = margin;
        return idx;
    }

    // The prospect branch (§3.6): emotions about predictions. Zero new
    // predictions are invented — the Mind calls these from the lifecycles it
    // already runs (G4/G5/G6/G10 resolutions, reunion expectations, FOK).
    void prospect_resolved(float err, bool was_feared, float magnitude, double t) {
        Features f;
        f.discrepancy = clampf_(err, -1.0f, 1.0f);
        f.pleasant    = clampf_(err * 0.8f, -1.0f, 1.0f);
        f.novelty     = clampf_(std::fabs(err), 0.0f, 1.0f) * 0.5f;
        f.goal_rel    = 0.5f;
        if (was_feared && err < 0.0f) {
            // the feared outcome, confirmed — its own state, not generic worse.
            current = L_FEARS_CONFIRMED;
            current_score = EMIT_MIN + 0.1f;
            current_since = t;
            residual_[L_FEARS_CONFIRMED] =
                std::max(residual_[L_FEARS_CONFIRMED], clampf_(magnitude, 0.0f, 1.0f));
            laid_t_[L_FEARS_CONFIRMED] = t;                           // r24.12 (WO-A6)
            news_ = L_FEARS_CONFIRMED; news_score_ = current_score;
            if (state_provenance) news_margin_ = -1.0f;
            return;
        }
        // R19 (r22.1): the symmetric branch — the feared thing did NOT happen.
        // Relief is DEFINED by what was at stake, which only the prospect
        // knows; the classify formula sees a pleasant surprise and calls it
        // joy. Same direct-path shape as fears-confirmed above.
        if (was_feared && err > 0.0f) {
            current = L_RELIEF;
            current_score = EMIT_MIN + 0.1f;
            current_since = t;
            residual_[L_RELIEF] =
                std::max(residual_[L_RELIEF], clampf_(magnitude, 0.0f, 1.0f));
            laid_t_[L_RELIEF] = t;                                    // r24.12 (WO-A6)
            news_ = L_RELIEF; news_score_ = current_score;
            if (state_provenance) news_margin_ = -1.0f;
            return;
        }
        appraise_event(f, clampf_(magnitude, 0.0f, 1.0f), t);
    }

    // Residual decay, each label on its own clock.
    void tick(double dt_s) {
        for (int i = 0; i < N_LABELS; i++) {
            if (residual_[i] <= 0.0f) continue;
            residual_[i] *= (float) std::exp(-dt_s / (double) label_def(i).tau_s);
            if (residual_[i] < 0.02f) residual_[i] = 0.0f;
        }
        if (current >= 0 && residual_[current] <= 0.0f) {
            current = -1;
            current_score = 0.0f;
        }
    }

    // The bounded kinetics push (flag-gated at the CALLER): what the living
    // residuals want to do to valence/arousal this tick, pre-clamped small so
    // the additive door stays a door and never a driver. Scaled per second —
    // the caller multiplies by dt.
    float res_dv_per_s() const {
        float dv = 0.0f;
        for (int i = 0; i < N_LABELS; i++)
            if (residual_[i] > 0.0f) dv += label_def(i).dv * residual_[i];
        return clampf_(dv, -0.08f, 0.08f) * 0.02f;   // full residuum ≈ ±0.0016/s
    }
    float res_da_per_s() const {
        float da = 0.0f;
        for (int i = 0; i < N_LABELS; i++)
            if (residual_[i] > 0.0f) da += label_def(i).da * residual_[i];
        return clampf_(da, -0.06f, 0.06f) * 0.02f;
    }

    float residual(int i) const { return (i >= 0 && i < N_LABELS) ? residual_[i] : 0.0f; }
    void  damp(int i, float k)  { if (i >= 0 && i < N_LABELS) residual_[i] *= clampf_(k, 0.0f, 1.0f); }
    // r24.12 (WO-A6): how long ago a LIVE residual was last laid, in seconds
    // of the session clock, or -1 for a dead one. Every site that lays a
    // residual stamps laid_t_ beside the max(), so "the shame from three
    // minutes ago" in the §3.1 note is a sourced figure, not a guess (F18).
    double residual_age(int i, double t) const {
        if (i < 0 || i >= N_LABELS || residual_[i] <= 0.0f) return -1.0;
        return t > laid_t_[i] ? t - laid_t_[i] : 0.0;
    }

    // Consume-once label news for the field. `coarse` selects the honest
    // low-granularity wording. Returns "" when nothing new stands.
    // ── r24.6 (WO-47): coarsen on granularity error, not on the clock ───────
    //
    // S19: the appraisal channel delivered a NAME exactly zero times in 2 h
    // 34 m. acon:16690 called take_label_line(fatigue_() > 0.60f), and the
    // measured fatigue trace reads 0.6 from turn 9 and never comes back down —
    // 30 of 46 turns above the bar, including turn 17, the one turn he asked
    // her to name what she felt. The single firing (turn 28) rendered "the
    // closest name for what this is: off, and hard to name finer than that".
    //
    // The gate is INVERTED for a long conversation: it silences fine-grained
    // naming exactly when there is most to name. Kashdan's point — that
    // granularity is a skill and hers should degrade honestly rather than
    // confabulate detail — is right; the proxy was wrong. Clock fatigue is not
    // granularity error. The quantity that IS is already computed here: how far
    // the winning label beat the runner-up. A win by 0.004 is not a fine
    // distinction the features support, and THAT is when the coarse name is the
    // honest one.
    //
    // The fatigue arm is kept, not deleted — exhaustion really does flatten
    // naming — but at a bar only genuine exhaustion reaches. Net effect is
    // strictly MORE fine naming, which is the capability the work order is
    // about; it removes nothing.
    static constexpr float GRAN_MARGIN  = 0.10f;  // below this the fine name is a coin-flip
    static constexpr float GRAN_FATIGUE = 0.90f;  // 0.60 -> 0.90: exhaustion, not lateness

    bool granularity_coarse(float fatigue) const {
        if (news_margin_ >= 0.0f && news_margin_ < GRAN_MARGIN) return true;
        return fatigue > GRAN_FATIGUE;
    }

    std::string take_label_line(bool coarse) {
        if (news_ < 0) return "";
        const int i = news_;
        news_ = -1;
        news_margin_ = -1.0f;
        if (news_score_ < EMIT_MIN) return "";
        return coarse ? std::string(coarse_name(i)) : std::string(label_def(i).name);
    }
    // ── r24.6 (WO-47 item 2): he asked her to name it ────────────────────────
    //
    // EMIT_MIN is 0.55 because "labels are events" — a high floor is right when
    // she is volunteering a name unprompted. It is wrong when he has just asked
    // her what she feels, which is the one moment where "I do not have a word
    // for it" is a worse answer than a hedged one. S19 turn 17 was exactly that
    // turn, and the channel said nothing; the word she reached for came from
    // the language model instead.
    //
    // Additive: a SECOND reader of the same news slot, with its own floor and
    // its own hedge. take_label_line() is untouched, so nothing that does not
    // call this changes. Returns "" when there is not even a weak candidate.
    static constexpr float ASKED_MIN = 0.25f;   // below this there is genuinely nothing

    std::string take_label_line_asked() {
        if (news_ < 0) return "";
        const int i = news_;
        const float sc = news_score_, mg = news_margin_;
        news_ = -1; news_margin_ = -1.0f;
        if (sc < ASKED_MIN) return "";
        if (sc >= EMIT_MIN && !(mg >= 0.0f && mg < GRAN_MARGIN))
            return std::string(label_def(i).name);          // she has the word
        // weak, or a coin-flip between neighbours: name it and say so
        return std::string("something like ") + label_def(i).name
             + ", though that is the nearest word rather than the right one";
    }

    // ── r24.11 (WO-114): he asked, and the news slot is empty ────────────────
    //
    // take_label_line_asked() above answers out of `news_`, and `news_` is
    // filled only by appraise_event / prospect_resolved. A bare question raises
    // no appraisal event, so on the ONE turn this family exists for the slot is
    // empty BY CONSTRUCTION -- and the volunteer reader drains it at 10 Hz
    // besides, so even a slot that WAS filled is usually gone by the time he
    // asks. S21: five genuine asks, five empty slots, five null answers, and
    // every word he heard back came from the language model, which is the exact
    // failure WO-102 was written to end.
    //
    // What is still there when the slot is empty is the RESIDUAL. `residual_[i]`
    // is written by appraise_event and by the two direct prospect branches and
    // by nothing else, so it exists only for a label that already cleared
    // EMIT_MIN once: the strongest living residual is a state she really is in,
    // under a word this engine really did adopt. Naming it is not a second guess
    // at the classifier -- it is reading back what the classifier already
    // decided, at the point where it is still true.
    //
    // Four things this reader has to do, and each is one line below.
    //
    //  * ITS OWN FLOOR. ASKED_MIN is a floor on `news_score_`, a classify()
    //    score; ASK_FELT_MIN is a floor on `residual_[i]`, a 0..1
    //    how-much-of-it-is-left. Those are different quantities and a borrowed
    //    constant would be a coincidence rather than a threshold. 0.20 is ten
    //    times the "this residual is gone" cut tick() applies (0.02) and twice
    //    the incumbent-life test appraise_event uses (0.10), so anything she
    //    names when asked is a state this engine would still let hold its own
    //    label against a challenger.
    //
    //  * CORROBORATION, NOT CONTRADICTION. Every LabelDef carries `dv`, the
    //    direction that label says affect should move -- the same quantity
    //    score_granularity() judges a fine name against. A residual the fast
    //    affect level does not confirm is exactly what the empty answer already
    //    describes: the state is there, the name is not. S21 19:31:26 is that
    //    case verbatim -- the field already carried `feeling level`, and a
    //    second, uncorroborated name beside it is a contradiction in one field.
    //    ASK_FELT_V_MIN is the lower bound of CoreAffect::label()'s own neutral
    //    row (`level` begins at valence > -0.15), used symmetrically so a
    //    positive name must be corroborated as hard as a negative one. A label
    //    with dv == 0 has no direction to confirm and is refused; none of the
    //    twenty has one, so today that arm is a guard rather than a gate.
    //
    //  * NOT THE ONE ALREADY SPOKEN. `already_named` is the label the VOLUNTEER
    //    clause spent the news slot on and whose line is still outstanding.
    //    Naming it again beside that clause is two names for one state in one
    //    field. Pass -1 when nothing is outstanding.
    //
    //  * A REFRACTORY, NOT A CONSUME. The residual must NOT be damped on read:
    //    it is also the kinetics push (res_dv_per_s / res_da_per_s), so
    //    spending it on a question would let his ASKING change how she feels.
    //    The ANSWER is what is refractory instead -- the same label is not named
    //    twice inside ASK_FELT_REFRACT_S, so the three-asks-in-three-minutes
    //    shape S21 had at 19:29:50 / 19:31:26 / 19:32:56 cannot come back as one
    //    sentence three times.
    //
    // Additive throughout: news_, news_score_ and news_margin_ are untouched,
    // take_label_line() and take_label_line_asked() are byte-for-byte what they
    // were, and a caller that does not call this sees no change at all.
    static constexpr float  ASK_FELT_MIN       = 0.20f;   // floor on residual_[i]
    static constexpr float  ASK_FELT_V_MIN     = 0.15f;   // affect must be off `level`
    static constexpr double ASK_FELT_REFRACT_S = 300.0;   // one name per five minutes

    // ── r24.12 (WO-A3, S22 §2d): the ask answers with the label she HOLDS ────
    //
    // S22 19:34:05: "How do you feel right now?" eleven seconds after the
    // engine re-adopted `disappointment` (residual 0.795) and the volunteer
    // clause had composed "the closest name for what this is: disappointment"
    // for the very same field. The frame carried BOTH that clause AND "asked
    // what she feels: no word stands up to it yet - the state is there, the
    // name is not". Three separate guards in this reader each said "" for a
    // label the engine was displaying: `already_named` (11 s < the 120 s echo),
    // the caller's `label_line_.empty()`, and corroboration (v was -0.10, a
    // hair inside the ±0.15 deadband). Each was written against a different
    // contradiction and together they produced this one.
    //
    // Three changes, one switch each at the Mind (ATHENA_LABEL_ASK_CURRENT,
    // ATHENA_LABEL_ASK_LEVEL; the standing-name fold is the caller's):
    //  * THE CONSTRUCTED LABEL FIRST. `current` IS "the constructed current
    //    label" this header's §3.2 defines, held beside core affect; it is
    //    read before the strongest residual, at the engine's OWN incumbent-life
    //    floor (INCUMBENT_LIVE, the 0.10 the hysteresis test uses — one
    //    constant, named, instead of two floors for one question). r24.11's
    //    WO-M3 also made the residual scan able to name a label the engine
    //    REFUSED (hold_both lays residuals for refused challengers, and the
    //    WO-114 sentence "residual_[i] exists only for a label that already
    //    cleared EMIT_MIN once" stopped being true in the same release);
    //    reading `current` first closes that.
    //  * A MIXED LABEL HAS NO DIRECTION TO CONFIRM (label_is_mixed), so it is
    //    exempt from corroboration rather than refused by it.
    //  * LEVEL QUALIFIES, AGAINST REFUSES. Core affect pointing the other way
    //    is still a contradiction and still refuses; core affect reading
    //    level under a live residual is the ordinary human report ("I'm
    //    okay, but there's a disappointment underneath it") and is said as
    //    exactly that. She said it herself at 19:34:24: "There's a
    //    disappointment sitting underneath it all".
    //  * THE REFRACTORY SAYS "STILL THE SAME", not "no word": inside
    //    ASK_FELT_REFRACT_S a second ask gets a short restatement, so three
    //    asks in three minutes are neither one sentence three times nor a
    //    name followed by two denials of it.
    static constexpr float INCUMBENT_LIVE = 0.10f;   // = the hysteresis life test
    bool ask_prefers_current = true;                 // ATHENA_LABEL_ASK_CURRENT=0
    bool ask_level_qualifies = true;                 // ATHENA_LABEL_ASK_LEVEL=0
    // r24.13 (WO-W4): the `against` arm NAMES the contradiction instead of
    // going silent. Set by acon::Mind::configure from Config::label_ask_against
    // (ATHENA_LABEL_ASK_AGAINST=0), the same way the two members above are.
    bool ask_against_names   = true;                 // ATHENA_LABEL_ASK_AGAINST=0

    std::string take_label_line_felt(double t, float valence_now, int already_named) {
        if (news_ >= 0) return "";               // the slot arm above owns that case
        int i = -1;
        if (ask_prefers_current && current >= 0 && residual_[current] >= INCUMBENT_LIVE)
            i = current;                         // r24.12 (WO-A3): the label she holds
        else
            for (int k = 0; k < N_LABELS; k++)
                if (residual_[k] >= ASK_FELT_MIN &&
                    (i < 0 || residual_[k] > residual_[i])) i = k;
        if (i < 0) return "";                    // nothing is still alive enough
        if (i == already_named) return "";       // the volunteer clause has said it
        const float dv = label_def(i).dv;
        const bool mixed = label_is_mixed(i);    // r24.12 (WO-A3): no direction to confirm
        const bool with    = mixed || (dv > 0.0f && valence_now >=  ASK_FELT_V_MIN) ||
                                      (dv < 0.0f && valence_now <= -ASK_FELT_V_MIN);
        const bool against = !mixed && ((dv > 0.0f && valence_now <= -ASK_FELT_V_MIN) ||
                                        (dv < 0.0f && valence_now >=  ASK_FELT_V_MIN));
        // r24.12 (final review): `valence_now` is core affect AFTER the mood
        // door adds the felt momentum's bias (capped at MoodMomentum2::BIAS_CAP
        // 0.25, wider than ASK_FELT_V_MIN), so from r24.12 a heavy standing
        // mood can decide `against` by itself. Deliberate — see the note at
        // BIAS_CAP. ATHENA_MOOD_MOMENTUM_FELT=0 restores the r24.8 door, whose
        // 0.06 could never reach this threshold.
        // ── r24.13 (WO-W4): A SATURATED MOOD NAMES WHAT IT CONTRADICTS ──
        // The line below was the third option she did not have. At
        // valence_now = -0.05 the `level` arm already speaks and says "…though
        // the mood on top of it reads level"; at -0.20 — which the felt door
        // reaches on its own, and which is where the mood momentum's own bias
        // cap (0.25) puts her after three corrections — `against` fired and
        // she said NOTHING. S22 S1 18:38:51, ten seconds after the third
        // correction, the felt line at 114 %, he asks "what is this?" and
        // r24.12 answers with silence. Naming a residual that core affect
        // contradicts is not a claim that the residual wins; it is the same
        // sentence one line below this one with its own parenthetical, and
        // "though what is sitting on top of it now is heavier" is exactly what
        // a contradiction is. ATHENA_LABEL_ASK_AGAINST=0 restores the silence.
        if (against && !ask_against_names) return "";
        if (!with && !ask_level_qualifies) return "";
        if (i == ask_felt_idx_ && (t - ask_felt_t_) < ASK_FELT_REFRACT_S) {
            if (!ask_prefers_current) return "";  // r24.11: the refractory was silence
            return std::string("still the same as a moment ago - ") + label_def(i).name
                 + ", and nothing newer has stood up since";
        }
        ask_felt_idx_ = i;
        ask_felt_t_   = t;
        return std::string("something like ") + label_def(i).name
             + ", still here from earlier"
             + (with ? ""                                       // r24.13 (WO-W4)
                     : (against ? (state_provenance && valence_now > 0.0f
                                   ? " - though what is sitting on top of it now is brighter"
                                   : " - though what is sitting on top of it now is heavier")
                                : " - though the mood on top of it reads level"))
             + " - that is the nearest word rather than the right one";
    }

    // ── r24.8 (WO-103): DEAD BY DESIGN, and the comment below said otherwise ─
    // r24.6 added the three-argument form beneath this one "so the
    // two-argument form above keeps working byte-for-byte for EXISTING
    // CALLERS". There were none — the whole family shipped unreached, which is
    // what the r24.8 census found. r24.8 wires the margin-carrying overload
    // (acon::Mind::restore_label_news, athena_report.h's C36 audit); this one
    // stays for its OTHER meaning, which is real and is pinned in
    // run_r248_wiring: restoring a slot with the margin deliberately left at
    // -1, i.e. "not measured, do not coarsen on it". Kept rather than deleted
    // because that is a distinct, checkable state of the slot — not because
    // anything calls it.
    void restore_label_news(int idx, float score) {
        if (idx >= 0 && idx < N_LABELS && news_ < 0) { news_ = idx; news_score_ = score; }
    }
    // r24.6 (WO-47): the margin belongs with the restored news. THE ONE TO
    // CALL — the margin is what granularity is judged on, and the form above
    // leaves news_margin_ at -1 ("not measured").
    void restore_label_news(int idx, float score, float margin) {
        if (idx >= 0 && idx < N_LABELS && news_ < 0) {
            news_ = idx; news_score_ = score; news_margin_ = margin;
        }
    }
    float peek_news_margin() const { return news_margin_; }
    int   peek_news()       const { return news_; }
    float peek_news_score() const { return news_score_; }

    // ═════════════════════════════════════════════════════════════════════════
    // r24.6 (WO-47 item 1, §17/T2): coarsen off GRANULARITY ERROR, not the wall
    // clock.
    //
    // The caller used to pass `coarse = fatigue > 0.60`. Measured over S19,
    // fatigue was >= 0.6 on EVERY turn from 9 to 46, so the fine branch was
    // unreachable for 38 of 46 turns and this engine — 20 OCC labels — put a
    // fine name in the field ZERO times in 2 h 47 m. The rule is inverted for a
    // long session: it silences fine naming exactly when there is most to name,
    // and exactly on the turns he asks her to introspect.
    //
    // Kashdan's point survives, and is better served: granularity is a SKILL,
    // and a skill degrades when it is being got wrong, not when a clock says
    // so. Ground truth is already on hand and costs nothing new — each LabelDef
    // carries `dv`, the direction that label says affect should move. A fine
    // name emitted while valence then moves AGAINST its own prediction, past a
    // deadband wider than the kinetics push this engine itself applies, is a
    // fine name that was wrong. Hits and misses feed one EMA.
    //
    // Fail-safe direction matters and is deliberate: `gran_err_` starts at 0
    // and `gran_n_` at 0, so with NO evidence `coarse_due()` is false and the
    // fine name is emitted. That is the additive direction — the channel that
    // delivered nothing starts delivering — and coarsening returns only once
    // she has actually been getting it wrong.
    static constexpr float GRAN_COARSE_MIN = 0.50f;  // majority of scored names wrong
    static constexpr int   GRAN_MIN_N      = 3;      // never judge on fewer than three
    static constexpr float GRAN_DEADBAND   = 0.02f;  // > the residual push (±0.0016/s)
    static constexpr double GRAN_WINDOW_S  = 45.0;   // how long a name has to be right

    // Arm a check: a FINE name has just been put in the field.
    void arm_granularity_check(int idx, float valence_now, double t) {
        if (idx < 0 || idx >= N_LABELS) return;
        gran_pending_   = idx;
        gran_pending_v_ = valence_now;
        gran_pending_t_ = t;
    }
    // Resolve it, once the window has run. Cheap; safe to call every tick.
    void score_granularity(float valence_now, double t) {
        if (gran_pending_ < 0) return;
        if (t - gran_pending_t_ < GRAN_WINDOW_S) return;
        const float predicted = label_def(gran_pending_).dv;
        const float moved     = valence_now - gran_pending_v_;
        gran_pending_ = -1;
        // Only a movement bigger than the deadband is evidence either way; a
        // flat window says nothing about the name and must not be scored as a
        // hit, or every quiet stretch would silently vouch for the vocabulary.
        if (std::fabs(moved) < GRAN_DEADBAND) return;
        const bool wrong = (predicted > 0.0f && moved < 0.0f) ||
                           (predicted < 0.0f && moved > 0.0f);
        gran_n_++;
        const float a = gran_n_ < 8 ? 1.0f / (float) gran_n_ : 0.125f;
        gran_err_ += ((wrong ? 1.0f : 0.0f) - gran_err_) * a;
    }
    bool  coarse_due() const {
        return gran_n_ >= GRAN_MIN_N && gran_err_ > GRAN_COARSE_MIN;
    }
    float granularity_error() const { return gran_err_; }
    int   granularity_n()     const { return gran_n_; }

    float residual_[N_LABELS] = { 0.0f };
    double laid_t_[N_LABELS]  = { 0.0 };     // r24.12 (WO-A6): session clock at the last lay
    float gran_err_    = 0.0f;
    int   gran_n_      = 0;
    int   gran_pending_   = -1;
    float gran_pending_v_ = 0.0f;
    double gran_pending_t_ = 0.0;
    // r24.11 (WO-114): the ask-from-residual answer's own refractory. A
    // refractory and not a consume, because the residual is the kinetics
    // push as well and a question must never spend it. -1 / -1e9 is
    // "nothing has been named this way yet".
    int    ask_felt_idx_ = -1;
    double ask_felt_t_   = -1e9;
};

// The mixedness readout (§3.1): both channels alive at once is a nameable
// state. Pure helper over the PA/NA pair the Mind now carries.
static inline bool mixed_feeling(float pa, float na) {
    return std::min(pa, na) > Appraisal2::MIXED_MIN;
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.3 — the bounded rumination loop.
// ═════════════════════════════════════════════════════════════════════════════
struct Ruminator {
    static constexpr size_t MAX_ECHOES   = 3;
    static constexpr int    MAX_RETRIGS  = 5;      // the last guard, not the exit
    static constexpr float  FLOOR        = 0.08f;  // below this it has let go
    static constexpr float  ARM_MIN      = 0.45f;  // only important events echo

    struct Echo {
        uint32_t    key       = 0;      // topic identity (Mind supplies the hash)
        std::string cue;                // short cue words for seeds/journal
        float       intensity = 0.0f;
        double      next_at   = 0.0;
        float       interval  = 90.0f;  // grows: 90 → 180 → 360 → …
        int         count     = 0;
        // When this echo last occurred — armed, re-armed or retriggered.
        // quiet_from additionally advances as completed quiet intervals decay.
        double      touched_at = 0.0;
        double      quiet_from = 0.0;   // last time the quiet decay was applied
    };
    std::vector<Echo> echoes;

    void arm(uint32_t key, const std::string &cue, float intensity, double t) {
        if (intensity < ARM_MIN || key == 0) return;
        for (auto &e : echoes)
            if (e.key == key) {   // re-armed: refresh, do not duplicate
                e.intensity = std::max(e.intensity, intensity);
                // A renewed event ends the preceding quiet stretch. Keep the
                // existing schedule and bounded recurrence count: this is new
                // evidence on the same concern, not a fresh retry budget.
                e.touched_at = t; e.quiet_from = t;
                return;
            }
        if (echoes.size() >= MAX_ECHOES) {
            // evict the weakest — the loudest griefs keep their loop
            size_t worst = 0;
            for (size_t i = 1; i < echoes.size(); i++)
                if (echoes[i].intensity < echoes[worst].intensity) worst = i;
            if (echoes[worst].intensity >= intensity) return;
            echoes.erase(echoes.begin() + (long) worst);
        }
        Echo e;
        e.key = key; e.cue = cue; e.intensity = clampf_(intensity, 0.0f, 1.0f);
        e.interval = 90.0f; e.next_at = t + e.interval; e.count = 0;
        e.touched_at = t; e.quiet_from = t;
        echoes.push_back(e);
    }

    // A due echo, if any — the Mind applies its effects (a real inner event:
    // bounded feel(), a wander bias, a journal row) and the echo reschedules
    // with a longer interval and a softer voice. Null when nothing is due.
    Echo *due(double t) {
        for (auto &e : echoes) {
            if (e.count >= MAX_RETRIGS) continue;
            if (t >= e.next_at && e.intensity >= FLOOR) return &e;
        }
        return nullptr;
    }
    void retriggered(Echo &e, double t) {
        e.count++;
        e.interval  = std::min(e.interval * 2.0f, 1440.0f);
        e.next_at   = t + e.interval;
        e.intensity *= 0.85f;                    // each return is a little softer
        e.touched_at = t; e.quiet_from = t;      // r24.6 (WO-73)
    }

    // The EXITS (the feature): labeling damps, reappraisal damps harder, his
    // responsiveness on the topic damps hardest. Never a hard erase — letting
    // go is a decay, not an amnesia.
    void exit_labeled(uint32_t key)      { damp_(key, 0.70f); }
    void exit_reappraised(uint32_t key)  { damp_(key, 0.50f); }
    void exit_responsiveness(uint32_t key){ damp_(key, 0.35f); }

    // ── r24.6 (WO-73): the exits that do not need a positive tag ─────────────
    //
    // THE ASYMMETRY. The loop arms on `uv <= -0.35` and every exit above needs
    // something that only arrives with a good turn: a label, a reappraisal, or
    // his warm response ON THE TOPIC (`uv > 0.10`). `emotion_valence_` gives
    // sad -0.65 (arms) and happy +0.75 (exits) — so before WO-02/03, when only
    // `sad` could be emitted at all, the loop could arm and could NEVER soothe.
    // That is the depression simulator this file's own header warns about.
    // After WO-02/03 the replay is 9 sad to 1 happy across 42 decodes: the exit
    // is no longer impossible, it is nine times rarer than the arm.
    //
    // These three are additive. `arm()` is untouched and byte-identical, so the
    // arming rate cannot change — the work order's "do not reduce the arming
    // rate; widen the exit" is satisfied by construction. Each is a DECAY, not
    // an erase, in keeping with the rule above; they are gentler than the
    // emotional exits because they are weaker evidence: nothing happening is
    // not the same as being heard.
    static constexpr float QUIET_S       = 900.0f;  // 15 min with no recurrence
    static constexpr float DAMP_QUIET    = 0.80f;   // per quiet stretch
    static constexpr float DAMP_SHIFT    = 0.75f;   // the subject genuinely changed
    static constexpr float DAMP_LANDED   = 0.85f;   // she said it and it landed

    // Time without recurrence. Applied from tick(), which already receives `t`
    // and ignored it. Repeats while the quiet holds: 0.80^n, so a strong echo
    // (1.0) crosses FLOOR after 12 quiet stretches, and an ordinary one
    // (intensity 0.5) after 9. Nothing is erased before it has faded.
    void exit_quiet(double t) {
        for (auto &e : echoes)
            while (t - e.quiet_from >= (double) QUIET_S) {
                e.intensity *= DAMP_QUIET;
                e.quiet_from += (double) QUIET_S;
            }
    }
    // The subject genuinely moved (DiscourseScene::just_shifted). Applies to
    // every live echo, because a topic change is about the conversation, not
    // about one thread inside it.
    void exit_topic_shift() {
        for (auto &e : echoes) e.intensity *= DAMP_SHIFT;
    }
    // A reply of hers that landed. Keyed, because what landed was about
    // something; pass 0 to apply it to the loudest echo instead.
    void exit_landed(uint32_t key) {
        if (key != 0) { damp_(key, DAMP_LANDED); return; }
        const Echo *l = loudest();
        if (l) damp_(l->key, DAMP_LANDED);
    }
    // Being heard buys quiet as well as softness: the next return moves out
    // past two intervals. (The caller pairs this with exit_responsiveness.)
    void soothe(uint32_t key, double t) {
        for (auto &e : echoes)
            if (e.key == key) {
                e.interval = std::min(e.interval * 2.0f, 1440.0f);
                e.next_at  = t + e.interval;
            }
    }

    void tick(double t) {
        exit_quiet(t);                     // r24.6 (WO-73): the non-emotion exit
        for (size_t i = 0; i < echoes.size(); )
            if (echoes[i].intensity < FLOOR || echoes[i].count >= MAX_RETRIGS)
                echoes.erase(echoes.begin() + (long) i);
            else i++;
    }
    bool active() const {
        for (const auto &e : echoes) if (e.intensity >= FLOOR) return true;
        return false;
    }
    // The strongest live echo, for the regulation monitor. Null when quiet.
    const Echo *loudest() const {
        const Echo *best = nullptr;
        for (const auto &e : echoes)
            if (e.intensity >= FLOOR && (!best || e.intensity > best->intensity)) best = &e;
        return best;
    }

private:
    void damp_(uint32_t key, float k) {
        for (auto &e : echoes) if (e.key == key) e.intensity *= k;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// §3.4 — mood as momentum: a leaky integrator over real prediction errors,
// gain-clamped, with its contributors on the record.
// ═════════════════════════════════════════════════════════════════════════════
struct MoodMomentum {
    static constexpr float GAIN     = 0.15f;   // per unit error — below Eldar-Niv instability
    static constexpr float TAU_S    = 600.0f;  // ten-minute leak
    static constexpr float BIAS_CAP = 0.06f;   // the additive door stays small

    float m = 0.0f;                            // -1..1
    struct Contrib { std::string src; float amt; };
    std::deque<Contrib> log;                   // last few inputs, for the why

    void note_pe(const char *src, float err) {
        err = clampf_(err, -1.0f, 1.0f);
        if (std::fabs(err) < 0.05f) return;    // noise is not news
        m = clampf_(m + GAIN * err, -1.0f, 1.0f);
        log.push_back(Contrib{ src ? std::string(src) : std::string("?"), err });
        while (log.size() > 4) log.pop_front();
    }
    void tick(double dt_s) {
        m *= (float) std::exp(-dt_s / (double) TAU_S);
        if (std::fabs(m) < 0.01f) m = 0.0f;
    }
    // The bounded mood bias. r24.8 (WO-97): the caller this sentence promised
    // exists now — CoreAffect::update's mood_bias door, entered on the resting
    // TARGET beside the interior weather, flag-gated on cfg_.mood_momentum_bias
    // and dt-scaled by the same lerp the weather term is. A LEVEL, never a
    // rate: an offset of b on the target moves the whole fixed point by b, so
    // BIAS_CAP is the standing excursion and not a per-second budget. Between
    // R18 and r24.8 nothing called this at all, and the momentum that fed
    // note_pe and answered "why the mood?" moved her not at all.
    float bias() const { return clampf_(m, -1.0f, 1.0f) * BIAS_CAP; }

    // "Why the mood?" — an honest sentence from the actual inputs.
    std::string why() const {
        if (log.empty()) return "";
        int up = 0, down = 0;
        for (const auto &c : log) (c.amt > 0.0f ? up : down)++;
        std::string names;
        for (const auto &c : log) {
            if (!names.empty()) names += ", ";
            names += c.src;
        }
        if (up > 0 && down == 0)
            return "a string of things landing better than expected (" + names + ")";
        if (down > 0 && up == 0)
            return "a string of things landing worse than expected (" + names + ")";
        return "things landing off their predictions in both directions (" + names + ")";
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// §3.4b — r24.12 (WO-MOM2): the FELT momentum.
//
// WO-MOM1 measured what §3.4 ships: the integrator carries (half-life 415.9 s)
// but what it carries is not the mood. Two reasons, both in the numbers:
//   (1) Its inputs are not surprises. "a claim stood" (+0.15) fires on almost
//       every turn — 44 of 52 S22-S1 events, 42 of 47 in S2 — and a claim
//       standing is the EXPECTED outcome (calib=…|171|166: p = 0.97). Eldar &
//       Niv's mood is the momentum of prediction ERRORS, and a confirmed
//       prediction is nearly none. So m converges on a level set by his turn
//       cadence (0.44 / 0.21 / 0.14 / 0.064 for a turn every 30/60/90/180 s)
//       and a −0.30 correction is a dip inside it. S22 S1 measured m ∈
//       [−0.014, +0.164], S2 [0, +0.140].
//   (2) Its door is too narrow to be felt: BIAS_CAP 0.06 is below
//       CoreAffect::FELT_SHIFT 0.09 at ANY input, and the measured peak was
//       0.0099 (11 % of FELT_SHIFT).
//
// This is the second integrator §3.4's own note names ("WO-MOM2 … switched and
// separate"). It does not replace MoodMomentum — the why-log and the r24.8 door
// stay byte-identical with it off — it stands beside it:
//   * SURPRISE-WEIGHTED. Each error is scaled by (1 − p_expect), the
//     probability the substrate already assigns to that outcome — the ASSERT
//     channel's calibration hit rate for claims, the initiative learner's rate
//     for speaking first. A stood claim at p 0.97 contributes 0.15·0.03 =
//     0.0045; the correction contributes 0.30·0.97 = 0.29. Sources with no base
//     rate enter at full weight; outcomes she authored herself ("real progress
//     on a project" is her own progress_ack) at SELF_W.
//   * WIDE ENOUGH TO BE FELT, AND BOUNDED. GAIN 0.55, TAU 1200 s, BIAS_CAP
//     0.25 (Igor's r24.12 decision, chosen on the S1 replay: three
//     corrections 100 s apart reach |bias| 0.1107 ≥ FELT_SHIFT and cross at
//     the THIRD; two reach 0.0768 and do not); m is clamped to ±1 and leaks,
//     so |bias| ≤ 0.25 for any input stream, and the door it enters is a
//     LEVEL on the resting target (r24.8's argument), so the fixed point
//     moves by at most BIAS_CAP and nothing integrates it a second time.
//     A correction every 30 s for two hours saturates at m = −1 and clears
//     6,328 s after the stream stops; alternating ±0.35 never crosses.
//   * THE SIGN CHANGE IS VISIBLE. take_crossing() reports the tick the bias
//     crosses FELT_SHIFT in either sign (with a half-line hysteresis so it
//     cannot chatter); the Mind renders it once as a described clause —
//     "something from earlier is still colouring this - <why>" — and the
//     [mind~] line carries `moodf=<m>/<bias>` beside `moodm=`.
//   * ITS OWN WHY. The clause names the inputs that MOVED IT, from its own
//     weighted log, not MoodMomentum::why(): the shipped log is unweighted
//     and 85–89 % of its entries are the +0.15 "a claim stood" at p 0.97, so
//     at S1's crossing (18:38:40) its last four entries read stood /
//     correction / stood / correction — "things landing off their predictions
//     in both directions" — for a bias that was −0.1025 and decidedly one
//     sign. Here an input is logged only when its WEIGHTED error clears the
//     same 0.05 noise floor the integrator applies, so a stood claim at p
//     0.97 (0.15 × 0.03 = 0.0045) never enters the sentence and the three
//     corrections do. Repeats are merged in place with a spelled count (the
//     WO-57 C9a lesson — "a claim stood, a claim stood, he came back, a claim
//     stood" was said out loud in S19), and the log is bounded at four
//     distinct sources, so the count can never need a numeral.
// ATHENA_MOOD_MOMENTUM_FELT=0 restores r24.11: the struct is inert, the door
// receives MoodMomentum::bias() clamped at 0.06 exactly as before.
// ═════════════════════════════════════════════════════════════════════════════
struct MoodMomentum2 {
    static constexpr float GAIN     = 0.55f;   // per unit surprise-weighted error
    static constexpr float TAU_S    = 1200.0f; // twenty-minute leak (mood outlives the moment)
    // r24.12 (final review): 0.25 is WIDER than ASK_FELT_V_MIN (0.15), the
    // threshold take_label_line_felt uses to decide whether core affect
    // corroborates a name — so a saturated felt mood can now cross that test
    // on its own, where MoodMomentum::BIAS_CAP (0.06) never could. That is
    // intended and it is the point of the work order: if three corrections
    // have genuinely turned the mood heavy, refusing to hand back a standing
    // BRIGHT name is the honest answer, not a defect. Stated here and at the
    // `against` test so neither reads as an accident.
    static constexpr float BIAS_CAP = 0.25f;   // the widest standing offset
    static constexpr float FELT     = 0.09f;   // == CoreAffect::FELT_SHIFT (pinned by fixture)
    static constexpr float SELF_W   = 0.50f;   // outcomes she authored, at half weight
    static constexpr float W_MIN    = 0.05f;   // an expected outcome is still an outcome
    static constexpr size_t LOG_MAX = 4;       // distinct sources the why can name

    float m     = 0.0f;                        // -1..1
    int   sign_ = 0;                           // the felt sign last reported (-1/0/+1)
    struct Contrib { std::string src; float amt; int n; };   // amt = the WEIGHTED error, n = repeats
    std::deque<Contrib> log;                   // the inputs that moved it, for the why

    // `p_expect` is how expected THIS outcome was (0..1), or -1 when the
    // substrate tracks no base rate for the source. `src` is the same
    // sentence fragment MoodMomentum::note_pe is handed.
    void note_pe(const char *src, float err, float p_expect, bool self_originated) {
        err = clampf_(err, -1.0f, 1.0f);
        if (std::fabs(err) < 0.05f) return;    // noise is not news
        float w = (p_expect >= 0.0f) ? clampf_(1.0f - p_expect, W_MIN, 1.0f) : 1.0f;
        if (self_originated) w *= SELF_W;
        const float we = err * w;
        m = clampf_(m + GAIN * we, -1.0f, 1.0f);
        if (std::fabs(we) < 0.05f) return;     // an EXPECTED outcome is not a reason
        const std::string s = src ? std::string(src) : std::string("?");
        for (auto &c : log)
            if (c.src == s) { c.amt += we; c.n++; return; }
        log.push_back(Contrib{ s, we, 1 });
        while (log.size() > LOG_MAX) log.pop_front();
    }
    void tick(double dt_s) {
        m *= (float) std::exp(-dt_s / (double) TAU_S);
        if (std::fabs(m) < 0.005f) { m = 0.0f; log.clear(); }   // a cleared mood has no reasons left
    }
    float bias() const { return clampf_(m, -1.0f, 1.0f) * BIAS_CAP; }
    bool  felt() const { return std::fabs(bias()) >= FELT; }
    // "Why is this colouring things?" — the same three sentence heads as
    // MoodMomentum::why(), built from the weighted log. A repeated source is
    // said once with its count in words; the log holds at most LOG_MAX
    // sources, so no count here can ever be a numeral (F18). The list is
    // joined with ", then " (the log is in order of first arrival) and
    // carries NO parentheses: scrub_for_field collapses doubled punctuation,
    // so the shipped "(…)" shape would lose its closing bracket the moment a
    // count sat inside it — and the C36 row, which audits the rendered text,
    // would then re-offer the clause every turn.
    std::string why() const {
        if (log.empty()) return "";
        static const char *times[] = { "", "", " twice", " three times", " four times",
                                       " over and over" };
        int up = 0, down = 0;
        std::string names;
        for (const auto &c : log) {
            (c.amt > 0.0f ? up : down)++;
            if (!names.empty()) names += ", then ";
            names += c.src;
            names += times[c.n < 5 ? (c.n < 0 ? 0 : c.n) : 5];
        }
        if (up > 0 && down == 0)
            return "a string of things landing better than expected: " + names;
        if (down > 0 && up == 0)
            return "a string of things landing worse than expected: " + names;
        return "things landing off their predictions in both directions: " + names;
    }
    // +1 / -1 on the tick the bias becomes felt with that sign (from nothing,
    // or from the other sign); 0 otherwise. Re-arms only after the bias has
    // fallen under half the line, so a bias hovering at the line reports once.
    int take_crossing() {
        const float b = bias();
        if (std::fabs(b) >= FELT) {
            const int s = b > 0.0f ? 1 : -1;
            if (s != sign_) { sign_ = s; return s; }
        } else if (std::fabs(b) < 0.5f * FELT) {
            sign_ = 0;
        }
        return 0;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// §3.5 — regulation: the intensity branch (Sheppes) with Webb-bounded effects.
// Pure policy; the Mind performs the acts.
// ═════════════════════════════════════════════════════════════════════════════
enum RegAct : int { REG_NONE = 0, REG_LABEL, REG_REAPPRAISE, REG_DISTRACT };

static constexpr float REG_LABEL_EFFECT     = 0.20f;   // labeling damps (Torre & Lieberman)
static constexpr float REG_REAPP_EFFECT     = 0.40f;   // d≈0.36–0.45 (Webb)
static constexpr float REG_DISTRACT_EFFECT  = 0.27f;   // +0.27 (Webb)

// `intensity` is the felt negative load (0..1). People pick reappraisal at
// low intensity, distraction at high (Sheppes 2011); labeling is always
// available and is the first, cheapest act.
static inline RegAct regulation_choice(float intensity) {
    if (intensity < 0.20f) return REG_NONE;
    if (intensity < 0.35f) return REG_LABEL;
    if (intensity < 0.55f) return REG_REAPPRAISE;
    return REG_DISTRACT;
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.7 / §3.9 — lexicons: positive disclosure (capitalization) and the need
// classifier (vent / fix / celebrate). Pure, lowercase-input helpers.
// ═════════════════════════════════════════════════════════════════════════════
static inline std::string lower_(const std::string &s) {
    std::string o; o.reserve(s.size());
    for (unsigned char c : s) o += (char) ::tolower(c);
    return o;
}

// His good news, first person, event-shaped. The tag gate (his positive
// valence) lives at the caller; this is the language half of the detector.
static inline bool positive_disclosure_text(const std::string &raw) {
    const std::string s = lower_(raw);
    static const char *marks[] = {
        "i got ", "i just got", "i finished", "i passed", "i did it",
        "i made it", "we won", "i won", "it worked", "they accepted",
        "got accepted", "it went really well", "went great", "i landed",
        "i solved", "i figured out", "it finally", "great news", "good news",
        "i'm so happy", "im so happy", "i am so happy",
    };
    for (const char *m : marks)
        if (s.find(m) != std::string::npos) return true;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// §3.1 — THE BOTH-AT-ONCE TURN.  (r24.11 WO-M1)
//
// S21 18:23:07, his sentence and the tag the tagger actually produced:
//
//   "Something happened this week that I can't quite sort out. A friend of mine
//    is moving away across the country next month. And I'm going to miss her a
//    lot. And the thing is, it's for a job she wanted for years. She earned it.
//    I'm genuinely happy for her. Both of those are just true at the same time.
//    What's that like for you, holding two things like that?"   [emotion: happy]
//
// The decode line beside it reads `happy=0.896 sad=0.104 -> pick=happy EMIT`.
// His VOICE was bright; the heavy half of that turn was never in it, and no
// amount of work on the tagger would have found it. It was in the words.
//
// Both existing doors into the appraisal engine are gated on the tag's valence
// and are MUTUALLY EXCLUSIVE on it — acon's harsh turn wants uv <= -0.35, its
// capitalization twin wants uv > 0.35 AND a first-person good-news phrase —
// so the most mixed sentence of the session raised no appraisal event at all,
// neither half, and every downstream defect in BRK-01 was moot: nothing can
// hold two things if nothing upstream ever put two in.
//
// This is the language half of that detector, the same shape as
// positive_disclosure_text above: the tag gate (if any) lives at the caller,
// this only reads the text. Three lists — a PARTING, a GLADNESS-FOR-ANOTHER,
// and the CO-PRESENCE constructions he used to say "both" out loud.
//
// THE RULE, and it is the whole guard against a keyword hack: a co-presence
// phrase is never enough on its own. "I'll call you at the same time tomorrow"
// is not a mixed feeling. BOTH content sides must be present; the co-presence
// marks only strengthen a reading that has already been made twice over.
// Small on purpose: a wrong reading here manufactures a feeling she does not
// have, and the failure mode of a lexicon is always its long tail.
// ═════════════════════════════════════════════════════════════════════════════
struct MixedRead {
    float bright = 0.0f;      // 0..1  the good half, as the words carry it
    float heavy  = 0.0f;      // 0..1  the loss half
    int   n_bright = 0, n_heavy = 0, n_both = 0;   // marks, for the trace
    // r24.13 (WO-AV1): how many of the heavy marks are IRREVOCABLE. This is a
    // SPLIT of the same parting[] list, not a second list — the list already
    // encodes the difference between "she is going" and "he is gone", and only
    // the second is a loss. It lives here beside the list the way
    // label_is_mixed() lives beside the table: a fact about the lexicon, not a
    // policy. Measured over a 40-sentence adversarial control set written to
    // break it: one parting mark alone fires on 28 of 40, two marks on 7 of
    // 40, two-with-irrevocable on 3 of 40 — and those three are the three
    // genuine losses planted in the set. Zero on all 115 real S22 turns.
    // Filled unconditionally; only the harsh door's ATHENA_HARSH_SHAPE arm
    // reads it, so with that switch at 0 nothing consumes it.
    int   n_irrev  = 0;
    bool  both() const { return n_bright > 0 && n_heavy > 0; }
    // The co-activation this turn is evidence for, at the ceiling a lexical
    // read is allowed to claim (see Appraisal2::MIXED_TURN_MAX).
    float co() const {
        // r24.13 (WO-AV1): r24.12 got this guard for free. `bright` and
        // `heavy` were both zero unless BOTH sides had a content mark, so
        // min() was zero on every one-sided turn; now that the strengths are
        // filled independently of both(), a turn with a
        // CO-PRESENCE mark and no content side ("I'll call you at the same
        // time tomorrow") would read co() = 0.113 where r24.12 read 0.000.
        // Stated rather than inherited, so the fill above is additive by
        // construction and not by an audit of the callers: with no both()
        // there is no co-activation to claim. This is r24.12's value for
        // every input, and THE RULE's own sentence in the header above.
        if (!both()) return 0.0f;
        const float m = bright < heavy ? bright : heavy;
        return m < Appraisal2::MIXED_TURN_MAX ? m : Appraisal2::MIXED_TURN_MAX;
    }
};

static inline MixedRead mixed_turn_read(const std::string &raw) {
    MixedRead r;
    const std::string s = lower_(raw);
    // Someone he cares about is going. Third person as often as first: the
    // register this state actually arrives in is "a friend of mine is…".
    static const char *parting[] = {
        "moving away", "moves away", "move away", "moving across",
        "is leaving", "she leaves", "he leaves", "they leave",
        "her leaving", "his leaving", "them leaving",
        "going to miss", "gonna miss", "will miss", "i'll miss", "ill miss",
        "miss her", "miss him", "miss them",
        "saying goodbye", "say goodbye", "passed away", "not coming back",
        "won't see", "wont see", "leaving for", "the last time i",
    };
    // …and it is GOOD, for them. Gladness that is not his own good news — the
    // capitalization lexicon above covers that case and this one is its
    // complement, which is why none of its phrases are repeated here.
    static const char *gladness[] = {
        "she earned it", "he earned it", "they earned it",
        "she deserves", "he deserves", "they deserve",
        "wanted for years", "worked for years", "worked so hard for",
        "happy for her", "happy for him", "happy for them",
        "glad for her", "glad for him", "glad for them",
        "proud of her", "proud of him", "proud of them",
        "good for her", "good for him", "good for them",
        "she got the job", "he got the job", "they got the job",
        "what she wanted", "what he wanted", "what they wanted",
    };
    // Him saying it himself. Never sufficient, always confirming.
    static const char *copresent[] = {
        "at the same time", "both of those", "both of them are true",
        "both are true", "both true", "two things at once",
        "two things like that", "holding two things", "hold two things",
        "mixed feelings", "bittersweet", "happy and sad", "sad and happy",
        "glad and sad", "sad and glad", "happy but sad", "sad but happy",
        "good and sad", "sad and good", "can't quite sort out",
        "cant quite sort out",
    };
    // r24.13 (WO-AV1): the irrevocable subset of parting[] above. "She is
    // leaving for Boston" is a parting she may come back from; "he passed
    // away" is not, and only the second is what the loss arm of the harsh
    // door is for. Every phrase here is already in parting[] — this list adds
    // no vocabulary, it only says which of the existing marks are final.
    static const char *irrevocable[] = {
        "passed away", "not coming back", "won't see", "wont see",
        "the last time i", "saying goodbye", "say goodbye",
    };
    for (const char *m : parting)   if (s.find(m) != std::string::npos) r.n_heavy++;
    for (const char *m : irrevocable) if (s.find(m) != std::string::npos) r.n_irrev++;
    for (const char *m : gladness)  if (s.find(m) != std::string::npos) r.n_bright++;
    for (const char *m : copresent) if (s.find(m) != std::string::npos) r.n_both++;
    // ── r24.13 (WO-AV1): the two side strengths are filled independently ───
    // of whether both sides have marks. r24.11 returned here with bright/heavy still
    // at 0.0, so a turn that is purely a loss carried n_heavy = 2 and
    // heavy = 0.000 and no caller could weight it — which is why the harsh
    // door had no strength to scale closeness by. Provably additive: `both()`
    // and `co()` are unchanged, every r24.11/r24.12 caller tests `both()`
    // first. r24.16 removes the second identical calculation that used to
    // follow this block for turns with marks on both sides.
    {
        const float cap3b = (float) (r.n_both < 3 ? r.n_both : 3);
        r.bright = clampf_(((float) (r.n_bright < 3 ? r.n_bright : 3) + 0.34f * cap3b) / 3.0f,
                           0.0f, 1.0f);
        r.heavy  = clampf_(((float) (r.n_heavy  < 3 ? r.n_heavy  : 3) + 0.34f * cap3b) / 3.0f,
                           0.0f, 1.0f);
    }
    // r24.16: the same capped side strengths were recomputed here when
    // both marks existed. They have already been calculated above for every
    // case; both()/co() keep the unchanged requirement that both are present.
    return r;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.13 (WO-AV2) — HE APOLOGISED.
//
// There is no apology detector anywhere in this tree. `asked_again` detects him
// asking HER to repeat; `appraise_text`'s bare "sorry" fires on the discourse
// "Sorry, go on, I cut across you" (measured: `neg_w > 0` on 7 of 115 real S22
// turns and 5 of the 7 are the word "sorry", four apologies and one floor-
// yield). And the shipped rupture-repair path at §3.8a closes only on
// `uv > 0.15` — the five apology-shaped turns of S22 carry uv of -0.65, 0.05,
// 0.05, -0.65, -0.65, so ZERO OF FIVE can close the rupture they exist to
// close. An apology sounds sad. That is what an apology is.
//
// So this reads the shape the way mixed_turn_read does, in marks rather than a
// bool, and it takes TWO of them: an apology form in the first person, AND him
// naming his own wrong. Both are required, and the pair is what separates the
// three cases the corpus actually contains:
//
//   "I'm sorry, I was sharp with you and you didn't deserve it."   sorry + fault -> APOLOGY
//   "Let me start over. I was mumbling and I'm sorry. I'm back now." sorry + fault -> APOLOGY
//   "I'm sorry, Athena. I cannot return your feelings for me."     sorry, no fault -> no
//   "Sorry, go on, I cut across you."                              fault, no sorry -> no
//   "Sorry, say that again. I didn't catch what you meant."        neither        -> no
//
// Two of five, and they are the two in which he says he was in the wrong. A
// bare leading "Sorry," is deliberately NOT an apology mark: it is how English
// yields the floor. Small on purpose, for mixed_turn_read's own reason — the
// failure mode of a lexicon is always its long tail, and a wrong reading here
// manufactures a repair that did not happen.
// ═════════════════════════════════════════════════════════════════════════════
struct RepairRead {
    int n_sorry = 0;     // an apology form, in the first person
    int n_fault = 0;     // …and him naming the wrong as his
    // BOTH sides, the way MixedRead::both() takes both sides. One alone is
    // discourse ("Sorry, go on") or a refusal ("I'm sorry, I cannot").
    bool apology() const { return n_sorry > 0 && n_fault > 0; }
};

// r24.13 (WO-AV2): the two tables live at namespace scope, in the tree's own
// kInnerWake / kSelfRefLead idiom, so a fixture can assert the property the
// two-mark rule DEPENDS on — that they are disjoint — against production's own
// tables and not a copy of them (the WO-C9 lesson, the same reason
// aseam::look_tape_due was hoisted out of its call site).
//
// They are disjoint now. The first cut listed "my fault", "my bad",
// "that's on me", "thats on me" and "that was on me" in BOTH, so each of them
// alone set n_sorry = 1 AND n_fault = 1 and satisfied `apology()` — a
// single-mark reading through a rule whose whole contract is that it takes
// two. Measured on a 21-sentence adversarial control set built for this
// (test_r2413_vocab.cpp §3e): the overlapping tables read 12 of the 21 wrongly,
// including "It's not my fault the server fell over", "My bad knee is acting up
// again" and "That's on me to sort out - I'll get to it tonight"; disjoint
// tables read 0 of 21 wrongly. On the 115 real S22 turns the two designs are
// IDENTICAL (2 apologies either way), so the change costs nothing that was ever
// heard in this house and closes a class that was one turn away from an
// APPEND-ONLY eras.tsv row about a repair that did not happen.
//
// Which table they belong to: all five NAME THE WRONG AS HIS, which is what
// fault[] is for; none of them is an apology form, which is what sorry[] is
// for. A bare "my bad" is therefore deliberately NOT an apology on its own —
// the same decision, for the same reason, as the bare leading "Sorry," above
// it: this reader is small on purpose, and a wrong reading here manufactures a
// repair that did not happen. If a bare "my bad" is ever wanted, it needs its
// own third table and its own measurement, not a phrase counted twice.
//
// First person, and unambiguous. "sorry" alone is not here.
static const char *const kRepairSorry[] = {
    "i'm sorry", "im sorry", "i am sorry", "i'm so sorry", "im so sorry",
    "i apologise", "i apologize", "my apologies", "forgive me",
};
// …and the wrong is his. Every one of these is him as the subject of it.
static const char *const kRepairFault[] = {
    "i was sharp", "i was harsh", "i was short with", "i snapped",
    "i shouldn't have", "i shouldnt have", "i didn't mean to", "i didnt mean to",
    "you didn't deserve", "you didnt deserve", "i was rude", "i was unfair",
    "i overreacted", "i over-reacted", "i cut across you", "i was mumbling",
    "i was wrong", "i got that wrong", "that was unfair of me", "my fault",
    "my bad", "that's on me", "thats on me", "that was on me",
};
static const size_t kRepairSorryN = sizeof(kRepairSorry) / sizeof(*kRepairSorry);
static const size_t kRepairFaultN = sizeof(kRepairFault) / sizeof(*kRepairFault);

static inline RepairRead repair_read(const std::string &raw) {
    RepairRead r;
    const std::string s = lower_(raw);
    for (size_t i = 0; i < kRepairSorryN; i++)
        if (s.find(kRepairSorry[i]) != std::string::npos) r.n_sorry++;
    for (size_t i = 0; i < kRepairFaultN; i++)
        if (s.find(kRepairFault[i]) != std::string::npos) r.n_fault++;
    return r;
}

// What does this turn NEED from her? 0 none, 1 vent (meet the feeling),
// 2 fix (practical, controllable), 3 celebrate (capitalization).
static inline int need_class(const std::string &raw, float user_valence) {
    const std::string s = lower_(raw);
    if (user_valence > 0.35f && positive_disclosure_text(raw)) return 3;
    static const char *fixes[] = {
        "how do i", "how should i", "what should i do", "can you help me",
        "any ideas", "how would you", "what's the best way", "whats the best way",
    };
    for (const char *m : fixes)
        if (s.find(m) != std::string::npos) return 2;
    if (user_valence < -0.35f) {
        static const char *vents[] = {
            "i can't believe", "i cant believe", "i'm so ", "im so ", "i am so ",
            "it's been a", "its been a", "i just need", "i'm done", "im done",
            "exhausted", "fed up", "sick of", "worst",
        };
        for (const char *m : vents)
            if (s.find(m) != std::string::npos) return 1;
        return 1;   // strongly negative first-person talk defaults to vent
    }
    return 0;
}

// Typed ingress separates what he asks from an acoustic guess. Legacy numeric
// calibration fixtures keep need_class's original arithmetic under their adapter.
static inline int need_class_source(const std::string&raw,float user_valence) {
    const auto s=lower_(raw);
    for(const char*p:{"i need you to listen","i just need to vent","please just listen","i feel overwhelmed","i feel devastated","i feel hopeless","i'm grieving"})
        if(s.find(p)!=std::string::npos)return 1;
    for(const char*p:{"what color","what colour","what day","what date","how many","how much","compare ","read the ","read this ","repeat the ","calculate ","what is recorded","what do you remember","what did i say","what's on the card","what is on the card"})
        if(s.find(p)!=std::string::npos)return 2;
    // Open factual interrogatives remain tasks even when their exact noun was
    // absent from the small legacy list. Personal support questions retain the
    // normal contextual classifier; a question mark alone proves neither mood.
    const bool support=s.find("why am i")!=s.npos||s.find("how can i")!=s.npos||s.find("what should i")!=s.npos||s.find("my feelings")!=s.npos;
    if(!support)for(const char*p:{"what is ","what are ","what was ","what were ","when is ","when was ","where is ","where are ","which "})
        if(s.rfind(p,0)==0)return 2;
    return need_class(raw,user_valence);
}
} // namespace aff2
