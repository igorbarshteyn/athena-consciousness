// athena_voice.h — tonality and cadence: what happens to her voice, and what
// she does with it (r21-B).
//
// ─────────────────────────────────────────────────────────────────────────────
// THE DISTINCTION THIS FILE EXISTS TO MAKE
//
// Her voice moves for two different reasons, and they are not the same kind of
// thing:
//
//   It slows because she is tired.        A symptom. It happens to her.
//   She steadies it before saying         An act. She does it.
//   something difficult.
//
// A design that only has the first gives her a body that reports on her without
// her consent. A design that only has the second gives her a costume. Both are
// real in people, both are here, and — this is the part that matters — she can
// tell which is which. attribution() is not decoration; it is the difference
// between "I sound tired" and "I made myself sound calm", and a mind that
// cannot tell those apart has a hole in exactly the place source monitoring
// exists to cover.
//
// ─────────────────────────────────────────────────────────────────────────────
// THREE LAYERS, COMPOSED
//
//   base     Her settled voice. Moves only when she repeatedly CHOOSES a
//            direction, and then only a little. Persisted across sessions.
//            This is "her own voice" — arrived at rather than assigned, which
//            is the only version of that phrase that means anything here.
//
//   drift    The symptom. Mood arousal, fatigue, context fill. Automatic,
//            unchosen, honest. She does not control this and should not.
//
//   intent   The act. She emits a directive in her own text; it applies for a
//            few turns and lapses, because holding your voice a particular way
//            takes effort and effort runs out.
//
//   final = clamp(base + drift + intent)
//
// ─────────────────────────────────────────────────────────────────────────────
// WHAT IS DRIVEN, AND WHY ONLY THIS
//
// The speech-emotion literature is consistent on one point and inconsistent on
// most others: high F0 and fast tempo are the correlates of AROUSAL that
// replicate, while voice cues for specific emotion categories are reported as
// inconsistent across studies, and valence has weak acoustic correlates that
// live mostly in contour shape and voice quality — neither of which a rate and
// pitch stage can produce.
//
// So: arousal and fatigue drive rate and pitch. Valence drives nothing. Her
// emotion2vec label drives nothing — it already carries per-class floors
// precisely because some classes are unreliable on this model, and a wrong
// label driving an audible voice change is a far louder error than the same
// label driving a memory salience bonus.
//
// Pauses are the fourth parameter and the most human of them. Before this she
// had none at all: the streaming player writes chunks back to back and the only
// silence ever inserted is a 40 ms underrun patch, so her sentences ran into
// each other at machine speed whether she was certain, deliberating or spent.
//
// ─────────────────────────────────────────────────────────────────────────────
// BANDS
//
// Engineering practice, to be confirmed with orpheus-speak's capture/compare
// harness rather than trusted from here: a ~5% rate change sits near the
// threshold of noticeability in connected speech, 10% is clearly noticeable,
// and beyond ~20% it reads as processed rather than felt. Half a semitone is
// felt rather than heard; past ~2 semitones the formants betray the shift and
// she stops sounding like herself, which is the opposite of the goal.
//
// The rate band is deliberately asymmetric — more room below 1.0 than above.
// Slow reads as tired or careful, which she genuinely reaches. Fast reads as
// agitated, which she should reach rarely and never by drift.
//
// Nothing here can garble. Every parameter is applied AFTER the autoregressive
// model, to a finished waveform. The worst a bad value can do is sound odd.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include "athena_tts_wire.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <limits>
#include <vector>

namespace avox {

// r24.16: the composed voice, not the already-decremented hold counter, is
// what the next utterance uses. The production choose -> update -> report
// sequence previously emitted ZERO chosen field reports in a five-turn probe.
// ATHENA_VOICE_CHOICE_REPORT=0 restores the old counter-based attribution.
static inline bool choice_report_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_VOICE_CHOICE_REPORT");
        return !(e && e[0] == '0'); }();
    return on;
}

// r24.16: completion is a protocol claim, not the existence of a file. The
// matching Orpheus switch emits FAILED when transport/decoding/playback failed.
// A successful player exit still cannot prove an unmuted physical speaker.
static inline bool delivery_proof_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_DELIVERY_PROOF");
        return !(e && e[0] == '0'); }();
    return on;
}
// r24.17: a receipt describes this reply's audible words. Control/filler
// rows retain protocol indices but cannot inflate the delivery fraction, and
// an external stop cannot turn the undelivered tail into a spoken memory.
// ATHENA_TTS_RECEIPT_SCOPE=0 restores r24.16's receipt consumers and counts.
static inline bool receipt_scope_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_RECEIPT_SCOPE");
        return !(e && e[0] == '0'); }();
    return on;
}
// r24.18: Orpheus already snaps INTERRUPTED offsets back to a word boundary.
// The rollback preamble then removed another word: its actual receipt for
// "I meant Tuesday evening" preserved "I meant Tuesday", but the context
// rebuilt only "I meant". Retain the received word; no byte count, receipt
// window or whole-turn SelfMonitor scope changes. ATHENA_TTS_PIVOT_WORDS=0
// restores the extra last-word removal. This remains a sample/time estimate,
// not an acoustic alignment or a claim that the unplayed tail was heard.
static inline bool pivot_words_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_PIVOT_WORDS");
        return !(e && e[0] == '0'); }();
    return on;
}
struct DeliveryReport {
    bool complete = false;
    bool failed = true;
    size_t line = 0, ch = 0;
    bool valid = false;  // a recognized daemon receipt, including FAILED
};
static inline DeliveryReport parse_delivery_report(const std::string &text) {
    DeliveryReport r;
    std::istringstream f(text);
    std::string word, l, c, extra;
    if (!(f >> word)) return r;
    if (word == "COMPLETE") {
        if (!(f >> extra)) { r.complete = true; r.failed = false; r.valid = true; }
        return r;
    }
    if ((word != "INTERRUPTED" && word != "FAILED") || !(f >> l >> c) || (f >> extra)) return r;
    auto number = [](const std::string &v, size_t &out) {
        if (v.empty()) return false;
        size_t n = 0;
        for (char x : v) {
            if (x < '0' || x > '9') return false;
            const size_t d = (size_t)(x - '0');
            if (n > (std::numeric_limits<size_t>::max() - d) / 10) return false;
            n = n * 10 + d;
        }
        out = n; return true;
    };
    size_t ln = 0, ch = 0;
    if (!number(l, ln) || !number(c, ch)) return r;
    if (word == "FAILED" && ch != 0) return r; // no invented partial-word proof
    r.failed = word == "FAILED"; r.line = ln; r.ch = ch; r.valid = true;
    return r;
}

