// athena_people.h
// ─────────────────────────────────────────────────────────────────────────────
// r24.13 (WO-H6) — the tenth store: the people in HIS life.
//
// Why this file exists, stated as the finding stated it (HUMAN §1.8): a thing
// about Igor survives a night if and only if some store has a row shape for it.
// Nine stores existed — selfevents, keepsakes, eras, dreams, chapters, we,
// tastes, selfref, projects — and every one of them is about HER. Measured over
// the two real S22 sessions: in S1 his world is populated (Caitlin, the kids,
// his grandfather, a cat that died, a vet, a dentist, a hospital, and at
// 19:28:17 the longest and warmest thing he said all night, about a friend
// moving across the country); in the whole of S2, nineteen hours later, those
// names appear ZERO times, while the octopus book and the hospital call — the
// two items the commitment ledger held a row shape for — are the first thing
// she raises. `Config::person_name` is one std::string. That is the whole of
// what the substrate knew about anybody but him.
//
// This is the tenth store and it is the FIRST that is about someone other than
// her. It is header-only and PURE (std-only file I/O and strings), in the exact
// discipline of athena_projects.h, whose helpers it reuses rather than copying:
// aproj::split_tabs, aproj::flat_text, aproj::load_text, aproj::write_atomic.
//
// THE COST PER TICK IS ZERO, and that is a design constraint, not a happy
// accident. Nothing here runs on the 10 Hz thread. The write happens once, in
// the memory-extraction pass that is already scheduled (at compaction and at
// close); the read is one linear scan of at most PEOPLE_MAX * 2 rows — what
// load_people reads and what Mind::install_people keeps, the aproj::WE_MAX * 2
// shape (r24.13, review A§5) — once, at the session's opening. There is no regex name-guessing anywhere: a name the
// extractor did not emit does not exist.
//
// THE PROVENANCE RULE (athena_projects.h states it for the Will and the Bond;
// this is its form here): everything a row says about a person may come only
// from something HE SAID, never from something she inferred, and never from
// something only she mentioned. `Person::src` records that and the loader
// refuses any value it does not know, so a row written by a future build whose
// provenance this one cannot vouch for is never spoken. Together with
// `Person::last` — the epoch he last said it — that is enough for any claim she
// makes about a person to be sourced: who said it, and when.
//
// WHAT IT IS NOT. It is not a knowledge graph. There are no relations between
// people, no attributes, no inference, no merging of aliases, no second store
// keyed on anything. One row per person he has named, at most PEOPLE_MAX of
// them, each of which can be REINFORCED and CORRECTED in place — the
// we.tsv merge path is the model — so the store does not grow a second row for
// a person he mentions twice.
//
// ATHENA_PEOPLE=0 restores r24.12 exactly: no file is written, no file is read,
// the extraction prompt carries no PERSON row type, and no clause is rendered.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "athena_projects.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace apeo {

