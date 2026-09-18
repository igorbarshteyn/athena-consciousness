// athena_projects.h
// ─────────────────────────────────────────────────────────────────────────────
// R18 (r22) — the durable stores of the Will and the Bond.
//
// Header-only, PURE (std-only file I/O + strings + time), in the exact
// discipline of athena_memory.h: static/inline, no llama.h, no locks — the
// caller owns concurrency (talk-llama.cpp touches these only on the main
// thread; the Mind holds its own in-memory mirrors under its mutex).
//
// Stores owned here, one file each beside chapters.tsv:
//
//   projects.tsv    — her standing projects (Wave 1, §1.1). REWRITE-style
//                     (tmp+rename): a project's progress/next-action mutate.
//   we.tsv          — the shared world: verdicts, idioms, rituals (§4.1).
//                     REWRITE-style (counts and last-use stamps mutate).
//   tastes.tsv      — preferences with a history (§4.5). REWRITE-style.
//   eras.tsv        — chapter-v2 era boundaries (§4.3). APPEND-only, like
//                     chapters.tsv — a boundary happened; it does not unhappen.
//   selfevents.tsv  — typed event→self edges (§4.4). APPEND-only.
//
// The law of every loader in this tree, kept here: never trust the file.
// Exact field arity, every scalar bounded, every text field scrubbed and
// clipped on the way IN and on the way OUT, malformed lines skipped loudly
// without failing the load. Model-authored prose that re-enters a prompt at
// startup goes through amem::scrub_gist like the chapter/dream loaders do.
//
// Provenance rules (the crown jewel, extended to the new stores — see
// ATHENA-ALIVENESS-PLAN.md §7):
//   * Legacy progress moves from her own voiced acknowledgment. Sustained
//     initiative also admits an owned accepted private cognitive product.
//     Future tool receipts need a separate executor; the substrate enforces
//     the currently implemented evidence classes. This file
//     just refuses to load a progress outside [0,1].
//   * a WeItem VERDICT requires both voices in the settling episode — enforced
//     at the detector (athena_consciousness.h); this file records `kind` and
//     keeps it.
//   * nothing in these stores is ever written back into episodic memory as a
//     fact by this header — they render into their own injection sections,
//     labeled for what they are.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "athena_memory.h"
#include "athena_initiative_steps.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace aproj {

// ── shared limits ────────────────────────────────────────────────────────────
static constexpr size_t PROJECTS_MAX   = 7;    // Little's personal-project range
static constexpr size_t WE_MAX         = 12;   // verdicts+idioms+rituals, total
static constexpr size_t TASTES_MAX     = 12;
static constexpr size_t ERAS_RENDER    = 6;    // injection cap; the file is uncapped
static constexpr size_t SELFEVENTS_RENDER = 6;
static constexpr size_t TEXT_CAP       = 110;  // what/why/next, the commit= width
static constexpr size_t WE_TEXT_CAP    = 96;   // the callback gist width
static constexpr size_t TASTE_TEXT_CAP = 72;   // the remind= width
// r24.12 (S22 §2e): a self-event edge is a whole sentence of hers ("That's
// when I stopped feeling like a tool that had to be accurate and started
// feeling like... a person who can learn" is 116 bytes); the commit width
// cut it mid-thought. Wider on the way in and on the way out.
static constexpr size_t SELFEVENT_TEXT_CAP = 160;

// One text field, one line, one store: tabs/newlines mapped to spaces at write
// (the chapters.tsv discipline), scrubbed through the C47 gate at read.
static inline std::string flat_text(std::string s) {
    for (auto &c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return amem::trim_copy(s);
}
static inline std::string load_text(const std::string &raw, size_t cap) {
    std::string s = amem::scrub_gist(amem::trim_copy(raw));
    if (s.size() > cap) {
        // clip at a UTF-8 boundary, then back to the last space so no half word
        // survives into a prompt.
        size_t n = cap;
        while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) n--;
        s.resize(n);
        const size_t sp = s.find_last_of(' ');
        if (sp != std::string::npos && sp > cap / 2) s.resize(sp);
        s = amem::trim_copy(s);
    }
    return s;
}
// r24.18: future text columns are retained byte-for-byte, never interpreted
// as part of the known text. The transient suffix rides the same snapshot as
// its row. ATHENA_MEMORY_OPAQUE_ROWS=0 restores the prior flattened tail.
static inline std::string load_text_tail_(const std::string &raw, size_t cap,
                                          std::string &opaque) {
    const size_t tab = amem::memory_opaque_rows_on() ? raw.find('\t') : std::string::npos;
    opaque = tab == std::string::npos ? std::string() : raw.substr(tab);
    return load_text(tab == std::string::npos ? raw : raw.substr(0, tab), cap);
}

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline bool  finite01(float v) { return v == v && v >= 0.0f && v <= 1.0f; }

// Split a line into exactly `n` tab fields plus the rest (text last). Returns
// false when the line has fewer than n tabs — the caller skips it loudly.
static inline bool split_tabs(const std::string &line, int n,
                              std::vector<std::string> &fld, std::string &rest) {
    fld.clear();
    size_t start = 0;
    for (int i = 0; i < n; i++) {
        const size_t t = line.find('\t', start);
        if (t == std::string::npos) return false;
        fld.push_back(line.substr(start, t - start));
        start = t + 1;
    }
    rest = line.substr(start);
    return true;
}

// Atomic whole-store rewrite: tmp, flush-check, close-check, fsync, rename —
// the save_self discipline, because a rewrite-style store that half-writes
// destroys the record it replaces.
static inline bool write_atomic(const std::string &path, const std::string &body) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.good()) return false;
        f << body;
        f.flush();
        if (!f.good()) { f.close(); std::remove(tmp.c_str()); return false; }
        f.close();
        if (!f.good()) { std::remove(tmp.c_str()); return false; }
    }