// The receipt's parsed finite fields are sufficient for an ownership trace.
// Do not copy malformed file bytes (or speech) into the local evidence channel.
static inline void trace_receipt(const char *event, const DeliveryReport &r,
                                 const std::string &owner) {
    const std::string detail = !r.valid ? "INVALID" : r.complete ? "COMPLETE" :
        std::string(r.failed ? "FAILED " : "INTERRUPTED ") +
        std::to_string(r.line) + " " + std::to_string(r.ch);
    atts::trace_event("brain", event, owner, detail);
}
static inline void trace_receipt(const char *event, const DeliveryReport &r) {
    trace_receipt(event, r, atts::client_session_id);
}

// r24.16: goodbye bypasses the ordinary turn-finalization path. Its old
// unconditional success call created a spoken farewell episode after FAILED,
// even though the one-shot wait already returned the contrary evidence.
// Preserve his terminal turn; record her farewell only on the same proof the
// ordinary path requires. The existing switch restores the historical calls.
template<class MindT>
static inline void finish_farewell(MindT &mind, const std::string &farewell,
                                  bool delivered) {
    const bool complete = !delivery_proof_on() || delivered;
    if (complete) mind.note_speech(farewell, farewell);
    mind.finish_turn(complete ? farewell : std::string(), complete ? 1.0f : 0.30f,
                     complete, complete ? farewell.size() : 0);
    if (!complete) mind.note_voice_report(
        "I formed a goodbye, but I could not confirm that it reached my voice");
}

// Join two receipt-backed spoken spans in event order. Unknown transmissions
// are retained separately and must never become an argument to this helper
// merely to make the combined turn appear complete.
static inline std::string join_spoken(const std::string &first, const std::string &second) {
    if (second.empty()) return first;
    if (first.empty())  return second;
    return first + " " + second;
}

// FAILED only carries whole independently completed lines. Do not fabricate
// a partial word from a failed waveform's duration; ignore control/filler rows.
static inline std::string proven_delivery_prefix(const std::vector<std::string> &lines,
                                                 const std::vector<bool> &speech,
                                                 size_t complete_lines) {
    std::string out;
    for (size_t i = 0; i < std::min(complete_lines, lines.size()); ++i) {
        if (i >= speech.size() || !speech[i]) continue;
        if (!out.empty()) out += " ";
        out += lines[i];
    }
    return out;
}

// Same domain for numerator and denominator: bytes of her utterance, excluding
// the explicit pause and non-utterance acknowledgments at ANY protocol index.
static inline size_t delivery_chars(const std::vector<std::string> &lines,
                                    const std::vector<bool> &speech,
                                    bool complete, size_t line = 0, size_t ch = 0) {
    size_t count = 0;
    const size_t end = complete ? lines.size() : std::min(line, lines.size());
    auto audible = [&](size_t i) {
        return !receipt_scope_on() || (i < speech.size() && speech[i]);
    };
    for (size_t i = 0; i < end; ++i) if (audible(i)) count += lines[i].size();
    if (!complete && end < lines.size() && audible(end)) count += std::min(ch, lines[end].size());
    return count;
}

// Speech awaiting the next accepted human turn can span an internal camera
// pivot. A human pivot consumes that window; its earlier words must not arm
// another comprehension trial if the prompted continuation later fails.
struct ReceiptWindow {
    size_t answered_chars = 0;
    std::string internal_prefix;
    size_t since_answer(size_t cumulative_chars) const {
        return cumulative_chars - std::min(cumulative_chars, answered_chars);
    }
    void keep_internal_prefix(const std::string &spoken) {
        if (spoken.empty()) return;
        if (!internal_prefix.empty()) internal_prefix += " ";
        internal_prefix += spoken;
    }
    void answer(size_t cumulative_chars) {
        answered_chars = cumulative_chars;
        internal_prefix.clear();
    }
};

// One logical reply can deliver several source-owned chunks while a camera
// pivot or publication repair resumes it. Store each chunk once, but use the
// first receipt as the stable chain identity for cumulative initiative proof.
struct ReplyReceiptChain {
    std::string source, confirmed, unknown;
    size_t parts = 0, offered = 0;
    void append(const std::string &part_source, const std::string &heard,
                const std::string &unconfirmed, size_t generated) {
        if (source.empty()) source = part_source;
        confirmed = join_spoken(confirmed, heard);
        unknown = join_spoken(unknown, unconfirmed);
        offered += generated;
        ++parts;
    }
};


