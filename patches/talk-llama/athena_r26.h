// Athena-owned, value-only conversation support. No model, device, worker,
// permission, persistence or process dispatch belongs in this header.
#pragma once
#include "athena_evidence.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace a26 {

inline bool enabled(const char *name) {
    const char *master = std::getenv("ATHENA_R26");
    const char *value = std::getenv(name);
    // Rig acceptance is still required. Preserve the established behavior
    // until explicitly enabled; master 0 dominates individual settings.
    if (master && master[0] == '0') return false;
    if (value && value[0]) return value[0] != '0';
    return master && master[0] && master[0] != '0';
}
struct Options {
    bool access = true, meta = true, memory = true, dialogue = true;
    bool affect = true, private_continuity = true, timing = true, speech = true;
    static Options environment() {
        return {enabled("ATHENA_R26_ACCESS"), enabled("ATHENA_R26_META"),
                enabled("ATHENA_R26_MEMORY"), enabled("ATHENA_R26_DIALOGUE"),
                enabled("ATHENA_R26_AFFECT"), enabled("ATHENA_R26_PRIVATE"),
                enabled("ATHENA_R26_TIMING"), enabled("ATHENA_R26_SPEECH")};
    }
    static Options off() { return {false,false,false,false,false,false,false,false}; }
};

inline std::string bounded(const std::string &s, size_t limit) {
    if (s.size() <= limit) return s;
    size_t n = limit;
    while (n && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    return s.substr(0, n);
}
inline std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
inline bool word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c >= 128 || c == '\'';
}
inline std::set<std::string> words(const std::string &s) {
    static const std::set<std::string> stop={"what","that","this","with","about","have","your",
        "tell","think","please","would","could","should","there","they","them","were",
        "just","really","some","from","when","then","more","into","been","does"};
    std::set<std::string> out;
    size_t p = 0;
    while (p < s.size()) {
        while (p < s.size() && !word_byte(static_cast<unsigned char>(s[p]))) ++p;
        const size_t b = p;
        while (p < s.size() && word_byte(static_cast<unsigned char>(s[p]))) ++p;
        if (p - b >= 4) {
            const auto word=lower(s.substr(b,p-b));
            if (!stop.count(word)) out.insert(word);
        }
    }
    return out;
}
inline float overlap(const std::set<std::string> &a, const std::set<std::string> &b) {
    if (a.empty() || b.empty()) return 0;
    size_t n = 0;
    for (const auto &w : a) n += b.count(w);
    return float(n) / float(std::max(a.size(), b.size()));
}
inline bool refers_back(const std::string &text) {
    const auto s=lower(text);
    return s.size()<=128 && (s.find("before that")!=std::string::npos ||
        s.find("second one")!=std::string::npos || s.find("first one")!=std::string::npos ||
        s.find("that topic")!=std::string::npos || s.find("go back")!=std::string::npos);
}
inline bool same_source(const aev::SourceRef &a, const aev::SourceRef &b) {
    return a.event.valid() && b.event.valid() &&
           a.event.session == b.event.session && a.event.sequence == b.event.sequence &&
           a.version == b.version && a.actor == b.actor;
}

// A local low-confidence span is evidence only when the token bytes exactly
// reconstruct the source transcript. It is never a guessed replacement word.
struct TokenEvidence { std::string text; float probability = -1; };
struct Hearing {
    std::optional<float> signal, uptake;
    std::string uncertain_span;
    bool translated = false;
    bool needs_check() const {
        return (signal && *signal < 0.40f) || (uptake && *uptake < 0.40f);
    }
};
inline std::string uncertain_word(const std::string &text,
                                  const std::vector<TokenEvidence> &tokens) {
    if (text.empty() || text.size() > 16384 || tokens.size() > 4096) return {};
    std::string joined;
    std::vector<float> probability;
    for (const auto &t : tokens) {
        if (!std::isfinite(t.probability) || t.probability < 0 || t.probability > 1) return {};
        if (t.text.size() > text.size() - std::min(text.size(), joined.size())) return {};
        joined += t.text;
        probability.insert(probability.end(), t.text.size(), t.probability);
    }
    if (joined != text || probability.size() != text.size()) return {};
    size_t p = 0, low_words = 0, clear_words = 0;
    std::string candidate;
    while (p < text.size()) {
        while (p < text.size() && !word_byte(static_cast<unsigned char>(text[p]))) ++p;
        const size_t b = p;
        float least = 1;
        bool known = true;
        while (p < text.size() && word_byte(static_cast<unsigned char>(text[p]))) {
            const float q = probability[p++];
            if (!std::isfinite(q) || q < 0 || q > 1) known = false;
            else least = std::min(least, q);
        }
        if (p == b || !known) continue;
        if (least < 0.35f) {
            ++low_words;
            if (p - b <= 48) candidate = text.substr(b, p - b);
        } else if (least >= 0.65f) ++clear_words;
    }
    // Multiple doubtful words do not justify pretending only one is missing.
    return low_words == 1 && clear_words >= 3 ? candidate : std::string();
}

enum class MemorySupport { UNMEASURED, NO_MATCH, SOURCES, CONTRAST };
enum class Delivery { UNMEASURED, NONE, PARTIAL, COMPLETE, UNKNOWN };
struct Evidence {
    Hearing hearing;
    MemorySupport memory = MemorySupport::UNMEASURED;
    size_t memory_sources = 0;
    Delivery delivery = Delivery::UNMEASURED;
    std::string confirmed_prefix;
};