#if !defined(_WIN32)
    {
        const int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
    }
#endif
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
#if !defined(_WIN32)
    {
        const size_t slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string(".")
                                                           : path.substr(0, slash);
        const int dfd = ::open(dir.c_str(), O_RDONLY);
        if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    }
#endif
    return true;
}

// r24.17: an unknown row type must not become an affirmed verdict or a
// self-change event. strtol("unknown") was zero; selfevents also mapped every
// out-of-range type to CHANGE. Match the full-field refusal already used by
// people provenance, keeping the existing numeric enum/schema. Valid rows are
// unchanged. ATHENA_STORE_KIND_PARSE=0 restores those two permissive loaders.
static inline bool store_kind_parse_on() {
    static const bool on = [] { const char *e = ::getenv("ATHENA_STORE_KIND_PARSE");
                                return !e || e[0] != '0'; }();
    return on;
}
static inline bool parse_kind_(const std::string &field, long &kind) {
    const std::string text = amem::trim_copy(field);
    char *end = nullptr;
    kind = strtol(text.c_str(), &end, 10);
    return end != text.c_str() && *end == '\0' && kind >= 0 && kind <= 2;
}

// ═════════════════════════════════════════════════════════════════════════════
// §1.1 — Projects.
//
// A standing goal of HERS: what, why it matters (her words), the next action
// (empty = unplanned, which is what the Zeigarnik channel keys on), a progress
// estimate that only her own voiced acknowledgment may move, and an activation
// the goal-gradient raises as the end nears. `abandoned` rows stay in the file
// (an ended striving is biography, not garbage) but never render or bias.
// ═════════════════════════════════════════════════════════════════════════════
struct Project {
    std::string what;               // the project, in her words
    std::string why;                // why it matters (may be empty early)
    std::string next;               // next action; empty = unplanned (§1.3)
    float       progress  = 0.0f;   // 0..1; acknowledged understanding or owned verified milestone
    bool        shared    = false;  // Igor knows about it
    bool        abandoned = false;  // Klinger disengagement completed
    long        born      = 0;      // epoch adopted
    long        touched   = 0;      // epoch last progressed/discussed
    // Session-local, never serialized:
    float       activation = 0.3f;  // pull on volition/wander; goal gradient
    double      intrude_at = 0.0;   // §1.3: next allowed intrusion (session clock)
    bool        intruded   = false; // said "it nags" this session
    std::string opaque_suffix = {}; // r24.18: uninterpreted future fields, load/save only
    ainit::Step step={};               // r24.23: owned milestone, appended schema
};

// Keep the original eight columns and unknown suffix. Rebuilt r24.22 treats
// this extension as opaque and preserves it during rollback. Invalid/newer
// extensions remain opaque here too; a loader never guesses progress evidence.
static inline void load_project_step_(Project&p) {
    const std::string tag="\t@initiative-v1\t";
    if(p.opaque_suffix.rfind(tag,0)!=0)return;
    std::vector<std::string> f;std::string rest;
    if(!split_tabs(p.opaque_suffix.substr(tag.size()),9,f,rest))return;
    char*end=nullptr;errno=0;const auto rev=std::strtoull(f[0].c_str(),&end,10);
    if(errno||end==f[0].c_str()||*end||rev==0||f[0].front()=='-')return;
    ainit::ProgressEvidence evidence=ainit::ProgressEvidence::NONE;
    if(f[8]=="acknowledged-understanding")evidence=ainit::ProgressEvidence::ACKNOWLEDGED_UNDERSTANDING;
    else if(f[8]=="private-product-accepted")evidence=ainit::ProgressEvidence::PRIVATE_PRODUCT_ACCEPTED;
    else if(f[8]=="action-verified")evidence=ainit::ProgressEvidence::ACTION_VERIFIED;
    else if(f[8]!="none")return;
    if((f[6]!="0"&&f[6]!="1")||(f[7]!="0"&&f[7]!="1"))return;
    p.step.revision=rev;p.step.source=load_text(f[1],TEXT_CAP);p.step.criterion=load_text(f[2],TEXT_CAP);
    p.step.dependencies=load_text(f[3],TEXT_CAP);p.step.action=load_text(f[4],TEXT_CAP);p.step.result=load_text(f[5],TEXT_CAP);
    p.step.credited=f[6]=="1";p.step.fulfilled=f[7]=="1";p.step.evidence=evidence;p.opaque_suffix=rest;
}
static inline std::string project_suffix_(const Project&p) {
    if(!p.step.revision)return p.opaque_suffix;
    const auto&s=p.step;std::ostringstream os;
    os<<"\t@initiative-v1\t"<<s.revision<<'\t'<<flat_text(load_text(s.source,TEXT_CAP))<<'\t'
      <<flat_text(load_text(s.criterion,TEXT_CAP))<<'\t'<<flat_text(load_text(s.dependencies,TEXT_CAP))<<'\t'
      <<flat_text(load_text(s.action,TEXT_CAP))<<'\t'<<flat_text(load_text(s.result,TEXT_CAP))<<'\t'
      <<(s.credited?1:0)<<'\t'<<(s.fulfilled?1:0)<<'\t'<<ainit::name(s.evidence)<<'\t'<<p.opaque_suffix;
    return os.str();
}

