// silero-endpointer.h — streaming end-of-turn detector backed by whisper.cpp's
// built-in Silero VAD (whisper_vad_* API). Owns a whisper_vad_context, feeds
// captured audio frame-aligned through whisper_vad_detect_speech_no_reset()
// (LSTM state preserved across calls), reads the per-frame probability stream,
// and drives the pure SileroTurnState machine to decide turn-end.
//
// Verified against whisper.cpp (include/whisper.h, src/whisper.cpp):
//   - whisper_vad_detect_speech_no_reset(ctx, samples, n): streaming inference
//   - whisper_vad_reset_state(ctx): clear LSTM between utterances
//   - whisper_vad_n_probs(ctx) / whisper_vad_probs(ctx): per-call frame probs
//     (probs.resize(n_chunks) each call -> per-call, not cumulative)
//   - n_probs == ceil(n_samples / n_window); Silero @16kHz uses n_window=512
//     (32 ms/frame). We feed exact multiples of the window (carry remainder)
//     so the last frame is never zero-padded; the actual window is re-derived
//     at runtime and a mismatch is reported.
//
// Depends only on whisper.h (+ the pure header). The audio source and the
// SDL poll are injected as template params so this header stays decoupled from
// common-sdl.

#pragma once

#include "silero-turn-state.h"

#include "whisper.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace athena {

// r24.20 review (VISION #2 / SPEECH F5 — law 10): the capture-cursor API is
// the overlay's extension of stock whisper.cpp's audio_async. Detect it on the
// ring TYPE, so this template compiles against the pinned stock header (no
// cursor: the r24.17 latest-window read, which is exactly the
// ATHENA_VAD_CAPTURE_CURSOR=0 arm) and against the overlay's (cursor present;
// the switch decides). A fixture's fake ring that supplies the two methods is
// taken at its word, as before.
template <class T, class = void>
struct ring_has_capture_cursor : std::false_type {};
template <class T>
struct ring_has_capture_cursor<T, std::void_t<
    decltype(std::declval<T &>().get_with_cursor(0, std::declval<std::vector<float> &>())),
    decltype(std::declval<T &>().get_since(uint64_t{0}, std::declval<std::vector<float> &>()))>>
    : std::true_type {};

// Quiet whisper's INFO/DEBUG chatter (whisper_vad_detect_speech_no_reset emits
// an INFO line every call — a flood at streaming cadence). WARN/ERROR always
// pass; INFO/DEBUG only when verbose. Installed once at init().
inline bool& silero_log_verbose() { static bool v = false; return v; }
inline void  silero_log_cb(ggml_log_level level, const char* text, void* /*ud*/) {
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN || silero_log_verbose()) {
        fputs(text, stderr);
    }
}

class SileroEndpointer {
public:
    struct Config {
        std::string      model_path;        // ggml-silero-v6.2.0.bin (required)
        bool             use_gpu    = false; // Silero runs on CPU: whisper.cpp defaults
                                            // VAD to CPU and the GPU graph won't schedule
                                            // (the model is 0.88 MB / a 128-unit LSTM, so
                                            // CPU is microseconds per frame).
        int              gpu_device = 0;
        int              n_threads  = 4;
        SileroTurnConfig turn;              // threshold / min_speech_ms / silence_ms
        int              poll_ms    = 100;  // capture + inference cadence
        bool             verbose    = false; // pass whisper VAD INFO logs through the filter
        bool             install_log_filter = true; // silence per-call streaming INFO spam
        bool             debug      = false; // per-poll [silero-dbg] trace (prob + accumulators)
    };

    SileroEndpointer() = default;
    ~SileroEndpointer() { free(); }
    SileroEndpointer(const SileroEndpointer&)            = delete;
    SileroEndpointer& operator=(const SileroEndpointer&) = delete;

