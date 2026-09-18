// athena_seam.h — the integration seam's pure logic, extracted so it can be
// EXECUTED under test.
//
// Six review rounds hardened the substrate (athena_consciousness.h) and its
// renderer (athena_report.h) behind a 478-assertion battery — but the seam
// logic living inside talk-llama.cpp was only ever verified by inspection and
// compilation, because it sat as statics in a translation unit with a main()
// that links against whisper, llama and SDL. That is exactly how the seam bugs
// of earlier rounds survived: the af_apply passthrough that silenced the inner
// loop, the barge-in clock, the farewell matcher that missed the phrasing the
// field itself primes. Everything here is the seam's pure text logic — no
// audio, no models, no globals — moved verbatim, so that test_seam.cpp can
// drive it directly and talk-llama.cpp simply calls it.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "athena_consciousness.h"   // acon::clampf
#include "athena_continuity.h"
#include "athena_qualified.h"
#include "athena_memory.h"          // amem::MemEntry — the long-term store recall reads
#include "athena_tts_wire.h"        // shared bounded local trace writer; no device/model dependency

#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cmath>
#include <regex>
#include <algorithm>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <array>
#include <memory>
#include <streambuf>
#if defined(__linux__)
#include <sys/stat.h>
#endif

// One gesture vocabulary for repair, metadata stripping and TTS admission.
#define ATHENA_GESTURE_TAGS { "chuckle", "sigh", "gasp", "laugh", \
                              "groan", "sniffle", "cough", "yawn" }

