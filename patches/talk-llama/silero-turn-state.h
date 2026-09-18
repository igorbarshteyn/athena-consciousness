// silero-turn-state.h — pure end-of-turn state machine for streaming VAD.
//
// This header has NO dependencies (no whisper, no ggml, no SDL) so it can be
// unit-tested in isolation. It consumes a stream of per-frame speech
// probabilities (e.g. from Silero) plus each frame's duration in ms, and
// decides when an utterance has ended: speech, followed by a configurable run
// of trailing silence. Prosody (or anything else) may override the trailing
// target per-utterance via set_silence_target_ms().
//
// Why this exists: the legacy energy VAD (vad_simple) measured silence
// *relative* to a sliding window, which degenerates on sustained quiet. Silero
// gives an absolute per-frame speech probability, so silence is just
// "probability below threshold" — and turn-end is "that, sustained for N ms".
//
// Frame accounting is in milliseconds (frame_ms), not frame counts, so the
// caller can feed variable-size blocks; the only requirement is that each
// captured frame is pushed exactly once (the wrapper guarantees this).

#pragma once

namespace athena {

struct SileroTurnConfig {
    float threshold         = 0.5f; // speech if prob >= threshold (Silero default 0.5)
    int   min_speech_run_ms = 100;  // HYSTERESIS: consecutive speech required to confirm
                                    // a speech onset/resume. Single-frame Silero spikes
                                    // (noise, breath) are shorter than this and so cannot
                                    // reset the trailing-silence clock. Mirrors Silero's
                                    // own min_speech_duration_ms (250). 0 = no hysteresis.
    int   min_speech_ms     = 120;  // total cumulative speech in the utterance before an
                                    // endpoint may fire (a second, weaker blip guard)
    int   silence_ms        = 700;  // trailing silence (ms) that ends a turn, unless
                                    // overridden per-utterance (e.g. by prosody)
};

class SileroTurnState {
public:
    enum class Phase { WaitingForSpeech, InSpeech, Trailing };

    struct Step {
        bool in_speech;        // currently inside a speech run
        bool entered_trailing; // this step crossed speech -> silence (set target now)
        bool endpoint;         // utterance just ended (sustained trailing silence)
    };

    explicit SileroTurnState(SileroTurnConfig cfg = {}) : cfg_(cfg) { reset(); }

    // Begin a fresh utterance.
    void reset() {
        phase_      = Phase::WaitingForSpeech;
        speech_ms_  = 0.0f;
        silence_ms_ = 0.0f;
        run_ms_     = 0.0f;
        held_ms_    = 0.0f;   // R5-AE
        last_prob_  = 0.0f;
        target_ms_  = cfg_.silence_ms;
    }

    // Override the trailing-silence target for the current utterance. Intended
    // to be called by the orchestrator right after a Step reports
    // entered_trailing (e.g. prosody: shorter on a turn-final fall, longer on a
    // mid-thought rise). A value <= 0 is ignored (keeps the default).
    void set_silence_target_ms(int ms) { if (ms > 0) target_ms_ = ms; }
    float held_ms() const { return held_ms_; }   // R5-AE

    Phase phase()             const { return phase_; }
    float speech_ms()         const { return speech_ms_; }
    float silence_ms()        const { return silence_ms_; }
    float run_ms()            const { return run_ms_; }
    float last_prob()         const { return last_prob_; }
    int   silence_target_ms() const { return target_ms_; }
    const SileroTurnConfig& config() const { return cfg_; }

    // Lost capture is neither speech nor silence. Keep this turn's observed
    // speech, but require fresh consecutive evidence on the retained side of
    // the gap before confirming onset/resumption or a trailing endpoint.
    void note_capture_gap() {
        run_ms_ = 0.0f;
        silence_ms_ = 0.0f;
        held_ms_ = 0.0f;
    }

    static const char* phase_name(Phase p) {
        switch (p) {
            case Phase::WaitingForSpeech: return "wait";
            case Phase::InSpeech:         return "speech";
            case Phase::Trailing:         return "trailing";
        }
        return "?";
    }

