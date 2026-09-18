// r24.19: session ownership for the existing local TTS file protocol.
// An actual late acknowledgement returned COMPLETE for a later reply which the
// daemon never submitted, then deleted that reply's trigger. Identity belongs
// to the opened input and its receipt, not to whichever file occupies the path
// when playback ends. ATHENA_TTS_SESSION_ID=0 restores the unlabelled protocol.
// Keep the brain and Orpheus copies identical; no library dependency is added.
#ifndef ATHENA_TTS_WIRE_H
#define ATHENA_TTS_WIRE_H
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
namespace atts {
// r24.20: real playback/wrapper probes left no durable owner/stop/receipt
// sequence after the ordinary protocol files were consumed. Trace only those
// discrete software events, in an explicitly supplied local session directory.
// No text/audio, fsync, retry wait or capture/PCM polling is introduced. A failed
// trace never changes delivery. ATHENA_TTS_PROTOCOL_TRACE=0 restores no trace.
// The memory submission trace shares this bounded append primitive; its own
// switch, path and event format remain independent of the speech protocol.
inline bool append_trace_record(const std::string &directory, const char *basename,
                                const std::string &row,
                                const std::string &limit_row) noexcept {
    const int saved_errno = errno;
    const auto append = [&]() -> bool {
        struct Fd { int value = -1; ~Fd() { if (value >= 0) ::close(value); } } dir, file;
        const auto failed = [&](const char *reason) {
            std::fprintf(stderr, "[athena-trace] incomplete file=%s reason=%s\n", basename ? basename : "unknown", reason);
            return false;
        };
        constexpr size_t cap = 1048576, reserve = 1024, max_row = 65536;
        if (!basename || !*basename || std::string(basename).find('/') != std::string::npos ||
            row.size() >= max_row || limit_row.size() >= reserve ||
            row.find_first_of("\r\n") != std::string::npos ||
            limit_row.find_first_of("\r\n") != std::string::npos)
            return failed("record");
        dir.value = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir.value < 0) return failed("directory");
        file.value = ::openat(dir.value, basename,
                             O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (file.value < 0) return failed("open");
        struct stat st;
        if (::fstat(file.value, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1)
            return failed("regular-file");
        if (::flock(file.value, LOCK_EX | LOCK_NB) != 0) return failed("busy");
        if (::fstat(file.value, &st) != 0 || st.st_size < 0) return failed("size");
        if ((unsigned long long)st.st_size >= cap) return failed("limit");
        const size_t size = (size_t)st.st_size;
        const bool full = size + row.size() + 1 > cap - reserve;
        std::string bytes = full ? limit_row : row;
        if (full) {
            if (size + bytes.size() + 1 > cap) return failed("limit");
            // JSON permits trailing whitespace. Filling this final line to the
            // cap makes the terminal loss explicit and prevents restart from
            // appending another apparent event after it. Never rotate/delete.
            bytes.append(cap - size - bytes.size() - 1, ' ');
        }
        bytes.push_back('\n');
        if (::write(file.value, bytes.data(), bytes.size()) != (ssize_t)bytes.size())
            return failed("write"); // partial bytes stay evidence, never retried
        return full ? failed("limit") : true;
    };
    bool ok = false;
    try { ok = append(); }
    catch (...) { std::fputs("[athena-trace] incomplete reason=exception\n", stderr); }
    errno = saved_errno;
    return ok;
}
inline void trace_event(const char *role, const char *event, const std::string &id,
                        const std::string &detail) noexcept {
    static const bool enabled = [] { const char *e = std::getenv("ATHENA_TTS_PROTOCOL_TRACE");
        return !(e && e[0] == '0'); }();
    const char *directory = std::getenv("ATHENA_TTS_PROTOCOL_TRACE_DIR");
    if (!enabled || !directory || !*directory) return;
    try {
        // Every caller supplies finite protocol data, never raw speech/report
        // bytes. Reject rather than clip an identity or emit an ambiguous row.
        const std::string r = role, e = event;
        if ((r != "brain" && r != "daemon" && r != "wrapper") || e.size() > 40 ||
            e.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos ||
            id.size() > 120 || id.find_first_not_of("0123456789-") != std::string::npos ||
            detail.size() > 80 || detail.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-") != std::string::npos) {
            std::fputs("[athena-trace] incomplete reason=event\n", stderr); return;
        }
        static std::atomic<unsigned long long> sequence{0};
        const auto seq = ++sequence;
        const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto mono = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const std::string prefix = "{\"schema\":1,\"wall_us\":" + std::to_string(wall) +
            ",\"mono_us\":" + std::to_string(mono) + ",\"pid\":" + std::to_string((long long)::getpid()) +
            ",\"seq\":" + std::to_string(seq) + ",\"role\":\"" + r + "\",\"event\":\"";
        const std::string row = prefix + e + "\",\"id\":\"" + id + "\",\"detail\":\"" + detail + "\"}";
        const std::string limit = prefix + "trace_incomplete\",\"id\":\"\",\"detail\":\"LIMIT\"}";
        append_trace_record(directory, ("tts-" + r + ".jsonl").c_str(), row, limit);
    } catch (...) { std::fputs("[athena-trace] incomplete reason=exception\n", stderr); }
}
inline bool session_id_on() {
    static const bool on = [] { const char *e = std::getenv("ATHENA_TTS_SESSION_ID");
        return !(e && e[0] == '0'); }();
    return on;
}
// The protocol is serialized by the brain. Process id, both clocks and a local
// sequence distinguish restarts and consecutive mini-sessions; this is ownership
// metadata, not an authentication secret.
inline std::string new_id() {
    static unsigned long long sequence = 0;
    return std::to_string((long long)::getpid()) + "-" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(++sequence);
}
inline std::string client_session_id; // brain main thread only; never a diag input
inline std::string header(const std::string &id, char mode) {
    return "---ATHENA_SESSION " + id + " " + mode + "---\n";
}
// Ownership packets must be atomic: queued directory events can observe a
// partly written header, and a legacy player can mistake an empty, newly opened
// stop file for its own unlabelled request. Share the same checked publication.
inline bool publish_packet(const std::string &path, const std::string &bytes) {
    std::string temporary = path + ".session-XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) return false;
    FILE *f = ::fdopen(fd, "w");
    bool ok = f != nullptr;
    if (f) {
        ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (std::fflush(f) != 0) ok = false;
        if (std::fclose(f) != 0) ok = false;
    } else ::close(fd);
    if (ok && ::rename(temporary.c_str(), path.c_str()) == 0) return true;
    std::remove(temporary.c_str());
    return false;
}
inline bool parse_header(const std::string &line, std::string &id, char &mode) {
    const std::string prefix = "---ATHENA_SESSION ";
    if (line.compare(0, prefix.size(), prefix) != 0 || line.size() <= prefix.size()+5 ||
        line.compare(line.size()-3, 3, "---") != 0) return false;
    const std::string body = line.substr(prefix.size(), line.size()-prefix.size()-3);
    const size_t space = body.find(' ');
    if (space == std::string::npos || space == 0 || space > 120 ||
        space+2 != body.size() || (body.back() != 'S' && body.back() != 'B') ||
        body.substr(0, space).find_first_not_of("0123456789-") != std::string::npos) return false;
    id = body.substr(0, space); mode = body.back(); return true;
}
inline std::string envelope(const std::string &id, const std::string &report) {
    return id.empty() ? report : "SESSION " + id + "\n" + report;
}
// A stale receipt is left in place until the daemon atomically replaces it.
// Removing a mismatching report could race removal of its newer replacement.
inline bool read_report(const std::string &path, const std::string &expected,
                        std::string &report) {
    std::ifstream f(path);
    if (!f && session_id_on() && !expected.empty()) return false;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    if (session_id_on() && !expected.empty()) {
        const std::string prefix = "SESSION " + expected + "\n";
        if (text.compare(0, prefix.size(), prefix) != 0) return false;
        text.erase(0, prefix.size());
    }
    report = std::move(text); return true;
}
// r24.20 review (SPEECH F1): a receipt that carries no "SESSION " label at all
// while this client is identified was written by an OLD daemon (r24.14 or
// stock): it has no atts::Input, it spoke the header line aloud, and it can
// never produce this client's matching receipt — the brain used to wait
// 36000 x 50 ms for one, silently, every turn. This is distinct from a foreign
// IDENTIFIED receipt (another session's), which read_report leaves for the
// daemon to replace. Only the first eight bytes are read; an empty or
// partially written file is "not legacy" (wait on). Consumed by the brain
// only; the daemon's stop_requested is untouched. Kept in both copies so the
// two files stay byte-identical, as the header above requires.
inline bool receipt_is_legacy(const std::string &path) {
    if (!session_id_on()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char head[8];
    f.read(head, sizeof head);
    const size_t n = (size_t) f.gcount();
    if (n == 0) return false;
    static const char label[] = "SESSION ";
    for (size_t i = 0; i < n && i < sizeof head; i++)
        if (head[i] != label[i]) return true;
    return false;
}
// The abort channel is part of the same ownership boundary. A stop for a
// queued successor used to be erased when its daemon session finally started.
inline void request_stop(const std::string &path, const std::string &id) {
    if (session_id_on() && !id.empty()) {
        if (publish_packet(path, envelope(id, "STOP\n")))
            trace_event("brain", "stop_publish", id, "STOP");
        else trace_event("brain", "stop_publish_failed", id, "STOP");
        return;
    }
    std::ofstream(path).put('1');
}
inline bool stop_requested(const std::string &path, const std::string &id,
                           std::atomic<bool> *observed = nullptr) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!session_id_on()) return true;
    std::string report;
    if (!read_report(path, id, report)) return false;
    // An unlabelled predecessor also cannot consume its identified successor's
    // stop. Legacy/manual stop files keep their existence-only meaning.
    const bool stopped = id.empty() ? report.compare(0, 8, "SESSION ") != 0 : report == "STOP\n";
    // The player must react before optional evidence I/O. Remember this actual
    // match with one atomic store; the session records it after playback joins.
    if (stopped && observed) observed->store(true);
    return stopped;
}
struct Input {
    std::ifstream file;
    std::string id;
    char mode = 'S';
    size_t start = 0;
    explicit Input(const std::string &path) : file(path) {
        if (!file || !session_id_on()) return;
        std::string first;
        if (std::getline(file, first) && !file.eof() && parse_header(first, id, mode))
            start = (size_t)file.tellg();
        else { file.clear(); file.seekg(0); }
    }
    bool current(const std::string &path) const {
        if (!session_id_on()) return true;
        Input candidate(path);
        // A live upgrade can replace an unlabelled, already playing input with
        // an identified one. Its late completion may not remove the new source.
        return candidate.id == id;
    }
};
} // namespace atts
#endif
