// athena_emotion.h — the emotion2vec speech-emotion tagger, shared.
//
// r19.6: extracted verbatim from talk-llama.cpp so that talk-llama AND the
// calibrator (athena-emotion-calibrate.cpp) run THE SAME CODE. A calibrator
// that reimplements the selection rule, the per-class floors or the ORT session
// setup is calibrating something other than the thing being calibrated — the
// floors it derives would be right for itself and wrong for her. There is one
// copy of this logic and both binaries link it.
//
// Compiled with the model only when ATHENA_EMOTION_ORT is defined (CMake sets it
// when ONNXRUNTIME_ROOT is given); otherwise tag() is a no-op returning "" and
// the baseline is byte-identical.
#pragma once
#include "athena_evidence.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef ATHENA_EMOTION_ORT
#include <onnxruntime_c_api.h>
#endif

// ── r19.7 (C66): an env FLAG reads its VALUE, not merely its presence ────────
//
// These were `getenv(...) != nullptr`, so `ATHENA_EMOTION_COLLAPSE_NEGATIVE=0`
// — written by someone who meant to turn it OFF — turned it ON. Found live in
// the S7 launcher: the production profile sets it to 0 and then defines the
// per-class floors that only make sense with collapse disabled. It cost nothing
// in S7 because no negative class ever cleared its floor, so nothing was
// mislabelled; the first time `sad` or `angry` fires she would be handed
// "[emotion: negative]" instead of the label the calibration was run to
// produce.
//
// `=1`, `=true`, `=yes`, `=on` and bare presence with any other value all stay
// enabled, so every existing launcher keeps working.
inline bool athena_env_flag(const char *name, bool dflt = false) {
    const char *v = std::getenv(name);
    if (!v) return dflt;
    if (!*v) return false;                       // set-but-empty is off
    const char c = (v[0] >= 'A' && v[0] <= 'Z') ? (char) (v[0] - 'A' + 'a') : v[0];
    if (c == '0' || c == 'f' || c == 'n') return false;   // 0 / false / no
    if (c == 'o' && (v[1] == 'f' || v[1] == 'F')) return false;   // off
    return true;
}

// r20p3.14.7 (EE1): a numeric env read that treats set-but-empty and
// non-numeric as UNSET, exactly as athena_env_flag treats set-but-empty as off.
// The floors below were read with `getenv() != nullptr` + atof(), and atof("")
// is 0.0 — so `export ATHENA_EMOTION_MIN=` (a value edited out while retuning)
// drove EVERY class floor to 0.0, and because that path also counts as "base
// set" it skipped the happy/sad safety defaults too. The result is a tag on
// every single utterance — a flat "pass the salt" scored as its argmax class —
// which then feeds memory salience, voice and the field. Returns true only for
// a value that parses cleanly and wholly as a finite number; otherwise the
// caller keeps its default, which is the honest reading of a blank knob.
inline bool athena_env_float(const char *name, float &out) {
    const char *v = std::getenv(name);
    if (!v || !*v) return false;                 // unset or set-but-empty
    char *e = nullptr;
    const float f = std::strtof(v, &e);
    if (e == v || *e != '\0') return false;      // non-numeric or trailing junk
    if (!(f == f) || f > 1e30f || f < -1e30f) return false;  // NaN / inf guard
    out = f;
    return true;
}

// ── r24.12 (WO-C7 / S22 §C.1.9): the runtime clip is bounded to its LAST N s ─
// tag() built a rank-1 tensor of the WHOLE clip, and the only bound on that
// clip was MIN_SAMPLES from below. S22 18:46:02: the outage tape (56 s) plus
// the live window (25 s) — 81 s, 1.3 M samples — went to emotion2vec in one
// piece and ORT failed inside the model's frontend on a 2.03 GB `Expand`
// allocation ("/AUDIO/Expand … BFCArena … Failed to allocate memory for
// requested buffer of size 2026552576"): an attention-shaped buffer,
// super-linear in the frame count, on a GPU already hosting the LLM, whisper
// and Orpheus. Across both S22 sessions 112 tagger decisions succeeded and the
// one that failed was that replay; no clip under 25 s ever failed.
//
// The bound is the calibrator's own: athena_calib.h's grade_take refuses a
// take over 15 s ("too long — one sentence is enough"), so every per-class
// floor was derived from 1.2–15 s clips and was being applied to an 81 s one —
// the same parity argument r19.8 (F4) made for trimming to the speech span.
// The LAST fifteen seconds, because what he said last is what she is
// answering and the end of an utterance carries its tone. probe() — the
// calibrator's path — is untouched: its takes are <= 15 s by construction.
// ATHENA_EMOTION_MAX_S: default 15; 0 = unbounded = r24.11. EE1 through
// athena_env_float: set-but-empty, non-numeric or outside [0, 600] keeps 15.
inline float emotion_max_s_read() {
    float f = 0.0f;
    if (!athena_env_float("ATHENA_EMOTION_MAX_S", f)) return 15.0f;
    return (f >= 0.0f && f <= 600.0f) ? f : 15.0f;
}
inline size_t emotion_clip_cap_samples(int sample_rate) {
    static const float s = emotion_max_s_read();
    if (s <= 0.0f || sample_rate <= 0) return 0;          // 0 = unbounded (r24.11)
    return (size_t) ((double) sample_rate * (double) s + 0.5);
}
// Pure: the tail of `pcm` that fits `cap_samples` (the whole clip when it
// already does, or when the cap is 0). This is what tag() hands the model.
inline std::vector<float> emotion_clip_window(const std::vector<float> &pcm, size_t cap_samples) {
    if (cap_samples == 0 || pcm.size() <= cap_samples) return pcm;
    return std::vector<float>(pcm.end() - (std::ptrdiff_t) cap_samples, pcm.end());
}


