// orpheus-speak.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Minimal C++ CLI that turns text into speech via:
//   1. llama-server  (Orpheus 3B GGUF on GPU)  → SNAC audio tokens
//   2. ONNX Runtime  (SNAC 24 kHz decoder)      → 24 kHz PCM → WAV file
//
// Zero Python dependencies.  Requires libcurl + onnxruntime C API.
//
// Usage:
//   orpheus-speak [options] "Text to speak"
//   orpheus-speak [options] -f input.txt
//   orpheus-speak [options] -f input.txt -o output.wav
//
// Designed to be used as the --speak command for whisper.cpp/talk-llama.
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>

#include "athena_tts_wire.h"
#include "athena_prosody.h"   // r21-B: tonality and cadence, applied after decode
#include <onnxruntime_c_api.h>

// posix_spawn's environ (CHANGES.MD §13): the player is launched with posix_spawnp
// instead of fork()+execvp so glibc uses clone(CLONE_VM|CLONE_VFORK) and does NOT
// dup_mmap/pte_alloc-copy this process's CUDA/ONNX-laden address space — keeping
// orpheus-speak out of the kernel bad_page fork path the UVM churn poisons.
extern char **environ;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration defaults (override via CLI flags)
// ─────────────────────────────────────────────────────────────────────────────
// r24.16: unavailable players and failed/empty HTTP responses reproduced
// COMPLETE with no delivered audio. FAILED carries only whole lines whose
// complete waveforms drained through a successfully exited player. A pipe is
// not an acoustic microphone: physical audibility remains a live-session test.
// ATHENA_TTS_DELIVERY_PROOF=0 restores the former completion report exactly.
// Keep the independent TTS executable's existing three-header install contract.
// Match the brain's r26 master/feature policy without pulling model headers in.
static bool r26_speech_on() {
    const char *master = std::getenv("ATHENA_R26");
    const char *feature = std::getenv("ATHENA_R26_SPEECH");
    if (master && master[0] == '0') return false;
    if (feature && feature[0]) return feature[0] != '0';
    return master && master[0] && master[0] != '0';
}
static bool tts_token_bounds_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_TOKEN_BOUNDS");
        return !(e && e[0] == '0'); }();
    return on;
}
// A malformed long custom-token number overflowed signed int in BOTH scanners.
// Saturate outside the model vocabulary while consuming every digit, so it is
// rejected without corrupting the next token or the streaming partial prefix.
// ATHENA_TTS_TOKEN_BOUNDS=0 restores the historical scanner arithmetic.
static int tts_token_digit(int tok, char digit) {
    if (tts_token_bounds_on() && tok > 28682) return 28683;
    return tok * 10 + (digit - '0');
}
// r24.17: batch/legacy watch paths accepted non-finite or short decoder
// output, and write_wav checked its buffer before flushing. Check the shared
// ONNX PCM boundary and batch waveforms; WAV-only synthesis remains supported.
// ATHENA_TTS_PCM_PROOF=0 restores those previous batch proof boundaries.
static bool tts_pcm_proof_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_PCM_PROOF");
        return !(e && e[0] == '0'); }();
    return on;
}
static bool tts_pcm_proven(const std::vector<float> &pcm, size_t expected) {
    if (pcm.empty()) return false;
    if (!tts_pcm_proof_on()) return true;
    return pcm.size() == expected &&
        std::all_of(pcm.begin(), pcm.end(), [](float x) { return std::isfinite(x); });
}

static bool tts_delivery_proof_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_DELIVERY_PROOF");
        return !(e && e[0] == '0'); }();
    return on;
}

struct Config {
    std::string api_url       = "http://127.0.0.1:8080/completion";
    std::string snac_model    = "snac24_decoder.onnx";  // SNAC ONNX decoder path
    std::string voice         = "tara";
    std::string output_wav    = "/dev/shm/orpheus_tts.wav"; // RAM-backed tmpfs — no disk IO
    std::string input_file;                              // -f <file>
    std::string text;                                    // positional text
    std::string play_cmd      = "";                      // e.g. "aplay" or "ffplay -nodisp -autoexit"
    std::string watch_file;                              // --watch <file>: persistent daemon mode
    float       temperature   = 0.6f;
    float       top_p         = 0.9f;
    float       rep_penalty   = 1.1f;
    int         max_tokens    = 2500;
    int         sample_rate   = 24000;
    bool        verbose       = false;
    bool        diag          = false;  // real-time pipeline diagnostics: per-second
                                        // device-fill heartbeat + stall-run markers
                                        // (for tracing TTS underruns / server stalls)
    bool        snac_cpu      = false;  // force SNAC decoder to CPU (saves ~1.7 GB VRAM)
    // ── SSE streaming TTS (--stream-tts) ─────────────────────────────────────
    bool        stream_tts    = false;  // stream tokens via SSE + incremental SNAC decode
    std::string play_raw_cmd  = "aplay -q -t raw -f S16_LE -c 1 --buffer-time=300000";
    // r21-B: the prosody sidecar. talk-llama writes it BEFORE the trigger file,
    // so it is in place when the trigger fires and no text is ever mangled to
    // carry it. Empty, missing or malformed means the identity setting, which
    // is byte-for-byte stock behaviour — the feature fails OFF.
    std::string ctl_path      = "";
                                        // persistent raw-PCM sink; "-r <rate> -" appended
    int         prebuffer_ms  = 350;    // startup watermark: buffer this much audio before
                                        // the FIRST write of a session (absorbs the
                                        // turn-start generation deficit while Qwen is
                                        // contending for the GPU; never applies mid-turn)

    // ── Garble-diagnosis instrumentation (off by default) ───────────────────
    std::string capture_dir;            // --capture-dir DIR: per-session dump of raw tokens,
                                        // parsed codes, and the live decoded WAV (empty = off)
    std::string decode_codes;           // --decode-codes FILE: offline — re-decode a captured
                                        // .codes file to WAV (the discriminator), then exit
    std::string compare_a, compare_b;   // --compare-wav A B: print correlation/RMS verdict, exit
};

// ─────────────────────────────────────────────────────────────────────────────
// Orpheus special token IDs
// ─────────────────────────────────────────────────────────────────────────────
// The Orpheus tokenizer maps audio codes to custom_token_N where
// N = AUDIO_OFFSET + codebook_layer * 4096 + code_index
// custom_token_0 through custom_token_9 are special (SOH, EOT, etc.)
// Audio codes start at custom_token_10.
// See: https://github.com/canopyai/Orpheus-TTS
static constexpr int ORPHEUS_TOKEN_BASE   = 128266;  // vocab ID of first audio token
static constexpr int ORPHEUS_CODEBOOK_SZ  = 4096;
static constexpr int ORPHEUS_FRAME_TOKENS = 7;       // tokens per SNAC frame
static constexpr int ORPHEUS_AUDIO_OFFSET = 10;      // audio starts at <custom_token_10>

// Special framing tokens (not audio)
static constexpr int TOKEN_SOH = 128259;  // start-of-human
static constexpr int TOKEN_EOT = 128009;  // end-of-text
static constexpr int TOKEN_EOH = 128260;  // end-of-human
static constexpr int TOKEN_SOA = 128261;  // start-of-audio  (custom_token_5 sometimes)

// ─────────────────────────────────────────────────────────────────────────────
// WAV writer  (16-bit PCM, mono)
// ─────────────────────────────────────────────────────────────────────────────
static bool write_wav(const std::string &path, const std::vector<float> &pcm, int sr) {
    if (tts_pcm_proof_on() &&
        !std::all_of(pcm.begin(), pcm.end(), [](float x) { return std::isfinite(x); })) return false;
    const int num_samples  = (int)pcm.size();
    const int bits         = 16;
    const int byte_rate    = sr * bits / 8;
    const int data_bytes   = num_samples * (bits / 8);
    const int file_size    = 36 + data_bytes;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    // RIFF header
    f.write("RIFF", 4);
    auto write32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto write16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    write32(file_size);
    f.write("WAVE", 4);

    // fmt chunk
    f.write("fmt ", 4);
    write32(16);           // chunk size
    write16(1);            // PCM
    write16(1);            // mono
    write32(sr);           // sample rate
    write32(byte_rate);    // byte rate
    write16(bits / 8);     // block align
    write16(bits);         // bits per sample

    // data chunk
    f.write("data", 4);
    write32(data_bytes);

    for (float s : pcm) {
        float clamped = std::max(-1.0f, std::min(1.0f, s));
        int16_t i16   = static_cast<int16_t>(clamped * 32767.0f);
        f.write(reinterpret_cast<const char*>(&i16), 2);
    }

    if (tts_pcm_proof_on()) { f.flush(); f.close(); }
    return f.good();
}

// WAV-only long text has one output, not the player's two disposable buffers.
// Append one decoded chunk at a time: the HTTP/decode memory window stays
// bounded, and the header names only samples successfully flushed to the file.
static bool append_wav(const std::string &path, const std::vector<float> &pcm,
                       int sr, size_t previous_samples) {
    const size_t max_samples = (size_t(INT32_MAX) - 36) / 2;
    if (previous_samples > max_samples || pcm.size() > max_samples - previous_samples)
        return false;
    if (!previous_samples) return write_wav(path, pcm, sr);
    if (tts_pcm_proof_on() &&
        !std::all_of(pcm.begin(), pcm.end(), [](float x) { return std::isfinite(x); })) return false;
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return false;
    f.seekp(0, std::ios::end);
    if (f.tellp() != std::streamoff(44 + previous_samples * 2)) return false;
    for (float sample : pcm) {
        const float clamped = std::max(-1.0f, std::min(1.0f, sample));
        const int16_t value = static_cast<int16_t>(clamped * 32767.0f);
        f.write(reinterpret_cast<const char *>(&value), sizeof value);
    }
    f.flush();
    if (!f) return false;
    const uint32_t data_bytes = uint32_t((previous_samples + pcm.size()) * 2);
    const uint32_t file_size = 36 + data_bytes;
    f.seekp(4);
    f.write(reinterpret_cast<const char *>(&file_size), sizeof file_size);
    f.seekp(40);
    f.write(reinterpret_cast<const char *>(&data_bytes), sizeof data_bytes);
    f.flush();
    f.close();
    return f.good();
}

// WAV reader (16-bit PCM; multi-channel → channel 0). Returns samples in -1..1.
static bool read_wav(const std::string &path, std::vector<float> &out, int &sr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char tag[4];
    auto rd32 = [&](uint32_t &v) { f.read(reinterpret_cast<char*>(&v), 4); };
    auto rd16 = [&](uint16_t &v) { f.read(reinterpret_cast<char*>(&v), 2); };
    if (!f.read(tag, 4) || std::strncmp(tag, "RIFF", 4)) return false;
    uint32_t riff_sz; rd32(riff_sz);
    if (!f.read(tag, 4) || std::strncmp(tag, "WAVE", 4)) return false;
    uint16_t channels = 1, bits = 16; uint32_t srate = 24000; bool have_fmt = false;
    while (f.read(tag, 4)) {
        uint32_t csz = 0; rd32(csz);
        if (!f) return false;
        if (!std::strncmp(tag, "fmt ", 4)) {
            if (csz < 16) return false;
            uint16_t fmt, balign; uint32_t byterate;
            rd16(fmt); rd16(channels); rd32(srate); rd32(byterate); rd16(balign); rd16(bits);
            if (!f) return false;
            if (csz > 16) f.seekg(csz - 16, std::ios::cur);   // skip any fmt extension
            have_fmt = true;
        } else if (!std::strncmp(tag, "data", 4)) {
            if (!have_fmt || bits != 16 || channels == 0) return false;
            size_t nsamp = csz / 2;
            std::vector<int16_t> raw(nsamp);
            f.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)nsamp * 2);
            size_t got = (size_t)f.gcount() / 2;
            out.clear(); out.reserve(got / channels);
            for (size_t i = 0; i + channels <= got; i += channels)
                out.push_back(raw[i] / 32768.0f);            // channel 0
            sr = (int)srate;
            return true;
        } else {
            f.seekg(csz + (csz & 1), std::ios::cur);          // skip unknown chunk (pad to even)
        }
    }
    return false;
}

// ── --compare-wav A B: WAV similarity verdict (garble discriminator scoring) ──
// Pure C++ replacement for ad-hoc analysis: prints correlation, max diff, and RMS
// diff between two WAVs (e.g. a session's live .wav vs its --decode-codes .redecode.wav).
// Two clean SNAC decodes of the same codes correlate ~0.999; garble (structurally
// wrong audio) craters the correlation. A small lag search rules out a trivial
// sample misalignment masquerading as a mismatch.
static int compare_wavs(const std::string &pa, const std::string &pb) {
    std::vector<float> a, b; int sra = 0, srb = 0;
    if (!read_wav(pa, a, sra)) { std::cerr << "[orpheus-speak] cannot read WAV: " << pa << "\n"; return 1; }
    if (!read_wav(pb, b, srb)) { std::cerr << "[orpheus-speak] cannot read WAV: " << pb << "\n"; return 1; }
    std::cerr << "[orpheus-speak] compare-wav:\n"
              << "  A " << pa << "  (" << a.size() << " samp, " << sra << " Hz, "
              << (sra ? (float)a.size() / sra : 0) << " s)\n"
              << "  B " << pb << "  (" << b.size() << " samp, " << srb << " Hz, "
              << (srb ? (float)b.size() / srb : 0) << " s)\n";
    if (a.empty() || b.empty()) { std::cerr << "  empty signal\n"; return 1; }
    if (a.size() != b.size())
        std::cerr << "  NOTE: length differs by " << (long)a.size() - (long)b.size()
                  << " samples (truncation/abort?)\n";

    auto corr_at = [&](int lag) -> double {
        size_t ia = lag >= 0 ? 0 : (size_t)(-lag);
        size_t ib = lag >= 0 ? (size_t)lag : 0;
        // r24.16: tiny diagnostic WAVs made these unsigned subtractions
        // wrap before the minimum-length check, then indexed outside both
        // recordings. Check the offsets before subtracting; normal WAVs and
        // all valid overlapping lags keep identical arithmetic and output.
        if (ia >= a.size() || ib >= b.size()) return -2.0;
        size_t n = std::min(a.size() - ia, b.size() - ib);
        if (n < 16) return -2.0;
        double sa = 0, sb = 0;
        for (size_t i = 0; i < n; i++) { sa += a[ia + i]; sb += b[ib + i]; }
        double ma = sa / n, mb = sb / n, num = 0, da = 0, db = 0;
        for (size_t i = 0; i < n; i++) {
            double x = a[ia + i] - ma, y = b[ib + i] - mb;
            num += x * y; da += x * x; db += y * y;
        }
        return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
    };
    int best_lag = 0; double best = -2.0;
    for (int lag = -32; lag <= 32; ++lag) { double c = corr_at(lag); if (c > best) { best = c; best_lag = lag; } }

    size_t ia = best_lag >= 0 ? 0 : (size_t)(-best_lag);
    size_t ib = best_lag >= 0 ? (size_t)best_lag : 0;
    size_t n = std::min(a.size() - ia, b.size() - ib);
    double sumd2 = 0, suma2 = 0; int maxd = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[ia + i] - b[ib + i];
        sumd2 += d * d; suma2 += (double)a[ia + i] * a[ia + i];
        int di = (int)(std::fabs(d) * 32768.0 + 0.5); if (di > maxd) maxd = di;
    }
    double rms = std::sqrt(sumd2 / n), srms = std::sqrt(suma2 / n);
    std::cerr << "  overlap     : " << n << " samp (best lag " << best_lag << ")\n"
              << "  correlation : " << best << "\n"
              << "  max |diff|  : " << maxd << " / 32768\n"
              << "  rms diff    : " << (srms > 0 ? 100.0 * rms / srms : 0.0) << "% of signal\n"
              << "  verdict     : "
              << (best >= 0.995 ? "MATCH — within SNAC decode noise (clean)"
                : best >= 0.95  ? "PARTIAL divergence — inspect (localized corruption?)"
                                : "MISMATCH — structurally different (garble: bad codes or on-GPU decode)")
              << "\n  (baseline: two clean decodes correlate ~0.999; garble drops it hard)\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// libcurl helpers
// ─────────────────────────────────────────────────────────────────────────────
static size_t curl_write_cb(char *data, size_t size, size_t nmemb, void *userp) {
    auto *buf = static_cast<std::string *>(userp);
    buf->append(data, size * nmemb);
    return size * nmemb;
}

// Build the Orpheus prompt format:
//   <custom_token_3><|begin_of_text|>{voice}: {text}<|eot_id|>
//   <custom_token_4><custom_token_5><custom_token_1>
//
// These map to token IDs in the Orpheus/Llama tokenizer:
//   <custom_token_3> = 128259 (start-of-human)
//   <|begin_of_text|> = standard Llama BOS
//   <|eot_id|>       = 128009 (end-of-text)
//   <custom_token_4> = 128260 (end-of-human)
//   <custom_token_5> = 128261 (start-of-audio)
//   <custom_token_1> = 128257 (audio generation marker)
static std::string build_orpheus_prompt(const std::string &voice, const std::string &text) {
    return "<custom_token_3><|begin_of_text|>" + voice + ": " + text +
           "<|eot_id|><custom_token_4><custom_token_5><custom_token_1>";
}

// Build the JSON body for /completion
static std::string build_request_json(const Config &cfg, const std::string &prompt,
                                      bool stream = false) {
    // Manual JSON construction to avoid external dependency.
    // Escape any quotes/backslashes in the prompt.
    std::string escaped;
    for (char c : prompt) {
        if (c == '"')       escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c == '\t') escaped += "\\t";
        else                escaped += c;
    }

    std::ostringstream js;
    js << "{"
       << "\"prompt\":\"" << escaped << "\","
       << "\"n_predict\":" << cfg.max_tokens << ","
       << "\"temperature\":" << cfg.temperature << ","
       << "\"top_p\":" << cfg.top_p << ","
       << "\"repeat_penalty\":" << cfg.rep_penalty << ","
       << "\"stop\":[\"<|eot_id|>\"],"
       << "\"parse_special\":true,"
       << "\"stream\":" << (stream ? "true" : "false")
       << "}";
    return js.str();
}