    // Returns false if no model path or the model fails to load — caller should
    // then fall back to the energy VAD.
    bool init(const Config& cfg) {
        free();
        cfg_ = cfg;
        if (cfg_.model_path.empty()) return false;

        whisper_vad_context_params cp = whisper_vad_default_context_params();
        cp.use_gpu    = cfg_.use_gpu;   // CPU by default (see Config) -- GPU VAD aborts
        cp.gpu_device = cfg_.gpu_device;
        cp.n_threads  = cfg_.n_threads;

        vctx_ = whisper_vad_init_from_file_with_params(cfg_.model_path.c_str(), cp);
        if (!vctx_) return false;

        // Installed AFTER the load so the one-time model banner prints, but the
        // per-call INFO from whisper_vad_detect_speech_no_reset is suppressed
        // during streaming (WARN/ERROR always pass; INFO/DEBUG only if verbose).
        if (cfg_.install_log_filter) {
            silero_log_verbose() = cfg_.verbose;
            whisper_log_set(silero_log_cb, nullptr);
        }

        state_       = SileroTurnState(cfg_.turn);
        carry_.clear();
        win_         = kSileroWindow;
        win_ms_      = 1000.0f * (float) kSileroWindow / (float) WHISPER_SAMPLE_RATE;
        win_checked_ = false;
        // 14.1: a re-init with a good model is a fresh start — the R5-AF
        // failure latch survived free()/init(), so a recovered VAD stayed
        // reported not-ready forever (and the first later hiccup re-tripped
        // an instant give-up off the stale counter).
        vad_fail_run_ = 0;
        vad_failed_   = false;
        return true;
    }

    // r21-full.5 (R5-AF): ...and still answering. Once the VAD has failed for a
    // second straight the endpointer reports not-ready, and the main loop's
    // existing `params.vad_engine == "silero" && g_silero.ready()` test routes
    // the rest of the session down the energy path instead of wedging here.
    bool ready() const { return vctx_ != nullptr && !vad_failed_; }

    void free() {
        if (vctx_) { whisper_vad_free(vctx_); vctx_ = nullptr; }
        carry_.clear();
    }

    // Feed one captured audio chunk (must be WHISPER_SAMPLE_RATE mono f32).
    // Buffers a sub-frame remainder so successive chunks tile the window grid
    // exactly. Returns the aggregate turn-state step for the frames consumed.
    SileroTurnState::Step feed(const float* samples, int n) {
        final_endpoint_ = false;
        SileroTurnState::Step none{ state_.phase() == SileroTurnState::Phase::InSpeech, false, false };
        if (!vctx_ || n <= 0) return none;

        carry_.insert(carry_.end(), samples, samples + n);
        const int nframes = (int) carry_.size() / win_;
        if (nframes <= 0) return none;

        const int feed_n = nframes * win_;
        if (!whisper_vad_detect_speech_no_reset(vctx_, carry_.data(), feed_n)) {
            carry_.erase(carry_.begin(), carry_.begin() + feed_n);
            // ── r21-full.5 (R5-AF): a failing VAD must degrade, not wedge ────
            // On failure the state machine is not advanced at all, so
            // `silence_ms_` can never reach the target and wait_for_endpoint
            // blocks in a 10 Hz loop indefinitely. There was no counter, no
            // fallback and no log line of the endpointer's own, so a
            // persistently failing VAD context is indistinguishable from "he
            // never stopped talking": she goes permanently unresponsive with a
            // silent terminal. After a second of consecutive failures, say so
            // once, synthesise an endpoint so the turn can complete, and report
            // not-ready so the main loop's existing energy path takes over for
            // the rest of the session.
            if (++vad_fail_run_ >= 10) {
                if (!vad_failed_) {
                    vad_failed_ = true;
                    fprintf(stderr, "[silero] the VAD stopped answering (%d consecutive "
                                    "failures) — falling back to the energy endpointer "
                                    "for the rest of this session\n", vad_fail_run_);
                }
                SileroTurnState::Step giveup{ false, false, true };
                return giveup;
            }
            return none;
        }
        vad_fail_run_ = 0;

        const int    np = whisper_vad_n_probs(vctx_);
        const float* p  = whisper_vad_probs(vctx_);

        if (!win_checked_ && np > 0) {
            const int obs = feed_n / np;                 // actual samples/frame
            if (obs != win_) {
                fprintf(stderr, "[silero] model window %d samples != assumed %d; "
                                "using %d (%.1f ms/frame)\n",
                        obs, win_, obs, 1000.0f * obs / (float) WHISPER_SAMPLE_RATE);
                win_ms_ = 1000.0f * (float) obs / (float) WHISPER_SAMPLE_RATE;
                // NOTE: carry was aligned to the assumed window; this is exact
                // when obs divides the assumed window (e.g. 256|512). A window
                // that is not a divisor would need re-alignment — not the case
                // for Silero @16kHz (512).
            }
            win_checked_ = true;
        }

        last_max_prob_ = 0.0f;
        for (int i = 0; i < np; ++i) if (p[i] > last_max_prob_) last_max_prob_ = p[i];

        SileroTurnState::Step agg = (np > 0 && p) ? state_.push_frames(p, np, win_ms_, &final_endpoint_) : none;
        carry_.erase(carry_.begin(), carry_.begin() + feed_n);
        return agg;
    }