// ─────────────────────────────────────────────────────────────────────────────
// emotion2vec speech-emotion tagger — init once, classify each utterance.
// Loads emotion2vec_plus_large.onnx (exported from FunASR; waveform→9 softmax,
// waveform standardization baked in) and runs it on the same pcmf32 buffer
// Whisper sees (16 kHz mono, no resample). Mirrors orpheus-speak's SnacDecoder
// ORT-C-API pattern: DisableCpuMemArena (the input is dynamic-length), CUDA EP
// with per-Run arena shrinkage, graceful CPU fallback. A confident, non-neutral
// emotion yields a "[emotion: <label>]" tag; everything else yields "".
// Compiled only when built with -DATHENA_EMOTION_ORT (set by CMake when
// ONNXRUNTIME_ROOT is provided); otherwise tag() is a no-op returning "".
// ─────────────────────────────────────────────────────────────────────────────
struct EmotionTagger {
    // r24.16: nine tensor elements are not necessarily nine FLOAT scores.
    // A uint8 output passed the old count check, then was read as 36 bytes.
    // =0 keeps the previous count-only contract for exact comparison.
    bool float_output = true;  // ATHENA_EMOTION_FLOAT_OUTPUT
    // Class label order MUST match the ONNX export (tokens.txt): index = class id.
    // Hoisted to a member so init() (floor setup) and tag() (selection) share it.
    static constexpr const char *const LABELS[9] = {
        "angry", "disgusted", "fearful", "happy", "neutral",
        "other", "sad", "surprised", "unk"};
    enum { IDX_ANGRY = 0, IDX_DISGUSTED = 1, IDX_FEARFUL = 2,
           IDX_NEUTRAL = 4, IDX_OTHER = 5, IDX_SAD = 6, IDX_UNK = 8 };  // class ids
    // [NEG-COLLAPSE] The acoustically-indistinct negative set (calibration showed
    // this model can't separate them); folded to one "negative" tag when enabled.
    static bool is_negative_class(int idx) {
        return idx == IDX_ANGRY || idx == IDX_DISGUSTED || idx == IDX_FEARFUL || idx == IDX_SAD;
    }
#ifdef ATHENA_EMOTION_ORT
    const OrtApi *g_ort = nullptr;
    OrtEnv *env = nullptr;
    OrtSessionOptions *opts = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *mem_info = nullptr;
    OrtAllocator *allocator = nullptr;
    OrtRunOptions *run_opts = nullptr;
    char *in_name = nullptr;
    char *out_name = nullptr;
    const char *in_names[1] = {};
    const char *out_names[1] = {};
    bool has_cuda = false;
#endif
    bool ok = false;
    bool debug = false;          // ATHENA_EMOTION_DEBUG=1 -> log every classification
    bool collapse_negative = false;  // [NEG-COLLAPSE] ATHENA_EMOTION_COLLAPSE_NEGATIVE=1 ->
                                     // emit {angry,disgusted,fearful,sad} as [emotion: negative]
    bool run_warned = false;     // log the first Run() failure once, then stay quiet
    // ── r24.12 (WO-C7 / S22 §C.1.9 D12): failures are COUNTED and each is named
    // "Run failed (…) — tagging inert this session" was printed once and then
    // silenced by run_warned, while `ok` was never cleared: the S22 OOM was
    // per-clip, the next short clip tagged fine (18:49:29 happy p=0.610), and
    // the session was not inert at all — but a second OOM would have been
    // invisible. Every failure now prints, with its clip length and this
    // ordinal, and says truthfully that later utterances are still tried.
    int  run_failures = 0;
    // The one per-session condition — an output tensor with fewer than nine
    // scores is a wrong ONNX — now actually makes the tagger inert, as its
    // message always claimed. ATHENA_EMOTION_BAD_ONNX_INERT=0 keeps r24.11's
    // retry-every-utterance.
    bool bad_onnx_inert = true;
    // ── r21-full.8 (R8-A) ────────────────────────────────────────────────────
    // How much of the posterior must be about an emotion at all before the
    // share question is worth asking. 0.10 sits an order of magnitude above the
    // measured noise band (ordinary neutral speech leaves 0.000-0.03 of
    // non-neutral mass) and well below every genuinely expressive utterance in
    // the three-session corpus (0.17-1.00). Env: ATHENA_EMOTION_EVIDENCE.
    float evidence_floor = 0.10f;
    // A backstop under the share rule: a class that owns its (small) evidence
    // outright still has to have been detected at all. Env: ATHENA_EMOTION_NOISE.
    float noise_floor    = 0.02f;
    // ── R21 (F4/S18-4): the share rule's own floor ───────────────────────────
    // R8-A reinterpreted the per-class floors as SHARE thresholds and observed
    // that they "turn out to be sensible share thresholds". Measured over the
    // 81-utterance corpus plus the eight live S18 decisions, they are not:
    // every one of the 21 true emissions owns 0.938-1.000 of the non-neutral
    // mass, while the per-class floors sit at 0.80 (angry, happy) and 0.95
    // (sad) — so for two of the three live classes the floor is below the
    // ENTIRE observed distribution and does no work. S18's single false tag
    // ("angry" on a calm sentence: five minutes of the recorded demo and an
    // existential crisis she did not need to have) owned 0.842. The gap
    // between 0.842 and 0.938 is the whole finding.
    //
    // 0.90 sits in the middle of a stable band: 0.85, 0.88, 0.90 and 0.92 all
    // give byte-identical corpus results (14/81; S12 5, S1 6, S2 3 — the same
    // numbers r23 produces) and all drop exactly the S18 angry. Applied as
    // max(per-class floor, this), so every existing knob still steers, a class
    // whose calibrated floor is higher (sad, 0.95) keeps its own stricter bar,
    // and a class retired by the 1.01 convention is still retired in the pick.
    //
    // NOTE: this REPLACES the evidence-ceiling proposal in the S18 report,
    // which was simulated against this corpus and regresses it — at a ceiling
    // of 0.35 four true emissions are lost, including the two heavy sads
    // (0.852/0.934) that R8-A exists to recover, and S12 falls 5 -> 3, failing
    // the pinned assertion in test_r21_full6.cpp.
    //
    // Env: ATHENA_EMOTION_SHARE. 0 disables the floor and restores r23 exactly.
    //
    // r24.6 (WO-03): 0.90 -> 0.65. At 0.90 this global bar sat ABOVE every
    // per-class floor the launcher sets except sad's (MIN_HAPPY 0.80,
    // MIN_ANGRY 0.80), so `max(floor, share_floor)` discarded both — two
    // calibrated knobs that could not affect anything. Replayed over all 42
    // S19 decodes the drop to 0.65 costs nothing and recovers exactly one
    // true positive (15:19:19, happy p=0.403 share=0.684); identical at
    // 0.55 / 0.60 / 0.65. sad's own 0.95 floor is above the new global bar
    // and keeps its stricter threshold, unchanged.
    float share_floor    = 0.65f;
    // r24.6 (WO-02): ATHENA_EMOTION_RETIRED_IN_DENOM=1 restores the pre-r24.6
    // denominator (retired-class mass counted as evidence). Kept for A/B.
    bool  retired_in_denom = false;
    // ATHENA_EMOTION_ABSOLUTE=1 -> the pre-r21-full.8 rule, floors on the raw
    // posterior. Kept so a calibration done the old way stays reproducible.
    bool  absolute_rule  = false;
    // ── r24.7 (WO-88): faint-tag the low-confidence wins ─────────────────────
    // Two S20 measurements. `angry 0.900` on the one-word joke "Pregnant."
    // pushed valence −0.30 — a single word is the thinnest possible acoustic
    // evidence for anger and the costliest false positive the tagger has. And
    // 2 of 7 sad emissions rode share ≈ 0.99 at posterior p < 0.4 — the share
    // rule is confident about the RATIO while the model held almost no
    // absolute evidence. Both stay EMITTED (nothing is suppressed; the census
    // keeps its counts); they arrive wearing the ", faint" marker the B2
    // machinery already scales the mood step by (happy 0.75 → happy,faint
    // 0.225 class), so the S20 FP would have cost −0.09 instead of −0.30.
    // The two arms live in decide() — one copy of the rule, shared with the
    // calibrator — as `faint_low`: (a) a share-rule win with posterior
    // p < 0.50; (b) a single-word utterance decoding angry (word count is
    // handed in by the caller; −1 = unknown keeps the arm inert, so the
    // calibrator and every existing call site are unchanged).
    // ATHENA_EMOTION_FAINT_EXTRA=0 restores the r24.6 evidence-only faint.
    bool  faint_extra    = true;
    // r24.12 (WO-A2, S22 §2d): arm (b) was written for a one-word REPLY
    // ("Pregnant.", angry 0.900). S22 18:22:41 was a one-word BARGE-IN —
    // "Enough." cut into her mid-sentence at angry p=1.000, evid 1.000 — and
    // arm (b) tagged it `[emotion: angry, faint]` (uv −0.1375), so the harsh
    // door (uv <= −0.35) never opened and the model was told the anger was
    // "barely there". A barge is not thin evidence: it is the second signal
    // (people cut in when frustrated — the pivot path's own comment), and a
    // full posterior on top of it is not the S20 shape. On the pivot path
    // only; the main path is unchanged. =0 restores WO-88 (b) on barges.
    bool  faint_barge_full = true;
    float conf = 0.50f;          // base min probability to emit a tag (per-class floors derive from this)
    float floors[9] = {0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f};  // per-class min prob; set from env in init()
    static constexpr int MIN_SAMPLES = 4800;  // 0.3 s @ 16 kHz — shorter clips are unreliable
    // r24.12 (review): the rate this tagger is fed at, named once. tag() called
    // emotion_clip_cap_samples(16000) and divided by a literal 16000.0 three
    // times — while the function's whole signature exists so the rate need not
    // be a literal. Every caller is the whisper path (WHISPER_SAMPLE_RATE), so
    // the value is unchanged and this is a naming repair, not a behaviour one;
    // a second rate would now have exactly one place to be introduced.
    static constexpr int SAMPLE_RATE = 16000;

#ifdef ATHENA_EMOTION_ORT
    // Consume an OrtStatus*: a non-null status is an error — log it and release
    // it (the C-API marks these returns warn_unused_result; dropping the pointer
    // both warns and leaks the status on the error path). Returns true on error.
    bool ort_bad(OrtStatus *st, const char *what) {
        if (!st) return false;
        fprintf(stderr, "init: emotion2vec %s failed: %s\n", what, g_ort->GetErrorMessage(st));
        g_ort->ReleaseStatus(st);
        return true;
    }
#endif

