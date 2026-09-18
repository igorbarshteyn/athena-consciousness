// athena-emotion-calibrate.cpp — interactive vocal-affect calibration for ATHENA.
//
// r19.6. Replaces the shell+Python calibrator, which violated the project's one
// hard architectural rule: zero Python at runtime. This is a C++ binary built by
// the same CMake as whisper-talk-llama, linking the same libraries — whisper,
// common, common-sdl, ONNX Runtime — and, critically, #including the SAME
// athena_emotion.h that ATHENA runs. A calibrator that reimplements the
// selection rule calibrates itself instead of her.
//
// It loads whisper and emotion2vec and nothing else: no LLM, no TTS, no memory.
//
//   ./athena-emotion-calibrate -mw models/ggml-small.en.bin
//                              -me models/emotion2vec_plus_large.onnx
//
//   --selftest   exercise the adaptive maths on synthetic distributions, no mic
//   --emotion-cpu  force the CPU execution provider
//   --list        list capture devices and exit
//
// The procedure is the one written up in EMOTION-RECALIBRATION.md: labelled
// takes (you know what register you were aiming for), per-class floors placed
// between the true and false distributions, and a class retired outright when
// they overlap. What this adds over doing it by hand is that it gates each take
// on quality before it can poison a distribution, spends takes adaptively on the
// classes that are still undecided, runs a validation round against the derived
// floors, and prints the exact export block.

#include "common.h"
#include "common-sdl.h"
#include "whisper.h"

#include "athena_emotion.h"
#include "athena_calib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>


// ── finding the models ──────────────────────────────────────────────────────
//
// CMake installs this next to whisper-talk-llama, i.e. in
// <ATHENA>/whisper.cpp/build/bin/ — three levels below the repo root, where
// models/ lives. Relative defaults therefore only resolved if you happened to
// run it from the root, and the failure ("could not load the emotion model")
// pointed at the model rather than at the working directory. The launcher
// solves the same problem by resolving ATHENA_DIR from its own path; this does
// the same, from /proc/self/exe.
static std::string dir_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

static bool file_exists(const std::string &p) {
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    return dir_of(buf);
}

// Roots to try, nearest-first: an explicit ATHENA_DIR, the current directory,
// then the executable's directory and each parent up to five levels (bin ->
// build -> whisper.cpp -> ATHENA covers the installed layout with room spare).
static std::vector<std::string> search_roots() {
    std::vector<std::string> roots;
    if (const char *ad = ::getenv("ATHENA_DIR")) if (*ad) roots.push_back(ad);
    roots.push_back(".");
    std::string d = exe_dir();
    for (int i = 0; i < 6 && !d.empty() && d != "/"; i++) {
        roots.push_back(d);
        d = dir_of(d);
    }
    return roots;
}

// An absolute path, or one that already resolves, is used as given. Otherwise
// the name is tried under each root — both as written and under models/, so
// `-me emotion2vec_plus_large.onnx` works as well as the full relative path.
static std::string resolve_model(const std::string &given, bool *found) {
    *found = false;
    if (given.empty()) return given;
    if (file_exists(given)) { *found = true; return given; }
    if (!given.empty() && given[0] == '/') return given;          // absolute: report as given
    const std::string bare = given.substr(given.find_last_of('/') + 1);
    for (const auto &root : search_roots()) {
        const std::string a = root + "/" + given;
        if (file_exists(a)) { *found = true; return a; }
        const std::string b = root + "/models/" + bare;
        if (file_exists(b)) { *found = true; return b; }
    }
    return given;
}

// ── terminal ────────────────────────────────────────────────────────────────

static const char *B = "\033[1m", *D = "\033[2m", *G = "\033[32m",
                  *Y = "\033[33m", *R = "\033[31m", *N = "\033[0m";

static void rule() { std::printf("%s%s%s\n", D, "──────────────────────────────────────────────────────────────", N); }

struct Params {
    std::string whisper_model = "models/ggml-small.en.bin";
    std::string emotion_model = "models/emotion2vec_plus_large.onnx";
    int   capture_id   = -1;
    bool  emotion_cpu  = false;
    bool  selftest     = false;
    bool  list_devices = false;
    bool  no_whisper   = false;
    bool  mic_test     = false;
    bool  verbose      = false;


    int   budget       = 4;      // max takes per class
    float base_floor   = 0.50f;
};