    // Block until the user's utterance ends (true) or shutdown/Ctrl-C (false).
    //   audio      : object with .get(ms, std::vector<float>&) (e.g. audio_async)
    //   is_running : refreshed from poll(); loop exits when it goes false
    //   poll       : pump UI events, return is_running (e.g. sdl_poll_events)
    //   on_trailing: optional; called once when speech first turns to silence,
    //                returns the silence target (ms) for this utterance
    //                (prosody hook). Return <=0 to keep the configured default.
    template <class AudioT, class PollFn>
    // `should_abort` lets the caller break out of a silence that would
    // otherwise block forever. ATHENA uses it so her volition accumulator can
    // reach threshold and let her speak first — without it, an assistant that
    // decides to say something has no way to ever get the floor, because the
    // endpoint wait only ever returns when SOMEONE ELSE has spoken. Defaulted
    // and unused by every other caller.
    bool wait_for_endpoint(AudioT& audio, bool& is_running, PollFn poll,
                           const std::function<int()>& on_trailing = {},
                           const std::function<bool()>& should_abort = {},
                           // r16: called once per poll AFTER the state update,
                           // for side effects that must not abort the wait —
                           // the seam's backchannel hook. Must not block.
                           const std::function<void()>& on_listen = {}) {
        if (!vctx_) return false;

        state_.reset();
        whisper_vad_reset_state(vctx_);
        carry_.clear();

        // ── r19.6 (C53): do not let the abort hook blind the endpointer ──────
        //
        // One poll delivers poll_ms (100 ms) of audio = 3 frames of 512 samples
        // = 96 ms of run, and `min_speech_run_ms` is 100. Speech is therefore
        // NEVER confirmed on the first poll of a call — confirmation needs two.
        // The state is reset at the top of every call, so once the caller's
        // abort predicate starts returning true on every poll (which is exactly
        // what an autonomous urge does: `Volition::urge` is a latch, cleared
        // only by acting on it), each call became reset → one 96 ms poll →
        // abort. `run_ms_` could never reach 100, the phase could never leave
        // WaitingForSpeech, and the endpointer went permanently deaf.
        //
        // The consequence is not a missed abort, it is a lost turn: the caller's
        // hook asks a DIFFERENT and much stricter question once the phase is
        // InSpeech (`wants_to_interject`, plus four seconds of speech), and that
        // question was never reachable. He talks; no endpoint ever fires; the
        // moment he pauses long enough for `room_is_quiet`, she takes the floor
        // unprompted — and the autonomous path skips transcription and then
        // clears the ring, so what he said is destroyed with no record of it.
        //
        // Fix: the hook is not consulted until enough audio has been fed for the
        // phase to mean something. Two polls, so a real onset is confirmed and
        // `InSpeech` is visible before any decision is taken on it. The cost is
        // at most one extra poll of latency on an autonomous turn in a silent
        // room; the benefit is that the phase gate the hook is written against
        // is actually reachable.
        // ── r21-full.5 (R5-AG): count POLLS, which is what the text says ────
        // `abort_arm_ms = 2 * poll_ms + 1` against a `fed_ms` accumulated from
        // sample counts: audio.get(100) returns exactly 1600 samples, i.e.
        // exactly 100.0 ms, so after two polls fed_ms is 200.0 and 200.0 >= 201.0
        // is false — arming happened on poll THREE, not two. That is 100 ms
        // added to every autonomous turn, and the whole gate hinged on
        // floating-point equality with the capture granularity: a device
        // returning 1599 samples per poll would push arming out permanently,
        // one returning 1601 would pull it in. Counting polls is exactly the
        // property the comment above describes and is immune to both. `fed_ms`
        // stays for the empty-chunk stall credit below, which genuinely is
        // about elapsed time rather than about polls of audio.
        const int  abort_arm_polls = 2;
        int        polls_fed = 0;
        double fed_ms = 0.0;
        auto may_abort = [&]() {
            return should_abort && polls_fed >= abort_arm_polls && should_abort();
        };

        std::vector<float> chunk;
        // r24.18: sleep(poll_ms) is followed by inference and listen hooks.
        // Reading only the newest poll_ms next time skipped every sample
        // captured during that work, and replayed the old window on a stalled
        // source. The callback's monotonically counted ranges make each
        // retained sample belong to one feed, regardless of scheduler delay.
        // The first read is the same window as before. Ring overwrite remains
        // possible during a long stall and is reported, never called silence.
        // ATHENA_VAD_CAPTURE_CURSOR=0 restores latest-window polling exactly.
        // r24.20 review (law 10): [[maybe_unused]] because against a stock
        // ring the cursor branch below is discarded at compile time.
        [[maybe_unused]] static const bool capture_cursor_on = [] {
            const char *e = std::getenv("ATHENA_VAD_CAPTURE_CURSOR");
            return !(e && e[0] == '0');
        }();
        [[maybe_unused]] bool have_capture_cursor = false;
        [[maybe_unused]] uint64_t capture_cursor = 0;
        while (is_running) {

            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.poll_ms));
            is_running = poll();
            if (!is_running) return false;