    // r19.6: `quiet` raises the ORT log threshold to ERROR. ATHENA herself keeps
    // WARNING (the default) — the memcpy-node and ScatterND notices are worth
    // having in a session log. The calibrator passes true, because two dozen
    // lines of provider chatter between the banner and the first prompt buries
    // the one line that matters, which is which model actually loaded.
    void init(const std::string &model_path, bool use_cpu, bool quiet = false) {

        float_output = athena_env_flag("ATHENA_EMOTION_FLOAT_OUTPUT", true);

        debug = athena_env_flag("ATHENA_EMOTION_DEBUG");           // C66

        // [NEG-COLLAPSE] Optional: fold the four indistinguishable negative classes
        // to a single "negative" tag (calibration showed this model only resolves
        // coarse valence here). Unset -> specific labels, exactly as before.
        collapse_negative = athena_env_flag("ATHENA_EMOTION_COLLAPSE_NEGATIVE");   // C66

        // Per-class probability floors (calibration knobs, read once here):
        //   ATHENA_EMOTION_MIN          overrides the base floor (default 0.50)
        //   ATHENA_EMOTION_MIN_<LABEL>  overrides one class, e.g.
        //                               ATHENA_EMOTION_MIN_SAD=0.85
        // A class is emitted only when its probability clears its own floor, so
        // raising SAD tames over-prediction while lowering others surfaces them.
        {
            float base = conf;
            // EE1: set-but-empty / non-numeric now reads as UNSET, so a blank
            // knob keeps the calibrated defaults instead of collapsing to 0.0.
            float base_val = 0.0f;
            const bool base_env = athena_env_float("ATHENA_EMOTION_MIN", base_val);
            if (base_env) base = base_val;
            for (int i = 0; i < 9; ++i) {
                floors[i] = base;
                std::string key = "ATHENA_EMOTION_MIN_";
                for (const char *p = LABELS[i]; *p; ++p) {
                    char c = *p; if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A'); key += c;
                }
                float class_val = 0.0f;
                const bool e = athena_env_float(key.c_str(), class_val);
                if (e) floors[i] = class_val;
                // r16: same treatment for HAPPY. Measured in the S3 morning:
                // his genuinely warm deliveries read happy p=0.140-0.222
                // against the 0.40 env floor and were dropped every time —
                // the entire warm register of his voice was invisible to
                // her. Neutral speech reads happy p<=0.03, so 0.25 admits
                // warmth without admitting noise. Env always wins.
                if (!e && !base_env && std::string(LABELS[i]) == "happy") floors[i] = 0.25f;
                // r14 (F6): evidence-bounded default for SAD when nothing set
                // it explicitly. Calibration data from the live sessions: a
                // genuinely heavy delivery read sad p=0.558 against the 0.60
                // floor and was dropped — the one ⚡ beat the tagger missed by
                // a hair — while every true EMIT read 0.749–1.000. 0.55 admits
                // the near-miss without admitting the noise floor. Env always
                // wins: ATHENA_EMOTION_MIN or ATHENA_EMOTION_MIN_SAD override.
                if (!e && !base_env && std::string(LABELS[i]) == "sad") floors[i] = 0.55f;
            }
            // r21-full.8 (R8-A): the two new knobs, same env discipline.
            float v = 0.0f;
            if (athena_env_float("ATHENA_EMOTION_EVIDENCE", v)) evidence_floor = v;
            if (athena_env_float("ATHENA_EMOTION_NOISE", v))    noise_floor    = v;
            if (athena_env_float("ATHENA_EMOTION_SHARE", v))    share_floor    = v;   // R21 (F4)
            absolute_rule = athena_env_flag("ATHENA_EMOTION_ABSOLUTE");   // 14.1: value, not presence (the C66 class)
            retired_in_denom = athena_env_flag("ATHENA_EMOTION_RETIRED_IN_DENOM");  // r24.6 (WO-02)
            faint_extra   = athena_env_flag("ATHENA_EMOTION_FAINT_EXTRA", true);    // r24.7 (WO-88)
            faint_barge_full = athena_env_flag("ATHENA_EMOTION_FAINT_BARGE_FULL", true);   // r24.12 (WO-A2)
            bad_onnx_inert   = athena_env_flag("ATHENA_EMOTION_BAD_ONNX_INERT", true);     // r24.12 (WO-C7)
            if (debug) {
                fprintf(stderr, "emotion: rule=%s evidence>=%.2f noise>=%.2f share>=%.2f\n",
                        absolute_rule ? "absolute (pre-r21-full.8)" : "share-of-non-neutral",
                        evidence_floor, noise_floor, share_floor);
                fprintf(stderr, "emotion: floors");
                for (int i = 0; i < 9; ++i) fprintf(stderr, " %s=%.2f", LABELS[i], floors[i]);
                fprintf(stderr, "\n");
            }
        }
#ifdef ATHENA_EMOTION_ORT
        g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (ort_bad(g_ort->CreateEnv(quiet ? ORT_LOGGING_LEVEL_ERROR : ORT_LOGGING_LEVEL_WARNING,
                             "athena-emotion", &env), "CreateEnv")) return;

        if (ort_bad(g_ort->CreateSessionOptions(&opts), "CreateSessionOptions")) return;
        if (ort_bad(g_ort->SetIntraOpNumThreads(opts, 0), "SetIntraOpNumThreads")) return;
        // DISABLE_ALL, not ENABLE_ALL: the CUDA EP's shape/layout optimizations
        // constant-fold emotion2vec's alibi-attention to a FIXED sequence length
        // (the export-time 149 frames), after which every Run() on a clip of a
        // different length fails with a Reshape mismatch (Input {16,149,149} vs
        // requested {1,16,T,T}). The raw graph is correct for dynamic lengths
        // (it validated on the CPU EP at multiple lengths); disabling graph
        // optimization keeps it that way on CUDA. No SetOptimizedModelFilePath
        // either — that serialization (NchwcTransformer) baked the same shape.
        if (ort_bad(g_ort->SetSessionGraphOptimizationLevel(opts, ORT_DISABLE_ALL),
                    "SetSessionGraphOptimizationLevel")) return;
        // The waveform input is dynamic-length; disabling the CPU arena prevents
        // unbounded growth across differently-sized utterances (orpheus Fix 2).
        if (ort_bad(g_ort->DisableCpuMemArena(opts), "DisableCpuMemArena")) return;

        if (!use_cpu) {
            OrtCUDAProviderOptions cuda_opts{};
            cuda_opts.device_id = 0;
            cuda_opts.arena_extend_strategy = 1;        // kSameAsRequested
            cuda_opts.do_copy_in_default_stream = 1;
            OrtStatus *cs = g_ort->SessionOptionsAppendExecutionProvider_CUDA(opts, &cuda_opts);
            if (cs) { g_ort->ReleaseStatus(cs); }       // CUDA unavailable → fall through to CPU
            else    { has_cuda = true; }
        }

        // 14.1: through ort_bad — a typo'd model path was disabling emotion
        // with ZERO diagnostic (and leaking the status), and the user debugged
        // "no tags all session" blind.
        if (ort_bad(g_ort->CreateSession(env, model_path.c_str(), opts, &session),
                    "CreateSession")) return;
        if (ort_bad(g_ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault,
                                               &mem_info), "CreateCpuMemoryInfo")) return;
        if (ort_bad(g_ort->GetAllocatorWithDefaultOptions(&allocator),
                    "GetAllocatorWithDefaultOptions")) return;
        if (ort_bad(g_ort->CreateRunOptions(&run_opts), "CreateRunOptions")) return;
        // Only request arena shrinkage for an arena that exists, or Run() errors.
        // Non-fatal: the session is already created; a missing run-config entry
        // isn't worth disabling emotion over, so consume the status and continue.
        if (has_cuda) ort_bad(g_ort->AddRunConfigEntry(run_opts,
                "memory.enable_memory_arena_shrinkage", "gpu:0"), "AddRunConfigEntry");

