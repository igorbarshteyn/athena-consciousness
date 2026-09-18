// athena_prosody.h — the audio half of r21-B.
//
// talk-llama decides how she should sound (athena_voice.h: her settled voice,
// plus what her state is doing to it, plus whatever she has chosen to hold) and
// writes one line to a sidecar. This reads that line and applies it to the
// finished waveform, between the SNAC decoder and ALSA.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY THIS IS THE SAFE PLACE TO DO IT
//
// Everything here runs DOWNSTREAM of the autoregressive model, on PCM that has
// already been decoded. It cannot garble: the failure mode of a wrong number is
// "she sounds odd", recoverable by changing the number, not "the token stream
// walked off the codebook manifold". That distinction is why the earlier
// attempts at this — which moved Orpheus's sampler, including the
// repetition_penalty that its own README documents as required >= 1.1 — produced
// audio that did not resemble speech, and why nothing in this file touches
// generation.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHAT IT DOES
//
//   rate       speaking tempo, pitch preserved            WSOLA time-stretch
//   semitones  pitch, duration preserved                  resample + restretch
//   gain_db    loudness                                   a multiply
//   pause_ms   the breath between chunks                  handled by the caller
//
// Rate and pitch are one pipeline, because doing either alone with a resampler
// changes the other. To move pitch by P semitones and tempo by R:
//
//   1. resample by pitch_ratio = 2^(P/12)   -> pitch x pitch_ratio, duration / pitch_ratio
//   2. WSOLA-stretch by pitch_ratio / R     -> duration back to input / R
//
// At this file's ranges (R in [0.90, 1.08], P in [-1, +1]) the stretch factor
// stays inside [0.87, 1.18], which is comfortably where WSOLA sounds clean.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE ACCOUNTING CONSTRAINT — read this before changing anything
//
// orpheus-speak maps played audio back to a character offset so that a barge-in
// can rebuild the part of her turn she actually said:
//
//     pos = chunk.text.size() * played_samples / chunk.pcm_total
//
// `pcm_total` counts samples the DECODER produced. The moment a time-stretch
// exists, samples written to the sink are no longer samples the decoder
// produced, and the ratio is wrong by exactly the stretch factor — every
// interruption would rebuild her committed turn from the wrong place, in the
// machinery that has already produced one invalid-UTF-8 bug.
//
// The fix is not to rescale pcm_total. It is to keep the mapping entirely in
// INPUT-sample space: process() reports how many input samples a call consumed
// alongside how many output samples it produced, the caller records both, and
// the character mapping uses the input figure while playback timing uses the
// output one. The `spc` samples-per-character fallback then needs no rate
// correction either, because it is already in input-sample space.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace aprs {

struct Setting {
    float rate      = 1.00f;
    float semitones = 0.0f;
    int   pause_ms  = 0;      // 0 here, not 70: an absent sidecar must be stock
    float gain_db   = 0.0f;

    bool identity() const {
        return std::fabs(rate - 1.0f) < 1e-4f &&
               std::fabs(semitones)   < 1e-4f &&
               std::fabs(gain_db)     < 1e-4f;
    }
};

// The same bands athena_voice.h enforces, re-enforced here. Two processes, one
// contract: a sidecar written by something else, or by a half-finished write,
// must not be able to ask for a voice outside what was validated.
static inline Setting clamp_setting(Setting s) {
    auto cl = [](float v, float lo, float hi) {
        if (!std::isfinite(v)) return lo > 0.0f ? 1.0f : 0.0f;   // rate -> 1, others -> 0
        return v < lo ? lo : (v > hi ? hi : v);
    };
    s.rate      = cl(s.rate, 0.90f, 1.08f);
    s.semitones = cl(s.semitones, -1.0f, 1.0f);
    s.gain_db   = cl(s.gain_db, -1.5f, 1.5f);
    s.pause_ms  = s.pause_ms < 0 ? 0 : (s.pause_ms > 320 ? 320 : s.pause_ms);
    return s;
}