static void usage(const char *argv0) {
    std::printf(
"usage: %s [options]\n"
"  -mw,  --model-whisper FNAME   whisper model (default models/ggml-small.en.bin)\n"
"  -me,  --model-emotion FNAME   emotion2vec ONNX (default models/emotion2vec_plus_large.onnx)\n"
"  -c,   --capture ID            capture device id (default: system default)\n"
"        --emotion-cpu           force the CPU execution provider\n"
"        --no-whisper            skip transcription (faster start, less feedback)\n"
"        --takes N               max takes per class (default 4)\n"
"        --base N.NN             base floor written to ATHENA_EMOTION_MIN (default 0.50)\n"
"        --list                  list capture devices and exit\n"
"        --mic-test              live level meter, to check capture before committing\n"
"  -v,   --verbose               keep the ONNX Runtime provider chatter\n"


"        --selftest              exercise the adaptive maths without a microphone\n"
"  -h,   --help                  this help\n", argv0);
}

// ── recording ───────────────────────────────────────────────────────────────
//
// Press ENTER to start, speak, then stop on trailing silence — the same shape as
// her own turn-taking, so what the model scores here is the same kind of clip it
// scores in conversation.
// The capture device is resumed ONCE, at startup, and never paused between
// takes — the same lifecycle whisper-talk-llama uses, and for the same reason.
//
// The first version paused after every take and resumed before the next. On a
// PipeWire / sof-soundwire stack that is a one-way door: SDL reports the resume
// as successful, the callback never fires again, `m_audio_len` stays 0, and
// every subsequent `get()` returns an empty buffer. The take then failed the
// duration gate and was reported as "too short — aim for 3 to 6 seconds", which
// sent the operator off to speak more slowly at a microphone that was no longer
// being read at all. Nothing about the failure pointed at the audio path.
//
// Returns false only if the session should end (quit event).
static bool record_take(audio_async &audio, std::vector<float> &out,
                        int max_ms, int silence_ms, float &peak_rms_seen,
                        bool &captured_anything) {
    audio.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    audio.clear();

    const int poll_ms = 100;
    std::vector<float> chunk;
    int elapsed = 0, quiet = 0, voiced = 0;
    size_t samples_seen = 0;
    peak_rms_seen = 0.0f;
    captured_anything = false;
    bool started = false;

    while (elapsed < max_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
        if (!sdl_poll_events()) return false;
        elapsed += poll_ms;
        audio.get(poll_ms, chunk);
        samples_seen += chunk.size();
        if (chunk.empty()) continue;
        const float r = acal::rms_of(chunk);
        if (r > peak_rms_seen) peak_rms_seen = r;

        // A simple presence gate; the take is graded properly afterwards.
        if (r >= 0.010f) { started = true; voiced += poll_ms; quiet = 0; }
        else if (started) { quiet += poll_ms; }

        // Live level meter, with the state named. Silence during a take is
        // ambiguous to look at — "listening" versus "hearing you" is the
        // difference between a quiet room and a dead capture path.
        const int bars = (int) std::min(28.0f, r * 400.0f);
        std::printf("\r    %s[%-28s]%s %5.3f  %s   ", D,
                    std::string((size_t) std::max(0, bars), '=').c_str(), N, r,
                    started ? (quiet ? "…" : "hearing you") : "listening  ");
        std::fflush(stdout);

        if (started && quiet >= silence_ms && voiced >= 800) break;
    }
    std::printf("\r%*s\r", 64, "");
    std::fflush(stdout);

    captured_anything = samples_seen > 0;
    audio.get(elapsed, out);
    return true;
}

// Confirm the capture path actually delivers samples before asking anyone to
// spend twenty minutes talking into it.
static bool warm_up_capture(audio_async &audio, float &peak) {
    std::vector<float> chunk;
    peak = 0.0f;
    size_t total = 0;
    for (int i = 0; i < 25; i++) {              // up to 2.5 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!sdl_poll_events()) return false;
        audio.get(100, chunk);
        total += chunk.size();
        const float r = acal::rms_of(chunk);
        if (r > peak) peak = r;
        if (total > 8000) return true;          // half a second of real samples
    }
    return total > 0;
}


static std::string transcribe(whisper_context *ctx, const std::vector<float> &pcm) {
    if (!ctx || pcm.empty()) return "";
    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.print_progress   = false;
    wp.print_special    = false;
    wp.print_realtime   = false;
    wp.print_timestamps = false;
    wp.single_segment   = true;
    wp.no_timestamps    = true;
    wp.language         = "en";
    wp.n_threads        = std::min(4, (int) std::thread::hardware_concurrency());
    if (whisper_full(ctx, wp, pcm.data(), (int) pcm.size()) != 0) return "";
    std::string out;
    for (int i = 0; i < whisper_full_n_segments(ctx); i++) out += whisper_full_get_segment_text(ctx, i);
    return ::trim(out);
}