        if (ort_bad(g_ort->SessionGetInputName(session, 0, allocator, &in_name),
                    "SessionGetInputName")) return;
        if (ort_bad(g_ort->SessionGetOutputName(session, 0, allocator, &out_name),
                    "SessionGetOutputName")) return;
        in_names[0] = in_name;
        out_names[0] = out_name;
        ok = true;
        fprintf(stderr, "%s: emotion2vec enabled (%s EP) [%s]\n", __func__,
                has_cuda ? "CUDA" : "CPU", model_path.c_str());
#else
        (void) model_path; (void) use_cpu; (void) quiet;
        fprintf(stderr, "%s: built without ONNX Runtime; emotion tagging disabled\n", __func__);
#endif

    }

    // ── r19.6: the two entry points the calibrator needs ────────────────────
    //
    // `decide` is the SELECTION RULE, lifted out of tag() so it can be applied
    // to a stored distribution without re-running the model. tag() calls it, so
    // there is exactly one copy: pick the best REAL emotion (neutral/other/unk
    // are excluded from the pick, deliberately — that is what lets a real
    // emotion sitting under a dominant neutral still surface), then emit only if
    // it clears THAT class's own floor.
    aev::AcousticObservation last_observation_;
    aev::AcousticObservation last_observation() const { return last_observation_; }
    struct Decision {
        int   cls = -1; float p = 0.0f; float floor = 1.0f;
        bool  emit = false; bool collapsed = false;
        // r21-full.8 (R8-A): the two quantities the rule now separates.
        float evidence = 0.0f;   // 1 - neutral - other - unk: how much of the
                                 // posterior is about an emotion at all
        // r24.6 (WO-02): the share DENOMINATOR — evidence minus the mass of
        // every class retired by the 1.01 convention, i.e. the posterior mass
        // that belongs to a class actually eligible to be picked. Equals
        // `evidence` when nothing is retired (and under RETIRED_IN_DENOM=1).
        float candidate = 0.0f;
        float share    = 0.0f;   // p / candidate: how confident, GIVEN that
        // r24.6 (WO-03): the bar the share was actually tested against —
        // max(per-class floor, global share floor) under the share rule, or
        // the per-class floor under ATHENA_EMOTION_ABSOLUTE. One copy of the
        // rule means one place the log line can read the true threshold.
        float bar      = 1.0f;
        // r24.7 (WO-88): this emission is a LOW-CONFIDENCE win — a share-rule
        // win at posterior p < 0.50, or angry decoded on a one-word utterance
        // — and renders ", faint" even when evidence >= 0.35. Emission itself
        // is untouched; only the mood step downstream scales.
        bool faint_low = false;
    };

    // r21-full.14 (B2): the tag string, in one testable place — same pattern
    // as decide() ("one copy of the rule"). Intensity survives into the tag:
    // below 0.35 evidence the emotion is real but barely-there, and the
    // suffix lets every consumer scale instead of taking p=0.105 sad at the
    // force of p=1.0 grief (S16's Sunday morning).
    // r24.7 (WO-88): `faint_low` joins the evidence test — a low-posterior
    // share win or a one-word angry arrives faintly too. Defaulted, so every
    // existing caller (and the r21-full.14 fixtures) is byte-identical.
    static std::string tag_text(bool collapsed, const char *label, float evidence,
                                bool faint_low = false) {
        return std::string("[emotion: ") + (collapsed ? "negative" : label)
             + ((evidence < 0.35f || faint_low) ? ", faint" : "") + "]";
    }

    // r24.7 (WO-88): `n_words` is the utterance's word count for the one-word-
    // angry arm; −1 (the default) = unknown, arm inert. Every existing caller
    // compiles and behaves unchanged.
    // r24.12 (WO-A2): `barged` = the utterance cut into her speech (pivot path).
    Decision decide(const float *scores, int n_words = -1, bool barged = false) const {
        Decision d;
        for (int i = 0; i < 9; ++i) {
            if (i == IDX_NEUTRAL || i == IDX_OTHER || i == IDX_UNK) continue;
            // r20p3.11 (RE1): a class retired by the 1.01 convention is skipped
            // in the PICK, not merely refused at the gate. Before this, the pick
            // ran first and the floor second, so a retired class that topped the
            // argmax became a veto over every other class rather than standing
            // down: an utterance scoring disgusted=0.42 (floor 1.01, retired)
            // and happy=0.31 (floor 0.25, well clear) emitted nothing at all.
            // The calibrator's own validation could not see it, because it
            // builds one-hot score vectors. Retiring a class the model cannot
            // separate is the documented, expected outcome of a calibration
            // pass, so this silenced the affect channel on exactly the
            // utterances the over-predicting class fires on.
            if (floors[i] > 1.0f) continue;
            if (scores[i] > d.p || d.cls < 0) { d.p = scores[i]; d.cls = i; }
        }
        d.floor = (d.cls >= 0) ? floors[d.cls] : 1.0f;
        // ── r21-full.8 (R8-A): the floor was on the wrong quantity ───────────
        //
        // The per-class floors were measured on CALIBRATION recordings — acted,
        // deliberate emotion, where `neutral` is small and a class's absolute
        // posterior is therefore close to its confidence. They were then applied
        // to CONVERSATIONAL speech, where emotion2vec is overwhelmingly
        // neutral-dominant: across 81 utterances from three live sessions the
        // median neutral posterior is 0.996. In that regime a class's absolute
        // posterior is not its confidence, it is
        //
        //     confidence  x  (1 - neutral mass)
        //
        // so the same warmth that reads happy=0.99 when acted reads happy=0.20
        // when spoken naturally, and a floor tuned in the first regime is
        // unreachable in the second. That is not a tuning error, it is two
        // different quantities wearing one name — and it is why S12 classified
        // 21 utterances and emitted ZERO, with three plainly warm turns scoring
        // happy=0.165/0.195/0.265 against a floor of 0.80, and two heavy ones
        // scoring sad=0.852/0.934 against a floor of 0.95.
        //
        // The regime-invariant quantity is the class's SHARE of the non-neutral
        // mass. A near-zero non-neutral mass would make that share meaningless
        // (a 0.003 happy against a 0.997 neutral has share 1.0 and means
        // nothing), so it is gated by an EVIDENCE floor: there has to be enough
        // non-neutral posterior for the question to be worth asking.
        //
        // Replayed against the same 81 utterances with the floors UNCHANGED:
        // S12 0 -> 5 emissions, S1 4 -> 6, S2 2 -> 3. The floors turn out to be
        // sensible share thresholds — which is what their own comments always
        // described them as ("genuine happy", "true sads were >= 0.92") — and
        // were only ever wrong about which number they were bounding.
        //
        // ATHENA_EMOTION_ABSOLUTE=1 restores the old rule exactly, for A/B and
        // for anyone whose calibration was done against the absolute posterior.
        d.evidence = 1.0f - scores[IDX_NEUTRAL] - scores[IDX_OTHER] - scores[IDX_UNK];
        if (d.evidence < 0.0f) d.evidence = 0.0f;
        // ── r24.6 (WO-02): retired classes are not evidence for a candidate ──
        //
        // RE1 (above) correctly skips a class retired by the 1.01 convention in
        // the PICK, but its probability mass stayed in the share DENOMINATOR,
        // so it still vetoed whatever was picked without ever being able to
        // win. S19 15:19:19 read happy=0.403 surprised=0.202 (surprised
        // retired): share 0.488 with the retired mass in, 0.684 with it out. A
        // class that cannot be selected cannot be evidence that the selected
        // class is wrong — it is simply not in the hypothesis set.
        //
        // The correction is scoped to the DENOMINATOR ONLY. `evidence` keeps
        // its original meaning — the total non-neutral posterior, "is there
        // enough here for the question to be worth asking" — because two other
        // consumers read it and both would REGRESS if it shrank:
        //   * the evidence floor. Retired mass is still real non-neutral
        //     posterior. Netting it out can push a passing utterance under
        //     evidence_floor and lose an emission the old rule made. Verified:
        //     happy=0.098 disgusted=0.042(retired) neutral=0.860 emits at
        //     share 0.700 today and is DROPPED by the netted form (0.098 <
        //     0.10) — a capability reduction, in the opposite direction to the
        //     one this work order exists to fix.
        //   * tag_text()'s ", faint" marker (evidence < 0.35), which
        //     acon:19386/19399 use to scale her mood step (happy 0.75 vs
        //     happy,faint 0.225). Verified: happy=0.28 surprised=0.12(retired)
        //     neutral=0.60 is a full tag today and a faint one under the
        //     netted form — same emission, a third of the effect.
        // Both are strictly additive to keep: `candidate` is a new quantity,
        // and share can now only rise, never fall.
        //
        // ATHENA_EMOTION_RETIRED_IN_DENOM=1 restores the old arithmetic.
        d.candidate = d.evidence;
        if (!retired_in_denom) {
            for (int i = 0; i < 9; ++i) {
                if (i == IDX_NEUTRAL || i == IDX_OTHER || i == IDX_UNK) continue;
                if (floors[i] > 1.0f) d.candidate -= scores[i];   // retired: not a
            }                                                     // candidate, so not
        }                                                         // evidence for one
        if (d.candidate < 0.0f) d.candidate = 0.0f;
        d.share = (d.candidate > 1e-6f && d.cls >= 0) ? (d.p / d.candidate) : 0.0f;
        if (d.share > 1.0f) d.share = 1.0f;
        if (absolute_rule) {
            d.bar  = d.floor;
            d.emit = (d.cls >= 0) && (d.p >= d.floor);
        } else {
            // R21 (F4/S18-4): the effective share bar is the HIGHER of the
            // per-class floor and the global share floor. A class retired by
            // the 1.01 convention is already excluded from the pick above, so
            // this cannot resurrect one; a class whose calibrated floor is
            // above the global one keeps its own, stricter bar.
            const float bar = d.floor > share_floor ? d.floor : share_floor;
            d.bar  = bar;
            d.emit = (d.cls >= 0) && (d.evidence >= evidence_floor) &&
                     (d.share >= bar) && (d.p >= noise_floor);
        }
        d.collapsed = d.emit && collapse_negative && is_negative_class(d.cls);
        // ── r24.7 (WO-88): the two faint arms, one copy of the rule ──────────
        // (a) A share-rule win whose absolute posterior is under a coin flip:
        //     share ≈ 0.99 at p = 0.36 is confidence about a RATIO, not about
        //     the feeling — S20 rode two sad emissions on exactly that. Scoped
        //     to the share rule: under ATHENA_EMOTION_ABSOLUTE the floors ARE
        //     absolute-posterior bars and p < 0.50 emissions are what the
        //     operator calibrated for.
        // (b) angry on a ONE-WORD utterance: the S20 joke "Pregnant." read
        //     angry 0.900 and cost −0.30 valence. One word is too little
        //     acoustics to take anger at full force; faint keeps the read and
        //     scales the push. n_words = −1 (unknown) keeps the arm inert.
        // Emission is preserved in both arms by construction — this sets a
        // marker on an EMIT that already happened, never the emit bit.
        if (faint_extra && d.emit) {
            if (!absolute_rule && d.p < 0.50f)          d.faint_low = true;   // (a)
            if (d.cls == IDX_ANGRY && n_words == 1 &&
                !(faint_barge_full && barged && d.p >= 0.90f)) d.faint_low = true;   // (b); r24.12 WO-A2: not a confident barge
        }
        return d;
    }

    // R19 (r22.1): how many elements the output tensor actually holds
    // (0 on any API failure). Both readers below check >= 9 before reading.