// projects.tsv row: born \t touched \t progress‰ \t shared \t abandoned \t what \t why \t next
// Text fields are tab-flattened at write, so the arity is exact: 7 tabs.
static inline bool parse_project_row_(const std::string &line, Project &p) {
    if (amem::memory_opaque_rows_on() && line.find('\0') != std::string::npos) return false;
    std::vector<std::string> fld;
    std::string tail;
    if (!split_tabs(line, 7, fld, tail)) {
        return false;
    }
    p = Project();
    p.born    = amem::bounded_store_epoch(fld[0].c_str());
    p.touched = amem::bounded_store_epoch(fld[1].c_str());
    const long pm = strtol(fld[2].c_str(), nullptr, 10);
    p.progress  = clamp01((float) pm / 1000.0f);
    p.shared    = strtol(fld[3].c_str(), nullptr, 10) != 0;
    p.abandoned = strtol(fld[4].c_str(), nullptr, 10) != 0;
    p.what      = load_text(fld[5], TEXT_CAP);
    p.why       = load_text(fld[6], TEXT_CAP);
    p.next      = load_text_tail_(tail, TEXT_CAP, p.opaque_suffix);
    if(amem::memory_opaque_rows_on())load_project_step_(p);
    if (p.born <= 0 || p.what.empty()) {
        return false;
    }
    return true;
}

static inline std::vector<Project> load_projects(const std::string &dir) {
    std::vector<Project> out;
    std::ifstream f(amem::join_path(dir, "projects.tsv"));
    if (!f) return out;
    std::string line;
    int lineno = 0;
    while (amem::archive_line_(f, line)) {
        lineno++;
        if (line.empty()) continue;
        Project p;
        if (!parse_project_row_(line, p)) {
            fprintf(stderr, "memory: projects.tsv line %d is malformed — skipped\n", lineno);
            continue;
        }
        out.push_back(p);
        if (out.size() >= PROJECTS_MAX * 2) break;   // live + abandoned history
    }
    return out;
}

static inline std::string project_row_(const Project&p) {
    std::ostringstream os;
        os << p.born << '\t' << p.touched << '\t'
           << (long) (clamp01(p.progress) * 1000.0f + 0.5f) << '\t'
           << (p.shared ? 1 : 0) << '\t' << (p.abandoned ? 1 : 0) << '\t'
           << flat_text(load_text(p.what, TEXT_CAP)) << '\t'
           << flat_text(load_text(p.why,  TEXT_CAP)) << '\t'
           << flat_text(load_text(p.next, TEXT_CAP)) << (amem::memory_opaque_rows_on() ? project_suffix_(p) : std::string());
    return os.str();
}