// POST to llama-server and return the raw response body.
// Uses thread_local CURL handle — allocated once per thread, reused across
// calls.  Keeps TCP connection alive to llama-server and avoids per-request
// handle allocation (~0.1-0.2ms savings per call, compounds over 10+ chunks).
static bool http_post(const std::string &url, const std::string &body,
                      std::string &response, bool verbose) {
    // 14.1: LOCAL handle + cleanup. The batch/legacy paths launch this via
    // std::async — one FRESH thread per chunk (no pool in libstdc++), and
    // CURL*/curl_slist* are trivially destructible, so thread_local leaked
    // one easy handle, its keep-alive socket fd, and one slist PER CHUNK —
    // EMFILE after an evening of turns, TTS silently dead. The SSE path's own
    // comment documents this exact mechanism as the reason it does not use
    // thread_local; now neither does this.
    struct CurlHold {
        CURL *h = nullptr;
        struct curl_slist *hdr = nullptr;
        ~CurlHold() {
            if (hdr) curl_slist_free_all(hdr);
            if (h)   curl_easy_cleanup(h);
        }
    } hold;
    hold.h = curl_easy_init();
    if (!hold.h) { std::cerr << "[orpheus-speak] curl_easy_init failed\n"; return false; }
    hold.hdr = curl_slist_append(nullptr, "Content-Type: application/json");
    hold.hdr = curl_slist_append(hold.hdr, "Connection: keep-alive");
    CURL *curl = hold.h;
    struct curl_slist *headers = hold.hdr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);      // disable Nagle — lower latency
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);     // keep TCP connection alive

    if (verbose) {
        std::cerr << "[orpheus-speak] POST " << url << "\n";
        std::cerr << "[orpheus-speak] body: " << body.substr(0, 200) << "...\n";
    }

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        std::cerr << "[orpheus-speak] curl error: " << curl_easy_strerror(res) << "\n";
        return false;
    }

    if (http_code != 200) {
        if (verbose)
            std::cerr << "[orpheus-speak] HTTP " << http_code
                      << " (response: " << response.substr(0, 120) << ")\n";
        response.clear();  // treat non-200 as empty
        return true;        // curl succeeded, but server rejected
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Async HTTP generation (thread-safe — no SNAC, no shared state)
// ─────────────────────────────────────────────────────────────────────────────
struct HttpResult {
    std::string response;
    float       server_ms;
    bool        ok;
};

static HttpResult http_generate(const std::string &api_url,
                                const std::string &voice,
                                const std::string &chunk_text,
                                const Config      &cfg,
                                bool               verbose) {
    std::string prompt = build_orpheus_prompt(voice, chunk_text);
    std::string body   = build_request_json(cfg, prompt);

    // Retry loop — server may return 503 when slots are momentarily busy
    // during concurrent submission.  Backoff gives the server time to
    // finish a prior request and free a slot.
    constexpr int max_retries = 3;

    for (int attempt = 0; attempt < max_retries; attempt++) {
        std::string response;

        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok  = http_post(api_url, body, response, verbose && attempt == 0);
        auto t1 = std::chrono::high_resolution_clock::now();

        float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

        if (ok && !response.empty()) {
            return { std::move(response), ms, true };
        }

        // Failed — retry with backoff (500ms, 1000ms)
        if (attempt + 1 < max_retries) {
            int backoff = 500 * (attempt + 1);
            std::cerr << "[orpheus-speak] slot busy, retry " << attempt + 1
                      << "/" << max_retries << " in " << backoff << "ms\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
        }
    }

    return { "", 0, false };
}

// ─────────────────────────────────────────────────────────────────────────────
// Token extraction
// ─────────────────────────────────────────────────────────────────────────────
// Extract custom_token IDs from the LLM response text.
// Orpheus outputs tokens like: <custom_token_28631> <custom_token_5043> ...
// We extract the integer N from each <custom_token_N>.
static std::vector<int> extract_audio_tokens(const std::string &text, bool verbose) {
    std::vector<int> tokens;
    tokens.reserve(512);  // typical response size

    // Hand-rolled scanner — much faster than std::regex for large responses.
    // Matches: <custom_token_DIGITS>
    static const char prefix[] = "<custom_token_";
    static const size_t plen = sizeof(prefix) - 1;

    size_t pos = 0;
    while (pos < text.size()) {
        pos = text.find(prefix, pos);
        if (pos == std::string::npos) break;
        pos += plen;

        // Parse digits
        int tok = 0;
        bool has_digits = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            tok = tts_token_digit(tok, text[pos]);
            has_digits = true;
            pos++;
        }

        // Must end with '>'
        if (!has_digits || pos >= text.size() || text[pos] != '>') continue;
        pos++; // skip '>'

        // Audio tokens start at <custom_token_10> (tokens 0-9 are special)
        if (tok >= ORPHEUS_AUDIO_OFFSET) {
            int code = tok - ORPHEUS_AUDIO_OFFSET;
            if (code < ORPHEUS_FRAME_TOKENS * ORPHEUS_CODEBOOK_SZ) {
                tokens.push_back(code);
            }
        }
    }

    if (verbose) {
        std::cerr << "[orpheus-speak] extracted " << tokens.size()
                  << " audio tokens from response\n";
    }
    return tokens;
}

// Extract the "content" or "text" field value from a JSON response.
// Handles both native /completion format: {"content":"..."}
// and OAI-compatible format: {"choices":[{"text":"..."}]}
static std::string extract_text_from_json(const std::string &json) {
    // Find "content": first (native /completion endpoint), then "text": (OAI compat)
    auto pos = json.find("\"content\":");
    if (pos == std::string::npos) {
        pos = json.find("\"text\":");
        if (pos == std::string::npos) return "";
    }

    // Find the colon after the key, then the opening quote of the value
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    pos++; // skip opening quote

    // Read until unescaped closing quote
    std::string result;
    for (size_t i = pos; i < json.size(); i++) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            i++;
            switch (json[i]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += '\\'; result += json[i]; break;
            }
        } else if (json[i] == '"') {
            break;
        } else {
            result += json[i];
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sentence splitter for chunked TTS
// ─────────────────────────────────────────────────────────────────────────────
// Split text at sentence boundaries so each sentence can be sent to Orpheus
// independently. This avoids attention skip issues on long text and keeps
// each request within the context window.
static std::vector<std::string> split_sentences(const std::string &text) {
    std::vector<std::string> sentences;
    std::string current;

    for (size_t i = 0; i < text.size(); i++) {
        current += text[i];

        bool is_terminal = (text[i] == '.' || text[i] == '?' || text[i] == '!');
        bool at_end      = (i + 1 >= text.size());
        bool next_space  = (!at_end && (text[i + 1] == ' ' || text[i + 1] == '\n'));
        // 14.1: an abbreviation's period is not a sentence — "Talk to Dr.
        // Smith tomorrow." was splitting into two Orpheus requests, each with
        // sentence-final falling intonation and a full breath pause between
        // "Dr." and "Smith": an authoritative period in the middle of a name.
        if (is_terminal && text[i] == '.' && !at_end) {
            static const char *kAbbr[] = {
                "dr", "mr", "mrs", "ms", "st", "vs", "etc", "e.g", "i.e",
                "prof", "jr", "sr", "approx",
            };
            size_t ws = i;
            while (ws > 0 && text[ws - 1] != ' ' && text[ws - 1] != '\n') ws--;
            std::string tok = text.substr(ws, i - ws);
            for (char &c2 : tok)
                if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 - 'A' + 'a');
            for (const char *a : kAbbr)
                if (tok == a) { is_terminal = false; break; }
        }
        bool at_boundary = is_terminal && (at_end || next_space);

        // Don't split fragments shorter than ~3 words
        if (at_boundary && current.size() > 15) {
            size_t start = current.find_first_not_of(" \n\r\t");
            if (start != std::string::npos) {
                sentences.push_back(current.substr(start));
            }
            current.clear();
        }
    }

    // Remaining text
    if (!current.empty()) {
        size_t start = current.find_first_not_of(" \n\r\t");
        if (start != std::string::npos) {
            std::string remainder = current.substr(start);
            if (!sentences.empty() && remainder.size() < 15) {
                sentences.back() += " " + remainder;
            } else {
                sentences.push_back(remainder);
            }
        }
    }

    if (sentences.empty() && !text.empty()) {
        sentences.push_back(text);
    }

    return sentences;
}

// ─────────────────────────────────────────────────────────────────────────────
// SNAC token deinterleaving
// ─────────────────────────────────────────────────────────────────────────────
// Orpheus produces 7 tokens per SNAC frame in this interleaved order:
//   [L0, L1a, L2a, L2b, L1b, L2c, L2d]
//
// We deinterleave into 3 codebook layers:
//   codes0 (L0):  1 code  per frame  → length = num_frames
//   codes1 (L1):  2 codes per frame  → length = num_frames * 2
//   codes2 (L2):  4 codes per frame  → length = num_frames * 4
//
// Each code value = raw_token_id % 4096  (the codebook index)
// The layer is encoded as raw_token_id / 4096.
struct SnacCodes {
    std::vector<int64_t> codes0;  // [num_frames]
    std::vector<int64_t> codes1;  // [num_frames * 2]
    std::vector<int64_t> codes2;  // [num_frames * 4]
};

static SnacCodes deinterleave_tokens(const std::vector<int> &tokens, bool verbose) {
    SnacCodes codes;
    int num_frames = (int)tokens.size() / ORPHEUS_FRAME_TOKENS;

    if (verbose) {
        std::cerr << "[orpheus-speak] " << tokens.size() << " tokens → "
                  << num_frames << " SNAC frames ("
                  << (float)num_frames / 11.71875f << "s audio)\n";   // 24000/2048 = 11.72 frames/s
    }

    // 14.1: euclidean fold — C++ '%' truncates toward zero, so a wrong-layer
    // glitch token (the OS2 garble class) produced a NEGATIVE code handed to
    // ONNX Gather (EP-dependent behavior). Fold to [0, SZ): a glitch now
    // degrades to wrong-but-valid audio instead of undefined decoding.
    auto fold = [](int v) {
        const int m = v % ORPHEUS_CODEBOOK_SZ;
        return m < 0 ? m + ORPHEUS_CODEBOOK_SZ : m;
    };
    for (int i = 0; i < num_frames; i++) {
        int base = i * ORPHEUS_FRAME_TOKENS;

        // Layer 0: 1 code per frame
        codes.codes0.push_back(fold(tokens[base + 0]));

        // Layer 1: 2 codes per frame (positions 1, 4)
        codes.codes1.push_back(fold(tokens[base + 1] - 1 * ORPHEUS_CODEBOOK_SZ));
        codes.codes1.push_back(fold(tokens[base + 4] - 4 * ORPHEUS_CODEBOOK_SZ));

        // Layer 2: 4 codes per frame (positions 2, 3, 5, 6)
        codes.codes2.push_back(fold(tokens[base + 2] - 2 * ORPHEUS_CODEBOOK_SZ));
        codes.codes2.push_back(fold(tokens[base + 3] - 3 * ORPHEUS_CODEBOOK_SZ));
        codes.codes2.push_back(fold(tokens[base + 5] - 5 * ORPHEUS_CODEBOOK_SZ));
        codes.codes2.push_back(fold(tokens[base + 6] - 6 * ORPHEUS_CODEBOOK_SZ));
    }

    return codes;
}

// ─────────────────────────────────────────────────────────────────────────────
// ONNX Runtime: SNAC decode  (codes → PCM float32)
// ─────────────────────────────────────────────────────────────────────────────
#define ORT_CHECK(expr) do {                                             \
    OrtStatus *_s = (expr);                                              \
    if (_s) {                                                            \
        const char *msg = g_ort->GetErrorMessage(_s);                    \
        std::cerr << "[orpheus-speak] ORT error: " << msg << "\n";       \
        g_ort->ReleaseStatus(_s);                                        \
        return {};                                                       \
    }                                                                    \
} while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Persistent SNAC decoder — init once, decode many times
// ─────────────────────────────────────────────────────────────────────────────
struct SnacDecoder {
    const OrtApi *g_ort = nullptr;
    OrtEnv *env = nullptr;
    OrtSessionOptions *opts = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *mem_info = nullptr;
    OrtAllocator *allocator = nullptr;
    OrtRunOptions *run_opts = nullptr;      // Fix 1: arena shrinkage per-Run

    const char *in_names[3] = {};
    const char *out_names[1] = {};
    bool is_dynamic = false;
    bool has_cuda_ep = false;
    bool ok = false;

    // Fix 3: pre-allocated padded buffers for dynamic model.
    // Each SNAC codes0 frame produces 2048 audio samples at 24 kHz.
    // MAX_C0=1024 covers ~42 seconds of audio per decode call — well beyond
    // any chunked utterance.  If exceeded, falls back to fresh allocation.
    static constexpr int64_t MAX_C0 = 1024;
    static constexpr int64_t MAX_C1 = MAX_C0 * 2;
    static constexpr int64_t MAX_C2 = MAX_C0 * 4;
    static constexpr int SAMPLES_PER_FRAME = 2048;
    std::vector<int64_t> buf_c0, buf_c1, buf_c2;

    void init(const Config &cfg) {
        cleanup();
        g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        if (!g_ort) {
            std::cerr << "[orpheus-speak] ORT API version unavailable\n";
            return;
        }
        // Failed setup owns every handle acquired so far, even when main
        // returns immediately. Never use an output from a failed API call.
        struct InitGuard {
            SnacDecoder &decoder;
            ~InitGuard() { if (!decoder.ok) decoder.cleanup(); }
        } guard{*this};
        auto good = [&](OrtStatus *status) {
            if (!status) return true;
            std::cerr << "[orpheus-speak] ORT init error: "
                      << g_ort->GetErrorMessage(status) << "\n";
            g_ort->ReleaseStatus(status);
            return false;
        };

        if (!good(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "orpheus-speak", &env))) return;
        if (!good(g_ort->CreateSessionOptions(&opts))) return;