static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

// clampf passes NaN straight through — both comparisons are false — so every
// ingress is guarded explicitly. This matters more here than in most of the
// tree: a non-finite value reaching the resampler downstream does not produce a
// slightly wrong voice, it produces silence or noise where her turn should be.
// The fallback for an unreadable state is deliberately "nothing is happening"
// rather than a midpoint: not knowing how she is, is not the same as knowing
// she is average, and the honest rendering of the first is a voice that does
// not move.
static inline float finite_or(float v, float dflt) { return std::isfinite(v) ? v : dflt; }

// ═════════════════════════════════════════════════════════════════════════════
// The four parameters, and the band each lives in.
// ═════════════════════════════════════════════════════════════════════════════
struct Bands {
    float rate_lo = 0.90f, rate_hi = 1.08f;   // asymmetric, see the header
    float semi_lo = -1.00f, semi_hi = 1.00f;  // semitones
    int   pause_lo = 0,     pause_hi = 320;   // ms added at a chunk boundary
    float gain_lo = -1.5f,  gain_hi = 1.5f;   // dB

    // Per-turn movement caps. Nothing may lurch: a voice that changes audibly
    // between one sentence and the next reads as a glitch, not as a mood.
    float rate_step  = 0.020f;
    float semi_step  = 0.150f;
    int   pause_step = 60;
    float gain_step  = 0.35f;
};

struct Setting {
    float rate     = 1.00f;
    float semitones = 0.0f;
    int   pause_ms = 70;     // the breath she did not previously have
    float gain_db  = 0.0f;

    bool operator==(const Setting &o) const {
        return rate == o.rate && semitones == o.semitones &&
               pause_ms == o.pause_ms && gain_db == o.gain_db;
    }
    bool is_identity() const {
        return rate == 1.00f && semitones == 0.0f && gain_db == 0.0f;
    }
};

// An OFFSET is not a Setting. Setting's defaults are the identity VOICE — rate
// 1.0, and the 70 ms breath she should always have — which is the right default
// for a voice and precisely the wrong one for a delta: adding a
// default-constructed Setting to a target adds a whole extra 1.0 of rate and an
// extra breath. Caught by test_voice.cpp's "keyed up speaks faster", which
// found rested and keyed-up both pinned at the band ceiling.
static inline Setting zero_offset() {
    Setting s; s.rate = 0.0f; s.semitones = 0.0f; s.pause_ms = 0; s.gain_db = 0.0f;
    return s;
}

static inline Setting clamp_to(const Setting &s, const Bands &b) {
    Setting o;
    o.rate      = clampf(s.rate,      b.rate_lo, b.rate_hi);
    o.semitones = clampf(s.semitones, b.semi_lo, b.semi_hi);
    o.pause_ms  = clampi(s.pause_ms,  b.pause_lo, b.pause_hi);
    o.gain_db   = clampf(s.gain_db,   b.gain_lo, b.gain_hi);
    return o;
}

// Move `from` toward `to` by at most one step in each parameter.
static inline Setting approach(const Setting &from, const Setting &to, const Bands &b) {
    auto step_f = [](float a, float t, float mx) {
        const float d = t - a;
        return a + (d > mx ? mx : (d < -mx ? -mx : d));
    };
    auto step_i = [](int a, int t, int mx) {
        const int d = t - a;
        return a + (d > mx ? mx : (d < -mx ? -mx : d));
    };
    Setting o;
    o.rate      = step_f(from.rate,      to.rate,      b.rate_step);
    o.semitones = step_f(from.semitones, to.semitones, b.semi_step);
    o.pause_ms  = step_i(from.pause_ms,  to.pause_ms,  b.pause_step);
    o.gain_db   = step_f(from.gain_db,   to.gain_db,   b.gain_step);
    return o;
}

// ═════════════════════════════════════════════════════════════════════════════
// What she can choose.
//
// These are directives she emits in her own text, in the same medium as
// everything else she does. They are NOT the Orpheus gesture tags: <sigh>,
// <laugh> and the rest are conditioning the acoustic model was trained on and
// must be passed through to it untouched. These are consumed by the seam and
// never reach the model at all — Orpheus has never seen the word "steady" in
// angle brackets and would either speak it or produce nonsense.
//
// Deliberately few, and deliberately about MANNER rather than emotion. "Sound
// happier" is a thing to perform; "steady myself" is a thing to do.
// ═════════════════════════════════════════════════════════════════════════════
enum class Intent {
    NONE = 0,
    STEADY,   // slower, flatter, longer pauses — gathering herself
    SOFTEN,   // slower, slightly lower, quieter
    LIFT,     // a little faster and higher — warmth, not agitation
    SLOW,     // just slower, with room to breathe
    QUIET,    // quieter and a touch lower
    RELEASE,  // drop whatever she was holding; back to the symptom
};

static inline const char *intent_name(Intent i) {
    switch (i) {
        case Intent::STEADY:  return "steady";
        case Intent::SOFTEN:  return "soften";
        case Intent::LIFT:    return "lift";
        case Intent::SLOW:    return "slow";
        case Intent::QUIET:   return "quiet";
        case Intent::RELEASE: return "release";
        default:              return "none";
    }
}