    // Advance by a single VAD frame.
    Step push_frame(float prob, float frame_ms) {
        last_prob_ = prob;
        const bool speech = prob >= cfg_.threshold;

        // Consecutive-speech run (hysteresis). A speech frame extends the run; any
        // silence frame breaks it. Onset and resume require run >= min_speech_run_ms
        // so isolated Silero spikes (noise/breath, typically 1-2 frames) are not
        // treated as speech and cannot reset the silence clock.
        if (speech) run_ms_ += frame_ms;
        else        run_ms_  = 0.0f;
        // r20p3.14.7 (ST1): confirmation requires that THIS is a speech frame.
        // With min_speech_run_ms == 0 (the documented "no hysteresis" setting, and
        // what a negative override collapses to) the bare `run_ms_ >= 0` was true
        // even on a pure-silence frame, where run_ms_ was just zeroed — so silence
        // registered as confirmed speech, the phase never left InSpeech, the
        // trailing-silence clock never advanced, and no endpoint could ever fire:
        // a hard deadlock the moment someone trusts the "0 = no hysteresis" doc.
        // For any min_speech_run_ms > 0 this is a provable no-op — a silence frame
        // has already zeroed run_ms_, so run_ms_ >= min (> 0) is false regardless.
        const bool confirmed = speech && run_ms_ >= (float) cfg_.min_speech_run_ms;

        Step s{false, false, false};
        switch (phase_) {
            case Phase::WaitingForSpeech:
                if (confirmed) {
                    phase_      = Phase::InSpeech;
                    speech_ms_ += run_ms_;            // count the whole confirmed run once
                }
                break;

            case Phase::InSpeech:
                if (speech) {
                    speech_ms_ += frame_ms;
                } else {
                    // First silence frame starts the trailing clock promptly.
                    phase_             = Phase::Trailing;
                    silence_ms_        = frame_ms;
                    held_ms_           = 0.0f;            // R5-AE
                    target_ms_         = cfg_.silence_ms; // default; orchestrator may override
                    s.entered_trailing = true;
                }
                break;

            case Phase::Trailing:
                if (confirmed) {
                    // Sustained speech resumed -> the pause was within the utterance.
                    phase_      = Phase::InSpeech;
                    speech_ms_ += run_ms_;
                    silence_ms_ = 0.0f;
                } else if (speech && held_ms_ < (float) cfg_.min_speech_run_ms) {
                    // Unconfirmed blip (< min_speech_run_ms): hold the silence clock
                    // -- neither reset nor advance -- until we know if it is real
                    // speech. This is what stops noise/breath from resetting silence.
                    //
                    // ── r21-full.5 (R5-AE): but the hold is BOUNDED ────────────
                    // A frame that never reaches min_speech_run_ms used to
                    // contribute nothing at all, forever — so any intermittent
                    // source (typing, a fan crossing 0.5, a TV, breath) that
                    // produces runs of at most min_speech_run_ms - 1 frames
                    // stopped the end-of-turn clock indefinitely, with no cap
                    // and no diagnostic. Measured against this header: a 3-on
                    // 1-off pattern at the shipped defaults pushes the endpoint
                    // from 700 ms to 2,816 ms, and a 7-on 1-off pattern at the
                    // 250 ms value this header itself recommends pushes it to
                    // 5,632 ms. Every turn ends in seconds of dead air.
                    //
                    // Once the accumulated hold exceeds one full run length the
                    // blips have PROVEN they are not a resuming utterance — a
                    // real one would have been confirmed by now — so from there
                    // they are credited to silence at half rate. Noise still
                    // cannot RESET the clock, which is the property the hold
                    // exists for; it can no longer stop it.
                    held_ms_ += frame_ms;
                } else if (speech) {
                    held_ms_ += frame_ms;
                    silence_ms_ += 0.5f * frame_ms;
                    if (silence_ms_ >= (float) target_ms_ &&
                        speech_ms_  >= (float) cfg_.min_speech_ms) {
                        s.endpoint = true;
                    }
                } else {
                    // NOT reset here: `held_ms_` measures how long this trailing
                    // episode has been held by blips in total, and an
                    // intermittent source is silent between them by definition —
                    // zeroing it on every gap made the bound unreachable for
                    // exactly the pattern it exists to bound.
                    silence_ms_ += frame_ms;
                    if (silence_ms_ >= (float) target_ms_ &&
                        speech_ms_  >= (float) cfg_.min_speech_ms) {
                        s.endpoint = true;
                    }
                }
                break;
        }

        s.in_speech = (phase_ == Phase::InSpeech);
        return s;
    }

    // Advance by a block of frames (all the same duration). The aggregate
    // reports in_speech of the final frame, OR of entered_trailing across the
    // block, and endpoint if any frame in the block ended the turn.
    // An optional receipt exposes the final frame without changing the
    // historical aggregate result. A delayed consumer must not spend an old
    // endpoint while a later frame is holding for possible speech resumption.
    Step push_frames(const float* probs, int n, float frame_ms, bool* final_endpoint = nullptr) {
        Step agg{false, false, false};
        if (final_endpoint) *final_endpoint = false;
        for (int i = 0; i < n; ++i) {
            const Step s = push_frame(probs[i], frame_ms);
            agg.in_speech         = s.in_speech;
            agg.entered_trailing |= s.entered_trailing;
            agg.endpoint         |= s.endpoint;
            if (final_endpoint) *final_endpoint = s.endpoint;
        }
        return agg;
    }

private:
    SileroTurnConfig cfg_;
    Phase phase_;
    float speech_ms_;
    float silence_ms_;
    float run_ms_;      // current consecutive-speech run (hysteresis)
    // r21-full.5 (R5-AE): time spent in Trailing holding the silence clock for
    // unconfirmed blips. Bounded, so an intermittent noise source cannot stop
    // the end of a turn indefinitely. Exposed so --silero-debug can show WHY a
    // turn is late instead of only that it is.
    float held_ms_ = 0.0f;
    float last_prob_;   // most recent frame probability (debug)
    int   target_ms_;
};

} // namespace athena