struct Continuation {
    std::string text, kind, actor;
    uint64_t source = 0, worker_request = 0;
    double age_s = -1;
    bool dream = false;
    aev::SourceRef context_source;
};

// A disposable topic cache, never a new obligation/task store. Each retained
// item keeps its speaker and source; nothing here infers permission or assent.
struct Thread {
    std::string text, actor;
    aev::SourceRef source;
    std::set<std::string> keys;
};
struct Dialogue {
    Thread current;
    std::string owner;
    std::deque<Thread> suspended;
    std::string received, received_tail, unresolved;
    Delivery delivery = Delivery::UNMEASURED;
    bool resumed = false;
    void input(const std::string &text, const aev::SourceRef &source,
               const std::string &actor, bool repair = false) {
        resumed = false;
        if (owner != actor) {
            current = {}; suspended.clear(); received.clear(); received_tail.clear(); unresolved.clear();
            delivery = Delivery::UNMEASURED;
        }
        owner=actor; // even a one-word/repair turn establishes speaker ownership
        const auto keys = words(text);
        if (repair || keys.size() < 2 || refers_back(text)) return;
        if (!current.text.empty() && overlap(keys, current.keys) < 0.18f) {
            for (auto it = suspended.begin(); it != suspended.end(); ++it) {
                if (it->actor == actor && overlap(keys, it->keys) >= 0.25f) {
                    Thread recovered = *it;
                    suspended.erase(it);
                    suspended.push_front(current);
                    current = std::move(recovered);
                    resumed = true;
                    break;
                }
            }
            if (!resumed) suspended.push_front(current);
        }
        if (!resumed) current = {bounded(text, 320), actor, source, keys};
        while (suspended.size() > 3) suspended.pop_back();
    }
    void receipt(const std::string &prefix, bool complete, bool received_known) {
        delivery = complete ? Delivery::COMPLETE : !prefix.empty() ? Delivery::PARTIAL :
                   received_known ? Delivery::NONE : Delivery::UNKNOWN;
        received = bounded(prefix,640);
        received_tail.clear();
        if (!prefix.empty()) {
            size_t start = prefix.size()>240 ? prefix.size()-240 : 0;
            while (start < prefix.size() && (static_cast<unsigned char>(prefix[start])&0xc0)==0x80) ++start;
            const auto space=prefix.find(' ',start);
            if (start && space!=std::string::npos && space+1<prefix.size()) start=space+1;
            received_tail = prefix.substr(start);
        }
        // "Unfinished" says only that delivery is incomplete, not what an
        // unseen draft contained. The authoritative source retains full text.
        unresolved = delivery == Delivery::PARTIAL ? received : std::string();
    }
};

// Bounded once-per-source effects. Failure to identify a source must retain the
// old behavior rather than merge unrelated legacy turns by text equality.
struct EventSet {
    std::deque<aev::SourceRef> seen;
    bool first(const aev::SourceRef &source) {
        if (!source.event.valid()) return true;
        for (const auto &old : seen) if (same_source(old, source)) return false;
        seen.push_back(source);
        if (seen.size() > 128) seen.pop_front();
        return true;
    }
};

enum class Floor { HUMAN, YIELDING, ATHENA, OVERLAP };
struct FloorView {
    Floor state = Floor::HUMAN;
    bool fresh_semantics = false;
    static FloorView observe(bool athena_speaking, bool human_speaking,
                             float silence_ms, bool semantic_input = false) {
        return {athena_speaking ? (human_speaking ? Floor::OVERLAP : Floor::ATHENA) :
                (human_speaking || silence_ms < 250 ? Floor::HUMAN : Floor::YIELDING),
                semantic_input};
    }
};
inline bool resume_overlap(bool lexical_continuer, const std::string &text,
                           float confidence, bool still_speaking, bool answer_due=false) {
    // A vocabulary match alone cannot erase a question, a continuing turn or
    // a doubtful recognition. The existing interruption path owns those cases.
    return lexical_continuer && !answer_due && !still_speaking &&
        text.find('?')==std::string::npos && std::isfinite(confidence) && confidence>=.70f;
}

// Conservative additional boundary protection. It never removes the existing
// fallback; callers still own sanitizer, stages, publication and receipts.
inline bool safe_clause(const std::string &s, size_t split) {
    if (split == std::string::npos || split == 0 || split > s.size()) return false;
    const size_t p = split - 1;
    // The next generated token may complete a number. At "12:" or "1,"
    // there is not yet evidence of a clause boundary; wait for the next byte.
    if ((s[p] == ',' || s[p] == ':' || s[p] == '.') && p > 0 &&
        s[p-1] >= '0' && s[p-1] <= '9' &&
        (split == s.size() || (s[split] >= '0' && s[split] <= '9'))) return false;
    int bracket = 0, angle = 0;
    for (size_t i = 0; i < split; ++i) {
        if (s[i] == '[' || s[i] == '(') ++bracket;
        if (s[i] == ']' || s[i] == ')') bracket = std::max(0, bracket - 1);
        if (s[i] == '<') ++angle;
        if (s[i] == '>') angle = std::max(0, angle - 1);
    }
    return bracket == 0 && angle == 0;
}
inline size_t early_clause(const std::string &s, size_t minimum = 28) {
    for (size_t i = minimum; i < s.size(); ++i)
        if ((s[i] == ',' || s[i] == ';' || s[i] == ':') && i + 1 < s.size() &&
            (s[i+1] == ' ' || s[i+1] == '\n') && safe_clause(s, i+1)) return i+1;
    return std::string::npos;
}

} // namespace a26