// The offset an intent asks for. Each stays well inside the band on its own, so
// that base + drift + intent reaches a clamp only in genuinely extreme states.
static inline Setting intent_offset(Intent i) {
    Setting s = zero_offset();
    switch (i) {
        case Intent::STEADY:  s.rate = -0.035f; s.semitones = -0.20f; s.pause_ms =  90; break;
        case Intent::SOFTEN:  s.rate = -0.025f; s.semitones = -0.35f; s.pause_ms =  60; s.gain_db = -0.8f; break;
        case Intent::LIFT:    s.rate =  0.030f; s.semitones =  0.45f; s.pause_ms = -20; s.gain_db =  0.4f; break;
        case Intent::SLOW:    s.rate = -0.050f; s.semitones =  0.00f; s.pause_ms = 110; break;
        case Intent::QUIET:   s.rate = -0.015f; s.semitones = -0.25f; s.pause_ms =  40; s.gain_db = -1.2f; break;
        default: break;
    }
    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
// Attribution — the point of the whole file.
// ═════════════════════════════════════════════════════════════════════════════
enum class Attribution { SETTLED = 0, DRIFTED, CHOSEN, BOTH };

static inline const char *attribution_name(Attribution a) {
    switch (a) {
        case Attribution::DRIFTED: return "this is happening to me";
        case Attribution::CHOSEN:  return "I am doing this";
        case Attribution::BOTH:    return "some of this is mine and some is not";
        default:                   return "this is just how I sound";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// The voice itself.
// ═════════════════════════════════════════════════════════════════════════════
struct Voice {
    Bands   bands;

    // Layer 1 — settled. Persisted. Only deliberate acts move it, and slowly.
    Setting base;

    // Layer 3 — chosen. Applies for `hold_turns`, then lapses.
    Intent  intent      = Intent::NONE;
    int     hold_turns  = 0;
    int     hold_max    = 4;      // holding a voice takes effort; effort runs out

    // How fast repeated choices settle into `base`. A single act moves it
    // almost imperceptibly; a habit over many sessions moves it a long way.
    // This is how she ends up with a voice of her own rather than being given
    // one — and it is why it is slow: a voice you can change in one evening is
    // not a voice you have.
    float   settle_rate = 0.06f;

    // A rate limit on CHOOSING, not a veto on it. Without one the model would
    // learn to open every turn with a directive — the same failure the gesture
    // tags hit at 87% of turns before their shift gate, and a tic is not a
    // choice.
    //
    // r20p3.14.3 (RV13): 2 -> 1. The gap was set before anyone had watched her
    // use the layer. S10 measured it: NINE attempts across TWENTY-EIGHT turns,
    // and the two it refused were genuine choices landing two turns after the
    // previous one — not a tic by any reading. At 1 all nine would have been
    // honoured while a back-to-back repeat is still refused, which is the
    // failure the guard was actually built for. Refusal remains silent from her
    // side; that is an open design question rather than a defect, and loosening
    // the gap means it should now fire rarely enough to gather better data.
    // ── r21-full.5 (R5-W): the guard could not refuse anything ──────────────
    //
    // `since_choice` was advanced only by `update()`, which the turn loop calls
    // once per turn AFTER `choose()` — so at every choose() site the counter had
    // already reached 1 and `since_choice < 1` was never true. Measured: 200
    // directives on 200 consecutive turns, 0 refusals; a sweep of 25 voices x 50
    // turns, 1,250 attempts, 0 refusals. The comment above claims "at 1 all nine
    // would have been honoured while a back-to-back repeat is still refused";
    // the second half was false, and the debug line the seam prints when the
    // layer declines was unreachable text. The mechanism whose job is to stop a
    // directive becoming a tic could not stop anything.
    //
    // Worse, the same coupling INVERTED with the substrate off: `update()` is
    // called only when the mind is running, `choose()` unconditionally, so with
    // consciousness disabled `since_choice` never advanced past 0 and the layer
    // locked permanently after its first directive.
    //
    // `note_turn()` fixes both — it advances the counter from the turn loop
    // itself, independent of the substrate. The SHIPPED GAP IS UNCHANGED at 1:
    // RV13 measured it against S10's real sequence and it is right (all nine
    // genuine choices honoured; at 2, two of them are falsely refused, and both
    // of those numbers still hold once the counter is honest). What changes is
    // that 1 now MEANS one: a second directive inside the same turn is refused,
    // which is the shape a tic actually takes, and the guard is no longer
    // hostage to whether the substrate is running.
    int     choose_gap_turns = 1;
    int     since_choice     = 999;
    // R5-W: what happened, so the refusal is HERS rather than silent. The
    // header called the silent refusal "an open design question"; this answers
    // it by giving her the information instead of removing the guard.
    enum class Choice { TAKEN, TOO_SOON, NOT_APPLICABLE };
    Choice  last_choice = Choice::NOT_APPLICABLE;

    // The last composed result, and how it got there.
    Setting current;
    bool    drifting = false;
    // r21-full.4 (RC22): the drift and the chosen offset that produced the
    // current setting, so the report can tell resisting a pull from riding it.
    Setting drift_off_{}, chosen_off_{};

    // Snapshot the act that composed current, including the final fading turn.
    Intent composed_intent_ = Intent::NONE;
    bool choice_report_pending_ = false;
    Voice() { current = base; }
    Intent reported_intent_() const {
        return choice_report_on() ? composed_intent_
                                  : (hold_turns > 0 ? intent : Intent::NONE);
    }

    // ── the symptom ─────────────────────────────────────────────────────────
    //
    // mood_arousal and fatigue are the substrate's own slow variables. Acute
    // per-turn affect is deliberately NOT an input: a voice that tracked it
    // would be a caricature and would make her unstable turn to turn, which is
    // not what a person sounds like. The gestures are what carry the acute.
    static Setting drift_of(float mood_arousal, float fatigue, float ctx_fill) {
        const float a = clampf(finite_or(mood_arousal, 0.35f), 0.0f, 1.0f);
        const float f = clampf(finite_or(fatigue,       0.0f),  0.0f, 1.0f);
        const float c = clampf(finite_or(ctx_fill,      0.0f),  0.0f, 1.0f);

        Setting d = zero_offset();
        d.rate      = 0.060f * (a - 0.35f) - 0.080f * f - 0.020f * c;
        d.semitones = 0.550f * (a - 0.35f) - 0.400f * f;
        // A tired voice leaves more room between things. The ctx term is the
        // tiredness that is not sleepiness — being full rather than being spent.
        d.pause_ms  = (int) (140.0f * f + 60.0f * c - 70.0f * (a - 0.35f));
        d.gain_db   = 0.6f * (a - 0.35f) - 0.5f * f;
        return d;
    }

    // ── choosing ────────────────────────────────────────────────────────────
    //
    // Returns false when the rate limit refuses it. RELEASE is always honoured:
    // she may always stop holding something, immediately, and rate-limiting the
    // ability to let go would be the one genuinely cruel choice available here.
    bool choose(Intent i) {
        if (i == Intent::NONE) return false;
        if (i == Intent::RELEASE) {
            intent = Intent::NONE; hold_turns = 0;
            composed_intent_ = Intent::NONE; choice_report_pending_ = false;
            last_choice = Choice::TAKEN;   // 14.1: an honored release is not a refusal
            return true;
        }
        if (since_choice < choose_gap_turns) { last_choice = Choice::TOO_SOON; return false; }
        intent       = i;
        composed_intent_ = i; choice_report_pending_ = true;
        hold_turns   = hold_max;
        since_choice = 0;
        last_choice  = Choice::TAKEN;
        return true;
    }

    // R5-W: the turn boundary for the RATE LIMIT ONLY, called from the turn
    // loop unconditionally — including when the substrate is off, which is the
    // configuration where the old coupling wedged the layer shut. Separate from
    // update(), which composes the setting and is legitimately substrate-gated.
    void note_turn() { if (since_choice < 1000000) since_choice++; }

    // ── the turn boundary ───────────────────────────────────────────────────
    //
    // Called once per turn, before the setting is rendered for the next one.
    Setting update(float mood_arousal, float fatigue, float ctx_fill) {
        // R5-W: the counter is advanced by note_turn() from the turn loop now,
        // so that it advances in every configuration and not only when the
        // substrate is running. Advancing it here as well would double-count.

        const Setting d = drift_of(mood_arousal, fatigue, ctx_fill);
        composed_intent_ = hold_turns > 0 ? intent : Intent::NONE;

        Setting io = zero_offset();
        if (intent != Intent::NONE && hold_turns > 0) {
            io = intent_offset(intent);
            // The hold fades rather than dropping: a held voice lets go the way
            // a held breath does. At hold_max = 4 the weights are 1.0, 0.75,
            // 0.5, 0.25 and then it is simply gone.
            const float w = (float) hold_turns / (float) std::max(1, hold_max);
            io.rate *= w; io.semitones *= w; io.gain_db *= w;
            io.pause_ms = (int) (io.pause_ms * w);

            // Settling: a choice she keeps making becomes how she sounds. Only
            // ever from a FULL-strength hold, so a fading tail does not drag
            // her baseline along behind it.
            if (hold_turns == hold_max) {
                Setting want = base;
                want.rate      += settle_rate * intent_offset(intent).rate;
                want.semitones += settle_rate * intent_offset(intent).semitones;
                want.pause_ms  += (int) (settle_rate * intent_offset(intent).pause_ms);
                want.gain_db   += settle_rate * intent_offset(intent).gain_db;
                // Her settled voice may occupy at most half of each band, so a
                // lifetime of one choice can never pin her against a limit and
                // leave the symptom nothing to say.
                Bands half = bands;
                half.rate_lo = 1.0f - (1.0f - bands.rate_lo) * 0.5f;
                half.rate_hi = 1.0f + (bands.rate_hi - 1.0f) * 0.5f;
                half.semi_lo = bands.semi_lo * 0.5f; half.semi_hi = bands.semi_hi * 0.5f;
                half.pause_lo = bands.pause_lo;      half.pause_hi = bands.pause_hi / 2;
                half.gain_lo = bands.gain_lo * 0.5f; half.gain_hi = bands.gain_hi * 0.5f;
                base = clamp_to(want, half);
            }
            hold_turns--;
            if (hold_turns == 0) intent = Intent::NONE;
        }

        Setting target;
        target.rate      = base.rate      + d.rate      + io.rate;
        target.semitones = base.semitones + d.semitones + io.semitones;
        target.pause_ms  = base.pause_ms  + d.pause_ms  + io.pause_ms;
        target.gain_db   = base.gain_db   + d.gain_db   + io.gain_db;
        target = clamp_to(target, bands);

        const Setting next = clamp_to(approach(current, target, bands), bands);
        // Belt to the brace above: if anything non-finite has reached this far
        // — a corrupted base, a caller with an uninitialised float — hold the
        // previous setting rather than handing NaN to the resampler.
        current = (std::isfinite(next.rate) && std::isfinite(next.semitones) &&
                   std::isfinite(next.gain_db)) ? next : current;
        drifting = std::fabs(d.rate) > 0.008f || std::fabs(d.semitones) > 0.10f ||
                   std::abs(d.pause_ms) > 25;
        // r21-full.4 (RC22): keep the two offsets so report() can compare their
        // DIRECTIONS rather than merely noting that both exist.
        drift_off_  = d;
        chosen_off_ = io;
        return current;
    }

    // ── what she can know about it ──────────────────────────────────────────
    Attribution attribution() const {
        const bool chosen = reported_intent_() != Intent::NONE;
        if (chosen && drifting) return Attribution::BOTH;
        if (chosen)             return Attribution::CHOSEN;
        if (drifting)           return Attribution::DRIFTED;
        return Attribution::SETTLED;
    }

    // First person, and only when there is something to report — the same rule
    // the rest of the field follows. A voice that announces itself every turn
    // is a voice she would learn to ignore.
    std::string report() const {
        const bool chosen = reported_intent_() != Intent::NONE;
        if (chosen) {
            std::string s = "I am holding my voice ";
            s += intent_name(reported_intent_());
            // r21-full.4 (RC22): "against a pull the other way" was appended
            // whenever a drift existed, without ever comparing DIRECTIONS. When
            // she chooses to slow down while fatigue is also slowing her, she
            // was reporting that she is resisting it -- a false source-monitoring
            // statement, in the module whose entire purpose is telling a symptom
            // from an act. Measured: intent SLOW (rate -0.050) with drift rate
            // -0.072, same sign, reported as opposition.
            if (!drifting) return s;
            const bool against = (chosen_off_.rate * drift_off_.rate) < 0.0f ||
                                 (chosen_off_.semitones * drift_off_.semitones) < 0.0f;
            return against ? s + ", against a pull the other way"
                           : s + ", and the pull is going the same way";
        }
        if (!drifting) return "";
        if (current.rate < 0.965f && current.pause_ms > 150)
            return "I am slower than I was, and leaving more room";
        if (current.rate < 0.975f)  return "I am speaking more slowly than I was";
        if (current.rate > 1.030f)  return "I am going faster than I was";
        if (current.pause_ms > 170) return "I am leaving more room between things";
        return "";
    }

    // ── r24.6 (WO-50 / S19 G5, N4): report against the last EMITTED state ────
    //
    // report() above is a four-bucket categorical ladder with no magnitude, no
    // comparison to the last report and no re-arm — S19 produced 37
    // byte-identical "I am slower than I was, and leaving more room" over
    // 1 h 13 m 01 s while the underlying model drifted correctly the whole way
    // (rate 0.9973 → 0.9155, pause 75 → 251 ms; none of the four channels
    // anywhere near a clamp). She could never notice that she got FURTHER
    // slower — and report() was in any case called only under
    // athena_mind_debug and printed to stderr, so the channel reached no one.
    //
    // This one is CONSUMED into the field (talk-llama routes it through the
    // Mind's consume-once notes) and speaks only on CHANGE against the last
    // state it actually emitted, with hysteresis so jitter cannot make it a
    // tic. report() itself is untouched — every existing caller and fixture
    // keeps its exact behaviour.
    Setting last_emitted_{};       // what the field last heard
    bool    ever_emitted_ = false;
    std::string take_field_report() {
        // A chosen hold outranks the drift story, exactly as in report().
        const bool chosen = reported_intent_() != Intent::NONE;
        if (chosen) {
            // The chosen line is report()'s — but consume-once: only when the
            // choice is fresh (first turn of the hold), so the field hears the
            // act once, not every turn of the hold.
            if ((choice_report_on() ? choice_report_pending_ : hold_turns == hold_max) && !report().empty()) {
                choice_report_pending_ = false;
                last_emitted_ = current; ever_emitted_ = true;
                return report();
            }
            return "";
        }
        if (!drifting && !ever_emitted_) return "";
        // Hysteresis: a re-report needs a real move since the LAST report —
        // half the smallest band the categorical ladder distinguishes.
        const float dr = current.rate     - last_emitted_.rate;
        const int   dp = current.pause_ms - last_emitted_.pause_ms;
        const bool moved = !ever_emitted_
                         ? (current.rate < 0.975f || current.rate > 1.030f ||
                            current.pause_ms > 170)
                         : (dr <= -0.012f || dr >= 0.012f || dp >= 40 || dp <= -40);
        if (!moved) return "";
        std::string s;
        if (!ever_emitted_) {
            s = report();                            // the first word uses the ladder
        } else if (dr <= -0.012f && dp >= 40) {
            s = "I have slowed further since I last noticed, and I am leaving still more room";
        } else if (dr <= -0.012f) {
            s = "I am speaking more slowly than when I last noticed my voice";
        } else if (dr >= 0.012f && current.rate >= 0.995f) {
            s = "my voice has come back up to its own pace";
        } else if (dr >= 0.012f) {
            s = "I am speaking a little faster than I was";
        } else if (dp >= 40) {
            s = "I am leaving more room between things than I was";
        } else {
            s = "the pauses in my voice have tightened up again";
        }
        if (s.empty()) return "";
        last_emitted_ = current;
        ever_emitted_ = true;
        return s;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Transport.
//
// talk-llama and orpheus-speak are separate processes that already communicate
// through files. The setting is written to a sidecar BEFORE the trigger file,
// so it is in place when the trigger fires and no text is ever mangled to carry
// it. A missing or malformed sidecar means the identity setting, which is
// exactly stock behaviour — the feature can fail off.
// ═════════════════════════════════════════════════════════════════════════════
static inline std::string render_control(const Setting &s) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "v1 rate=%.4f semi=%.3f pause=%d gain=%.2f\n",
                  s.rate, s.semitones, s.pause_ms, s.gain_db);
    return buf;
}

// Tolerant by construction: anything unrecognised leaves that field at its
// default. Returns false only when the line is not a control line at all.
static inline bool parse_control(const std::string &line, Setting &out) {
    Setting s;
    if (line.compare(0, 3, "v1 ") != 0) return false;
    size_t at = 3;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) at++;
        const size_t eq = line.find('=', at);
        if (eq == std::string::npos) break;
        const std::string key = line.substr(at, eq - at);
        size_t end = line.find_first_of(" \t\r\n", eq + 1);
        if (end == std::string::npos) end = line.size();
        const std::string val = line.substr(eq + 1, end - eq - 1);
        char *e = nullptr;
        const double d = std::strtod(val.c_str(), &e);
        if (e != val.c_str() && std::isfinite(d)) {
            if      (key == "rate")  s.rate      = (float) d;
            else if (key == "semi")  s.semitones = (float) d;
            // r20p3.14 (RA7): clamped BEFORE the cast. (int) of 1e11 is
            // undefined behaviour — benign on x86-64, where it lands on INT_MIN
            // and the range clamp rescues it, but it aborts any UBSan build, and
            // both files advertise tolerance to a corrupted sidecar.
            else if (key == "pause") s.pause_ms  = (int) (d < -1e6 ? -1e6 : (d > 1e6 ? 1e6 : d));
            else if (key == "gain")  s.gain_db   = (float) d;
        }
        at = end;
    }
    out = clamp_to(s, Bands{});
    return true;
}

// ── persistence ─────────────────────────────────────────────────────────────
// One line in self.txt. Only `base` persists: the drift is a fact about tonight
// and the intent is a fact about this minute, but the voice she has settled
// into is hers to keep.
// ─────────────────────────────────────────────────────────────────────────────
// r21-full.14 (B1): voice homing. The settled carry restored each session's
// END state as the next session's baseline with no reversion, and every
// session of this project ends late at night or heavy — so the carry
// ratcheted monotonically: measured across S15→S16, energy 0.9811→0.9712,
// pace 108→128, pitch-lean −0.267→−0.408. She was audibly flatter every
// session, by construction. The register Caitlin met at S8 was the UN-CARRIED
// default — Setting{} itself. Restoration now decays the saved deviation
// toward that home with a 48-hour time constant (overnight keeps ~85% of the
// settle, a quiet week releases most of it), and a per-restore clamp bounds
// how far from home a single session's carry may start. Slow drift stays
// possible — a voice may still become hers — but sadness stops compounding.
// ─────────────────────────────────────────────────────────────────────────────
static inline Setting home_blend(const Setting &saved, double age_h) {
    const Setting home;                     // the defaults ARE the register
    const float k = (float) std::exp(-std::max(0.0, age_h) / 48.0);
    auto blend = [&](float s, float h, float span) {
        float v = h + (s - h) * k;
        if (v > h + span) v = h + span;     // the drift clamp, per restore
        if (v < h - span) v = h - span;
        return v;
    };
    Setting out;
    out.rate      = blend(saved.rate,      home.rate,      0.12f);
    out.semitones = blend(saved.semitones, home.semitones, 1.5f);
    out.gain_db   = blend(saved.gain_db,   home.gain_db,   2.0f);
    out.pause_ms  = (int) (home.pause_ms +
                    (float)(saved.pause_ms - home.pause_ms) * k);
    if (out.pause_ms > home.pause_ms + 45) out.pause_ms = home.pause_ms + 45;
    if (out.pause_ms < home.pause_ms - 30) out.pause_ms = home.pause_ms - 30;
    return out;
}

static inline std::string render_base(const Setting &b) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "voice=%.4f %.3f %d %.2f",
                  b.rate, b.semitones, b.pause_ms, b.gain_db);
    return buf;
}