// Parses the one line athena_voice.h::render_control writes. Tolerant: an
// unknown key is skipped, a malformed value leaves that field default, and a
// line that is not a control line at all returns false. The caller treats every
// failure as "no setting", which is stock behaviour — the feature fails OFF.
static inline bool parse_control(const std::string &line, Setting &out) {
    if (line.compare(0, 3, "v1 ") != 0) return false;
    Setting s;
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
    out = clamp_setting(s);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Stretcher — streaming resample + WSOLA, one instance per chunk.
//
// Per chunk rather than per session because chunks are decoded independently
// and the player pulls them in order; carrying overlap state across a boundary
// would smear the end of one sentence into the start of the next, which is the
// opposite of the breath this feature is adding.
//
// Streaming matters: the player pulls ~200 ms slices precisely so a barge-in is
// noticed within a couple of hundred milliseconds, and buffering a whole chunk
// to stretch it would give that back.
// ═════════════════════════════════════════════════════════════════════════════
class Stretcher {
  public:
    void configure(const Setting &s, int sample_rate) {
        cfg_  = clamp_setting(s);
        rate_ = sample_rate > 0 ? sample_rate : 24000;
        pitch_ratio_ = std::pow(2.0f, cfg_.semitones / 12.0f);
        stretch_     = pitch_ratio_ / (cfg_.rate > 0.01f ? cfg_.rate : 1.0f);
        gain_        = std::pow(10.0f, cfg_.gain_db / 20.0f);

        // 30 ms frame, half-overlap. Long enough to carry a pitch period at a
        // speaking f0 (a 90 Hz voice is 11 ms), short enough that the
        // correlation search stays cheap at 24 kHz.
        frame_ = (size_t) (rate_ * 0.030);
        if (frame_ < 128) frame_ = 128;
        if (frame_ % 2) frame_++;
        // Hann at 50% overlap sums to unity, so the SYNTHESIS hop is fixed at
        // half a frame and the stretch lives entirely in how fast the analysis
        // window walks the input. Getting these the wrong way round inverts the
        // effect — asking her to slow down speeds her up — which is exactly
        // what the first version of this did, and what the duration-ratio check
        // in the prosody test caught.
        hop_out_ = frame_ / 2;
        hop_in_  = (size_t) std::lround((double) hop_out_ / (double) stretch_);
        if (hop_in_ < 1) hop_in_ = 1;
        search_  = (size_t) (rate_ * 0.005);   // +/- 5 ms

        window_.resize(frame_);
        for (size_t i = 0; i < frame_; i++)
            window_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * (float) i / (float) frame_);

        reset();
    }

    void reset() {
        in_.clear(); out_.clear(); tail_.assign(hop_out_, 0.0f);
        first_frame_ = true;
        read_pos_ = 0.0; hist_.assign(4, 0.0f); primed_ = false;
        pending_in_ = 0;
    }

    bool identity() const { return cfg_.identity(); }
    // 14.1: gain alone is a MULTIPLY — the header's own promise. Routing a
    // gain-only settle through cubic resample at ratio 1.0 + WSOLA at stretch
    // 1.0 added a faint phasey coloration to every utterance where only
    // loudness moved. Time/pitch untouched → the fast path applies, with the
    // gain factor it already implements.
    bool passthrough_() const {
        return std::fabs(cfg_.rate - 1.0f) < 1e-4f &&
               std::fabs(cfg_.semitones)   < 1e-4f;
    }

    // Feed `n` input samples; append output to `out`. Returns the number of
    // INPUT samples this call is accounted as having consumed — which for the
    // streaming case is simply `n`, since everything not yet emitted is held in
    // internal state and will be emitted by a later call or by flush(). The
    // caller pairs that with out.size() growth for the two-space accounting the
    // header describes.
    size_t process(const float *pcm, size_t n, std::vector<float> &out) {
        if (passthrough_()) {
            if (gain_ != 1.0f) { for (size_t i = 0; i < n; i++) out.push_back(pcm[i] * gain_); }
            else               { out.insert(out.end(), pcm, pcm + n); }
            return n;
        }
        resample_into_(pcm, n);
        emit_(out, /*drain_all=*/false);
        return n;
    }

    // Everything still held. Called when a chunk is finished so its tail is not
    // swallowed — without this the last ~30 ms of every sentence would vanish.
    void flush(std::vector<float> &out) {
        if (passthrough_()) return;
        emit_(out, /*drain_all=*/true);
        if (!in_.empty()) {
            // ── r20p3.14.7 (RA15): window the junction, don't butt-splice ──────
            // emit_ left tail_ holding the falling half of the last frame's Hann
            // window, expecting the next frame to overlap-add against it. flush
            // instead dropped the RAW leftover in_ straight after — a hard step
            // measured at 5x–36x the signal's own largest sample-to-sample delta,
            // i.e. an audible click at the end of every chunk whenever prosody is
            // doing anything (stretch_ != 1). Overlap-add the leftover's rising
            // portion against tail_ (Hann at 50% overlap sums to unity, so the
            // seam is continuous), then pass any remainder through so the last
            // syllable is neither dropped nor clicked.
            if (first_frame_) {
                // No frame was emitted (chunk shorter than one frame): tail_ is
                // still the zero vector, so windowing against it would FADE IN
                // the only audio there is — the RA5 defect. Pass through raw.
                for (float v : in_) out.push_back(v * gain_);
            } else {
                const size_t n = std::min(hop_out_, in_.size());
                for (size_t i = 0; i < n; i++)
                    out.push_back((tail_[i] + in_[i] * window_[i]) * gain_);
                for (size_t i = n; i < in_.size(); i++)
                    out.push_back(in_[i] * gain_);
            }
            in_.clear();
        }
    }

  private:
    // Cubic (Catmull-Rom) resampling. Linear is audibly gritty on voiced speech
    // even at these small ratios, and cubic costs four multiplies.
    void resample_into_(const float *pcm, size_t n) {
        if (n == 0) return;
        if (!primed_) {
            hist_.assign(4, pcm[0]);
            primed_ = true;
        }
        for (size_t i = 0; i < n; i++) {
            hist_[0] = hist_[1]; hist_[1] = hist_[2]; hist_[2] = hist_[3]; hist_[3] = pcm[i];
            while (read_pos_ < 1.0) {
                const float t = (float) read_pos_;
                const float a = hist_[0], b = hist_[1], c = hist_[2], d = hist_[3];
                const float v = b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d
                                                             + t * (3.0f * (b - c) + d - a)));
                in_.push_back(v);
                read_pos_ += (double) pitch_ratio_;
            }
            read_pos_ -= 1.0;
        }
    }

    // WSOLA: for each output hop, find the analysis offset within +/- search_
    // whose frame best continues what was just written, then Hann overlap-add.
    // The correlation search is what keeps pitch periods aligned across the
    // splice; plain OLA at these ratios produces the characteristic warble.
    void emit_(std::vector<float> &out, bool drain_all) {
        const size_t need = frame_ + search_ * 2;
        while (in_.size() >= need || (drain_all && in_.size() >= frame_)) {
            const size_t max_off = in_.size() >= frame_ ? std::min(search_ * 2, in_.size() - frame_) : 0;
            size_t best = 0;
            if (max_off > 0) {
                float best_score = -1e30f;
                // Match against the second half of the previous synthesis frame
                // — the part about to be overlapped. This is what keeps pitch
                // periods aligned across the splice; plain OLA at these ratios
                // gives the characteristic warble.
                for (size_t off = 0; off <= max_off; off += 2) {
                    float sc = 0.0f;
                    for (size_t i = 0; i < hop_out_; i += 4)
                        sc += tail_[i] * in_[off + i];
                    if (sc > best_score) { best_score = sc; best = off; }
                }
            }
            // r20p3.14 (RA5): the FIRST frame of a chunk is emitted unwindowed.
            // tail_ starts at zero, so overlap-adding against it multiplied the
            // opening hop_out_ samples by the rising half of a Hann window — a
            // measured 0.38x energy over the first 15 ms of every chunk, i.e. a
            // fade-in on the first phoneme of every sentence, present whenever
            // the feature is doing anything at all. The tail_ write below is
            // unchanged, so the second frame's overlap still sums to unity.
            if (first_frame_) {
                for (size_t i = 0; i < hop_out_; i++)
                    out.push_back(in_[best + i] * gain_);
                first_frame_ = false;
            } else {
                for (size_t i = 0; i < hop_out_; i++)
                    out.push_back((tail_[i] + in_[best + i] * window_[i]) * gain_);
            }
            for (size_t i = 0; i < hop_out_; i++)
                tail_[i] = in_[best + hop_out_ + i] * window_[hop_out_ + i];

            // The nominal analysis position advances by hop_in_, NOT by
            // best + hop_in_: `best` is a per-iteration alignment tolerance, and
            // accumulating it would let the analysis position drift away from
            // the nominal one and take the duration with it.
            const size_t adv = std::min(in_.size(), hop_in_);
            in_.erase(in_.begin(), in_.begin() + (long) adv);
        }
    }

    Setting cfg_;
    int     rate_ = 24000;
    float   pitch_ratio_ = 1.0f, stretch_ = 1.0f, gain_ = 1.0f;
    size_t  frame_ = 720, hop_out_ = 360, hop_in_ = 360, search_ = 120;
    std::vector<float> window_, in_, out_, tail_, hist_;
    double  read_pos_ = 0.0;
    bool    primed_ = false;
    bool    first_frame_ = true;
    size_t  pending_in_ = 0;
};

} // namespace aprs