// ── the elicitation script ──────────────────────────────────────────────────
//
// Content does not matter to the model; only the acoustics do. Using the SAME
// sentence for the neutral and the emotional take is what makes the pair
// comparable — the difference between them is register and nothing else.
struct Prompt { const char *cls; const char *line; const char *how; };

static const Prompt SCRIPT[] = {
    { "happy",     "I finished the thing I was working on, and it came out well.",
                   "pleased, lifted — the way you'd tell a friend good news" },
    { "sad",       "I've been up since four and I still have the whole day ahead.",
                   "heavy, tired, weight in the voice" },
    { "angry",     "That is not what I asked you to do, and you did it anyway.",
                   "genuinely irritated — clipped, hard consonants" },
    { "surprised", "There's something in the report I really wasn't expecting.",
                   "caught off guard — pitch up, sudden" },
    { "fearful",   "I don't want to think about that one any more tonight.",
                   "uneasy, tight, not quite steady" },
    { "disgusted", "Someone left the whole thing sitting there for three days.",
                   "repelled — the face you'd make at a smell" },
};
static const size_t N_CLASSES = sizeof(SCRIPT) / sizeof(SCRIPT[0]);

static int class_index(const char *label) {
    for (int i = 0; i < 9; i++) if (!std::strcmp(EmotionTagger::LABELS[i], label)) return i;
    return -1;
}

static void wait_enter(const char *msg) {
    std::printf("%s", msg);
    std::fflush(stdout);
    int c;
    while ((c = std::getchar()) != '\n' && c != EOF) { }
}

// ── selftest: the maths, with no microphone ─────────────────────────────────
static int selftest() {
    int fail = 0;
    auto check = [&](bool cond, const char *what) {
        std::printf("  %s%s%s %s\n", cond ? G : R, cond ? "PASS" : "FAIL", N, what);
        if (!cond) fail++;
    };
    std::printf("\n%sathena-emotion-calibrate — selftest%s\n\n", B, N);

    // clean separation
    {
        const acal::Separation s = acal::separate({0.80f, 0.90f}, {0.10f, 0.20f, 0.05f});
        check(s.separable, "a clean split is separable");
        check(s.floor > 0.20f && s.floor <= 0.80f, "and the floor lands between the two");
        check(s.margin > 0.5f, "with a wide margin");
    }
    // overlap -> retire
    {
        const acal::Separation s = acal::separate({0.40f, 0.55f}, {0.50f, 0.60f});
        check(!s.separable, "overlapping distributions are not separable");
        check(s.floor >= acal::RETIRED, "and the class is retired");
    }
    // the S6 case: disgusted false at 0.815, no true readings at all
    {
        const acal::Separation s = acal::separate({}, {0.815f, 0.10f});
        check(!s.separable, "a class with no true readings is retired");
    }
    // a floor must never sit at or below the strongest false reading
    {
        const acal::Separation s = acal::separate({0.52f}, {0.50f}, 1);
        check(!s.separable || s.floor > 0.50f, "a derived floor always clears max(false)");
    }
    // adaptivity
    {
        acal::Separation wide;  wide.enough_data = true; wide.separable = true; wide.margin = 0.40f;
        acal::Separation tight; tight.enough_data = true; tight.separable = true; tight.margin = 0.06f;
        acal::Separation bad;   bad.enough_data = true;  bad.separable = false; bad.margin = -0.50f;
        acal::Separation early;
        check(acal::extra_takes_wanted(wide,  2, 4) == 0, "a clean class asks for no more takes");
        check(acal::extra_takes_wanted(tight, 2, 4) == 1, "a tight class asks for one more");
        check(acal::extra_takes_wanted(bad,   2, 4) == 0, "a hopeless class stops spending voice");
        check(acal::extra_takes_wanted(early, 0, 4) == 4, "and the minimum is always collected");
        check(acal::extra_takes_wanted(tight, 4, 4) == 0, "the budget is never exceeded");
    }
    // take grading
    {
        check(!acal::grade_take(1.0f, 0.05f, 0.0f).ok,   "a one-second take is rejected");
        check(!acal::grade_take(4.0f, 0.002f, 0.0f).ok,  "a near-silent take is rejected");
        check(!acal::grade_take(4.0f, 0.05f, 0.02f).ok,  "a clipping take is rejected");
        check( acal::grade_take(4.0f, 0.05f, 0.0f).ok,   "a good take passes");
        check(!acal::grade_take(20.0f, 0.05f, 0.0f).ok,  "a monologue is rejected");
    }
    // the export block always names every class
    {
        std::vector<acal::ClassResult> rs;
        acal::ClassResult a; a.label = "happy"; a.sep = acal::separate({0.8f,0.9f},{0.1f}); rs.push_back(a);
        acal::ClassResult b; b.label = "disgusted"; b.sep = acal::separate({}, {0.815f}); rs.push_back(b);
        const std::string blk = acal::export_block(rs, 0.50f);
        check(blk.find("ATHENA_EMOTION_MIN_HAPPY=") != std::string::npos, "the export names HAPPY");
        check(blk.find("ATHENA_EMOTION_MIN_DISGUSTED=1.01") != std::string::npos,
              "and retires DISGUSTED explicitly rather than by omission");
        check(blk.find("ATHENA_EMOTION_MIN=0.50") != std::string::npos, "and sets the base");
    }
    // the selection rule agrees with the tagger she actually runs
    {
        EmotionTagger t;
        for (int i = 0; i < 9; i++) t.floors[i] = 0.50f;
        float s[9] = {0,0,0,0,0,0,0,0,0};
        s[EmotionTagger::IDX_NEUTRAL] = 0.90f;   // dominant neutral
        s[6] = 0.60f;                            // sad, under it, over its floor
        const EmotionTagger::Decision d = t.decide(s);
        check(d.cls == 6 && d.emit, "a real emotion under a dominant neutral still surfaces");
        t.floors[6] = 1.01f;
        check(!t.decide(s).emit, "and a retired class never fires");
    }

    std::printf("\n%s%s%s\n\n", fail ? R : G, fail ? "selftest FAILED" : "selftest passed", N);
    return fail ? 1 : 0;
}