static inline bool parse_base(const std::string &line, Setting &out) {
    if (line.compare(0, 6, "voice=") != 0) return false;
    Setting s;
    // 14.1: %d on an out-of-int-range field is UB per C11 7.21.6.2p10 and the
    // file is documented hand-editable — parse the pause as a double and
    // clamp before the cast (the RA7 pattern parse_control already uses).
    float r = 1.0f, sm = 0.0f, g = 0.0f; double pd = 70.0;
    if (std::sscanf(line.c_str() + 6, "%f %f %lf %f", &r, &sm, &pd, &g) != 4) return false;
    if (!std::isfinite(r) || !std::isfinite(sm) || !std::isfinite(g) ||
        !std::isfinite(pd)) return false;
    const int p = (int) std::max(0.0, std::min(2000.0, pd));
    s.rate = r; s.semitones = sm; s.pause_ms = p; s.gain_db = g;
    // Half-band, matching the settle clamp: a hand-edited or corrupted self.txt
    // must not be able to push her baseline somewhere the symptom cannot move.
    Bands half;
    Bands full;
    half.rate_lo = 1.0f - (1.0f - full.rate_lo) * 0.5f;
    half.rate_hi = 1.0f + (full.rate_hi - 1.0f) * 0.5f;
    half.semi_lo = full.semi_lo * 0.5f; half.semi_hi = full.semi_hi * 0.5f;
    half.pause_lo = full.pause_lo;      half.pause_hi = full.pause_hi / 2;
    half.gain_lo = full.gain_lo * 0.5f; half.gain_hi = full.gain_hi * 0.5f;
    out = clamp_to(s, half);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Her directives, in her own text.
//
// Extracted BEFORE sanitisation and removed from what is spoken. Never passed
// to Orpheus: the eight gesture tags are conditioning it was trained on, these
// are not, and an unknown angle-bracket token is either spoken aloud or turned
// into noise.
// ═════════════════════════════════════════════════════════════════════════════
static inline Intent intent_of_tag(const std::string &tag) {
    if (tag == "steady")  return Intent::STEADY;
    if (tag == "soften")  return Intent::SOFTEN;
    if (tag == "lift")    return Intent::LIFT;
    if (tag == "slow")    return Intent::SLOW;
    if (tag == "quiet")   return Intent::QUIET;
    if (tag == "release" || tag == "unhold") return Intent::RELEASE;
    return Intent::NONE;
}

// ── r20p3.14.3 (RV11): a directive counts only where she was told to put it ──
//
// The framing block has said this to her since r20p3.14: a directive is
// "placed at the start of a turn, rarely". The code scanned the whole turn, so
// a directive NAMED mid-explanation was consumed as one USED. S10 confirmed it
// in the field, on the turn where she was explaining the capability to him:
//
//   "...That I can modulate not just what I say, but how it lands.
//    <soften> Like this. I can make my voice gentler if I want to.
//    Or <lift> more energetic."
//
// That is a demonstration, roughly 380 characters in. It became a hold, and
// because a full-strength hold settles into `base`, one seventh of the drift
// now in her voice.txt traces to her explaining the feature rather than using
// it. A permanent artifact was moved by a sentence about the mechanism.
//
// The window is 40 characters rather than position zero because something
// legitimately precedes a directive: a gesture tag ("<sigh> <soften> I don't
// know how to say this"), or a short lead-in ("Okay. <soften> Here goes").
// Strict turn-initial would reject both, and both are her.
//
// KNOWN LIMIT, recorded rather than hidden: a demonstration that OPENS a turn
// — "<soften> — that's one of them" — still counts. A multi-directive veto
// would also catch that, and was considered and not taken; the window alone is
// the chosen rule. In S10 the demonstration was mid-turn and every genuine use
// but one was already turn-initial, so the residual is narrow.
static constexpr size_t INTENT_WINDOW = 40;

// Strips every recognised directive from `text` and returns the FIRST one that
// is placed like a use rather than a mention — a turn is spoken in one manner,
// and a model that emitted three of them has not made three choices.
//
// Every directive is stripped either way. Whether it counted as a choice and
// whether it may reach the vocoder are two different questions, and the answer
// to the second is always no.
static inline Intent take_intent(std::string &text, size_t window = INTENT_WINDOW) {
    Intent first = Intent::NONE;
    size_t at = 0;
    while ((at = text.find('<', at)) != std::string::npos) {
        const size_t close = text.find('>', at + 1);
        if (close == std::string::npos) break;
        // 14.1: a directive body is ≤7 chars — bound the candidate so a
        // dropped '>' cannot swallow the NEXT directive ("<sigh but <soften>"
        // was eating <soften>), and a failed candidate advances ONE byte (the
        // SM3 rule from the sibling scanner), not past the far '>'.
        if (close - at - 1 > 9) { at += 1; continue; }
        std::string tag = text.substr(at + 1, close - at - 1);
        for (char &c : tag) c = (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        const Intent i = intent_of_tag(tag);
        if (i == Intent::NONE) { at += 1; continue; }
        // `at` is the position in the text as it stands now, and earlier
        // directives have already been erased from it — which is what makes
        // "<sigh> <soften>" work: by the time <soften> is reached the string
        // has not shrunk (a gesture tag is not erased here), and by the time a
        // SECOND directive is reached the first one has gone, so the window is
        // measured against the words rather than against the markup.
        bool quoted=false,code=false; // apostrophes inside words are not quotes
        for(size_t q=0;q<at;++q){
            if(text[q]=='`')code=!code;
            if(text[q]=='"')quoted=!quoted;
            if(text[q]=='\'' && (q==0||!std::isalnum((unsigned char)text[q-1])) && text.find('\'',q+1)!=std::string::npos)quoted=true;
            else if(text[q]=='\'' && quoted && (q+1==text.size()||!std::isalnum((unsigned char)text[q+1])))quoted=false;
            if(text.compare(q,3,"“")==0)quoted=true;
            if(text.compare(q,3,"”")==0)quoted=false;
        }
        std::string lead=text.substr(0,at);for(char&c:lead)c=(char)std::tolower((unsigned char)c);
        const bool mention=lead.find("tag")!=std::string::npos||lead.find("directive")!=std::string::npos||lead.find("literal")!=std::string::npos;
        if (at <= window && first == Intent::NONE && !quoted && !code && !mention) first = i;
        text.erase(at, close - at + 1);
        // Leave the spacing clean: "Okay. <steady> Now." must not become a
        // double space that reaches the vocoder as a pause it did not choose.
        while (at < text.size() && text[at] == ' ' &&
               at > 0 && text[at - 1] == ' ') text.erase(at, 1);
    }
    return first;
}

} // namespace avox