        if (!good(g_ort->SetIntraOpNumThreads(opts, 0))) return;
        if (!good(g_ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL))) return;

        // Fix 2: disable CPU memory arena — prevents unbounded growth from
        // dynamic input shapes.  Uses raw malloc/free per tensor instead.
        // The SNAC model is small (~50 MB) and tensors are tiny, so the
        // per-call malloc overhead is negligible vs. compute time.
        if (!good(g_ort->DisableCpuMemArena(opts))) return;

        // Cache the optimized graph to disk when available. A cache-option
        // failure is not a failed decoder, but still owns an ORT status.
        std::string opt_path = cfg.snac_model + ".optimized";
        good(g_ort->SetOptimizedModelFilePath(opts, opt_path.c_str()));

        // CUDA EP (skip if --snac-cpu requested)
        if (!cfg.snac_cpu) {
            OrtCUDAProviderOptions cuda_opts{};
            cuda_opts.device_id = 0;
            // kSameAsRequested (1) prevents power-of-two over-allocation.
            // Combined with Fix 3 (fixed shapes), the GPU arena stabilizes
            // after the first Run() and never grows.
            cuda_opts.arena_extend_strategy = 1;
            cuda_opts.do_copy_in_default_stream = 1;
            OrtStatus *cs = g_ort->SessionOptionsAppendExecutionProvider_CUDA(opts, &cuda_opts);
            if (cs) {
                if (cfg.verbose) std::cerr << "[orpheus-speak] CUDA EP unavailable, using CPU\n";
                g_ort->ReleaseStatus(cs);
            } else {
                has_cuda_ep = true;
                if (cfg.verbose) std::cerr << "[orpheus-speak] SNAC using CUDA EP\n";
            }
        } else {
            std::cerr << "[orpheus-speak] SNAC using CPU (--snac-cpu)\n";
        }

        OrtStatus *css = g_ort->CreateSession(env, cfg.snac_model.c_str(), opts, &session);
        if (css) {
            // Previously this discarded the status and returned "SNAC decoder init
            // failed" with no reason. Surface the actual ORT/CUDA error...
            std::cerr << "[orpheus-speak] SNAC CreateSession failed: "
                      << g_ort->GetErrorMessage(css) << "\n";
            g_ort->ReleaseStatus(css);
            session = nullptr;
            // ...and if the CUDA EP was in play, retry on CPU rather than killing
            // the daemon. A CUDA-EP session failure is almost always a context/MPS
            // problem (e.g. the MPS server down); CPU SNAC is slower but keeps TTS
            // alive. has_cuda_ep is cleared so the RunOptions below don't request a
            // GPU memory arena that no longer exists.
            if (!has_cuda_ep) return;
            std::cerr << "[orpheus-speak] SNAC: falling back to CPU EP\n";
            has_cuda_ep = false;
            if (opts) { g_ort->ReleaseSessionOptions(opts); opts = nullptr; }
            if (!good(g_ort->CreateSessionOptions(&opts))) return;
            if (!good(g_ort->SetIntraOpNumThreads(opts, 0))) return;
            if (!good(g_ort->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL))) return;
            if (!good(g_ort->DisableCpuMemArena(opts))) return;
            OrtStatus *css2 = g_ort->CreateSession(env, cfg.snac_model.c_str(), opts, &session);
            if (css2) {
                std::cerr << "[orpheus-speak] SNAC CPU CreateSession also failed: "
                          << g_ort->GetErrorMessage(css2) << "\n";
                g_ort->ReleaseStatus(css2);
                session = nullptr;
                return;
            }
            std::cerr << "[orpheus-speak] SNAC running on CPU EP (fallback)\n";
        }

        // Fix 2: use OrtDeviceAllocator (raw malloc/free) instead of
        // OrtArenaAllocator which grows monotonically with dynamic shapes
        if (!good(g_ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &mem_info))) return;
        if (!good(g_ort->GetAllocatorWithDefaultOptions(&allocator))) return;

        // Fix 1: create RunOptions with arena shrinkage for any active arena.
        // CPU arena is disabled by Fix 2, so only request GPU shrinkage when
        // CUDA EP is active.  Requesting shrinkage on a non-existent arena
        // causes ORT to error on every Run() call.
        if (!good(g_ort->CreateRunOptions(&run_opts))) return;
        if (has_cuda_ep) {
            if (!good(g_ort->AddRunConfigEntry(run_opts,
                "memory.enable_memory_arena_shrinkage", "gpu:0"))) return;
        }

        // Read input/output names from model
        for (size_t i = 0; i < 3; ++i) {
            char *name = nullptr;
            if (!good(g_ort->SessionGetInputName(session, i, allocator, &name))) return;
            in_names[i] = name;
            if (!name) return;
        }
        char *no = nullptr;
        if (!good(g_ort->SessionGetOutputName(session, 0, allocator, &no))) return;
        out_names[0] = no;
        if (!no) return;

        // Detect static vs dynamic
        OrtTypeInfo *ti = nullptr;
        if (!good(g_ort->SessionGetInputTypeInfo(session, 0, &ti)) || !ti) return;
        std::unique_ptr<OrtTypeInfo, decltype(g_ort->ReleaseTypeInfo)> type_owner(ti, g_ort->ReleaseTypeInfo);
        const OrtTensorTypeAndShapeInfo *si = nullptr;
        if (!good(g_ort->CastTypeInfoToTensorInfo(ti, &si)) || !si) return;
        size_t dc = 0;
        if (!good(g_ort->GetDimensionsCount(si, &dc))) return;
        std::vector<int64_t> dims(dc);
        if (!good(g_ort->GetDimensions(si, dims.data(), dc))) return;
        is_dynamic = (dc < 2 || dims[1] <= 0);

        // Fix 3: pre-allocate padded buffers for dynamic model
        if (is_dynamic) {
            buf_c0.resize(MAX_C0, 0);
            buf_c1.resize(MAX_C1, 0);
            buf_c2.resize(MAX_C2, 0);
        }

        if (cfg.verbose) {
            std::cerr << "[orpheus-speak] SNAC model: "
                      << (is_dynamic ? "dynamic" : "static")
                      << ", inputs: " << in_names[0] << ", " << in_names[1] << ", " << in_names[2] << "\n";
        }

        ok = true;
    }

    // Serializes ORT Run() calls.  The batch path was single-threaded by
    // construction; the SSE streaming path decodes from multiple chunk
    // threads concurrently, so all decodes funnel through this mutex.
    std::mutex run_mutex;

    // Core decode: run the session on exact buffers, append PCM to `out`.
    // Returns false on ORT error.
    bool run_decode(const Config &cfg, int64_t *d0, int64_t n0, int64_t *d1, int64_t n1,
                    int64_t *d2, int64_t n2, std::vector<float> &out) {
        std::lock_guard<std::mutex> lk(run_mutex);
        int64_t s0[] = {1, n0}, s1[] = {1, n1}, s2[] = {1, n2};
        OrtValue *inputs[3] = {};
        OrtStatus *st;
        if (tts_pcm_proof_on()) {
            // r24.17: valid Run status is not proof of float PCM. The old
            // readers ignored metadata failures and interpreted uint8 output
            // as floats. Own every result until its contract has been checked.
            OrtValue *output = nullptr;
            OrtTensorTypeAndShapeInfo *info = nullptr;
            auto release = [&]() {
                if (info) g_ort->ReleaseTensorTypeAndShapeInfo(info);
                if (output) g_ort->ReleaseValue(output);
                for (auto *v : inputs) if (v) g_ort->ReleaseValue(v);
            };
            auto good = [&](OrtStatus *status) {
                if (!status) return true;
                if (cfg.verbose) std::cerr << "[orpheus-speak] ORT PCM error: "
                                          << g_ort->GetErrorMessage(status) << "\n";
                g_ort->ReleaseStatus(status);
                return false;
            };
            if (!good(g_ort->CreateTensorWithDataAsOrtValue(mem_info, d0, n0*8, s0, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[0])) || !inputs[0] ||
                !good(g_ort->CreateTensorWithDataAsOrtValue(mem_info, d1, n1*8, s1, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[1])) || !inputs[1] ||
                !good(g_ort->CreateTensorWithDataAsOrtValue(mem_info, d2, n2*8, s2, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[2])) || !inputs[2] ||
                !good(g_ort->Run(session, run_opts, in_names, (const OrtValue *const *)inputs, 3, out_names, 1, &output)) || !output ||
                !good(g_ort->GetTensorTypeAndShape(output, &info)) || !info) {
                release(); return false;
            }
            size_t ne = 0;
            ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
            float *pcm = nullptr;
            const bool valid = good(g_ort->GetTensorElementType(info, &type)) &&
                type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
                good(g_ort->GetTensorShapeElementCount(info, &ne)) &&
                n0 > 0 && ne == (size_t)n0 * SAMPLES_PER_FRAME &&
                good(g_ort->GetTensorMutableData(output, (void**)&pcm)) && pcm &&
                std::all_of(pcm, pcm + ne, [](float x) { return std::isfinite(x); });
            if (valid) out.insert(out.end(), pcm, pcm + ne);
            release();
            return valid;
        }
        st = g_ort->CreateTensorWithDataAsOrtValue(mem_info, d0, n0*8, s0, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[0]);
        if (st) g_ort->ReleaseStatus(st);
        st = g_ort->CreateTensorWithDataAsOrtValue(mem_info, d1, n1*8, s1, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[1]);
        if (st) g_ort->ReleaseStatus(st);
        st = g_ort->CreateTensorWithDataAsOrtValue(mem_info, d2, n2*8, s2, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[2]);
        if (st) g_ort->ReleaseStatus(st);

        OrtValue *output = nullptr;
        OrtStatus *rs = g_ort->Run(session, run_opts, in_names, (const OrtValue *const *)inputs, 3, out_names, 1, &output);
        if (rs) {
            if (cfg.verbose) std::cerr << "[orpheus-speak] ORT Run error: " << g_ort->GetErrorMessage(rs) << "\n";
            g_ort->ReleaseStatus(rs);
            for (auto &v : inputs) if (v) g_ort->ReleaseValue(v);
            return false;
        }

        float *pcm = nullptr;
        g_ort->GetTensorMutableData(output, (void**)&pcm);
        OrtTensorTypeAndShapeInfo *info = nullptr;
        g_ort->GetTensorTypeAndShape(output, &info);
        size_t ne = 0; g_ort->GetTensorShapeElementCount(info, &ne);
        g_ort->ReleaseTensorTypeAndShapeInfo(info);

        out.insert(out.end(), pcm, pcm + ne);
        g_ort->ReleaseValue(output);
        for (auto &v : inputs) g_ort->ReleaseValue(v);
        return true;
    }

    // Decode codes at their EXACT shape (no MAX_C0 padding).  Used by the
    // SSE streaming path, where windows are a constant 4 frames — a stable
    // tiny shape that ORT caches after the first call, making each decode
    // a few ms instead of the padded ~300-400 ms.
    std::vector<float> decode_exact(const Config &cfg, const SnacCodes &codes) {
        if (!ok) return {};
        std::vector<float> out;
        std::vector<int64_t> c0(codes.codes0), c1(codes.codes1), c2(codes.codes2);
        if (c0.empty()) return out;
        run_decode(cfg, c0.data(), (int64_t)c0.size(), c1.data(), (int64_t)c1.size(),
                   c2.data(), (int64_t)c2.size(), out);
        return out;
    }

    std::vector<float> decode(const Config &cfg, const SnacCodes &codes) {
        if (!ok) return {};
        std::vector<float> all_pcm;

        auto run_one = [&](int64_t *d0, int64_t n0, int64_t *d1, int64_t n1,
                           int64_t *d2, int64_t n2) {
            return run_decode(cfg, d0, n0, d1, n1, d2, n2, all_pcm);
        };

        if (is_dynamic) {
            int64_t actual_n0 = (int64_t)codes.codes0.size();
            int64_t actual_n1 = (int64_t)codes.codes1.size();
            int64_t actual_n2 = (int64_t)codes.codes2.size();

            if (actual_n0 <= MAX_C0) {
                // Fix 3: use pre-allocated padded buffers — the arena sees
                // the same tensor shape [1, MAX_C0/C1/C2] every time, so no
                // new allocations and no growth.  Zero-pad unused slots;
                // truncate output PCM to match actual input length.
                std::memset(buf_c0.data(), 0, MAX_C0 * sizeof(int64_t));
                std::memset(buf_c1.data(), 0, MAX_C1 * sizeof(int64_t));
                std::memset(buf_c2.data(), 0, MAX_C2 * sizeof(int64_t));
                std::memcpy(buf_c0.data(), codes.codes0.data(), actual_n0 * sizeof(int64_t));
                std::memcpy(buf_c1.data(), codes.codes1.data(), actual_n1 * sizeof(int64_t));
                std::memcpy(buf_c2.data(), codes.codes2.data(), actual_n2 * sizeof(int64_t));
                const bool decoded = run_one(buf_c0.data(), MAX_C0, buf_c1.data(), MAX_C1, buf_c2.data(), MAX_C2);
                if (!decoded && tts_pcm_proof_on()) return {};
                // Truncate: SNAC produces SAMPLES_PER_FRAME samples per codes0 frame
                int64_t actual_samples = actual_n0 * SAMPLES_PER_FRAME;
                if ((int64_t)all_pcm.size() > actual_samples)
                    all_pcm.resize(actual_samples);
            } else {
                // Fallback for extremely long audio (>42s per chunk)
                std::vector<int64_t> c0(codes.codes0), c1(codes.codes1), c2(codes.codes2);
                const bool decoded = run_one(c0.data(), c0.size(), c1.data(), c1.size(), c2.data(), c2.size());
                if (!decoded && tts_pcm_proof_on()) return {};
            }
        } else {
            static constexpr int W0 = 12, W1 = 24, W2 = 48, SPW = 24576;
            int total = (int)codes.codes0.size();
            int nwin = (total + W0 - 1) / W0;
            all_pcm.reserve(nwin * SPW);
            for (int w = 0; w < nwin; w++) {
                std::vector<int64_t> c0(W0, 0), c1(W1, 0), c2(W2, 0);
                int o0 = w*W0, o1 = w*W1, o2 = w*W2;
                int n0 = std::min(W0, total - o0);
                int n1 = std::min(W1, (int)codes.codes1.size() - o1);
                int n2 = std::min(W2, (int)codes.codes2.size() - o2);
                for (int i = 0; i < n0; i++) c0[i] = codes.codes0[o0+i];
                for (int i = 0; i < n1; i++) c1[i] = codes.codes1[o1+i];
                for (int i = 0; i < n2; i++) c2[i] = codes.codes2[o2+i];
                size_t prev = all_pcm.size();
                const bool decoded = run_one(c0.data(), W0, c1.data(), W1, c2.data(), W2);
                if (!decoded && tts_pcm_proof_on()) return {};
                if (tts_pcm_proof_on() && all_pcm.size() != prev + W0 * SAMPLES_PER_FRAME)
                    return {}; // a short successful tensor is not zero-filled speech
                if (w == nwin - 1 && n0 < W0)
                    all_pcm.resize(prev + (int)((float)n0 / W0 * SPW));
            }
        }

        if (cfg.verbose) {
            std::cerr << "[orpheus-speak] SNAC decoded " << all_pcm.size()
                      << " samples (" << (float)all_pcm.size() / cfg.sample_rate
                      << "s)" << (is_dynamic ? " in 1 pass" : "") << "\n";
        }
        return all_pcm;
    }

    void cleanup() {
        if (!g_ort) return;
        if (allocator) {
            auto free_name = [&](const char *&name) {
                if (!name) return;
                if (OrtStatus *status = g_ort->AllocatorFree(allocator, (void*)name))
                    g_ort->ReleaseStatus(status);
                name = nullptr;
            };
            for (auto &name : in_names) free_name(name);
            free_name(out_names[0]);
        }
        if (run_opts) { g_ort->ReleaseRunOptions(run_opts); run_opts = nullptr; }
        if (mem_info) { g_ort->ReleaseMemoryInfo(mem_info); mem_info = nullptr; }
        if (session)  { g_ort->ReleaseSession(session); session = nullptr; }
        if (opts)     { g_ort->ReleaseSessionOptions(opts); opts = nullptr; }
        if (env)      { g_ort->ReleaseEnv(env); env = nullptr; }
        allocator = nullptr;  // borrowed default allocator
        ok = has_cuda_ep = is_dynamic = false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SSE streaming TTS subsystem (--stream-tts)
// ─────────────────────────────────────────────────────────────────────────────
// Replaces "generate whole chunk → decode whole chunk → WAV → aplay" with:
//   1. llama-server /completion with "stream":true (SSE) — tokens arrive as
//      they are generated (~160 tok/s = ~2x the 82 tok/s real-time rate)
//   2. a 4-frame sliding-window SNAC decode — to emit frame e, decode frames
//      [e-1 .. e+2] and keep the middle 2048-sample slice; head emits frames
//      0-1 from the first window, tail emits the remainder from the last
//      window.  Constant tiny ONNX shape → a few ms per decode, and no frame
//      is ever dropped (unlike the reference impl, which loses head + tail).
//   3. a per-session raw-PCM player (aplay reading stdin) — stdin close gives
//      exact drain semantics, eliminating WAV files and the drain-guard
//      heuristic.  Stall gaps are fed with silence to avoid underrun clicks.
//
// First audio per chunk ≈ time to 4 frames (28 tokens ≈ 0.2-0.4 s of
// generation) instead of the full chunk + a ~300-400 ms padded decode.

// Incrementally scan `tail` for complete <custom_token_N> matches, appending
// code values (N - ORPHEUS_AUDIO_OFFSET) to `out`.  Consumed text is erased;
// a trailing partial match (e.g. "<custom_tok") is preserved for next call.
static void scan_tokens_streaming(std::string &tail, std::vector<int> &out) {
    static const char prefix[] = "<custom_token_";
    static const size_t plen = sizeof(prefix) - 1;

    size_t pos = 0;
    size_t keep_from = std::string::npos;   // start of an incomplete match
    while (pos < tail.size()) {
        size_t lt = tail.find('<', pos);
        if (lt == std::string::npos) { pos = tail.size(); break; }

        // Is this the start of a (possibly partial) prefix?
        size_t avail = tail.size() - lt;
        size_t cmp = std::min(avail, plen);
        if (tail.compare(lt, cmp, prefix, cmp) != 0) { pos = lt + 1; continue; }
        if (avail < plen) { keep_from = lt; break; }    // partial prefix at end

        // Parse digits
        size_t p = lt + plen;
        int tok = 0; bool has_digits = false;
        while (p < tail.size() && tail[p] >= '0' && tail[p] <= '9') {
            tok = tts_token_digit(tok, tail[p]); has_digits = true; p++;
        }
        if (p >= tail.size()) { keep_from = lt; break; }  // digits may continue
        if (!has_digits || tail[p] != '>') { pos = lt + 1; continue; }
        p++;  // consume '>'

        if (tok >= ORPHEUS_AUDIO_OFFSET) {
            int code = tok - ORPHEUS_AUDIO_OFFSET;
            if (code < ORPHEUS_FRAME_TOKENS * ORPHEUS_CODEBOOK_SZ) out.push_back(code);
        }
        pos = p;
    }
    if (keep_from != std::string::npos) tail.erase(0, keep_from);
    else                                tail.clear();
}

// ── Persistent raw-PCM sink ──────────────────────────────────────────────────
// One player process per session.  float PCM → S16_LE over a pipe; blocking
// writes provide backpressure; closing stdin makes the player drain and exit,
// so finish() returns exactly when the last sample has been handed to ALSA.
// r24.17: a two-hundred-millisecond audio slice is not a wall-time bound:
// a nonreading player blocked write(), so the stop poll could never run.
// Retain backpressure with interruptible POLLOUT waits. The existing stop
// request remains visible even after generation's main loop has finished.
// ATHENA_TTS_SINK_ABORT=0 restores the blocking pipe mechanism.
static bool tts_sink_abort_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_SINK_ABORT");
        return !(e && e[0] == '0'); }();
    return on;
}
struct PcmSink {
    pid_t pid = -1;
    int   fd  = -1;
    int   sample_rate = 24000;
    std::atomic<bool> *abort_flag = nullptr;
    std::atomic<bool> *stop_observed = nullptr;
    std::string stop_path, stop_session_id;
    bool write_aborted = false;
    size_t last_write_bytes = 0;

    bool start(const std::string &raw_cmd, int rate) {
        sample_rate = rate;
        int p[2];
        if (pipe(p) != 0) { perror("[orpheus-speak] pipe"); return false; }

        std::vector<std::string> args;
        std::istringstream iss(raw_cmd);
        std::string tok;
        while (iss >> tok) args.push_back(tok);
        args.push_back("-r"); args.push_back(std::to_string(rate));
        args.push_back("-");

        std::vector<char *> argv;
        for (auto &a : args) argv.push_back(&a[0]);
        argv.push_back(nullptr);

        // posix_spawnp (glibc clone(CLONE_VM|CLONE_VFORK)) instead of fork()+execvp:
        // no dup_mmap/pte_alloc over this process's CUDA/ONNX address space (CHANGES.MD
        // §13). Behaviour is identical — stdin<-pipe read end, PATH search, and pid is
        // a normal child waited by finish()/abort_now() below.
        // ── r24.6 (WO-49 item 4): stop discarding the player's stderr ────────
        // This addopen sent aplay's stderr to /dev/null, so EVERY xrun report
        // from S19's 27,720 ms of injected silence was thrown away — the one
        // diagnostic that names an underrun as the device saw it. The child
        // now inherits our stderr (which the launcher timestamps into the
        // log). ORPHEUS_PLAYER_STDERR=0 restores /dev/null for players whose
        // startup chatter proves noisy.
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, p[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&fa, p[0]);
        posix_spawn_file_actions_addclose(&fa, p[1]);
        static const bool player_stderr_on = [] {
            const char *e = ::getenv("ORPHEUS_PLAYER_STDERR");
            return !(e && e[0] == '0');
        }();
        if (!player_stderr_on)
            posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(p[0]);
        if (rc != 0) { close(p[1]); pid = -1; errno = rc; perror("[orpheus-speak] posix_spawnp"); return false; }
        fd = p[1];
        if (tts_sink_abort_on()) {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                abort_now();
                return false;
            }
        }
        return true;
    }

    bool write_all(const int16_t *buf, size_t n) {
        const char *ptr = (const char *)buf;
        size_t left = n * sizeof(int16_t);
        last_write_bytes = 0;
        write_aborted = false;
        while (left > 0) {
            if (tts_sink_abort_on() && abort_flag) {
                if (!abort_flag->load() && !stop_path.empty() &&
                    atts::stop_requested(stop_path, stop_session_id, stop_observed))
                    abort_flag->store(true);
                if (abort_flag->load()) { write_aborted = true; return false; }
            }
            ssize_t w = ::write(fd, ptr, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (tts_sink_abort_on() && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    struct pollfd pfd{fd, POLLOUT, 0};
                    const int ready = ::poll(&pfd, 1, 20);
                    if (ready < 0 && errno != EINTR) return false;
                    if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
                    continue;
                }
                return false;   // EPIPE → player died
            }
            if (tts_sink_abort_on() && w == 0) return false;
            ptr += w; left -= (size_t)w;
            last_write_bytes += (size_t)w;
        }
        return true;
    }

    bool write_pcm(const float *pcm, size_t n) {
        if (fd < 0) return false;
        std::vector<int16_t> s16(n);
        for (size_t i = 0; i < n; i++) {
            float v = pcm[i] * 32767.0f;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            s16[i] = (int16_t)v;
        }
        return write_all(s16.data(), n);
    }

    bool write_silence_ms(int ms) {
        if (fd < 0) return false;
        size_t n = (size_t)((int64_t)sample_rate * ms / 1000);
        std::vector<int16_t> z(n, 0);
        return write_all(z.data(), n);
    }

    // Close stdin → player drains its buffer and exits.  Returns when audio
    // has fully played (the exact .done timing talk-llama waits on).
    bool finish() {
        if (fd >= 0) { close(fd); fd = -1; }
        if (pid <= 0) return false;
        int st = 0; pid_t got;
        if (!tts_delivery_proof_on()) { waitpid(pid, &st, 0); pid = -1; return true; }
        do { got = waitpid(pid, &st, 0); } while (got < 0 && errno == EINTR);
        pid = -1;
        return got > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }

    // r24.16: once all PCM fits in the pipe, waitpid used to hide the whole
    // audible tail from the stop poll. Close input, then reap without blocking
    // so the existing playback loop still observes a cut during that tail.
    int poll_drain() {
        if (fd >= 0) { close(fd); fd = -1; }
        if (pid <= 0) return -1;
        int status = 0;
        const pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == 0 || (got < 0 && errno == EINTR)) return 0;
        pid = -1;
        return got > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : -1;
    }

    // Hard stop for barge-in: kill the player without draining.  Closing the
    // PCM device (process death) halts output within one ALSA period — this
    // is what makes an interruption feel immediate.
    void abort_now() {
        if (fd >= 0) { close(fd); fd = -1; }
        if (pid > 0) { kill(pid, SIGKILL); int st; waitpid(pid, &st, 0); pid = -1; }
    }
};

// ── 4-frame sliding-window decoder ───────────────────────────────────────────
// Maintains the code stream for one chunk; advances emission one frame at a
// time with 1 frame of left context and 2 of lookahead (the canonical Orpheus
// streaming scheme), decoding a constant [1,4]/[1,8]/[1,16] shape.
struct SlidingWindowDecoder {
    static constexpr int W = 4;             // window frames
    std::vector<int> codes;                 // raw code stream (7 per frame)
    int emitted = 0;                        // frames already emitted
    bool failed = false;                    // any incomplete/non-finite decode window

    std::vector<float> decode_window_(SnacDecoder &snac, const Config &cfg,
                                     int first, int count) {
        std::vector<float> pcm = snac.decode_exact(cfg, window(first, count));
        if (tts_delivery_proof_on()) {
            if (pcm.size() < (size_t)count * SnacDecoder::SAMPLES_PER_FRAME) failed = true;
            for (float &v : pcm) if (!std::isfinite(v)) { failed = true; v = 0.0f; }
        }
        return pcm;
    }

    int frames() const { return (int)codes.size() / ORPHEUS_FRAME_TOKENS; }

    void feed(const std::vector<int> &more) {
        codes.insert(codes.end(), more.begin(), more.end());
    }

    // Build SnacCodes for frames [f0, f0+nf)
    SnacCodes window(int f0, int nf) const {
        std::vector<int> sub(codes.begin() + (size_t)f0 * ORPHEUS_FRAME_TOKENS,
                             codes.begin() + (size_t)(f0 + nf) * ORPHEUS_FRAME_TOKENS);
        return deinterleave_tokens(sub, false);
    }