// ── bounds ───────────────────────────────────────────────────────────────────
// PEOPLE_MAX is deliberately small. The finding's own warning is that a tenth
// store competes with projects/we/tastes for the personality prompt's room, and
// law 7 forbids a store that tries to be a knowledge graph. Twelve is what
// aproj::WE_MAX carries for the shared world; the file, like we.tsv, tolerates
// twice that on the way in so a store that grew under an older build still
// loads every row it can.
static constexpr size_t PEOPLE_MAX      = 12;
static constexpr size_t PEOPLE_NAME_CAP = 48;   // "his grandfather" is a name here too
static constexpr size_t PEOPLE_REL_CAP  = 48;
static constexpr size_t PEOPLE_SAID_CAP = 110;  // aproj::TEXT_CAP, the commit= width
// Never trust the file: a line longer than this cannot be one of ours (a full
// row at every cap is ~230 bytes) and is skipped loudly rather than scrubbed,
// clipped and stored.
//
// r24.13 (review C§m7): this comment used to end "…which is what keeps a 10 MB
// people.tsv — one enormous line or many — from becoming a 10 MB allocation
// walked field by field", and that was not true of the allocation. The loader
// read with std::getline(f, line), which allocates and copies the WHOLE line
// before any guard can look at its length; the guard prevented the field walk
// and nothing else, and the fixture's own "10 MB single line: 1 row(s) in
// 22.1 ms" was that allocation happening. apeo::read_line_bounded now reads
// through a fixed 512-byte buffer and holds at most PEOPLE_LINE_MAX + 1 bytes,
// so the claim above is now a property of the code rather than of the comment.
static constexpr size_t PEOPLE_LINE_MAX = 4096;
// r24.13 (review C§m7): the scan's own bounds. load_people's only stopping
// rule was `out.size() >= PEOPLE_MAX * 2`, which counts ACCEPTED rows — every
// malformed, over-long, wrong-provenance and duplicate-key line left it
// unchanged, so a people.tsv made entirely of such lines (a partial disk
// write, two stores `cat`ted together, a botched hand-edit) was read to EOF at
// startup, with a find_person scan and an fprintf per line, before her first
// turn and with nothing bounding it. This is a new on-disk parser; bounds are
// not optional. The largest legitimate store is PEOPLE_MAX * 2 rows of ~230
// bytes (~5.5 kB), so 2,400 lines is four hundred times the real thing and
// 64 MB is six times the largest file the safety fixture itself builds — both
// are ceilings on a runaway file, never on a real one. PEOPLE_SCAN_GRIPES
// bounds the DIAGNOSTIC, which on a session log is the slower half.
static constexpr size_t PEOPLE_SCAN_LINES  = PEOPLE_MAX * 200;   // 2,400
static constexpr size_t PEOPLE_SCAN_BYTES  = 64u * 1024u * 1024u;
static constexpr int    PEOPLE_SCAN_GRIPES = 8;

// ── the row ──────────────────────────────────────────────────────────────────
struct Person {
    // Provenance. HEARD is the only value this build writes, and the only one
    // load_people will admit: the row came out of the extraction pass reading a
    // transcript of HIS turns. A row carrying anything else was written by a
    // build that knows something this one does not, and a claim about a person
    // whose provenance cannot be vouched for is exactly the claim she must not
    // make — so it is refused on the way in, the way we.tsv refuses an unknown
    // `kind`.
    enum Src { HEARD = 0 };
    std::string name;               // the person, as HE says it
    std::string relation;           // their relation to him, in his words
    std::string said;               // what he last said about them
    long        first    = 0;       // epoch he first named them
    long        last     = 0;       // epoch he last said this  ← the "when"
    int         mentions = 1;       // times he has brought them up (reinforcement)
    int         asked    = 0;       // times SHE has raised them back to him
    int         src      = HEARD;
    std::string opaque_suffix = {}; // r24.18: future columns retained, never quoted
};