            bool cursor_read = false;
            if constexpr (ring_has_capture_cursor<AudioT>::value) {
                if (capture_cursor_on) {
                    const auto captured = have_capture_cursor
                        ? audio.get_since(capture_cursor, chunk)
                        : audio.get_with_cursor(cfg_.poll_ms, chunk);
                    if (have_capture_cursor && captured.begin > capture_cursor) {
                        fprintf(stderr, "[silero] microphone history gap: %llu samples were no longer retained\n",
                                (unsigned long long)(captured.begin - capture_cursor));
                        // A partial VAD frame must not join samples from opposite
                        // sides of a capture gap. The turn state retains the speech
                        // already observed; a missing range supplies no new evidence.
                        carry_.clear();
                        whisper_vad_reset_state(vctx_);
                        state_.note_capture_gap();
                    }
                    capture_cursor = captured.end;
                    have_capture_cursor = true;
                    cursor_read = true;
                }
            }
            if (!cursor_read) {
                audio.get(cfg_.poll_ms, chunk);   // stock ring, or ATHENA_VAD_CAPTURE_CURSOR=0
            }
            if (chunk.empty()) {
                // Still honour the abort during a stretch with no new samples —
                // otherwise a caller whose audio source stalls could hold the
                // floor closed forever. A stall delivers no audio, so the arming
                // gate is credited by TIME here rather than by samples: the
                // failure this guards against is a silent source, not a talker.
                fed_ms += (double) cfg_.poll_ms;
                if (fed_ms >= (double) (abort_arm_polls * cfg_.poll_ms)) polls_fed = abort_arm_polls;
                if (may_abort()) return false;
                continue;
            }