    // Decode whatever is decodable; append emitted samples to `out`.
    // final=true flushes the tail (and handles chunks shorter than W).
    void drain(SnacDecoder &snac, const Config &cfg, std::vector<float> &out, bool final) {
        constexpr int SPF = SnacDecoder::SAMPLES_PER_FRAME;
        int F = frames();

        // Head: first window emits frames 0-1
        if (emitted == 0) {
            if (F >= W) {
                std::vector<float> pcm = decode_window_(snac, cfg, 0, W);
                if ((int)pcm.size() >= 2 * SPF) out.insert(out.end(), pcm.begin(), pcm.begin() + 2 * SPF);
                emitted = 2;
            } else if (final && F > 0) {
                std::vector<float> pcm = decode_window_(snac, cfg, 0, F);
                size_t want = (size_t)F * SPF;
                if (pcm.size() > want) pcm.resize(want);
                out.insert(out.end(), pcm.begin(), pcm.end());
                emitted = F;
                return;
            } else {
                return;
            }
        }

        // Steady state: emit frame e from window [e-1, e+2]
        while (emitted >= 2 && emitted + 3 <= F) {
            std::vector<float> pcm = decode_window_(snac, cfg, emitted - 1, W);
            if ((int)pcm.size() >= 2 * SPF)
                out.insert(out.end(), pcm.begin() + SPF, pcm.begin() + 2 * SPF);
            emitted++;
        }

        // Tail: flush remaining frames from the last full window
        if (final && emitted < F) {
            int f0 = std::max(0, F - W);
            std::vector<float> pcm = decode_window_(snac, cfg, f0, F - f0);
            int off = (emitted - f0) * SPF;
            if (off >= 0 && off < (int)pcm.size())
                out.insert(out.end(), pcm.begin() + off, pcm.end());
            emitted = F;
        }
    }
};

// ── Offline re-decode (garble discriminator) ─────────────────────────────────
// Re-decode a captured .codes file with the SAME SnacDecoder + sliding-window
// logic, deterministically. Compare this WAV against the session's live .wav:
//   clean offline + garbled live  -> the contended on-GPU decode corrupted it
//   both garbled                  -> the model emitted bad audio codes
// Run with --snac-cpu for a contention-free reference decode.
static int decode_codes_file(const Config &cfg, SnacDecoder &snac) {
    std::ifstream f(cfg.decode_codes);
    if (!f) { std::cerr << "[orpheus-speak] cannot open codes file: " << cfg.decode_codes << "\n"; return 1; }
    std::vector<float> all;
    std::string line; std::vector<int> cur; int chunk = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        SlidingWindowDecoder d; d.feed(cur);
        std::vector<float> out; d.drain(snac, cfg, out, /*final=*/true);
        all.insert(all.end(), out.begin(), out.end());
        std::cerr << "[orpheus-speak] re-decode chunk " << chunk << ": "
                  << cur.size() << " codes -> " << out.size() << " samples\n";
        cur.clear(); ++chunk;
    };
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') { flush(); continue; }   // chunk-delimiter line
        std::istringstream is(line); int v; while (is >> v) cur.push_back(v);
    }
    flush();
    const std::string out_path = cfg.decode_codes + ".redecode.wav";
    if (!write_wav(out_path, all, cfg.sample_rate)) { std::cerr << "[orpheus-speak] WAV write failed\n"; return 1; }
    std::cerr << "[orpheus-speak] re-decode wrote " << out_path << " ("
              << all.size() << " samples, " << (float)all.size() / cfg.sample_rate << " s)\n";
    return 0;
}

// ── Per-chunk streaming state ────────────────────────────────────────────────
struct SessionSync {
    std::mutex m;
    std::condition_variable cv;
};

// One trigger-file line as it sits inside a chunk's text. Kept so an abort
// position (samples played) can be mapped back to talk-llama's line indexing
// without fuzzy text matching.
struct ChunkLine {
    size_t line_idx;   // global line number within the session (talk-llama's write order)
    size_t off;        // char offset of this line inside the chunk text
    size_t len;        // line length in chars
};

struct StreamChunk {
    std::string text;
    size_t index = 0;

    // SSE parse state (touched only by the chunk's curl thread)
    std::string sse_buf;       // unparsed SSE bytes
    std::string tok_tail;      // partial <custom_token_ text
    SlidingWindowDecoder dec;
    bool generation_end = false;    // explicit native stop or OpenAI DONE
    bool generation_limited = false; // an explicit limit cannot prove whole speech

    // Shared with playback thread (guarded by SessionSync::m)
    std::deque<float> pcm;
    bool done   = false;
    bool failed = false;
    // A cancelled worker is done too; only a naturally completed waveform
    // supplies a denominator for mapping played samples to the whole text.
    bool audio_complete = false;
    int  tokens = 0;
    float server_ms = 0;
    size_t pcm_total = 0;          // total samples this chunk has produced
    std::vector<ChunkLine> lines;  // constituent trigger-file lines

    std::atomic<bool> *abort_flag = nullptr;   // session abort (barge-in)

    SessionSync *sync = nullptr;
    SnacDecoder *snac = nullptr;
    const Config *cfg = nullptr;
    std::chrono::high_resolution_clock::time_point t_post;
    bool first_pcm_logged = false;
    // Written only by this chunk's decoding worker; read after all jobs join.
    double first_pcm_mono = 0;
    std::string source_session;

    // capture-mode buffers (populated only when cfg->capture_dir is set)
    std::string cap_tokens;            // verbatim SSE content (model output) — for parser checks
    std::vector<float> cap_pcm;        // preserved copy of decoded PCM (playback drains c->pcm)
};

// Read a protocol scalar without matching the same bytes inside content.
// Native llama-server emits stop/stopped_limit booleans; OpenAI completion
// responses also carry finish_reason. Whitespace is not part of their identity.
static bool sse_scalar_is(const std::string &json, const char *key, const char *value) {
    for (size_t at = 0; at < json.size(); ++at) {
        if (json[at] != '"') continue;
        const size_t begin = ++at;
        bool escaped = false;
        while (at < json.size() && json[at] != '"') {
            if (json[at] == '\\') { escaped = true; if (++at == json.size()) break; }
            ++at;
        }
        if (at == json.size()) return false;
        if (escaped || json.compare(begin, at - begin, key) != 0) continue;
        size_t colon = at + 1;
        while (colon < json.size() && std::isspace((unsigned char)json[colon])) ++colon;
        if (colon == json.size() || json[colon] != ':') continue;
        size_t first = colon + 1;
        while (first < json.size() && std::isspace((unsigned char)json[first])) ++first;
        const size_t n = std::strlen(value);
        if (json.compare(first, n, value) != 0) continue;
        const size_t end = first + n;
        if (end == json.size() || json[end] == ',' || json[end] == '}' ||
            json[end] == ']' || std::isspace((unsigned char)json[end])) return true;
    }
    return false;
}

// SSE write callback: parse events, scan tokens, advance the window decoder,
// publish PCM.  Runs on the chunk's curl thread; decodes serialize through
// SnacDecoder::run_mutex.
static size_t sse_write_cb(char *data, size_t size, size_t nmemb, void *userp) {
    auto *c = static_cast<StreamChunk *>(userp);
    if (c->abort_flag && c->abort_flag->load()) return 0;   // short-write → curl aborts the transfer
    size_t total = size * nmemb;
    c->sse_buf.append(data, total);

    std::vector<int> new_codes;
    size_t nl;
    while ((nl = c->sse_buf.find('\n')) != std::string::npos) {
        std::string line = c->sse_buf.substr(0, nl);
        c->sse_buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data: ", 0) != 0) continue;     // ignore non-data lines
        std::string payload = line.substr(6);
        if (payload.empty()) continue;
        if (payload == "[DONE]") { c->generation_end = true; continue; }
        // A clean HTTP EOF can still be a truncated generation. Keep every
        // received frame playable, but do not turn that transport success into
        // proof that the model finished the text. A later DONE cannot undo an
        // explicit token-limit outcome.
        if (sse_scalar_is(payload, "stopped_limit", "true") ||
            sse_scalar_is(payload, "stop_type", "\"limit\"") ||
            sse_scalar_is(payload, "finish_reason", "\"length\""))
            c->generation_limited = true;
        // Defensive: never scan the final event.  Token events carry
        // "stop":false; some llama-server builds repeat the accumulated
        // content in the "stop":true event, which would duplicate audio.
        if (sse_scalar_is(payload, "stop", "true")) {
            c->generation_end = true;
            continue;
        }
        if (sse_scalar_is(payload, "finish_reason", "\"stop\"")) c->generation_end = true;
        std::string content = extract_text_from_json(payload);
        if (content.empty()) continue;
        c->tok_tail += content;
        if (!c->cfg->capture_dir.empty()) c->cap_tokens += content;
        scan_tokens_streaming(c->tok_tail, new_codes);
    }

    if (!new_codes.empty()) {
        c->tokens += (int)new_codes.size();
        c->dec.feed(new_codes);
        std::vector<float> out;
        c->dec.drain(*c->snac, *c->cfg, out, /*final=*/false);
        if (!out.empty()) {
            if (c->first_pcm_mono == 0)
                c->first_pcm_mono = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if ((c->cfg->verbose || c->cfg->diag) && !c->first_pcm_logged) {
                auto dt = std::chrono::duration<float, std::milli>(
                    std::chrono::high_resolution_clock::now() - c->t_post).count();
                std::cerr << "[orpheus-speak]   chunk " << c->index + 1
                          << ": first PCM in " << (int)dt << " ms session=" << c->source_session
                          << " steady=" << std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()
                          << " audible=unverified\n";
                c->first_pcm_logged = true;
            }
            std::lock_guard<std::mutex> lk(c->sync->m);
            c->pcm.insert(c->pcm.end(), out.begin(), out.end());
            if (!c->cfg->capture_dir.empty()) c->cap_pcm.insert(c->cap_pcm.end(), out.begin(), out.end());
            c->pcm_total += out.size();
            c->sync->cv.notify_all();
        }
    }
    return total;
}

// Streaming counterpart of http_generate: SSE POST with retry-on-503.
// Retries only if no frames were received (a 503 body carries no tokens).

// Barge-in: libcurl polls this between transfer events; returning nonzero
// cancels the SSE request, which also frees the llama-server slot for the
// pivot turn that is about to need the GPU.
static int sse_abort_cb(void *userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *c = static_cast<StreamChunk *>(userp);
    return (c->abort_flag && c->abort_flag->load()) ? 1 : 0;
}

static void http_stream_generate(StreamChunk *c) {
    const Config &cfg = *c->cfg;
    std::string prompt = build_orpheus_prompt(cfg.voice, c->text);
    std::string body   = build_request_json(cfg, prompt, /*stream=*/true);

    // One chunk = one std::async thread, so a thread_local handle would leak
    // one CURL handle per chunk over a long daemon run.  Local + cleanup.
    CURL *curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::mutex> lk(c->sync->m);
        c->done = true; c->failed = true;
        c->sync->cv.notify_all();
        return;
    }
    struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Connection: keep-alive");
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    constexpr int max_retries = 3;
    bool ok = false;

    for (int attempt = 0; attempt < max_retries && !ok; attempt++) {
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, cfg.api_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, c);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, sse_abort_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, c);

        c->t_post = std::chrono::high_resolution_clock::now();
        CURLcode res = curl_easy_perform(curl);
        auto t1 = std::chrono::high_resolution_clock::now();
        c->server_ms = std::chrono::duration<float, std::milli>(t1 - c->t_post).count();

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200) {
            ok = true;
        } else if (c->abort_flag && c->abort_flag->load()) {
            break;  // barge-in abort: no retries, no tail flush — just mark done
        } else if (c->dec.frames() == 0 && attempt + 1 < max_retries) {
            int backoff = 500 * (attempt + 1);
            std::cerr << "[orpheus-speak] chunk " << c->index + 1
                      << " HTTP " << http_code << ", retry in " << backoff << "ms\n";
            // ── r20p3.14.7 (OS2): reset the DECODER too, not just the SSE bufs ──
            // The gate is frames()==0, but frames()==codes.size()/7, so it also
            // passes with 1..6 RESIDUAL codes from a transfer that died inside the
            // first frame. llama-server delivers ~one token per SSE event, so an
            // early death leaves those partial codes in dec.codes — and the retry
            // appended the fresh stream onto them, shifting the entire 7-token
            // frame alignment for the whole chunk (deinterleaving cross-layer,
            // feeding negative codebook indices to ONNX: garbled ~15 s of audio).
            // Clearing dec is safe precisely because the gate proved no whole
            // frame exists yet.
            c->dec = SlidingWindowDecoder();
            c->tokens = 0;
            c->generation_end = false;
            c->generation_limited = false;
            c->sse_buf.clear(); c->tok_tail.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
        } else {
            break;  // mid-stream failure: keep what we have, give up
        }
    }

    // Final flush of the window tail, then mark done
    const bool aborted = c->abort_flag && c->abort_flag->load();
    std::vector<float> out;
    if (!aborted) c->dec.drain(*c->snac, cfg, out, /*final=*/true);
    if (!out.empty() && c->first_pcm_mono == 0)
        c->first_pcm_mono = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(c->sync->m);
        if (!out.empty()) {
            c->pcm.insert(c->pcm.end(), out.begin(), out.end());
            if (!cfg.capture_dir.empty()) c->cap_pcm.insert(c->cap_pcm.end(), out.begin(), out.end());
            c->pcm_total += out.size();
        }
        c->done = true;
        c->failed = tts_delivery_proof_on()
            ? (!aborted && (!ok || !c->generation_end || c->generation_limited || c->pcm_total == 0 || c->dec.failed || c->dec.codes.size() % ORPHEUS_FRAME_TOKENS != 0 || !c->tok_tail.empty() ||
                              c->sse_buf.find_first_not_of(" \t\r\n") != std::string::npos))
            : (!ok && c->dec.frames() == 0 && !aborted);
        c->audio_complete = !aborted && ok && !c->failed;
        c->sync->cv.notify_all();
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// ── Per-session capture (garble diagnosis) ───────────────────────────────────
// After a finished SSE session, dump: raw model output (.tokens), parsed audio
// codes — chunk-delimited and re-decodable (.codes), the live decoded audio that
// actually played (.wav), and a summary (.meta). Off unless --capture-dir is set;
// runs after playback.join(), off the realtime path.
static void write_capture(const Config &cfg,
                          const std::deque<std::unique_ptr<StreamChunk>> &chunks,
                          const std::string &report) {
    static std::atomic<int> sess{0};
    char base[600];
    std::snprintf(base, sizeof(base), "%s/tts_%ld_%03d",
                  cfg.capture_dir.c_str(), (long)time(nullptr), sess.fetch_add(1));
    const std::string b = base;

    { std::ofstream f(b + ".codes");
      for (auto &c : chunks) {
          f << "# chunk " << c->index << " (" << c->dec.codes.size() << " codes) \""
            << c->text << "\"\n";
          for (size_t i = 0; i < c->dec.codes.size(); ++i)
              f << c->dec.codes[i] << (((i + 1) % ORPHEUS_FRAME_TOKENS == 0) ? '\n' : ' ');
          f << "\n";
      } }

    { std::ofstream f(b + ".tokens");
      for (auto &c : chunks)
          f << "=== chunk " << c->index << " \"" << c->text << "\" ===\n"
            << c->cap_tokens << "\n\n"; }

    { std::vector<float> all;
      for (auto &c : chunks) all.insert(all.end(), c->cap_pcm.begin(), c->cap_pcm.end());
      write_wav(b + ".wav", all, cfg.sample_rate); }

    { std::ofstream f(b + ".meta");
      f << "epoch\t" << (long)time(nullptr) << "\nresult\t" << report
        << "\nchunks\t" << chunks.size() << "\n"
        << "idx\tchars\tcodes\tframes\tsamples\tserver_ms\n";
      for (auto &c : chunks)
          f << c->index << "\t" << c->text.size() << "\t" << c->dec.codes.size() << "\t"
            << (c->dec.codes.size() / ORPHEUS_FRAME_TOKENS) << "\t"
            << c->cap_pcm.size() << "\t" << c->server_ms << "\n"; }

    std::cerr << "[orpheus-speak] capture: " << b << ".{tokens,codes,wav,meta} ("
              << chunks.size() << " chunks)\n";
}