// ── small shared predicates ──────────────────────────────────────────────────
// One accessor, not three literals: the Mind's eligibility test, the loader and
// the fixture all ask the same question of the same function.
static inline bool has_digit(const std::string &s) {
    for (unsigned char c : s) if (c >= '0' && c <= '9') return true;
    return false;
}
// r24.13 (review A§2): one emphasis unwrapper, asked by the PERSON test and by
// every field it then parses, so the two cannot drift. `**PERSON**` and
// `_PERSON_` are the two decorations amem::parse_extracted's own C42 comment
// already names as habits ("**9**" is quoted there verbatim); the field values
// get the same treatment because a model that bolds the row type bolds the
// name in it.
static inline std::string unwrap_emphasis(const std::string &s) {
    std::string t = amem::trim_copy(s);
    // Alternating, because the wrapper and the whitespace nest either way
    // round: "PERSON** " and " **PERSON**" are both one pass short of clean.
    for (;;) {
        const size_t was = t.size();
        while (!t.empty() && (t.front() == '*' || t.front() == '_')) t.erase(t.begin());
        while (!t.empty() && (t.back()  == '*' || t.back()  == '_')) t.pop_back();
        t = amem::trim_copy(t);
        if (t.size() == was) break;
    }
    return t;
}
// The merge key. Case and punctuation are not identity — "Caitlin", "caitlin"
// and "Caitlin," are one person, and the extractor writes all three. Nothing
// cleverer than that: no aliasing, no nicknames, no fuzzy match. A key that
// guesses is a key that welds two people together, which is worse than a
// duplicate row.
//
// r24.13 (review C§1): the key used to keep a byte only when std::isalnum(c)
// was true. In the "C" locale — the only locale this build ever runs in, since
// nothing calls std::setlocale — every UTF-8 lead and continuation byte
// (>= 0x80) fails that test, so EVERY non-ASCII letter was deleted from the
// key. Two production consequences, both of them the thing the paragraph above
// says the key must never do. `norm_name("Ирина")` was "", so
// merge_person refused the row at its empty-key guard and a person whose name
// carries no Latin letter could never enter the store at all — in a store
// built for a Russian-speaking user's life. And `Anna Иванова` and
// `Anna Петрова` both normalised to "anna", so find_person matched
// and merge_person's reinforce arm pasted the second person's `relation` and
// `said` onto the first — after which the WO-H6 clause renders a sourced,
// quoted, attributed claim about a named real person that he never made. The
// same collapse hit every accented Latin pair: `Renée Dubois` and `Ren Dubois`
// both went to "ren dubois".
//
// The fix is BYTE-SAFE and needs no locale, no dependency and no UTF-8
// decoder: a byte >= 0x80 is kept VERBATIM and only ASCII is case-folded. Two
// spellings of the same non-ASCII name that differ in case are then two keys —
// which is a duplicate row, and this file's own rule is that a duplicate row
// is better than a weld.
static inline std::string norm_name(const std::string &s) {
    std::string out;
    bool sp = false;
    for (unsigned char c : s) {
        if (c >= 0x80 || std::isalnum(c)) {
            if (sp && !out.empty()) out += ' ';
            sp = false;
            out += (c < 0x80) ? (char) std::tolower(c) : (char) c;
        } else sp = true;
    }
    return out;
}

// ── the merge path: reinforce or correct, never duplicate ────────────────────
// The we.tsv model (Mind::we_idiom_locked_): a matching row is UPDATED in
// place, a new one is appended, and the store is capped by evicting the row he
// has left alone longest. The three ways a row moves:
//
//   * REINFORCED — he named them again: `mentions` goes up and `last` moves
//     forward, which is what makes the row rise to the top of the read.
//     r24.13 (review C§m4): `last` is what raises the row and `mentions` is
//     the TIEBREAK behind it — see Mind::take_person_note's selection, which
//     is the reader this column was missing. It said "which is what makes the
//     row rise" of a field nothing read; a comment is part of the deliverable,
//     so the field was wired to the read rather than the sentence softened.
//   * CORRECTED — he said something newer about them, or named the relation for
//     the first time: `said` and `relation` are REPLACED, not appended to. A
//     store that accumulates every sentence he ever said about his wife is the
//     knowledge graph this file refuses to be.
//   * KEPT — `first` never moves later, and `asked` is never the extractor's to
//     edit: the origin of the row, and her own record of having raised it, are
//     not claims about the person.
//
// One key search, used by the merge and by the loader's duplicate healing, so
// "the same person" means the same thing in both places.
static inline size_t find_person(const std::vector<Person> &ps, const std::string &key) {
    for (size_t i = 0; i < ps.size(); i++) if (norm_name(ps[i].name) == key) return i;
    return ps.size();
}