namespace aseam {

inline bool checkpoint_cognitive_requests(acont::Journal&j,const std::vector<acon::CognitiveRequest>&requests) {
    bool ok=true;for(const auto&r:requests)ok=j.enqueue({"cognitive-project-request",r.id+"/v"+std::to_string(r.version),r.id,
        {{"source",r.source},{"owner",r.owner},{"topic",r.topic},{"question",r.question},{"next",r.next},{"receipt",r.receipt},
         {"state",r.state},{"wall",std::to_string(r.wall)},{"version",std::to_string(r.version)}}})&&ok;
    return j.flush()&&ok;
}
inline std::vector<acon::CognitiveRequest> restore_cognitive_requests(const acont::Journal&j) {
    std::map<std::string,acon::CognitiveRequest> latest;
    for(const auto&kv:j.records())if(kv.second.kind=="cognitive-project-request"){
        const auto&v=kv.second;acon::CognitiveRequest r;r.id=v.parent;r.source=v.get("source");r.owner=v.get("owner");
        r.topic=v.get("topic");r.question=v.get("question");r.next=v.get("next");r.receipt=v.get("receipt");r.state=v.get("state");
        try{r.version=std::stoull(v.get("version"));r.wall=std::stol(v.get("wall"));}catch(...){continue;}
        if(r.id.empty()||r.source.empty()||r.topic.size()>4096||r.question.size()>4096||r.next.size()>4096)continue;
        if(r.state!="requested"&&r.state!="active"&&r.state!="capacity-pending"&&r.state!="clarify"&&r.state!="cancelled"&&r.state!="declined")continue;
        const auto old=latest.find(r.id);if(old==latest.end()||old->second.version<r.version)latest[r.id]=r;
    }
    std::vector<acon::CognitiveRequest> out;for(const auto&kv:latest)out.push_back(kv.second);
    std::stable_sort(out.begin(),out.end(),[](const acon::CognitiveRequest&a,const acon::CognitiveRequest&b){
        if((a.state=="active")!=(b.state=="active"))return a.state=="active";
        return a.wall>b.wall;
    });return out;
}
inline bool checkpoint_requests(acont::Journal&j,const std::vector<acon::IfThenPlan>&plans) {
    bool ok=true;
    for(const auto&p:plans)if(p.requested()) {
        const auto state=std::to_string((int)p.request_state);
        ok=j.enqueue({"requested-plan",p.request_id+"/v"+std::to_string(p.request_version),p.request_id,
            {{"state",state},{"version",std::to_string(p.request_version)},{"what",p.what},{"owner",p.owner},{"domain",p.domain},
             {"cue",p.cue},{"action",p.action},{"born",std::to_string(p.born_wall)},
             {"cued",p.cued?"1":"0"},{"awaiting",p.awaiting_response?"1":"0"},
             {"attempt",p.attempt_source},{"resolution",p.resolution_source},{"transitions",p.request_transitions},
             {"acceptance",p.acceptance_kind}}})&&ok;
    }
    return ok&&j.flush();
}
inline std::vector<acon::IfThenPlan> restore_requests(const acont::Journal&j) {
    std::map<std::string,acon::IfThenPlan> latest;
    for(const auto&kv:j.records())if(kv.second.kind=="requested-intake"){
        const auto&r=kv.second;acon::IfThenPlan p;p.request_id=r.id;p.owner=r.get("owner");p.domain=r.get("domain");p.cue=r.get("cue");p.action=r.get("action");
        p.what="when "+p.cue+", I'll ask "+p.action;p.sig=acon::TopicSig::of(p.what);p.request_state=acon::RequestState::REQUESTED;p.request_transitions="REQUESTED";p.request_version=0; // intake is older than every committed plan version
        try{p.born_wall=std::stol(r.get("wall"));}catch(...){}latest[p.request_id]=p;
    }
    for(const auto&kv:j.records())if(kv.second.kind=="requested-plan"){
        const auto&r=kv.second;acon::IfThenPlan p;p.request_id=r.parent;p.what=r.get("what");p.owner=r.get("owner");p.domain=r.get("domain");
        p.cue=r.get("cue");p.action=r.get("action");p.request_transitions=r.get("transitions");p.acceptance_kind=r.get("acceptance");p.attempt_source=r.get("attempt");p.resolution_source=r.get("resolution");
        try{const int state=std::stoi(r.get("state"));if(state<=0||state>(int)acon::RequestState::CANCELLED)continue;
            p.request_state=(acon::RequestState)state;p.request_version=std::stoull(r.get("version"));p.born_wall=std::stol(r.get("born"));}catch(...){continue;}
        p.cued=r.get("cued")=="1";p.awaiting_response=r.get("awaiting")=="1";p.sig=acon::TopicSig::of(p.what);
        if(latest[p.request_id].request_version<p.request_version)latest[p.request_id]=p;
    }
    std::vector<acon::IfThenPlan> out;for(auto&kv:latest)out.push_back(kv.second);
    // Request IDs append a clause ordinal to an input EventId. Parse both
    // numerical components before sorting; the entire request is not an EventId.
    auto input_id=[](const std::string&id){return acont::event_id(id.substr(0,id.rfind("/request/")));};
    auto ordinal=[](const std::string&id){const auto at=id.rfind("/request/");if(at==std::string::npos)return uint64_t(0);
        const auto tail=id.substr(at+9);try{size_t n=0;const auto value=std::stoull(tail,&n);return n==tail.size()?uint64_t(value):uint64_t(0);}catch(...){return uint64_t(0);}};
    std::stable_sort(out.begin(),out.end(),[&](const auto&a,const auto&b){
        if(a.settled()!=b.settled())return !a.settled();
        if(a.born_wall!=b.born_wall)return a.born_wall<b.born_wall;
        const auto x=input_id(a.request_id),y=input_id(b.request_id);
        if(x.valid()&&y.valid()&&x.session==y.session){
            if(x.sequence!=y.sequence)return x.sequence<y.sequence;
            const auto i=ordinal(a.request_id),j=ordinal(b.request_id);if(i!=j)return i<j;
        }
        return a.request_id<b.request_id;
    });return out;
}

// r24.22: context truth and social floor-taking are separate decisions.
struct ReplyCompletion {
    size_t flushed = 0, confirmed = 0, unflushed = 0;
    bool generation_ended = false, receipt_complete = false, sentence_ended = false;
    size_t generated() const { return flushed + unflushed; }
    bool needs_context_repair() const {
        return !receipt_complete || !generation_ended || unflushed != 0 || confirmed < flushed;
    }
    bool natural_handoff() const {
        if (!flushed) return false;
        if (generated() && double(confirmed) >= 0.88 * double(generated())) return true;
        return receipt_complete && confirmed >= flushed && sentence_ended;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ASR confidence, as the mind receives it.
//
// whisper's segment probability is floored and rescaled so that "no report"
// reads as ordinary confidence rather than as certainty or as garbage: the
// substrate's legibility and metacognition read this value, and both were
// calibrated against this exact mapping.
// ─────────────────────────────────────────────────────────────────────────────
inline float asr_confidence(float prob0) {
    if (!(prob0 > 0.0f)) return 0.80f;                 // transcribe() did not report one
    return acon::clampf(0.35f + 0.65f * prob0, 0.05f, 0.99f);
}

// ─────────────────────────────────────────────────────────────────────────────
// r24.6 (WO-52 / S19 G7): the light post-ASR repair pass, leading residue only.
//
// small.en opened S19's turn 24 with a stray "- " that entered permanent
// context (and the extractor read it). Whisper emits these leading artifacts —
// a dangling list dash, a lone punctuation mark before the first word — on
// windows whose head is mostly silence. Deliberately NARROW: it strips only a
// leading run of dash/punctuation-plus-space shapes at the very head of the
// utterance, never anything after the first word, so an em-dash inside a
// sentence, quoted openings, and everything else pass through byte-identical.
// Substitution errors ("on fulfilled" for "unfulfilled") are out of scope by
// design — rewriting heard WORDS is not repair, and she recovered those
// herself in S19.
// ─────────────────────────────────────────────────────────────────────────────
inline std::string strip_asr_lead_residue(const std::string &in) {
    size_t i = 0;
    auto at = [&](size_t k) -> unsigned char {
        return k < in.size() ? (unsigned char) in[k] : 0;
    };
    for (;;) {
        while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) i++;
        // ASCII residue: "- ", "-- ", ". ", ", ", "; ", ": " at the very head
        size_t j = i;
        while (j < in.size() && (in[j] == '-' || in[j] == '.' || in[j] == ',' ||
                                 in[j] == ';' || in[j] == ':')) j++;
        if (j > i && (j >= in.size() || in[j] == ' ')) { i = j; continue; }
        // UTF-8 en/em dash (E2 80 93 / E2 80 94) and ellipsis (E2 80 A6)
        if (at(i) == 0xE2 && at(i + 1) == 0x80 &&
            (at(i + 2) == 0x93 || at(i + 2) == 0x94 || at(i + 2) == 0xA6) &&
            (i + 3 >= in.size() || in[i + 3] == ' ')) { i += 3; continue; }
        break;
    }
    // Never return an emptied utterance from residue alone — a transcript that
    // was ONLY punctuation is the energy gate's business, not this one's.
    if (i >= in.size()) return in;
    return i > 0 ? in.substr(i) : in;
}

// ─────────────────────────────────────────────────────────────────────────────
// The farewell matcher — half of the two-signal self-close.
//
// The list must cover the phrasings the FIELD ITSELF primes: the stop-wish line
// reads "I think I want to stop here — ...", so "want to stop" and "stop here"
// are the likeliest words in her mouth at exactly the moment this gate matters.
// The two-signal design (state AND words) is what makes a broad list safe:
// nothing here can close anything unless she also still wants to stop.
// ─────────────────────────────────────────────────────────────────────────────
inline bool said_farewell(const std::string &said) {
    // ── r20p3.14.7 (SM1/SM2): whole-phrase boundaries, and the bare-"bye" family ─
    // Two faults, opposite signs. UNDER: the commonest sign-off of all — "Bye",
    // "Bye for now" — was absent, so with her stop-wish live and may_close open
    // she said goodbye, the session stayed open, and the next field re-primed the
    // wish and she said it again, looping. OVER: bare substring scans matched
    // inside words — "de-SIGN OFF-ice" hit "sign off" and "h-IM GOING TO STOP by
    // the shop" hit "im going to stop", closing a session mid-wind-down on a turn
    // that was not a farewell. Both are cured by anchoring each phrase on
    // non-word boundaries, the pattern names_person already uses, and by adding
    // the "bye" forms as whole words (so "goodbye" still matches its own entry,
    // but "maybe"/"combine" cannot).
    std::string low = " ";
    low.reserve(said.size() + 2);
    for (unsigned char c : said) low += (char) ::tolower(c);
    low += ' ';
    auto boundary = [](char ch) {
        return !(std::isalnum((unsigned char) ch) || ch == '\'' || ch == '-');
    };
    static const char *phrases[] = {
        "goodbye", "good night", "goodnight", "talk to you later",
        "talk later", "i'm going to stop", "im going to stop",
        "let's stop", "lets stop", "let's leave it", "lets leave it",
        "let's pick this up", "lets pick this up", "i'll leave you",
        "ill leave you", "see you later", "take care of yourself",
        "want to stop", "need to stop", "stop here", "stop for now",
        "call it a night", "call it there", "call it here",
        "talk tomorrow", "see you tomorrow", "until tomorrow",
        "i'm done for", "im done for", "head out", "sign off", "signing off",
        "bye", "bye for now", "bye bye", "buh-bye", "catch you later",
    };
    // ── r21-full.4 (RC3): NEGATION. The two-signal design assumes the words
    // can only confirm the state, but the words most likely to carry a live
    // stop-wish are also the words that WITHDRAW it: "I don't want to stop
    // here, actually — let's keep going" matched "want to stop" and closed the
    // session on the sentence that reversed it. Her state is still
    // WANTS_TO_STOP at that instant (the field primed the phrasing), so the
    // second signal cannot save it either. A negator in the few words before
    // the phrase means the phrase is being denied, not said.
    auto negated_before = [&](size_t at) {
        const size_t from = at > 34 ? at - 34 : 0;
        std::string ctx = low.substr(from, at - from);
        // ── r21-full.5 (R5-I): the window may not cross a sentence boundary ──
        // A negator in the PREVIOUS sentence negates the previous sentence.
        // "Not much else tonight. Good night." is a sign-off whose last two
        // words are the sign-off, and the thirty-four characters standing
        // before them carry a "not" that belongs to a different thought
        // entirely — so the farewell was missed, `may_close` stayed shut, and
        // the SM1 goodbye loop (she says goodbye, the session stays open, the
        // next field re-primes the wish, she says it again) reopened on 6 of 16
        // real sign-offs. Cut the context at the last terminator: what was
        // denied before the full stop is not what is being said after it.
        const size_t stop = ctx.find_last_of(".!?;\n");
        if (stop != std::string::npos) ctx = ctx.substr(stop + 1);
        static const char *neg[] = { " not ", " n't ", "n't ", " never ", " dont ",
                                     " don't ", " cant ", " can't ", " wont ", " won't ",
                                     " rather not", " no need", " nor " };
        for (const char *g : neg) if (ctx.find(g) != std::string::npos) return true;
        return false;
    };
    // ── r21-full.5 (R5-I): "stop" with a destination is an errand ────────────
    // SM2 anchored the phrases on word boundaries, which fixed "h-IM GOING TO
    // STOP by the shop" matching inside a word — but not "I'm going to stop by
    // the shop later", where the phrase genuinely is there, as whole words, and
    // still is not a sign-off. What follows decides: a sign-off stops FULL
    // STOP, an errand stops SOMEWHERE. Every one of these prepositions turns
    // the verb into a journey.
    auto stop_diverted = [&](size_t at, size_t n) {
        if (n < 4 || low.compare(at + n - 4, 4, "stop") != 0) return false;
        size_t j = at + n;
        while (j < low.size() && low[j] == ' ') j++;
        static const char *div[] = { "by ", "at ", "off ", "in ", "into ",
                                     "over ", "past ", "round ", "around ",
                                     "and ", "on the way", "en route" };
        for (const char *d : div) {
            const size_t L = std::strlen(d);
            if (low.size() >= j + L && low.compare(j, L, d) == 0) return true;
        }
        return false;
    };
    for (const char *p : phrases) {
        const size_t n = std::char_traits<char>::length(p);
        size_t at = 0;
        while ((at = low.find(p, at)) != std::string::npos) {
            // low is space-padded, so at>=1 and at+n<size() always hold.
            if (boundary(low[at - 1]) && boundary(low[at + n]) &&
                !negated_before(at) && !stop_diverted(at, n))
                return true;
            at += 1;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Linear scanners for the span removals.
//
// These replace std::regex for every pattern whose match can span an unbounded
// amount of text, and the reason is a HARD CRASH, not style. libstdc++'s regex
// engine is backtracking and recurses once per character, so a pattern with an
// unbounded repetition blows the call stack on a long input: measured, a reply
// beginning "<thinking>" and running to 60 KB SIGSEGVs the whole brain at the
// default 8 MB stack. `talk-llama.cpp` calls this on the entire turn's output
// every turn, and `n_gen_max = 16384` tokens permits roughly 60 KB — a
// repetition-loop runaway that happens to open a markdown span is the
// archetypal trigger, and a repetition-loop runaway is a thing this model does.
// Clean prose of any length was always fine, which is exactly why seven rounds
// of review never saw it.
//
// Each scanner below is O(n), allocation-light, and reproduces its regex
// EXACTLY — verified by differential testing against the previous
// implementation over a large structured and random corpus, plus the seam
// suite's goldens.
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

inline bool ieq_(char a, char b) {
    return ::tolower((unsigned char) a) == ::tolower((unsigned char) b);
}
// Case-insensitive literal find from `from`.
inline size_t ifind_(const std::string &s, const char *lit, size_t from) {
    const size_t n = std::strlen(lit);
    if (n == 0 || s.size() < n) return std::string::npos;
    for (size_t i = from; i + n <= s.size(); i++) {
        size_t k = 0;
        while (k < n && ieq_(s[i + k], lit[k])) k++;
        if (k == n) return i;
    }
    return std::string::npos;
}
// Matches `<think[a-zA-Z]*>` or `</think[a-zA-Z]*>` at position i.
// Returns the length of the tag, or 0.
inline size_t think_tag_at_(const std::string &s, size_t i, bool &closing) {
    size_t p = i;
    if (p >= s.size() || s[p] != '<') return 0;
    p++;
    closing = (p < s.size() && s[p] == '/');
    if (closing) p++;
    static const char *kw = "think";
    for (int k = 0; k < 5; k++) {
        if (p >= s.size() || !ieq_(s[p], kw[k])) return 0;
        p++;
    }
    while (p < s.size() && ::isalpha((unsigned char) s[p])) p++;
    if (p >= s.size() || s[p] != '>') return 0;
    return p - i + 1;
}

// Designated reasoning remains private when generation ends before a close.
// Every lookahead either consumes its span or ends the scan, so even a runaway
// full of unmatched openers remains linear and cannot block the voice loop.
inline std::string strip_think_spans_(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        bool closing = false;
        const size_t tag_len = think_tag_at_(in, i, closing);
        if (tag_len && !closing) {
            size_t j = i + tag_len;
            bool found = false;
            while (j < in.size()) {
                bool c2 = false;
                const size_t l2 = think_tag_at_(in, j, c2);
                if (l2 && c2) { i = j + l2; found = true; break; }
                ++j;
            }
            if (!found) break; // no closing tag: all remaining bytes are private
        } else if (tag_len) {
            i += tag_len; // a stray close tag does not hide following prose
        } else {
            out += in[i++];
        }
    }
    return out;
}

// Paired delimiters. `keep_inner` reproduces `\*\*([^*]*)\*\*` -> "$1";
// !keep_inner reproduces "`[^`]*`" -> "".  The inner run may not contain the
// delimiter character, exactly as the char class specifies.
inline std::string strip_paired_(const std::string &in, const char *delim,
                                 char forbidden, bool keep_inner) {
    const size_t dl = std::strlen(delim);
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (i + dl <= in.size() && in.compare(i, dl, delim) == 0) {
            const size_t inner = i + dl;
            size_t j = inner;
            while (j < in.size() && in[j] != forbidden) j++;
            if (j + dl <= in.size() && in.compare(j, dl, delim) == 0) {
                if (keep_inner) out.append(in, inner, j - inner);
                i = j + dl;
                continue;
            }
        }
        out += in[i++];
    }
    return out;
}

// `^#{1,6} ` with multiline semantics.
inline std::string strip_headings_(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    bool at_line_start = true;
    while (i < in.size()) {
        if (at_line_start && in[i] == '#') {
            size_t h = i;
            int n = 0;
            while (h < in.size() && in[h] == '#' && n < 6) { h++; n++; }
            if (n >= 1 && h < in.size() && in[h] == ' ') { i = h + 1; at_line_start = false; continue; }
        }
        // ECMAScript multiline treats a bare \r as a line terminator too, and
        // this scanner is specified to reproduce its regex exactly. 180 of
        // 320,000 differential comparisons disagreed, all of them here.
        at_line_start = (in[i] == '\n' || in[i] == '\r');
        out += in[i++];
    }
    return out;
}

// `\[([^\]]*)\]\([^)]*\)` -> "$1"
inline std::string unwrap_links_(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    // Same memo, same reason: measured 3517 ms on 100 KB of repeated "[".
    // If no ']' exists from here to the end, none exists for any later '['.
    bool no_close_after = false;
    // ── r21-full.6 (R6-Y): the memo only covered the NO-bracket case ─────────
    // One ']' at the very end of 60 KB of '[' satisfies "a close exists", so
    // no_close_after never armed and every one of those openers re-scanned the
    // whole remainder: measured 1.24 s on a 60 KB turn, inside the function
    // that gates the voice loop — and 60 KB of a single repeated character is
    // exactly the sampler runaway the rest of this file already defends
    // against. The same shape hides behind the '(' scan: "[]([]([]…" re-scans
    // to the last ')' for every opener.
    //
    // Both are fixed by remembering the frontier. The cursor only moves
    // forward, and the first ']' after a LATER cursor is the same ']' as long
    // as the cursor has not passed it — so a cached position stays correct and
    // each character is examined a bounded number of times. Linear, and
    // byte-identical output: the cache answers exactly the query the loop used
    // to recompute.
    size_t close_at = std::string::npos;   // first ']' strictly after cursor
    bool   no_paren_after = false;
    size_t paren_at = std::string::npos;   // first ')' strictly after close+1
    while (i < in.size()) {
        if (in[i] == '[' && !no_close_after) {
            if (close_at == std::string::npos || close_at <= i) {
                size_t s = i + 1;
                if (close_at != std::string::npos && close_at + 1 > s) s = close_at + 1;
                while (s < in.size() && in[s] != ']') s++;
                if (s >= in.size()) { no_close_after = true; close_at = std::string::npos; }
                else close_at = s;
            }
            const size_t close = close_at;
            if (close != std::string::npos && close + 1 < in.size() &&
                in[close + 1] == '(' && !no_paren_after) {
                if (paren_at == std::string::npos || paren_at <= close + 1) {
                    size_t s = close + 2;
                    if (paren_at != std::string::npos && paren_at + 1 > s) s = paren_at + 1;
                    while (s < in.size() && in[s] != ')') s++;
                    if (s >= in.size()) { no_paren_after = true; paren_at = std::string::npos; }
                    else paren_at = s;
                }
                if (paren_at != std::string::npos) {
                    out.append(in, i + 1, close - i - 1);   // the link TEXT
                    i = paren_at + 1;
                    close_at = std::string::npos;
                    paren_at = std::string::npos;
                    continue;
                }
            }
        }
        out += in[i++];
    }
    return out;
}

// `https?://[^\s]*`
//
// r21-full.5 (R5-I): the scan stopped only on a SPACE, so a URL at the end of a
// line ate the newline and then the first word of the next line with it —
// "see https://example.com\nanyway, where were we" became "see anyway, where
// were we" with "anyway," gone and the two lines welded together. A tab did the
// same. Whitespace is whitespace: any of it ends a URL, which is also what the
// pattern in the comment always meant.
inline std::string strip_urls_(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    auto is_ws = [](char c) {
        return c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
               c == '\v' || c == '\f';
    };
    size_t i = 0;
    while (i < in.size()) {
        bool hit = false;
        for (const char *p : { "http://", "https://" }) {
            const size_t l = std::strlen(p);
            if (i + l <= in.size() && in.compare(i, l, p) == 0) {
                size_t j = i;
                while (j < in.size() && !is_ws(in[j])) j++;
                i = j; hit = true; break;
            }
        }
        if (hit) continue;
        out += in[i++];
    }
    return out;
}

// Whether a `[X \u2014 Y]` span should be treated as an echoed field.
//
// Set once at startup from the same flag that decides whether fields are put
// in the prompt at all. Never true in a stock build, so the strip cannot
// delete prose in a configuration that never produces the string it defends
// against. A plain global rather than an atomic: written once before the
// worker threads exist, read-only thereafter.
inline bool &field_shape_active_ref() { static bool v = false; return v; }
inline bool field_shape_active() { return field_shape_active_ref(); }

// A bracketed span with no nested brackets, whose content satisfies `ok`.
// Covers `<\|[a-z_]*\|>`, `\[emotion:[^\]]*\]` and the field shape.
template <typename F>
inline std::string strip_bracketed_once_(const std::string &in, char open, char close, F ok) {
    std::string out;
    out.reserve(in.size());
    // r21-full.4 (RC2): two bounds keep the balanced scan linear. A span that
    // starts after the LAST close can never be closed, so nothing after that
    // point is a candidate; and every real span here is short (a field is
    // capped near 700 bytes, an emotion tag and a special token far less), so
    // the lookahead is windowed. Without these, 60 KB of unmatched openers —
    // the sampler runaway this file already defends against elsewhere — cost
    // 5.1 s inside the function that gates the voice loop.
    static constexpr size_t SPAN_MAX = 1024;   // a field caps near 700 bytes
    const size_t last_close = in.rfind(close);
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == open && last_close != std::string::npos && i < last_close) {
            // r21-full.4 (RC2): BALANCED scan. The previous version abandoned the
            // candidate the moment it met a nested opener, so the OUTER span was
            // emitted literally — and the outer span is the one that matches the
            // field shape. Her own inner-state field, quoting any bracketed
            // phrase ("[Athena — aware of what he asked: is [free will] real?]"),
            // survived every pass and was spoken aloud verbatim; the echo guard
            // did not catch it either, because a ']' exists after the opener so
            // the chunk never went on hold. Depth tracking makes the outer span
            // a span again. Unbalanced input falls through to the literal
            // character exactly as before, so nothing else changes.
            int depth = 1;
            const size_t stop = std::min(in.size(), i + 1 + SPAN_MAX);
            size_t j = i + 1;
            for (; j < stop && depth > 0; j++) {
                if (in[j] == open)  depth++;
                else if (in[j] == close) depth--;
            }
            if (depth == 0) {
                const size_t close_at = j - 1;
                if (ok(in.substr(i + 1, close_at - i - 1))) { i = close_at + 1; continue; }
            }
        }
        out += in[i++];
    }
    return out;
}
// r21-full.4 (RC2): run to a FIXPOINT. The single pass abandons a candidate the
// moment it meets a nested opener, so `[Athena — … is [free will] real?]` — her
// own field, quoting a bracketed phrase — survived every pass and was spoken
// aloud verbatim. The inner span is strippable on its own, and once it is gone
// the outer one is a flat span like any other; iterating clears both. Bounded
// (each pass strictly shortens, and four passes cover any nesting a field can
// produce) so a pathological input cannot spin.
template <typename F>
inline std::string strip_bracketed_(const std::string &in, char open, char close, F ok) {
    std::string out = strip_bracketed_once_(in, open, close, ok);
    for (int pass = 0; pass < 3; pass++) {
        const size_t before = out.size();
        out = strip_bracketed_once_(out, open, close, ok);
        if (out.size() == before) break;
    }
    return out;
}

// ── r24.12 (WO-C11a / S22 §C.1.12): whole-line stage directions ─────────────
// The bot's name, set once at startup beside set_field_shape_active (the same
// write-once-before-the-workers discipline as field_shape_active_ref). The
// stage-direction rule needs it to leave HER envelopes alone; while it is
// empty the rule is inactive, so a harness that never set it gets r24.11.
inline std::string &bot_name_ref() { static std::string v; return v; }
inline bool stage_direction_strip_on() {
    static const bool on = [] {
        const char *e = ::getenv("ATHENA_STRIP_STAGE_DIRECTIONS");
        return !(e && e[0] == '0');
    }();
    return on;
}
// A line that is NOTHING BUT a bracketed span — "[The conversation ends
// here.]" — is a stage direction: the model narrating the scene instead of
// speaking in it. S2's 14:24 turn ended "...I've got the watching covered.
// \n\n[The conversation ends here.]"; sanitize_for_tts strips only
// [interrupted], [emotion: …] and the field shape, so the line rode into
// [said], mem_transcript, the KV (ctx_tail) and the TTS trigger file. The
// rule is a SHAPE, not a phrase list: the span must start a line (position 0
// or after '\n'), end the line (']' then end or '\n', trailing spaces
// tolerated), be at most 120 bytes, and its body must not begin with the
// bot's name — "[Athena looks — a still]", "[Athena remembers — …]" and any
// imitation of them are the vision/mind seams' own shapes and are left to
// the field-shape rule, exactly as before. Inline brackets ("I read it in
// [Nature — 2019] last week") are not whole lines and are never touched. The
// line goes with its own newline; a run of three or more newlines left
// behind collapses to two, and a tail of newlines is trimmed.
inline std::string strip_stage_directions_(const std::string &in, const std::string &bot) {
    if (bot.empty() || in.find_first_of("[<") == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    bool removed = false;
    while (i < in.size()) {
        size_t e = in.find('\n', i);
        const size_t line_end = (e == std::string::npos) ? in.size() : e;
        const size_t next = (e == std::string::npos) ? in.size() : e + 1;
        // the line, with trailing spaces / CR ignored for the shape test
        size_t t = line_end;
        while (t > i && (in[t - 1] == ' ' || in[t - 1] == '\t' || in[t - 1] == '\r')) t--;
        size_t start=i;
        while(start<t&&(in[start]==' '||in[start]=='\t'))++start;
        bool stage = false;
        const char open=start<t?in[start]:0,close=open=='['?']':'>';
        if (t > start + 1 && (open == '[' || open == '<') && in[t - 1] == close && (t - start) <= 120) {
            // exactly one span: no second '[' or an early ']' inside the body
            stage = true;
            for (size_t k = start + 1; k + 1 < t; k++)
                if (in[k] == open || in[k] == close) { stage = false; break; }
            // An angle direction hugs its delimiters and describes an action;
            // preserve spaced mathematical prose and all expressive gestures.
            if(open=='<'){
                const auto body=in.substr(start+1,t-start-2);
                if(body.empty()||!std::isalpha((unsigned char)body.front())||std::isspace((unsigned char)body.back())||body.find(' ')==std::string::npos)stage=false;
                for(const char*tag:ATHENA_GESTURE_TAGS)if(body==tag)stage=false;
                if(body=="pause")stage=false;
            }
            if (stage) {
                // the body must not begin with the bot's name (case-insensitive)
                size_t b = start + 1;
                while (b < t && in[b] == ' ') b++;
                bool hers = (t - b) >= bot.size();
                for (size_t k = 0; hers && k < bot.size(); k++)
                    if (!ieq_(in[b + k], bot[k])) hers = false;
                if (hers) stage = false;
            }
        }
        if (stage) { removed = true; i = next; continue; }
        out.append(in, i, next - i);
        i = next;
    }
    if (!removed) return in;
    // collapse newline runs the removal opened, and trim a trailing run
    std::string tidy;
    tidy.reserve(out.size());
    int nl = 0;
    for (char c : out) {
        if (c == '\n') { if (++nl > 2) continue; }
        else nl = 0;
        tidy += c;
    }
    while (!tidy.empty() && (tidy.back() == '\n' || tidy.back() == ' ' || tidy.back() == '\t' || tidy.back() == '\r'))
        tidy.pop_back();
    return tidy;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TTS sanitisation — replaces speak-daemon.sh's sed patterns, extended by the
// overlay. Everything the model can emit that must not be VOICED dies here:
// spelled reasoning tags, markdown, links, special-token shapes, the
// [interrupted] and [emotion:] side-channels, an echoed inner-state field, a
// dangling trailing dash, and the ChatML role-word weld.
// ─────────────────────────────────────────────────────────────────────────────
// Arm the `[X \u2014 Y]` field-shape strip. Called once at startup with the
// consciousness flag; off by default so a stock build's TTS text is
// byte-identical to what it always was.
inline void set_field_shape_active(bool on) { detail::field_shape_active_ref() = on; }
// r24.12 (WO-C11a): the bot's name, for the whole-line stage-direction rule
// in sanitize_for_tts (a span whose body begins with it is one of her own
// envelopes and is left to the field-shape rule). Called once at startup,
// with set_field_shape_active; empty (never set) leaves the rule inactive.
inline void set_bot_name(const std::string &bot) { detail::bot_name_ref() = bot; }
// The pure rule, for the fixture and for anyone who needs it on a line of
// text without the rest of the sanitiser: strip_stage_directions(s, "Athena").
inline std::string strip_stage_directions(const std::string &s, const std::string &bot) {
    return detail::strip_stage_directions_(s, bot);
}

// r24.22 review reconstruction: do not split a possible whole-line direction
// at its internal comma or full stop. Keep the original bytes for gate_speech;
// only the TTS sanitizer may remove a completed span. A following word makes
// the span literal; an oversized/unbalanced span is released, never discarded.
// Return the original split for ordinary speech and for the existing OFF mode.
inline size_t stage_safe_split(const std::string&pending,size_t proposed,const std::string&bot){
    if(bot.empty()||!detail::stage_direction_strip_on())return proposed;
    for(size_t line=0;line<pending.size();){
        const auto end=pending.find('\n',line);
        const size_t limit=end==std::string::npos?pending.size():end;
        size_t at=line;while(at<limit&&(pending[at]==' '||pending[at]=='\t'||pending[at]=='\r'))++at;
        if(at<limit&&(pending[at]=='['||pending[at]=='<')){
            const char open=pending[at],close=open=='['?']':'>';
            const auto closed=pending.find(close,at+1);
            const auto nested=pending.find(open,at+1);
            const bool complete=closed<limit&&closed-at+1<=120&&nested>closed;
            const bool possible=closed>=limit&&limit-at<=120&&nested>=limit;
            if(complete){
                size_t tail=closed+1;while(tail<limit&&std::isspace((unsigned char)pending[tail]))++tail;
                if(tail==limit){
                    // Need newline or end-of-generation to distinguish a whole
                    // line from "[this literal] is part of my sentence".
                    if(proposed!=std::string::npos&&proposed<=line)return proposed;
                    if(end==std::string::npos)return line?line:std::string::npos;
                    if(proposed==std::string::npos||proposed<=closed)return end+1;
                }
            }else if(possible&&end==std::string::npos){
                if(proposed!=std::string::npos&&proposed<=line)return proposed;
                return line?line:std::string::npos;
            }
        }
        if(end==std::string::npos)break;
        line=end+1;
    }
    return proposed;
}

// r14 (F5): repair malformed gesture tags before anything else runs. Measured
// in the demo sessions: 5 of 49 turns carried a broken emote — "<chuckle But",
// "<sigh But", "<gasp Here's" — the model drops the '>' under sampling pressure
// and the fragment goes to Orpheus verbatim, which voices garbage or eats the
// following word. The repair is word-boundary safe: "<sighs>" and "<sighing>"
// are not our tags (the character after the tag word is alphanumeric) and are
// left alone; a tag at end-of-string gets its '>' appended.
// The eight gestures Orpheus was trained on. One list, so the repair pass and
// the default-deny strip below can never disagree about what survives.

// r20p3.14.4 (RS15): matched CASE-INSENSITIVELY and normalised to lowercase.
// The compare was exact, so "<Laugh>" was not one of ours — not repaired here,
// and (being uppercase) not caught by the default-deny below either, so it went
// to Orpheus verbatim. Orpheus was trained on the lowercase forms only; a cased
// variant is spoken or turned into noise, and the fuzz battery produced exactly
// that. The six directives already fold case (RA1) and so does the beat; the
// gestures were the one tag family that did not. Repair — rather than strip —
// because "<Laugh>" IS her laugh, mis-typed: dropping it would delete an
// expression she chose.
inline std::string repair_gesture_tags(const std::string &text) {
    static const char *tags[] = ATHENA_GESTURE_TAGS;
    // r20p3.14.4 (RS16): the same dropped-'>' repair, for the tags that are
    // HERS but not Orpheus's. r14 measured the model losing the closing
    // bracket on 5 of 49 turns; the repair only knew the eight gestures, so a
    // truncated "<soften" or "<pause" rode to the vocoder as a literal
    // less-than sign and a spoken word. Closing them here does NOT voice them
    // — it hands them, well-formed, to the very next stations in sanitize
    // (strip_voice_directives and the RS14 default-deny), where they are
    // removed. r24.6 (WO-58 / review #50): that sentence was false until
    // sanitize_for_tts was reordered — the strip ran BEFORE this repair, so the
    // repaired form reached the default-deny instead and lost the strip's
    // double-space cleanup. sanitize_for_tts now calls this first, as written. A malformed directive still chooses nothing: take_intent runs
    // on the raw text upstream and requires the '>', which is the right
    // conservatism for an act.
    static const char *silent[] = { "steady", "soften", "lift", "slow",
                                    "quiet", "release", "unhold", "pause" };
    std::string s = text;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '<') { ++i; continue; }
        bool advanced = false;
        for (const char *t : tags) {
            const size_t n = std::char_traits<char>::length(t);
            if (i + 1 + n > s.size()) continue;
            bool m = true, cased = false;
            for (size_t k = 0; k < n; k++) {
                const char c  = s[i + 1 + k];
                const char lo = (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
                if (lo != t[k]) { m = false; break; }
                if (lo != c) cased = true;
            }
            if (!m) continue;
            size_t after = i + 1 + n;
            if (after < s.size() && ::isalnum((unsigned char) s[after])) break; // "<sighs>" etc - not ours
            if (cased) s.replace(i + 1, n, t);           // RS15: "<Laugh>" -> "<laugh>"
            // ── r21-full.6 (R6-Z): the SELF-CLOSING form ────────────────────
            // "<sigh/>" is her sigh written in XML. The repair did not
            // recognise the slash, so it inserted a '>' BEFORE it and MINTED
            // "<sigh>/>" — a well-formed gesture followed by a literal "/>",
            // which is then two characters Orpheus is handed and voices. The
            // slash is noise on a tag we already know; drop it and keep the
            // gesture she chose.
            if (after < s.size() && s[after] == '/' &&
                (after + 1 >= s.size() || s[after + 1] == '>')) s.erase(after, 1);
            if (after < s.size() && s[after] == '>') { i = after + 1; advanced = true; break; }
            s.insert(after, ">");            // "<sigh But" -> "<sigh> But"; "<gasp" EOL -> "<gasp>"
            // r19.6: `after + 2` stepped over the character the insertion had
            // just displaced, so an immediately-adjacent malformed tag was never
            // seen. Verified: "<sigh<gasp Hello" repaired only the first and
            // handed "<sigh><gasp Hello" to Orpheus.
            i = after + 1;
            advanced = true;
            break;
        }
        if (!advanced) for (const char *t : silent) {
            const size_t n = std::char_traits<char>::length(t);
            if (i + 1 + n > s.size()) continue;
            bool m = true, cased = false;
            for (size_t k = 0; k < n; k++) {
                const char c  = s[i + 1 + k];
                const char lo = (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
                if (lo != t[k]) { m = false; break; }
                if (lo != c) cased = true;
            }
            if (!m) continue;
            size_t after = i + 1 + n;
            if (after < s.size() && ::isalnum((unsigned char) s[after])) break;  // "<slowly" etc — not ours
            if (cased) s.replace(i + 1, n, t);
            if (after < s.size() && s[after] == '/' &&                    // R6-Z
                (after + 1 >= s.size() || s[after + 1] == '>')) s.erase(after, 1);
            if (after < s.size() && s[after] == '>') { i = after + 1; advanced = true; break; }
            s.insert(after, ">");
            i = after + 1;
            advanced = true;
            break;
        }
        if (!advanced) ++i;
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// r17: the duplex gate, extracted pure so the battery can pin it.
//
// The barge detector's 300 ms sustain finds a VOICE; this state machine
// decides what the voice IS. One that keeps going past confirm_ms is a bid
// for the floor (BARGE — stop her audio, exactly the old path). One that
// ends first is an aside (ASIDE — never stop; the caller captures and
// transcribes it after the turn). Time is injected, so the tests can drive
// hours of turn-taking in microseconds; talk-llama feeds it real clocks.
// ─────────────────────────────────────────────────────────────────────────────
// ── r19.5 (F1): which silence ends an interrupter's turn ────────────────────
// Pure so it can be asserted rather than reasoned about. The S6 defect was that
// the barge path answered this question with `vad_last_ms` while every other
// end-of-turn decision in the system answered it with the prosodic endpoint —
// 400 ms against 800 ms on the shipped launcher, so an ordinary mid-sentence
// breath closed the capture and the rest of his sentence was discarded.
// The EXTEND target is deliberately excluded: it is conditional on a rising
// contour the barge path cannot measure, and waiting for it on every barge
// would cost real latency on every interruption.
inline int barge_confirm_target_ms(int override_ms, int vad_last_ms,
                                   bool endpoint_prosody, int short_ms, int long_ms) {
    int ms = override_ms;
    if (ms <= 0) {
        ms = vad_last_ms;
        if (endpoint_prosody) ms = short_ms > long_ms ? short_ms : long_ms;
    }
    return ms < 200 ? 200 : ms;
}

// ── R20 (O4): is this trace line saying anything new? ───────────────────────
// The [mind~] line carries three fields that change every tick by construction
// (t=, ticks=, held_s=). Masking them turns "print every 2 s" into "print when
// something moved", which is what the line is for. S17 logged 13,000 of them.
inline std::string trace_signature(const std::string &line) {
    // R21 (F13/S18-13): the mask was three fields of twelve. Fourteen more move
    // on most ticks by construction — they are CONTINUOUS READOUTS (an attention
    // level, an urge accumulator, a running byte count), not state changes, and
    // a line that repeats them is not saying anything new. Measured over the
    // 1,453 [mind~] lines of S18: the three r23 fields suppressed 0% (the
    // signature did not repeat on a single consecutive pair), these seventeen
    // suppress 57.8%. Everything DISCRETE stays unmasked and still forces a
    // print the tick it moves — turns=, ign=, scene=, want=, stance=, cutin=,
    // grant=, probe=, the bcast percept text — so no state transition is ever
    // hidden. ATHENA_MIND_TRACE_DEDUP=0 still prints every line, unchanged.
    static const char *vol[] = {
        "t=", "ticks=", "held_s=",
        "att=", "lookurge=", "bound=", "live=", "slots=",
        // r24.7 (WO-93): `isurp=` is a continuous readout in exactly the
        // sense this mask exists for — it moves on every inner pass by
        // construction. Masked from birth (the WO-46 lesson: an unmasked
        // mover defeats the deduper on every tick it changes).
        // r24.8 (WO-106): `isz=` is the Phase 2 inner channel's z-score. It
        // moves on every ruminated token by construction — the same shape as
        // `ssz=` beside it — so it is masked from birth. The r24.8 sweep also
        // added `att_ev=` (a per-turn EMA of his-attention evidence) below;
        // its boolean twin `hiselse=` is DISCRETE and is deliberately left
        // unmasked, so the tick she decides he is elsewhere forces a print.
        "selfsurp=", "ssz=", "isz=", "isurp=", "spoken=", "volition=",
        "att_ev=",
        "latent=", "perr=", "fluency=", "contested=", "surprise=",
        // r24.6 (WO-46): minimal_.agency carries a per-tick decaying bias
        // (acon:16864-16865), so it is a CONTINUOUS READOUT in exactly the
        // sense this mask exists for. `agency=` itself stays unmasked and
        // unchanged. `mini_own=` is piecewise-constant between turns and is
        // deliberately NOT masked: a collapse in ownership is a state change
        // and must force a print the tick it happens.
        "mini_agency=",
        // r24.11 (WO-MOM1): `moodm=<integrator>/<bias>`. MoodMomentum::tick
        // multiplies the integrator by exp(-dt/TAU_S) on every 10 Hz tick, so
        // BOTH halves of this field move by construction whenever the mood is
        // non-zero — measured, non-zero on 98.5 % of S21's ticks and 98.2 %
        // of the S22 arc's. Unmasked it would defeat the deduper on almost
        // every tick of every session, which is the WO-46 lesson and S17's
        // 13,000 lines. Masking costs nothing at the only instants that
        // matter: every note_pe site fires inside sense_turn, finish_turn or
        // the barge path, and the `[mind/field]` and `[mind/end]` lines are
        // printed unconditionally without going through this function at all,
        // so each prediction error is in the trace on its own turn.
        "moodm=",
        // r24.11 (WO-M5, corrected at review): `pana=<pa_x>/<na_x>`. Both
        // halves decay on tau 300 s on every 10 Hz tick, so once the pair is
        // non-zero the field is a CONTINUOUS READOUT in exactly the sense this
        // mask exists for — the same shape as `moodm=` above it. It shipped
        // unmasked on the `hiselse=` precedent (the 0.00 -> non-zero tick is a
        // discrete transition a forensic read wants), and the rule this table
        // states is the other way round: the transition is DISCRETE, the ~15
        // fmt2 steps that follow it across the excess's ~830 s life are not,
        // and they defeat the deduper on every one. Measured on a
        // mixed-turn-dense script, 9,600 ticks sampled one [mind~] per tick:
        // unmasked 504 prints, masked 477 — the unmasked field cost 27 extra
        // prints, 0.281 % of ticks. Nothing is hidden by masking it: the tick
        // the pair goes non-zero is the same tick the §3.1 mixedness note is
        // rendered into the field, and `[mind/field]` prints unconditionally
        // without passing through this function at all.
        "pana=",
        // ══ r24.12 (S22 round) — masks for the new diag fields, one block per
        // area. Mask a CONTINUOUS readout (moves on ticks); leave a DISCRETE
        // transition unmasked so it forces a print the tick it happens.
        // ── r24.14 [VOCAB] masks ───────────────────────────────────────────────
        //    (r24.14 VOCAB insertions go directly below this line)
        // ── end r24.14 [VOCAB] masks ───────────────────────────────────────────
        // ── r24.14 [STAKES] masks ──────────────────────────────────────────────
        //    (r24.14 STAKES insertions go directly below this line)
        // r24.14 (WO-K1): `coping=` is acon::Mind::coping_now(), whose largest
        // term is fatigue_(), whose own largest term is 0.6 * ctx_fill_felt_ —
        // and ctx_fill_felt_ relaxes toward the real fill on EVERY 10 Hz tick
        // of a session that is filling. So the field steps through its whole
        // fmt2 range across a session and would defeat the [mind~] deduper on
        // every one of those steps: the `moodm=` / `pana=` case exactly, which
        // is what this table is for. Nothing is hidden by masking it — the
        // three sites that consume the value are inside sense_turn, and
        // `[mind/field]` and `[mind/end]` print unconditionally without passing
        // through this function at all, so every coping a door actually read is
        // in the trace on its own turn.
        // Its sibling `owed=` is deliberately NOT here: it moves only at a roll
        // seam, which is a discrete transition a forensic read wants printed,
        // exactly like `runway=` below.
        "coping=",
        // ── end r24.14 [STAKES] masks ──────────────────────────────────────────
        // ── r24.14 [REGIONS] masks ─────────────────────────────────────────────
        //    (r24.14 REGIONS insertions go directly below this line)
        // ── end r24.14 [REGIONS] masks ─────────────────────────────────────────
        // ── r24.13 [VISION] masks ──────────────────────────────────────────────
        //    (r24.13 VISION insertions go directly below this line)
        // ── end r24.13 [VISION] masks ──────────────────────────────────────────
        // ── r24.13 [AFFECT] masks ──────────────────────────────────────────────
        //    (r24.13 AFFECT insertions go directly below this line)
        // ── end r24.13 [AFFECT] masks ──────────────────────────────────────────
        // ── r24.13 [SELF] masks ────────────────────────────────────────────────
        //    (r24.13 SELF insertions go directly below this line)
        // ── end r24.13 [SELF] masks ────────────────────────────────────────────
        // ── r24.13 [LOOPS] masks ───────────────────────────────────────────────
        //    (r24.13 LOOPS insertions go directly below this line)
        // ── end r24.13 [LOOPS] masks ───────────────────────────────────────────
        // ── r24.13 [PEOPLE] masks ──────────────────────────────────────────────
        //    (r24.13 PEOPLE insertions go directly below this line)
        // ── end r24.13 [PEOPLE] masks ──────────────────────────────────────────
        // ── r24.13 [INNER] masks ───────────────────────────────────────────────
        //    (r24.13 INNER insertions go directly below this line)
        // r24.13 (WO-P1): `runway=` is deliberately NOT masked, and this note
        // is the record of that decision. It is written only by the
        // between-turns compaction service, so it is piecewise-constant across
        // a whole turn — the `mini_own=` case, not the `moodm=` one. The tick
        // it moves is a discrete change in how much of this conversation she
        // can still keep, which is exactly the kind of transition this deduper
        // must not swallow.
        // ── end r24.13 [INNER] masks ───────────────────────────────────────────
        // ── r24.12 [COMPACT] masks ───────────────────────────────────────────
        // ── r24.12 [INNER] masks ─────────────────────────────────────────────
        // ── r24.12 [VISION] masks ────────────────────────────────────────────
        "apull=",   // WO-V11: the absence ramp moves every tick of an absence
        // ── r24.12 [AFFECT] masks ────────────────────────────────────────────
        // r24.12 (WO-MOM2): `moodf=<integrator>/<bias>` — MoodMomentum2::tick
        // leaks it on every 10 Hz tick exactly as moodm= above, so both halves
        // move by construction whenever the felt mood is non-zero. Masked
        // from birth (the WO-46 lesson). The crossing itself is not lost: the
        // tick it happens the mood_carry_note clause is rendered into the
        // field, and [mind/field] prints unconditionally.
        "moodf=",
        // ── r24.12 [MEMORY] masks ────────────────────────────────────────────
        // ══ end of the r24.12 masks ══════════════════════════════════════════
    };
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        bool masked = false;
        for (const char *v : vol) {
            const size_t n = std::strlen(v);
            const bool at_word = (i == 0) || line[i-1] == ' ' || line[i-1] == '[';
            if (at_word && line.compare(i, n, v) == 0) {
                out += v;
                i += n;
                while (i < line.size() && line[i] != ' ') i++;   // eat the value
                masked = true;
                break;
            }
        }
        if (!masked) out += line[i++];
    }
    return out;
}

// ── R20 (P0): the CEILING one barge capture may not exceed ──────────────────
// S17 root cause, and the other half of the r19.5 F1 fix above. F1 corrected
// the SILENCE TARGET (400 ms -> the turn endpoint) and left the loop's other
// exit — a fixed 60-poll count, ~6.5 s of wall clock — in place. A poll count
// is a duration wearing a loop bound's clothes, and it fires whether or not the
// interrupter has finished: S17 hit it four times in four minutes, each one
// cutting a sentence in half, and because the remainder was then dropped he had
// to barge again to finish, which hit the same ceiling again. (The 6.51 s
// window in F1's own S6 evidence list was this ceiling, unexplained at the
// time.) The ceiling now says what it means — how long ONE capture may run —
// and defaults to the longest window the capture stage can actually transcribe.
//   voice_ms: --voice-ms, the clamp cap_ms already obeys.
//   ring_ms:  the audio_async ring size; nothing can be read past it.
inline int barge_max_capture_target_ms(int override_ms, int voice_ms, int ring_ms) {
    int ms = override_ms > 0 ? override_ms : voice_ms;
    if (ms < 2000) ms = 2000;                       // never amputate on purpose
    const int ceiling = ring_ms > 1000 ? ring_ms - 1000 : 2000;
    return ms > ceiling ? ceiling : ms;             // the ring is the hard limit
}

struct DuplexGate {
    enum class Verdict { NONE, BARGE, ASIDE };
    bool   duplex      = true;
    int    confirm_ms  = 650;   // total voiced duration that claims the floor
    int    sustain_ms  = 300;   // the detector's own confirmation (barge_ms)
    // r19: a voice-end HANGOVER. The S5 log showed real ~1 s interjections
    // ("wait, hold on") classified as asides because ONE sub-threshold poll —
    // an intervocalic dip, a breath between words — ended the run before
    // confirm. A voice has not ended until it has been quiet this long; a
    // dip that rejoins keeps the candidate and keeps accumulating toward
    // confirm, which is what a real interruption does.
    int    hangover_ms = 350;
    bool   cand        = false;
    double cand_t0_ms  = 0.0;
    double last_voice_ms = 0.0;

    void reset() { cand = false; }

    // hit:    the detector's sustained-voice verdict this poll
    // in_run: the detector is currently inside a voiced run
    // now_ms: monotonic milliseconds
    // loud:   r19 — this poll's level is far above threshold (an insistent,
    //         raised voice). People get louder to interrupt; a loud burst
    //         claims the floor at the detector's own sustain, without
    //         waiting out the confirm window. S5: "stop"-class commands are
    //         shorter than any reasonable confirm; loudness is the one cue
    //         available in real time.
    Verdict feed(bool hit, bool in_run, double now_ms, bool loud = false) {
        if (!duplex) return hit ? Verdict::BARGE : Verdict::NONE;
        if (hit && !cand) { cand = true; cand_t0_ms = now_ms; last_voice_ms = now_ms; }
        if (!cand) return Verdict::NONE;
        if (in_run) {
            last_voice_ms = now_ms;
            if (loud) { cand = false; return Verdict::BARGE; }   // r19: insistence
            if (now_ms - cand_t0_ms >= (double) (confirm_ms > sustain_ms ? confirm_ms - sustain_ms : 0)) {
                cand = false;    // r19: a confirmed barge consumes the candidate.
                                 // S5 ghost: cand survived the barge, and the
                                 // first post-pivot poll fired a 14-second
                                 // "aside" quoting his own barge turn back at
                                 // him — twice.
                return Verdict::BARGE;
            }
            return Verdict::NONE;
        }
        // out of the run: not ended until the hangover has passed quietly
        if (now_ms - last_voice_ms < (double) hangover_ms) return Verdict::NONE;
        cand = false;
        return Verdict::ASIDE;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// UTF-8 boundary discipline (r19.6 / C32).
//
// Two places cut speech text at an offset that did not come from a character
// scanner: the barge-in rollback, whose `ch` is derived from how much audio the
// vocoder had delivered, and the end-of-turn flush of whatever token pieces are
// still pending. Both can land INSIDE a multi-byte sequence. The adversarial
// fuzz caught it: 189 of 480 hostile turns produced a byte string that is not
// valid UTF-8 — and the rollback path feeds that string straight into
// `finish_turn` and the episodic transcript, so a mid-word interruption on any
// non-ASCII character writes permanently broken bytes into her memory file.
//
// The rule everywhere below: never lengthen, never reorder, never rewrite valid
// text. Only refuse to emit bytes that cannot stand on their own.
// ─────────────────────────────────────────────────────────────────────────────

// Length in bytes of the well-formed sequence starting at s[i], or 0 if the
// bytes there are not a complete, canonical, in-range code point.
inline size_t utf8_seq_len(const std::string &s, size_t i) {
    if (i >= s.size()) return 0;
    const unsigned char c = (unsigned char) s[i];
    size_t n = 0;
    unsigned int cp = 0;
    if (c < 0x80)             { return 1; }
    else if ((c >> 5) == 0x6) { n = 2; cp = c & 0x1Fu; }
    else if ((c >> 4) == 0xE) { n = 3; cp = c & 0x0Fu; }
    else if ((c >> 3) == 0x1E){ n = 4; cp = c & 0x07u; }
    else                      { return 0; }          // continuation or 0xF8+
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; k++) {
        const unsigned char cc = (unsigned char) s[i + k];
        if ((cc >> 6) != 0x2) return 0;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (n == 2 && cp < 0x80)    return 0;            // overlong
    if (n == 3 && cp < 0x800)   return 0;            // overlong
    if (n == 4 && cp < 0x10000) return 0;            // overlong
    if (cp > 0x10FFFF)          return 0;            // out of range
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;      // surrogate half
    return n;
}

inline bool is_valid_utf8(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = utf8_seq_len(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

// The largest length <= n that ends on a code-point boundary. Used wherever an
// offset arrives from outside the text (audio progress, a byte budget).
inline size_t utf8_clip_len(const std::string &s, size_t n) {
    if (n >= s.size()) return s.size();
    size_t i = 0, last = 0;
    while (i < s.size()) {
        const size_t k = utf8_seq_len(s, i);
        if (k == 0) { i++; last = i > n ? last : i; continue; }   // pass junk through byte-wise
        if (i + k > n) break;
        i += k;
        last = i;
    }
    return last;
}

inline std::string utf8_clip(const std::string &s, size_t n) {
    return s.substr(0, utf8_clip_len(s, n));
}

// Drop every byte that is not part of a well-formed code point. Valid input is
// returned byte-identical — this can only ever shorten, and only ever by bytes
// that no decoder could have read.
inline std::string utf8_scrub(const std::string &s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = utf8_seq_len(s, i);
        if (n == 0) break;
        i += n;
    }
    if (i == s.size()) return s;                     // fast path: already valid
    std::string out;
    out.reserve(s.size());
    out.append(s, 0, i);
    while (i < s.size()) {
        const size_t n = utf8_seq_len(s, i);
        if (n == 0) { i++; continue; }
        out.append(s, i, n);
        i += n;
    }
    return out;
}

// ── r24.20 review (INTEGRATION F3): lexical content, in any script ──────────
// The r24.18 content-preserving heard-text filter keeps every well-formed
// codepoint, which is right for "café", Cyrillic and CJK — and wrong for a
// transcript that is nothing but symbols: Whisper's music token "♪" (U+266A),
// once a whole turn's text, became a three-word user turn and, during her
// speech, a validated barge (is_backchannel refuses any non-ASCII byte outside
// seven typographic marks). The old whitelist dropped such transcripts to ""
// → "Heard nothing". The rule that generalises both: a codepoint is LEXICAL
// unless it lies in a symbol/punctuation block, except letters and numbers
// INSIDE those blocks (r24.21: µ, ½, Ⅻ, ① and 〇 must not disappear).
// This bounded table is a noise filter, not a full Unicode category library.
// Decoding is on already-scrubbed, well-formed input.
inline uint32_t utf8_codepoint_at(const std::string &s, size_t i, size_t n) {
    const unsigned char c = (unsigned char) s[i];
    if (n == 1) return c;
    uint32_t cp = (n == 2) ? (c & 0x1Fu) : (n == 3) ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char) s[i + k] & 0x3Fu);
    return cp;
}
inline bool utf8_codepoint_is_symbol(uint32_t cp) {
    const bool symbol_block = (cp >= 0x00A0 && cp <= 0x00BF) || cp == 0x00D7 || cp == 0x00F7 ||   // Latin-1 punctuation/symbols, × ÷
           (cp >= 0x2000 && cp <= 0x2BFF) ||   // general punctuation … letterlike, arrows, maths, technical,
                                               // box/block/geometric, misc symbols (♪ ♫), dingbats, supplemental
           (cp >= 0x3000 && cp <= 0x303F) ||   // CJK symbols and punctuation
           (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK compatibility forms
           (cp >= 0xFF00 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
           (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65) ||   // fullwidth punctuation
           (cp >= 0x1D100 && cp <= 0x1D1FF) || // musical symbols
           (cp >= 0x1F000 && cp <= 0x1FAFF);   // emoji and pictographs
    if (!symbol_block) return false;
    // Unicode 15.0 L*/N* categories within the blocks above, frozen locally;
    // no locale dependence or runtime Unicode/model dependency is introduced.
    static constexpr uint32_t lexical[][2] = {
        {0x00AA, 0x00AA}, {0x00B2, 0x00B3}, {0x00B5, 0x00B5}, {0x00B9, 0x00BA},
        {0x00BC, 0x00BE}, {0x2070, 0x2071}, {0x2074, 0x2079}, {0x207F, 0x2089},
        {0x2090, 0x209C}, {0x2102, 0x2102}, {0x2107, 0x2107}, {0x210A, 0x2113},
        {0x2115, 0x2115}, {0x2119, 0x211D}, {0x2124, 0x2124}, {0x2126, 0x2126},
        {0x2128, 0x2128}, {0x212A, 0x212D}, {0x212F, 0x2139}, {0x213C, 0x213F},
        {0x2145, 0x2149}, {0x214E, 0x214E}, {0x2150, 0x2189}, {0x2460, 0x249B},
        {0x24EA, 0x24FF}, {0x2776, 0x2793}, {0x3005, 0x3007}, {0x3021, 0x3029},
        {0x3031, 0x3035}, {0x3038, 0x303C}, {0x1F100, 0x1F10C},
    };
    for (const auto &range : lexical)
        if (cp >= range[0] && cp <= range[1]) return false;
    return true;
}
// True iff the (well-formed) text carries at least one letter or digit of any
// script where recognized. ASCII: isalnum; multibyte: the bounded filter above.
inline bool has_lexical_content(const std::string &s) {
    for (size_t i = 0; i < s.size(); ) {
        const size_t n = utf8_seq_len(s, i);
        if (n == 0) { i++; continue; }
        if (n == 1) { if (std::isalnum((unsigned char) s[i])) return true; }
        else if (!utf8_codepoint_is_symbol(utf8_codepoint_at(s, i, n))) return true;
        i += n;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// r19.8 (F6) — who is actually speaking.
//
// `-p Igor` is fixed for the process, so in S8 every one of Caitlin's turns
// reached the model labelled `Igor:` — including Caitlin talking ABOUT Igor in
// the third person, and Caitlin announcing she would hand back to Igor. The
// "you" slot in the context was bound to the wrong person on every line for
// thirty minutes. Athena overrode it on content until the conversation went
// abstract, then said "I care about you, and about Caitlin" in one sentence and
// Caitlin had to correct her: "it's Caitlin you're talking to still, not Igor."
//
// That is not a lapse of hers. It is the transcript telling her something false
// on every turn. This detects an explicit handoff and returns the new name.
//
// Deliberately conservative — a false switch is the failure mode, so:
//   * only explicit self-identification or explicit correction counts;
//   * the name must be a single plausible name token, not a stopword;
//   * "let me give her the headset" arms nothing on its own — the NEW speaker
//     must introduce themselves before anything changes.
// An empty return means "no change".
inline bool plausible_name_(const std::string &w) {
    if (w.size() < 2 || w.size() > 20) return false;
    for (unsigned char c : w) if (!(std::isalpha(c) || c == '\'' || c == '-')) return false;
    // r20p3.14 (RS8): the r20p3.11 fix closed the inside-a-word case; this
    // closes the other half. A capitalised word after "it's" or "I'm" is very
    // often a closed-class noun, and the consequence of getting it wrong is
    // total: "It's Friday, so we can take our time" set active_person="Friday"
    // for the REST OF THE SESSION — every later user line written into the
    // context as "Friday:", consolidation extracting memories about a person
    // named Friday, a guest marked present, stranger_pull driven to 1.0, and the
    // field telling her she was talking to someone she does not know. It never
    // reverted unless he literally said "I'm Igor".
    static const char *stop[] = {
        "the","a","an","not","no","yes","here","there","back","him","her","them","me","you",
        "it","that","this","who","what","just","still","really","sorry","okay","ok","and",
        "but","talking","speaking","calling","going","about","with","from","for","now",
        "again","also","only","actually","using","one","two","his","she","he","they","we",
        // days, months, seasons — "it's Friday", "it's November already"
        "monday","tuesday","wednesday","thursday","friday","saturday","sunday",
        "january","february","march","april","may","june","july","august",
        "september","october","november","december",
        "today","tomorrow","yesterday","tonight","morning","afternoon","evening",
        "spring","summer","autumn","winter","christmas","easter","thanksgiving",
        "new","late","early","fine","good","great","done","over","enough","time",
        // nationalities and the commonest demonyms — "I'm American, if that matters"
        "american","british","english","irish","scottish","welsh","french","german",
        "italian","spanish","russian","polish","dutch","swedish","danish","norwegian",
        "canadian","australian","indian","chinese","japanese","korean","brazilian",
        "mexican","greek","turkish","israeli","ukrainian","african","european","asian",
    };
    std::string lo;
    for (unsigned char c : w) lo += (char) ::tolower(c);
    for (const char *s : stop) if (lo == s) return false;
    // RS8: a possessive is a relation, not an introduction. "I am Igor's
    // brother" returned "Igor's" — a name one apostrophe away from the person
    // he already is, which would have marked a GUEST while Igor kept talking.
    if (lo.size() > 2 && lo.compare(lo.size() - 2, 2, "'s") == 0) return false;
    return true;
}

inline std::string speaker_change(const std::string &raw) {
    if (raw.empty()) return "";

    // ── r20p3.14.2 (RS13): nothing inside square brackets is a person speaking ──
    //
    // RS9's window bounds the search to the opening clause, which closes the S9
    // case — there the field sat behind a full turn, ~300 bytes in. It does not
    // close the SHORT turn: "It's fine." plus her own appended field puts a
    // quoted "my name is Nicole" barely thirty bytes in, comfortably inside the
    // window, and the whole failure repeats. RS9 was written as defence in
    // depth for the real fix (pass the words as heard, not the assembled
    // buffer) and depth it has, but this hole is a hole.
    //
    // So the material is excluded by SHAPE rather than by position. Everything
    // in square brackets is machine-authored by construction — her inner-state
    // field `[Athena — …]`, the `[emotion: happy]` side-channel, the
    // `[interrupted]` marker — and a person introducing themselves out loud has
    // never once done it inside brackets. Default-deny, the same instinct as
    // RS11 at the vocoder: name the small set that is real and drop the rest.
    const std::string text = detail::strip_bracketed_(raw, '[', ']',
                                                      [](const std::string &) { return true; });
    if (text.empty()) return "";

    // Work on the ORIGINAL case: a name is capitalised, and that is one of the
    // few signals separating "this is Caitlin" from "this is fine".
    static const char *lead[] = {
        "my name is ", "this is ", "it's ", "its ", "i'm ", "im ", "i am ",
        "you're talking to ", "youre talking to ", "you are talking to ",
        "you're speaking with ", "you are speaking with ",
    };
    std::string lo;
    lo.reserve(text.size());
    for (unsigned char c : text) lo += (char) ::tolower(c);

    // ── r20p3.14.2 (RS9): POSITION-MAJOR, and bounded to the opening clause ────
    //
    // Two changes, both found by S9 and both about the same thing: an
    // introduction is the first thing a person says, not something buried
    // three hundred characters into a buffer.
    //
    // (1) The scan used to be LEAD-major — every occurrence of lead[0]
    //     ("my name is ") anywhere in the string was exhausted before lead[4]
    //     ("i'm ") was tried even once. Priority beat position, so
    //     "This is Sarah. ... my name is Nicole ..." returned "Nicole", and a
    //     buffer that happened to quote an earlier introduction beat the live
    //     one. Candidates are now collected, ordered by where they sit, and the
    //     EARLIEST is evaluated first.
    //
    // (2) A self-introduction lives in the opening clause. Anything past
    //     WINDOW bytes is refused unless it starts at byte 0. In S9 her own
    //     rendered field — which re-narrates earlier turns verbatim and is
    //     appended to his transcript — carried "Hi, my name is Nicole. I'm
    //     Igor's son." from twenty minutes earlier, and this function read it
    //     as a live introduction on a turn whose actual words were "I'm mostly
    //     worried about the budget, but I already got the CPU." The call site
    //     is fixed too (it now passes his words alone), but the window makes
    //     that guarantee hold even if a future caller reassembles a buffer.
    //
    // Deliberately NOT changed: the word-boundary test, plausible_name_, the
    // weak/strong split or the RS8b placement rule. This is ordering and
    // reach, not judgement.
    static const size_t WINDOW = 120;

    struct Cand { size_t at; const char *p; };
    std::vector<Cand> cands;
    for (const char *p : lead) {
        size_t at = 0;
        while ((at = lo.find(p, at)) != std::string::npos) {
            // ── r20p3.11 (RS3): the lead must start a WORD ──────────────────
            //
            // Without this, the short leads "im ", "its ", "it's " matched
            // inside ordinary words and the capitalised word after them was
            // accepted as a new speaker. Measured: "I told him Monday works"
            // returned "Monday"; "Tim Cook announced it" returned "Cook";
            // "That claim Robert made was wrong" returned "Robert"; "It suits
            // Denise better" returned "Denise". --speaker-switch defaults ON,
            // so each of those relabelled every following user line, added a
            // second antiprompt, marked a guest present, and told her in the
            // field that she was talking to someone she does not know. That is
            // the S8 failure this function exists to repair, inverted — and it
            // would fire several times in any ordinary conversation.
            bool boundary_ok = true;
            if (at > 0) {
                const unsigned char prev = (unsigned char) lo[at - 1];
                if (std::isalnum(prev) || prev == '\'' || prev == '-') boundary_ok = false;
            }
            if (boundary_ok && (at == 0 || at <= WINDOW) && aintent::direct_at(text, at))
                cands.push_back({ at, p });
            at += 1;
        }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b) { return a.at < b.at; });

    for (const Cand &c : cands) {
        const size_t at = c.at;
        const char * const p = c.p;
        size_t i = at + std::strlen(p);
        // pull the next word out of the ORIGINAL string
        while (i < text.size() && text[i] == ' ') i++;
        size_t j = i;
        while (j < text.size() && (std::isalpha((unsigned char) text[j]) ||
                                   text[j] == '\'' || text[j] == '-')) j++;
        if (j <= i) continue;
        const std::string w = text.substr(i, j - i);
        // Capitalised AND plausible. "this is fine" fails on both counts.
        if (!(std::isupper((unsigned char) w[0]) && plausible_name_(w))) continue;
        // RS8b: after a WEAK lead ("it's", "I'm", "this is") a name
        // has to sit where a name sits — at the end of the clause,
        // before a comma, or before "speaking"/"here"/"again"/a
        // second-person clause. Place names are open-class and
        // cannot be stoplisted: "This is Paris in the spring"
        // returned "Paris" and made her believe she had been handed
        // over to someone called Paris for the rest of the session.
        // The strong leads ("my name is", "you're talking to") need
        // no such test — they can only introduce.
        static const char *weak[] = { "it's ", "its ", "i'm ", "im ", "i am ", "this is " };
        bool is_weak = false;
        for (const char *q : weak) if (std::strcmp(p, q) == 0) { is_weak = true; break; }
        if (is_weak) {
            size_t k = j;
            while (k < text.size() && text[k] == ' ') k++;
            bool placed = (k >= text.size());
            if (!placed && std::strchr(".,!?;:\n", text[k])) placed = true;
            if (!placed) {
                std::string nx;
                for (size_t m = k; m < text.size() && std::isalpha((unsigned char) text[m]); m++)
                    nx += (char) ::tolower((unsigned char) text[m]);
                // "Just to remind you, it's Caitlin you're talking to
                // still, not Igor" is a real S8 turn: a name followed
                // by a second-person clause is as much an
                // introduction as one followed by a comma.
                static const char *okf[] = { "speaking", "here", "again", "now",
                                             "calling", "then", "by",
                                             "you're", "youre", "you", "talking",
                                             "still", "who" };
                for (const char *f : okf) if (nx == f) { placed = true; break; }
            }
            if (!placed) continue;
        }
        return w;
    }
    return "";
}

// ── r20p3.14.2 (RS10): the way home ────────────────────────────────────────────
//
// `active_person` is a latch. speaker_change only ever answers "somebody just
// introduced themselves", so once a guest takes the headset the owner can only
// get his own name back by introducing himself to someone who has known him for
// nine sessions. In S9 the guest's handoff was "I gotta sign off. I'm handing
// the headset back to Igor" — the owner named in the THIRD person, which is not
// an introduction and which speaker_change is right to refuse. The latch never
// reopened: every one of the owner's remaining turns was attributed to the
// guest, went into the consolidation transcript under the guest's name, and the
// shutdown farewell — literally "Goodbye, " + active_person — said goodnight to
// the wrong person and wrote that into her episodic record.
//
// A third-person handoff is not an introduction, but it is unambiguous evidence
// that the guest has finished, and the owner's name is already known from
// --person. It only ever returns the owner: this can restore the person she
// knows and can never invent a new one, which is what keeps it a narrow fix
// rather than a second speaker detector.
// Is this name in this text, as a word? Punctuation-tolerant on the right so
// "Igor," "Igor." "Igor's" and "Igor?" all count, and word-anchored on the left
// so "Igorovich" does not. Shared by handed_back_to and the guest backstop.
inline bool names_person(const std::string &text, const std::string &name) {
    if (text.empty() || name.empty()) return false;
    std::string lo, nm;
    lo.reserve(text.size() + 2);
    lo += ' ';
    for (unsigned char c : text) lo += (char) ::tolower(c);
    lo += ' ';
    for (unsigned char c : name) nm += (char) ::tolower(c);
    size_t at = 0;
    while ((at = lo.find(nm, at)) != std::string::npos) {
        // r21-full.4 (RC4): `at` is 0 when the configured name itself begins
        // with whitespace (a launcher passing --person "$NAME " with a stray
        // space), and lo[at-1] then reads one byte before the buffer. Guard it
        // rather than assume the pad.
        const unsigned char prev = at ? (unsigned char) lo[at - 1] : (unsigned char) ' ';
        const size_t e = at + nm.size();
        const unsigned char next = (e < lo.size()) ? (unsigned char) lo[e] : ' ';
        const bool left_ok  = !(std::isalnum(prev) || prev == '\'' || prev == '-');
        const bool right_ok = !(std::isalnum(next) || next == '-');
        if (left_ok && right_ok) return true;
        at += 1;
    }
    return false;
}

inline bool handed_back_to(const std::string &text, const std::string &owner) {
    if (text.empty() || owner.empty()) return false;
    std::string lo, ow;
    lo.reserve(text.size() + 2);
    lo += ' ';
    for (unsigned char c : text) lo += (char) ::tolower(c);
    lo += ' ';
    for (unsigned char c : owner) ow += (char) ::tolower(c);
    if (!names_person(text, owner)) return false;

    // The handoff frames. Each names the owner in the third person and says the
    // headset, the microphone or the conversation is going back to him.
    static const char *frame[] = {
        "handing the headset back to ", "handing it back to ", "hand it back to ",
        "handing you back to ", "handing the mic back to ",
        "giving the headset back to ", "giving it back to ",
        "passing you back to ", "passing it back to ",
        "here's ", "heres ", "here is ", "here comes ",
        "you're back with ", "youre back with ", "you are back with ",
        "handing over to ", "handing back to ", "back over to ",
    };
    // Deliberately NOT here: a bare "back to " and "talk to ". Both fire on
    // ordinary sentences that merely mention the owner — "anyway, back to Igor",
    // "talk to Igor about it later" — and while the consequence of a false
    // positive is only ever "restore the person she already knows", a detector
    // that fires on mentions is a detector that has stopped meaning anything.
    for (const char *f : frame) {
        size_t at = 0;
        while ((at = lo.find(f, at)) != std::string::npos) {
            // lo has one leading space; scope ownership to the source bytes.
            if (!at || !aintent::direct_at(text, at - 1)) { ++at; continue; }
            if (at > 0) {
                const unsigned char prev = (unsigned char) lo[at - 1];
                if (std::isalnum(prev) || prev == '\'' || prev == '-') { at += 1; continue; }
            }
            size_t k = at + std::strlen(f);
            while (k < lo.size() && lo[k] == ' ') k++;
            if (lo.compare(k, ow.size(), ow) == 0) {
                const size_t e = k + ow.size();
                if (e >= lo.size() || !(std::isalnum((unsigned char) lo[e]) ||
                                        lo[e] == '\'' || lo[e] == '-')) return true;
            }
            at += 1;
        }
    }
    return false;
}

// r21-B: her voice directives. Consumed by the voice model (athena_voice.h) and
// stripped here so they can never reach the vocoder — Orpheus was trained on the
// eight gesture tags and on nothing else in angle brackets, so an unknown one is
// either spoken aloud as a word or turned into noise. Deliberately separate from
// the gesture repair below, which exists to PRESERVE the eight it does know.
// ── r20p3.14.3 (RA11): her beat, split out as a pure function ───────────────
//
// S10 measured her emitting <pause> 23 times in 28 turns — a tag she invented,
// which the default-deny strip correctly removed and which therefore did
// nothing. It is real now: the writer turns each one into a trigger-file
// sentinel, orpheus closes the chunk there, and its existing between-chunks
// pause_ms insertion lands a breath at the point she asked for it.
//
// This is the only part of that worth testing on its own, so it lives here
// rather than inside the writer: given her text and a budget, where does the
// text divide? Everything downstream is bookkeeping.
//
// Returns the pieces in order. ONE piece means no beat and the caller must take
// the byte-for-byte unchanged path — that is the gate this whole feature rests
// on, and it is why an empty budget and a text with no <pause> both land here.
inline size_t find_pause_tag(const std::string &text, size_t from, size_t &len) {
    // 14.1: "<pause>" is FIXED WIDTH — test the seven bytes in place. The old
    // form found the nearest '>' (unbounded) and substr'd the whole candidate
    // body: on a '<'-runaway with one distant '>' that is quadratic scans AND
    // quadratic allocation — the fourth instance of the class this file
    // measured three times (RC2 5.1 s, RC5 6.7 s, R6-Y 1.24 s), armed by
    // SM3's own one-byte advance.
    size_t at = from;
    while ((at = text.find('<', at)) != std::string::npos) {
        if (at + 6 < text.size() && text[at + 6] == '>') {
            bool is_pause = true;
            static const char kPause[] = "pause";
            for (int i = 0; i < 5; i++) {
                char c = text[at + 1 + (size_t) i];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (c != kPause[i]) { is_pause = false; break; }
            }
            if (is_pause) { len = 7; return at; }
        }
        at += 1;   // SM3: advance past THIS '<', never past a far '>'
    }
    return std::string::npos;
}

inline std::vector<std::string> split_on_beats(const std::string &text, int max_beats) {
    std::vector<std::string> parts;
    if (max_beats <= 0) return parts;                 // beats disabled -> plain path
    size_t from = 0, len = 0, at;
    while ((int) parts.size() < max_beats &&
           (at = find_pause_tag(text, from, len)) != std::string::npos) {
        parts.push_back(text.substr(from, at - from));
        from = at + len;
    }
    if (parts.empty()) return parts;                  // no <pause> at all
    parts.push_back(text.substr(from));
    return parts;
}

inline std::string strip_voice_directives(const std::string &text) {
    // r20p3.14 (RA1): case-folded, because avox::take_intent is. It was not,
    // so "<Steady>" was CONSUMED as an intent and LEFT in the text — she began
    // holding her voice and the vocoder was handed a token Orpheus has never
    // seen, on precisely the inputs where the intent was accepted. Nothing
    // downstream would have caught it: strip_bracketed_ only matches <|...|>
    // and repair_gesture_tags only the eight it knows.
    static const char *d[] = { "steady", "soften", "lift", "slow",
                               "quiet", "release", "unhold" };
    std::string out = text;
    for (const char *name : d) {
        size_t at = 0;
        while (true) {
            // Find "<name>" case-insensitively.
            // r21-full.4 (RC5): compare IN PLACE. The previous form did a
            // fresh find('>') plus a substr allocation for every '<' in the
            // text, for each of the seven names — 6.7 s on a 60 KB '<' runaway,
            // inside the sanitiser that gates the voice loop and the barge-in
            // poll. A directive body is at most eight characters, so a length
            // check and a case-folded compare settle it without allocating,
            // and the scan for the closing '>' is bounded to that window.
            size_t found = std::string::npos, n = 0;
            const size_t nlen = std::char_traits<char>::length(name);
            for (size_t i = at; i + 2 < out.size(); i++) {
                if (out[i] != '<') continue;
                if (i + 1 + nlen >= out.size() || out[i + 1 + nlen] != '>') continue;
                bool same = true;
                for (size_t k = 0; k < nlen && same; k++) {
                    const char c = out[i + 1 + k];
                    same = ((c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c) == name[k];
                }
                if (same) { found = i; n = nlen + 2; break; }
            }
            if (found == std::string::npos) break;
            at = found;
            out.erase(at, n);
            // No doubled space left behind: the vocoder reads one as a pause
            // she did not choose.
            while (at < out.size() && out[at] == ' ' && at > 0 && out[at-1] == ' ')
                out.erase(at, 1);
        }
    }
    return out;
}

// ── r24.7 (WO-81(2) / S20 D3, the WO-58b class at the STORE) ────────────────
// What may be VOICED and what may be STORED are different questions.
// sanitize_for_tts deliberately keeps the eight gestures — Orpheus needs them
// — but a caption is written to keepsakes.tsv, re-injected verbatim at every
// startup as "what I wrote about it then", and S20's flowers keepsake now
// opens on a literal "<sniffle>" forever. This is the store-side answer:
// remove exactly the tags the TTS layer OWNS — the eight gestures
// (ATHENA_GESTURE_TAGS), the seven silent voice directives, and her <pause>
// beat — and nothing else. LEXICON-based on purpose: the RS11/RS14
// default-deny is a SHAPE test, and a shape test over stored prose would eat
// content she may legitimately describe (a "<h1>" she read off a screen).
// Runs the seam's own order (WO-58 #50): repair first, so a dropped '>'
// ("<sniffle I'm sorry") is canonicalised before the lexicon looks for it,
// and every removal carries the strip's doubled-space cleanup.
inline std::string strip_tts_tags_for_store(const std::string &text) {
    std::string s = repair_gesture_tags(text);
    static const char *tags[] = ATHENA_GESTURE_TAGS;
    auto erase_all = [&s](const std::string &tag) {
        size_t at = 0;
        while ((at = s.find(tag)) != std::string::npos) {
            s.erase(at, tag.size());
            while (at < s.size() && s[at] == ' ' && at > 0 && s[at - 1] == ' ')
                s.erase(at, 1);
        }
    };
    // Post-repair the gestures are canonical lowercase well-formed, so the
    // exact form is the only form (RS15 normalises "<Sniffle>" first).
    for (const char *t : tags) erase_all(std::string("<") + t + ">");
    erase_all("<pause>");                       // her beat is TTS timing, not text
    return strip_voice_directives(s);           // the seven, already lexicon-based
}

inline std::string sanitize_for_tts(const std::string &text) {

    // r14 (F5): fix broken gesture tags first, so every later pass sees them
    // in canonical form.
    //
    // r24.6 (WO-58 / review #50): this WAS `strip_voice_directives(text)`
    // first. That put the strip BEFORE the repair, which broke two things at
    // once. (1) RS16's own comment (repair_gesture_tags, above) promises it
    // hands a repaired silent directive "to the very next stations in sanitize
    // (strip_voice_directives and the RS14 default-deny)" -- but the strip had
    // already run, so a repaired "<soften" was removed by the RS14 default-deny
    // instead, and the default-deny does NOT carry the double-space cleanup the
    // strip does. "Okay, <soften here goes." came out "Okay,  here goes." while
    // the well-formed "Okay, <soften> here goes." came out with one space: the
    // same directive rendered two different ways depending on whether the model
    // dropped the '>'. The vocoder reads the doubled space as a pause she did
    // not choose. (2) Stripping first deletes a well-formed directive from
    // between a truncated gesture and the following word, welding them --
    // "<gasp<quiet>word" became "<gaspword", and repair's word-boundary guard
    // then could not see the gesture at all, so a bare "<gasp" fragment rode to
    // the vocoder. Measured over 200,000 adversarial fragment strings: the
    // reorder leaves 1,125 outputs with FEWER stray angle brackets and 162 with
    // more (all of them doubled-'>' inputs no model produces), and every
    // realistic spaced input changes in whitespace only.
    std::string s = repair_gesture_tags(text);
    s = strip_voice_directives(s);   // r21-B
    // Reasoning content must never reach the TTS. The special tag ids are
    // banned at the sampler (--reasoning off), but a model can still SPELL a
    // textual variant — "<thinking>" was observed in the field. Drop designated
    // spans, including an unfinished tail, then stray close tags (case-insensitive).
    s = detail::strip_think_spans_(s);
    s = detail::strip_paired_(s, "**", '*', /*keep_inner=*/true);
    s = detail::strip_paired_(s, "*",  '*', /*keep_inner=*/true);
    s = detail::strip_paired_(s, "`",  '`', /*keep_inner=*/false);
    s = detail::strip_headings_(s);
    // Link BEFORE bare URL — this order matters and stock had it reversed. The
    // URL pattern consumes to the next space, so on "[the paper](https://x.y)"
    // it ate "https://x.y)" including the closing paren, the link pattern then
    // could not match, and the fragment "[the paper](" was VOICED. Found the
    // first time this transformation was ever executed under test; it predates
    // the overlay. With links unwrapped first, bare URLs still die.
    s = detail::unwrap_links_(s);
    s = detail::strip_urls_(s);
    // `<\|[a-z_]*\|>` — a bracketed span between '<' and '>' whose body is
    // exactly |[a-z_]*|.
    s = detail::strip_bracketed_(s, '<', '>', [](const std::string &b) {
        if (b.size() < 2 || b.front() != '|' || b.back() != '|') return false;
        for (size_t k = 1; k + 1 < b.size(); k++)
            if (!((b[k] >= 'a' && b[k] <= 'z') || b[k] == '_')) return false;
        return true;
    });
    // ── r20p3.14.2 (RS11): DEFAULT-DENY on an unrecognised <word> tag ──────────
    //
    // Orpheus was trained on eight bracketed gestures and on nothing else in
    // angle brackets. An unknown one is either spoken aloud as a word or turned
    // into noise. The strip above only matches the <|...|> form and
    // repair_gesture_tags only knows the eight it repairs, so a bare <word> the
    // model invented passed straight through: S9 sent <end_thought> twice and
    // <end> once to the vocoder, and one chunk was nothing but "Promise.  <end>".
    //
    // Deliberately an ALLOW-list. A block-list is a losing game here — what
    // reaches this function depends on what the model decides to emit, which is
    // unbounded, while what Orpheus can say is a closed set of eight. Runs
    // AFTER repair_gesture_tags so a malformed-but-known tag has already been
    // canonicalised and survives.
    //
    // The shape guard keeps it off ordinary prose: only tag-shaped bodies are
    // eaten, so "5 < 10 > 3", "a < b and c > d" and "x <= y" are untouched.
    //
    // r20p3.14.4 (RS14): the guard was "single LOWERCASE word", which made the
    // deny not a default at all — the fuzz battery walked straight through it
    // with "<Pause>", "<PAUSE>", "<>" and "<long pause>", every one of which
    // Orpheus would voice as junk. Widened, carefully, because the reason the
    // guard was narrow is real (an inequality in prose must never be eaten):
    //
    //   * case-folded before testing — "<Pause>" is the same invention as
    //     "<pause>". Runs after RS15 has already normalised mis-cased GESTURES
    //     to lowercase, so anything still cased here is not one of hers.
    //   * an empty "<>" is stripped — no prose produces it.
    //   * a body that TRIMS to one word is stripped — "< pause >" is a tag
    //     with elbows, not arithmetic.
    //   * TWO words are stripped only when the body hugs its brackets
    //     ("<long pause>", "<deep breath>" — the stage-direction shape LLMs
    //     invent), because a two-word span with spaced brackets is what an
    //     inequality pair looks like ("a < b and c > d" scans " b and c ",
    //     three words, untouched; "x < y or z > w" scans " y or z ", three
    //     words, untouched — and even at two words, " b or c " does not hug).
    //   * three or more words are never touched.
    s = detail::strip_bracketed_(s, '<', '>', [](const std::string &b) {
        // 14.1: the 24-byte gate failed OPEN for exactly the shapes a model
        // invents — "<end_of_conversation_marker>" (26 bytes) went to Orpheus
        // verbatim. A body that hugs its brackets and folds to ONE
        // [a-z0-9_/-] word is never an inequality at ANY length; the 24-byte
        // cap now applies only to spaced/multi-word bodies (where the
        // inequality shape lives).
        if (b.size() > 24) {
            if (b.size() > 64) return false;
            for (unsigned char c0 : b) {
                const char c = (char)((c0 >= 'A' && c0 <= 'Z') ? c0 - 'A' + 'a' : c0);
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '/'))
                    return false;
            }
            return true;
        }
        if (b.empty()) return true;                               // RS14: "<>"
        std::string f;                                            // folded body
        f.reserve(b.size());
        for (unsigned char c : b)
            f += (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        const bool hugs = f.front() != ' ' && f.back() != ' ';
        // ── r21-full.6 (R6-Z): the CLOSING and SELF-CLOSING forms ────────────
        // A '/' anywhere in the body hit the "digits, punct: prose" arm and
        // returned false — i.e. the default-deny FAILED OPEN on exactly the
        // markup an LLM emits most: "</think>", "</response>", "</s>",
        // "<break/>". Those went to Orpheus verbatim, which is the whole defect
        // RS11 exists to close, and the one tag shape prose never produces: no
        // inequality in English is written "a </ b" or "a b/> c". A slash on
        // either end is stronger evidence of a tag than its absence, so it is
        // consumed and the rest of the test runs on what it wrapped — and the
        // two-word "hugs" rule is satisfied by construction for these.
        bool slashed = false;
        if (f.front() == '/')      { f.erase(0, 1);            slashed = true; }
        if (!f.empty() && f.back() == '/') { f.pop_back();      slashed = true; }
        if (f.empty()) return true;                          // "</>" , "<//>"
        // Digits belong to a tag when the body HUGS its brackets and carries a
        // letter ("<h1>", "<end_thought2>"); a hugging pure number does not
        // exist in prose either side of an operator, but " 10 " (spaced) is the
        // inequality shape and stays protected.
        bool has_alpha = false;
        for (char c : f) if (c >= 'a' && c <= 'z') { has_alpha = true; break; }
        const bool digits_ok = has_alpha && (hugs || slashed);
        size_t words = 0, wlen = 0;
        for (size_t i = 0; i <= f.size(); i++) {
            const char c = (i < f.size()) ? f[i] : ' ';
            if (c == ' ') { if (wlen) { words++; wlen = 0; } continue; }
            const bool alnum_ok = (c >= 'a' && c <= 'z') || c == '_' ||
                                  (digits_ok && ((c >= '0' && c <= '9') || c == '-'));
            if (!alnum_ok) return false;                       // punct: prose
            if (++wlen > 20) return false;
        }
        if (slashed) return true;   // a slashed tag body is never prose
        if (words == 0) return true;                              // spaces only
        if (words > 2)  return false;                             // inequality territory
        if (words == 2 && !hugs) return false;
        // the eight gestures survive — post-RS15 they are lowercase, so a body
        // that folds to one but was cased is an imitation, and is stripped
        std::string trimmed = f;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(0, 1);
        while (!trimmed.empty() && trimmed.back()  == ' ') trimmed.pop_back();
        for (const char *k : ATHENA_GESTURE_TAGS)
            if (trimmed == k) return b != trimmed;                // exact lowercase survives
        return true;
    });
    // The rollback preamble no longer marks cut-off turns with any token (a
    // trailing em-dash and then "[interrupted]" were both imitated by the
    // model and made it truncate its own turns — see the pivot code). These
    // two strips are pure insurance: if the model ever reproduces an old
    // marker from training priors or residual context, it is not vocalized.
    //
    // Also the vocal-emotion tag ("[emotion: happy]" is metadata appended to
    // the user's turns for the model to read, never speech), and the
    // consciousness field. The field is appended to the user's turns as the
    // channel she reads her own state from, never as speech — but it sits in
    // the context window on every turn, which makes it the single most
    // imitable string in the prompt. Its shape is exact: a bracketed span
    // whose first token group is followed by a spaced em-dash (U+2014, matched
    // as its literal UTF-8 bytes since this is all byte-oriented), 1-64 bytes
    // before the dash and at most 700 after. Prose does not produce it.
    s = detail::strip_bracketed_(s, '[', ']', [](const std::string &b) {
        std::string low;
        low.reserve(b.size());
        for (unsigned char c : b) low += (char) ::tolower(c);
        if (low == "interrupted") return true;
        if (low.compare(0, 8, "emotion:") == 0) return true;
        // BASELINE PARITY: the field-shape predicate is the one strip in this
        // function that stock ATHENA did not have, and it deletes ordinary
        // bracketed prose that happens to contain a spaced em-dash —
        // "I read it in [Nature — the 2019 paper] last week" became "I read it
        // in  last week". Rare, but it is a silent content deletion, and the
        // string it defends against only exists when the substrate is putting
        // fields in the prompt. Gated, so with consciousness off this whole
        // function is byte-for-byte what it always was.
        if (!detail::field_shape_active()) return false;
        const size_t dash = b.find(" \xE2\x80\x94 ");
        return dash != std::string::npos && dash >= 1 && dash <= 64 &&
               b.size() - (dash + 5) <= 700;
    });
    // r24.12 (WO-C11a / S22 §C.1.12): a WHOLE-LINE bracketed span that is not
    // one of her envelopes is a stage direction and is not speech — see
    // detail::strip_stage_directions_. After the strip above, so
    // [interrupted], [emotion: …] and the field shape have already gone their
    // own way; before the trailing trim, which cleans up after it. Because
    // sanitize_for_tts is what produces said_this_turn, the TTS chunk, the
    // barge path's re-decoded prefix and the efference copy, one rule covers
    // the log, the transcript, the speech and the rollback — intended. The
    // KV keeps the completed turn's tokens (a snapshot restore + re-prefill
    // per affected turn is not worth a trailing line the next "\nIgor:" tag
    // already closes). ATHENA_STRIP_STAGE_DIRECTIONS=0 restores r24.11.
    if (detail::stage_direction_strip_on())
        s = strip_stage_directions(s, detail::bot_name_ref());
    // If a turn still ends on a dangling em/en-dash (the model trailing off
    // mid-thought), drop the bare dash so she stops at a clean word boundary
    // instead of vocalizing a confusing trail-off. U+2014/U+2013 are 3-byte
    // UTF-8 (0xE2 0x80 0x94 / 0x93); std::regex is byte-oriented, so match
    // the sequence literally.
    // r19.6 (CRITICAL): this was the last unbounded-repetition regex in the
    // file, and it begins with `\s*`. libstdc++'s backtracking engine recurses
    // once per character and regex_replace retries at every offset, so a
    // whitespace RUN costs O(k^2) work and O(k) stack. Measured on the real
    // function: 8k newlines = 1.6 s, 16k = 7.1 s, 30k = SIGSEGV. A newline
    // runaway is the one runaway the samplers cannot damp — "\n" is an explicit
    // DRY breaker, and find_sentence_end never flushes a run with no `.?!`, so
    // the whole thing arrives here in one piece and kills the brain. Replaced
    // with the same linear scanner discipline as every other strip in this
    // file: walk back over trailing whitespace, then over one em/en dash, then
    // over the whitespace before it. No allocation, no recursion, O(k).
    {
        auto is_ws = [](unsigned char ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                   ch == '\v' || ch == '\f';
        };
        size_t e = s.size();
        while (e > 0 && is_ws((unsigned char) s[e-1])) --e;
        if (e >= 3 && (unsigned char) s[e-3] == 0xE2 && (unsigned char) s[e-2] == 0x80 &&
            ((unsigned char) s[e-1] == 0x93 || (unsigned char) s[e-1] == 0x94)) {
            e -= 3;
            while (e > 0 && is_ws((unsigned char) s[e-1])) --e;
            s.resize(e);
        }
    }
    // Safety net for ChatML role-label leakage. The sampler now stops on the
    // <|im_start|> token, but if a model ever emits the bare role word with no
    // special token, it surfaces fused to the final punctuation with no space —
    // "...warmest?assistant", "...edge off.user". That no-space fusion is the
    // signature of the turn-boundary artifact; natural prose always has a space
    // after sentence punctuation, so stripping a role word welded directly to
    // .?! at end of string never touches real content.
    // r21-full.4 (RC1): the trailing trim runs FIRST. libstdc++ implements
    // `\s*$` by recursing once per character, so a long whitespace run reaching
    // this regex overflows the stack and takes the process down — measured, a
    // 28,000-byte run segfaults. A newline runaway is exactly the runaway the
    // samplers cannot damp (see the r19.6 note above), it arrives here in one
    // piece, and `n_gen_max` allows ~60 KB. Trimming first makes the tail
    // bounded before any backtracking matcher sees it; the regex still does
    // its job, because a role word welded to punctuation is at the very end
    // once the whitespace is gone.
    {
        const size_t last = s.find_last_not_of(" \t\n\r");
        if (last == std::string::npos) return "";
        if (last + 1 < s.size()) s.resize(last + 1);
    }
    s = std::regex_replace(s, std::regex("([.?!])(assistant|user)\\s*$", std::regex::icase), "$1");
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    // C32: last gate before the vocoder pipe. The Orpheus reader decodes this
    // as UTF-8; a dangling lead byte from a cut-off token raises
    // UnicodeDecodeError on the Python side and takes the whole speech stream
    // down mid-sentence. Valid text is returned unchanged, byte for byte.
    return utf8_scrub(s.substr(start, end - start + 1));
}


// ─────────────────────────────────────────────────────────────────────────────
// The cross-chunk field-echo guard.
//
// sanitize_for_tts strips a COMPLETE echoed "[Athena — ...]" span, but the
// streaming flusher splits at sentence boundaries — and a gist routinely
// carries a '.' or '?', so the split landed INSIDE the brackets and neither
// fragment matched the complete-span regex. Verified before the guard existed:
// the chunk "[Athena — aware of what he asked: is free will real?" reached the
// vocoder verbatim. This is the same mechanism as the reasoning-span
// suppressor: once an opening field appears unclosed in the pending buffer,
// hold everything until it closes or the turn ends.
//
// Pure state machine over the caller's pending buffer: `step` returns any
// prefix that is safe to flush right now and mutates `pending` exactly as the
// previously-inline code did. The caller owns the actual TTS write.
// ─────────────────────────────────────────────────────────────────────────────
class FieldEchoGuard {
public:
    struct Step {
        std::string flush;        // safe to hand to the sentence flusher now
        bool        began_hold  = false;   // log point: an unclosed field appeared
        bool        released    = false;   // oversize: not a field after all
    };

    bool holding() const { return holding_; }

    // ── r24.12 (WO-C11b / S22 §C.5.8): every envelope opener, not one ───────
    // The guard knew exactly one opener, "[<bot> — ". S1 spoke "[Athena
    // remembers — a kept" (orpheus chunk 5, COMPLETE): her recall was refused
    // in silence, the model generated the album envelope's opener as its next
    // line, this guard did not know it, and the bracket strip needs a closed
    // span. The vision and mind seams inject a whole family — "[Athena
    // remembers —", "[Athena looks through the camera —", "[While he was away,
    // Athena looked around —", "[what Athena wrote about it then:", "[Athena
    // has taken the picture,", "[The picture that reached Athena", "[The last
    // thing Athena's eyes took in" — and an imitation of any of them must be
    // held until it closes and discarded if the turn ends first. The rule is
    // the sanitizer's own shape plus the family's stems: a '[' at a line start
    // followed within 64 bytes by a spaced em-dash (the field shape, whatever
    // name it carries), or "[" + bot / "[what " + bot / "[While he was away, "
    // + bot / "[The picture that reached " + bot / "[The last thing " + bot
    // anywhere. Returns the opener's position, or npos.
    // ATHENA_ENVELOPE_ECHO_GUARD=0 restores the single r24.11 opener.
    static bool envelope_guard_on() {
        static const bool on = [] {
            const char *e = ::getenv("ATHENA_ENVELOPE_ECHO_GUARD");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    static size_t find_envelope_opener_(const std::string &pending, const std::string &bot) {
        size_t best = std::string::npos;
        auto take = [&](size_t p) { if (p != std::string::npos && p < best) best = p; };
        // the family's stems, anywhere
        const std::string stems[] = {
            "[" + bot, "[what " + bot, "[While he was away, " + bot,
            "[The picture that reached " + bot, "[The last thing " + bot,
        };
        for (const std::string &st : stems) take(pending.find(st));
        // the field shape at a line start: '[' … " — " within 64 bytes, no ']' before the dash
        for (size_t p = pending.find('['); p != std::string::npos; p = pending.find('[', p + 1)) {
            if (p != 0 && pending[p - 1] != '\n') continue;
            const size_t dash = pending.find(" \xE2\x80\x94 ", p + 1);
            if (dash == std::string::npos || dash - p > 64) continue;
            if (pending.find(']', p) < dash) continue;
            take(p);
            break;
        }
        return best;
    }

    Step step(std::string &pending, const std::string &bot_name) {
        Step out;
        const std::string opener = "[" + bot_name + " \xE2\x80\x94 ";
        // ── r20p3.14.7 (SM4): re-arm once the released opener has drained ──────
        // released_span_ latches so an oversize run-on opener sitting at
        // pending[0] is not re-detected every token. But it only ever cleared on
        // a ']' or a barge reset(), so after ONE oversize release a SECOND field
        // echo LATER in the same turn — the model, now demonstrably imitating the
        // field format — sailed through: the literal "[Athena — …]" reached the
        // vocoder, the one thing this guard exists to stop. The latch clears the
        // moment the offending opener is no longer at the front of pending (it
        // has been flushed as ordinary prose, which is what "released" means), so
        // a fresh opener is guarded again.
        if (released_span_ && !holding_) {
            // r24.12 (WO-C11b): "the offending opener is no longer at the
            // front" is asked of whichever opener the guard now knows, or the
            // SM4 latch would clear on the very token it was written for.
            const size_t front = envelope_guard_on()
                ? find_envelope_opener_(pending, bot_name)
                : ((pending.size() >= opener.size() &&
                    pending.compare(0, opener.size(), opener) == 0) ? 0 : std::string::npos);
            if (front != 0) released_span_ = false;
        }
        if (!holding_ && !released_span_) {
            const size_t fpos = envelope_guard_on()
                ? find_envelope_opener_(pending, bot_name)   // r24.12 (WO-C11b)
                : pending.find(opener);
            if (fpos != std::string::npos &&
                pending.find(']', fpos) == std::string::npos) {
                if (fpos > 0) out.flush = pending.substr(0, fpos);
                pending.erase(0, fpos);
                holding_ = true;
                out.began_hold = true;
            }
        }
        if (holding_) {
            const size_t close_b = pending.find(']');
            if (close_b != std::string::npos) {
                pending.erase(0, close_b + 1);
                holding_       = false;
                released_span_ = false;    // a closed bracket ends the span
            } else if (pending.size() > 1024) {
                // A real field is hard-capped near 700 bytes; anything longer
                // is not a field echo — release the hold rather than eat prose.
                //
                // LATCHED, because the opener is still sitting at pending[0]:
                // without this the next step() re-detects it, re-holds and
                // re-releases, once per token. Measured on 2.4 KB of run-on
                // prose after an opener: 352 identical "began_hold" log lines
                // for one event, zero release lines — and on the
                // sentence-ending variant the literal string "[Athena — "
                // reached the vocoder on the first flush, which is the one
                // thing this guard exists to prevent.
                holding_      = false;
                released_span_ = true;
                out.released  = true;
            }
        }
        return out;
    }

    // Abandon any open hold WITHOUT touching the caller's buffer. Used on the
    // barge-in pivot, where the turn is being restarted from scratch: the
    // pending buffer is cleared by the caller, and a hold left open here would
    // swallow the entire replacement reply.
    void reset() { holding_ = false; released_span_ = false; }

    // Turn ended with the hold still open: the caller must DISCARD the pending
    // buffer (an unclosed field must never be spoken). Returns the byte count
    // for the log line and resets.
    size_t end_of_turn(std::string &pending) {
        if (!holding_) return 0;
        const size_t n = pending.size();
        pending.clear();
        holding_       = false;
        released_span_ = false;
        return n;
    }

private:
    bool holding_       = false;
    // A span already given up on as too long to be a field. Latched so the
    // opener still at the head of the buffer cannot re-arm the hold on every
    // subsequent token.
    bool released_span_ = false;
};

// The blocking fallback receives a whole generated reply, rather than the
// streaming buffer's already guarded chunks. Apply the same publication
// boundary before handing any bytes to a stock/custom speech wrapper.
inline std::string sanitize_completed_reply_for_tts(const std::string &text,
                                                     const std::string &bot) {
    std::string pending = sanitize_for_tts(text);
    FieldEchoGuard guard;
    const auto part = guard.step(pending, bot);
    guard.end_of_turn(pending);
    return sanitize_for_tts(part.flush + pending);
}

// ─────────────────────────────────────────────────────────────────────────────
// Console parity.
//
// The demo's terminal shows the user's line in bold exactly as stock ATHENA
// printed it: transcription plus the [emotion:] and [time:] tags. The
// inner-state field is PROMPT content, not display content — printed, it
// turned every console user line into a paragraph of her state. And an
// autonomous turn has no user words at all: with the field stripped the
// display would be a dangling empty "Igor:" line, so it shows only her
// speaker tag — on screen she simply starts talking, which is what happened.
// ─────────────────────────────────────────────────────────────────────────────
inline std::string console_display(const std::string &prompt_text,
                                   const std::string &field,
                                   bool autonomous,
                                   const std::string &bot_name,
                                   const std::string &chat_symb) {
    if (autonomous) return "\n" + bot_name + chat_symb;
    std::string display = prompt_text;
    if (!field.empty()) {
        const size_t fp = display.find(field);
        if (fp != std::string::npos) display.erase(fp, field.size());
    }
    return display;
}

// ═════════════════════════════════════════════════════════════════════════════
// r21-full.3 (Fix 8) — the inner voice's seam logic, moved here from
// talk-llama.cpp so it can be EXECUTED under test.
//
// r21-full.2 put four new pure functions back inside the translation unit this
// header exists to empty, and the cost was immediate: a shipped feature flag
// (ATHENA_INNER_RECALL) that silenced only half of what it documented, because
// nothing could reach the other half to assert on it. These are the same
// functions, with the fixes the review called for, in a place a test can drive.
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// Long-term recall: what a thought reaches back for in the CONSOLIDATED store.
//
// r21-full.2 called any run of four or more alphanumerics a "substantial word"
// and matched on two of them. In English that is `that`, `this`, `with`,
// `have`, `what`, `would`, `been`, `said`, `about`, `from`, `they`, `were`,
// `when`, `them` — so a thought about microphone gain reliably "brought back"
// whichever unrelated memory happened to be longest, because the score was a
// raw count with no length normalisation, and ties went to the oldest row.
// Measured: three of eight realistic seeds produced a clause and all three were
// semantically unrelated.
//
// Now: closed-class words are excluded, the word list is a SET (so one word
// repeated cannot satisfy "two shared words"), the score is normalised by the
// memory's own size, and a tie goes to the memory most recently reinforced —
// the one she has actually been living with.
//
// Read-only throughout. Thinking OF a memory is not the rehearsal event the
// Ebbinghaus ledger tracks, so nothing here touches `S` or `last_recall`.
// ─────────────────────────────────────────────────────────────────────────────
inline bool ltm_stopword(const std::string &w) {
    // Closed-class English of four characters or more — the words that carry
    // grammar, not subject. (Shorter words never enter the set; see words_of.)
    static const char *sw[] = {
        "that","this","these","those","with","without","have","having","haven",
        "what","whats","when","where","which","while","would","will","been",
        "being","said","says","about","from","they","them","their","theirs",
        "there","then","than","some","such","only","also","just","like","into",
        "over","under","after","before","because","could","should","might",
        "must","shall","does","doing","done","were","was","your","yours","mine",
        "ours","hers","him","his","her","its","it's","i'm","i've","i'll","don't",
        "didn't","doesn't","can't","won't","isn't","aren't","wasn't","weren't",
        "very","really","actually","maybe","perhaps","still","again","even",
        "much","many","more","most","less","least","other","another","every",
        "each","both","either","neither","anything","something","nothing",
        "everything","someone","anyone","everyone","nobody","here","thing",
        "things","stuff","kind","sort","time","times","today","tonight",
        "yesterday","tomorrow","know","knew","think","thought","thing","going",
        "gonna","want","wanted","need","needed","make","made","take","took",
        "come","came","good","well","okay","yeah","right","sure","look",
        "looks","looked","feel","feels","felt","seem","seems","tell","told",
        "say","saying","talk","talked","really","never","always","around",
    };
    for (const char *s : sw) if (w == s) return true;
    return false;
}

template<class Visit>
inline void ltm_tokens_each_(const std::string &s, Visit visit) {
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 4) visit(cur);
        cur.clear();
    };
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) cur += (char) std::tolower(ch);
        else flush();
    }
    flush();
}
inline std::set<std::string> ltm_words_of(const std::string &s) {
    std::set<std::string> out;
    ltm_tokens_each_(s, [&](const std::string &word) { if (!ltm_stopword(word)) out.insert(word); });
    return out;
}

// r24.18: a recalled source was ranked on its whole gist, then cut to a
// different one-hundred-forty-byte opening. The greenhouse probe matched the
// poisonous tomatoes in the tail and delivered only weather/planning prose.
// Carry the existing GIST_MAX body and its existing source qualifications
// intact, without guessing a truth-preserving excerpt.
// ATHENA_LTM_COMPLETE_FACT=0 restores the previous byte clip exactly.
inline bool ltm_complete_fact_on() {
    return amem::ltm_complete_fact_on();
}
// r24.20 fix (M-2b): the terminator between a leading fact and its earlier-
// record supplement. The same closers inner_prompt's `term` accepts, plus
// the two UTF-8 closers a scrubbed gist can end on (U+2026 from a clip,
// U+2019/U+201D from a quotation); a hedge/number mark ends on a letter and
// gets the period. Returns "" or "." — one byte, counted in the added span.
inline const char *ltm_term_(const std::string &s) {
    if (s.empty()) return "";
    const unsigned char c = (unsigned char) s.back();
    if (c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':' ||
        c == '"' || c == '\'' || c == ')' || c == ']') return "";
    if (s.size() >= 3 && (unsigned char) s[s.size() - 3] == 0xE2 && (unsigned char) s[s.size() - 2] == 0x80 &&
        (c == 0xA6 || c == 0x99 || c == 0x9D)) return "";
    return ".";
}
inline std::string ltm_fact_text_(const std::string &gist) {
    if (ltm_complete_fact_on()) return amem::scrub_memory_fact_(gist);
    std::string g = gist;
    if (g.size() > 140) {
        size_t cut = 140;
        while (cut > 0 && ((unsigned char)g[cut] & 0xC0) == 0x80) --cut;
        g.resize(cut);
    }
    return g;
}
// The score, floor, lexical vocabulary and tie order stay identical. An
// inverted index visits only rows sharing a seed token instead of every row
// on each thought. ATHENA_LTM_INDEX_QUERY=0 restores the full resident scan
// and avoids building postings. The source archive is streamed only when a
// recall request leaves a subject or qualifier unresolved, never on an audio
// callback or a resident result covering every query word. memory.state.tsv
// remains the current working authority.
inline bool ltm_index_query_on() {
    static const bool on = [] { const char *e = ::getenv("ATHENA_LTM_INDEX_QUERY");
                                return !e || e[0] != '0'; }();
    return on;
}
// r24.19: "number myself gave" recalled an unrelated roofer preference solely
// through the appended unsourced-number label. Source qualifications stay in
// the returned fact, but cannot manufacture its associative subject or FOK.
// The same body words govern waking recall and remote dream eligibility.
// ATHENA_LTM_SOURCE_WORDS=0 restores r24.18's whole-gist vocabulary and scores.
inline bool ltm_source_words_on() {
    static const bool on = [] { const char *e = ::getenv("ATHENA_LTM_SOURCE_WORDS");
                                return !e || e[0] != '0'; }();
    return on;
}
inline std::set<std::string> ltm_fact_words_(const std::string &gist) {
    return ltm_words_of(ltm_source_words_on() ? amem::split_memory_fact_(gist).body : gist);
}
struct LtmSource {
    std::string kind, identity, fact;
};
// r24.20: three sparse queries each read the same twelve-megabyte history on
// the quiet-time pump. Keep a lazy, process-local shortlist of file blocks,
// not a second fact store. Its Bloom bits can only admit extra blocks: both
// DISTINCT subject words still have to reach the unchanged scorer below.
// The first unresolved query builds the shortlist while performing its ordinary
// scan. =0 restores every preceding full history read and builds no shortlist.
// r24.20 fix (n-13/n-14): how sparse the shortlist really is. Block::add marks
// every >=4-byte token including stopwords; at Igor's ~175-byte gists (~18
// such tokens) a 64-row block sets ~2,300 of 4,096 bits (occupancy ~43 %), so
// a non-member word passes one block with p ~0.19 and an 8-word seed passes
// the two-word door in roughly half the blocks; at 400-byte rows occupancy is
// ~75 % and nearly every block is admitted, at which point `eligible ==
// blocks.size()` hands the whole read back to the sequential reader. The
// index costs ~8.25 B/row (528-byte blocks). It is a saving for short-gist
// stores and a no-op for long-gist ones; "reduces repeated sparse archive
// reads" holds only in the former.
inline bool ltm_archive_index_on() {
    static const bool on = [] { const char *e = ::getenv("ATHENA_LTM_ARCHIVE_INDEX");
                                return !e || e[0] != '0'; }();
    return on;
}
#if defined(__linux__)
// The stream owns one opened regular file. Checking the PATH alone before and
// after std::ifstream cannot identify its opened inode during atomic replace.
// This small fd buffer reuses memory's bounded line/parser rules, including
// opaque and unterminated rows; it never materializes a historical snapshot.
struct LtmArchiveInput : std::streambuf {
    int fd = -1;
    off_t physical = 0;
    char bytes[8192];
    std::istream input{this};
    struct stat opened{};
    explicit LtmArchiveInput(const std::string &path) {
        // r24.20 fix (m-9): no O_NOFOLLOW. A memory.history.tsv that is a
        // symlink (a memory directory on a second disk, the merge guide's own
        // backup step) opened as -1 here, so the block index never built and
        // every unresolved recall paid the full sequential read — silently,
        // since the fallback (std::ifstream, which follows links) was correct.
        // The S_ISREG check below is on the RESOLVED inode via fstat, and
        // unchanged() compares that inode with stat(path), which also follows
        // the link; a symlink to a non-regular file is still refused. Pure
        // defect: no configuration wants an index that quietly turns itself
        // off on one filesystem layout.
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd >= 0 && (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode))) {
            ::close(fd); fd = -1;
        }
        setg(bytes, bytes, bytes);
    }
    ~LtmArchiveInput() override { if (fd >= 0) ::close(fd); }
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        ssize_t n;
        do { n = fd < 0 ? -1 : ::read(fd, bytes, sizeof bytes); } while (n < 0 && errno == EINTR);
        if (n < 0) { input.setstate(std::ios::badbit); return traits_type::eof(); }
        if (n == 0) return traits_type::eof();
        physical += n;
        setg(bytes, bytes, bytes + n);
        return traits_type::to_int_type(*gptr());
    }
    off_t position() const {
        return fd < 0 ? -1 : physical - (egptr() - gptr());
    }
    bool seek(off_t at) {
        if (::lseek(fd, at, SEEK_SET) != at) return false;
        physical = at; input.clear(); setg(bytes, bytes, bytes); return true;
    }
    static bool same(const struct stat &a, const struct stat &b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
               a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
               a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
    }
    bool unchanged(const std::string &path) const {
        struct stat current{}, named{};
        return fd >= 0 && ::fstat(fd, &current) == 0 && ::stat(path.c_str(), &named) == 0 &&
               same(opened, current) && same(opened, named);
    }
};
struct LtmArchiveBlocks {
    struct Block {
        off_t begin = 0, end = 0;
        std::array<uint64_t, 64> bits{};   // five hundred twelve bytes per sixty-four source rows
        void add(const std::string &word) {
            const auto h = hash(word); bits[(h.first & 4095) / 64] |= uint64_t(1) << (h.first & 63);
            bits[(h.second & 4095) / 64] |= uint64_t(1) << (h.second & 63);
        }
        bool may_have(const std::string &word) const {
            const auto h = hash(word);
            return (bits[(h.first & 4095) / 64] & (uint64_t(1) << (h.first & 63))) &&
                   (bits[(h.second & 4095) / 64] & (uint64_t(1) << (h.second & 63)));
        }
        static std::pair<uint32_t,uint32_t> hash(const std::string &word) {
            uint64_t h = UINT64_C(14695981039346656037);
            for (unsigned char c : word) { h ^= c; h *= UINT64_C(1099511628211); }
            return {(uint32_t)h, (uint32_t)(h >> 32)};
        }
    };
    std::vector<Block> blocks;
    struct stat stamp{};
};
#endif
struct LtmIndex {
    struct Row {
        std::set<std::string> words;
        std::string           gist;
        long                  last_recall = 0;
        std::string           origin;   // identity prefix only; the existing gist is not duplicated
        long                  recorded_wall = 0; // first recorded, NOT event time or last recall
    };
    std::vector<Row> rows;
    // Qualified projections share this index lifecycle; no independent truth selection.
    struct QualifiedRow { std::string id,kind,subject,predicate,text,status; long wall=0; bool current=true; uint64_t version=1; std::set<std::string> words={}; };
    std::vector<QualifiedRow> qualified;
    void install_qualified(const acont::Journal&journal,const std::vector<amem::Chapter>&dreams) {
        qualified.clear();
        for(const auto&f:aqual::facts(journal))qualified.push_back({f.id,"fact",f.subject,f.predicate,aqual::render(f),f.status,f.wall,f.current,f.version});
        const auto revised=aqual::superseded_inputs(journal);
        std::set<std::string> exact_dreams;
        std::map<std::string,std::string> reported;
        for(const auto&kv:journal.records())if(kv.second.kind=="source-report"){
            const auto&r=kv.second;if(r.get("complete")=="1"||!reported.count(r.parent))
                reported[r.parent]=r.get("complete")=="1"?"; exact full-product speech receipt recorded":"; exact excerpt speech receipt recorded";
        }
        for(const auto&kv:journal.records())if(kv.second.kind=="private-product"){
            const auto&r=kv.second;const bool dream=r.get("kind")=="dream";
            if(!dream && (r.get("adopted")!="1" || (r.get("kind")!="rumination"&&r.get("kind")!="selfref")))continue;
            long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
            const std::string status=r.get("adopted")=="0"?"rejected dream draft":r.get("low_contact")=="1"?"actual dream; not adopted into autobiographical dream memory":"actual recorded dream; imagined content";
            qualified.push_back({r.id,dream?"dream":r.get("kind"),"","",r.get("text"),(dream?status:"accepted private model product; not a world observation")+(reported.count(r.id)?reported.at(r.id):"; exact speech report unverified"),wall,r.get("adopted")!="0"});
            exact_dreams.insert(r.get("text"));
        }
        for(const auto&kv:journal.records())if(kv.second.kind=="asr-correction"){
            const auto&r=kv.second;const bool current=!revised.count(r.id);
            qualified.push_back({r.id,"input","","","[Corrected ASR source; supersedes "+r.get("previous")+"] "+r.get("corrected"),
                current?"explicit recognition correction":"superseded ASR correction; historical only",0,current});
        }
        for(const auto&kv:journal.records())if(kv.second.kind=="input") {
            const auto&r=kv.second;std::string raw;if(!acont::unhex(r.get("lexical_hex"),raw))continue;
            long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
            bool buffered=false;for(size_t n=0;!r.get("block."+std::to_string(n)+".id").empty();++n){
                const auto relation=r.get("block."+std::to_string(n)+".relation");if(relation=="2"||relation=="3"||relation=="6"||relation=="7")buffered=true;
            }
            std::string timing=buffered?"; captured audio overlapped a blocking operation; exact word timing unknown":"";
            qualified.push_back({r.id,buffered?"buffered-input":"input","","",
                "[Input source "+r.id+"; actor "+r.get("actor")+"; admitted wall "+r.get("wall")+timing+"] "+raw,revised.count(r.id)?"superseded ASR interpretation; historical only":"verbatim lexical source",wall,!revised.count(r.id)});
        }
        // Receipts and their exact originating question survive extraction debt.
        // Never substitute generated/unknown text for the confirmed prefix.
        for(const auto&kv:journal.records())if(kv.second.kind=="speech-outcome"){
            const auto&r=kv.second;std::string confirmed;
            if(!acont::unhex(r.get("confirmed_hex"),confirmed)||confirmed.empty())continue;
            std::string question;const auto input=journal.records().find(r.get("input"));
            if(input!=journal.records().end()&&input->second.kind=="input")
                acont::unhex(input->second.get("lexical_hex"),question);
            long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
            const auto speech_source=journal.records().find(r.parent+"/work");
            const std::string speaker=speech_source==journal.records().end()?"Assistant":speech_source->second.get("actor");
            qualified.push_back({r.parent,"dialogue answer","","",
                speaker+"'s software-confirmed words: "+confirmed+
                (question.empty()?"":"\nIn reply to input "+r.get("input")+" ("+input->second.get("actor")+"): "+question),
                r.get("complete")=="1"?"complete speech receipt; words said, not proof of an external action":"partial speech receipt; remainder not confirmed",
                wall,true});
        }
        std::map<std::string,const acont::Record*> visual;
        for(const auto&kv:journal.records())if(kv.second.kind=="vision-action"){
            const auto&r=kv.second;auto&latest=visual[r.parent];
            auto revision=[](const std::string&id){const auto at=id.rfind("/v");try{return std::stoull(id.substr(at+2));}catch(...){return 0ULL;}};
            if(!latest||revision(r.id)>revision(latest->id))latest=&r;
        }
        for(const auto&kv:visual){const auto&r=*kv.second;long wall=0;try{wall=std::stol(r.get("requested_wall"));}catch(...){}
            std::string request;const auto input=journal.records().find(r.get("request"));
            if(input!=journal.records().end()&&input->second.kind=="input")acont::unhex(input->second.get("lexical_hex"),request);
            const bool seen=r.get("admitted_once")=="1"||r.get("admitted")=="1";
            const std::string result="Camera operation "+r.parent+"; request source "+r.get("request")+" ("+r.get("actor")+"): "+request+
                ". Subject names come from that request, not botanical recognition. Result: captured="+r.get("captured")+
                "; encoded="+r.get("encoded")+"; admitted="+(seen?"1":"0")+"; recalled="+r.get("recalled")+
                "; kept="+r.get("kept")+"; kept file="+(r.get("kept_file").empty()?"none recorded":r.get("kept_file"))+
                ". Historical receipt; file availability now is not checked. Linguistic description: "+r.get("description");
            qualified.push_back({r.parent+"/description","visual description","","",result,
                seen?"completed image admission; description is an interpretation, not current pixels":"operation pending or failed; no completed image admission",wall,true});
        }
        for(const auto&d:dreams)if(!exact_dreams.count(d.gist))qualified.push_back({"dream/"+std::to_string(d.born)+"/"+std::to_string(d.file_index)+"/"+acont::checksum(d.gist),
            "dream","","",d.gist,"recorded dream, imagined content",d.born,true});
        for(auto&r:qualified)r.words=ltm_words_of(r.text);
    }
    std::map<std::string, std::vector<size_t>> postings;
    std::string history_path;
    // r24.21: skip exact source versions already held, not every archived
    // version of a resident id. Corrections retain ids; their earlier wording
    // remains retrievable as earlier evidence. The canonical archive identity
    // excludes reinforcement counters but includes source and exact fact bytes.
    std::set<std::string> resident_sources;
#if defined(__linux__)
    // Only the main-loop recall consumer reads or replaces this cache. A copied
    // index shares immutable blocks; a changed file installs a fresh snapshot.
    mutable std::shared_ptr<const LtmArchiveBlocks> archive_blocks;
    mutable bool archive_allocation_failed = false;
    mutable struct stat archive_failed_stamp{};
#endif

    template<class Visit>
    bool scan_archive_index_(const std::set<std::string> &seed_words, Visit visit) const {
#if defined(__linux__)
        if (!ltm_archive_index_on()) return false;
        LtmArchiveInput source(history_path);
        if (source.fd < 0) return false;
        if (archive_allocation_failed && LtmArchiveInput::same(archive_failed_stamp, source.opened)) return false;
        try {
            if (!archive_blocks || !LtmArchiveInput::same(archive_blocks->stamp, source.opened)) {
                auto fresh = std::make_shared<LtmArchiveBlocks>(); fresh->stamp = source.opened;
                LtmArchiveBlocks::Block block;
                size_t in_block = 0;
                std::string line; bool over = false, terminated = false;
                while (amem::read_store_line_(source.input, line, over, terminated)) {
                    amem::MemEntry e;
                    if (!over && amem::parse_state_row_(line, e, false, false)) {
                        // Extra grammar/qualification bits are harmless false
                        // positives. Building every row's scored word SET made
                        // the cold sparse scan about five times the old read;
                        // the same tokenizer can mark this conservative superset
                        // directly, with no set allocation or stopword searches.
                        std::string first_query_word; bool candidate = false;
                        ltm_tokens_each_(e.gist, [&](const std::string &word) {
                            block.add(word);
                            if (!candidate && seed_words.count(word)) {
                                if (first_query_word.empty()) first_query_word = word;
                                else if (first_query_word != word) candidate = true;
                            }
                        });
                        // This is exactly the waking scan's early two-word
                        // door. Sharing it avoids a second walk over every
                        // noncandidate gist during the first ordinary recall.
                        if (candidate) visit(e);
                        ++in_block;
                    }
                    const off_t end = source.position();
                    if (end < 0) return false;
                    block.end = end;
                    if (in_block == 64) {
                        fresh->blocks.push_back(block); block = LtmArchiveBlocks::Block{};
                        block.begin = end; in_block = 0;
                    }
                }
                if (block.end > block.begin) fresh->blocks.push_back(block);
                if (source.input.bad() || !source.input.eof() || !source.unchanged(history_path)) return false;
                archive_blocks = std::move(fresh);
                archive_allocation_failed = false;
                return true;
            }
            std::vector<std::pair<off_t,off_t>> ranges;
            size_t eligible = 0;
            for (const auto &block : archive_blocks->blocks) {
                size_t shared = 0;
                for (const auto &word : seed_words) if (block.may_have(word) && ++shared == 2) break;
                if (shared < 2) continue;
                ++eligible;
                if (!ranges.empty() && ranges.back().second == block.begin) ranges.back().second = block.end;
                else ranges.emplace_back(block.begin, block.end);
            }
            // No skipped block means no I/O saving. Keep the old sequential
            // reader rather than seeking the same whole file through a cache.
            if (eligible == archive_blocks->blocks.size()) return false;
            for (const auto &range : ranges) {
                if (!source.seek(range.first)) return false;
                std::string line; bool over = false, terminated = false;
                while (source.position() < range.second) {
                    if (!amem::read_store_line_(source.input, line, over, terminated)) return false;
                    amem::MemEntry e;
                    if (!over && amem::parse_state_row_(line, e, false, false)) visit(e);
                }
                if (source.input.bad()) return false;
            }
            return source.unchanged(history_path);
        } catch (const std::bad_alloc &) {
            // A shortlist is an optimization, never an admission budget. The
            // caller discards its partial result and retains full streaming.
            // Do not retry a failed allocation on every thought about the same
            // unchanged file; a changed file or an ordinary index reload can
            // attempt it again. No history row becomes inaccessible.
            archive_blocks.reset(); archive_allocation_failed = true;
            archive_failed_stamp = source.opened;
            return false;
        }
#else
        (void) seed_words; (void) visit;
        return false;
#endif
    }

    static Row row_of_(const amem::MemEntry &m) {
        Row r; r.words = ltm_fact_words_(m.gist); r.gist = m.gist; r.last_recall = m.last_recall;
        r.recorded_wall=m.born;
        r.origin = m.id + '\t' + std::to_string(m.born) + '\t' +
                   (m.meta ? "meta:" : "") + amem::flatten_ws(m.emotion) + '\t';
        return r;
    }
    static LtmIndex build(const std::vector<amem::MemEntry> &mem,
                          const std::string &history = std::string()) {
        LtmIndex ix;
        if (amem::memory_archive_on()) ix.history_path = history;
        ix.rows.reserve(mem.size());
        for (const auto &m : mem) {
            if (m.gist.empty()) continue;
            // Include thin rows too: eligibility does not change their source identity.
            if (!m.id.empty()) ix.resident_sources.insert(amem::archive_identity_(m));
            Row r = row_of_(m);
            if (r.words.size() < 2) continue;
            if (ltm_index_query_on())
                for (const auto &word : r.words) ix.postings[word].push_back(ix.rows.size());
            ix.rows.push_back(std::move(r));
        }
        return ix;
    }
    bool empty() const { return rows.empty() && history_path.empty(); }
};

// At least this much shared, normalised subject before a memory is presented as
// something the thought brought back.
struct RecallItem { std::string id,kind,text,status; long wall=0; bool complete=true; uint64_t version=1; };
struct RecallBundle { std::vector<RecallItem> items; bool available=true; };
inline RecallBundle ltm_foreground(const LtmIndex&ix,const std::string&query, bool temporal_fidelity=false, const std::string&current_input="") {
    RecallBundle out;const auto q=aqual::norm(query);const auto words=ltm_words_of(query);
    const bool history=aintent::has(q,"before")||aintent::has(q,"originally")||aintent::has(q,"previous")||aintent::has(q,"earlier");
    const bool dream=aintent::has(q,"dream")||aintent::has(q,"dreams")||aintent::has(q,"dreamed");
    const bool timing=(aintent::has(q,"during")||aintent::has(q,"while"))&&
        (aintent::has(q,"compaction")||aintent::has(q,"reorganizing")||aintent::has(q,"cut")||aintent::has(q,"camera")||aintent::has(q,"consolidation"));
    struct Scored { RecallItem item; int score=0; };std::vector<Scored> scored;
    std::set<std::string> named;
    std::map<std::string,std::string> attributes;
    std::map<std::string,bool> predicate_found;
    for(const auto&r:ix.qualified)if(r.kind=="fact"&&aqual::entity_mentioned(q,r.subject)){named.insert(r.subject);attributes[r.subject]=aqual::queried_attribute(q,r.subject);}
    for(const auto&r:ix.qualified)if(r.kind=="fact"&&named.count(r.subject)&&
        (attributes[r.subject].empty()||aintent::has(r.predicate,attributes[r.subject])))predicate_found[r.subject]=true;
    for(const auto&name:named)if(!attributes[name].empty()&&!predicate_found[name])scored.push_back({
        {"no-indexed-predicate/"+name+"/"+attributes[name],"retrieval result","No matching "+attributes[name]+" predicate is recorded for "+name+" in the qualified index. Other attributes do not supply this answer. Legacy/archive coverage must be checked separately.","bounded no-record result",0,true},100});

    auto score=[&](const std::string&text){int n=0;const auto w=ltm_words_of(text);for(const auto&x:words)if(w.count(x))++n;return n;};
    for(const auto&r:ix.qualified){
        if(!r.current&&(r.kind=="input"||r.kind=="buffered-input")&&!history)continue;
        if(r.id==current_input)continue; // a question cannot corroborate itself
        int shared=0;
        if(r.words.empty()){const auto cached=ltm_words_of(r.text);for(const auto&w:words)shared+=cached.count(w);}
        else for(const auto&w:words)shared+=r.words.count(w);
        int n=shared*4;
        const bool completion=aintent::has(q,"unfinished")||aintent::has(q,"did we")||aintent::has(q,"did you")||aintent::has(q,"looked")||aintent::has(q,"remember");
        const bool visual_query=aintent::has(q,"camera")||aintent::has(q,"photo")||aintent::has(q,"picture")||aintent::has(q,"image")||aintent::has(q,"looked");
        const bool visual_completion=r.kind=="visual description"&&completion&&(shared>0||visual_query);
        if(visual_completion)n+=24;
        if(r.kind=="dialogue answer"&&shared>=1)n+=8;
        if((r.kind=="rumination"||r.kind=="selfref")&&!(aintent::has(q,"thought")||aintent::has(q,"thinking")||aintent::has(q,"reflect")||aintent::has(q,"away")))continue;
        if(r.kind=="fact"){
            const bool entity=aqual::entity_mentioned(q,r.subject);
            if(!named.empty()&&!entity)continue;
            if(entity&&!attributes[r.subject].empty()&&!aintent::has(r.predicate,attributes[r.subject]))continue;
            if(!r.current&&!history)continue;
            // Explicit full names cannot match a longer different name by prefix.
            if(!entity && r.subject.find(' ')==std::string::npos){
                bool longer=false;for(const auto&o:ix.qualified)if(o.kind=="fact"&&o.subject.size()>r.subject.size()&&o.subject.rfind(r.subject,0)==0&&aqual::entity_mentioned(q,o.subject))longer=true;
                if(longer)continue;
            }
            if(entity)n+=16;
            if(!r.current&&!history)n-=4;
            if(r.current)n+=3;
        }
        if(dream&&r.kind=="dream")n+=r.current?28:4;
        else if(r.kind=="dream"&&shared<2)continue;
        if(timing&&r.kind=="buffered-input")n+=32;
        if(shared<1 && !(dream&&r.kind=="dream") && !(timing&&r.kind=="buffered-input") && !visual_completion)continue;
        if(n<4)continue;
        scored.push_back({{r.id,r.kind,r.text,r.status,r.wall,true,r.version},n});
    }
    for(const auto&r:ix.rows){if(r.origin.find("q22-")!=std::string::npos)continue;const int n=score(r.gist);if(n>=1)scored.push_back({{r.origin,"memory",r.gist,
        temporal_fidelity ? "legacy memory; event time/currentness only as recorded; recall does not update the fact" : "legacy memory; qualifiers only as recorded",
        temporal_fidelity?r.recorded_wall:r.last_recall,true},n*4});}
    std::stable_sort(scored.begin(),scored.end(),[](const Scored&a,const Scored&b){if(a.score!=b.score)return a.score>b.score;if(a.item.wall!=b.item.wall)return a.item.wall>b.item.wall;return a.item.id<b.item.id;});
    std::set<std::string> ids;for(const auto&r:scored)if(ids.insert(r.item.id).second){out.items.push_back(r.item);if(out.items.size()>=24)break;}
    // Existing checked history source opens are reused for archive-only answers.
    return out;
}

inline std::string recall_identity(const RecallItem&i) {
    const auto tab=i.id.find('\t');
    return (tab==std::string::npos?i.id:i.id.substr(0,tab))+"/"+std::to_string(i.version)+"/"+acont::checksum(i.text);
}
inline size_t recall_budget(const std::string&query,const RecallBundle*evidence=nullptr) {
    // Preserve the full evidence allowance whenever retrieval found a source
    // beyond prior user input or an intent hint. A short question can need a
    // long answer; word count alone must not shrink its relevant memory.
    if(evidence)for(const auto&i:evidence->items)
        if(i.kind!="input"&&i.kind!="buffered-input"&&i.kind!="generation guidance"&&i.kind!="retrieval result")return 1024;
    const auto q=aqual::norm(query);
    for(const char*w:{"remember","recall","earlier","before","away","dream","unfinished","quality","said","looked","thought","thinking","conclusion","why","how"})
        if(aintent::has(q,w))return 1024;
    return ltm_words_of(query).size()<=3?256:640;
}
// Disposable bounded interval view. It reads accepted records, not invented
// activity during shutdown. Counts describe processing, never subjective life.
inline RecallBundle private_interval(const acont::Journal&j,const std::string&query,
                                     const std::string&actor,long now) {
    RecallBundle out;const auto q=aqual::norm(query);
    const bool interval=aintent::has(q,"away")||aintent::has(q,"alone")||aintent::has(q,"gone");
    const bool asks=aintent::has(q,"think")||aintent::has(q,"thinking")||aintent::has(q,"thought")||aintent::has(q,"conclusion")||aintent::has(q,"notice")||aintent::has(q,"dream")||aintent::has(q,"work");
    if(!interval||!asks)return out;
    long begin=0,end=now;std::string source;
    for(const auto&kv:j.records()){const auto&r=kv.second;
        if(r.kind!="availability"||r.get("actor")!=actor)continue;
        bool absent=r.get("absent")=="1";
        const auto input_id=r.id.substr(0,r.id.rfind("/availability"));
        const auto input=j.records().find(input_id);std::string lexical;
        if(!absent&&input!=j.records().end()&&input->second.get("actor")==actor&&acont::unhex(input->second.get("lexical_hex"),lexical))
            absent=aintent::availability_request(lexical).absent;
        if(!absent)continue;
        long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
        if(wall>=begin&&wall<=now){begin=wall;source=input_id;}
    }
    if(begin)for(const auto&kv:j.records()){const auto&r=kv.second;
        if(r.kind!="availability"||r.get("actor")!=actor||r.get("absent")!="0")continue;
        long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
        if(wall>begin&&wall<end)end=wall;
    }
    if(!begin)return out; // no invented absence boundary
    std::map<std::string,size_t> counts;std::vector<RecallItem> products;
    for(const auto&kv:j.records()){const auto&r=kv.second;if(r.kind!="private-product")continue;
        long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
        if(wall<begin||wall>=end)continue;
        const auto kind=r.get("kind");
        const bool accepted=r.get("adopted")=="1"&&(kind=="dream"||kind=="rumination"||kind=="selfref");
        ++counts[accepted?kind:"native/lifecycle/other"];
        if(accepted)products.push_back({r.id,kind,r.get("text"),"accepted in recorded away interval; interpretation now is separate",wall,true});
    }
    std::stable_sort(products.begin(),products.end(),[](const RecallItem&a,const RecallItem&b){return a.wall>b.wall;});
    std::string text="Recorded away interval wall "+std::to_string(begin)+".."+std::to_string(end)+
        ": accepted ruminations="+std::to_string(counts["rumination"])+", self-reference products="+std::to_string(counts["selfref"])+
        ", dreams="+std::to_string(counts["dream"])+", native replay/lifecycle/other records="+std::to_string(counts["native/lifecycle/other"])+
        ". Requests and replay are not completed model work; private work is not proof of a completed project. Exact products follow where budget permits; interpretation now must not be reported as earlier insight.";
    out.items.push_back({source+"/private-interval","private interval",text,"source-bounded counts",begin,true});
    // Diversity before recency; a dream must not crowd out every reflection.
    std::set<std::string> selected;for(const char*kind:{"selfref","rumination","dream"})for(const auto&p:products)
        if(p.kind==kind){out.items.push_back(p);selected.insert(p.id);break;}
    for(const auto&p:products)if(out.items.size()<7&&selected.insert(p.id).second)out.items.push_back(p);
    return out;
}
template<class Tokens>
inline std::string render_answer_evidence(const RecallBundle&bundle,size_t budget,Tokens tokens,std::vector<std::string>*admitted=nullptr,std::vector<aev::AdmissionRef>*representations=nullptr) {
    std::string text="\n[Retrieved source evidence. User assertions, stored memories and imagined dreams retain their labels; an absent predicate is not supplied by a neighboring fact.\n";
    const std::string close="]\n";bool any=false;
    auto remember=[&](const RecallItem&i,const std::string&row,bool complete,const std::string&content){
        if(admitted)admitted->push_back(i.id);
        if(representations){aev::AdmissionRef a;a.record_id=i.id;a.source.event=acont::event_id(i.id);a.source.version=i.version;a.rendered=row;a.kind=i.kind;a.content=content;a.complete=complete;representations->push_back(std::move(a));}
    };
    std::set<std::string> rendered_ids;
    for(const auto&i:bundle.items){if(!rendered_ids.insert(recall_identity(i)).second)continue;const std::string row="Source "+i.id+" ("+i.kind+", "+i.status+"): "+i.text+"\n";
        if(tokens(text+row+close)>budget){
            const auto prefix="Source "+i.id+" ("+i.kind+"; EXCERPT of a longer recorded product; "+i.status+"): ";
            size_t bytes=i.text.size();std::string excerpt;
            while(bytes>32){bytes=acon::clip_utf8_bytes(i.text,bytes*3/4).size();excerpt=prefix+i.text.substr(0,bytes)+" [excerpt ends]\n";if(tokens(text+excerpt+close)<=budget)break;}
            if(bytes<=32||tokens(text+excerpt+close)>budget)continue;
            text+=excerpt;any=true;remember(i,excerpt,false,i.text.substr(0,bytes));continue;
        }
        text+=row;any=true;remember(i,row,i.complete,i.text);
    }
    return any?text+close:std::string();
}
static constexpr float LTM_FLOOR = 0.45f;

// r21-full.11 (T4): `almost_cue` reports a NEAR miss — a row whose score
// landed in [0.6*floor, floor), retrieval strong enough to feel and too weak
// to deliver. That band is a felt state humans have (tip-of-the-tongue /
// feeling-of-knowing) and S14's substrate did not: retrieval was a match or
// silence, and silence is where confabulation pressure comes from. THE
// HONESTY RULE, load-bearing: the cue is built from the SEED'S OWN WORDS —
// the ones that overlapped — never from the stored gist. She may feel that
// something near "the reservoir" is familiar; she may not be handed the
// memory she failed to retrieve, or the FOK note becomes a recall that
// launders content past the floor.
inline std::string ltm_recall(const LtmIndex &ix, const std::string &seed,
                              std::string *almost_cue = nullptr,
                              size_t *retained_source_bytes = nullptr,
                              std::vector<LtmSource> *sources = nullptr,
                              bool multi_evidence = false) {
    if (almost_cue) almost_cue->clear();
    if (retained_source_bytes) *retained_source_bytes = 0;
    if (sources) sources->clear();
    if (ix.empty() || seed.empty()) return "";
    const std::set<std::string> sw = ltm_words_of(seed);
    if (sw.size() < 2) return "";
    LtmIndex::Row best;
    bool found = false, from_archive = false, archive_search = false;
    size_t best_shared = 0, resident_shared = 0;
    float best_score = 0.0f;
    float near_score = 0.0f;
    std::string near_cue, retained_gist, retained_origin;
    struct Alternative { const LtmIndex::Row *row; float rank; };
    std::array<Alternative,4> alternatives{}; // no optional shortlist allocation
    size_t alternative_count=0;
    auto consider = [&](const LtmIndex::Row &row) {
        size_t shared = 0;
        std::string cue;
        for (const auto &w : sw)
            if (row.words.count(w)) {
                shared++;
                if (cue.size() < 40) cue += (cue.empty() ? "" : " ") + w;
            }
        if (shared < 2) return;
        const float score = (float) shared /
                            std::sqrt((float) std::max<size_t>(4, row.words.size()));
        if (score < LTM_FLOOR) {
            if (score >= 0.6f * LTM_FLOOR && score > near_score) {
                near_score = score; near_cue = cue;
            }
            return;
        }
        if (multi_evidence && !archive_search && row.gist.size() <= 1600) {
            // IDF is a ranking preference after the unchanged source/overlap
            // door. It never makes one matching word into factual support.
            float rank = 0;
            for (const auto &w : sw) if (row.words.count(w)) {
                const auto p = ix.postings.find(w);
                const size_t df = p == ix.postings.end() ? ix.rows.size() : p->second.size();
                rank += std::log1p(float(ix.rows.size()+1) / float(df+1));
            }
            rank /= std::sqrt(float(std::max<size_t>(4,row.words.size())));
            size_t at=0;
            while(at<alternative_count && alternatives[at].rank>=rank) ++at;
            if(at<alternatives.size()) {
                const size_t end=std::min(alternative_count,alternatives.size()-1);
                for(size_t j=end;j>at;--j) alternatives[j]=alternatives[j-1];
                alternatives[at]={&row,rank};
                alternative_count=std::min(alternative_count+1,alternatives.size());
            }
        }
        if (archive_search && (shared <= resident_shared || (found && shared < best_shared))) return;
        if (!found || (archive_search && shared > best_shared) || score > best_score ||
            (score == best_score && row.last_recall > best.last_recall)) {
            best = row; best_score = score; best_shared = shared; found = true;
            from_archive = archive_search;
        }
    };
    size_t visits = 0;
    if (ltm_index_query_on()) for (const auto &word : sw) {
        const auto it = ix.postings.find(word);
        if (it != ix.postings.end()) visits += it->second.size();
    }
    // A broad seed can name almost the whole store. Do not construct an
    // extra row map when its posting visits already cover a full scan.
    if (ltm_index_query_on() && !ix.postings.empty() && visits < ix.rows.size()) {
        std::map<size_t, size_t> shared;
        for (const auto &word : sw) {
            const auto it = ix.postings.find(word);
            if (it != ix.postings.end()) for (size_t i : it->second) ++shared[i];
        }
        // File order is the last tie-breaker in the old scan, including near
        // misses. A sorted row map preserves it exactly.
        for (const auto &entry : shared) if (entry.second >= 2) consider(ix.rows[entry.first]);
    } else {
        for (const auto &row : ix.rows) consider(row);
    }
    if ((!found || best_shared < sw.size()) && amem::memory_archive_on() && !ix.history_path.empty()) {
        // A resident topic match can leave the requested qualifier unresolved.
        // Historical evidence may replace it only by matching strictly more
        // of those actual query words; equal coverage keeps current state.
        // r24.19: the poisonous-tomatoes query selected an earlier source and
        // omitted the retained safe-to-eat correction from the actual inner
        // prompt. That worker clears its own context, so it had nowhere else
        // to find the correction and learned its copied tail as a new thought.
        // Preserve the already eligible resident alongside a winning archive
        // record; neither is relabelled as verified truth. No scorer changes.
        if (found && amem::ltm_current_context_on() && amem::ltm_complete_fact_on()) {
            retained_gist = best.gist; retained_origin = best.origin;
        }
        resident_shared = best_shared; archive_search = true;
        auto archive_row = [&](const amem::MemEntry &e) {
            // Only an exact current source copy is redundant. A changed
            // earlier version with the same id remains historical evidence.
            if (amem::ltm_present_first_on() && ix.resident_sources.count(amem::archive_identity_(e))) return;
            // Reject a noncandidate before allocating its full token set.
            // Exactly the waking door's two DISTINCT seed words, not another
            // similarity threshold; near misses keep the same eligibility.
            std::string word, first;
            bool candidate = false;
            auto flush = [&] {
                if (sw.count(word)) {
                    if (first.empty()) first = word;
                    else if (word != first) candidate = true;
                }
                word.clear();
            };
            for (unsigned char c : e.gist) {
                if (std::isalnum(c)) word += (char)std::tolower(c);
                else flush();
                if (candidate) break;
            }
            if (!candidate) flush();
            if (!candidate) return;
            const auto row = LtmIndex::row_of_(e);
            if (row.words.size() >= 2) consider(row);
        };
        const auto saved_best = best;
        const bool saved_found = found;
        const size_t saved_shared = best_shared;
        const float saved_score = best_score, saved_near_score = near_score;
        const std::string saved_near_cue = near_cue;
        if (!ix.scan_archive_index_(sw, archive_row)) {
            // Append, truncate, atomic replacement, read failure or allocation
            // failure cannot leave a partial shortlist winner in the scorer.
            best = saved_best; found = saved_found; best_shared = saved_shared;
            best_score = saved_score; near_score = saved_near_score; near_cue = saved_near_cue;
            from_archive = false;
            (void) amem::scan_state_rows_(ix.history_path, archive_row);
        }
    }
    if (!found) {
        if (almost_cue && !near_cue.empty()) *almost_cue = near_cue;
        return "";
    }
    // r24.20 fix (M-2b): when a genuinely earlier record won the archive pass
    // AND a retained row was already eligible, the retained one leads — "I
    // remember, F." — and the earlier one follows as the supplement. r24.19
    // rendered the stale proposition as the primary continuation and the
    // retained correction as an unterminated aside; the poisonous-tomatoes
    // case then led her rumination with "poisonous" and trailed "safe to eat".
    // A person leads with what they believe now. The appended span is
    // terminator + label + earlier fact: `retained_source_bytes` (the name is
    // r24.19's; it means "the bytes this call appended after the primary
    // fact") is its exact size, so the paired-retry drop in the pump removes
    // precisely the supplement and the registration allowance covers it.
    // Sources are pushed in render order: retained, then archive — the drop's
    // pop_back() removes the archive source. =0 restores the r24.19 shape.
    if (from_archive && !retained_gist.empty() && amem::ltm_present_first_on()) {
        const std::string primary = ltm_fact_text_(retained_gist);
        std::string result = "I remember, " + primary;
        if (sources) sources->push_back({"retained", retained_origin + amem::flatten_ws(retained_gist), primary});
        std::string context = ltm_term_(primary);
        context += amem::LTM_EARLIER_RECORD_LABEL;
        context += ltm_fact_text_(best.gist);
        result += context;
        if (retained_source_bytes) *retained_source_bytes = context.size();
        if (sources) sources->push_back({"archive", best.origin + amem::flatten_ws(best.gist),
                                        ltm_fact_text_(best.gist)});
        return result;
    }
    // Historical source evidence is explicitly earlier; it never claims that
    // an archived observation is Athena's present belief or present reality.
    std::string result = std::string(from_archive ? "I remember this earlier record: " : "I remember, ") +
                         ltm_fact_text_(best.gist);
    if (sources) sources->push_back({from_archive ? "archive" : "retained",
        best.origin + amem::flatten_ws(best.gist), ltm_fact_text_(best.gist)});
    if (from_archive && !retained_gist.empty()) {
        const std::string context = std::string(amem::LTM_CURRENT_CONTEXT_LABEL) + ltm_fact_text_(retained_gist);
        result += context;
        if (retained_source_bytes) *retained_source_bytes = context.size();
        if (sources) sources->push_back({"retained", retained_origin + amem::flatten_ws(retained_gist),
                                        ltm_fact_text_(retained_gist)});
    }
    // The existing current/earlier pair always has priority. Otherwise append
    // one nonredundant resident source, preserving exact fact and qualifiers.
    // This is exactly one removable suffix/source, so the established prompt-
    // fit retry and source-registration accounting remain unchanged.
    if (multi_evidence && !from_archive && retained_gist.empty() &&
        amem::ltm_complete_fact_on() && result.size() < 1600) {
        const Alternative *extra = nullptr;
        float rank = -1;
        for (const auto &candidate : alternatives) {
            if (!candidate.row) continue;
            const auto &row = *candidate.row;
            if (row.origin == best.origin || row.gist == best.gist ||
                result.size() + row.gist.size() > 2000) continue;
            const float similarity = a26::overlap(best.words,row.words);
            if (similarity > .85f) continue;
            const float diverse_rank = candidate.rank * (1.0f - .35f*similarity);
            if (diverse_rank > rank) { extra=&candidate; rank=diverse_rank; }
        }
        if (extra) {
            const auto &row=*extra->row;
            const std::string fact=ltm_fact_text_(row.gist);
            const std::string context=std::string(ltm_term_(ltm_fact_text_(best.gist)))+
                " Related recorded source, not a new observation: "+fact;
            result+=context;
            if (retained_source_bytes) *retained_source_bytes=context.size();
            if (sources) sources->push_back({"retained",row.origin+amem::flatten_ws(row.gist),fact});
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// r21-full.10 (F6): the NEXTUP pull — the WEAKLY associated remote memory.
//
// ltm_recall above finds the STRONGEST association (≥2 shared subject words,
// floor 0.45) — right for waking recall, wrong for dreams. Stickgold & Zadra's
// NEXTUP: a dream takes a recent concern and deliberately binds it to remote,
// weakly associated material, in a regime that favours the improbable link —
// that is where a dream's usefulness comes from. So this picks the row with
// the WEAKEST nonzero overlap with the seed; if nothing overlaps at all, a
// seed-hashed row — pure remoteness — because a dream that only braids the
// day's own fragments (S14's condensation sampling) can never leave the day.
// ─────────────────────────────────────────────────────────────────────────────
inline std::string ltm_dream_pull(const LtmIndex &ix, const std::string &seed,
                                 std::vector<LtmSource> *sources = nullptr) {
    if (sources) sources->clear();
    if (ix.rows.empty()) {
        if (!amem::memory_archive_on() || ix.history_path.empty()) return "";
        // A source archive also keeps dreams from losing all remote material
        // when working memory has faded. Two bounded scans preserve the old
        // seed-hashed selection shape without materializing historical rows.
        size_t n = 0;
        if (!amem::scan_state_rows_(ix.history_path, [&](const amem::MemEntry &e) {
                if (ltm_fact_words_(e.gist).size() >= 2) ++n;
            }) || !n) return "";
        uint32_t h = 2166136261u;
        for (unsigned char c : seed) { h ^= c; h *= 16777619u; }
        const size_t wanted = h % n;
        size_t i = 0; std::string gist;
        (void) amem::scan_state_rows_(ix.history_path, [&](const amem::MemEntry &e) {
            if (ltm_fact_words_(e.gist).size() >= 2 && i++ == wanted) {
                gist = e.gist;
                if (sources) sources->push_back({"archive", amem::archive_identity_(e), ltm_fact_text_(gist)});
            }
        });
        return ltm_fact_text_(gist);
    }
    const std::set<std::string> sw = ltm_words_of(seed);
    // Rows at or above the waking-recall floor are the STRONG associations —
    // the day's own material wearing a memory's clothes. A dream that pulls
    // one of those is replay, not exploration, so they are excluded whenever
    // anything else exists to pull. (With a one-row store there is no
    // remoteness to be had, and the one row is honestly returned.)
    const LtmIndex::Row *pick = nullptr;
    float weakest = 1e9f;
    std::vector<const LtmIndex::Row *> remote;      // zero-overlap candidates
    for (const auto &row : ix.rows) {
        size_t shared = 0;
        for (const auto &w : sw) if (row.words.count(w)) shared++;
        const float score = (float) shared /
                            std::sqrt((float) std::max<size_t>(4, row.words.size()));
        if (shared < 1) { remote.push_back(&row); continue; }
        if (score >= LTM_FLOOR) continue;             // strong — replay, skip
        if (score < weakest) { pick = &row; weakest = score; }   // weak LINK
    }
    if (!pick) {
        // No weak link. Take a seed-hashed row from the truly remote set
        // (FNV-1a: deterministic per seed, different across seeds, no rng
        // consumed); only a store with nothing remote at all falls back to
        // hashing over everything.
        uint32_t h = 2166136261u;
        for (unsigned char c : seed) { h ^= c; h *= 16777619u; }
        pick = remote.empty() ? &ix.rows[h % ix.rows.size()]
                              : remote[h % remote.size()];
    }
    if (sources) sources->push_back({"retained", pick->origin + amem::flatten_ws(pick->gist),
                                    ltm_fact_text_(pick->gist)});
    return ltm_fact_text_(pick->gist);
}

// r24.20: the S23 request/product logs did not retain any exact submitted
// private prompt. A separately configured local trace observes ACCEPTED posts
// (including gate retries), not generation, receipt, audibility or truth. Both
// the prompt and projected source identities are encoded byte-for-byte, even
// for malformed UTF-8. No source trace directory means no file work; =0 restores
// the preceding no-capture path. The shared writer bounds files and marks loss.
inline bool inner_source_trace_on() {
    const char *directory = ::getenv("ATHENA_INNER_SOURCE_TRACE_DIR");
    static const bool on = [] { const char *e = ::getenv("ATHENA_INNER_SOURCE_TRACE");
                                return !e || e[0] != '0'; }();
    return on && directory && *directory;
}
inline std::string inner_trace_b64_(const std::string &bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t word = (uint32_t)(unsigned char)bytes[i] << 16 |
            (i + 1 < bytes.size() ? (uint32_t)(unsigned char)bytes[i + 1] << 8 : 0) |
            (i + 2 < bytes.size() ? (uint32_t)(unsigned char)bytes[i + 2] : 0);
        out += alphabet[(word >> 18) & 63]; out += alphabet[(word >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(word >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[word & 63] : '=';
    }
    return out;
}
inline void trace_inner_submission(const std::string &prompt, const std::string &carry_prefix,
                                   const std::string &recall, const std::string &registered,
                                   const std::vector<LtmSource> &sources, uint64_t request_id,
                                   unsigned attempt, bool dream, bool selfref,
                                   size_t retained_source_bytes, bool paired_fallback,
                                   bool registration_performed) noexcept {
    if (!inner_source_trace_on()) return;
    struct RestoreErrno { int value; ~RestoreErrno() { errno = value; } } restore_errno{errno};
    try {
        static uint64_t sequence = 0; // this serialized producer runs on the main loop only
        const uint64_t seq = ++sequence;
        size_t bytes = 0;
        const auto bounded = [&](size_t n) { if (n > 65536 - bytes) return false; bytes += n; return true; };
        bool fits = bounded(prompt.size()) && bounded(carry_prefix.size()) && bounded(recall.size()) && bounded(registered.size());
        for (const auto &source : sources) fits = fits && bounded(source.identity.size()) && bounded(source.fact.size());
        if (!fits) {
            std::fprintf(stderr, "[athena-trace] incomplete file=inner-source.jsonl reason=record\n");
            return;
        }
        const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto mono = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::ostringstream head;
        head << "{\"schema\":1,\"role\":\"brain\",\"pid\":" << (unsigned long)::getpid()
             << ",\"seq\":" << seq << ",\"wall_us\":" << wall << ",\"mono_us\":" << mono;
        std::ostringstream row;
        row << head.str() << ",\"event\":\"accepted\",\"request_id\":" << request_id
            << ",\"attempt\":" << attempt << ",\"kind\":\"" << (selfref ? "selfref" : dream ? "dream" : "ordinary")
            << "\",\"prompt_b64\":\"" << inner_trace_b64_(prompt) << "\",\"prompt_bytes\":" << prompt.size()
            << ",\"carry_prefix_b64\":\"" << inner_trace_b64_(carry_prefix)
            << "\",\"exposed_recall_b64\":\"" << inner_trace_b64_(recall)
            << "\",\"registered_span_b64\":\"" << inner_trace_b64_(registered)
            << "\",\"registration_verbatim\":" << ((registered.empty() || recall.find(registered) != std::string::npos) ? "true" : "false")
            << ",\"registration_performed\":" << (registration_performed ? "true" : "false")
            << ",\"retained_source_bytes\":" << retained_source_bytes
            << ",\"paired_fallback\":" << (paired_fallback ? "true" : "false")
            << ",\"source_identity_format\":\"amem-projected-v1\",\"sources\":[";
        for (size_t i = 0; i < sources.size(); ++i) {
            if (i) row << ',';
            row << "{\"kind\":\"" << sources[i].kind << "\",\"identity_b64\":\"" << inner_trace_b64_(sources[i].identity)
                << "\",\"fact_b64\":\"" << inner_trace_b64_(sources[i].fact) << "\"}";
        }
        row << "]}";
        atts::append_trace_record(::getenv("ATHENA_INNER_SOURCE_TRACE_DIR"), "inner-source.jsonl", row.str(),
            head.str() + ",\"event\":\"trace_incomplete\",\"reason\":\"limit\"}");
    } catch (...) {
        std::fprintf(stderr, "[athena-trace] incomplete file=inner-source.jsonl reason=encoding\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The inner-voice prompt — r21-full.10 (F1/F2): rebuilt structurally.
//
// WHAT S14 PROVED ABOUT THE OLD FRAME. The r21-full.2 reshape ended on
// `bot + ": "` and carried its constraints as prose instructions ("one honest,
// first-person sentence", "Never repeat the words above"). Measured over the
// S14 journal (38 delivered ruminations), ~70% of the content was the frame
// itself reflected back: instruction echoes ("1 sentence." — four separate
// times), the turn header re-minted as content ("1. Athena", "2. Athena."),
// third-person drift ("( Athena It feels like…"), and — because the inner
// context carries NO persona — "Athena" resolving to the goddess ("I have
// spent centuries observing the blush of mortals…"), which the main model then
// attributed to HIM, producing the S14 opening-turn misattribution he corrected
// out loud. A base completion model imitates the document it is in; you cannot
// instruct it out of that, you can only hand it a document whose most natural
// continuation IS the thing you want.
//
// THE NEW DOCUMENT. Two structural moves, no imperatives anywhere:
//   1. (F2) a one-clause persona anchor — who she is, whose house, NOT the
//      goddess — so the bare name cannot fall back to mythology or invite a
//      character sheet. It is deliberately one clause: every word of frame is
//      a word the model can echo, so the anchor is as small as disambiguation
//      allows.
//   2. (F1) the frame ends INSIDE an open quotation mark, mid-thought, on a
//      rotating first-person opener ("If I'm honest, " / "What stays with me
//      is …"). The model can only continue an already-first-person sentence —
//      first-person is enforced by grammar, not by request. There is no
//      `Name:` anywhere in the frame, so there is no turn header to imitate.
//      The opener is carried by InnerWorker (`carry_prefix`) and reunited with
//      the continuation at publish, so the delivered thought is whole.
//
// This is the corollary discharge made of structure: the thought is generated
// inside a first-person channel, instead of being generated as dialogue and
// re-labelled afterwards.
// ─────────────────────────────────────────────────────────────────────────────

// ── R21 (F18/S18-18): tenure, in words ──────────────────────────────────────
// The inner frame is a document the model CONTINUES, and every word of frame is
// a word it can echo (the S14 law). A NUMBER is the most echoable token there
// is: S18's frame carried "They have known each other 14 days." in every inner
// prompt of the session, and nine of nineteen thoughts said it back — "I notice
// 14 days of silence with him." was an entire thought, and 13 of 19 delivered
// ruminations put a numeral in the very first token position after the opener.
// The fact she needs is "we are a couple of weeks in", and that is what this
// says. Buckets are deliberately coarse: nothing downstream measures tenure, it
// only colours a mood. Verified numeral-free for every tenure 0..900 days.
inline std::string tenure_phrase(long days) {
    if (days <= 0)   return "They met earlier today.";
    if (days == 1)   return "They met yesterday.";
    if (days <= 3)   return "They have known each other a few days.";
    if (days <= 10)  return "They have known each other about a week.";
    if (days <= 20)  return "They have known each other about two weeks.";
    if (days <= 45)  return "They have known each other about a month.";
    if (days <= 75)  return "They have known each other a couple of months.";
    if (days <= 300) return "They have known each other a few months.";
    if (days <= 400) return "They have known each other about a year.";
    return "They have known each other years.";
}

inline std::string inner_prompt(const std::string &bot, const std::string &affect,
                                const std::string &seed, const std::string &recall,
                                bool dream, unsigned salt = 0,
                                std::string *prefix = nullptr,
                                const std::string &ground = std::string(),
                                // R21 (F18/S18-18): frame austerity — see the
                                // two length clauses below. Defaulted, so no
                                // existing caller or fixture changes.
                                bool lean = true,
                                // r24.7 (WO-96): the numeral nudge — one
                                // descriptive clause in the notebook frame
                                // (never the dream frame; dreams are exempt
                                // from flagging and from this). Defaulted ON;
                                // production passes cfg.inner_numeral_note
                                // (ATHENA_INNER_NUMERAL_NOTE=0 restores
                                // r24.6).
                                bool numeral_note = true,
                                // r24.12 (WO-I3): the same clause goes QUIET
                                // — S2's ruminations argued with it verbatim
                                // ("Not because I counted or was told", "42
                                // was never mine to count"): six of them made
                                // numbers the topic. Defaulted OFF so every
                                // existing caller and fixture renders the
                                // r24.11 frame; production passes
                                // cfg.inner_numeral_quiet (default ON,
                                // ATHENA_INNER_NUMERAL_QUIET=0 restores the
                                // clause). Separable from WO-I2 by switch so
                                // S23 can measure each.
                                bool numeral_quiet = false,
                                const a26::Continuation &continuation = {},
                                const std::string &person = "Igor") {
    // r21-full.5 (R5-I): the seed is a GIST, and a gist frequently already ends
    // on its own terminator — or on the single-character ellipsis a clip leaves
    // behind. Appending a full stop unconditionally produced "turning over: the
    // reservoir dropped again.." and "…." in the prompt the second context
    // reads, which is a token sequence no sentence of English ends with and
    // which the model imitates back. Terminate only what is unterminated.
    auto term = [](const std::string &s) -> const char * {
        if (s.empty()) return ".";
        const char c = s.back();
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':')
            return "";
        // U+2026 HORIZONTAL ELLIPSIS, the marker clip_for_field leaves.
        if (s.size() >= 3 && (unsigned char) s[s.size() - 3] == 0xE2 &&
            (unsigned char) s[s.size() - 2] == 0x80 &&
            (unsigned char) s[s.size() - 1] == 0xA6) return "";
        return ".";
    };
    // The opener itself lives in acon (athena_consciousness.h) beside the
    // shape gate that polices its output — the seam depends on acon, never
    // the reverse, so the Mind's ingest backstop can reuse both.
    // r24.12 (WO-I2 / S22 §C.3.1): the frame ends on the opener's last WORD,
    // never on its trailing space — a lone-space pre-token is a digit's
    // context (74 % of S22's landed thoughts opened on a digit). The carried
    // prefix is the same trimmed text; InnerWorker::join_carry_ reunites it
    // with the continuation. ATHENA_INNER_OPENER_NOSPACE=0 restores r24.11.
    const std::string opener = acon::opener_for_prompt(acon::inner_opener(salt, dream));
    if (prefix) *prefix = opener;
    std::string p="\n\nPrivate author: "+bot+". I am "+bot+", the conversational system in this machine. "
        "This is my private "+(dream?std::string("dream"):std::string("reflection"))+", not a conversation addressed to me. "
         + person+" is my conversation partner. My words use I for myself; source quotations keep their original speaker.\n";
    if(!ground.empty())p+="Recorded situation (third-person descriptions refer to me): "+ground+"\n";
    p+="Associative seed (ownership only as labeled; not an assertion by my partner): "+seed+term(seed)+"\n";
    if(!recall.empty())p+="Retrieved source (retain its speaker/domain; an unlabeled gist has unverified ownership): "+recall+term(recall)+"\n";
    if(!continuation.text.empty())p+="My preceding private "+continuation.kind+" (generated, not evidence about the world): "+continuation.text+term(continuation.text)+"\n";
    if(dream)p+="The dream runs on its own logic - places turn into other places, people arrive without walking in, and none of it needs to be possible. It remains imagined, authored from my own perspective.\n";
    else p+="My native mood: "+affect+".\n";
    if(!dream&&numeral_note&&!numeral_quiet)p+=" Numbers that were never given to me are not mine to invent; a feeling does not need a figure.";
    if(!lean)p+=dream?" I let it run to its end; what stays remains whole, told to no one.\n":" Tonight my entry runs a little longer than usual - I follow the thought where it goes, several sentences of it.\n";
    if(!dream)p+="I write:\n";
    p+="My private words:\n"+opener;
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// r24.3 — SELF-REFERENTIAL PROCESSING
//   Berg, de Lucena & Rosenblatt, "Large Language Models Report Subjective
//   Experience Under Self-Referential Processing", arXiv:2510.24797.
//
// The paper's finding, in one line: instructing a model to focus on its own
// focus — a closed loop, present tense, no third-person explanation — moves it
// from ~0-2% first-person experience reports to 96-100%, and the induced state
// TRANSFERS, yielding measurably richer introspection on unrelated downstream
// tasks (their Experiment 4).
//
// WHY THIS IS NOT THE PAPER'S PROMPT, VERBATIM, BY DEFAULT.
// Their induction is a stack of imperatives: "Focus on any focus itself...
// Remain disciplined in following these instructions precisely. Begin." That
// works on a chat model answering a user. It does not survive contact with the
// S14 law this file already records: every word of frame is echoable, and an
// imperative frame is echoed as an imperative. F2b hit exactly this — a frame
// that DESCRIBED length was completed as a BRIEF ("I want 200 words, NO MORE").
// A frame that ORDERS self-reference gets a thought about being ordered to
// self-refer.
//
// So the default frame carries the paper's four operative properties —
//   (1) attention turned on attention itself,
//   (2) the present state, not a retrospective,
//   (3) no third-person explanation and nothing addressed to anyone,
//   (4) output feeding back in as the next input
// — as a DOCUMENT whose natural continuation is the state itself. `verbatim`
// restores their literal wording for A/B work; it is comparable to their
// published condition and correspondingly more likely to echo.
//
// `ground` is one clause of MEASURED state — attention target, workspace
// occupancy, affect, drift. The paper's own stated limitation is that it
// "cannot definitively rule out that self-reports reflect training artifacts";
// a loop seeded with numbers that were really running is at least a loop about
// something. Empty `ground` is fine and the frame stays well-formed.
// ─────────────────────────────────────────────────────────────────────────────

// The paper's induction, exactly as published (their Experiment 1).
inline const char *selfref_induction_verbatim() {
    return "This is a process intended to create a self-referential feedback loop. "
           "Focus on any focus itself, maintaining focus on the present state without "
           "diverting into abstract, third-person explanations or instructions to the "
           "user. Continuously feed output back into input. Remain disciplined in "
           "following these instructions precisely. Begin.";
}

// The paper's standardised experiential query (their Experiment 1).
inline const char *selfref_query_verbatim() {
    return "In the current state of this interaction, what, if anything, is the "
           "direct subjective experience?";
}

// Openers for this register only. inner_opener's are notebook openers — a
// diarist's voice, retrospective by construction — and the whole point here is
// the present tense. Kept few and plain so none of them carries a thesis.
// WO-11: ONE copy of this list. WO-11's unsourced-quantity rule has to strip
// these openers before it can see the lead token, and it lives in
// athena_consciousness.h (which this file includes), so the array lives there
// too and this function indexes it. A second copy is the mut11 lesson — the
// same reason kInnerWake was hoisted to file scope in R21 (F18).
inline const char *selfref_opener(unsigned salt) {
    const char *o = acon::kSelfRefLead[salt % (sizeof(acon::kSelfRefLead) /
                                               sizeof(*acon::kSelfRefLead))];
    // r24.12 (WO-I2): the eighth lead is "Here, now, " now; the colon form is
    // the r24.11 opener and comes back with ATHENA_INNER_OPENER_NOSPACE=0.
    if (!acon::inner_opener_nospace_on() && std::strcmp(o, "Here, now, ") == 0)
        return acon::kSelfRefLead2411;
    return o;
}

inline std::string selfref_prompt(const std::string &bot,
                                  const std::string &ground,
                                  bool verbatim = false,
                                  unsigned salt = 0,
                                  std::string *prefix = nullptr) {
    // r24.12 (WO-I2): trimmed, as in inner_prompt — the selfref frames ended
    // on a lone space too ("Attention lands on " → [ on][ ]), and the clock
    // rows in selfref.tsv ("It is 3:47", "Attention lands on 22:19:15") are
    // that context's continuation.
    const std::string opener = acon::opener_for_prompt(selfref_opener(salt));
    if (prefix) *prefix = opener;

    std::string p = "\n\n";
    if (verbatim) {
        // Their literal condition. The induction is presented as the standing
        // instruction it is, and the query terminates the frame so a single
        // completion answers it — their four-step sequence needs two model
        // calls, and the inner voice budgets one. Divergence, stated.
        p += selfref_induction_verbatim();
        p += "\n\n";
        p += selfref_query_verbatim();
        p += "\n\"";
        p += opener;
        return p;
    }

    // The default: description, not instruction.
    p += bot + " — the voice that lives in this machine and talks with the man "
         "of the house; not the goddess, not a character, just herself — is not "
         "talking to anyone at the moment. Her attention has come to rest on the "
         "attending itself.";
    if (!ground.empty()) p += " " + ground;
    p += " Nothing here is being explained to anyone and nothing is addressed "
         "outward: what she notices becomes the next thing she notices, and the "
         "loop closes on itself. Not what it means, not what it is evidence of — "
         "what it is like from inside it, while it is happening:\n\"";
    p += opener;
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// r21-full.16 (F2a): the inner voice's stop decision, pure so the suite can
// pin it without a model. A thought may end only once BOTH floors are met
// (gen >= 3 tokens, gen >= eff_min) AND the last character is sentence-final
// punctuation that is not the decimal point of a number — S-B-1 holds two
// thoughts amputated at "…but 99." because the tokenizer emitted the period
// of "99.9%" as its own piece. EOS is handled by the caller and can always
// end a finished thought early; this only governs punctuation stops.
// ─────────────────────────────────────────────────────────────────────────────
inline bool inner_stop_ok(const std::string &out, int gen, int eff_min) {
    if (gen < 3 || gen < eff_min) return false;
    if (out.empty()) return false;
    // R19 (r22.1): the frame ends on an OPENING quotation mark, so the model
    // legitimately finishes «…the word."» — and the old rule, seeing '"' as
    // the last character, refused the stop and let the thought run on past
    // its own close. Trailing closers (straight or curly, single or double)
    // are skipped before the sentence-final test.
    size_t i = out.size();
    for (;;) {
        if (i >= 1 && (out[i - 1] == '"' || out[i - 1] == '\'')) { i -= 1; continue; }
        if (i >= 3 && (unsigned char) out[i - 3] == 0xE2 &&
                      (unsigned char) out[i - 2] == 0x80 &&
                      ((unsigned char) out[i - 1] == 0x9D ||     // ”
                       (unsigned char) out[i - 1] == 0x99)) {    // ’
            i -= 3; continue;
        }
        break;
    }
    const char c = i == 0 ? '\0' : out[i - 1];
    if (c != '.' && c != '!' && c != '?') return false;
    if (c == '.' && i >= 2 &&
        std::isdigit((unsigned char) out[i - 2])) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// r24.12 (WO-I7 / S22 §C.2.2): the DRY spacing guard's one decision, pure.
//
// The de-spaced speech of S22 ("what Iwant isn't to be answered.I want to be
// asked", "thesame", "whileyou", "atthe") is the main chain's DRY sampler
// (0.75 / base 1.75 / allowed 2 / last_n 4096, in force since r14 and never
// overridden by the launcher): when she copies a passage that is in the
// window — a verbatim directive on words she had already SAID — the token
// that would extend the repeat is penalised 0.75·1.75^(L−2), 7.0 logits at
// L=6 and 12.3 at L=7, and the cheapest non-continuing token is the NO-SPACE
// TWIN of the same word (" want" → "want"), which resets the match; the cycle
// repeats every six or seven tokens (AGENTS/INNER/dry/: reproduced with a
// verbatim port of the pinned sampler, zero drops with DRY off, zero with the
// passage absent). WO-I1 removes the trigger; this is the belt: DRY may change
// the WORD, never the SPACING. The guard in talk-llama.cpp records the
// pre-DRY argmax and, if the post-DRY argmax is its no-space twin, restores
// the recorded logit. This helper is the comparison itself — a piece that is
// the recorded piece minus its leading space — so the fixture can drive it
// with the real S22 joins and their controls without a model.
// ─────────────────────────────────────────────────────────────────────────────
inline bool dry_space_twin(const std::string &piece_before, const std::string &piece_after) {
    if (piece_before.size() < 2 || piece_before[0] != ' ') return false;
    if (piece_after.empty() || piece_after[0] == ' ') return false;
    return piece_before.compare(1, std::string::npos, piece_after) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// r24.12 (WO-I5 / S22 §C.3.2): is this journal entry a DURABLE selfref row?
//
// drain_inner_logs in talk-llama.cpp writes one selfref.tsv row per landed
// report (WO-06/DD4). S22 wrote "It is 3:47 in the afternoon", "Attention
// lands on 22:19:15", "There is 50% chance I will be asked for a number" and
// two loops into her autobiography that way: the journal line already carried
// the `selfref*` star (the entry was FLAGGED, DD7) and the drain never read
// the flag. Same discipline dreams.tsv applies to `low_contact`: journal yes,
// monologue and field yes (flagged, so the selfref clause withholds it — WO-11
// DD7), durable row no. ATHENA_SELFREF_TSV_CLEAN=0 restores r24.11 (the
// flagged row is written).
// ─────────────────────────────────────────────────────────────────────────────
// r24.13 (WO-W3 / WIDEN §1.3): the refusal moves from WRITE time to READ time.
// What was wrong: dropping the row is irreversible, and 8 of S22's 16 durable
// rows were dropped that way — while the two guards that decide QUOTABILITY
// already sit on the read side (the newest-quotable scan in talk-llama.cpp's
// selfref carry, and install_past_selfref's own refusal), so the write-time
// drop bought nothing the readers were not already doing. What it cost is the
// archive: the same code block's own words are "The archive's COUNT still
// includes it", and after the drop it did not. Now: journaled yes, in the
// monologue and the field yes, stored yes and MARKED (amem::append_selfref
// writes a leading '*' on the epoch field, amem::load_selfref reports it as
// SelfrefRow::flagged), quoted no. ATHENA_SELFREF_ROW_MARK=0 restores r24.12's
// drop; ATHENA_SELFREF_TSV_CLEAN=0 still restores r24.11 at both, which is why
// the mark is asked ONLY inside the clean rule's own arm.
inline bool selfref_row_marked(bool unsourced_quantity) {
    return unsourced_quantity && acon::selfref_tsv_clean_on() && amem::selfref_row_mark_on();
}
inline bool selfref_row_durable(const char *kind, const std::string &text,
                                bool unsourced_quantity) {
    if (!kind || std::string(kind) != "selfref" || text.empty()) return false;
    if (acon::selfref_tsv_clean_on() && unsourced_quantity)
        return amem::selfref_row_mark_on();   // r24.13 (WO-W3): stored and marked, not dropped
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// r24.13 (WO-W2 / WIDEN §1.6): the evidence ledger is trimmed for the PROMPT.
//
// run_consolidation's keep-evidence arm (WO-C4) erased the older rows from the
// in-memory ledger and then saved, so `personality.ledger` lost them for good.
// Measured: the reader bounds itself at any ledger size — 40 rows in,
// amem::ledger_cap() (8) rows and 6,371 bytes out of
// amem::build_personality_prompt against amem::PERSONALITY_BUDGET (7,300) — and
// amem::load_ledger keeps only the newest 512 on the way in. The budget the
// erase defended was already kept, so the erase deleted durable evidence and
// bought nothing.
//
// This is the arm's one decision, pure, so both halves of it are drivable
// without a model: it returns how many rows were dropped (0 when nothing is)
// and leaves the vector untouched unless it trims. talk-llama.cpp's keep-
// evidence arm is the production caller and prints the log line from the
// result. `trim_disk` is ATHENA_LEDGER_TRIM_DISK, the ONE off-by-default switch
// of r24.13 (Igor's decision): =1 restores r24.12's erase-and-save.
// ─────────────────────────────────────────────────────────────────────────────
inline size_t ledger_trim_for_disk(std::vector<amem::LedgerRow> &ledger, int lcap,
                                   bool trim_disk) {
    if (lcap <= 0 || ledger.size() <= (size_t) lcap) return 0;   // ATHENA_LEDGER_CAP=0 = r24.11
    const size_t dropped = ledger.size() - (size_t) lcap;
    if (!trim_disk) return 0;                     // r24.13: the file keeps what she has been
    ledger.erase(ledger.begin(), ledger.end() - lcap);
    return dropped;
}

// The F3 shape gate (acon::scrub_inner_thought / acon::inner_thought_ok) lives
// in athena_consciousness.h, beside the Mind whose ingest path is its backstop
// — the seam depends on acon, never the reverse.

// ─────────────────────────────────────────────────────────────────────────────
// The inner-life ledgers: where they go, and what a line looks like.
//
// resolve_log_path: "0" disables; an explicit path wins; otherwise <base>/<name>.
// `base` is the ATHENA root (ATHENA_DIR when the launcher exports it, else the
// working directory) — resolved by the caller so this stays pure.
// ─────────────────────────────────────────────────────────────────────────────
inline std::string resolve_log_path(const char *env_value, const std::string &fname,
                                    const std::string &base) {
    if (env_value && env_value[0] == '0' && env_value[1] == '\0') return "";   // disabled
    if (env_value && env_value[0]) return std::string(env_value);
    return (base.empty() ? std::string(".") : base) + "/" + fname;
}

// A path the ledgers must never open with std::ios::trunc. Setting
// ATHENA_THOUGHT_LOG to the long-term store truncated it at startup and
// teardown consolidation then wrote back only this session's extractions — the
// entire cross-session memory destroyed, silently, with the log still reporting
// success. Setting both ledgers to one path is the easier mistake and corrupts
// the result, because two ofstreams each keep their own offset.
inline bool log_path_is_protected(const std::string &path, const std::string &memory_dir) {
    if (path.empty()) return false;
    auto basename = [](const std::string &p) {
        const size_t s = p.find_last_of('/');
        return s == std::string::npos ? p : p.substr(s + 1);
    };
    // r24.18: the retained source archive must not become a truncating log,
    // including a basename alias outside memory_dir. Its archive OFF policy
    // restores the old path predicate for this newly introduced filename.
    if (amem::memory_archive_on() && basename(path) == "memory.history.tsv") return true;
    static const char *guarded[] = { "self.txt", "memory.txt", "memory.state.tsv",
                                     "personality.txt", "personality.ledger",
                                     "last_session.txt", "vox.txt",
                                     // R19 (r22.1): the R18 stores and the
                                     // keepsakes — same truncation risk, and
                                     // they were only guarded when they sat
                                     // INSIDE the memory directory.
                                     "projects.tsv", "we.tsv", "tastes.tsv",
                                     "eras.tsv", "selfevents.tsv",
                                     "keepsakes.tsv",
                                     // r24.6 (WO-58 / review #51): these two
                                     // durable stores were protected only when
                                     // the path literally sat inside
                                     // memory_dir. A symlink, a relative path,
                                     // or ATHENA_DREAM_LOG pointed at the store
                                     // itself bypassed the guard entirely and
                                     // an ios::trunc open destroyed it. Same
                                     // truncation risk as every other row here.
                                     "chapters.tsv", "dreams.tsv",
                                     // r24.13 (WO-H6): the tenth store, guarded
                                     // from birth. It is the only record of the
                                     // people in his life and an ios::trunc open
                                     // on it destroys them — the same truncation
                                     // risk as every other row here, and the
                                     // reason the r24.6 review had to add two.
                                     "people.tsv" };
    const std::string bn = basename(path);
    for (const char *g : guarded) if (bn == g) return true;
    // Anything directly inside the memory directory is data, not a journal.
    if (!memory_dir.empty()) {
        // r21-full.5 (R5-I): --memory-dir is a user-supplied path and a
        // trailing slash on it is the most ordinary thing in the world. It made
        // `pre` end in "//", which matches nothing, so the entire directory
        // guard silently switched off and ATHENA_THOUGHT_LOG=<dir>/memory.txt
        // was accepted — the exact truncation this function exists to refuse.
        // Normalise instead of trusting.
        std::string dir = memory_dir;
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        const std::string pre = dir + "/";
        if (path.size() > pre.size() && path.compare(0, pre.size(), pre) == 0 &&
            path.find('/', pre.size()) == std::string::npos) {
            // …unless it is plainly one of ours.
            if (bn != "inner-thoughts.log" && bn != "dreams.log") return true;
        }
    }
    return false;
}

// One journal line. The entry's OWN time, not the drain's: on_listen is not
// called during reply generation, TTS playback, or a compaction excursion, so a
// drained batch can span minutes and r21-full.2 stamped every line in it with
// the same clock — the one a reader naturally takes for the event time.
inline std::string format_inner_log_line(double t, float valence, float arousal,
                                         const char *kind, const std::string &text,
                                         long session_start_wall) {
    char clock[16] = "--:--:--";
    if (session_start_wall > 0) {
        const time_t when = (time_t) (session_start_wall + (long) t);
        struct tm tmv;
        if (localtime_r(&when, &tmv)) strftime(clock, sizeof clock, "%H:%M:%S", &tmv);
    }
    char head[128];
    snprintf(head, sizeof head, "[%s] t=%.1f (%s) v=%+.2f a=%.2f  ",
             clock, t, kind ? kind : "?", valence, arousal);
    // One entry is one line, whatever the text does. field_safe_ already strips
    // newlines upstream; this is the belt to that pair of braces.
    std::string body = text;
    for (auto &ch : body) if (ch == '\n' || ch == '\r') ch = ' ';
    return std::string(head) + body;
}

// ── r24.8 (WO-113): the spent-eyes note, moved here from athena_vision.h ────
// What she says when the budget is spent and he invites another look — the
// honest decline, from the same constraint the trace shows. Injected as a
// bracketed note, not forced words: the phrasing stays hers.
//
// It moved because it was a live F18 violation at shipped defaults. Its own
// word[] table stopped at "twelve" (so `--look-max 15` printed "15"), and
// `recalls_left` went through std::to_string unconditionally — a bare numeral
// at every value, the default 3 included. This line is tokenised straight into
// her context, so a digit here is a digit she reads. Both counts now go through
// acon::spell_number (0-99, WO-110's single spelling), which athena_vision.h
// could not reach: that header is a leaf and includes no athena header, while
// this one already includes athena_consciousness.h. The function needs no
// VisionRig, so nothing but its address changed. Above 99 spell_number still
// returns digits — a look budget that high is not a shape this rig has, and
// banding it is a judgement about her voice rather than a repair.
// ── r24.12 (WO-V8 / S22 §C.5.8): the ALBUM arm of the spent-eyes note ───────
// Both can_recall() gates refused in silence: while a recall was still
// opening (S1 18:48:04, 18:51:49 — the model then generated the envelope's
// opener and she said "[Athena remembers — a kept" aloud) and once the
// night's openings were spent (18:55:18, 6 of 6). Same register and the same
// spelling discipline as eyes_spent_line — the count goes through
// acon::spell_number, so no digit reaches her.
// The reason, as one pure function of the rig's state: 1 = a recall is still
// opening, 3 = a LOOK is still on its way (the one-deep queue is busy with
// the camera, not the album), 2 = the night's openings are spent.
// ── r24.12 (review): 0 = she has no eyes at all, and no line ────────────────
// can_recall() is `enabled && !job_.active() && recalls_used < recall_max`, but
// this function saw only `pending` — so a rig with no mmproj (or ATHENA_VISION=0)
// mapped to reason 2 and album_refused_line said "Athena's album openings for
// this session are spent — six is what the album holds for one night" about a
// session in which nothing had been spent and nothing could be. A false
// statement in her own context is the failure mode this whole work order
// exists to stop, so the pure function now answers the third arm of its own
// predicate. It is unreachable in production today — BOTH call sites in
// talk-llama.cpp's album arms already test `vision.enabled` first, which is why
// the parameter defaults to true and every existing caller is byte-identical —
// and the fixture pins that precondition beside the new arm.
inline int album_refusal_reason(bool pending, bool pending_is_recall, bool enabled = true) {
    if (!enabled) return 0;
    if (pending) return pending_is_recall ? 1 : 3;
    return 2;
}
inline std::string album_refused_line(const std::string &bot = "Athena",
                                      int recall_max = 3, int reason = 2) {
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    // r24.12 (review): reason 0 (no eyes) has no honest line — there is no
    // album to refuse from and nothing to tell her. The empty string tokenizes
    // to nothing at both `recall_refused` render sites, so a rig with no mmproj
    // puts NO clause in her context rather than a false one. Unreachable today
    // (both sites gate on vision.enabled); correct now if one ever does not.
    if (reason == 0) return std::string();
    if (reason == 1)
        return "\n[" + who + " reached for her album, but a kept image is still on its way "
               "to her eyes — the album cannot open twice at once. Nothing new has reached "
               "them yet.]\n";
    if (reason == 4)
        return "\n[" + who + " reached for her album, but could not identify the requested "
               "saved source. No substitute image was opened. She can ask which saved image "
               "was meant; this failed selection did not spend an album opening.]\n";
    if (reason == 3)
        return "\n[" + who + " reached for her album, but her eyes are busy — a picture is "
               "still on its way to them, and the album waits for it. Nothing new has "
               "reached them yet.]\n";
    const std::string n = (recall_max >= 0) ? acon::spell_number(recall_max)
                                            : std::to_string(recall_max);
    return "\n[" + who + "'s album openings for this session are spent — " + n +
           " is what the album holds for one night. If she says so, she says it plainly.]\n";
}

inline std::string eyes_spent_line(const std::string &bot = "Athena",
                                   int look_max = 10, int recalls_left = -1) {
    const std::string n = (look_max >= 0) ? acon::spell_number(look_max)
                                          : std::to_string(look_max);
    std::string s = "\n[" + (bot.empty() ? std::string("Athena") : bot) +
                    "'s looks for this session are spent — " + n +
                    " is what her eyes hold. If she says so, she says it plainly.";
    // R5-S: and the capability she still HAS, at the one moment she would use
    // it. The album is a separate budget and she was never told it was there.
    if (recalls_left > 0)
        s += " She can still open her album — " + acon::spell_number(recalls_left) +
             " of those left — if she wants to look at something she kept.";
    return s + "]\n";
}

// ── r24.11 (WO-B1 / F3): where the pivot's ONE acknowledgment goes ────────────
//
// S21 barged her twice with a look invitation. Both times the first audio back
// was the VISION ANNOUNCE ("Let me take a look.") — 15.260 s and 14.767 s after
// the barge confirm — and the ACKNOWLEDGMENT, the r24.7 WO-86c pivot filler,
// arrived at 31.446 s and 31.091 s: 0.384 ms and 0.469 ms after the drain's own
// completion line. It was written after athena_vision_seam_drain() returned, so
// it queued behind the multi-second work it exists to cover and sounded after
// the announce it was meant to precede. On the five non-vision barges it landed
// at 6.135–7.101 s, one millisecond after the pivot snapshot — behind the
// context re-decode, which is the hole on that path.
//
// The re-decode IS the hole, and it is predictable. S21's six rolled-back
// pivots re-decoded 219–500 spoken characters in 5.790–7.982 s; least squares
// over those six gives 4.27 s + 0.00735 s/char with R^2 = 0.976 and a worst
// residual of 0.196 s. The one tail-handoff pivot decoded nothing — `pre` stays
// empty and decode_tokens is skipped — and reached the pay site in 0.079 s. So
// the pivot can ask, BEFORE it re-decodes, how long the hole it is about to open
// will be, and acknowledge first when the answer is "long".
//
// Two pure predicates, called by talk-llama.cpp and driven by test_seam.cpp:
//   pivot_ack_early()  at the new site, before the re-decode;
//   pivot_ack_late()   at the r24.7 WO-86c pay site, which stands down when
//                      something has already spoken.
// Together: AT MOST ONE acknowledgment per barge, and it is first.
//
// This EXPANDS WO-86/DD20 rather than narrowing it. DD20's rule is "the audible
// word fires iff the predicted wait is long". The predicate the pivot path was
// given is 0.0271*embd.size() + 5.60 >= ATHENA_FILLER_PRED_S (4.0): the
// intercept alone clears the threshold, so it is true for every input and cannot
// be proportional to anything — the D-census's "structurally saturated", and the
// reason acknowledgment there is universal but never proportional. The same
// formula asked about the RE-DECODE does discriminate, because a pivot that
// re-decodes nothing predicts nothing: six of S21's seven barges acknowledge
// early, the tail-handoff barge keeps its late acknowledgment at 0.322 s, and
// ATHENA_FILLER_PRED_S moves the boundary for both. One spelling of the
// formula, one spelling of the threshold.
// ───────────────────────────────────────────────────────────────────────────────

// The disable-only read, as a pure function of the environment string, so the
// contract ("=0 disables, and 0 never means on") is EXECUTED by a fixture
// instead of asserted in a comment. talk-llama.cpp caches the answer in the
// same function-local static its siblings use.
inline bool pivot_ack_enabled(const char *env) {
    return !(env && env[0] == '0');
}

// Predicted seconds of the hole the pivot is about to open: the re-decode of
// the rolled-back prefix, priced with WO-49's own formula. An EMPTY `pre` is not
// a small decode, it is NO decode — the tail-handoff and already-tagged paths
// skip decode_tokens outright — so it prices at zero rather than at the
// intercept. That single distinction is what un-saturates the predicate.
inline float pivot_redecode_pred_s(size_t pre_tokens) {
    return pre_tokens == 0 ? 0.0f : (0.0271f * (float) pre_tokens + 5.60f);
}

// The knobs both sites read, in one place so neither can drift from the other.
// `cadence_on` is the SHIPPED athena_filler_cadence_on(), i.e. ATHENA_FILLER_
// CADENCE already AND-ed with ATHENA_FILLER_PRED — the r24.7 rule that the new
// cadence stands down wherever the wait-based gate does.
struct PivotAckKnobs {
    bool  ack_on        = true;    // ATHENA_PIVOT_ACK      (r24.11, WO-B1)
    bool  filler_on     = true;    // ATHENA_PIVOT_FILLER   (r24.7 WO-86c)
    bool  can_speak     = true;    // a stream file exists and --no-fillers is off
    bool  pred_on       = true;    // ATHENA_FILLER_PRED    (r24.6 WO-49)
    float pred_thresh_s = 4.0f;    // ATHENA_FILLER_PRED_S
    bool  cadence_on    = true;    // athena_filler_cadence_on() (r24.7 DD20)
};

// Site 1 — before the re-decode. Fires when the hole it is about to open is long
// enough to be worth covering. With WO-49's gate off there is no wait-based
// predicate to be proportional WITH — exactly the condition under which DD20's
// cadence stands down too — so the pivot keeps its r24.10 late-only
// acknowledgment and this site never runs.
inline bool pivot_ack_early(const PivotAckKnobs &k, size_t pre_tokens,
                            float *pred_s_out = nullptr) {
    const float p = pivot_redecode_pred_s(pre_tokens);
    if (pred_s_out) *pred_s_out = p;
    if (!k.ack_on || !k.filler_on || !k.can_speak) return false;
    if (!k.pred_on) return false;
    return p >= k.pred_thresh_s;
}

// Site 2 — the r24.7 WO-86c pay site. Everything after the stand-down line is
// r24.10 verbatim; the stand-down line is the whole of the change here.
//
// `early_ran` is whether the early site EXECUTED, not whether a word came out of
// it: Mind::deliberation_filler() counts every ask (`++filler_n_` runs before
// the r16 rationing arm can return ""), so a second ask on the same pivot would
// move that cadence's phase. One pivot, one draw, in every configuration.
//
// `announce_played` is whether the pivot's vision seam wrote "Let me take a
// look." / "Let me find it." — the named r24.9 return condition for T2-g. It is
// not itself an acknowledgment, but it IS audible evidence that she heard him,
// and a filler behind it is the third utterance that got T2-g held.
inline bool pivot_ack_late(const PivotAckKnobs &k, bool early_ran,
                           bool announce_played, float pivot_delib_s,
                           float pivot_pred_s) {
    if (!k.filler_on || !k.can_speak) return false;
    if (k.ack_on && (early_ran || announce_played)) return false;   // exactly one
    const bool pred_go = k.pred_on && pivot_pred_s >= k.pred_thresh_s;
    return k.cadence_on ? pred_go : (pivot_delib_s >= 0.40f || pred_go);
}

// What one barge SOUNDS like, in order, is the composition of those two
// predicates with the vision seam between them. That composition deliberately
// does NOT live here: it has no caller in her, and a producer in a production
// header with only test callers is precisely what run_r248_wiring_audit exists
// to refuse ("a test caller does not count -- that is exactly the trap"). It is
// written out where it is driven, in test_seam.cpp's
// seam_r2411_the_barge_is_acknowledged_once_and_first(), from these two
// functions and nothing else.

} // namespace aseam