int main(int argc, char **argv) {
    Params p;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if      ((a == "-mw" || a == "--model-whisper") && i + 1 < argc) p.whisper_model = argv[++i];
        else if ((a == "-me" || a == "--model-emotion") && i + 1 < argc) p.emotion_model = argv[++i];
        else if ((a == "-c"  || a == "--capture")       && i + 1 < argc) p.capture_id    = std::atoi(argv[++i]);
        else if (a == "--emotion-cpu") p.emotion_cpu  = true;
        else if (a == "--no-whisper")  p.no_whisper   = true;
        else if (a == "--selftest")    p.selftest     = true;
        else if (a == "--list")        p.list_devices = true;
        else if (a == "--mic-test")    p.mic_test     = true;
        else if (a == "-v" || a == "--verbose") p.verbose = true;


        else if (a == "--takes" && i + 1 < argc) p.budget     = std::atoi(argv[++i]);
        else if (a == "--base"  && i + 1 < argc) p.base_floor = (float) std::atof(argv[++i]);
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 1; }
    }

    // Line-buffered, so the transcript stays in order when it is piped to a file
    // or pasted into a bug report. Block buffering put every stdout line after
    // every stderr line, which made the model-loading chatter look like it came
    // before the banner.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (p.selftest) return selftest();
    // r24.16: scalar posteriors and one-hot replay concealed the runtime's
    // neutral denominator. =0 restores the complete previous tool path.
    const bool share_calibration = athena_env_flag("ATHENA_CALIB_SHARE", true);


    // The floors must NOT be inherited from the environment during a
    // calibration pass — the whole point is to read the model, not to read a
    // distribution already filtered by the floors being derived. Clearing them
    // here means the operator does not have to remember to comment them out.
    ::unsetenv("ATHENA_EMOTION_MIN");
    ::unsetenv("ATHENA_EMOTION_COLLAPSE_NEGATIVE");
    for (int i = 0; i < 9; i++) {
        std::string k = "ATHENA_EMOTION_MIN_";
        for (const char *c = EmotionTagger::LABELS[i]; *c; ++c)
            k += (char) ((*c >= 'a' && *c <= 'z') ? *c - 'a' + 'A' : *c);
        ::unsetenv(k.c_str());
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL_Init(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
        return 1;
    }
    if (p.list_devices) {
        const int n = SDL_GetNumAudioDevices(SDL_TRUE);
        std::printf("capture devices:\n");
        for (int i = 0; i < n; i++) std::printf("  %d: %s\n", i, SDL_GetAudioDeviceName(i, SDL_TRUE));
        SDL_Quit();
        return 0;
    }

    std::printf("\n%sATHENA — vocal affect calibration%s\n", B, N);
    rule();

    // Resolve the models relative to the repo, not to the working directory:
    // this binary is installed three levels down in whisper.cpp/build/bin.
    bool found_emotion = false, found_whisper = false;
    const std::string emotion_path = resolve_model(p.emotion_model, &found_emotion);
    const std::string whisper_path = resolve_model(p.whisper_model, &found_whisper);
    if (!found_emotion) {
        std::fprintf(stderr,
            "\n%sCould not find the emotion model.%s Looked for \"%s\" under:\n", R, N, p.emotion_model.c_str());
        for (const auto &r : search_roots()) std::fprintf(stderr, "    %s/\n    %s/models/\n", r.c_str(), r.c_str());
        std::fprintf(stderr,
            "Give an explicit path with -me, or set ATHENA_DIR to the repo root.\n");
        SDL_Quit();
        return 1;
    }
    std::printf("  emotion model: %s%s%s\n", D, emotion_path.c_str(), N);
    if (!p.no_whisper)
        std::printf("  whisper model: %s%s%s\n", D,
                    found_whisper ? whisper_path.c_str() : "(not found — continuing without transcription)", N);

    EmotionTagger tagger;
    tagger.init(emotion_path, p.emotion_cpu, /*quiet=*/!p.verbose);

    if (!tagger.ok) {
        std::fprintf(stderr,
            "\n%sThe emotion model did not load.%s The file is there (%s), so this is the\n"
            "runtime rather than the path — check that this binary was built with\n"
            "-DONNXRUNTIME_ROOT=<...>, and try --emotion-cpu if the CUDA provider is\n"
            "unhappy. Without the model there is nothing to calibrate.\n",
            R, N, emotion_path.c_str());
        SDL_Quit();
        return 1;
    }

    whisper_context *wctx = nullptr;
    if (!p.no_whisper && found_whisper) {
        whisper_context_params cp = whisper_context_default_params();
        wctx = whisper_init_from_file_with_params(whisper_path.c_str(), cp);
        if (!wctx) std::printf("%s  (whisper did not load — continuing without transcription)%s\n", Y, N);
    }


    audio_async audio(30 * 1000);
    if (!audio.init(p.capture_id, WHISPER_SAMPLE_RATE)) {
        std::fprintf(stderr, "audio.init() failed — is the microphone available?\n");
        if (wctx) whisper_free(wctx);
        SDL_Quit();
        return 1;
    }
    // Resumed ONCE and left running for the whole session — see record_take.
    if (!audio.resume()) {
        std::fprintf(stderr, "\n%scould not start capture on that device.%s Try --list, then -c <id>.\n", R, N);
        if (wctx) whisper_free(wctx);
        SDL_Quit();
        return 1;
    }
    {
        float peak = 0.0f;
        std::printf("\n  checking the microphone ");
        std::fflush(stdout);
        const bool alive = warm_up_capture(audio, peak);
        if (!alive) {
            std::printf("\n\n%sThe capture device opened but is delivering no samples.%s\n"
                        "Nothing can be calibrated through it. Run with --list to see the devices\n"
                        "and select one explicitly with -c <id>; on this machine the headset is\n"
                        "usually not device 0.\n", R, N);
            audio.pause();
            if (wctx) whisper_free(wctx);
            SDL_Quit();
            return 1;
        }
        std::printf("— %sok%s (idle level %.4f)\n", G, N, peak);
        if (peak > 0.02f)
            std::printf("  %snote: the room is not quiet (idle level %.3f). Background noise raises\n"
                        "  every reading and can make classes look inseparable when they are not.%s\n", Y, peak, N);

    }

    if (p.mic_test) {
        std::printf("\n%sMicrophone test%s — speak; Ctrl+C to stop.\n\n", B, N);
        std::vector<float> chunk;
        while (sdl_poll_events()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            audio.get(100, chunk);
            const float r = acal::rms_of(chunk);
            const int bars = (int) std::min(40.0f, r * 400.0f);
            std::printf("\r  [%-40s] %5.3f  %s ", std::string((size_t) std::max(0, bars), '=').c_str(),
                        r, r >= 0.010f ? "speech" : "      ");
            std::fflush(stdout);
        }
        std::printf("\n");
        audio.pause();
        if (wctx) whisper_free(wctx);
        SDL_Quit();
        return 0;
    }


    std::printf(
"\nThis takes about twenty minutes. Put your headset on and use it exactly as you\n"
"would when talking to her — same position, same distance, same room. Everything\n"
"measured here is specific to that setup.\n\n"
"Each line is said twice: once flat, once in the named register. The flat take is\n"
"not filler — it is the evidence that the register is distinguishable at all.\n\n"
"Aim for three to six seconds. Takes that are too short, too quiet or clipping are\n"
"rejected and offered again, because a bad take poisons the measurement and you\n"
"cannot hear that it was bad.\n\n");
    wait_enter("Press ENTER when you are ready. ");

    // per-class true readings, and every reading that is NOT of that class
    std::vector<std::vector<float>> trues(N_CLASSES), falses(N_CLASSES);
    int usable_takes = 0, rejected = 0;
    std::vector<acal::Posterior> class_takes[9], flat_takes;
    bool in_script[9] = {};
    for (const auto &pr : SCRIPT) in_script[class_index(pr.cls)] = true;
    // Keep these gates identical in derivation, actual-rule replay and export.
    // The per-class measured thresholds supply the share floor; a hidden
    // inherited global floor must not override them after validation.
    const float calib_evidence = 0.10f, calib_noise = 0.02f, calib_share = 0.0f;
    auto separation = [&](size_t c) {
        return share_calibration
            ? acal::derive_share_floors(class_takes, in_script, flat_takes, 2,
                                        calib_evidence).sep[class_index(SCRIPT[c].cls)]
            : acal::separate(trues[c], falses[c]);
    };

    // Record one take, grade it, and file its nine probabilities.
    // `intended` is the class index in SCRIPT, or -1 for a neutral take.
    auto one_take = [&](const Prompt &pr, int intended, const char *banner) -> bool {
        for (int attempt = 0; attempt < 4; attempt++) {
            std::printf("\n  %s%s%s\n", B, banner, N);
            std::printf("  say:  %s\"%s\"%s\n", D, pr.line, N);
            if (intended >= 0) std::printf("  as:   %s%s%s\n", Y, pr.how, N);
            else               std::printf("  as:   %sflat and matter-of-fact%s\n", D, N);
            wait_enter("  ENTER to record, then speak. ");

            std::vector<float> raw;
            float peak = 0.0f;
            bool  got_samples = false;
            // r21-full.5 (R5-X): 20 s, not 15. The grader rejects above 15 s,
            // and at a 15 s cap that branch was unreachable — the buffer could
            // not exceed the number the gate compares against.
            if (!record_take(audio, raw, /*max_ms=*/20000, /*silence_ms=*/800, peak, got_samples))
                return false;

            // Grade the SPEECH, not the buffer: most of a take is the pause
            // before someone starts. This also gives the model the utterance
            // rather than the utterance plus six seconds of room tone.
            const acal::SpeechSpan sp = acal::find_speech(raw, WHISPER_SAMPLE_RATE);
            std::vector<float> pcm;
            if (sp.found) pcm.assign(raw.begin() + (long) sp.begin, raw.begin() + (long) sp.end);

            // R5-X: grade the VOICED duration; hand the model the padded span,
            // which is what the padding is for. `span` stays in the operator
            // line so a rejection is something they can act on: "4.6 s
            // captured, 0.9 s of it speech" says what to do differently.
            const float span = sp.found ? (float) pcm.size() / (float) WHISPER_SAMPLE_RATE : 0.0f;
            const float dur  = sp.found ? sp.voiced_s : 0.0f;
            const float rms  = acal::rms_of(pcm);
            const float clip = acal::clipped_fraction(pcm);
            const acal::TakeQuality q = acal::grade_take(dur, rms, clip, /*no_audio=*/!got_samples);
            if (!q.ok) {
                rejected++;
                std::printf("  %srejected%s — %s  (%.1fs captured, %.1fs of it speech, in %.1fs, rms %.3f, peak %.3f)\n",
                            R, N, q.why, span, dur,
                            (float) raw.size() / (float) WHISPER_SAMPLE_RATE, rms, peak);
                if (!got_samples) {
                    std::printf("  %sThe capture device is open but delivered no samples. This is not\n"
                                "  something you can fix by speaking differently. Quit, run with --list\n"
                                "  to see the devices, and select one explicitly with -c <id>.%s\n", Y, N);
                    return false;
                }
                continue;
            }


            float probs[9];
            if (!tagger.probe(pcm, probs)) {
                rejected++;
                std::printf("  %sthe model could not score that one%s — trying again\n", R, N);
                continue;
            }

            if (wctx) {
                const std::string heard = transcribe(wctx, pcm);
                if (!heard.empty()) std::printf("  heard: %s%s%s\n", D, heard.c_str(), N);
            }

            // file it: the intended class gets a TRUE reading, every other
            // class gets a FALSE reading from this same take
            for (size_t c = 0; c < N_CLASSES; c++) {
                const int gi = class_index(SCRIPT[c].cls);
                if (gi < 0) continue;
                if ((int) c == intended) trues[c].push_back(probs[gi]);
                else                     falses[c].push_back(probs[gi]);
            }
            if (share_calibration) {
                acal::Posterior recorded{};
                std::copy(probs, probs + 9, recorded.begin());
                if (intended >= 0) class_takes[class_index(pr.cls)].push_back(recorded);
                else flat_takes.push_back(recorded);
            }
            usable_takes++;

            if (intended >= 0) {
                const int gi = class_index(pr.cls);
                std::printf("  %s%-9s = %.3f%s   (%.1fs)\n", G, pr.cls, probs[gi < 0 ? 4 : gi], N, dur);
            } else {
                std::printf("  %sfiled as a neutral reference%s   (%.1fs)\n", D, N, dur);
            }
            return true;
        }
        std::printf("  %sskipping this one after four attempts%s\n", Y, N);
        return true;
    };

    // ── round 1: one neutral and one emotional take per class ───────────────
    std::printf("\n");
    rule();
    std::printf("%sRound 1 — the pairs%s\n", B, N);
    for (size_t c = 0; c < N_CLASSES; c++) {
        char b1[96], b2[96];
        std::snprintf(b1, sizeof(b1), "%zu of %zu · %s · flat", c + 1, N_CLASSES, SCRIPT[c].cls);
        std::snprintf(b2, sizeof(b2), "%zu of %zu · %s · in register", c + 1, N_CLASSES, SCRIPT[c].cls);
        if (!one_take(SCRIPT[c], -1, b1)) goto done;
        if (!one_take(SCRIPT[c], (int) c, b2)) goto done;
    }

    // ── round 2: spend remaining takes only where they buy something ────────
    {
        std::printf("\n");
        rule();
        std::printf("%sRound 2 — only the classes still undecided%s\n", B, N);
        bool any = false;
        for (size_t c = 0; c < N_CLASSES; c++) {
            const float maxf = share_calibration ? separation(c).max_false
                             : (falses[c].empty() ? 0.0f
                                : *std::max_element(falses[c].begin(), falses[c].end()));
            if (!acal::worth_eliciting(maxf)) {
                std::printf("\n  %s%s — its false readings already sit at %.3f; nothing can separate it%s\n",
                            D, SCRIPT[c].cls, maxf, N);
                continue;
            }
            acal::Separation s = separation(c);
            int want = acal::extra_takes_wanted(s, (int) trues[c].size(), p.budget);
            if (want > 0) any = true;
            for (int k = 0; k < want; k++) {
                char b[96];
                std::snprintf(b, sizeof(b), "%s · take %d (margin %.3f)",
                              SCRIPT[c].cls, (int) trues[c].size() + 1, s.margin);
                if (!one_take(SCRIPT[c], (int) c, b)) goto done;
                s = separation(c);
                if (acal::extra_takes_wanted(s, (int) trues[c].size(), p.budget) == 0) break;
            }
        }
        if (!any) std::printf("\n  %severy class settled in round 1 — nothing more to record%s\n", D, N);
    }

    // ── derive, validate, print ─────────────────────────────────────────────
    {
        std::vector<acal::ClassResult> results;
        const acal::ShareDerivation derived = share_calibration
            ? acal::derive_share_floors(class_takes, in_script, flat_takes, 2, calib_evidence)
            : acal::ShareDerivation{};
        std::printf("\n");
        rule();
        std::printf("%sResults%s\n\n", B, N);
        std::printf("  %-10s %6s %8s %9s %8s\n", "class", "takes", "min true", "max false", "floor");
        for (size_t c = 0; c < N_CLASSES; c++) {
            acal::ClassResult r;
            r.label = SCRIPT[c].cls;
            r.sep = share_calibration ? derived.sep[class_index(SCRIPT[c].cls)]
                                      : acal::separate(trues[c], falses[c]);
            if (share_calibration && r.sep.separable) {
                // Replay exactly the decimal value the operator will paste.
                char decimal[32];
                std::snprintf(decimal, sizeof decimal, "%.2f", r.sep.floor);
                r.sep.floor = std::strtof(decimal, nullptr);
            }
            r.retired = !r.sep.separable;
            results.push_back(r);
            if (r.sep.separable)
                std::printf("  %s%-10s%s %6zu %8.3f %9.3f %8.2f\n", G, r.label.c_str(), N,
                            r.sep.n_true, r.sep.min_true, r.sep.max_false, r.sep.floor);
            else
                std::printf("  %s%-10s%s %6zu %8.3f %9.3f %8s  %sretired%s\n", Y, r.label.c_str(), N,
                            r.sep.n_true, r.sep.min_true, r.sep.max_false, "1.01", D, N);
        }

        if (share_calibration) {
            EmotionTagger v;
            for (float &floor : v.floors) floor = p.base_floor;
            for (const auto &r : results)
                v.floors[class_index(r.label.c_str())] = r.retired ? acal::RETIRED : r.sep.floor;
            v.absolute_rule = false; v.retired_in_denom = false;
            v.share_floor = calib_share; v.evidence_floor = calib_evidence; v.noise_floor = calib_noise;
            const acal::ReplayResult replay = acal::replay_under_runtime_rule(
                class_takes, in_script, v.floors, calib_share, flat_takes, calib_evidence, calib_noise);
            int true_count = 0, true_emits = 0, false_emits = 0;
            auto verify = [&](const acal::Posterior &posterior, int intended) {
                const auto decision = v.decide(posterior.data());
                if (intended >= 0) ++true_count;
                if (decision.emit && decision.cls == intended) ++true_emits;
                if (decision.emit && decision.cls != intended) ++false_emits;
            };
            for (int c = 0; c < 9; ++c) if (in_script[c])
                for (const auto &take : class_takes[c]) verify(take, c);
            for (const auto &take : flat_takes) verify(take, -1);
            const bool replay_agrees = true_count == replay.n_true && true_emits == replay.emit_on_true &&
                                       false_emits == replay.emit_on_false;
            std::printf("\n  full-vector replay: %d of %d intended takes correctly tagged; %d false tags.\n"
                        "  %d takes below the evidence gate; derivation %s after %d iterations.\n",
                        true_emits, true_count, false_emits, derived.dropped_no_evidence,
                        derived.converged ? "converged" : "did not converge", derived.iterations);
            // A source of unsupported labels is not a usable calibration.
            // Preserve all measurements in the transcript, but emit no paste
            // block unless the rounded exported rule passes its actual replay.
            if (!derived.converged || !replay_agrees || true_emits == 0 || false_emits != 0) {
                std::printf("  No validated export: record additional clean takes or retain your current calibration.\n");
            } else {
                std::printf("\nPaste this AFTER existing emotion exports in your launcher:\n\n"
                            "export ATHENA_EMOTION_DEBUG=1\n"
                            "export ATHENA_EMOTION_ABSOLUTE=0\n"
                            "export ATHENA_EMOTION_RETIRED_IN_DENOM=0\n"
                            "export ATHENA_EMOTION_COLLAPSE_NEGATIVE=0\n"
                            "export ATHENA_EMOTION_EVIDENCE=%.2f\n"
                            "export ATHENA_EMOTION_NOISE=%.2f\n%s",
                            calib_evidence, calib_noise,
                            acal::export_block(results, p.base_floor, calib_share, acal::CALIB_SPACE).c_str());
            }
            std::printf("\n%d usable takes, %d rejected. No launcher was edited.\n", usable_takes, rejected);
        } else {
        // Validation: apply the derived floors to every take already recorded and
        // report what ATHENA would have done. This is the honest check — it uses
        // her decide(), not a reimplementation of it.
        {
            EmotionTagger v;
            for (int i = 0; i < 9; i++) v.floors[i] = p.base_floor;
            for (const auto &r : results) {
                const int gi = class_index(r.label.c_str());
                if (gi >= 0) v.floors[gi] = r.sep.separable ? r.sep.floor : acal::RETIRED;
            }
            int would_emit_on_neutral = 0, would_emit_on_true = 0, true_takes = 0;
            for (size_t c = 0; c < N_CLASSES; c++) {
                const int gi = class_index(SCRIPT[c].cls);
                if (gi < 0) continue;
                for (float pr : trues[c]) {
                    true_takes++;
                    float s[9] = {0,0,0,0,0,0,0,0,0};
                    s[gi] = pr;
                    if (v.decide(s).emit) would_emit_on_true++;
                }
                for (float pr : falses[c]) {
                    float s[9] = {0,0,0,0,0,0,0,0,0};
                    s[gi] = pr;
                    if (v.decide(s).emit) would_emit_on_neutral++;
                }
            }
            std::printf("\n  against the takes you just recorded, these floors would have emitted on\n"
                        "  %s%d of %d%s takes that WERE in register, and %s%d%s that were not.\n",
                        G, would_emit_on_true, true_takes, N,
                        would_emit_on_neutral ? R : G, would_emit_on_neutral, N);
            if (would_emit_on_neutral > 0)
                std::printf("  %sa false tag is far more expensive than a missed one — it changes what she\n"
                            "  says AND writes a false-valence memory. Consider raising those floors.%s\n", Y, N);
        }

        std::printf("\n");
        rule();
        std::printf("%sPaste this into launch-athena-397b.sh%s\n\n", B, N);
        std::printf("export ATHENA_EMOTION_DEBUG=1\n%s", acal::export_block(results, p.base_floor).c_str());
        if (acal::all_retired(results))
            std::printf("\n%severy class retired: on this voice and this microphone the tone channel is\n"
                        "honestly silent. That is a legitimate outcome — a channel that is wrong is\n"
                        "worse than one that says nothing, which is exactly what S6 demonstrated.%s\n", Y, N);
        std::printf("\n%s%d usable takes, %d rejected.%s\n\n", D, usable_takes, rejected, N);
        } // ATHENA_CALIB_SHARE=0: exact scalar/one-hot/export restore
    }

done:
    audio.pause();
    if (wctx) whisper_free(wctx);
    SDL_Quit();
    return 0;
}