// Returns true when a NEW row was added; false when an existing one was
// reinforced or the input was refused.
static inline bool merge_person(std::vector<Person> &ps, const Person &in) {
    // A row with no name cannot be spoken; a row with no `last` cannot be
    // sourced (it is the epoch he said it); a row whose provenance this build
    // does not know may not be trusted at all.
    if (in.name.empty() || in.last <= 0 || in.src != Person::HEARD) return false;
    const std::string key = norm_name(in.name);
    if (key.empty()) return false;
    const size_t at = find_person(ps, key);
    if (at != ps.size()) {
        Person &p = ps[at];
        p.mentions += 1;
        if (in.last > p.last) p.last = in.last;
        if (in.first > 0 && (p.first <= 0 || in.first < p.first)) p.first = in.first;
        if (!in.relation.empty()) p.relation = in.relation;      // a correction
        if (!in.said.empty())     p.said     = in.said;          // the newest thing he said
        return false;
    }
    Person add = in;
    if (add.first <= 0)   add.first = add.last;
    if (add.mentions < 1) add.mentions = 1;
    if (add.asked < 0)    add.asked = 0;
    ps.push_back(add);
    // The cap, by disuse — the Mind::we_evict_locked_ rule without its spoken
    // notice, because a person he has stopped mentioning is not a thing she
    // should announce having forgotten.
    //
    // r24.13 (review A§5): ONE eviction per added row, which is what
    // Mind::we_evict_locked_ actually does and what the line above always
    // claimed. As a `while` this drained a store that arrived over-cap all the
    // way down to PEOPLE_MAX in a single call — and a store DOES arrive
    // over-cap: apeo::load_people and apeo::save_people both carry
    // PEOPLE_MAX * 2 ("live + the older build's history") and Mind::install_we,
    // the model for Mind::install_people, keeps WE_MAX * 2 in memory for
    // exactly that reason. With the `while`, the first person the extractor
    // found in a twenty-four-row store erased twelve rows at once and the next
    // athena_flush_stores wrote the truncated view back over people.tsv. One
    // per add is the same cap for any store this build grows from empty (the
    // vector is at most PEOPLE_MAX before the push_back, so the loop could only
    // ever run once), and it is the difference between a bounded store and an
    // in-memory trim that reaches the disk — WO-W2's own finding this round.
    //
    // r24.13 (review C§m4): `first` is the tiebreak, and this is the one read
    // that makes it a field rather than a column. Every row written by one
    // extraction pass carries the SAME `last` (Mind::note_person_rows stamps
    // them all with one now_wall), so ties here are ordinary, not exotic, and
    // untied they were broken by whichever row happened to sit earlier in the
    // vector. When he has left two people alone equally long, the one he has
    // known longest stays.
    if (ps.size() > PEOPLE_MAX) {
        size_t worst = 0;
        long   worst_last = -1, worst_first = -1;
        for (size_t i = 0; i < ps.size(); i++) {
            const long l = ps[i].last  > 0 ? ps[i].last  : ps[i].first;
            const long g = ps[i].first > 0 ? ps[i].first : l;
            if (worst_last < 0 || l < worst_last ||
                (l == worst_last && g > worst_first)) {
                worst_last = l; worst_first = g; worst = i;
            }
        }
        ps.erase(ps.begin() + (long) worst);
    }
    return true;
}