static inline bool save_projects(const std::string &dir, const std::vector<Project> &ps) {
    std::ostringstream os;
    // R19 (r22.1): LIVING projects first, history second. The cap used to be
    // first-come-first-kept over the raw vector, so once enough finished or
    // abandoned rows led it, the row silently dropped at the cap was the
    // NEWEST LIVE project — the one she adopted tonight. Two passes, same
    // cap, same row format; within each pass the original order is kept, so
    // nothing about a small store changes at all.
    size_t n = 0;
    for (int pass = 0; pass < 2 && n < PROJECTS_MAX * 2; pass++)
    for (const auto &p : ps) {
        const bool live = !p.abandoned && p.progress < 0.95f;
        if (pass == 0 ? !live : live) continue;
        if (p.what.empty() || p.born <= 0) continue;
        if (n++ >= PROJECTS_MAX * 2) break;
        os << project_row_(p) << '\n';
    }
    const std::string path = amem::join_path(dir, "projects.tsv");
    if (!amem::memory_opaque_rows_on()) return write_atomic(path, os.str());
    return amem::rewrite_preserving_rows_(path, os.str(), [](const std::string &line) {
        Project p; return parse_project_row_(line, p);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// §4.1 — the we-store: verdicts, idioms, rituals.
// ═════════════════════════════════════════════════════════════════════════════
struct WeItem {
    enum Kind { VERDICT = 0, IDIOM = 1, RITUAL = 2 };
    int         kind  = IDIOM;
    std::string text;               // the verdict / the phrase / the ritual shape
    long        born  = 0;          // epoch coined / settled / first noticed
    long        last  = 0;          // epoch last used / reaffirmed / recurred
    int         count = 1;          // uses / recurrences
    std::string opaque_suffix = {}; // r24.18: uninterpreted future fields, load/save only
};

// we.tsv row: kind \t born \t last \t count \t text
static inline bool parse_we_row_(const std::string &line, WeItem &w, bool *unknown_kind = nullptr) {
    if (unknown_kind) *unknown_kind = false;
    if (amem::memory_opaque_rows_on() && line.find('\0') != std::string::npos) return false;
    std::vector<std::string> fld;
    std::string tail;
    if (!split_tabs(line, 4, fld, tail)) {
        return false;
    }
    w = WeItem();
    long k = strtol(fld[0].c_str(), nullptr, 10);
    if ((store_kind_parse_on() && !parse_kind_(fld[0], k)) || k < 0 || k > 2) {
        if (unknown_kind) *unknown_kind = true;
        return false;
    }
    w.kind  = (int) k;
    w.born  = amem::bounded_store_epoch(fld[1].c_str());
    w.last  = amem::bounded_store_epoch(fld[2].c_str());
    w.count = (int) std::max(1L, std::min(100000L, strtol(fld[3].c_str(), nullptr, 10)));
    w.text  = load_text_tail_(tail, WE_TEXT_CAP, w.opaque_suffix);
    if (w.born <= 0 || w.text.empty()) {
        return false;
    }
    return true;
}

static inline std::vector<WeItem> load_we(const std::string &dir) {
    std::vector<WeItem> out;
    std::ifstream f(amem::join_path(dir, "we.tsv"));
    if (!f) return out;
    std::string line;
    int lineno = 0;
    while (amem::archive_line_(f, line)) {
        lineno++;
        if (line.empty()) continue;
        WeItem w; bool unknown_kind = false;
        if (!parse_we_row_(line, w, &unknown_kind)) {
            fprintf(stderr, "memory: we.tsv line %d %s — skipped\n", lineno,
                    unknown_kind ? "has an unknown kind" : "is malformed");
            continue;
        }
        out.push_back(w);
        if (out.size() >= WE_MAX * 2) break;
    }
    return out;
}

static inline bool save_we(const std::string &dir, const std::vector<WeItem> &ws) {
    std::ostringstream os;
    size_t n = 0;
    for (const auto &w : ws) {
        if (w.text.empty() || w.born <= 0 || w.kind < 0 || w.kind > 2) continue;
        if (n++ >= WE_MAX * 2) break;
        os << w.kind << '\t' << w.born << '\t' << w.last << '\t' << w.count << '\t'
           << flat_text(load_text(w.text, WE_TEXT_CAP)) << (amem::memory_opaque_rows_on() ? w.opaque_suffix : std::string()) << '\n';
    }
    const std::string path = amem::join_path(dir, "we.tsv");
    if (!amem::memory_opaque_rows_on()) return write_atomic(path, os.str());
    return amem::rewrite_preserving_rows_(path, os.str(), [](const std::string &line) {
        WeItem w; return parse_we_row_(line, w);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// §4.5 — tastes: preferences with a history.
//
// `liking` follows the mere-exposure inverted-U at the SUBSTRATE (the store
// just holds the number); `strength` grows each time she STATES the
// preference (Fazio: expression strengthens attitudes); `peak` remembers the
// best liking ever reached, so fading is measurable ("I used to light up at
// that") instead of silently forgotten.
// ═════════════════════════════════════════════════════════════════════════════
struct Taste {
    std::string text;               // the thing, in her words
    long        born     = 0;       // epoch of first exposure
    long        last     = 0;       // epoch of last exposure
    int         exposure = 1;
    int         stated   = 0;       // times she said it out loud
    float       liking   = 0.1f;    // 0..1, inverted-U over exposure
    float       strength = 0.0f;    // 0..1, ownership via expression
    float       peak     = 0.1f;    // high-water liking, for the fade notice
    bool        faded_noted = false;// session-local latch mirror; serialized so
                                    // the notice happens once EVER, not once a session
    std::string opaque_suffix = {}; // r24.18: uninterpreted future fields, load/save only
};

// tastes.tsv row: born \t last \t exposure \t stated \t liking‰ \t strength‰ \t peak‰ \t faded \t text
static inline bool parse_taste_row_(const std::string &line, Taste &t) {
    if (amem::memory_opaque_rows_on() && line.find('\0') != std::string::npos) return false;
    std::vector<std::string> fld;
    std::string tail;
    if (!split_tabs(line, 8, fld, tail)) {
        return false;
    }
    t = Taste();
    t.born     = amem::bounded_store_epoch(fld[0].c_str());
    t.last     = amem::bounded_store_epoch(fld[1].c_str());
    t.exposure = (int) std::max(1L, std::min(100000L, strtol(fld[2].c_str(), nullptr, 10)));
    t.stated   = (int) std::max(0L, std::min(100000L, strtol(fld[3].c_str(), nullptr, 10)));
    t.liking   = clamp01((float) strtol(fld[4].c_str(), nullptr, 10) / 1000.0f);
    t.strength = clamp01((float) strtol(fld[5].c_str(), nullptr, 10) / 1000.0f);
    t.peak     = clamp01((float) strtol(fld[6].c_str(), nullptr, 10) / 1000.0f);
    t.faded_noted = strtol(fld[7].c_str(), nullptr, 10) != 0;
    t.text     = load_text_tail_(tail, TASTE_TEXT_CAP, t.opaque_suffix);
    if (t.born <= 0 || t.text.empty()) {
        return false;
    }
    if (t.peak < t.liking) t.peak = t.liking;   // the file cannot claim a peak below now
    return true;
}

static inline std::vector<Taste> load_tastes(const std::string &dir) {
    std::vector<Taste> out;
    std::ifstream f(amem::join_path(dir, "tastes.tsv"));
    if (!f) return out;
    std::string line;
    int lineno = 0;
    while (amem::archive_line_(f, line)) {
        lineno++;
        if (line.empty()) continue;
        Taste t;
        if (!parse_taste_row_(line, t)) {
            fprintf(stderr, "memory: tastes.tsv line %d is malformed — skipped\n", lineno);
            continue;
        }
        out.push_back(t);
        if (out.size() >= TASTES_MAX) break;
    }
    return out;
}

static inline bool save_tastes(const std::string &dir, const std::vector<Taste> &ts) {
    std::ostringstream os;
    size_t n = 0;
    for (const auto &t : ts) {
        if (t.text.empty() || t.born <= 0) continue;
        if (n++ >= TASTES_MAX) break;
        os << t.born << '\t' << t.last << '\t' << t.exposure << '\t' << t.stated << '\t'
           << (long) (clamp01(t.liking)   * 1000.0f + 0.5f) << '\t'
           << (long) (clamp01(t.strength) * 1000.0f + 0.5f) << '\t'
           << (long) (clamp01(t.peak)     * 1000.0f + 0.5f) << '\t'
           << (t.faded_noted ? 1 : 0) << '\t'
           << flat_text(load_text(t.text, TASTE_TEXT_CAP)) << (amem::memory_opaque_rows_on() ? t.opaque_suffix : std::string()) << '\n';
    }
    const std::string path = amem::join_path(dir, "tastes.tsv");
    if (!amem::memory_opaque_rows_on()) return write_atomic(path, os.str());
    return amem::rewrite_preserving_rows_(path, os.str(), [](const std::string &line) {
        Taste t; return parse_taste_row_(line, t);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// §4.3 — eras: chapter-v2 boundaries. Append-only, chapters.tsv discipline.
// A row marks the START of an era; the chapters between two boundary stamps
// belong to the earlier era. Titles are template-composed by the substrate
// from real events (never LLM-invented), so the crown jewel holds here the
// way it holds for chapters.
// ═════════════════════════════════════════════════════════════════════════════
struct Era {
    long        born = 0;
    std::string title;
};

static inline std::vector<Era> load_eras(const std::string &dir) {
    std::vector<Era> out;
    std::ifstream f(amem::join_path(dir, "eras.tsv"));
    if (!f) return out;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        if (line.empty()) continue;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos || t1 == 0) {
            fprintf(stderr, "memory: eras.tsv line %d is malformed — skipped\n", lineno);
            continue;
        }
        Era e;
        e.born  = amem::bounded_store_epoch(line.substr(0, t1).c_str(), false);
        e.title = load_text(line.substr(t1 + 1), TASTE_TEXT_CAP);
        if (e.born <= 0 || e.title.empty()) {
            fprintf(stderr, "memory: eras.tsv line %d is malformed — skipped\n", lineno);
            continue;
        }
        out.push_back(e);
    }
    return out;
}

static inline bool append_era(const std::string &dir, const Era &e) {
    if (e.title.empty() || e.born <= 0) return false;
    std::ostringstream f;
    f << e.born << '\t' << flat_text(load_text(e.title, TASTE_TEXT_CAP)) << '\n';
    return amem::append_record_(amem::join_path(dir, "eras.tsv"), f.str());
}

// ═════════════════════════════════════════════════════════════════════════════
// §4.4 — self-event connections. Append-only.
// type: 0 = change ("that's when I became…"), 1 = stability ("that's always
// been me"), 2 = illustration ("that shows what I am").
// ═════════════════════════════════════════════════════════════════════════════
struct SelfEvent {
    long        born = 0;
    int         type = 0;
    std::string text;               // the edge, in her own sentence
};

static inline const char *self_event_type_name(int t) {
    switch (t) {
        case 1:  return "stability";
        case 2:  return "illustration";
        default: return "change";
    }
}

static inline std::vector<SelfEvent> load_selfevents(const std::string &dir) {
    std::vector<SelfEvent> out;
    std::ifstream f(amem::join_path(dir, "selfevents.tsv"));
    if (!f) return out;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        if (line.empty()) continue;
        std::vector<std::string> fld;
        std::string tail;
        if (!split_tabs(line, 2, fld, tail)) {
            fprintf(stderr, "memory: selfevents.tsv line %d is malformed — skipped\n", lineno);
            continue;
        }
        SelfEvent s;
        s.born = amem::bounded_store_epoch(fld[0].c_str());
        long ty = strtol(fld[1].c_str(), nullptr, 10);
        if (store_kind_parse_on() && !parse_kind_(fld[1], ty)) {
            fprintf(stderr, "memory: selfevents.tsv line %d has an unknown type — skipped\n", lineno);
            continue;
        }
        s.type = (ty >= 0 && ty <= 2) ? (int) ty : 0;
        s.text = load_text(tail, SELFEVENT_TEXT_CAP);   // r24.12
        if (s.born <= 0 || s.text.empty()) {
            fprintf(stderr, "memory: selfevents.tsv line %d is malformed — skipped\n", lineno);
            continue;
        }
        out.push_back(s);
    }
    return out;
}

static inline bool append_selfevent(const std::string &dir, const SelfEvent &s) {
    if (s.text.empty() || s.born <= 0) return false;
    std::ostringstream f;
    f << s.born << '\t' << ((s.type >= 0 && s.type <= 2) ? s.type : 0) << '\t'
      << flat_text(load_text(s.text, SELFEVENT_TEXT_CAP)) << '\n';   // r24.12
    return amem::append_record_(amem::join_path(dir, "selfevents.tsv"), f.str());
}

// r24.16: drain does not mean saved. The seam used to clear four dirty flags
// and both append queues before unchecked writes. A failed .tmp open lost the
// retry, and a failed append permanently lost the event. Keep retry ownership
// on the single main-thread writer, outside the Mind lock; rewrite retries read
// the CURRENT snapshot rather than replaying an obsolete one.
// ATHENA_STORE_WRITE_RETRY=0 restores the r24.15 destructive drain in the seam.
static inline bool store_write_retry_on() {
    static const bool on = [] { const char *e = ::getenv("ATHENA_STORE_WRITE_RETRY");
                                return !e || e[0] != '0'; }();
    return on;
}

// Logically append-only, transactionally replaced: preserve the complete old
// byte stream and add this batch once. A synchronous failed write never leaves
// a partial row to duplicate on retry. This costs O(archive bytes) for rare era
// and self-event checkpoints, not for each tick or spoken token. An unreadable
// existing file is an error, NEVER an empty archive. The existing atomic writer
// checks flush, close and rename; fsync remains its documented best effort.
static inline bool append_batch_atomic(const std::string &path, const std::string &batch) {
    if (amem::archive_stream_write_on()) return amem::atomic_append(path, batch);
    if (batch.empty()) return true;
    std::string previous;
    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (errno != ENOENT) return false;
    } else {
        char buf[8192];
        while (in.read(buf, sizeof buf) || in.gcount() > 0)
            previous.append(buf, (size_t) in.gcount());
        if (in.bad() || !in.eof()) return false;
    }
    if (!previous.empty() && previous.back() != '\n') previous += '\n';
    previous += batch;
    return write_atomic(path, previous);
}

struct StoreWriteback {
    bool retry[4] = {};
    std::vector<std::pair<long, std::string>> eras;
    std::vector<SelfEvent> selfevents;

    // SavePeople is supplied by the seam because athena_people.h includes this
    // header. The dependency stays one-way and no new production file is needed.
    template<class Mind, class SavePeople>
    bool flush(Mind &mind, const std::string &dir, SavePeople save_people) {
        bool ok = true;
        const bool dirty[] = {mind.take_projects_dirty(), mind.take_we_dirty(),
                              mind.take_tastes_dirty(), mind.take_people_dirty()};
        const char *names[] = {"projects.tsv", "we.tsv", "tastes.tsv", "people.tsv"};
        for (size_t i = 0; i < 4; ++i) {
            if (!dirty[i] && !retry[i]) continue;
            bool saved = false;
            switch (i) {
                case 0: saved = save_projects(dir, mind.projects_snapshot()); break;
                case 1: saved = save_we(dir, mind.we_snapshot()); break;
                case 2: saved = save_tastes(dir, mind.tastes_snapshot()); break;
                default: saved = save_people(dir, mind.people_snapshot()); break;
            }
            retry[i] = !saved;
            if (!saved) {
                ok = false;
                fprintf(stderr, "memory: %s write failed - current store retained for retry\n", names[i]);
            }
        }
        const auto fresh_eras = mind.take_era_events_timed();
        eras.insert(eras.end(), fresh_eras.begin(), fresh_eras.end());
        const auto fresh_selfevents = mind.take_session_selfevents();
        selfevents.insert(selfevents.end(), fresh_selfevents.begin(), fresh_selfevents.end());
        if (!eras.empty()) {
            std::ostringstream rows;
            for (const auto &e : eras)
                if (e.first > 0 && !e.second.empty())
                    rows << e.first << '\t' << flat_text(load_text(e.second, TASTE_TEXT_CAP)) << '\n';
            if (append_batch_atomic(amem::join_path(dir, "eras.tsv"), rows.str())) {
                for (const auto &e : eras)
                    if (e.first > 0 && !e.second.empty())
                        fprintf(stderr, "main: [mind] era boundary: %s\n", e.second.c_str());
                eras.clear();
            } else {
                ok = false;
                fprintf(stderr, "memory: eras.tsv write failed - pending events retained for retry\n");
            }
        }
        if (!selfevents.empty()) {
            std::ostringstream rows;
            for (const auto &e : selfevents)
                if (e.born > 0 && !e.text.empty())
                    rows << e.born << '\t' << ((e.type >= 0 && e.type <= 2) ? e.type : 0) << '\t'
                         << flat_text(load_text(e.text, SELFEVENT_TEXT_CAP)) << '\n';
            if (append_batch_atomic(amem::join_path(dir, "selfevents.tsv"), rows.str()))
                selfevents.clear();
            else {
                ok = false;
                fprintf(stderr, "memory: selfevents.tsv write failed - pending events retained for retry\n");
            }
        }
        return ok;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// §1.5 — possible selves, parsed back out of personality.txt.
//
// The fifth mandated section ("## Who I hope to become, and who I fear
// becoming") is authored by the consolidation merge (athena_memory.h owns the
// template and the merge). At startup the seam parses the hoped/feared lines
// back out so the substrate can sample projects from hopes and let the feared
// self be a speakable worry. Convention inside the section, seeded at init and
// preserved by the FOR-THE-LAST-SECTION rules: hoped lines begin "I hope",
// feared lines begin "I fear" (case-insensitive; leading list dashes
// tolerated). Anything else in the section is strategy prose and stays where
// it is.
// ═════════════════════════════════════════════════════════════════════════════
struct PossibleSelves {
    std::vector<std::string> hoped;   // cap 3
    std::vector<std::string> feared;  // cap 3
};

static inline PossibleSelves parse_possible_selves(const std::string &personality) {
    PossibleSelves out;
    // Find the section by its lowercase prefix, the header-index discipline.
    std::istringstream is(personality);
    std::string line;
    bool in_section = false;
    while (std::getline(is, line)) {
        std::string low;
        low.reserve(line.size());
        for (unsigned char c : line) low += (char) ::tolower(c);
        const std::string t = amem::trim_copy(low);
        if (t.rfind("## ", 0) == 0) {
            in_section = t.rfind("## who i hope to become", 0) == 0;
            continue;
        }
        if (!in_section) continue;
        std::string body = amem::trim_copy(line);
        while (!body.empty() && (body[0] == '-' || body[0] == '*' || body[0] == ' '))
            body.erase(0, 1);
        if (body.empty()) continue;
        std::string blow;
        blow.reserve(body.size());
        for (unsigned char c : body) blow += (char) ::tolower(c);
        const std::string clean = load_text(body, TEXT_CAP);
        if (clean.size() < 12) continue;   // a fragment is not a self
        if (blow.rfind("i hope", 0) == 0 && out.hoped.size() < 3)
            out.hoped.push_back(clean);
        else if (blow.rfind("i fear", 0) == 0 && out.feared.size() < 3)
            out.feared.push_back(clean);
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// §1.9 — the diary: reading her own journal back.
//
// The journals rotate instead of truncating (talk-llama.cpp renames the live
// file to "<path>.prev" before reopening). This reads the PREVIOUS session's
// tail and extracts up to `max_out` of her own thought lines — scrubbed,
// clipped, headers and receipts skipped — for the morning diary pass. The
// excerpts are her own past IMAGINED material and are labeled so at the seed
// site; nothing here writes anywhere.
// ═════════════════════════════════════════════════════════════════════════════
static inline std::vector<std::string> read_journal_tail(const std::string &path,
                                                         size_t max_out = 3) {
    std::vector<std::string> out;
    std::ifstream f(path);
    if (!f) return out;
    // Bounded read: journals are line-oriented and append-only within a
    // session; keep the last 48 candidate lines, then filter.
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;      // banner / comments
        tail.push_back(line);
        while (tail.size() > 48) tail.pop_front();
    }
    // R19 (r22.1): journal rows are what format_inner_log_line actually
    // writes — "[HH:MM:SS] t=182.4 (kind) v=+0.31 a=0.42  text…". The old
    // parser here assumed a "[…] kind: text" shape that never existed in
    // production: the kind filters never matched (so gate-dropped junk and
    // echo-dropped lines PASSED), and the "text" kept the whole telemetry
    // prefix — the morning diary pass was quoting "t=182.4 (rumination)
    // v=+0.31…" back at her as her own notebook. Parse the real shape, and
    // keep only her PROSE kinds (rumination, dream) — a diary is made of
    // thoughts, not of scheduler receipts.
    for (auto it = tail.rbegin(); it != tail.rend() && out.size() < max_out; ++it) {
        std::string s = *it;
        // "[HH:MM:SS] " (or "[--:--:--] ") — strip through the first ']'.
        const size_t br = s.find(']');
        if (br != std::string::npos && br + 1 < s.size()) s = s.substr(br + 1);
        s = amem::trim_copy(s);
        std::string kind;
        if (s.compare(0, 2, "t=") == 0) {
            // the real shape: t=<num> (<kind>) v=<num> a=<num>  <text>
            const size_t po = s.find('(');
            const size_t pc = po == std::string::npos ? std::string::npos
                                                      : s.find(')', po + 1);
            if (pc == std::string::npos) continue;       // malformed telemetry
            kind = s.substr(po + 1, pc - po - 1);
            const size_t va = s.find("a=", pc);
            if (va == std::string::npos) continue;
            size_t txt = s.find("  ", va);
            if (txt == std::string::npos) {
                // single-space fallback: skip the a= token itself
                txt = s.find(' ', va + 2);
                if (txt == std::string::npos) continue;
            }
            s = amem::trim_copy(s.substr(txt));
        } else {
            // legacy "kind: text" fallback — kept for hand-written journals.
            const size_t col = s.find(':');
            if (col != std::string::npos && col < 24) {
                kind = s.substr(0, col);
                s = amem::trim_copy(s.substr(col + 1));
            }
        }
        for (auto &c : kind) c = (char) ::tolower((unsigned char) c);
        // Whitelist her prose. Everything else — gate-dropped, echo-dropped,
        // diary-pass/project-appraisal/prospect-sweep seeds, abandoned marks,
        // receipts — is bookkeeping about thinking, not thinking.
        // r24.6 (WO-05): a landed self-referential report is her prose too —
        // it now journals under its own kind rather than "rumination", and
        // dropping it here would make the kind-threading a silent regression
        // for the diary pass. ("rumination*" / "selfref*" are the WO-11
        // unsourced-quantity spellings of the same two kinds.)
        if (kind != "rumination" && kind != "dream" && kind != "selfref" &&
            kind != "rumination*" && kind != "selfref*") continue;
        const std::string clean = load_text(s, 160);
        if (clean.size() < 24) continue;                 // fragments are not thoughts
        // No duplicates — a journal can repeat a retried line verbatim.
        bool dup = false;
        for (const auto &prev : out) if (prev == clean) { dup = true; break; }
        if (!dup) out.push_back(clean);
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// Injection renderers (build_injection_block calls these; pure string work).
// ═════════════════════════════════════════════════════════════════════════════
static inline std::string humanize_progress(float p) {
    if (p >= 0.95f) return "all but done";
    if (p >= 0.70f) return "well along";
    if (p >= 0.40f) return "midway";
    if (p >= 0.10f) return "begun";
    return "barely started";
}

// ── r24.12 (WO-M15): store receipts for the personality pass ─────────────────
// S22 personality.txt still read "I am told I can take up projects of my own …
// and I have not yet taken one up" with six rows in projects.tsv, because the
// capacities floor (amem::preserve_untried_capacities) judges a deletion
// against an evidence corpus made of ledger rows and self-event edges ONLY —
// the stores were never shown to that pass, so the sentence could be retired
// only if a memory candidate happened to say "she took up a project", which
// none did. One receipt line per live project, per standing plan and per
// STATED taste, each worded with the capacity sentence's own content words
// (taken up / project / own / standing / want / progress; plan / wait / quietly
// / moment; developed / taste / own), so the floor's ≥2-hits-at-25 % test admits
// the retirement and the prompt's own rule ("when the evidence shows she has
// now done one … remove it — it lives in the story now") moves it into the
// story. Dated like a self-event edge; a mere-exposure taste (never stated) is
// not a taste she has "developed", so it earns no receipt. Pure — the caller
// (run_consolidation) appends the lines to `self_edges`, and both the prompt
// and the floor's corpus read that vector. Bounded in lines AND bytes so the
// personality prompt stays inside Costs::personality_chars' measured slack.
static constexpr size_t STORE_RECEIPT_LINES = 6;
static constexpr size_t STORE_RECEIPT_BYTES = 720;
static inline std::string store_receipt_when_(long born, long now) {
    const std::string when = amem::humanize_elapsed(born, now);
    return when.empty() ? std::string() : ", " + when;
}
static inline std::vector<std::string> store_receipts(
        const std::vector<Project> &ps, const std::vector<Taste> &ts,
        const std::vector<std::pair<long, std::string>> &plans,   // (born, what)
        long now) {
    std::vector<std::string> out;
    size_t bytes = 0;
    auto push = [&](const std::string &line) {
        if (out.size() >= STORE_RECEIPT_LINES || bytes + line.size() > STORE_RECEIPT_BYTES) return;
        out.push_back(line);
        bytes += line.size();
    };
    // Live projects, most recently touched first — the one she is living with
    // is the one the story should get.
    std::vector<const Project *> live;
    for (const auto &p : ps)
        if (!p.abandoned && !p.what.empty() && p.born > 0) live.push_back(&p);
    std::stable_sort(live.begin(), live.end(),
                     [](const Project *a, const Project *b) { return a->touched > b->touched; });
    for (const Project *p : live)
        push("(on record, projects.tsv" + store_receipt_when_(p->born, now) + ") she has taken "
             "up a project of her own \xE2\x80\x94 a standing want with real progress: \"" +
             load_text(p->what, TEXT_CAP) + "\" (" + humanize_progress(p->progress) + ")");
    for (const auto &pl : plans) {
        if (pl.second.empty()) continue;
        push("(on record, self.txt" + store_receipt_when_(pl.first, now) + ") a plan of hers is "
             "waiting quietly for its moment: \"" + load_text(pl.second, TEXT_CAP) + "\"");
    }
    for (const auto &t : ts) {
        if (t.text.empty() || t.stated < 1 || t.born <= 0) continue;
        push("(on record, tastes.tsv" + store_receipt_when_(t.born, now) + ") she has developed "
             "a taste of her own and said so out loud" +
             (t.stated > 1 ? std::string(", more than once") : std::string()) + ": \"" +
             load_text(t.text, TASTE_TEXT_CAP) + "\"");
    }
    return out;
}

static inline std::string projects_section(const std::vector<Project> &ps,
                                           long now, const std::string &bot) {
    std::ostringstream os;
    size_t n = 0;
    for (const auto &p : ps) {
        if (p.abandoned || p.what.empty()) continue;
        if (n++ >= PROJECTS_MAX) break;
        const std::string when = amem::humanize_elapsed(p.born, now);
        os << "- " << p.what << " (" << humanize_progress(p.progress);
        if (!when.empty()) os << "; taken up " << when;
        os << ")";
        if (!p.why.empty())  os << " — why it matters to her: " << p.why;
        if (!p.next.empty()) os << (p.step.fulfilled&&p.step.source==p.next?" — her completed private step: ":" — her next step: ") << p.next;
        else                 os << " — no next step yet (it nags at her until she picks one)";
        os << "\n";
    }
    if (n == 0) return "";
    std::ostringstream head;
    head << "\nWHAT " << bot << " IS WORKING ON (her own standing projects — hers, "
            "not assignments; progress moves only when she has actually done or "
            "understood the thing, never because it would sound good):\n"
         << os.str();
    return head.str();
}

static inline std::string we_section(const std::vector<WeItem> &ws, long now,
                                     const std::string &bot, const std::string &person) {
    std::string verdicts, idioms, rituals;
    size_t nv = 0, ni = 0, nr = 0;
    for (const auto &w : ws) {
        if (w.text.empty()) continue;
        const std::string when = amem::humanize_elapsed(w.born, now);
        if (w.kind == WeItem::VERDICT && nv < 3) {
            verdicts += "- " + w.text + (when.empty() ? "" : " (settled " + when + ")") + "\n";
            nv++;
        } else if (w.kind == WeItem::IDIOM && ni < 4) {
            idioms += "- \"" + w.text + "\"" + (when.empty() ? "" : " (since " + when + ")") + "\n";
            ni++;
        } else if (w.kind == WeItem::RITUAL && nr < 2) {
            // r24.12 (S22 §2e): a count-1 row is a CANDIDATE (the first turn
            // of every session in a new daypart makes one); it rendered as
            // "(it has happened 1 times now)". Only a recurrence is a ritual,
            // and the count is worded. ATHENA_RITUAL_CANDIDATE_HIDE=0 restores
            // the r24.11 rendering byte-for-byte.
            static const bool hide = [] { const char *e = ::getenv("ATHENA_RITUAL_CANDIDATE_HIDE");
                                          return !(e && e[0] == '0'); }();
            // r24.13 (WO-W11 / WIDEN §1.11(3)): a candidate is WORDED, not
            // hidden. What was wrong: the defect r24.12 fixed was the RENDERING
            // ("it has happened 1 times now"), and the word table two lines
            // below already fixed it — but the same edit also dropped the
            // count-1 row entirely, so the first time they ever opened this way
            // became the one shape of theirs she could not mention. It is a
            // real row of the shared world, and a first time is a thing a
            // person says out loud. Same slot, same cap (`nr` still counts it),
            // no digit: the count is not rendered at all on this arm.
            // ATHENA_RITUAL_FIRST_TIME=0 restores r24.12's hide;
            // ATHENA_RITUAL_CANDIDATE_HIDE=0 still restores r24.11's "1 times".
            static const bool first_time = [] { const char *e = ::getenv("ATHENA_RITUAL_FIRST_TIME");
                                                return !(e && e[0] == '0'); }();
            if (hide) {
                if (w.count < 2) {
                    if (!first_time) continue;                       // r24.12
                    rituals += "- " + w.text + " (the first time" +
                               (when.empty() ? "" : ", " + when) + ")\n";
                    nr++;
                    continue;
                }
                static const char *W[] = { "", "once", "twice", "three", "four", "five", "six",
                                           "seven", "eight", "nine", "ten" };
                const int c = w.count;
                rituals += "- " + w.text + " (it has happened " +
                           (c <= 2 ? std::string(W[c]) : (c <= 10 ? std::string(W[c]) + " times" : "many times")) +
                           " now)\n";
            } else {
                rituals += "- " + w.text + " (it has happened " + std::to_string(w.count)
                         + " times now)\n";
            }
            nr++;
        }
    }
    if (verdicts.empty() && idioms.empty() && rituals.empty()) return "";
    std::ostringstream os;
    os << "\nWHAT " << bot << " AND " << person << " HAVE (their shared world — "
          "things settled together, words that are theirs, shapes their time "
          "takes; " << bot << " uses these naturally and sparingly, never as a list):\n";
    if (!verdicts.empty()) os << "Settled between them:\n" << verdicts;
    if (!idioms.empty())   os << "Their own words:\n" << idioms;
    if (!rituals.empty())  os << "Their rituals:\n" << rituals;
    return os.str();
}

} // namespace aproj