            fed_ms += 1000.0 * (double) chunk.size() / (double) WHISPER_SAMPLE_RATE;
            if (polls_fed < 1000000) polls_fed++;   // R5-AG
            const SileroTurnState::Step s = feed(chunk.data(), (int) chunk.size());


            if (cfg_.debug) {
                fprintf(stderr, "main: [silero-dbg] %-8s maxp=%.2f speech=%.0f sil=%.0f run=%.0f tgt=%d\n",
                        SileroTurnState::phase_name(state_.phase()), last_max_prob_,
                        state_.speech_ms(), state_.silence_ms(), state_.run_ms(),
                        state_.silence_target_ms());
            }

            if (s.entered_trailing && on_trailing) {
                state_.set_silence_target_ms(on_trailing());
            }
            if (on_listen) on_listen();   // r16: non-blocking side effects only
            // A delayed read may contain a complete pause AND resumed speech.
            // push_frames reports whether an endpoint occurred anywhere in
            // that block; the decision to answer must describe its latest
            // frame, including a still-unconfirmed resumption held by the
            // existing hysteresis. Otherwise recovering skipped audio would
            // itself introduce a new cutoff. The current target also respects
            // a prosody hook that just extended this pause. OFF keeps the old
            // aggregate flag.
            // ── r24.20 review · COMPOSE-5 (INTEGRATION F3 × SPEECH F7) ──────
            // Deliberately NOT inside the `if constexpr` above: this is a
            // decision about the frames of one block, and a block has several
            // frames whether it was read through the cursor or as the newest
            // window (a 100 ms poll is three 32 ms frames). Against a stock
            // common-sdl.h the READ below falls back to r24.17's latest window
            // and this rule stays live on the same switch — the deferral is the
            // only thing between a 1–3-frame block-ending speech run and an
            // endpoint fired on top of it (SPEECH F7; the S6/S17 clipped-capture
            // class), and compiling it out with the cursor would have taken it
            // away from exactly the build that has no backlog recovery either.
            // So a stock build is not "all r24.17 here": ATHENA_VAD_CAPTURE_CURSOR
            // still decides this line, and =0 is still r24.14's aggregate flag.
            // The startup line in talk-llama.cpp says so.
            const bool endpoint_now = capture_cursor_on
                ? (s.endpoint && (vad_failed_ || (final_endpoint_ &&
                   state_.silence_ms() >= (float)state_.silence_target_ms())))
                : s.endpoint;
            if (endpoint_now) {
                if (cfg_.debug)
                    fprintf(stderr, "main: [silero-dbg] ENDPOINT (speech=%.0fms silence=%.0fms)\n",
                            state_.speech_ms(), state_.silence_ms());
                return true;
            }

            // Checked AFTER this iteration's audio has been fed, not before.
            // The caller's abort predicate reads state_.phase() to avoid
            // seizing the floor while the user is speaking — checked first,
            // that phase was one poll stale, leaving a 100-200 ms window where
            // an urge could fire exactly as the user drew breath and began.
            // After the feed, the phase reflects the audio just captured.
            if (may_abort()) return false;
        }
        return false;
    }


    const SileroTurnState& state() const { return state_; }

private:
    // Silero v4/v5/v6 process 512-sample windows at 16 kHz (32 ms/frame);
    // re-derived at runtime in feed() and reported if a model differs.
    static constexpr int kSileroWindow = 512;

    Config              cfg_{};
    whisper_vad_context* vctx_ = nullptr;
    SileroTurnState     state_{};
    int  vad_fail_run_ = 0;   // R5-AF: consecutive VAD failures
    bool vad_failed_   = false; // latched: the VAD stopped answering
    bool final_endpoint_ = false; // final frame's decision, not the block's OR
    std::vector<float>  carry_;
    float               last_max_prob_ = 0.0f; // max prob of the last chunk (debug)
    int                 win_         = kSileroWindow;
    float               win_ms_      = 1000.0f * (float) kSileroWindow / 16000.0f;
    bool                win_checked_ = false;
};

} // namespace athena