// ── the file ─────────────────────────────────────────────────────────────────
// people.tsv row:  src \t first \t last \t mentions \t asked \t name \t relation \t said
//
// Eight fields, seven tabs, the text fields tab-flattened at write so the arity
// is exact — the projects.tsv/we.tsv shape. REWRITE-style (aproj::write_atomic:
// tmp, flush-check, close-check, fsync, rename), because `mentions`, `last`,
// `said` and `asked` all mutate, and a half-written rewrite destroys the record
// it replaces. Created lazily on the first save, exactly like we.tsv: absence
// is normal and never blocks startup.
//
// FORWARD COMPATIBILITY. A file written by a later build may carry a ninth
// column. `said` is the tail, so an unknown column would otherwise be pasted
// into a prompt with a tab in the middle of it; instead the tail is cut at its
// first tab and the remainder ignored. A row whose `src` this build does not
// know is refused outright — see Person::Src.
// r24.13 (review C§m7): the bounded read. std::getline(std::istream&,
// std::string&) has already allocated and copied the whole line by the time
// PEOPLE_LINE_MAX can be consulted, so the guard below prevented the FIELD
// WALK and never the allocation — which is what the PEOPLE_LINE_MAX comment
// claimed it did. This reads through one fixed 512-byte buffer and never holds
// more than PEOPLE_LINE_MAX bytes of a line, so a ten-megabyte line is a
// bounded skip and the rows behind it are still read (which is the behaviour
// the safety fixture pins, and the reason this is a bounded READ and not a
// file-size refusal). `len` is the line's true length for the diagnostic;
// `bytes` accumulates everything consumed, for the scan's byte ceiling.
// Returns false only when there is nothing left in the file.
//
// ── r24.13 (review D§9): the byte ceiling is asked INSIDE the line too ──────
// What was wrong: `bytes` was accumulated here and tested only by the caller,
// after the line was complete. A people.tsv that is ONE line larger than
// PEOPLE_SCAN_BYTES — a /dev/urandom redirect, two stores concatenated, a
// partial disk write with no trailing newline — was therefore read in full, at
// startup, before her first turn, because the ceiling could not be consulted
// until a line ended and no line ever did. Measured on this tree before the
// fix: 74.2 ms for a 65 MB single line and 586.9 ms for 512 MB, linear in the
// file and bounded by nothing but its size. That is the "walked to EOF at
// startup" shape C§m7 was written to close, surviving in the one input the
// bound could not see. The allocation was always safe (the fixed buffer, the
// PEOPLE_LINE_MAX hold); this is the READ.
// The line is abandoned as over-long, which it is, and the caller's own
// ceiling test then fires on the next turn of its loop and prints the honest
// "larger than this build will scan" line. Nothing legitimate is near this:
// the largest real store is about 5.5 kB.
static inline bool read_line_bounded(std::istream &f, std::string &line,
                                     bool &over, size_t &len, size_t &bytes) {
    line.clear();
    over = false;
    len  = 0;
    bool any = false;
    char buf[512];
    for (;;) {
        // get(char*, n, delim) sets failbit when it extracts NOTHING, which is
        // an ordinary empty line here and not an error.
        f.clear(f.rdstate() & ~std::ios::failbit);
        if (!f.good()) break;
        f.get(buf, (std::streamsize) sizeof buf, '\n');
        const size_t got = (size_t) f.gcount();
        if (got == 0) break;
        any    = true;
        len   += got;
        bytes += got;
        if (!over && line.size() + got <= PEOPLE_LINE_MAX) line.append(buf, got);
        else { over = true; line.clear(); }
        // r24.13 (review D§9): …and stop AT the ceiling, not after the line
        // that crosses it. See the note above this function.
        if (bytes > PEOPLE_SCAN_BYTES) { over = true; line.clear(); break; }
    }
    f.clear(f.rdstate() & ~std::ios::failbit);
    if (f.peek() == '\n') { f.get(); bytes += 1; any = true; }
    return any;
}

// r24.16: strtol("INFERRED", nullptr, 10) is zero, the HEARD enum value.
// Unknown text therefore crossed the provenance refusal as a sourced claim.
// Validate the entire trimmed field before interpreting its enum; the archive
// stays untouched, and valid numeric HEARD rows keep their prior meaning.
// ATHENA_PEOPLE_PROVENANCE_PARSE=0 restores r24.15's permissive conversion.
static inline bool provenance_parse_on() {
    static const bool on = [] { const char *e = ::getenv("ATHENA_PEOPLE_PROVENANCE_PARSE");
                                return !e || e[0] != '0'; }();
    return on;
}
// r24.18: ingress and rewrite ownership use the same source/name predicate.
// Optional future columns ride outside `said`, through the existing snapshot.
static inline const char *parse_person_row_(const std::string &line, Person &p,
                                            long *bad_source = nullptr) {
    if (line.size() > PEOPLE_LINE_MAX ||
        (amem::memory_opaque_rows_on() && line.find('\0') != std::string::npos)) return "is too long or malformed";
        std::vector<std::string> fld;
        std::string tail;
        if (!aproj::split_tabs(line, 7, fld, tail)) {
            return "is malformed";
        }
        const std::string source = provenance_parse_on() ? amem::trim_copy(fld[0]) : fld[0];
        char *source_end = nullptr;
        const long src = strtol(source.c_str(), &source_end, 10);
        if ((provenance_parse_on() && (source_end == source.c_str() || *source_end != '\0')) ||
            src != (long) Person::HEARD) {
            if (bad_source) *bad_source = src ? src : -1;
            return "has a provenance this build does not know";
        }
        p = Person();
        p.src      = Person::HEARD;
        p.first    = amem::bounded_store_epoch(fld[1].c_str());
        p.last     = amem::bounded_store_epoch(fld[2].c_str());
        p.mentions = (int) std::max(1L, std::min(100000L, strtol(fld[3].c_str(), nullptr, 10)));
        p.asked    = (int) std::max(0L, std::min(100000L, strtol(fld[4].c_str(), nullptr, 10)));
        p.name     = aproj::load_text(fld[5], PEOPLE_NAME_CAP);
        p.relation = aproj::load_text(fld[6], PEOPLE_REL_CAP);
        // The ninth-column rule: the tail is one field, whatever a later build
        // put after it.
        const size_t extra = tail.find('\t');
        if (amem::memory_opaque_rows_on() && extra != std::string::npos) p.opaque_suffix = tail.substr(extra);
        p.said     = aproj::load_text(extra == std::string::npos ? tail : tail.substr(0, extra),
                                      PEOPLE_SAID_CAP);
        if (p.name.empty() || p.last <= 0) {
            return "is malformed";
        }
        if (p.first <= 0) p.first = p.last;
    if (norm_name(p.name).empty()) return "has a name that normalises to nothing";
    return nullptr;
}