// ── SSE streaming session (replaces FILL/PLAY/REFILL when --stream-tts) ─────
// Returns the completion report written to the .done file:
//   "COMPLETE"            — the whole session played out
//   "INTERRUPTED <L> <C>" — barge-in: L lines fully spoken, C chars
//                           (word-snapped) spoken into line L
static std::string run_sse_session(const Config &cfg, SnacDecoder &snac, atts::Input *owned_input = nullptr) {
    atts::Input local_input(cfg.watch_file);
    atts::Input &input = owned_input ? *owned_input : local_input;
    if (cfg.verbose) std::cerr << "[orpheus-speak] session started (SSE streaming)\n";

    static constexpr int max_inflight = 5;
    static constexpr int CHUNK_CHAR_LIMIT = 300;

    // ── barge-in abort channel ──
    // talk-llama requests an abort by creating <base>.stop next to the
    // trigger file. Checked in the 20 ms session loop; the playback thread,
    // SSE transfers, and the sink all key off one atomic.
    std::string stop_path = cfg.watch_file;
    {
        auto dot = stop_path.rfind('.');
        if (dot != std::string::npos) stop_path = stop_path.substr(0, dot);
        stop_path += ".stop";
    }
    // Identified stop requests may already belong to this queued session.
    // Their owner is checked at every poll; only legacy inputs clear on entry.
    if (input.id.empty() && atts::stop_requested(stop_path, {}))
        std::remove(stop_path.c_str());
    std::atomic<bool> session_abort{false}, stop_observed{false};

    SessionSync sync;
    std::deque<std::unique_ptr<StreamChunk>> chunks;   // stable addresses
    std::vector<std::future<void>> tasks;
    size_t next_submit = 0;
    std::string pending_chunk;
    size_t file_pos = input.start;
    bool end_received = false;

    size_t line_counter = 0;             // global line index (talk-llama's write order)
    std::vector<ChunkLine> pending_lines;

    auto t0 = std::chrono::high_resolution_clock::now();
    

    auto flush_pending = [&]() {
        if (pending_chunk.empty()) return;
        auto c = std::make_unique<StreamChunk>();
        c->text = pending_chunk;
        c->index = chunks.size();
        c->source_session=input.id;
        c->sync = &sync; c->snac = &snac; c->cfg = &cfg;
        c->abort_flag = &session_abort;
        c->lines = std::move(pending_lines);
        pending_lines.clear();
        {
            std::lock_guard<std::mutex> lk(sync.m);
            chunks.push_back(std::move(c));
        }
        pending_chunk.clear();
    };

    auto read_new_lines = [&]() {
        if (end_received) return;
        std::ifstream legacy;
        if (input.id.empty()) legacy.open(cfg.watch_file);
        std::ifstream &ifs = input.id.empty() ? legacy : input.file;
        if (!input.id.empty()) ifs.clear();
        if (!ifs) return;
        ifs.seekg(file_pos);
        std::string line;
        while (std::getline(ifs, line)) {
            // ── r20p3.14.7 (OS1): never consume a TORN line ───────────────────
            // talk-llama appends with buffered fprintf, and a multi-line beat
            // batch (or any line) flushed larger than the stdio buffer reaches
            // this file in more than one write() — so a 20 ms poll can land with
            // only "Second sen" of "Second sentence.\n" on disk. std::getline
            // returns that fragment AND sets eofbit; the old code then (a) spoke
            // the fragment as a whole line, desyncing every downstream index, and
            // far worse (b) took file_pos = tellg(), which at EOF is
            // pos_type(-1) → (size_t)-1, permanently poisoning file_pos so the
            // rest of the turn — INCLUDING ---END--- — was never read again and
            // .done never written: a hung session. A line with no terminating
            // newline yet is simply not ready; leave file_pos at the last
            // complete line and re-read it whole on the next poll.
            if (ifs.eof()) break;
            file_pos = ifs.tellg();
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line == "---END---") {
                flush_pending();
                end_received = true;
                return;
            }
            // ── r20p3.14.3 (RA11): her beat ─────────────────────────────────
            //
            // The breath already exists — pause_ms of silence is written
            // BETWEEN chunks, sliced with an abort check, gated on more_coming
            // so nothing trails the last one. What she had no way to do was
            // CAUSE a boundary: chunks only ever broke at CHUNK_CHAR_LIMIT.
            // S10 measured her asking for one 23 times in 28 turns, with a
            // <pause> tag she had invented and which did nothing. talk-llama
            // now turns that tag into this sentinel and the existing machinery
            // does the rest, so the beat is her CURRENT pause_ms — the same
            // breath the drift is already giving her, not a second number.
            //
            // The sentinel still CONSUMES AN INDEX. talk-llama counted it into
            // session_lines, and "INTERRUPTED <L> <C>" indexes the lines
            // talk-llama wrote — C55 records what happens when the two sides
            // disagree about which lines count, and the failure mode there is a
            // barge-in rebuilding her committed speech from the wrong place,
            // i.e. a memory of words she did not say. Zero length, so the
            // character mapping can never land "inside" it.
            if (line == "---PAUSE---") {
                flush_pending();
                pending_lines.push_back({ line_counter++, 0, 0 });
                continue;
            }
            if (!pending_chunk.empty() &&
                (int)(pending_chunk.size() + 1 + line.size()) > CHUNK_CHAR_LIMIT) {
                flush_pending();
            }
            // record where this trigger-file line lands inside the chunk text
            // (offset accounts for the joining space added below)
            pending_lines.push_back({line_counter++,
                                     pending_chunk.empty() ? 0 : pending_chunk.size() + 1,
                                     line.size()});
            if (!pending_chunk.empty()) pending_chunk += " ";
            pending_chunk += line;
        }
    };

    auto inflight = [&]() -> size_t {
        std::lock_guard<std::mutex> lk(sync.m);
        size_t n = 0;
        for (size_t i = 0; i < next_submit; i++)
            if (!chunks[i]->done) n++;
        return n;
    };

    auto all_submitted_done = [&]() -> bool {
        std::lock_guard<std::mutex> lk(sync.m);
        for (size_t i = 0; i < next_submit; i++)
            if (!chunks[i]->done) return false;
        return true;
    };

    // ── Playback thread: consume chunk PCM in order into the raw sink ──
    std::atomic<bool> session_over{false};
    size_t total_samples = 0;
    float  first_audio_ms = 0;
    double first_sink_write_mono = 0; // playback thread owns; read only after join
    float  startup_buffer_ms = 0;     // audio buffered when playback began
    float  startup_low_water_ms = -1; // lowest device-fill estimate seen
    int64_t silence_ms_total = 0;      // injected stall-silence (device near-dry only)
    int64_t silence_events = 0;

    // barge-in: where the audio actually stopped, filled by the playback
    // thread at abort time (sample domain; mapped to text after join)
    bool playback_failed = false;     // playback owns; read only after join
    int    abort_chunk_idx    = -1;   // chunk containing the last audible sample
    size_t abort_chunk_played = 0;    // samples of that chunk's PCM that played

    std::thread playback([&]() {
        PcmSink sink;
        sink.abort_flag = &session_abort;
        sink.stop_observed = &stop_observed;
        sink.stop_path = stop_path;
        sink.stop_session_id = input.id;
        bool sink_ok = sink.start(cfg.play_raw_cmd, cfg.sample_rate);
        if (!sink_ok) playback_failed = true;
        bool wrote_any = false;
        bool started = false;         // startup watermark gate (once per session)
        const size_t prebuf_target =
            (size_t)((int64_t)cfg.sample_rate * std::max(0, cfg.prebuffer_ms) / 1000);
        std::vector<float> buf;
        size_t play_idx = 0;

        // ── Device-fill write clock ──────────────────────────────────────
        // The device consumes at exactly real-time once started, so
        //   buffered ≈ (samples written) − (wall time since first write).
        // Silence is injected ONLY when this estimate nears dry (<60 ms) —
        // NOT when the in-process queue is momentarily empty between 85 ms
        // frame arrivals (rev-2 bug: that fired whenever supply < 1.71× RT,
        // chopping the first utterance while ~400 ms of real audio sat
        // unplayed in aplay's buffer).
        std::chrono::steady_clock::time_point t_play0;
        size_t written_samples = 0;
        float  min_buffered = 1e9f;
        auto buffered_ms_now = [&]() -> float {
            if(!wrote_any)return 0.0f;
            float el = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - t_play0).count();
            return (float)written_samples * 1000.0f / cfg.sample_rate - el;
        };
        constexpr float LOW_WATER_MS = 60.0f;

        // barge-in: ordered record of what was written to the device —
        // (chunk index, samples) for PCM, (-1, samples) for injected silence.
        // At abort, real time pins the played position; this maps it back to
        // a chunk-relative sample count.
        // r21-B: the timeline now carries BOTH sample counts per segment.
        //   .second  output samples — what was handed to ALSA, so elapsed wall
        //            time converts to a position in it
        //   .third   input samples  — what the decoder produced, which is the
        //            space chunk.pcm_total and the character mapping live in
        // Keeping them apart is the whole of the accounting fix: with a
        // time-stretch in the path they are no longer the same number, and
        // using the output figure against pcm_total would put every barge-in's
        // rebuild off by exactly the stretch factor.
        struct Seg { int chunk; size_t out; size_t in; };
        std::vector<Seg> timeline;
        bool aborted_local = false;
        bool player_reaped = false;

        // How she is to sound, this turn. Read once per session: a setting that
        // changed mid-turn would put a discontinuity at a chunk boundary and
        // multiply the accounting problem by the chunk count.
        aprs::Setting prosody;
        if (!cfg.ctl_path.empty()) {
            std::ifstream cf(cfg.ctl_path);
            std::string line;
            if (cf && std::getline(cf, line) && aprs::parse_control(line, prosody)) {
                if (cfg.verbose && !prosody.identity())
                    std::cerr << "[orpheus-speak] prosody: rate=" << prosody.rate
                              << " semi=" << prosody.semitones
                              << " pause=" << prosody.pause_ms
                              << " gain=" << prosody.gain_db << "dB\n";
            } else {
                prosody = aprs::Setting();   // fail off, silently: stock behaviour
            }
        }
        aprs::Stretcher stretch;
        stretch.configure(prosody, cfg.sample_rate);
        int stretch_chunk = -1;             // which chunk the stretcher is mid-way through
        std::vector<float> shaped;          // reused output scratch

        // ── diag (--diag): real-time underrun/stall tracing ──────────────────
        // A 1 Hz heartbeat of the device-fill estimate + cumulative stalls makes
        // a developing starvation visible as it happens (the session-end summary
        // is too late to correlate with the server log / GPU telemetry). A
        // stall-RUN marker (rate-limited) flags each acute dry spell. All emitted
        // to stderr, which the launcher's ts wrapper timestamps into athena.log.
        auto   diag_hb_t       = std::chrono::high_resolution_clock::now();
        auto   diag_stall_log_t= diag_hb_t - std::chrono::seconds(1);
        bool   diag_stalling   = false;
        float  diag_run_minfill= 1e9f;
        int64_t diag_run_inj    = 0, diag_run_ms=0;
        uint64_t wait_text_polls=0, wait_pcm_polls=0, wait_watermark_polls=0;

        while (true) {
            buf.clear();
            if (cfg.diag && wrote_any) {
                auto now = std::chrono::high_resolution_clock::now();
                if (std::chrono::duration<float, std::milli>(now - diag_hb_t).count() >= 1000.0f) {
                    std::cerr << "[orpheus-speak] diag: chunk=" << play_idx
                              << " fill=" << (long)buffered_ms_now() << "ms"
                              << " played=" << (long)((float)written_samples * 1000.0f / cfg.sample_rate) << "ms"
                              << " stalls=" << silence_events
                              << " silence=" << silence_ms_total << "ms\n";
                    diag_hb_t = now;
                }
            }
            bool chunk_done = false, have_chunk = false;
            {
                std::unique_lock<std::mutex> lk(sync.m);
                sync.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                    if (session_abort.load()) return true;
                    if (session_over.load() && play_idx >= chunks.size()) return true;
                    // r20p3.14 (RA6): the predicate must reflect the START
                    // WATERMARK below, not merely "some PCM exists". Before
                    // playback has begun, the pull refuses until prebuf_target
                    // is buffered, so waking on the first 4096-sample frame left
                    // this thread spinning at 100% of a core for ~200 ms at every
                    // turn start — one stat() per iteration, and contention on
                    // the very mutex the decoder needs to publish more PCM, on
                    // the GPU-contended path the watermark exists to protect.
                    if (play_idx >= chunks.size()) return false;
                    const auto &cc = *chunks[play_idx];
                    if (started) return !cc.pcm.empty() || cc.done;
                    return cc.done || cc.pcm.size() >= prebuf_target;
                });
                if (play_idx >= chunks.size()&&!session_over.load())++wait_text_polls;
                if (play_idx < chunks.size()) {
                    have_chunk = true;
                    auto &c = *chunks[play_idx];
                    if(!c.done&&c.pcm.empty())++wait_pcm_polls;
                    else if(!started&&!c.done&&c.pcm.size()<prebuf_target)++wait_watermark_polls;
                    chunk_done = c.done;
                    // r20p3.14 (RA6): pre-existing. The wait predicate returned
                    // true as soon as any PCM existed, but the watermark below
                    // refuses to pull until prebuf_target is buffered — so for
                    // ~200 ms at every turn start this loop spun at 100% of a
                    // core, issuing a stat() per iteration and taking the very
                    // mutex the decoder needs to publish PCM, on the
                    // GPU-contended path the watermark exists to protect. The
                    // 50 ms timed wait now actually sleeps. (See the predicate.)
                    // Startup watermark: hold the session's first write until
                    // enough audio is buffered to ride out the turn-start
                    // generation deficit (Qwen still contending for the GPU).
                    // Chunk completion or session end starts playback with
                    // whatever exists (tiny chunks must not wait forever).
                    if (!started &&
                        (c.pcm.size() >= prebuf_target || c.done || session_over.load())) {
                        started = true;
                        startup_buffer_ms = (float)c.pcm.size() * 1000.0f / cfg.sample_rate;
                    }
                    if (started && !c.pcm.empty()) {
                        // Pull at most ~200 ms per pass. write_pcm blocks while
                        // the pipe is full, and generation runs >1.3x real-time,
                        // so handing a fully-decoded chunk to one write() pinned
                        // this thread for seconds (field: 4.5 s barge-in abort
                        // latency). Slicing bounds the block, so the abort
                        // check below runs every couple hundred ms of audio.
                        const size_t slice = (size_t) (cfg.sample_rate / 5);
                        const size_t n = std::min(c.pcm.size(), slice);
                        buf.assign(c.pcm.begin(), c.pcm.begin() + n);
                        c.pcm.erase(c.pcm.begin(), c.pcm.begin() + n);
                    }
                }
            }

            // ── barge-in abort: pin the played position, kill the player ──
            // The main session loop stops polling the stop file once every
            // chunk is submitted and decoded — which, at >1.3x real-time
            // generation, is long before playback finishes. The drain tail
            // must watch for it here (field: a barge in a story's tail went
            // unseen and 8.6 s of audio played to natural completion).
            if (!session_abort.load()) {
                if (atts::stop_requested(stop_path, input.id, &stop_observed)) {
                    session_abort.store(true);
                    if (cfg.verbose) std::cerr << "[orpheus-speak] abort requested (barge-in)\n";
                }
            }
            if (session_abort.load()) {
                if (wrote_any) {
                    const float el = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - t_play0).count();
                    size_t played = (size_t)std::max(0.0f, el * (float)cfg.sample_rate / 1000.0f);
                    played = std::min(played, written_samples);
                    size_t remaining = played;
                    std::vector<size_t> per;   // per-chunk INPUT samples played
                    for (const auto &seg : timeline) {
                        if (remaining == 0) break;
                        // `played` is derived from elapsed wall time, so it is
                        // in OUTPUT samples and is consumed against seg.out.
                        const size_t take = std::min(seg.out, remaining);
                        remaining -= take;
                        if (seg.chunk >= 0 && take > 0) {
                            // What accumulates is the INPUT samples that
                            // produced it, pro rata within a partly-played
                            // segment — because that is the space pcm_total and
                            // the character mapping below are in.
                            const size_t in_take = seg.out > 0
                                ? (size_t)((double)seg.in * (double)take / (double)seg.out)
                                : 0;
                            if ((size_t)seg.chunk >= per.size()) per.resize(seg.chunk + 1, 0);
                            per[seg.chunk] += in_take;
                            abort_chunk_idx    = seg.chunk;
                            abort_chunk_played = per[seg.chunk];
                        }
                    }
                }
                aborted_local = true;
                break;
            }

            if (!buf.empty()) {
                if (!wrote_any) {
                    first_audio_ms = std::chrono::duration<float, std::milli>(
                        std::chrono::high_resolution_clock::now() - t0).count();
                    t_play0 = std::chrono::steady_clock::now();
                    wrote_any = true;
                }
                // A fresh chunk gets a fresh stretcher: overlap state carried
                // across a boundary would smear the end of one sentence into
                // the start of the next, which is the opposite of the breath
                // this feature exists to add.
                if ((int)play_idx != stretch_chunk) {
                    // A filler may precede the model's chosen first phrase.
                    // Apply the atomically published native control at a fresh
                    // stretcher boundary, never halfway through a PCM chunk.
                    if(r26_speech_on()&&!cfg.ctl_path.empty()){
                        std::ifstream control(cfg.ctl_path);std::string line;aprs::Setting next;
                        if(control&&std::getline(control,line)&&aprs::parse_control(line,next))prosody=next;
                    }
                    if((cfg.diag||cfg.verbose)&&r26_speech_on())
                        std::cerr<<"[prosody-consumed] session="<<input.id<<" chunk="<<play_idx<<" rate="<<prosody.rate
                            <<" semi="<<prosody.semitones<<" pause="<<prosody.pause_ms<<" gain="<<prosody.gain_db<<" acoustic_effect=unmeasured\n";
                    stretch.configure(prosody, cfg.sample_rate);
                    stretch_chunk = (int)play_idx;
                }
                shaped.clear();
                const size_t consumed = stretch.process(buf.data(), buf.size(), shaped);
                size_t sent = (!sink_ok && tts_sink_abort_on()) ? 0 : shaped.size();
                if (sink_ok && !shaped.empty() &&
                    !sink.write_pcm(shaped.data(), shaped.size())) {
                    sink_ok = false;
                    if (tts_sink_abort_on()) sent = sink.last_write_bytes / sizeof(int16_t);
                }
                if (sink_ok && sent && first_sink_write_mono == 0)
                    first_sink_write_mono = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                written_samples += sent;
                total_samples   += sent;
                const size_t used = sent == shaped.size() || shaped.empty() ? consumed
                    : (size_t)((double)consumed * (double)sent / (double)shaped.size());
                timeline.push_back({ (int)play_idx, sent, used });
                continue;   // drain more immediately
            }

            if (started && have_chunk && chunk_done) {
                // Flush what the stretcher is still holding, or the last ~30 ms
                // of every sentence would simply vanish.
                if ((int)play_idx == stretch_chunk) {
                    shaped.clear();
                    stretch.flush(shaped);
                    if (!shaped.empty()) {
                        size_t sent = (!sink_ok && tts_sink_abort_on()) ? 0 : shaped.size();
                        if (sink_ok && !sink.write_pcm(shaped.data(), shaped.size())) {
                            sink_ok = false;
                            if (tts_sink_abort_on()) sent = sink.last_write_bytes / sizeof(int16_t);
                        }
                        written_samples += sent;
                        total_samples   += sent;
                        timeline.push_back({ (int)play_idx, sent, 0 });
                    }
                }
                // The breath. Before r21-B there was none: chunks were written
                // back to back and the only silence ever inserted was the 40 ms
                // underrun patch below, so her sentences ran together at machine
                // speed whether she was certain, deliberating or spent.
                // Recorded with zero INPUT samples — it belongs to no character.
                // r20p3.14 (RA2): only BETWEEN chunks. Without the successor
                // test the last chunk of every session got a trailing pause,
                // which sink.finish() then blocks on — up to 320 ms added to the
                // end-to-end latency of every single turn, on the turn-taking
                // path. There is nothing to breathe between after the last one.
                bool more_coming = !session_over.load();
                if (!more_coming) {
                    std::lock_guard<std::mutex> lk(sync.m);
                    more_coming = (play_idx + 1) < chunks.size();
                }
                if (prosody.pause_ms > 0 && sink_ok && wrote_any && more_coming) {
                    // r20p3.14 (RA3): written in slices with an abort check
                    // between them. The ~200 ms slicing of normal playback exists
                    // precisely so a barge-in is noticed promptly (the field
                    // regression it fixed was 4.5 s); one unsliced 320 ms
                    // blocking write at a chunk boundary — the likeliest moment
                    // for someone to cut in — handed a third of that back.
                    int left = prosody.pause_ms;
                    size_t np = 0;
                    while (left > 0 && sink_ok && !session_abort.load()) {
                        const int piece = left > 100 ? 100 : left;
                        if (!sink.write_silence_ms(piece)) { sink_ok = false; break; }
                        np   += (size_t)((int64_t)cfg.sample_rate * piece / 1000);
                        left -= piece;
                        if (atts::stop_requested(stop_path, input.id, &stop_observed)) session_abort.store(true);
                    }
                    if (np > 0) {
                        written_samples += np;
                        timeline.push_back({ -1, np, 0 });
                    }
                }
                play_idx++;
                continue;
            }

            bool over = session_over.load() && play_idx >= [&]{
                std::lock_guard<std::mutex> lk(sync.m); return chunks.size(); }();
            if (over) {
                if (!tts_delivery_proof_on()) break;
                const int drained = sink.poll_drain();
                if (drained != 0) {
                    player_reaped = true;
                    if (drained < 0) playback_failed = true;
                    break;
                }
                // The CV is already satisfied at session_over, so do not spin
                // on it while the actual speaker drains the remaining pipe.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            // Anti-underrun guard: feed silence only when the DEVICE is
            // nearly dry — a genuine gap (waiting on the next chunk's text),
            // not the inter-frame arrival jitter of normal streaming.
            //
            // ── r24.6 (WO-49 item 3): inject to a TARGET FILL, not a fixed
            // 40 ms per pass. The wait above is 50 ms per starved pass and the
            // old injection was exactly 40 ms — 40 ms of output per 50 ms of
            // wall is 0.8× while the device consumes at 1.0×, so the guard
            // LOST 200 ms of device fill per second of famine (measured:
            // median 640, max 799 ms injected per wall-second, fill change
            // −201 ms/s — the three catastrophic S19 turns rode this all the
            // way to −1.4 s). Topping the estimate back up to LOW_WATER + one
            // wait-quantum makes the injection rate match consumption during
            // a famine, so the fill holds instead of bleeding. Bounded per
            // pass and written in 100 ms slices with the same abort check the
            // breath path uses (RA3 — a barge-in must not wait behind an
            // unsliced blocking write). ORPHEUS_FILL_TARGET=0 restores the
            // fixed 40 ms exactly.
            if (wrote_any && sink_ok) {
                static const bool fill_target_on = [] {
                    const char *e = ::getenv("ORPHEUS_FILL_TARGET");
                    return !(e && e[0] == '0');
                }();
                float b = buffered_ms_now();
                if (b < min_buffered) min_buffered = b;
                if (b < LOW_WATER_MS) {
                    int inject_ms = 40;
                    if (fill_target_on) {
                        inject_ms = (int)((LOW_WATER_MS + 50.0f) - b);   // top up past the next wait
                        if (inject_ms < 40)  inject_ms = 40;
                        if (inject_ms > 400) inject_ms = 400;            // bound one pass
                    }
                    size_t ninj = 0;
                    int left = inject_ms;
                    while (left > 0 && sink_ok && !session_abort.load()) {
                        const int piece = left > 100 ? 100 : left;
                        if (!sink.write_silence_ms(piece)) { sink_ok = false; break; }
                        ninj += (size_t)((int64_t)cfg.sample_rate * piece / 1000);
                        left -= piece;
                        if (atts::stop_requested(stop_path, input.id, &stop_observed)) session_abort.store(true);
                    }
                    const int injected = inject_ms - left;
                    written_samples += ninj;
                    timeline.push_back({ -1, ninj, 0 });
                    silence_ms_total += injected;
                    silence_events++;
                    if (cfg.diag) {
                        diag_run_inj++;diag_run_ms+=injected;
                        if (b < diag_run_minfill) diag_run_minfill = b;
                        // Mark the START of a contiguous dry spell (rate-limited so a
                        // choppy run can't flood). Recovery is logged on exit below.
                        auto now = std::chrono::high_resolution_clock::now();
                        if (!diag_stalling &&
                            std::chrono::duration<float, std::milli>(now - diag_stall_log_t).count() >= 250.0f) {
                            std::cerr << "[orpheus-speak] diag: STALL begin chunk=" << play_idx
                                      << " fill=" << (long)b << "ms\n";
                            diag_stall_log_t = now;
                        }
                        diag_stalling = true;
                    }
                } else if (cfg.diag && diag_stalling) {
                    std::cerr << "[orpheus-speak] diag: STALL end chunk=" << play_idx
                              << " injected=" << diag_run_inj << " (" << diag_run_ms << "ms)"
                              << " minfill=" << (long)diag_run_minfill << "ms\n";
                    diag_stalling    = false;
                    diag_run_inj     = 0;diag_run_ms=0;
                    diag_run_minfill = 1e9f;
                }
            }
        }
        if(cfg.diag)std::cerr<<"[orpheus-speak] wait-observations session="<<input.id
            <<" waiting_text_polls="<<wait_text_polls<<" waiting_inference_or_decode_polls="<<wait_pcm_polls
            <<" startup_watermark_polls="<<wait_watermark_polls<<" injected_ms="<<silence_ms_total
            <<" device_underrun=unmeasured competing_session=unmeasured units=poll-counts-not-duration\n";
        startup_low_water_ms = (min_buffered > 1e8f) ? -1.0f : min_buffered;
        if (!sink_ok && !(tts_sink_abort_on() && sink.write_aborted)) playback_failed = true;
        if (aborted_local) {
            sink.abort_now();   // device closes → silence within one ALSA period
        } else if (!player_reaped) {
            if (!sink.finish()) playback_failed = true; // player exit is part of the proof
        }
    });

    // ── Main loop: read text, submit streaming requests ──
    while (true) {
        // An abandoned mini-session must never read a later reply at the
        // reused pathname. Its already opened input retains the old inode.
        if (!input.current(cfg.watch_file)) {
            session_abort.store(true);
            sync.cv.notify_all();
        }
        // barge-in: talk-llama dropped a stop file → abort the session
        if (!session_abort.load()) {
            if (atts::stop_requested(stop_path, input.id, &stop_observed)) {
                session_abort.store(true);
                sync.cv.notify_all();
                if (cfg.verbose) std::cerr << "[orpheus-speak] abort requested (barge-in)\n";
            }
        }
        if (session_abort.load()) break;   // stop reading and submitting

        read_new_lines();
        // Pipeline starved → push the partial sentence now (chunk-1 latency)
        if (!pending_chunk.empty() && next_submit == chunks.size() && all_submitted_done())
            flush_pending();

        size_t total;
        { std::lock_guard<std::mutex> lk(sync.m); total = chunks.size(); }
        while (next_submit < total && inflight() < (size_t)max_inflight) {
            StreamChunk *cp;
            { std::lock_guard<std::mutex> lk(sync.m); cp = chunks[next_submit].get(); }
            if (cfg.verbose) {
                std::cerr << "[orpheus-speak]   chunk " << next_submit + 1
                          << ": \"" << cp->text.substr(0, 60)
                          << (cp->text.size() > 60 ? "..." : "") << "\"\n";
            }
            tasks.push_back(std::async(std::launch::async, http_stream_generate, cp));
            next_submit++;
        }

        if (end_received && next_submit == total && all_submitted_done()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto &t : tasks) t.wait();
    session_over = true;
    sync.cv.notify_all();
    playback.join();

    // Correlate existing stage observations outside decoding/playback and all
    // callbacks. A successful pipe write is NOT an acoustic-onset measurement.
    const char *r26_master=std::getenv("ATHENA_R26"), *r26_timing=std::getenv("ATHENA_R26_TIMING");
    const bool timing_enabled = !(r26_master && r26_master[0]=='0') &&
        ((r26_timing && r26_timing[0]) ? r26_timing[0]!='0' :
         (r26_master && r26_master[0] && r26_master[0]!='0'));
    if ((cfg.diag || cfg.verbose) && timing_enabled) {
        double first_pcm=0;
        for (const auto &chunk:chunks) if (chunk->first_pcm_mono>0 &&
            (first_pcm==0 || chunk->first_pcm_mono<first_pcm)) first_pcm=chunk->first_pcm_mono;
        std::ostringstream timing; timing.precision(17);
        timing << "[orpheus-speak] r26-timing session=" << input.id
               << " first_pcm_steady=" << first_pcm << " first_sink_write_steady=" << first_sink_write_mono
               << " acoustic_onset=unmeasured clock=steady\n";
        std::cerr << timing.str();
    }

    // Optional trace I/O happens after the player has settled, never between a
    // matching stop and its abort. This is an observed-stop record; its clocks
    // date trace publication, not the instant sound left a physical speaker.
    if (stop_observed.load()) atts::trace_event("daemon", "stop_observe", input.id, "STOP");

    // ── completion report ──
    // Map the abort position (chunk, samples played) back to talk-llama's
    // line indexing. Chars heard ≈ proportional position in the chunk's
    // audio; the playing chunk is almost always fully decoded (generation
    // runs ≥1.3× ahead of playback), so the mapping is usually exact. If it
    // is still streaming, estimate its total from the session's measured
    // samples-per-char.
    std::string report = "COMPLETE";
    if (session_abort.load()) {
        size_t rep_line = 0, rep_char = 0;
        std::lock_guard<std::mutex> lk(sync.m);
        if (abort_chunk_idx >= 0 && (size_t)abort_chunk_idx < chunks.size()) {
            const auto &c = *chunks[abort_chunk_idx];
            size_t total = c.pcm_total;
            if (!c.audio_complete || total == 0) {
                double spc = 2600.0;   // fallback ≈9 chars/s of speech at 24 kHz
                size_t s_sum = 0, ch_sum = 0;
                for (const auto &q : chunks)
                    if (q->audio_complete && q->pcm_total > 0) { s_sum += q->pcm_total; ch_sum += q->text.size(); }
                if (ch_sum > 0) spc = (double)s_sum / (double)ch_sum;
                total = std::max(total, (size_t)((double)c.text.size() * spc));
            }
            size_t pos = total ? (size_t)((double)c.text.size() * (double)abort_chunk_played / (double)total) : 0;
            pos = std::min(pos, c.text.size());

            bool placed = false;
            for (const auto &ln : c.lines) {
                if (pos < ln.off) {                  // in the joining gap → this line not started
                    rep_line = ln.line_idx; rep_char = 0; placed = true; break;
                }
                if (pos < ln.off + ln.len) {         // inside this line
                    rep_line = ln.line_idx;
                    size_t cc = pos - ln.off;
                    if (cc > 0 && cc < ln.len) {     // snap back to a word boundary
                        const std::string lt = c.text.substr(ln.off, ln.len);
                        const size_t sp = lt.rfind(' ', cc);
                        cc = (sp == std::string::npos) ? 0 : sp;
                    }
                    rep_char = cc; placed = true; break;
                }
            }
            if (!placed) {                           // past the chunk's last line
                rep_line = c.lines.empty() ? 0 : c.lines.back().line_idx + 1;
                rep_char = 0;
            }
        }
        report = "INTERRUPTED " + std::to_string(rep_line) + " " + std::to_string(rep_char);
        if (input.id.empty() && atts::stop_requested(stop_path, {}))
        std::remove(stop_path.c_str());
        if (cfg.verbose)
            std::cerr << "[orpheus-speak] aborted at line " << rep_line
                      << ", char " << rep_char << "\n";
    }

    if (tts_delivery_proof_on()) {
        size_t first_failed = chunks.size();
        for (size_t i = 0; i < chunks.size(); ++i)
            if (chunks[i]->failed || (!session_abort.load() && chunks[i]->pcm_total == 0)) {
                first_failed = i; break;
            }
        if (playback_failed || first_failed != chunks.size()) {
            size_t proved_lines = 0;
            // A successful full drain proves earlier COMPLETE chunks. A failed
            // player or an abort proves no additional whole-chunk prefix here;
            // the existing INTERRUPTED estimate is never promoted to proof.
            // ── r24.20 review (SPEECH F4): the chunks AFTER the gap ─────────
            // The playback thread skips a chunk with no PCM and plays every
            // later one, so a mid-session failure (an HTTP 500 on chunk 2 of 5,
            // a runaway that hit n_predict) leaves chunks 3–5 played through a
            // player that exited 0 — and `FAILED L 0` can only name the
            // proven PREFIX; the receipt grammar has no way to say "lines 5–9
            // played, lines 3–4 did not". Extending it was measured and
            // rejected: every brain from r24.16 to r24.20 parses an extra
            // field or a new word as INVALID, i.e. FAILED 0 0 unsettled, which
            // is strictly less credit than today. So the receipt stays, the
            // brain keeps unknown rows as separately sourced attempted wording,
            // without crediting them as received speech, and THIS line
            // says exactly which lines played past the gap, so the log and
            // s23-evidence can settle a dispute the receipt cannot.
            std::string played_past_gap;
            if (!playback_failed && !session_abort.load()) {
                for (size_t i = 0; i < first_failed; ++i)
                    if (!chunks[i]->lines.empty())
                        proved_lines = chunks[i]->lines.back().line_idx + 1;
                for (size_t i = first_failed + 1; i < chunks.size(); ++i) {
                    const auto &c = *chunks[i];
                    if (c.failed || c.pcm_total == 0) continue;
                    // speech lines only: a ---PAUSE--- sentinel holds an index
                    // with zero length and is not a word of hers
                    size_t lo = SIZE_MAX, hi = 0;
                    for (const auto &ln : c.lines)
                        if (ln.len > 0) { lo = std::min(lo, ln.line_idx); hi = std::max(hi, ln.line_idx); }
                    if (lo == SIZE_MAX) continue;
                    played_past_gap += played_past_gap.empty() ? " lines " : ", ";
                    played_past_gap += std::to_string(lo);
                    if (hi != lo) played_past_gap += "-" + std::to_string(hi);
                }
            }
            report = "FAILED " + std::to_string(proved_lines) + " 0";
            std::cerr << "[orpheus-speak] delivery unproven: "
                      << (playback_failed ? "player failed" : "incomplete or empty audio chunk")
                      << "; " << report;
            if (!played_past_gap.empty())
                std::cerr << " (played past the gap to a clean player exit, uncredited by the receipt:"
                          << played_past_gap << ")";
            std::cerr << "\n";
        }
    }

    if (cfg.verbose && total_samples > 0) {
        auto total_ms = std::chrono::duration<float, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count();
        float server_ms = 0; int tok = 0; size_t nch;
        { std::lock_guard<std::mutex> lk(sync.m);
          nch = chunks.size();
          for (auto &c : chunks) { server_ms += c->server_ms; tok += c->tokens; } }
        std::cerr << "[orpheus-speak] streaming timing (SSE):\n"
                  << "  llama-server:  " << server_ms << " ms (" << tok << " tokens)\n"
                  << "  first audio:   " << first_audio_ms << " ms after session start\n"
                  << "  startup buf:   " << startup_buffer_ms << " ms (watermark "
                  << cfg.prebuffer_ms << " ms)\n"
                  << "  stall silence: " << silence_ms_total << " ms in "
                  << silence_events << " stalls\n"
                  << "  low water:     " << startup_low_water_ms << " ms min device fill\n"
                  << "  total wall:    " << total_ms << " ms\n"
                  << "  audio length:  " << (float)total_samples / cfg.sample_rate * 1000.0f << " ms\n"
                  << "  chunks:        " << nch << "\n"
                  << "  result:        " << report << "\n"
                  << "  pipeline:      SSE + 4-frame sliding-window decode (max "
                  << max_inflight << " in-flight)\n";
    }

    if (!cfg.capture_dir.empty()) write_capture(cfg, chunks, report);
    return report;
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI
// ─────────────────────────────────────────────────────────────────────────────
static void usage(const char *argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options] \"text to speak\"\n"
        << "       " << argv0 << " [options] -f input.txt\n"
        << "       " << argv0 << " [options] --watch /tmp/speak.txt  (daemon mode)\n\n"
        << "Options:\n"
        << "  -f FILE        Read input text from FILE\n"
        << "  -o FILE        Output WAV path        [/dev/shm/orpheus_tts.wav]\n"
        << "  --voice NAME   Orpheus voice           [tara]\n"
        << "  --api URL      llama-server URL        [http://127.0.0.1:8080/completion]\n"
        << "  --snac PATH    SNAC ONNX decoder path  [snac24_decoder.onnx]\n"
        << "  --play CMD     Play command after gen   (e.g. 'aplay')\n"
        << "  --watch FILE   Daemon mode: watch FILE for changes, speak each update\n"
        << "                 SNAC session stays alive — much faster after first utterance\n"
        << "  --temp F       Temperature              [0.6]\n"
        << "  --top-p F      Top-p                    [0.9]\n"
        << "  --rep-pen F    Repetition penalty        [1.1]\n"
        << "  --max-tokens N Max tokens to generate   [2500]\n"
        << "  --snac-cpu     Force SNAC decoder to CPU (frees ~1.7 GB VRAM)\n"
        << "  --stream-tts   Watch mode: SSE-stream tokens from llama-server and decode\n"
        << "                 incrementally (4-frame sliding window) into a persistent\n"
        << "                 raw-PCM player — first audio ~0.5s after chunk submission\n"
        << "  --play-raw CMD Raw-PCM player for --stream-tts ['aplay -q -t raw -f S16_LE -c 1 --buffer-time=300000']\n"
        << "                 ('-r <rate> -' is appended automatically)\n"
        << "  --capture-dir DIR Garble diagnosis: per-session dump of .tokens/.codes/.wav/.meta\n"
        << "  --decode-codes F  Offline: re-decode a captured .codes file to F.redecode.wav, exit\n"
        << "  --compare-wav A B Print correlation/RMS verdict between two WAVs (garble scoring), exit\n"
        << "  --prebuffer-ms N  Startup watermark for --stream-tts [350]: buffer N ms of\n"
        << "                 audio before a session's first write (turn-start chop guard)\n"
        // r24.6 (WO-48): --control existed since r21-B and was in --help ZERO
        // times — which is why nobody caught the launcher never passing it and
        // the whole voice/breath feature riding it stayed inert for 59
        // sessions (rate pinned 1.000, the between-chunks breath never fired).
        << "  --control FILE Prosody sidecar (r21-B): one 'v1 rate=.. semi=.. pause=..\n"
        << "                 gain=..' line, read once per session; written by talk-llama\n"
        << "                 beside the trigger file (usually <watch-file>.ctl)\n"
        << "  -v, --verbose  Verbose logging\n"
        << "  --diag         Real-time pipeline diagnostics: 1 Hz device-fill heartbeat\n"
        << "                 + stall-run markers (trace TTS underruns / server stalls)\n"
        << "  -h, --help     This message\n\n"
        << "Voices: tara, leah, jess, leo, dan, mia, zac, zoe\n"
        << "Emotion tags: <laugh> <chuckle> <sigh> <cough> <sniffle> <groan> <yawn> <gasp>\n";
}