#ifdef ATHENA_EMOTION_ORT
    size_t ort_out_elems_(OrtValue *output) {
        OrtTensorTypeAndShapeInfo *tinfo = nullptr;
        size_t n_elem = 0;
        OrtStatus *st2 = g_ort->GetTensorTypeAndShape(output, &tinfo);
        if (!st2 && tinfo) {
            OrtStatus *st3 = g_ort->GetTensorShapeElementCount(tinfo, &n_elem);
            if (st3) { g_ort->ReleaseStatus(st3); n_elem = 0; }
            if (float_output) {
                ONNXTensorElementDataType kind = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
                OrtStatus *type_status = g_ort->GetTensorElementType(tinfo, &kind);
                if (type_status) { g_ort->ReleaseStatus(type_status); n_elem = 0; }
                if (kind != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) n_elem = 0;
            }
            g_ort->ReleaseTensorTypeAndShapeInfo(tinfo);
        } else if (st2) {
            g_ort->ReleaseStatus(st2);
        }
        return n_elem;
    }
#endif
    // Run the model and hand back the RAW nine-class distribution. This is what
    // the calibrator measures; nothing here consults or depends on the floors,
    // so a calibration pass reads the model rather than reading its own settings.
    // Returns false if the model is unavailable or the clip is too short.
    bool probe(const std::vector<float> &pcm, float out[9]) {
        for (int i = 0; i < 9; ++i) out[i] = 0.0f;
        if (!ok || (int) pcm.size() < MIN_SAMPLES) return false;
#ifdef ATHENA_EMOTION_ORT
        OrtValue *input = nullptr, *output = nullptr;
        const int64_t shape[1] = { (int64_t) pcm.size() };
        OrtStatus *st = g_ort->CreateTensorWithDataAsOrtValue(
            mem_info, (void *) pcm.data(), pcm.size() * sizeof(float),
            shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
        if (st) { g_ort->ReleaseStatus(st); return false; }
        st = g_ort->Run(session, run_opts, in_names,
                        (const OrtValue *const *) &input, 1, out_names, 1, &output);
        if (st) {
            fprintf(stderr, "emotion: Run failed (%s)\n", g_ort->GetErrorMessage(st));
            g_ort->ReleaseStatus(st);
            g_ort->ReleaseValue(input);
            return false;
        }
        bool got = false;
        float *scores = nullptr;
        st = g_ort->GetTensorMutableData(output, (void **) &scores);
        // R19 (r22.1): nine floats are read — the tensor must actually hold
        // nine. A wrong or corrupt ONNX made this an out-of-bounds read.
        if (!st && scores && ort_out_elems_(output) < 9) scores = nullptr;
        if (!st && scores) { for (int i = 0; i < 9; ++i) out[i] = scores[i]; got = true; }
        if (st) g_ort->ReleaseStatus(st);
        g_ort->ReleaseValue(output);
        g_ort->ReleaseValue(input);
        return got;
#else
        return false;
#endif
    }

    // Returns "[emotion: <label>]" for a confident non-neutral utterance, else "".
    // r24.7 (WO-88): `n_words` = the transcribed utterance's word count, for
    // the one-word-angry faint arm; −1 (default) = unknown, arm inert.
    std::string tag(const std::vector<float> &pcm, int n_words = -1, bool barged = false) {   // r24.12 (WO-A2)

        last_observation_ = {};
        if (!ok || (int) pcm.size() < MIN_SAMPLES) return "";
#ifdef ATHENA_EMOTION_ORT
        // LABELS is now a struct member (shared with init()); see top of struct.
        std::string result;
        // ── r24.12 (WO-C7): the LAST emotion_clip_cap_samples() of the clip ──
        // (15 s by default — the calibrator's own ceiling; 0 = the whole clip,
        // r24.11). The 81 s S22 replay would have been 15 s here. Both tag()
        // call sites in talk-llama.cpp (the main turn and the barge pivot)
        // pass through this; probe() does not.
        std::vector<float> tail;
        const std::vector<float> *use = &pcm;
        const size_t cap = emotion_clip_cap_samples(SAMPLE_RATE);   // r24.12 (review)
        if (cap && pcm.size() > cap) {
            tail = emotion_clip_window(pcm, cap);
            use  = &tail;
            if (debug)
                fprintf(stderr, "emotion: %.1f s clip — tagging its last %.1f s\n",
                        (double) pcm.size() / (double) SAMPLE_RATE,
                        (double) cap / (double) SAMPLE_RATE);
        }
        OrtValue *input = nullptr;
        OrtValue *output = nullptr;
        const int64_t shape[1] = { (int64_t) use->size() };   // ONNX input "waveform" is rank-1 [T]
        OrtStatus *st = g_ort->CreateTensorWithDataAsOrtValue(
            mem_info, (void *) use->data(), use->size() * sizeof(float),
            shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
        if (st) { g_ort->ReleaseStatus(st); return ""; }
        st = g_ort->Run(session, run_opts, in_names,
                        (const OrtValue *const *) &input, 1, out_names, 1, &output);
        if (st) {
            // r24.12 (WO-C7 D12): every failure, named and counted. "tagging
            // inert this session" was false — nothing here clears `ok`, and the
            // next clip is tried exactly as before.
            run_failures++;
            fprintf(stderr, "emotion: Run failed on a %.1f s clip (%s) — this utterance untagged; "
                            "later ones are still tried (failure #%d)\n",
                    (double) use->size() / (double) SAMPLE_RATE,   // r24.12 (review)
                    g_ort->GetErrorMessage(st), run_failures);
            g_ort->ReleaseStatus(st);
            g_ort->ReleaseValue(input);
            return "";
        }
        float *scores = nullptr;
        st = g_ort->GetTensorMutableData(output, (void **) &scores);
        // R19 (r22.1): decide() reads NINE floats — count before reading.
        if (!st && scores && ort_out_elems_(output) < 9) {
            if (!run_warned) {
                fprintf(stderr, "emotion: output tensor holds %s "
                        "scores - tagging inert this session\n",
                        float_output ? "fewer than nine usable float" : "fewer than 9");
                run_warned = true;
            }
            // r24.12 (WO-C7 D12): a per-session condition (a wrong ONNX), so
            // the message's claim is now made true — no further Run() on it.
            if (bad_onnx_inert) ok = false;
            scores = nullptr;
        }
        if (!st && scores) {
            // Selection: pick the best REAL emotion (skip neutral/other/unk) and
            // emit it when it clears that class's floor — regardless of whether
            // neutral has the plurality. This surfaces a salient emotion sitting
            // under a dominant neutral (the old argmax-over-9 could not), while
            // per-class floors let calibration tame an over-predicted class (sad)
            // without suppressing the rest.
            // r19.6: one copy of the rule, in decide(), shared with the
            // calibrator. Behaviour is unchanged — this is the same three lines.
            const Decision d = decide(scores, n_words, barged);
            last_observation_.posterior_available = true;
            std::copy(scores, scores+9, last_observation_.posterior.begin());
            last_observation_.probability=d.p; last_observation_.share=d.share;
            last_observation_.evidence=d.evidence;
            last_observation_.calibration_id="legacy-selection; posterior reliability uncalibrated";   // r24.7 (WO-88) / r24.12 (WO-A2)
            const int   bestE = d.cls;
            const float bestP = d.p, fl = d.floor;
            const bool  emit = d.emit, collapsed = d.collapsed;
            // r21-full.14 (B2): intensity survives into the tag. S16's Sunday
            // ran on sad EMITs at p=0.105-0.388 — real but barely-there — and
            // the binary tag hit the him-model, her mood, and the voice settle
            // as hard as a p=1.0 grief. A faint marker lets every consumer
            // scale; the S8 baseline (7 tags, affect running free) is the
            // register being restored.
            if (emit) result = tag_text(collapsed, LABELS[bestE], d.evidence,
                                        d.faint_low);   // r24.7 (WO-88)

            if (debug) {
                // Full 9-class distribution + decision — the calibration log line.
                // pick= stays the underlying class (so calibration reads true);
                // a remap is shown as a trailing "[collapsed: negative]".
                fprintf(stderr, "emotion:");
                for (int i = 0; i < 9; ++i) fprintf(stderr, " %s=%.3f", LABELS[i], scores[i]);
                // r21-full.8 (R8-A): print BOTH quantities. The whole defect
                // was that one number was read as the other, and the log line
                // is where the next calibration pass will read it.
                // r24.6 (WO-03): print the BAR that actually gates, not only the
                // per-class floor. The two differ whenever max() picks the
                // global share floor, and in S19 every pick=happy line printed
                // floor=0.80 while the real bar was 0.90 — a calibration pass
                // reading this log would have tuned the wrong number.
                fprintf(stderr, "  -> pick=%s p=%.3f evid=%.3f share=%.3f floor=%.2f bar=%.2f %s%s\n",
                        bestE >= 0 ? LABELS[bestE] : "-", bestP, d.evidence, d.share,
                        fl, d.bar, emit ? "EMIT" : "drop",
                        collapsed ? " [collapsed: negative]" : "");
            }
        }
        if (st) g_ort->ReleaseStatus(st);
        g_ort->ReleaseValue(output);
        g_ort->ReleaseValue(input);
        return result;
#else
        (void) n_words; (void) barged;   // r24.7 (WO-88) / r24.12 (WO-A2): consumed by the ORT branch only
        return "";
#endif
    }
};