static inline std::vector<Person> load_people(const std::string &dir) {
    std::vector<Person> out;
    std::ifstream f(amem::join_path(dir, "people.tsv"));
    if (!f) return out;                                  // absence is normal
    std::string line;
    long   lineno = 0;
    size_t bytes = 0, len = 0;
    int    gripes = 0;
    bool   over = false, cut = false;
    // r24.13 (review C§m7): one rate-limited diagnostic channel for the whole
    // scan. A file of several million rejected lines printed one fprintf per
    // line to the session log, which on a real run is the slower half of the
    // stall — the amem::scrub_gist lesson ("a single 1 MB row at ~75 s of
    // startup stall — per row — before her first turn, with no diagnostic")
    // learned on the other side.
    auto gripe = [&](const char *what, long ln, size_t a, long b) {
        if (++gripes > PEOPLE_SCAN_GRIPES) return;
        if (a)      fprintf(stderr, "memory: people.tsv line %ld %s (%zu bytes) — skipped\n", ln, what, a);
        else if (b) fprintf(stderr, "memory: people.tsv line %ld %s (%ld) — skipped\n", ln, what, b);
        else        fprintf(stderr, "memory: people.tsv line %ld %s — skipped\n", ln, what);
    };
    while (read_line_bounded(f, line, over, len, bytes)) {
        lineno++;
        // The scan's bounds, asked BEFORE the work: the old loop's only
        // stopping rule counted ACCEPTED rows, so a file of nothing but
        // rejected or duplicate lines was walked to EOF at startup.
        if ((size_t) lineno > PEOPLE_SCAN_LINES || bytes > PEOPLE_SCAN_BYTES) { cut = true; break; }
        if (line.empty() && !over) continue;
        if (over || len > PEOPLE_LINE_MAX) {
            gripe("is too long", lineno, len, 0);
            continue;
        }
        Person p; long bad_source = 0;
        const char *error = parse_person_row_(line, p, &bad_source);
        if (error) {
            gripe(error, lineno, 0, bad_source);
            continue;
        }
        // A duplicate key in the file is corruption, not two people. Heal it on
        // the way in — newest wins for what he said, the counters take the
        // larger — so a hand-edited or concatenated file loads as one row per
        // person instead of shadowing the newer with the older. A duplicate
        // LINE is not a fresh mention, so `mentions` is a max here and not the
        // increment merge_person applies to a real one.
        // r24.13 (review C§1): the empty-key guard merge_person has and this
        // did not. With the old norm_name a name of pure punctuation — or, far
        // worse, any wholly non-ASCII name — keyed to "", and two such rows
        // folded into one on load: first name kept, newer row's said/relation
        // pasted onto it. norm_name no longer produces "" for a real name, and
        // this makes the refusal explicit anyway, so the two callers of
        // find_person cannot disagree about what an empty key means.
        const std::string key = norm_name(p.name);
        if (key.empty()) {
            gripe("has a name that normalises to nothing", lineno, 0, 0);
            continue;
        }
        const size_t at = find_person(out, key);
        if (at != out.size()) {
            Person &q = out[at];
            if (p.last > q.last) {
                q.last = p.last;
                if (!p.said.empty())     q.said     = p.said;
                if (!p.relation.empty()) q.relation = p.relation;
            }
            if (p.first > 0 && (q.first <= 0 || p.first < q.first)) q.first = p.first;
            q.mentions = std::max(q.mentions, p.mentions);
            q.asked    = std::max(q.asked,    p.asked);
            continue;
        }
        out.push_back(p);
        if (out.size() >= PEOPLE_MAX * 2) break;      // live + the older build's history
    }
    if (cut)
        fprintf(stderr, "memory: people.tsv is larger than this build will scan "
                        "(%ld lines / %zu bytes read) — the rest is left on disk, untouched\n",
                lineno, bytes);
    if (gripes > PEOPLE_SCAN_GRIPES)
        fprintf(stderr, "memory: people.tsv had %d unreadable lines in all (the first %d are above)\n",
                gripes, PEOPLE_SCAN_GRIPES);
    return out;
}