static Config parse_args(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-f") && i + 1 < argc)             cfg.input_file = argv[++i];
        else if ((a == "-o") && i + 1 < argc)        cfg.output_wav = argv[++i];
        else if ((a == "--voice") && i + 1 < argc)    cfg.voice      = argv[++i];
        else if ((a == "--api") && i + 1 < argc)      cfg.api_url    = argv[++i];
        else if ((a == "--snac") && i + 1 < argc)     cfg.snac_model = argv[++i];
        else if ((a == "--play") && i + 1 < argc)     cfg.play_cmd   = argv[++i];
        else if ((a == "--watch") && i + 1 < argc)    cfg.watch_file = argv[++i];
        else if ((a == "--control") && i + 1 < argc)  cfg.ctl_path   = argv[++i];
        else if ((a == "--temp") && i + 1 < argc)     cfg.temperature= std::stof(argv[++i]);
        else if ((a == "--top-p") && i + 1 < argc)    cfg.top_p      = std::stof(argv[++i]);
        else if ((a == "--rep-pen") && i + 1 < argc)  cfg.rep_penalty= std::stof(argv[++i]);
        else if ((a == "--max-tokens") && i + 1 < argc) cfg.max_tokens= std::stoi(argv[++i]);
        else if (a == "-v" || a == "--verbose")       cfg.verbose     = true;
        else if (a == "--diag")                       cfg.diag        = true;
        else if (a == "--snac-cpu")                   cfg.snac_cpu    = true;
        else if (a == "--stream-tts")                 cfg.stream_tts  = true;
        else if (a == "--no-stream-tts")              cfg.stream_tts  = false;
        else if ((a == "--play-raw") && i + 1 < argc) cfg.play_raw_cmd = argv[++i];
        else if ((a == "--prebuffer-ms") && i + 1 < argc) cfg.prebuffer_ms = std::stoi(argv[++i]);
        else if ((a == "--capture-dir") && i + 1 < argc)  cfg.capture_dir  = argv[++i];
        else if ((a == "--decode-codes") && i + 1 < argc) cfg.decode_codes = argv[++i];
        else if ((a == "--compare-wav") && i + 2 < argc) { cfg.compare_a = argv[++i]; cfg.compare_b = argv[++i]; }
        else if (a == "-h" || a == "--help")          { usage(argv[0]); exit(0); }
        else if (a[0] != '-')                         cfg.text        = a;
        else { std::cerr << "Unknown option: " << a << "\n"; usage(argv[0]); exit(1); }
    }
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio playback via fork/exec (replaces system() to avoid shell overhead)
// ─────────────────────────────────────────────────────────────────────────────
// Splits play_cmd into argv, appends wav_path, forks and execs directly.
// Saves ~5ms per chunk vs system() by avoiding shell process spawning.
// Redirects child stderr to /dev/null (equivalent to 2>/dev/null).
static int play_audio(const std::string &play_cmd, const std::string &wav_path) {
    // Tokenize play_cmd (e.g. "aplay -q -D pulse" → ["aplay", "-q", "-D", "pulse"])
    std::vector<std::string> args;
    std::istringstream iss(play_cmd);
    std::string tok;
    while (iss >> tok) args.push_back(tok);
    args.push_back(wav_path);

    // Build null-terminated argv array for execvp
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    // posix_spawnp instead of fork()+execvp (CHANGES.MD §13 — no dup_mmap over the
    // CUDA/ONNX address space). Legacy batch path; behaviour identical.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { errno = rc; return -1; }   // spawn failed
    int status = 0; pid_t got;
    if (tts_delivery_proof_on()) {
        do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
        if (got < 0) return -1;
    } else { waitpid(pid, &status, 0); }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline: text → (chunked) API → tokens → SNAC → WAV → play
// ─────────────────────────────────────────────────────────────────────────────
// For short text (1-2 sentences): single Orpheus request, same as before.
// For long text (3+ sentences): parallel chunked processing:
//   - All-ahead submission: all chunks submitted as staggered concurrent HTTP requests
//   - Retry with backoff on HTTP 503 (slot busy)
//   - llama-server continuous batching fills up to -np slots in parallel
//   - Background playback: play each chunk via double-buffered WAV as it arrives
// This avoids Orpheus attention-skip on long text and stays within context.
static bool speak_text(const Config &cfg, SnacDecoder &snac, const std::string &text) {
    if (text.empty()) return false;

    auto sentences = split_sentences(text);
    bool use_chunked = (int)text.size() > 300;

    if (cfg.verbose) {
        std::cerr << "[orpheus-speak] voice=" << cfg.voice
                  << " text=\"" << text.substr(0, 80) << (text.size() > 80 ? "..." : "")
                  << "\"" << (use_chunked ? " [chunked: " + std::to_string(sentences.size()) + " sentences]" : "")
                  << "\n";
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<float> all_pcm;
    std::atomic<bool> delivery_failed{false};

    if (use_chunked) {
        // ── Chunked mode: character-based grouping ─────────────────────
        // Orpheus-3B reliably vocalizes up to ~430 chars per request.
        // Beyond that it probabilistically hits <eot_id> before finishing.
        // Group sentences until adding the next would exceed the limit.
        static constexpr int CHUNK_CHAR_LIMIT = 300;  // safe for 2304 tokens/slot

        std::vector<std::string> chunks;
        std::string current;
        for (const auto &s : sentences) {
            if (!current.empty()
                && (int)(current.size() + 1 + s.size()) > CHUNK_CHAR_LIMIT) {
                chunks.push_back(current);
                current.clear();
            }
            if (!current.empty()) current += " ";
            current += s;
        }
        if (!current.empty()) chunks.push_back(current);

        // ── Parallel generation + pipelined playback ─────────────────
        //
        // Architecture:
        //   - All-ahead submission: submit ALL chunks as concurrent HTTP
        //     requests with a small stagger (200ms) between each.  With
        //     -np N, llama-server's continuous batching processes up to N
        //     in parallel on the GPU.
        //   - Retry with backoff: if a slot is momentarily busy (HTTP 503),
        //     http_generate retries up to 3 times with 500ms/1000ms backoff.
        //   - Background playback: play each chunk individually via
        //     double-buffered WAV as soon as its SNAC decode completes.

        std::string wav_buf[2] = { cfg.output_wav, cfg.output_wav + ".buf" };
        int buf_idx = 0;

        float total_server_ms = 0, total_decode_ms = 0;
        int   total_tokens    = 0;
        size_t total_pcm_samples = 0;

        // Sliding window submission — keep at most max_inflight HTTP
        // requests in flight at once.  With -np 3, this means 3 slots
        // actively generating + 2 queued and ready, so the GPU never
        // idles between chunks.  Prevents overwhelming llama-server
        // with dozens of concurrent connections on long responses
        // (which exhausts the retry budget for late chunks).
        //
        // For short responses (≤ max_inflight chunks), this behaves
        // identically to all-ahead submission.
        static constexpr int max_inflight = 5;  // np=3 active + 2 queued

        std::vector<std::future<HttpResult>> http_futures(chunks.size());
        size_t next_submit = 0;

        // Submit initial window with stagger
        for (; next_submit < std::min(chunks.size(), (size_t)max_inflight); next_submit++) {
            if (next_submit > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            http_futures[next_submit] = std::async(std::launch::async,
                http_generate, cfg.api_url, cfg.voice, chunks[next_submit],
                std::cref(cfg), (next_submit == 0) && cfg.verbose);
        }

        std::thread play_thread;
        auto play_end_time = std::chrono::high_resolution_clock::now();

        // Collect results in order; submit next chunk as each completes
        for (size_t i = 0; i < chunks.size(); i++) {
            if (cfg.verbose) {
                std::cerr << "[orpheus-speak]   chunk " << i + 1 << "/" << chunks.size()
                          << ": \"" << chunks[i].substr(0, 60)
                          << (chunks[i].size() > 60 ? "..." : "") << "\"\n";
            }

            // 1. Wait for this chunk's HTTP response (may already be done)
            HttpResult hr = http_futures[i].get();

            // Submit next chunk now that one result has been collected.
            // The slot that just finished (or is about to finish) will be
            // available for this new request.  No stagger needed — the
            // server is no longer at the initial submission burst.
            if (next_submit < chunks.size()) {
                http_futures[next_submit] = std::async(std::launch::async,
                    http_generate, cfg.api_url, cfg.voice, chunks[next_submit],
                    std::cref(cfg), false);
                next_submit++;
            }

            if (!hr.ok) {
                std::cerr << "[orpheus-speak]   chunk " << i + 1
                          << " HTTP failed after retries, skipping\n";
                delivery_failed = true;
                continue;
            }

            total_server_ms += hr.server_ms;

            // 2. Parse tokens + SNAC decode (main thread — SNAC not thread-safe)
            std::string gen_text = extract_text_from_json(hr.response);
            if (gen_text.empty()) gen_text = hr.response;

            std::vector<int> audio_tokens = extract_audio_tokens(gen_text, false);
            if (audio_tokens.empty()) { delivery_failed = true; continue; }

            int usable = ((int)audio_tokens.size() / ORPHEUS_FRAME_TOKENS)
                       * ORPHEUS_FRAME_TOKENS;
            if (audio_tokens.size() != (size_t)usable) delivery_failed = true;
            audio_tokens.resize(usable);
            total_tokens += usable;

            SnacCodes codes = deinterleave_tokens(audio_tokens, false);

            auto t_s0 = std::chrono::high_resolution_clock::now();
            std::vector<float> pcm = snac.decode(cfg, codes);
            auto t_s1 = std::chrono::high_resolution_clock::now();
            total_decode_ms += std::chrono::duration<float, std::milli>(t_s1 - t_s0).count();

            if (!tts_pcm_proven(pcm, codes.codes0.size() * SnacDecoder::SAMPLES_PER_FRAME)) {
                delivery_failed = true; continue;
            }
            total_pcm_samples += pcm.size();

            if (cfg.play_cmd.empty()) {
                if (!append_wav(cfg.output_wav, pcm, cfg.sample_rate,
                                total_pcm_samples - pcm.size())) {
                    std::cerr << "[orpheus-speak] failed to append: " << cfg.output_wav << "\n";
                    delivery_failed = true;
                }
                continue;
            }

            float audio_ms = (float)pcm.size() / cfg.sample_rate * 1000.0f;

            // 3. Wait for PREVIOUS chunk's playback to finish
            if (play_thread.joinable()) play_thread.join();

            // Drain guard: aplay -D pulse may return before PulseAudio
            // finishes playing the audio.  Sleep until expected end time.
            {
                auto now = std::chrono::high_resolution_clock::now();
                if (now < play_end_time)
                    std::this_thread::sleep_until(play_end_time);
            }

            // 4. Write WAV to current buffer and start background playback
            if (!write_wav(wav_buf[buf_idx], pcm, cfg.sample_rate)) {
                std::cerr << "[orpheus-speak] failed to write: " << wav_buf[buf_idx] << "\n";
                delivery_failed = true;
                continue;
            }

            if (!cfg.play_cmd.empty()) {
                play_end_time = std::chrono::high_resolution_clock::now()
                              + std::chrono::milliseconds((int)audio_ms + 100);

                std::string wav = wav_buf[buf_idx];
                std::string pcmd = cfg.play_cmd;
                play_thread = std::thread([pcmd, wav, &delivery_failed]() {
                    if (play_audio(pcmd, wav) != 0) delivery_failed = true;
                });
            }

            buf_idx = 1 - buf_idx;  // toggle double buffer
        }

        // Wait for final playback to finish
        if (play_thread.joinable()) play_thread.join();
        {
            auto now = std::chrono::high_resolution_clock::now();
            if (now < play_end_time)
                std::this_thread::sleep_until(play_end_time);
        }

        // Clean up secondary WAV buffer
        if (!cfg.play_cmd.empty()) std::remove(wav_buf[1].c_str());

        auto t_end = std::chrono::high_resolution_clock::now();

        if (cfg.verbose) {
            auto total_ms = std::chrono::duration<float, std::milli>(t_end - t0).count();
            float audio_sec = (float)total_pcm_samples / cfg.sample_rate;
            std::cerr << "[orpheus-speak] pipelined timing:\n"
                      << "  llama-server:  " << total_server_ms << " ms (" << total_tokens << " tokens)\n"
                      << "  SNAC decode:   " << total_decode_ms << " ms\n"
                      << "  total wall:    " << total_ms << " ms\n"
                      << "  audio length:  " << audio_sec * 1000.0f << " ms\n"
                      << "  chunks:        " << chunks.size()
                      << " (from " << sentences.size() << " sentences)\n"
                      << "  pipeline:      sliding window (max " << max_inflight << " in-flight) + play-while-decode\n";
        }

    } else {
        // ── Single-shot mode: one Orpheus request for the whole text ─────
        std::string prompt = build_orpheus_prompt(cfg.voice, text);
        std::string body   = build_request_json(cfg, prompt);
        std::string response;

        if (!http_post(cfg.api_url, body, response, cfg.verbose) || response.empty()) {
            std::cerr << "[orpheus-speak] API call failed\n";
            return false;
        }

        auto t1 = std::chrono::high_resolution_clock::now();

        std::string gen_text = extract_text_from_json(response);
        if (gen_text.empty()) gen_text = response;

        std::vector<int> audio_tokens = extract_audio_tokens(gen_text, cfg.verbose);
        if (audio_tokens.empty()) {
            std::cerr << "[orpheus-speak] no audio tokens in response\n";
            return false;
        }

        int usable = ((int)audio_tokens.size() / ORPHEUS_FRAME_TOKENS) * ORPHEUS_FRAME_TOKENS;
        if (audio_tokens.size() != (size_t)usable) delivery_failed = true;
        audio_tokens.resize(usable);

        SnacCodes codes = deinterleave_tokens(audio_tokens, cfg.verbose);
        if (codes.codes0.empty()) return false;

        auto t2 = std::chrono::high_resolution_clock::now();

        all_pcm = snac.decode(cfg, codes);
        if (!tts_pcm_proven(all_pcm, codes.codes0.size() * SnacDecoder::SAMPLES_PER_FRAME)) return false;

        auto t3 = std::chrono::high_resolution_clock::now();

        if (!write_wav(cfg.output_wav, all_pcm, cfg.sample_rate)) {
            std::cerr << "[orpheus-speak] failed to write: " << cfg.output_wav << "\n";
            return false;
        }

        auto t4 = std::chrono::high_resolution_clock::now();

        if (!cfg.play_cmd.empty()) {
            if (play_audio(cfg.play_cmd, cfg.output_wav) != 0) delivery_failed = true;
        }

        auto t5 = std::chrono::high_resolution_clock::now();

        if (cfg.verbose) {
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<float, std::milli>(b - a).count();
            };
            float audio_sec = (float)all_pcm.size() / cfg.sample_rate;
            std::cerr << "[orpheus-speak] timing breakdown:\n"
                      << "  llama-server:  " << ms(t0, t1) << " ms\n"
                      << "  token parse:   " << ms(t1, t2) << " ms\n"
                      << "  SNAC decode:   " << ms(t2, t3) << " ms\n"
                      << "  WAV write:     " << ms(t3, t4) << " ms\n"
                      << "  playback:      " << ms(t4, t5) << " ms\n"
                      << "  total:         " << ms(t0, t5) << " ms\n"
                      << "  audio length:  " << audio_sec * 1000.0f << " ms\n"
                      << "  RTF:           " << ms(t0, t4) / (audio_sec * 1000.0f) << "x\n";
        }
    }

    return !tts_delivery_proof_on() || !delivery_failed.load();
}

// Read and trim a text file
static std::string read_text_file(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string text = ss.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    // r24.20: the batch wrapper shares the exact no-follow/nonblocking append
    // path instead of reopening a previously checked shell path with >>. This
    // internal entry point needs neither a model nor curl/SNAC initialization.
    // Its pid/sequence/clocks identify this helper invocation, not the shell.
    if (argc > 1 && std::string(argv[1]) == "--trace-wrapper") {
        bool valid = false;
        if (argc == 5) {
            const std::string event = argv[2], detail = argv[4];
            if (event == "owner_publish" || event == "owner_publish_failed") valid = detail == "B";
            else if (event == "receipt_consume" && detail.size() <= 80) {
                valid = detail == "COMPLETE" || detail == "INVALID";
                if (!valid) {
                    std::istringstream input(detail);
                    std::string kind, line, characters, extra;
                    input >> kind >> line >> characters;
                    valid = (kind == "FAILED" || kind == "INTERRUPTED") &&
                        !line.empty() && line.find_first_not_of("0123456789") == std::string::npos &&
                        !characters.empty() && characters.find_first_not_of("0123456789") == std::string::npos &&
                        !(input >> extra) && detail == kind + " " + line + " " + characters;
                }
            }
        }
        if (!valid) {
            std::fputs("[athena-trace] incomplete file=tts-wrapper.jsonl reason=arguments\n", stderr);
            return 0;
        }
        atts::trace_event("wrapper", argv[2], argv[3], argv[4]);
        return 0;
    }
    Config cfg = parse_args(argc, argv);

    // --compare-wav A B: no model/network needed — score two WAVs and exit.
    if (!cfg.compare_a.empty())
        return compare_wavs(cfg.compare_a, cfg.compare_b);

    // The streaming raw-PCM sink writes to a pipe; if the player dies we want
    // EPIPE from write(), not process death.
    std::signal(SIGPIPE, SIG_IGN);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Initialize SNAC decoder once
    SnacDecoder snac;
    snac.init(cfg);
    if (!snac.ok) {
        std::cerr << "[orpheus-speak] SNAC decoder init failed\n";
        return 1;
    }

    // Offline garble discriminator: re-decode a captured .codes file and exit.
    if (!cfg.decode_codes.empty())
        return decode_codes_file(cfg, snac);

    // Capture mode: ensure the dump directory exists (best-effort).
    if (!cfg.capture_dir.empty()) mkdir(cfg.capture_dir.c_str(), 0755);

    if (!cfg.watch_file.empty()) {
        // Pre-warm the streaming window shape so the first session doesn't
        // pay ONNX kernel/arena setup for [1,4]/[1,8]/[1,16] (the one-time
        // ~700 ms first-chunk outlier observed in logs).
        if (cfg.stream_tts) {
            SnacCodes warm;
            warm.codes0.assign(4, 0);
            warm.codes1.assign(8, 0);
            warm.codes2.assign(16, 0);
            auto tw0 = std::chrono::high_resolution_clock::now();
            snac.decode_exact(cfg, warm);
            if (cfg.verbose) {
                auto ms = std::chrono::duration<float, std::milli>(
                    std::chrono::high_resolution_clock::now() - tw0).count();
                std::cerr << "[orpheus-speak] SNAC window warmup: " << (int)ms << " ms\n";
            }
        }
        // ── Watch mode: unified streaming protocol ──────────────────────
        // All speech (generation, goodbye, heard_ok) arrives as lines in
        // the trigger file, terminated by a ---END--- sentinel.
        //
        // Protocol:
        //   talk-llama appends "sentence\n" lines during token generation
        //   talk-llama appends "---END---\n" when done
        //   orpheus-speak reads lines incrementally, batches all available
        //   sentences into speak_text() for sliding-window pipelined playback
        //   orpheus-speak signals .done after END, deletes trigger file,
        //   and drains stale inotify events before returning to idle.
        //
        // Identified sessions retain their opened input, deduplicate directory
        // events by nonce, and label both receipt and stop. The producer replaces
        // the retained trigger. Legacy clients keep deletion/drain behavior.
        std::cerr << "[orpheus-speak] watching " << cfg.watch_file
                  << " (SNAC session warm, Ctrl+C to quit)\n";

        // Derive .done path: <dir>/speak_tts.txt → <dir>/speak_tts.done
        std::string done_file = cfg.watch_file;
        auto dot = done_file.rfind('.');
        if (dot != std::string::npos) done_file = done_file.substr(0, dot);
        done_file += ".done";

        // ── r24.6 (WO-48): the startup assertion the feature never had ───────
        // talk-llama writes the prosody sidecar as <watch-file>.ctl on every
        // turn. If that file exists here but no --control was passed, the
        // whole r21-B voice/breath channel is being written into a file
        // nobody reads — exactly the state S19 ran in for 2 h 47 m (and 59
        // sessions before it), with zero log evidence. Say so, loudly, once.
        if (cfg.ctl_path.empty()) {
            const std::string ctl_guess = cfg.watch_file + ".ctl";
            struct stat st_ctl;
            if (stat(ctl_guess.c_str(), &st_ctl) == 0)
                std::cerr << "[orpheus-speak] WARNING: " << ctl_guess
                          << " exists but --control was not passed — the r21-B "
                             "prosody channel (rate/pitch/breath) is being written "
                             "and never read. Pass --control '" << ctl_guess << "'\n";
        }

        std::string watch_dir, watch_base;
        int ifd = -1, wd = -1;
        auto start_watch = [&]() -> bool {
        // Extract directory and basename for inotify directory watch
        {
            std::vector<char> dp(cfg.watch_file.begin(), cfg.watch_file.end());
            std::vector<char> bp(cfg.watch_file.begin(), cfg.watch_file.end());
            dp.push_back('\0'); bp.push_back('\0');
            watch_dir  = dirname(dp.data());
            watch_base = basename(bp.data());
        }

        ifd = inotify_init();
        if (ifd < 0) {
            perror("[orpheus-speak] inotify_init");
            return false;
        }

        wd = inotify_add_watch(ifd, watch_dir.c_str(),
                                   IN_MOVED_TO | IN_CLOSE_WRITE);
        if (wd < 0) {
            perror("[orpheus-speak] inotify_add_watch");
            close(ifd);
            return false;
        }

        return true;
        };
        // Recovery and event registration used to leave an unobserved window:
        // a whole new request could arrive after the snapshot but before the
        // watch, then wait forever. Register first for the identified protocol.
        if (atts::session_id_on() && !start_watch()) return 1;

        std::string last_session_id;
        // ── Crash-restart recovery (CHANGES.MD §20) ──────────────────────────
        // If a PREVIOUS daemon instance died mid-session (e.g. the 20260702-134528
        // GPF), the trigger file it never consumed is still on disk — and since a
        // completed trigger receives no further writes, inotify below would NEVER
        // fire for it: a restarted daemon idles forever while the brain waits on a
        // .done that cannot come. Recover at startup:
        //   trigger exists AND contains ---END--- → the turn is fully written and
        //     its audio is unrecoverable unless a matching receipt was already
        //     published. Preserve that receipt or report FAILED under proof;
        //     DELIVERY_PROOF=0 retains the older COMPLETE failure restore.
        //     r24.20 review (SPEECH F3/F9): "unrecoverable" means unPROVABLE.
        //     A crash can happen before or after playback; the restarted
        //     daemon has no evidence to choose between them. FAILED 0 0
        //     records that uncertainty. ATHENA_TTS_SENT_WORDS retains the
        //     attempted wording separately from confirmed speech.
        //   trigger exists WITHOUT ---END--- → the brain is still mid-generation;
        //     leave it — its next sentence append fires IN_CLOSE_WRITE and the
        //     normal session replays the turn from the top.
        {
            atts::Input recovered(cfg.watch_file);
            if (recovered.file) {
                // Content, mode and identity must come from this one opened
                // source. A second open combined an old END with a new nonce
                // and emitted FAILED for the still-live replacement request.
                std::string content((std::istreambuf_iterator<char>(recovered.file)),
                                    std::istreambuf_iterator<char>());
                if (content.find("---END---") != std::string::npos ||
                    (!recovered.id.empty() && recovered.mode == 'B')) {
                    if (recovered.id.empty()) std::remove(cfg.watch_file.c_str());
                    last_session_id = recovered.id;
                    // r20p3.14.7 (OS3): atomic, same as the main .done write below.
                    {
                        std::string previous;
                        // A completed identified trigger is deliberately retained
                        // until the next producer begin. Restart cannot overwrite
                        // its already published receipt with a fabricated failure.
                        if (recovered.id.empty() || !atts::read_report(done_file, recovered.id, previous)) {
                            const std::string tmp = done_file + ".tmp";
                            bool written = false;
                            { std::ofstream df(tmp, std::ios::trunc);
                              df << atts::envelope(recovered.id, tts_delivery_proof_on() ?
                                  "FAILED 0 0" : "COMPLETE") << "\n";
                              df.close(); written = bool(df); }
                            const bool renamed = ::rename(tmp.c_str(), done_file.c_str()) == 0;
                            atts::trace_event("daemon", written && renamed ? "receipt_publish" : "receipt_publish_failed",
                                              recovered.id, tts_delivery_proof_on() ? "FAILED 0 0" : "COMPLETE");
                        } else atts::trace_event("daemon", "recovery_receipt_preserve", recovered.id, "PRESENT");
                    }
                    std::cerr << "[orpheus-speak] recovered stale completed session at startup"
                                 " — completion boundary released without replay\n";
                } else {
                    std::cerr << "[orpheus-speak] stale in-progress trigger found at startup —"
                                 " leaving for the live session to complete\n";
                }
            }
        }

        if (!atts::session_id_on() && !start_watch()) return 1;

        char evbuf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

        while (true) {
            // ── Idle: block until trigger file activity ──────────────────
            int len = read(ifd, evbuf, sizeof(evbuf));
            if (len <= 0) break;

            // Check if the event is for our trigger file, and classify type.
            // IN_MOVED_TO: speak-daemon.sh did atomic mv → batch mode (no END sentinel)
            // IN_CLOSE_WRITE: talk-llama streaming append → streaming mode (expects END)
            bool batch_trigger = false;
            bool stream_trigger = false;
            int pos = 0;
            while (pos < len) {
                auto *ev = reinterpret_cast<struct inotify_event *>(&evbuf[pos]);
                if (ev->len > 0 && watch_base == ev->name) {
                    if (ev->mask & IN_MOVED_TO)    batch_trigger = true;
                    if (ev->mask & IN_CLOSE_WRITE) stream_trigger = true;
                }
                pos += sizeof(struct inotify_event) + ev->len;
            }
            if (!batch_trigger && !stream_trigger) continue;

            // Verify file actually exists (could be stale event after drain)
            {
                struct stat st;
                if (stat(cfg.watch_file.c_str(), &st) != 0) continue;
            }

            atts::Input input(cfg.watch_file);
            if (!input.file) continue; // producer may be replacing the pathname
            if (!input.id.empty()) {
                if (input.id == last_session_id) continue;
                batch_trigger = input.mode == 'B';
            }
            atts::trace_event("daemon", "input_accept", input.id, batch_trigger ? "B" : "S");
            // Completion report for the .done file: "COMPLETE", or
            // "INTERRUPTED <line> <char>" after a barge-in abort.
            std::string session_report = "COMPLETE";

            // r24.20: the real legacy-stream and wrapper-batch probes both
            // synthesized and returned COMPLETE after an owned stop was already
            // queued at this admission boundary. SSE checked it inside its own
            // loop, so mode selection accidentally decided whether a queued
            // cancellation counted. Share this one pre-dispatch observation.
            // ATHENA_TTS_QUEUED_STOP=0 restores the prior mode-local handling.
            // An unlabelled manual request still has no identified stop owner;
            // this does not claim to cancel later legacy HTTP/player stalls.
            static const bool queued_stop_on = [] {
                const char *e = std::getenv("ATHENA_TTS_QUEUED_STOP");
                return !(e && e[0] == '0');
            }();
            std::string queued_stop_path = cfg.watch_file;
            const auto queued_dot = queued_stop_path.rfind('.');
            if (queued_dot != std::string::npos) queued_stop_path.resize(queued_dot);
            queued_stop_path += ".stop";
            const bool stopped_before_start = queued_stop_on && !input.id.empty() &&
                atts::stop_requested(queued_stop_path, input.id);
            if (stopped_before_start) {
                session_report = "INTERRUPTED 0 0";
                atts::trace_event("daemon", "stop_observe", input.id, "STOP");
            } else if (batch_trigger) {
                // ── Batch mode: speak-daemon.sh wrote complete file via mv ──
                // No ---END--- sentinel — read entire file, process, done.
                // This is the backward-compatible fallback for when
                // --stream-file is not set on talk-llama.
                if (cfg.verbose) {
                    std::cerr << "[orpheus-speak] batch session (speak-daemon.sh)\n";
                }
                std::string text = input.id.empty() ? read_text_file(cfg.watch_file) :
                    std::string((std::istreambuf_iterator<char>(input.file)), {});
                if (!text.empty()) {
                    const bool spoken = speak_text(cfg, snac, text);
                    if ((tts_delivery_proof_on() && !spoken) ||
                        (tts_pcm_proof_on() && cfg.play_cmd.empty())) session_report = "FAILED 0 0";
                }
            } else if (cfg.stream_tts) {
                // ── Streaming via SSE + incremental window decode ─────────
                // Tokens decode to PCM as they arrive; a persistent raw-PCM
                // player gives gapless ordered playback and exact drain.
                session_report = run_sse_session(cfg, snac, &input);
            } else {

            // ── Streaming: continuous pipeline until ---END--- ──────────
            // Unlike the batch approach (read → speak_text → read → speak_text),
            // this maintains a SINGLE sliding window across the entire session.
            // New sentences are read and chunked into the pipeline between each
            // collect/play cycle, so Orpheus generation for the next chunk
            // overlaps with playback of the current chunk — no gaps.
            if (cfg.verbose) {
                std::cerr << "[orpheus-speak] session started\n";
            }

            static constexpr int max_inflight = 5;
            static constexpr int CHUNK_CHAR_LIMIT = 300;

            // Pipeline state — persists across sentence arrivals
            std::vector<std::string> chunks;
            std::vector<std::future<HttpResult>> http_futures;
            size_t next_submit = 0;
            size_t next_collect = 0;

            // Sentence accumulator for chunking
            std::string pending_chunk;

            // File reading state
            size_t file_pos = input.start;
            bool end_received = false;

            // Playback state
            std::string wav_buf[2] = { cfg.output_wav, cfg.output_wav + ".buf" };
            int buf_idx = 0;
            std::thread play_thread;
            std::atomic<bool> delivery_failed{false};
            auto play_end_time = std::chrono::high_resolution_clock::now();

            // Stats
            float total_server_ms = 0, total_decode_ms = 0;
            int total_tokens = 0;
            size_t total_pcm_samples = 0;
            auto t0 = std::chrono::high_resolution_clock::now();

            // ── Helper: flush pending_chunk into chunks vector ──
            auto flush_pending = [&]() {
                if (!pending_chunk.empty()) {
                    chunks.push_back(pending_chunk);
                    http_futures.resize(chunks.size());
                    pending_chunk.clear();
                }
            };

            // ── Helper: read available lines, chunk sentences ──
            auto read_new_lines = [&]() {
                if (end_received) return;
                if (!input.current(cfg.watch_file)) {
                    end_received = true;
                    delivery_failed = true;
                    return;
                }
                std::ifstream legacy;
                if (input.id.empty()) legacy.open(cfg.watch_file);
                std::ifstream &ifs = input.id.empty() ? legacy : input.file;
                if (!input.id.empty()) ifs.clear();
                if (!ifs) return;
                ifs.seekg(file_pos);
                std::string line;
                while (std::getline(ifs, line)) {
                    // r20p3.14.7 (OS1): a torn line (no trailing '\n' on disk yet)
                    // sets eofbit; consuming it desyncs the indices and, worse,
                    // tellg()==-1 poisons file_pos so ---END--- is never seen and
                    // the session hangs. Leave file_pos and re-read whole next
                    // poll. (Same guard as the primary reader above.)
                    if (ifs.eof()) break;
                    file_pos = ifs.tellg();
                    while (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    if (line == "---END---") {
                        flush_pending();
                        end_received = true;
                        return;
                    }
                    // Group sentences into chunks ≤ CHUNK_CHAR_LIMIT
                    if (!pending_chunk.empty() &&
                        (int)(pending_chunk.size() + 1 + line.size()) > CHUNK_CHAR_LIMIT) {
                        chunks.push_back(pending_chunk);
                        http_futures.resize(chunks.size());
                        pending_chunk.clear();
                    }
                    if (!pending_chunk.empty()) pending_chunk += " ";
                    pending_chunk += line;
                }
            };

            // ── Helper: submit chunks up to sliding window limit ──
            auto submit_available = [&]() {
                while (next_submit < chunks.size() &&
                       (next_submit - next_collect) < (size_t)max_inflight) {
                    http_futures[next_submit] = std::async(std::launch::async,
                        http_generate, cfg.api_url, cfg.voice,
                        chunks[next_submit], std::cref(cfg),
                        (next_submit == 0) && cfg.verbose);
                    next_submit++;
                }
            };

            // ── Collect-and-decode helper (appends PCM to accumulator) ──
            std::vector<float> pcm_accum;
            float accum_audio_ms = 0;
            int accum_chunks = 0;

            auto collect_one = [&]() -> bool {
                if (next_collect >= next_submit) return false;

                if (cfg.verbose) {
                    std::cerr << "[orpheus-speak]   chunk " << next_collect + 1
                              << ": \"" << chunks[next_collect].substr(0, 60)
                              << (chunks[next_collect].size() > 60 ? "..." : "")
                              << "\"\n";
                }

                HttpResult hr = http_futures[next_collect].get();
                next_collect++;

                // Top up pipeline while we process this result
                read_new_lines();
                if (next_collect >= chunks.size() && !pending_chunk.empty())
                    flush_pending();
                submit_available();

                if (!hr.ok) {
                    std::cerr << "[orpheus-speak]   chunk " << next_collect
                              << " HTTP failed, skipping\n";
                    delivery_failed = true;
                    return false;
                }
                total_server_ms += hr.server_ms;

                std::string gen_text = extract_text_from_json(hr.response);
                if (gen_text.empty()) gen_text = hr.response;
                std::vector<int> audio_tokens = extract_audio_tokens(gen_text, false);
                if (audio_tokens.empty()) { delivery_failed = true; return false; }
                int usable = ((int)audio_tokens.size() / ORPHEUS_FRAME_TOKENS)
                           * ORPHEUS_FRAME_TOKENS;
                if (audio_tokens.size() != (size_t)usable) delivery_failed = true;
                audio_tokens.resize(usable);
                total_tokens += usable;
                SnacCodes codes = deinterleave_tokens(audio_tokens, false);
                auto t_s0 = std::chrono::high_resolution_clock::now();
                std::vector<float> pcm = snac.decode(cfg, codes);
                auto t_s1 = std::chrono::high_resolution_clock::now();
                total_decode_ms += std::chrono::duration<float, std::milli>(t_s1 - t_s0).count();
                if (!tts_pcm_proven(pcm, codes.codes0.size() * SnacDecoder::SAMPLES_PER_FRAME)) {
                    delivery_failed = true; return false;
                }
                total_pcm_samples += pcm.size();

                // Append to accumulator — multiple chunks become one
                // seamless WAV with zero inter-chunk gaps.
                float audio_ms = (float)pcm.size() / cfg.sample_rate * 1000.0f;
                pcm_accum.insert(pcm_accum.end(), pcm.begin(), pcm.end());
                accum_audio_ms += audio_ms;
                accum_chunks++;
                return true;
            };

            // Buffer strategy: wait for MIN_BUFFER_INITIAL chunks before
            // FIRST playback to build a cushion, then switch to playing
            // whatever is available (MIN_BUFFER_SUBSEQUENT=1).  This avoids
            // the 10-30s silence gaps that occur when FILL demands 3 chunks
            // between every batch — after the initial buffer is built, the
            // REFILL phase keeps the pipeline fed during playback.
            static constexpr int MIN_BUFFER_INITIAL    = 1;
            static constexpr int MIN_BUFFER_SUBSEQUENT = 1;
            int min_buffer_now = MIN_BUFFER_INITIAL;

            while (true) {
                // ── FILL: collect + decode until buffer ready ──────────
                while (accum_chunks < min_buffer_now) {
                    read_new_lines();
                    if (next_collect >= chunks.size() && !pending_chunk.empty())
                        flush_pending();
                    submit_available();

                    if (next_collect < next_submit) {
                        collect_one();
                    } else {
                        // Nothing in flight — play what we have or wait
                        if (accum_chunks > 0 || end_received) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }

                // Nothing decoded and session over → done
                if (pcm_accum.empty()) {
                    if (end_received && next_collect >= chunks.size()) break;
                    continue;
                }

                // ── PLAY: concatenated PCM as one seamless WAV ────────
                if (play_thread.joinable()) play_thread.join();
                {
                    auto now = std::chrono::high_resolution_clock::now();
                    if (now < play_end_time)
                        std::this_thread::sleep_until(play_end_time);
                }

                if (cfg.verbose && accum_chunks > 1) {
                    std::cerr << "[orpheus-speak]   playing " << accum_chunks
                              << " chunks combined (" << (int)accum_audio_ms
                              << " ms audio)\n";
                }

                if (!write_wav(wav_buf[buf_idx], pcm_accum, cfg.sample_rate)) {
                    delivery_failed = true;
                    pcm_accum.clear();
                    accum_audio_ms = 0;
                    accum_chunks = 0;
                    continue;
                }
                if (!cfg.play_cmd.empty()) {
                    play_end_time = std::chrono::high_resolution_clock::now()
                                  + std::chrono::milliseconds((int)accum_audio_ms + 100);
                    std::string wav = wav_buf[buf_idx];
                    std::string pcmd = cfg.play_cmd;
                    play_thread = std::thread([pcmd, wav, &delivery_failed]() {
                        if (play_audio(pcmd, wav) != 0) delivery_failed = true;
                    });
                }
                buf_idx = 1 - buf_idx;

                pcm_accum.clear();
                accum_audio_ms = 0;
                accum_chunks = 0;

                // After first batch, switch to "play whatever is ready"
                // mode — the pipeline is primed, no need to re-buffer.
                min_buffer_now = MIN_BUFFER_SUBSEQUENT;

                // ── REFILL: collect more while current batch plays ────
                // While the combined WAV plays, decode the next batch.
                // Uses wait_for() instead of blocking .get() so we can
                // exit promptly when playback ends — even if Orpheus is
                // mid-generation.  Whatever chunks are ready at that
                // point get played immediately by FILL (min_buffer=1).
                {
                    auto now = std::chrono::high_resolution_clock::now();
                    while (now < play_end_time) {
                        read_new_lines();
                        if (next_collect >= chunks.size() && !pending_chunk.empty())
                            flush_pending();
                        submit_available();

                        if (next_collect < next_submit) {
                            // Non-blocking check: is the next result ready?
                            auto status = http_futures[next_collect].wait_for(
                                std::chrono::milliseconds(500));
                            if (status == std::future_status::ready) {
                                collect_one();
                            }
                            // If not ready, loop checks clock → exits if
                            // playback ended, avoiding the blocking gap.
                        } else if (end_received) {
                            break;
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        now = std::chrono::high_resolution_clock::now();
                    }
                }

                // Check if we're completely done
                if (end_received && next_collect >= chunks.size()
                    && pcm_accum.empty()) break;
            }

            // Wait for final playback
            if (play_thread.joinable()) play_thread.join();
            {
                auto now = std::chrono::high_resolution_clock::now();
                if (now < play_end_time)
                    std::this_thread::sleep_until(play_end_time);
            }
            std::remove(wav_buf[1].c_str());

            // Print session timing
            if (cfg.verbose && total_pcm_samples > 0) {
                auto t_end = std::chrono::high_resolution_clock::now();
                auto total_ms = std::chrono::duration<float, std::milli>(t_end - t0).count();
                float audio_sec = (float)total_pcm_samples / cfg.sample_rate;
                std::cerr << "[orpheus-speak] streaming timing:\n"
                          << "  llama-server:  " << total_server_ms
                          << " ms (" << total_tokens << " tokens)\n"
                          << "  SNAC decode:   " << total_decode_ms << " ms\n"
                          << "  total wall:    " << total_ms << " ms\n"
                          << "  audio length:  " << audio_sec * 1000.0f << " ms\n"
                          << "  chunks:        " << chunks.size() << "\n"
                          << "  pipeline:      continuous sliding window (max "
                          << max_inflight << " in-flight)\n";
            }

            if ((tts_delivery_proof_on() && delivery_failed.load()) ||
                (tts_pcm_proof_on() && !chunks.empty() && cfg.play_cmd.empty())) session_report = "FAILED 0 0";

            } // end streaming mode (else branch)

            // ── Cleanup (shared by batch + streaming) ────────────────────
            // Identified input stays until its producer replaces it. Unlinking
            // or draining here deleted a newer reply published during playback.
            // Its nonce deduplicates all old directory events without losing a
            // new one; there is still only one trigger and one receipt on disk.
            last_session_id = input.id;
            if (input.id.empty() && input.current(cfg.watch_file)) {
            std::remove(cfg.watch_file.c_str());

            // Drain ALL pending inotify events.  Each sentence append
            // triggered IN_CLOSE_WRITE; those events are now stale.
            // Without this drain, stale events would trigger duplicate
            // sessions that re-process deleted/recreated files.
            {
                int flags = fcntl(ifd, F_GETFL, 0);
                fcntl(ifd, F_SETFL, flags | O_NONBLOCK);
                while (read(ifd, evbuf, sizeof(evbuf)) > 0) { /* discard */ }
                fcntl(ifd, F_SETFL, flags);  // restore blocking
            }
            }

            // Signal completion LAST.  The moment talk-llama reads the
            // report it may start a new session (barge-in resume writes the
            // unspoken remainder immediately), so the old trigger file and
            // its stale events must already be gone by then.
            // ── r20p3.14.7 (OS3): write it ATOMICALLY ─────────────────────────
            // std::ofstream(done_file) truncates to zero on open and only flushes
            // the report when the temporary is destroyed, so talk-llama's 50 ms
            // stat()-then-open poll could catch the .done EMPTY — and
            // tts_parse_done reads an empty report as COMPLETE, silently turning
            // an INTERRUPTED turn (the barge just wrote "INTERRUPTED 7 42") into a
            // fully-committed one: she remembers saying the whole thing she was
            // cut off in. Rename is atomic, so the reader sees the file whole or
            // not at all — the same durable pattern the voice sidecar uses.
            {
                const std::string tmp = done_file + ".tmp";
                bool written = false;
                { std::ofstream df(tmp, std::ios::trunc);
                  df << atts::envelope(input.id, session_report) << "\n";
                  df.close(); written = bool(df); }
                const bool renamed = ::rename(tmp.c_str(), done_file.c_str()) == 0;
                atts::trace_event("daemon", written && renamed ? "receipt_publish" : "receipt_publish_failed",
                                  input.id, session_report);
            }

            if (cfg.verbose) {
                std::cerr << "[orpheus-speak] session complete\n";
            }
        }

        close(ifd);

    } else {
        // ── Single-shot mode ─────────────────────────────────────────────
        std::string text;
        if (!cfg.input_file.empty()) {
            text = read_text_file(cfg.input_file);
        } else {
            text = cfg.text;
        }

        // Trim
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();

        if (text.empty()) {
            std::cerr << "[orpheus-speak] no input text\n";
            usage(argv[0]);
            snac.cleanup();
            return 1;
        }

        bool ok = speak_text(cfg, snac, text);
        snac.cleanup();
        curl_global_cleanup();
        return ok ? 0 : 1;
    }
}