static inline std::string people_rows_text(const std::vector<Person> &ps) {
    std::ostringstream os;
    size_t n = 0;
    for (const auto &p : ps) {
        if (p.name.empty() || p.last <= 0 || p.src != Person::HEARD) continue;
        if (n++ >= PEOPLE_MAX * 2) break;
        os << p.src << '\t' << p.first << '\t' << p.last << '\t'
           << std::max(1, p.mentions) << '\t' << std::max(0, p.asked) << '\t'
           << aproj::flat_text(aproj::load_text(p.name,     PEOPLE_NAME_CAP)) << '\t'
           << aproj::flat_text(aproj::load_text(p.relation, PEOPLE_REL_CAP))  << '\t'
           << aproj::flat_text(aproj::load_text(p.said,     PEOPLE_SAID_CAP))
           << (amem::memory_opaque_rows_on() ? p.opaque_suffix : std::string()) << '\n';
    }
    return os.str();
}
static inline bool save_people(const std::string &dir, const std::vector<Person> &ps) {
    const auto rendered=people_rows_text(ps);
    const std::string path = amem::join_path(dir, "people.tsv");
    if (!amem::memory_opaque_rows_on()) return aproj::write_atomic(path, rendered);
    return amem::rewrite_preserving_rows_(path, rendered, [](const std::string &line) {
        Person p; return parse_person_row_(line, p) == nullptr;
    });
}

// ── the extraction row type ──────────────────────────────────────────────────
// The prompt half lives in amem::build_extract_prompt (gated on
// amem::people_store_on), because a prompt rule belongs beside the prompt. This
// is the parser for what that rule asks for:
//
//     PERSON | who | how they are related to him | what he said about them
//
// It is a TYPED FIELD WITH A PARSER, not a wording change, which is why it can
// sit beside rules the extractor has followed since R20 without competing with
// them. The residue — the input with those lines removed and every other byte,
// including the exact trailing-newline state, preserved — is what goes on to
// amem::parse_extracted, so the compaction pass sees precisely what r24.12's
// would have seen and its truncated-tail guard reads the same signature.
//
// Refused, quietly: a row with no name, a row with more than the three fields
// the rule asks for (the extractor inventing a column is not something to
// guess at), and a name that carries an Arabic digit — a person is not a
// figure, and this is the one place a model-authored field could put one into
// a store that reaches her frame.
static inline std::string take_person_rows(const std::string &model_out, long now,
                                           std::vector<Person> *out) {
    std::string residue;
    residue.reserve(model_out.size());
    size_t pos = 0;
    while (pos < model_out.size()) {
        const size_t nl = model_out.find('\n', pos);
        const bool   terminated = nl != std::string::npos;
        const std::string line = model_out.substr(pos, terminated ? nl - pos : std::string::npos);
        pos = terminated ? nl + 1 : model_out.size();
        // Is it one of ours? The first bar-field, trimmed, upper-cased, must be
        // exactly PERSON. A bullet or an enumeration in front of it is the most
        // ordinary thing an LLM does unasked (the r19.6/C42 lesson), so both are
        // stripped before the test.
        // r24.13 (review A§2): the enumeration is now actually stripped, and
        // so is the bold wrapper. What was wrong: this loop consumed
        // whitespace, '-' and '*' and nothing else, while the comment above it
        // claimed a bullet AND an enumeration were both taken off "before the
        // test". They were not. `2. PERSON | Sarah | his sister | …` therefore
        // failed the == "PERSON" compare, fell into the residue, and
        // amem::parse_extracted stored it as a memory: its own enumeration
        // rule needs a DIGIT after the ". " and found 'P', so impstr stayed
        // "2. PERSON", the last-digit-run rule scored it 2, and the gist —
        // "Sarah | his sister | she has moved to Berlin", bars and all — passed
        // substance_ok and went into memory.txt as a salience-two junk row,
        // while Sarah never reached people.tsv. `**PERSON** | …` failed
        // identically. The C42 comment in parse_extracted says of exactly this
        // shape: "Enumerated lists are one of the most common things an LLM
        // does unasked, so this is not an exotic input."
        //
        // The loop repeats because the two decorations nest ("- 2. **PERSON**",
        // "2) - PERSON") and one pass over each would leave the other. It is
        // free of risk by construction: `head` is only ever used to ANSWER the
        // PERSON question and to parse a row that answered it yes — a line that
        // is not one of ours goes to the residue as the original `line`, byte
        // for byte, which is what keeps the compaction pass unchanged.
        std::string head = line;
        {
            size_t b = 0;
            for (;;) {
                const size_t was = b;
                while (b < head.size() && (head[b] == ' ' || head[b] == '\t' ||
                                           head[b] == '-' || head[b] == '*')) b++;
                size_t d = b;
                while (d < head.size() && head[d] >= '0' && head[d] <= '9') d++;
                if (d > b && d < head.size() &&
                    (head[d] == '.' || head[d] == ')' || head[d] == ':')) {
                    b = d + 1;
                    while (b < head.size() && (head[b] == ' ' || head[b] == '\t')) b++;
                }
                if (b == was) break;
            }
            head = head.substr(b);
        }
        const size_t bar = head.find('|');
        bool mine = false;
        if (bar != std::string::npos) {
            std::string k = unwrap_emphasis(head.substr(0, bar));
            for (auto &c : k) c = (char) std::toupper((unsigned char) c);
            mine = (k == "PERSON");
        }
        if (!mine) {
            residue += line;
            if (terminated) residue += '\n';
            continue;
        }
        // Consumed either way: a malformed PERSON line is still a PERSON line,
        // and letting it fall through to parse_extracted would turn the row
        // type into junk memories — which is the one thing this must not cost
        // the compaction pass.
        std::vector<std::string> f;
        {
            size_t s = bar + 1;
            while (true) {
                const size_t b2 = head.find('|', s);
                // r24.13 (review A§2): unwrap_emphasis, not trim_copy — a model
                // that writes `**PERSON**` writes `**Sarah**` beside it, and a
                // name with asterisks on it is a different person to norm_name.
                f.push_back(unwrap_emphasis(head.substr(s, b2 == std::string::npos
                                                           ? std::string::npos : b2 - s)));
                if (b2 == std::string::npos) break;
                s = b2 + 1;
                if (f.size() >= 8) break;               // bounded, whatever it wrote
            }
        }
        if (f.size() != 3) continue;
        Person p;
        p.src      = Person::HEARD;
        p.name     = aproj::load_text(aproj::flat_text(f[0]), PEOPLE_NAME_CAP);
        p.relation = aproj::load_text(aproj::flat_text(f[1]), PEOPLE_REL_CAP);
        p.said     = aproj::load_text(aproj::flat_text(f[2]), PEOPLE_SAID_CAP);
        p.first = p.last = now;
        if (p.name.empty() || has_digit(p.name)) continue;
        if (out) out->push_back(p);
    }
    return residue;
}

} // namespace apeo
