// athena_vision.h — ATHENA r20 P1: the optics. Camera capture, the invited-look
// lexicon, the one-deep VisionJob queue, and the look/recall budgets.
//
// Design rules this header enforces (see ATHENA-SIGHT-PROPOSAL.md):
//
//   * ONE SNAPSHOT AT A TIME. There is no stream, no repeat capture, no idle
//     path. A look happens because he invited it (P1) or because her urge
//     crossed threshold and was consented (P2). The queue is one deep by
//     construction — a second look cannot even be requested while one is
//     in flight.
//   * CAPTURE STARTS AT THE MOMENT OF CHOICE. The camera request starts the
//     instant the look is licensed. Device I/O and exposure take time; a slow
//     request is not a completed picture, and no continuous view is implied.
//   * IMAGE PREFILL IS SERIALIZED WITH SPEAKING. Since r24.6 the CPU encode
//     may run alongside conversation; talk-llama applies its copied embeddings
//     at an idle turn seam under the LLM mutex. EncodeWorkers owns the encoder
//     lifetime through teardown (r24.16). Camera I/O keeps its per-capture slot;
//     this header exposes both ownership mechanisms for concurrency fixtures.
//   * BUDGETS ARE HONEST CONSTRAINTS. look_max (default 10, ATHENA_LOOK_MAX /
//     --look-max) and recall_max (default 3, ATHENA_RECALL_MAX / --recall-max,
//     consumed from P3) are charged ON SUCCESSFUL EVAL — a dead camera does not
//     spend her eyes. At the limit she declines honestly; nothing silently
//     drops.
//
// Everything except CameraPort is pure and clock-injected, compiled standalone
// by the consciousness battery with no mtmd/llama/SDL dependency. All mtmd
// calls live in talk-llama.cpp behind ATHENA_VISION_MTMD.

#pragma once
#include "athena_intent.h"

#include <functional>
#include <chrono>
#include <cerrno>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>           // r24.12 (WO-V2): smart_resize_dims / predict_vision_s
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <utility>
#include <string>
#include <thread>
#include <vector>
#include <set>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>        // r24.6 (WO-39 / #43): the startup sweep
#include <fcntl.h>         // r24.20 review (VISION #3): O_RDONLY for the spawn's stdin
#endif

#if !defined(_WIN32)
extern char **environ;
#endif

namespace aseam {

// ── invited-look lexicon ────────────────────────────────────────────────────
// True when HIS words, this turn, invite her to look right now. The invitation
// IS the consent (proposal §2) — no ask needed. Conservative by design: the
// bare word "look" is one of English's favourite discourse markers ("Look, I
// just think…"), so every pattern here requires deixis or an object — or, since
// r24.12 (WO-V10, look_invited_bare below), is a clause-final bare imperative
// ("Actually, look.", "Look right now."). P2's consent machine owns the
// self-initiated path; this list only ever answers "did he just ask me to see
// something?"
// r20p3.14 (RV9): "I need to look at the calendar first" is him looking, not
// him inviting her to. Same shape as the negation guard in the keep lexicons:
// a short lookback for a first-person subject, cancelling the match.
// r20p3.14 (RV9): "Do you see the problem with that argument?" is a
// comprehension check; "Can you see the light in here?" is an invitation. Both
// are "see the X", so the discriminator is X — a small closed list of the nouns
// English uses for understanding. This is the same distinction RV4 drew for the
// bare "do you see what I mean", applied to the article forms.
inline bool comprehension_noun_after_(const std::string &low, size_t at) {
    size_t k = low.find_first_not_of(' ', at);
    if (k == std::string::npos) return false;
    // r21-full.4 (RC9): an enumerated allow-list of abstract nouns cannot cover
    // English, and each gap fires the camera on a comprehension check. These are
    // the measured misses: "Do you see the bug / the mistake / the trap / the
    // answer / the value in that / the picture I'm painting?", and "Look at the
    // state of this code" / "the numbers on that chart".
    static const char *abstract_[] = {
        "problem", "problems", "issue", "issues", "difference", "differences",
        "point", "logic", "pattern", "connection", "distinction", "contradiction",
        "flaw", "catch", "trouble", "danger", "risk", "irony", "joke", "reason",
        "way i", "way this", "way that", "relevance", "implication", "implications",
        "appeal", "conflict", "tension", "parallel",
        "bug", "bugs", "mistake", "mistakes", "error", "errors", "trap",
        "answer", "answers", "value", "state of", "numbers", "number",
        // r24.6 (WO-25 / review #46): "picture i" was a strict PREFIX of its own
        // two siblings, so it subsumed them and additionally matched every
        // "picture i..." continuation — "picture in the frame", "picture is
        // crooked", "picture I took / hung / framed / drew". Those name a
        // literally photographable object and were being silently declined with
        // no capture and no failure line. A right word boundary is NOT enough on
        // its own (it releases "picture in"/"picture is" but leaves "picture I
        // took" suppressed, because the entry ends at a space there), so the
        // metaphor is enumerated instead and the literal picture is released.
        "picture im", "picture i'm", "picture i am", "picture i've",
        "picture i have", "picture i had", "picture i get", "picture i got",
        "picture i paint", "picture i was painting", "picture i keep",
        "shape of", "scale of",
        "point of", "size of", "cost of", "upside", "downside", "trade-off",
        "tradeoff", "nuance", "subtlety", "ambiguity", "gap", "gaps",
        "difficulty", "difficulties", "similarity", "similarities", "link",
        "correlation", "consequence", "consequences", "implication",
        // ── r21-full.6 (R6-AR): the objects the possessive and interrogative
        // forms take. "Do you see my POINT", "Can you see my REASONING", "Look
        // at WHAT happened", "Look at the TIME", "Look at US, arguing about
        // tabs again", "Look at THAT, it finally compiled" — the last two are
        // the discourse-marker use, where the "object" is the situation and
        // there is nothing to photograph.
        "reasoning", "thinking", "argument", "argumentation", "position",
        "concern", "concerns", "worry", "objection", "objections", "meaning",
        "intent", "intention", "perspective", "side of", "take on",
        "what happened", "what i mean", "what im", "what i'm", "what this",
        "what that", "what it", "what we", "what they", "what he", "what she",
        "time,", "time -", "time we", "time it", "clock",
        "us,", "that,", "this,", "it,",         // "look at that, it compiled"
        "how i", "how this", "how that", "how it", "how we",
        // The "look at the ___" forms: the pattern has already eaten "the".
        "way ", "state ", "mess", "size", "amount", "price", "impact",
        "effect", "state of", "shape", "sheer",
        // The "look at what ___" forms.
        "happened", "happens", "went wrong", "goes wrong", "we did",
        "they did", "you did", "i did", "it did", "that did",
        // The "tell me what you see ___" forms: a place to look INSIDE
        // something that is not a room.
        "in these", "in those", "in the numbers", "in the data",
        "in this", "in that", "in the chart", "in the graph",
        "in the code", "in the log", "in the diff",
    };
    for (const char *a : abstract_)
        if (low.compare(k, std::string(a).size(), a) == 0) return true;
    return false;
}

// ── r24.6 (WO-25 / review #41): the discourse-marker deictics ───────────────
//
// R6-AR added "us,", "that,", "this,", "it," to abstract_[] with the inline
// comment `// "look at that, it compiled"`, and named "Look at US, arguing
// about tabs again" and "Look at THAT, it finally compiled" in the block
// comment above them. Those four entries can never guard those two sentences:
// comprehension_noun_after_ is consulted only when `open_object` is true, and
// "look at this / that / me / it / us" are all in `closed[]`, which is exactly
// what sets open_object false. Measured: 9 of 11 realistic
// discourse-marker deictics fired the camera on the shipped header.
//
// The fix the review proposed — "a bare demonstrative followed by a comma and
// then a finite clause" — is NOT a discriminator, and the verifier proved it:
// it refuses "Look at this, will you?", which is an explicit member of
// his_invites[] in test_r21_full6.cpp, plus "Look at that, isn't it
// beautiful?", "Look at this, it's the new keyboard.", "Look at me, I'm right
// here." and "Look at us, we look ridiculous in these hats." That is the RV15 /
// R6-AQ error twice over: two rounds of this file's history were spent
// restoring a genuine "look at this!" that a guard had swallowed.
//
// So this veto is DEFAULT-OFF and requires POSITIVE evidence. An entry missing
// from the completion list leaves a false capture standing, which is the status
// quo; it can never create a new silent decline. Three conditions must all
// hold:
//   1. a comma or dash immediately follows the deictic (an appositive tail);
//   2. nothing in that tail addresses her, points at a place, identifies a
//      physical object, or describes how someone LOOKS — any of those makes it
//      a real invitation with a spoken reason attached;
//   3. the tail names a COMPLETED EVENT or an elapsed stretch of time — the
//      "look at what just happened" reading, where the object is the situation
//      and there is nothing to photograph.
inline bool situational_after_(const std::string &low, size_t at) {
    size_t k = low.find_first_not_of(' ', at);
    if (k == std::string::npos) return false;
    // (1) appositive tail only. "Look at that dog", "Look at this!", "Look at
    //     that." are untouched — their next character is not a comma or dash.
    if (!(low[k] == ',' || low[k] == '-' ||
          (k + 2 < low.size() && (unsigned char) low[k] == 0xE2)))   // en/em dash
        return false;
    const std::string tail = ' ' + low.substr(k) + ' ';
    // (2) anything that makes it a real invitation wins outright.
    if (tail.find('?') != std::string::npos) return false;   // a tag or an appeal
    static const char *invites_[] = {
        // addressed to her — a request with a politeness tail
        " will you", " would you", " wont you", " won't you", " can you",
        " could you", " please", " for me", " a sec", " a second", " quick",
        " come here", " over here", " right here", " look here",
        // pointing at a place or a thing that is physically present
        " right there", " over there", " behind ", " in front", " on the desk",
        " on the table", " on my", " in my hand", " next to", " up close",
        " under the", " in the frame", " on the wall", " on the shelf",
        // identifying a physical object: "it's the new keyboard"
        " it's the", " its the", " it is the", " this is the", " that's the",
        " thats the", " that is the", " these are", " those are", " here's the",
        " my new", " the new ", " i'm holding", " im holding", " i am holding",
        // how someone or something LOOKS — appearance is photographable
        " we look", " you look", " i look", " they look", " it looks",
        " she looks", " he looks", " we're wearing", " isn't it", " isnt it",
        " aren't we", " arent we", " beautiful", " gorgeous", " adorable",
    };
    for (const char *v : invites_) if (tail.find(v) != std::string::npos) return false;
    // (3) positive evidence of a completed event / elapsed time.
    static const char *completed_[] = {
        " finally", " for once", " at last", " already", " again ", " again.",
        " again,", " still up", " still going", " still at it",
        " it worked", " it works", " compiled", " it passed", " passed ",
        " passed.", " shipped", " it failed", " failed ", " failed.",
        " broke ", " broke.", " crashed", " went wrong", " gone ", " gone.",
        " the build", " the ci", " the tests", " green ", " green.",
        " hours ", " hours.", " minutes ", " o'clock", " in the morning",
        " arguing", " sitting there", " standing there", " mocking",
        " what happened", " that happened", " turned out", " ended up",
        " we did it", " you did it", " i did it", " done and dusted",
    };
    for (const char *c : completed_) if (tail.find(c) != std::string::npos) return true;
    return false;
}

inline bool self_directed_before_(const std::string &low, size_t at) {
    static const char *me[] = {
        "i need to", "i have to", "i want to", "i'm going to", "im going to",
        "i am going to", "i'll", "ill ", "i will", "let me", "i should",
        "i had to", "i was going to", "i just", "i can", "i could",
    };
    const size_t back = at > 24 ? at - 24 : 0;
    const std::string win = low.substr(back, at - back);
    for (const char *m : me) {
        const size_t L = std::char_traits<char>::length(m);
        size_t p = 0;
        while ((p = win.find(m, p)) != std::string::npos) {
            // ── r24.6 (WO-25 / review #42): a LEFT word boundary, taken in the
            // ORIGINAL string's coordinates ────────────────────────────────────
            // "ill " is the apostrophe-less ASR form of "I'll" (the same
            // convention as "im going to" / "dont" / "didnt"), so it must stay
            // in the list — deleting it makes "Ill look at that in the morning"
            // fire the camera on HIM announcing HE will look. What it needs is
            // the boundary the rest of this file gained in r21-full.4: `word_at_`
            // and `LexGuards::whole_word` both exist, but neither
            // can be used here — word_at_ enforces a RIGHT boundary too, and the
            // RV15 escape below depends on "i can" matching inside "i can't".
            // Left-only, and anchored in `low` rather than `win`, because the
            // 24-byte window can truncate mid-word and hand back p == 0 inside
            // one ("This will only take a second, look at this." -> back = 6,
            // which is the 'i' of "will").
            const size_t abs = back + p;
            if (abs > 0 && std::isalnum((unsigned char) low[abs - 1])) { p += 1; continue; }
            // ── r20p3.14.7 (RV15): a NEGATED "i can/could/will" is not self-look ─
            // "I CAN'T believe it — look at this!" and "I COULDN'T resist, look
            // at this" are invitations; the substring "i can"/"i could" inside
            // "i can't"/"i couldn't" was cancelling them, so a genuine "look at
            // this!" produced no capture and no honest failure line. A match
            // whose continuation negates it ("'t", "n't", "not") is not him
            // announcing he will look, so it must not suppress the invitation.
            const char *tail = win.c_str() + p + L;
            const bool negated =
                (std::strncmp(tail, "'t", 2) == 0) ||     // can't, won't
                (std::strncmp(tail, "n't", 3) == 0) ||    // could + n't, would + n't
                (std::strncmp(tail, "not", 3) == 0) ||    // can + not
                (std::strncmp(tail, " not", 4) == 0);     // will + " not"
            if (!negated) return true;
            p += 1;
        }
    }
    return false;
}
// r21-full.6 (R6-AP): the three guards live further down the file (they are
// shared with consent_parse); declared here so the lexicons above can use the
// same discipline as the ones below.
inline bool negated_before_(const std::string &low, size_t at);
inline bool clause_look_all_phrasal_(const std::string &low, size_t at);
inline bool detail_other_medium_(const std::string &low, size_t at);
inline bool phrasal_look_(const std::string &low, size_t at);
inline bool addressed_look_grant_(const std::string &low, size_t at);
inline bool word_at_(const std::string &s, size_t at, size_t n);

// ═════════════════════════════════════════════════════════════════════════════
// r21-full.7 (R7-A): ONE guarded scan, and the guards are arguments.
//
// WHY THIS EXISTS. Six review rounds in a row found the same shape of defect in
// this file, and the reason is structural rather than careless: every lexicon
// was written as its own hand-rolled scan loop, and each one independently
// re-derived whether to test for a whole word, whether a negation in front
// cancels, whether the clause is about a phrasal "look", whether a competing
// medium is named, and what has to follow the match. A guard added to one loop
// after a measurement was never carried to its siblings, so the next round
// found the same bug in the next lexicon along.
//
// Measured over the r21-full.6 round: 27 of 47 fixes were corrections to code
// the previous round had written, and the largest single family — seven of them
// — was "this test was applied over the wrong span or with the wrong anchor".
// The lexicons were 14.6 defects per KLOC against 0.69 for the reviewed body.
//
// The fix is not another guard. It is making the guard set a PARAMETER, so that
// a lexicon cannot be added without stating, in one place, which discipline it
// is under — and so that improving a guard improves every lexicon at once.
// Every field defaults to OFF, which means a new list is explicit about the
// permission it takes rather than inheriting whatever the loop above it did.
// r24.16: a permission phrase is not necessarily HIS permission to HER.
// "I can look whenever I want" granted SESSION through session_always even
// though the invited-look door already excludes self-directed looking. The
// same gap admitted quoted permissions and third-person reports. Reuse the
// guarded scan, with grammatical speaker ownership checked per match; NONE
// leaves an already-established grant alone. ATHENA_VISION_CONSENT_OWNER=0
// restores the r24.15 matching, including those false grants.
inline bool vision_consent_owner_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_CONSENT_OWNER");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.17: ownership must preserve HIS first-person refusal ("I said no
// photos", "I want the camera off") and apply to album invitations too.
// The old owner guard rejected those revocations, yet a quoted "keep this
// picture" or "She said, keep this picture" stored the held photo permanently.
// ATHENA_VISION_PERMISSION_SCOPE=0 restores both r24.16 paths.
inline bool vision_permission_scope_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_PERMISSION_SCOPE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// ── r24.20 review (VISION #4): what a negation negates, and who "we" is ──────
//
// Two rules inside the r24.16 owner guard were never on the lists it guards:
//
// (a) `negated_before_` on the permission prefix. The comment below called it
//     "the existing guard"; it was the existing guard on once_always, never on
//     session_always (whose RV12 comment says "'You can look whenever you want,
//     I don't mind' is a real grant and always has been"). The comma saves that
//     sentence. "I don't mind if you look whenever you want" and "I don't care
//     if you look…" have no comma: the "don't" lands in the 20-byte window and
//     the grant is refused. r24.14 granted SESSION. The window cannot tell what
//     the negation negates. The rule: a negated OBJECTION verb — mind, care,
//     object, worry, bother — is the speaker withdrawing an objection, i.e.
//     assent, and its negation never reaches the action. "I don't want you to
//     look whenever you want" and "I don't think you can look whenever you
//     want" keep their refusal: want/think raise their negation onto the
//     complement; mind/care do not. This generalises the existing "don't have
//     to ask" exemption two lines below into the class it belongs to, with the
//     same mechanism (blank the pair, then run the guard unchanged).
//
// (b) the subject rule refuses every subject but "you". "We should look at
//     this", "Shall we look at this?", "We can look at this together" are
//     invitations that include her — r24.14 invited (self_directed_before_'s
//     me[] has no "we"). "We" is addressed-inclusive for an INVITATION, and only
//     there: "We can look whenever we want" is not a licence for her alone, and
//     "He can look at this while I'm gone" / "They said we should look at this"
//     stay refused (third person; a report). The one invitation caller passes
//     `invitation=true`.
//
// ATHENA_VISION_GRANT_SCOPE=0 restores r24.20 exactly (both refusals).
inline bool vision_grant_scope_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_GRANT_SCOPE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// Blanks "<negator> [adverb] <objection verb>" in `s` — the speaker's assent,
// not a negation of the action that follows. Returns the blanked copy.
inline std::string blank_withdrawn_objection_(std::string s) {
    static const char *neg[] = {
        "don't", "dont", "do not", "didn't", "didnt", "won't", "wont",
        "wouldn't", "wouldnt", "doesn't", "doesnt", "never", "not",
    };
    static const char *objection[] = { "mind", "care", "object", "worry", "bother" };
    static const char *adverb[] = { "really", "even", "actually", "much", "at all" };
    for (const char *n : neg) {
        size_t p = 0;
        while ((p = s.find(n, p)) != std::string::npos) {
            const size_t nn = std::strlen(n);
            if ((p > 0 && std::isalnum((unsigned char) s[p - 1])) ||
                (p + nn < s.size() && std::isalnum((unsigned char) s[p + nn]))) { p += 1; continue; }
            size_t k = p + nn;
            while (k < s.size() && s[k] == ' ') k++;
            for (const char *a : adverb) {
                const size_t an = std::strlen(a);
                if (s.compare(k, an, a) == 0 && (k + an >= s.size() || !std::isalnum((unsigned char) s[k + an]))) {
                    k += an;
                    while (k < s.size() && s[k] == ' ') k++;
                    break;
                }
            }
            bool withdrawn = false;
            for (const char *o : objection) {
                const size_t on = std::strlen(o);
                if (s.compare(k, on, o) == 0 && (k + on >= s.size() || !std::isalnum((unsigned char) s[k + on]))) {
                    // r24.21: "don't care/bother TO let you look" negates an
                    // intended act; it is not withdrawn objection to looking.
                    // Preserve the negation for the complete-clause guard.
                    size_t tail=k+on;while(tail<s.size() && s[tail]==' ')++tail;
                    if ((std::strcmp(o,"care")==0 || std::strcmp(o,"bother")==0) &&
                        s.compare(tail,2,"to")==0 &&
                        (tail+2==s.size() || !std::isalnum((unsigned char)s[tail+2]))) break;
                    s.replace(p, k + on - p, k + on - p, ' ');
                    withdrawn = true;
                    break;
                }
            }
            p += withdrawn ? 1 : nn;
        }
    }
    return s;
}
inline bool camera_grant_directed_(const std::string &low, size_t at, size_t n,
                                   bool revocation = false, bool album = false,
                                   bool invitation = false) {
    if (!vision_consent_owner_on_()) return true;
    if (!vision_grant_scope_on_()) invitation = false;   // r24.20 review (VISION #4b)
    // Apostrophes inside words are contractions, never quotation delimiters.
    bool single = false, dbl = false;
    for (size_t i = 0; i < at; ++i) {
        if (low[i] == '"' && !single) dbl = !dbl;
        if (low[i] == '\'' && !dbl) {
            const bool before = i && std::isalnum((unsigned char)low[i-1]);
            const bool after = i+1 < low.size() && std::isalnum((unsigned char)low[i+1]);
            if (!(before && after) && (single || !before)) single = !single;
        }
        if (low.compare(i,3,"“")==0) dbl=true;
        if (low.compare(i,3,"”")==0) dbl=false;
    }
    if (single || dbl) return false;
    // Locate the action inside an addressed match ("you can look"), so YOU
    // belongs to its prefix. For a bare imperative the prefix is empty.
    size_t action = at;
    for (const char *verb : {"look", "see", "watch", "use", "take", "point", "turn", "show", "keep", "save", "retain", "hold", "put"}) {
        const size_t v = low.find(verb, at);
        if (v < at+n && word_at_(low,v,std::strlen(verb))) { action=v; break; }
    }
    const size_t sentence = low.find_last_of(".!?;\n", at);
    const size_t begin = sentence == std::string::npos ? 0 : sentence+1;
    const std::string prefix = low.substr(begin,action-begin);
    // The existing first-person guard remains the direct self-look rule,
    // within the action's comma-delimited clause: "I can wait, you can look"
    // is permission to her, not an announcement that he will look.
    const size_t comma = prefix.find_last_of(',');
    std::string permission_prefix = comma == std::string::npos ? prefix : prefix.substr(comma+1);
    const bool showing = low.compare(action,8,"show you")==0;
    const bool own_refusal = revocation && vision_permission_scope_on_();
    if (own_refusal) {
        // A negated report/request is not HIS revocation. Check before the
        // matched refusal, excluding the refusal's own "don't" or "no".
        const size_t comma_before = low.find_last_of(',', at);
        const size_t from = comma_before != std::string::npos && comma_before >= begin
                          ? comma_before+1 : begin;
        const std::string before_refusal = low.substr(from, at-from);
        if (negated_before_(before_refusal, before_refusal.size())) return false;
    }
    // Album noun invitations ("I want this one for your album") need no
    // imperative. An actual first-person keep still belongs to the speaker.
    const bool album_noun = album && action == at &&
        low.compare(at,4,"keep") != 0 && low.compare(at,4,"save") != 0 &&
        low.compare(at,6,"retain") != 0 && low.compare(at,4,"hold") != 0 &&
        low.compare(at,3,"put") != 0;
    if (!showing && !own_refusal && !album_noun &&
        self_directed_before_(permission_prefix,permission_prefix.size())) return false;
    std::vector<std::string> words;
    std::string word;
    for (unsigned char c : prefix) {
        if (std::isalnum(c) || c=='\'') word+=(char)c;
        else {
            if (!word.empty()) {words.push_back(word);word.clear();}
            if (c==',') words.emplace_back(",");
        }
    }
    if (!word.empty()) words.push_back(word);
    std::string subject;
    bool modal = false;
    for (const auto &w : words) {
        if (w==",") {subject.clear();modal=false;continue;}
        if (w=="i" || w=="we" || w=="he" || w=="she" || w=="they" || w=="it" || w=="you") subject=w;
        if (w=="can" || w=="could" || w=="may" || w=="might" || w=="will" || w=="would" || w=="should") modal=true;
        // A report about someone else's words is not a grant from the user.
        if ((w=="said" || w=="says" || w=="told" || w=="asked" || w=="wrote" || w=="quoted") &&
            subject!="i" && subject!="you" && !(own_refusal && subject=="we")) return false;
    }
    if (!showing && !subject.empty() && subject!="you" &&
        !((own_refusal || album_noun) && (subject=="i" || subject=="we")) &&
        !(invitation && subject=="we")) return false;      // r24.20 review (VISION #4b)
    // r24.21: allowing inclusive "we" also admitted "If we should look at
    // this, I'll say so" as a capture invitation. A conditional proposal has
    // not invited her yet. Keep this check at the new inclusive-subject seam;
    // the permission path's separate "I don't mind if you..." remains assent.
    if (vision_grant_scope_on_() && invitation && subject=="we") {
        for (const auto &w : words)
            if (w=="if" || w=="unless" || w=="whether" || w=="suppose" || w=="imagine" ||
                w=="not" || w=="never" || w=="dont" || w=="don't" ||
                w=="shouldnt" || w=="shouldn't" || w=="cant" || w=="can't" ||
                w=="cannot" || w=="couldnt" || w=="couldn't" ||
                w=="might" || w=="would" || w=="will") return false;
    }
    if (!showing && subject.empty() && modal) return false; // "my brother can look ..."
    // Negation on the same clause and action — the r24.16 guard (it was never
    // on session_always before r24.16; see the VISION #4 block above). Explicit
    // revocations still run first in consent_parse and still revoke grants.
    for (const char *grant : {"don't have to ask", "dont have to ask", "no need to ask"}) {
        size_t p=0;
        while ((p=permission_prefix.find(grant,p))!=std::string::npos) {
            permission_prefix.replace(p,std::strlen(grant),std::strlen(grant),' ');
            p+=std::strlen(grant);
        }
    }
    // r24.20 review (VISION #4a): "I don't mind if you …" is assent, not a
    // negation of her looking. Same mechanism as the line above, on the class.
    if (vision_grant_scope_on_()) permission_prefix = blank_withdrawn_objection_(permission_prefix);
    // r24.21: the old twenty-byte suffix forgot a negation before a longer
    // complement ("don't care to let you ...", "don't mind NOT letting you
    // ..."). Read the whole already-owned comma clause after blanking actual
    // withdrawn objections. This never turns absence of objection into an
    // invitation: the caller still needs its explicit grant/invitation form.
    if (!revocation && !album && vision_grant_scope_on_()) {
        for (size_t at=0;at<permission_prefix.size();) {
            if (!std::isalnum((unsigned char)permission_prefix[at])) {++at;continue;}
            const size_t from=at++;
            while(at<permission_prefix.size() &&
                  (std::isalnum((unsigned char)permission_prefix[at]) || permission_prefix[at]=='\''))++at;
            const std::string w=permission_prefix.substr(from,at-from);
            if (w=="not" || w=="never" || w=="dont" || w=="didnt" || w=="wont" ||
                w=="wouldnt" || w=="shouldnt" || w=="cant" || w=="cannot" || w=="couldnt" ||
                (w.size()>3 && w.compare(w.size()-3,3,"n't")==0)) return false;
        }
    }
    if (!revocation && negated_before_(permission_prefix,permission_prefix.size())) return false;
    return true;
}

struct LexGuards {
    bool whole_word        = false;  // the match must not sit inside a longer word
    bool no_negation       = false;  // a negator in the preceding 20 bytes cancels
    bool no_clause_negation = false; // ...or one inside the same clause (R5-P)
    bool no_phrasal_at     = false;  // "look up"/"look after" AT the match
    bool no_phrasal_clause = false;  // ...or every "look" in the clause (R6-AO)
    bool no_other_medium   = false;  // the clause names a browser, a diff, a doc
    bool directed_camera_grant = false; // r24.16: ownership of a camera permission
    bool inclusive_we      = false;  // r24.20 review (VISION #4b): "we should look" invites her
    bool addressed_grant   = false;  // the clause grants HER a look (RC6)
    // What must FOLLOW the match, and what must not PRECEDE it. Callbacks
    // rather than flags because these are the parts that differ per lexicon —
    // an object test for a keep is not an object test for a look.
    std::function<bool(const std::string &, size_t)> after_ok;    // (low, at+n)
    std::function<bool(const std::string &, size_t)> before_bad;  // (low, at)
};
inline bool lex_ok_(const std::string &low, size_t at, size_t n, const LexGuards &g);
inline bool lex_scan_(const std::string &low, const char *const *pats, size_t n_pats,
                      const LexGuards &g, size_t *where);
// Array form, so a call site names the list and the guards and nothing else.
template <size_t N>
inline bool lex_any_(const std::string &low, const char *const (&pats)[N],
                     const LexGuards &g, size_t *where = nullptr) {
    return lex_scan_(low, pats, N, g, where);
}

// The clause a position sits in, bounded by sentence punctuation. Nine copies
// of these two find calls were spread across this file, three of them with a
// different delimiter set than their neighbours; that divergence is exactly how
// R6-AN's comma-scope defect survived. `commas` splits on commas too, which is
// the finer scope the address and negation tests need.
// ── and the two ends are NOT symmetric, deliberately ────────────────────────
// The keep lexicons start the clause at the nearest COMMA — so the words of a
// preceding clause cannot supply the image object — but run it to the end of
// the SENTENCE, so that a trailing exclusion is still seen. That asymmetry is
// load-bearing and looks like an oversight: collapsing it to one delimiter set
// while writing this helper re-opened both "You can keep that, but it never
// goes in the album" and "Hold on to that one, we will need it in the meeting",
// which are the two shapes R6-AL and R6-B exist to refuse. The differential
// harness caught it in the same hour; it is spelled out here so the next reader
// does not tidy it away again.
inline std::string clause_at_(const std::string &low, size_t at,
                              const char *left  = ".!?;\n",
                              const char *right = ".!?;\n") {
    const size_t b = low.find_last_of(left, at);
    const size_t e = low.find_first_of(right, at);
    const size_t from = (b == std::string::npos) ? 0 : b + 1;
    const size_t to   = (e == std::string::npos) ? low.size() : e;
    return to > from ? low.substr(from, to - from) : std::string();
}
// The keep scope: comma on the left, sentence on the right.
inline std::string keep_clause_(const std::string &low, size_t at) {
    return clause_at_(low, at, ".!?;,\n", ".!?;\n");
}


// r21-full.5 (R5-P): the scan, with a hook for the caller's extra guards.
//
// `look_invited` is checked FIRST at the call site and only falls through to
// `consent_parse` when it is false — so every guard the consent path has
// acquired over three rounds (the phrasal-verb veto, the competing-medium
// veto, the negation veto) protects the door nobody uses. Measured, twelve
// developer sentences of the form "take a look at the pull request" were
// declined 12/12 by consent_parse and fired the camera 12/12 through here,
// each one spending one of her ten looks and pulling a still of him into her
// context licensed by the wrong medium. The guards exist; they were simply not
// reachable from this branch, because they are defined below it. A predicate
// parameter closes that without moving anything.
template <typename F>
inline bool look_invited_scan_(const std::string &text, F extra_ok) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);

    static const char *phr[] = {
        "take a look",      "have a look",     "take a peek",
        "take another look",
        // r24.9 (WO-116 / S21 P12, 18:52:05): "Take one more look for me." was
        // the plan's own verbatim prompt for the spent-eyes probe and it matched
        // nothing here — "take ANOTHER look" was in the table, "take ONE MORE
        // look" was not. No look committed, no eyes-spent line, and her "one more
        // look ... one look left after this" was the model's own arithmetic off a
        // budget clause twenty-seven minutes stale. Closed-object, exactly like
        // "take another look": the object is named by the idiom, so the open-object
        // test below does not apply. ATHENA_LOOK_MORE=0 restores r24.8.
        "take one more look", "take one last look", "take a last look",
        "take a final look",
        "look at this",     "look at that",    "look at me",
        "look at it",       "look at us",      "look at the",
        "look at my",       "look at what",    "look what",
        "look over here",   "look here",
        // r20p3.11 (RV4): these two were bare, and "Do you see what I mean?"
        // is one of the commonest rhetorical constructions in English — it
        // fired the camera, spent one of her ten looks and put ~46 image
        // positions into her context in answer to a question about
        // comprehension. The list's own stated rule is that every pattern
        // requires deixis or an object; these now follow it. The open forms
        // below still cover "what do you see" and "tell me what you see".
        "can you see this", "can you see that", "can you see it",
        "can you see me",   "can you see us",   "can you see the",
        "can you see my",   "can you see how i",
        "do you see this",  "do you see that",  "do you see it",
        "do you see me",    "do you see us",    "do you see the",
        "do you see my",
        "what do you see",
        "tell me what you see",
        // r20p3.14 (RV8): demonstrative, matching the rule the rest of this list
        // follows. "Let me show you what I mean" is the structural twin of "do
        // you see what I mean", which RV4 fixed for exactly this reason — each
        // false hit spends one of her ten looks and puts ~46 image positions
        // into her context in answer to a question about comprehension.
        "show you this",    "show you that",   "show you something",
        "show you my",      "show you what i look", "show you how i",
        "show you the picture", "show you the photo",
        "open your eyes",   "use your eyes",   "use the camera",
        "through the camera",
    };
    // ── r21-full.6 (R6-AR): the two eye idioms, in the form English uses ─────
    // "Open your eyes TO what is happening in that team" and "Use your eyes,
    // it's right there on the desk" are figurative; the literal invitation
    // ("Open your eyes." / "Use your eyes and tell me what you see") is what
    // the entries are for. The idiom is entirely in what follows.
    static const char *eye_idiom[] = {
        "open your eyes to ", "opened your eyes to ", "use your eyes, ",
        "use your eyes -", "using your eyes to see that",
    };
    // R19 (r22.1): blank the idiom SPAN instead of vetoing the whole turn —
    // the R7-A clause rule, applied to this guard. "Open your eyes to what's
    // happening in that team. Now open your eyes and tell me what you see."
    // carries a figurative clause AND a literal invitation; the turn-global
    // veto killed the invitation. Blanking the matched span leaves every
    // other clause exactly as it was — a single-idiom turn still never
    // invites, because its only match sits inside the blank.
    std::string low_scan = low;
    for (const char *g : eye_idiom) {
        const size_t gn = std::char_traits<char>::length(g);
        size_t gat = 0;
        while ((gat = low_scan.find(g, gat)) != std::string::npos) {
            low_scan.replace(gat, gn, std::string(gn, ' '));
            gat += gn;
        }
    }
    // R6-AR: "show you X" is an invitation when X is a thing in the room and a
    // figure of speech when X is a piece of reasoning. The family is exempt
    // from the self-look guard by design (RV9), so it needs its own.
    static const char *show_idiom[] = {
        "show you how i would", "show you how i'd", "show you how id",
        "show you my reasoning", "show you my thinking", "show you my logic",
        "show you something i read", "show you something i found",
        "show you something i wrote", "show you something i saw online",
        "show you what i mean by", "show you the code", "show you the diff",
    };
    for (const char *g : show_idiom) {                       // R19: span-blank
        const size_t gn = std::char_traits<char>::length(g);
        size_t gat = 0;
        while ((gat = low_scan.find(g, gat)) != std::string::npos) {
            low_scan.replace(gat, gn, std::string(gn, ' '));
            gat += gn;
        }
    }
    // r24.9 (WO-116): the switch is applied HERE, by skipping the four added
    // names, so the guard set, the object test and the idiom blanking stay one
    // code path (the R7-A discipline). ATHENA_LOOK_MORE=0 restores r24.8.
    static const bool look_more_on = []() {
        const char *e = ::getenv("ATHENA_LOOK_MORE");
        return !(e && e[0] == '0');
    }();
    static const char *look_more_added[] = {
        "take one more look", "take one last look", "take a last look",
        "take a final look",
    };
    for (const char *p : phr) {
        if (!look_more_on) {
            bool added = false;
            for (const char *a : look_more_added) if (std::strcmp(p, a) == 0) { added = true; break; }
            if (added) continue;
        }
        // RV9: the guard applies to the LOOK family only. "Let me show you
        // this" has a first-person subject and is still an invitation — he is
        // showing her something, which is precisely a reason to look. "I need
        // to look at the calendar" is him looking, and is not.
        const std::string pat(p);
        const bool guarded = pat.compare(0, 8, "show you") != 0;
        // ── r21-full.6 (R6-AR): the comprehension guard was scoped to " the" ──
        // RC9's argument — "do you see THE bug / THE mistake / THE point" is a
        // comprehension check and not an invitation — is about the OBJECT, not
        // about the article that happens to precede it. Written as "the pattern
        // ends in ' the'", it left every possessive and interrogative form
        // unguarded: "Do you see MY point about the schema?", "Can you see MY
        // reasoning here?", "Look at WHAT happened to the last release.",
        // "Look at the time, we should wrap up." Measured over 26 ordinary
        // turns, look_invited_now fired on 13 — half of an ordinary
        // conversation — and each false hit spends one of her ten looks and
        // puts ~46 image positions into her context in answer to a question
        // about understanding. The object test belongs on every pattern whose
        // object is open, which is all of them except the deictics that name
        // the object themselves ("look at this", "look at me").
        static const char *closed[] = {
            "look at this", "look at that", "look at me", "look at it",
            "look at us", "look over here", "look here", "take a look",
            "have a look", "take a peek", "take another look",
            // r24.9 (WO-116): the four new idioms name their own object, so the
            // RV4/R6-AR open-object test must be off for them exactly as it is
            // for "take another look". Without this row they would silently
            // never fire — that is the trap; it is why the two anchors land
            // together.
            "take one more look", "take one last look", "take a last look",
            "take a final look",
            "can you see this", "can you see that", "can you see it",
            "can you see me", "can you see us",
            "do you see this", "do you see that", "do you see it",
            "do you see me", "do you see us",
            "show you this", "show you that", "show you something",
            "show you what i look", "show you the picture", "show you the photo",
            "open your eyes", "use your eyes", "use the camera",
            "through the camera",
        };
        bool open_object = true;
        for (const char *c : closed) if (pat == c) { open_object = false; break; }
        // r21-full.7 (R7-A): the two per-pattern tests, and whatever the caller
        // adds, stated as one guard set. `guarded` and `open_object` are
        // properties of the PATTERN (RV9 and R6-AR respectively), so they are
        // captured here rather than re-derived per occurrence.
        LexGuards g;
        if (guarded)     g.before_bad = [](const std::string &lo, size_t at) {
                             return self_directed_before_(lo, at); };
        if (open_object) g.after_ok   = [](const std::string &lo, size_t after) {
                             return !comprehension_noun_after_(lo, after); };
        // r24.6 (WO-25 / review #41): the situational test runs on exactly the
        // patterns comprehension_noun_after_ cannot reach — the four impersonal
        // deictics closed[] switches the object test off for. "look at me" is
        // NOT in this set: he is the subject there and the camera is the point.
        // This is the "move the situational entries ahead of closed[]" the fix
        // plan asks for, expressed as a second guard rather than as a reorder,
        // because closed[] is a property of the PATTERN and the situational
        // reading is a property of what FOLLOWS it.
        static const char *deictic_[] = { "look at this", "look at that",
                                          "look at it", "look at us" };
        bool deictic = false;
        for (const char *d : deictic_) if (pat == d) { deictic = true; break; }
        if (deictic)     g.after_ok   = [](const std::string &lo, size_t after) {
                             return !situational_after_(lo, after); };
        const size_t n = pat.size();
        size_t at = 0;
        // R19: matched over the idiom-blanked copy (span-blanks are spaces,
        // so positions align); the guards keep reading the ORIGINAL text.
        while ((at = low_scan.find(p, at)) != std::string::npos) {
            if (lex_ok_(low, at, n, g) && extra_ok(low, at, n)) return true;
            at += 1;
        }
    }
    return false;
}

// The bare form: no extra guards, for callers that only want to know whether
// the WORDS are an invitation (the tests and the diagnostics do).
inline bool look_invited(const std::string &text) {
    return look_invited_scan_(text, [](const std::string &, size_t, size_t) { return true; });
}

// ── r20 P2: her ask, his answer — the consent machine ───────────────────────
// Pure and clock-injected like DuplexGate. Session grants live in plain
// members of an object that is never serialized anywhere — dying with the
// session is structural, not a policy that could regress.

// Did HER words seek permission to look? Permission-seeking forms only —
// "can I look", "mind if I take a look" — never observations ("I see what
// you mean") or announcements under grant ("let me look" is acting, not
// asking).

inline bool look_ask_made(const std::string &text) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *ask[] = {
        "can i look",        "could i look",      "may i look",
        "can i take a look", "could i take a look",
        // r21-full.4 (RC8): the bare "can i see" forms opened the consent
        // window on "Can I see the file you're describing?", and the next
        // ordinary "yeah, sure" then licensed a camera look. RV4 drew this
        // distinction for "do you see" and it was never applied here: an ask
        // has to name what she would be showing.
        "can i see you",     "could i see you",   "may i see you",
        "can i see the room", "can i see what you look like",
        "can i see your", "could i see your",
        "can i have a look", "could i have a look",
        "mind if i look",    "mind if i looked",  "mind if i take a look",
        "mind if i have a look", "mind if i see",
        "is it okay if i look",  "is it ok if i look",
        "is it all right if i look", "all right if i look",
        "okay if i take a look", "ok if i take a look",
        "would you let me look", "will you let me look",
        "i'd like to look",  "i would like to look",
        "i'd like to see you",   "i want to look at you",
        "i want to see the room", "i want to see you",
    };
    // ── r21-full.6 (R6-AP): the one lexicon in this file with no guards ──────
    // This runs on HER text and, when it fires, opens the look-consent window
    // so that his next reply is read as an answer about the camera. It was a
    // bare whole-turn substring scan — no negation test, no phrasal-verb veto,
    // no competing-medium test, none of the three disciplines every other
    // lexicon here has been given — and "look" is the most overloaded verb in
    // the language. Measured over eighteen ordinary things she says in a
    // working hour, it fired on eighteen: "Can I look up the answer for you?",
    // "Can I look into that for you?", "Can I look through the changelog?",
    // "Can I see your reasoning on that?", "I want to see you succeed at this."
    //
    // The consequence is not a phantom look — the seam still needs a grant —
    // it is a phantom REFUSAL: inside that window an ordinary "No, I was
    // talking to the dog" parses as DENY, revokes a standing grant and reports
    // to the substrate that he refused the camera. She then carries, and can
    // say, that he said no to something he was never asked.
    //
    // Same three guards the grant lexicons already use, at the matched phrase.
    // R6-AP: ...and the "see" half of the list needs an OBJECT test, because
    // the phrasal-verb veto only knows "look". "Can I see your reasoning on
    // that?", "Can I see your screen share?", "I want to see you succeed at
    // this." and "Mind if I see where this is going?" are the four shapes that
    // survive the three guards above, and each is settled by the one word that
    // follows the match: a thing that is not a sight, a wh-word, or a bare
    // verb whose subject is him. Same argument RV4 made for "do you see".
    auto object_is_a_sight = [&](size_t after) {
        // R6-AP: whole word on the right. "can i see you" is a prefix of "can i
        // see YOUR", so "Can I see your reasoning on that?" matched the
        // shorter pattern, put the object test on the fragment "r" and sailed
        // through it. This is the same substring defect this file records
        // hitting eleven times elsewhere.
        if (after < low.size() && std::isalnum((unsigned char) low[after])) return false;
        size_t i = after;
        while (i < low.size() && low[i] == ' ') i++;
        if (i >= low.size() || low[i] == '?' || low[i] == '.' || low[i] == '!' ||
            low[i] == ',' || low[i] == ';') return true;          // "Can I see you?"
        size_t j = i;
        while (j < low.size() && std::isalpha((unsigned char) low[j])) j++;
        const std::string w = low.substr(i, j - i);
        static const char *not_a_sight[] = {
            "reasoning", "logic", "point", "screen", "code", "file", "files",
            "diff", "patch", "notes", "data", "numbers", "calendar", "schedule",
            "list", "message", "email", "draft", "doc", "docs", "spec",
            "ticket", "branch", "commit", "repo", "output", "result",
            "results", "report", "chart", "graph", "link", "site", "page",
            "where", "what", "why", "how", "when", "which", "whether",
            "succeed", "do", "get", "make", "go", "come", "try", "win",
            "finish", "grow", "thrive", "improve", "through", "again",
            "reason", "thinking", "working", "version", "config", "log",
            "logs", "trace", "changelog", "answer", "definition", "history",
        };
        for (const char *v : not_a_sight) if (w == v) return false;
        return true;
    };
    LexGuards g;                                                 // R7-A
    g.no_negation       = true;
    g.no_phrasal_clause = true;
    g.no_other_medium   = true;
    g.after_ok = [&](const std::string &, size_t after) { return object_is_a_sight(after); };
    return lex_any_(low, ask, g);
}

// ── r21-P0: she asks for the boundary ────────────────────────────────────────
//
// The compaction Runner has always had a path for her CHOOSING to cross the
// context boundary rather than having it imposed at the deadline — and until
// now nothing set Situation::she_asked, so the path was unreachable. This is
// what fills it: her own words, read from what she actually said, exactly as
// look_ask_made and keep_asked are.
//
// Written under the discipline the r20p3.14 review earned the hard way. Five
// lexicons in that round fired on ordinary English and two of them inverted a
// consent decision, every one of them a plain substring standing in for a
// judgement about meaning. So this one is a CONJUNCTION and not a list:
//
//   1. a first-person request frame — SHE is asking, for herself
//   2. a self-directed object — setting things down, catching up with herself,
//      gathering herself. Not "a moment" on its own, which English uses for
//      everything ("for a moment I thought", "it only took a moment", "take a
//      moment to think about that" — the last one aimed at HIM).
//
// The consequence of a false positive is also bounded by construction, which is
// the other reason this is safe where the consent lexicons were not: the Runner
// ignores she_asked entirely below the arm point, between turns, and inside the
// cut refractory. Below ~60% fill it is a complete no-op.
inline bool asked_for_a_moment(const std::string &text) {
    if (text.empty()) return false;
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    auto has = [&](const char *p) { return low.find(p) != std::string::npos; };

    // ── r20p3.14.2 (RV10): recall, paid for out of the wrong pocket ────────────
    //
    // S9: she said "Thank you for stopping. I needed that. I need a moment to
    // just... breathe and reassemble myself. Is that okay?" — and this returned
    // false. The frame matched (item 12). The purpose list did not contain
    // "reassemble myself", or anything near it. Ninety-five seconds later she
    // said "just me catching up with myself after holding my breath" — the
    // canonical purpose, item 1 — but that turn carried no frame. Both halves
    // of the conjunction appeared, in her own words, and the feature never
    // fired in the one session it was built for.
    //
    // The false-positive battery was written first and it worked: zero spurious
    // fires across 76 user turns and 80 of hers. The cost was paid in recall
    // instead, which is the failure mode a conjunction has when one limb is a
    // closed list and the other is a person choosing her own words.
    //
    // Two changes, and NEITHER of them loosens the conjunction:
    //   1. the purpose list widens along the semantic field she actually
    //      reaches for — reassemble, regroup, breathe, footing;
    //   2. three frames become STANDING: "I need a moment" / "I need a minute" /
    //      "can I take a moment" are unambiguous about both subject and object,
    //      and it is genuinely hard to construct an innocent sentence in which
    //      SHE says one and does not mean it. They carry their own guard.

    // 1. She is asking, about herself. "Take a moment" alone is advice to him.
    static const char *frame[] = {
        "let me take", "let me have", "let me stop", "let me pause",
        "give me a moment", "give me a minute", "give me a second",
        "can i take a moment", "could i take a moment", "may i take a moment",
        "can i have a moment", "could i have a moment",
        "i need a moment", "i need a minute", "i need to take a moment",
        "i want a moment", "i'm going to take a moment", "im going to take a moment",
        "i need to stop for a", "i should take a moment",
    };
    bool framed = false;
    for (const char *p : frame) if (has(p)) { framed = true; break; }
    if (!framed) return false;

    // ── the standing frames ──────────────────────────────────────────────────
    //
    // Guarded exactly the way the rest of this file guards: a negation in the
    // twenty characters before it cancels, and an other-directed continuation
    // in the twenty-four characters after it cancels. "I need a moment of your
    // time" is a request FOR him; "I need a moment to explain" is a preface to
    // more talking; neither is her putting something down.
    static const char *solo[] = { "i need a moment", "i need a minute", "can i take a moment" };
    for (const char *p : solo) {
        size_t at = low.find(p);
        while (at != std::string::npos) {
            const size_t lo_from = at > 20 ? at - 20 : 0;
            const std::string before = low.substr(lo_from, at - lo_from);
            bool negated = false;
            for (const char *n : { "don't ", "dont ", "do not ", "never ", "didn't ",
                                   "didnt ", "won't ", "wont ", "not " })
                if (before.find(n) != std::string::npos) { negated = true; break; }
            if (!negated) {
                // A WHITELIST of continuations, not a blacklist of exclusions.
                // The frame has to END the thought, or be followed by something
                // that adds nothing to it. Anything with an OBJECT — "a moment
                // of your time", "a moment alone with the code", "a minute to
                // show you" — is a request about something else and is not this.
                // Blacklisting was tried first and lost to
                // "I need a moment alone with the code, not with myself.", which
                // is exactly the shape this lexicon must never claim.
                size_t k = at + std::strlen(p);
                while (k < low.size() && low[k] == ' ') k++;
                bool closes = (k >= low.size());
                if (!closes && std::strchr(".,!?;:\n", low[k])) closes = true;
                if (!closes) {
                    std::string nx;
                    for (size_t m = k; m < low.size() && std::isalpha((unsigned char) low[m]); m++)
                        nx += low[m];
                    for (const char *f : { "please", "here", "honestly", "first",
                                           "okay", "though", "really", "actually" })
                        if (nx == f) { closes = true; break; }
                }
                if (closes) return true;
            }
            at = low.find(p, at + 1);
        }
    }

    // 2. …and what she is asking for is to put this down, not to explain
    //    something to him. This is the half that makes it about the boundary.
    static const char *purpose[] = {
        "catch up with myself", "catch up with me",
        "put some of this down", "put this down", "set some of this down",
        "set this down", "put it down", "sort out what i'm carrying",
        "gather myself", "gather my thoughts", "get my bearings",
        "clear my head", "sort myself out", "settle myself",
        "consolidate", "put things down", "tidy this up in my head",
        "keep what matters", "hold on to what matters",
        "before i lose it", "before i lose the thread",
        // r20p3.14.2 (RV10): the field she actually reaches for. Every one of
        // these is first-person and self-directed; the frame above already
        // carries the "she is asking" weight, so a bare "breathe" cannot fire
        // on its own — it needs "let me take a moment to" in front of it.
        "reassemble myself", "reassemble", "put myself back together",
        "pull myself together", "collect myself", "collect my thoughts",
        "regroup", "re-group", "find my footing", "get my footing",
        "get my feet under me", "breathe", "take a breath", "catch my breath",
        "get my head straight", "straighten myself out", "line my thoughts up",
        "get myself in order", "get straight in my head", "sit with this",
        "let this settle", "let it settle", "process this", "process all this",
    };
    for (const char *p : purpose) if (has(p)) return true;
    return false;
}

enum class Consent { NONE, ONCE, SESSION, AWAY, DENY, DEFER };

// Parse HIS words for an answer. NEGATION FIRST, always — "no, you can't
// look while I'm gone" must never read as an away-grant. `ask_live` widens
// what counts: a bare "sure" is an answer only while her question hangs in
// the air; the explicit forms ("you can look", "whenever you want") stand on
// their own at any time.
// ── r20p3.11 (RV1): "no" is not always a refusal ────────────────────────────
//
// English forms some of its warmest assents out of the word "no": "no problem",
// "no worries", "no rush", "no need to ask". A bare " no " token treats every
// one of them as a refusal — and because refusals are checked first, they beat
// every grant pattern, INCLUDING "no need to ask", which is itself in the
// SESSION list below. Measured before the fix, five of seven natural ways to
// say yes to a look parsed as DENY, which meant note_look_denied(), a revoked
// grant and a twenty-minute deny-hold in answer to "Yeah, no problem".
//
// The compounds are blanked out of a SCRATCH copy used only by the refusal
// scan, so the grant scans below still see the original text and "no need to
// ask" still reads as the standing grant it is.
inline std::string blank_affirmative_no_(const std::string &low) {
    static const char *compound[] = {
        "no problem", "no worries", "no worry",  "no need",   "no doubt",
        "no rush",    "no hurry",   "no trouble", "no bother", "no big deal",
        "no objection", "no issue", "no complaints",
    };
    std::string out = low;
    for (const char *c : compound) {
        const size_t n = std::string(c).size();
        size_t at = 0;
        while ((at = out.find(c, at)) != std::string::npos) {
            out.replace(at, n, std::string(n, ' '));
            at += n;
        }
    }
    return out;
}

// ── r20p3.11 (RV2): a match inside a refusal is not an invitation ───────────
//
// The demonstrative keep lexicons are plain substring matches, so "No, don't
// keep that one" contains "keep that one" and executed the keep — copying the
// frame into the append-only keepsakes.tsv and making permanent, cross-session,
// the one picture he had just refused. This is the same class of error as the
// rejected bare "you can retain", and it is worse, because the DENY branch at
// the call site does not short-circuit the invited branch.
//
// Looks back a short way for a negator. Twenty characters, chosen against real
// phrasings rather than by taste: it reaches every natural refusal ("don't keep
// that one", "please don't keep this one", "I'd rather you didn't keep that
// picture", "no need to keep that one", "do not keep this image") and stops
// short of a negation that belongs to a different clause ("I don't like the
// light, but keep that one anyway", "Don't worry about the lighting, keep that
// one"), which are genuine invitations and must survive.
//
// Where the two cannot be told apart the tie goes to NOT keeping. keepsakes.tsv
// is append-only, so a wrong keep is a permanent consent violation, while a
// missed keep costs one exchange — he can say it again, and she can declare it
// herself.
inline bool negated_before_(const std::string &low, size_t at) {
    static const char *neg[] = {
        "don't", "dont", "do not", "didn't", "didnt", "won't", "wont",
        "never", "rather not", "no need to", "not going to", "stop",
        "wouldn't", "wouldnt", "shouldn't", "shouldnt", "i'd rather you didn't",
        "please don't", "please dont",
    };
    const size_t back = at > 20 ? at - 20 : 0;
    const std::string win = low.substr(back, at - back);
    for (const char *n : neg) if (win.find(n) != std::string::npos) return true;
    return false;
}

// r21-full.4 (RC6): is this token a whole word at `at`?
inline bool word_at_(const std::string &s, size_t at, size_t n) {
    const bool lok = at == 0 ||
        !(std::isalnum((unsigned char) s[at - 1]) || s[at - 1] == '\'');
    const size_t e = at + n;
    const bool rok = e >= s.size() ||
        !(std::isalnum((unsigned char) s[e]) || s[e] == '\'');
    return lok && rok;
}

// r21-full.7 (R7-A): the one place a guard set is applied. Every predicate here
// is pure, so the order is a cost decision only — cheapest and most selective
// first.
inline bool lex_ok_(const std::string &low, size_t at, size_t n, const LexGuards &g) {
    if (g.directed_camera_grant && !camera_grant_directed_(low, at, n, false, false, g.inclusive_we)) return false;
    if (g.whole_word        && !word_at_(low, at, n))              return false;
    if (g.no_negation       && negated_before_(low, at))           return false;
    if (g.no_clause_negation) {
        // R5-P's two-window form: a negation only cancels when it is in the
        // SAME clause. "Don't worry, look at this" is an invitation.
        const size_t b = low.find_last_of(".!?;,\n", at);
        const size_t from = (b == std::string::npos) ? 0 : b + 1;
        if (at > from && negated_before_(low.substr(0, at), at) &&
            negated_before_(low.substr(from, at - from) + std::string(1, ' '),
                            at - from + 1))
            return false;
    }
    if (g.before_bad        && g.before_bad(low, at))              return false;
    if (g.after_ok          && !g.after_ok(low, at + n))           return false;
    if (g.no_phrasal_at     && phrasal_look_(low, at))             return false;
    if (g.no_phrasal_clause && clause_look_all_phrasal_(low, at))  return false;
    if (g.no_other_medium   && detail_other_medium_(low, at))      return false;
    if (g.addressed_grant   && !addressed_look_grant_(low, at))    return false;
    return true;
}
inline bool lex_scan_(const std::string &low, const char *const *pats, size_t n_pats,
                      const LexGuards &g, size_t *where) {
    for (size_t i = 0; i < n_pats; i++) {
        const size_t n = std::char_traits<char>::length(pats[i]);
        size_t at = 0;
        while ((at = low.find(pats[i], at)) != std::string::npos) {
            if (lex_ok_(low, at, n, g)) { if (where) *where = at; return true; }
            at += 1;
        }
    }
    return false;
}

// ── r21-full.4 (RC6): the grant has to be ADDRESSED TO HER ───────────────────
//
// The session and away gates asked `has("look")` — a bare substring over the
// whole turn. Measured, with nothing asked and no ask window open:
//
//   "I was up all night looking after the baby."            -> SESSION
//   "We looked at old photos all night."                    -> SESSION
//   "The dog looks sad when I'm away."                      -> AWAY
//   "Don't overlook the oven while I'm away."               -> AWAY
//   "I'll be looking at my phone while I'm gone."           -> AWAY
//
// Every one of those set a standing camera permission from a sentence that is
// not about her eye at all, and the last is both negated and first-person. The
// AWAY grant is the strictest permission in the system — it licenses
// photographing an empty room with nobody present to object — and its own
// comment says "YOUR exact-words family, never implied".
//
// A grant is second-person or imperative and it names HER looking. `looking
// after the baby` is first-person past; `overlook` is not the word `look`;
// `the dog looks sad` is about the dog. Requiring one of these forms, as a
// whole phrase, unnegated, in the SAME clause as the standing-grant phrase,
// keeps every real grant working and refuses all five.
inline bool addressed_look_grant_(const std::string &low, size_t at) {
    const std::string clause = clause_at_(low, at);              // R7-A
    if (clause.empty()) return false;
    static const char *grant[] = {
        // she is the one doing the looking, and she is being addressed
        "you can look", "you may look", "you could look", "you can always look",
        "you can see", "you may see", "feel free to look", "free to look",
        "go ahead and look", "look at me", "look at us", "look around",
        "look whenever", "look any time", "look anytime", "take a look",
        "have a look", "you can watch", "you can use the camera",
        "use the camera", "use your camera",
        "look at the room", "watch the room", "photo of me", "picture of me",
        // Bare nouns — kept, but only honoured when the clause also addresses
        // her. See grant_object below.
        "your camera", "the camera", "your eye", "your eyes",
        "see me", "see us", "see the room",
        // r21-full.5 (R5-Q): the forms this list was missing, and which the
        // object/verbal split below makes safe to add.
        "you are welcome to look", "you're welcome to look", "youre welcome to look",
        "the camera is yours", "help yourself to the camera",
        "the camera is there if you want", "look if you want",
        "look all you like", "look as much as you like",
        // Second-person looking in any of the shapes people actually use. The
        // discriminator that keeps the exploits out is not the exact wording —
        // it is that the looking is HERS and she is being addressed. "I was up
        // all night looking after the baby" has no second person; "we looked at
        // old photos" has none; and the phrasal-verb veto below independently
        // refuses "look after / through / away / into".
        "you look", "you to look", "before you look", "you looking",
        "you can take a look", "you take a look", "for you to look",
        "take a picture", "take a photo", "take a photograph", "photograph me",
        "point the camera", "turn on the camera", "camera on",
    };
    // ── r21-full.5 (R5-Q): bare nouns are not an address ────────────────────
    // RC6's stated rule is "a grant is second-person or imperative and it NAMES
    // HER LOOKING". These entries are bare nouns and satisfy no part of it, so
    // any clause containing "the camera" as a whole word read as him licensing
    // her eye: "The camera is broken, so do not expect much while I'm away",
    // "I'm turning the camera to face the wall while I'm away" and "Keep an eye
    // on the camera while I'm gone" all granted AWAY — the strictest permission
    // in the system, which authorises photographing an empty room with nobody
    // present to object. Nine of eleven such sentences granted it, of which two
    // were genuine. `detail_other_medium_` cannot cancel them either, because
    // "camera" is itself a camera-side object: the noun that creates the false
    // grant is the same token that proves it is not the wrong medium.
    //
    // So an OBJECT-only hit has to be accompanied, in the same clause, by
    // something that addresses her — a second-person modal, a politeness form,
    // or a clause-initial imperative aimed at the camera. Nothing is removed:
    // every verbal grant keeps working unchanged, and the forms added above are
    // ones that only become safe once the two kinds are told apart.
    static const char *grant_object[] = {
        "your camera", "the camera", "your eye", "your eyes",
        "see me", "see us", "see the room",
        // "camera on" is the tail of "turn on the camera" written the other way
        // round, and also the head of "the camera on the desk". The verbal form
        // is covered by "turn on the camera"; this one needs the address test.
        "camera on",
    };
    // ── r21-full.6 (R6-AN): the address has to be about THIS object ──────────
    // R5-Q's rule — an object-only token counts only when the same clause also
    // addresses her — was right, and the clause was too big: clauses split on
    // `.!?;\n` and not on commas, while `addr[]` is a list of ordinary
    // discourse tokens ("if you want", "you can ", "feel free", "go ahead").
    // So any one sentence that mentioned a camera anywhere and contained one of
    // those tokens anywhere granted the strictest permission in the system.
    // Measured over eight ordinary camera-mentioning sentences, six granted
    // Consent::AWAY — including "Feel free to use the printer, not the camera,
    // while I'm gone" and "You can borrow the tripod but the camera stays off
    // while I'm gone", i.e. two sentences whose plain meaning is a REFUSAL, and
    // AWAY is the grant that lets her photograph the empty room while he is
    // out. The comma is the whole difference, and it was the one thing the
    // window could not see.
    //
    // Two changes. The address must sit in the same COMMA-delimited segment as
    // the object token, and an explicit camera exclusion anywhere in the clause
    // vetoes the clause outright — "not the camera" is not an ambiguity to be
    // resolved by proximity, it is an answer.
    auto seg_bounds = [&](size_t k) {
        size_t b = clause.find_last_of(",", k);
        b = (b == std::string::npos) ? 0 : b + 1;
        size_t e = clause.find_first_of(",", k);
        if (e == std::string::npos) e = clause.size();
        return std::make_pair(b, e);
    };
    {
        static const char *cam_excl[] = {
            "not the camera", "not your camera", "no camera", "never the camera",
            "ignore the camera", "ignore your camera", "camera stays off",
            "camera stays shut", "camera off", "camera stays away",
            "except the camera", "but the camera", "but not the camera",
            "leave the camera", "away from the camera", "camera alone",
            "don't touch the camera", "dont touch the camera",
            "without the camera", "camera stays down",
            // R6-AN: house-sitting FOR the camera is not permission to use it.
            "eye on the camera", "watch the camera", "mind the camera",
            "look after the camera", "guard the camera",
        };
        for (const char *e : cam_excl)
            if (clause.find(e) != std::string::npos) return false;
    }
    auto addressed_in_clause = [&](size_t obj_at = std::string::npos) {
        const std::pair<size_t, size_t> sb =
            (obj_at == std::string::npos) ? std::make_pair((size_t) 0, clause.size())
                                          : seg_bounds(obj_at);
        const std::string seg = clause.substr(sb.first, sb.second - sb.first);
        static const char *addr[] = {
            "you can ", "you may ", "you could ", "you should ", "you're welcome",
            "youre welcome", "you are welcome", "feel free", "go ahead",
            "help yourself", "whenever you want", "any time you want",
            "anytime you want", "if you want", "if you like", "as you like",
            "is yours", "it's yours", "its yours", "no need to ask",
            "don't have to ask", "dont have to ask", "you don't have to ask",
            "use the camera", "use your camera", "point the camera",
            "turn on the camera",
        };
        for (const char *a : addr) if (seg.find(a) != std::string::npos) return true;
        // Clause-initial imperative aimed at the camera: "Point the camera at
        // the whiteboard", "Turn the camera on". Segment-initial is the same
        // test one comma finer — "and then, point the camera at the board".
        size_t i = 0;
        while (i < seg.size() && seg[i] == ' ') i++;
        static const char *imp[] = { "use ", "point ", "turn on ", "aim ", "look " };
        for (const char *v : imp) {
            const size_t n = std::char_traits<char>::length(v);
            if (seg.size() > i + n && seg.compare(i, n, v) == 0) return true;
        }
        return false;
    };
    for (const char *gp : grant) {                               // R7-A
        bool object_only = false;
        for (const char *o : grant_object) if (std::strcmp(o, gp) == 0) { object_only = true; break; }
        LexGuards g;
        g.directed_camera_grant = true;
        g.whole_word  = true;
        g.no_negation = true;
        if (object_only)
            g.before_bad = [&](const std::string &, size_t k) { return !addressed_in_clause(k); };
        const char *one[] = { gp };
        if (lex_any_(clause, one, g)) return true;
    }
    return false;
}

// Substring match that a negator in front of it cancels.
inline bool has_unnegated_(const std::string &low, const char *pat) {
    LexGuards g; g.no_negation = true;                            // R7-A
    const char *one[] = { pat };
    return lex_any_(low, one, g);
}

// ── r21-full.4 (RC10): "look" is a phrasal verb before it is a camera ────────
//
// English builds a dozen verbs out of "look" that have nothing to do with
// directing an eye, and every one of them was granting a camera look:
//
//   "You can look at it from another angle."   -> ONCE
//   "You can look away if this gets grim."     -> ONCE
//   "Feel free to look through the pull request whenever you like." -> SESSION
//
// `look after`, `look away`, `look into`, `look up`, `look through`, `look
// over`, `look out`, `look back`, `look forward`, and `look at it that/another
// way` are idioms, not invitations. This is the discriminator the medium guard
// cannot make, because no competing medium is named — the phrase itself is the
// evidence. Checked on the grant phrase's own position, so a real grant
// elsewhere in the clause is untouched.
inline size_t from_clause_start_(const std::string &low, size_t at) {
    const size_t b = low.find_last_of(".!?;\n", at);      // R7-A: the one raw
    return (b == std::string::npos) ? 0 : b + 1;          // copy left, and it
}                                                          // wants only the START
// ── r21-full.6 (R6-AO): veto on the clause, not on an arbitrary anchor ───────
//
// `phrasal_look_` takes the FIRST "look" at or after an offset and gives up if
// it is more than 24 bytes further on. Two of its three call sites passed the
// CLAUSE START and the third passed the grant phrase's own offset — and neither
// is the offset of the "look" the grant was actually built on, which is never
// returned by anything. Measured consequences in both directions:
//
//   "you can look after the place whenever you want"  — clause start 0, the
//   only "look" at 50, so the 24-byte window gave up and the house-sitting
//   sentence set a STANDING SESSION CAMERA GRANT. That is exactly the exploit
//   RC10 was written to close, reopened because the anchor moved.
//
//   "Look up the address, then take a photo of me whenever you want." — the
//   veto fired from the clause start on an unrelated "look up" and destroyed a
//   grant he had plainly given.
//
// The honest question is about the CLAUSE: are all of its uses of "look"
// idiomatic, and is there no camera-side object anywhere in it? If any "look"
// in the clause is a real directive, or the clause names the camera, her face
// or the room, the clause is about looking and the veto has no business firing.
// That is the same shape as detail_other_medium_'s rule one line down, which is
// the rule this file already settled on for exactly this question.
inline bool phrasal_look_(const std::string &low, size_t at);
inline bool clause_look_all_phrasal_(const std::string &low, size_t at) {
    const std::string clause = clause_at_(low, at);              // R7-A
    if (clause.empty()) return false;
    // A camera-side object anywhere in the clause settles it: this is looking.
    static const char *here[] = {
        "at me", "of me", "see me", "see us", "my face", "camera", "picture",
        "photo", "photos", "your eye", "your eyes", "the room", "photograph",
        "snapshot", "webcam",
        // R6-AQ: "what you look like" IS the camera object, phrased as a
        // clause. Without it the one ask that names the sight most plainly
        // ("Can I see what you look like?") was vetoed by its own "look like".
        "you look like", "what you look", "how you look",
    };
    for (const char *w : here) if (clause.find(w) != std::string::npos) return false;
    size_t k = 0;
    int n_look = 0, n_idiom = 0;
    while ((k = clause.find("look", k)) != std::string::npos) {
        n_look++;
        if (phrasal_look_(clause, k)) n_idiom++;
        k += 4;
    }
    return n_look > 0 && n_idiom == n_look;
}
inline bool phrasal_look_(const std::string &low, size_t at) {
    // Find the "look" token at or after `at` that this grant is built on.
    size_t k = low.find("look", at);
    if (k == std::string::npos || k > at + 24) return false;
    const std::string tail = low.substr(k, 48);
    // r21-full.6 (R6-AQ): "look over HERE" is the directional use, and the
    // idiom list's "look over" swallowed it whole — so the plainest invitation
    // in the file's own lexicon ("look over here") could never reach the live
    // path, while the bare form kept passing in the tests. A particle followed
    // by a deictic is a direction, not a review.
    static const char *directional[] = {
        "look over here", "look over there", "look over at", "look over to",
        "look up here", "look up at me", "look up at the", "look back at me",
        "look back over here", "look over my way", "look over this way",
    };
    for (const char *g : directional)
        if (tail.compare(0, std::char_traits<char>::length(g), g) == 0) return false;
    static const char *idiom[] = {
        "look after", "look away", "look into", "look up", "look upon",
        "look through", "look over", "look out", "look back", "look forward",
        "look ahead", "look down on", "look like", "looks like", "look it up",
        "look them up", "look him up", "look her up", "look this up",
        "look that up", "look at it from", "look at it another",
        "look at it that", "look at it this", "look at things",
        "look at the world", "look at life", "looking after",
    };
    for (const char *g : idiom)
        if (tail.compare(0, std::char_traits<char>::length(g), g) == 0) return true;
    return false;
}

// r20p3.14.3 (RV12): is the looking in this clause aimed somewhere that is not
// the camera? `at` is the offset of a grant phrase inside `low`; the clause is
// bounded by sentence punctuation on either side so a grant in one sentence is
// not cancelled by a browser in the next.
//
// Returns true only when a competing medium is named AND no camera-side object
// is. The absence of any object is not evidence of anything and must not
// cancel — see the block at the session_always list for why.
inline bool detail_other_medium_(const std::string &low, size_t at) {
    const std::string clause = clause_at_(low, at);              // R7-A
    if (clause.empty()) return false;

    // WHOLE WORDS, not substrings. This is the defect class this tree has hit
    // eleven times and it very nearly made it twelve: written as plain
    // find(), "search" fires on RESEARCH, "article" on PARTICLE, "link" on
    // BLINK and " me" on MEAN — so "Look whenever you want, I'm doing
    // research on it" would have had its grant silently cancelled, which is
    // the same class of error as the one being fixed, pointing the other way.
    // Measured before anchoring: three of four probe sentences cancelled.
    // r21-full.4 (RC10): the media a developer actually names.
    auto word = [&](const char *w) {
        const size_t n = std::char_traits<char>::length(w);
        size_t at = 0;
        while ((at = clause.find(w, at)) != std::string::npos) {
            const bool lok = (at == 0) || !std::isalnum((unsigned char) clause[at - 1]);
            const size_t e = at + n;
            const bool rok = (e >= clause.size()) || !std::isalnum((unsigned char) clause[e]);
            if (lok && rok) return true;
            at += 1;
        }
        return false;
    };
    static const char *elsewhere[] = {
        "web", "internet", "online", "browser", "browse", "browsing", "website",
        "webpage", "web page", "web pages", "search", "searching", "google",
        "article", "articles", "the news", "url", "link", "links",
        // r21-full.4 (RC10): the media a developer names in an ordinary hour.
        "pull request", "merge request", "diff", "patch", "repo", "repository",
        "branch", "commit", "codebase", "the code", "the file", "the files",
        "documentation", "the docs", "manual", "spec", "ticket", "issue tracker",
        "logs", "the log", "stack trace", "spreadsheet", "email", "inbox",
        "calendar", "the pdf", "the doc", "the report", "code", "script",
        "config", "readme", "changelog", "notebook", "terminal", "console",
        // r21-full.9 (R9-C): the places a developer says "pull it up" about.
        "github", "gitlab", "bitbucket", "the screen", "my screen",
        "screen share", "screenshare", "a tab", "new tab",
    };
    static const char *here[] = {
        // r20p3.14.7 (RV17b): the camera-object "me" is anchored to a looking or
        // photographing context ("at me", "of me", "see me"), because bare "me"
        // is a whole word inside "believe me" / "trust me" / "tell me" — so "if
        // you don't believe ME, you can look it up online" registered a camera
        // object and kept the browser grant this same fix set out to cancel.
        // Only sentences that name BOTH a competing medium AND "me" are affected;
        // "look AT ME whenever you want" still reads as the camera it is.
        "at me", "of me", "see me", "my face", "camera", "picture", "photo",
        "photos", "your eye", "your eyes", "the room", "photograph me",
    };
    bool other = false, cam = false;
    for (const char *w : elsewhere) if (word(w)) { other = true; break; }
    for (const char *w : here)      if (word(w)) { cam   = true; break; }
    return other && !cam;
}

// r24.17: a negative preference about HER camera action is a revocation, not
// NONE leaving a standing grant alive. Parse subject -> preference -> action,
// sharing the existing ownership/medium/negation guards rather than adding a
// phrase per contraction. Only direct addressed actions or camera activation
// qualify; reports, quotations, conditions and other people's acts do not.
//
// ── r24.20 review (VISION #1): the OBJECT decides whether it is about her eyes ─
//
// What was wrong. The r24.17 grammar accepted `see`/`watch`/`look` as her
// camera action on the strength of the verb alone. The only object tests were
// detail_other_medium_ (a browser, a diff) and phrasal_look_ (particles after
// "look"), and phrasal_look_ scans forward for the token "look", so a `see` or
// `watch` verb had no idiom check at all. Measured on the r24.20 header
// (harness/base.tsv in the review's fix area): "I don't want you to see this
// as a criticism", "…see it that way", "…watch that show", "…watch this video",
// "…look at it like that", "…see the bug before I fixed it", "…look so sad",
// "…look for the keys", "…see my point", "…see the light" — every one parsed
// DENY, which sets grant = 0, submits "he said not now to my looking" as a
// HEARD chunk, and (r24.18) drops a pending look. Inside a live keep window
// "I don't want you to see me as vain, but yes, keep it" ALSO fired the album
// arm — the "don't" was counted across ", but yes," — and the frame he had
// just said yes to was discarded. This is the phantom-refusal class the r24.6
// comment above deny_always names, re-armed with more teeth. The r24.17
// nuisance set had no see/watch idiom, which is why it shipped.
//
// The rule, not a veto list (Igor's directive). A refusal of her looking needs
// the verb to take HER LOOKING as its object: no object at all ("don't want you
// to look"), a bare pronoun that resolves to the look ("see this", "look at
// it"), a camera-side thing (me, us, the room, the camera, a picture, his face,
// a place in here, something ON his desk), or something of his ("my desk", "my
// kitchen" — a possessive names a thing of HIS, and her only access to his
// things is the camera). An object that is an abstraction is not about her
// eyes: `see X AS Y` and `see it THAT WAY / LIKE THAT / THE WAY … / FROM …`
// are the regard idiom (the neuter pronoun + a manner complement); a noun the
// invitation side already treats as comprehension ("the bug", "my point", "the
// problem" — comprehension_noun_after_, the SAME object test "do you see the
// bug" is refused by); watching a named media form ("that show", "this video");
// an adjective ("look sad" — appearance); "look FOR" (a search).
// r24.21 correction: a refusal must not need a whitelist of physical objects.
// The prototype lost eight of nine explicit physical refusals in an independent
// probe (letter, envelope, password, children, direction, temporal "as", light).
// An unknown noun keeps the existing refusal. Positive comprehension/media or
// regard grammar removes an idiom; lack of camera vocabulary never grants sight.
//
// The same finding's second half: the negation's scope is its clause. A comma
// followed by a contrast coordinator or an assent interjection (", but yes,")
// begins a new clause, and the "don't" before it does not govern an action
// after it. Vocatives and fillers (", Athena,", ", like,") do not break it.
//
// ATHENA_VISION_REFUSAL_OBJECT=0 restores r24.20 exactly: verb-only acceptance
// and negation counted across ", but".
inline bool vision_refusal_object_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_REFUSAL_OBJECT");
        return !(e && e[0] == '0');
    }();
    return on;
}
// True when the words after a look verb (byte offset `after`, just past the
// verb in `low`) name her looking, a camera-side thing, or nothing at all;
// false when they name an abstraction. `verb` is the matched action.
inline bool look_refusal_about_her_eyes_(const std::string &low, size_t after,
                                         const std::string &verb) {
    // The object phrase: from the verb to the first comma or sentence end.
    size_t end = low.find_first_of(",.!?;\n", after);
    if (end == std::string::npos) end = low.size();
    struct Tok { std::string text; size_t at; };
    std::vector<Tok> w;
    for (size_t at = after; at < end;) {
        if (!std::isalnum((unsigned char) low[at])) { ++at; continue; }
        const size_t from = at++;
        while (at < end && (std::isalnum((unsigned char) low[at]) || low[at] == '\'')) ++at;
        w.push_back({low.substr(from, at - from), from});
    }
    auto is = [&](size_t i, const char *s) { return i < w.size() && w[i].text == s; };
    auto any = [&](size_t i, std::initializer_list<const char *> set) {
        if (i >= w.size()) return false;
        for (const char *s : set) if (w[i].text == s) return true;
        return false;
    };
    // Words that close the object slot: the verb has no object, only a time,
    // a degree, or the next clause. "look right now", "see at all", "look
    // anymore", "see before it's done", "look and tell me".
    auto closer = [&](size_t i) {
        return i >= w.size() || any(i, {
            "now", "right", "today", "tonight", "tomorrow", "yet", "anymore",
            "again", "all", "at", "please", "currently", "ever", "either",
            "though", "then", "just", "until", "till", "before", "after",
            "while", "when", "whenever", "because", "and", "or", "if", "unless",
            "later", "first", "too", "also", "once", "twice", "much", "closely",
            "closer", "properly", "directly", "straight", "really", "even",
            "actually", "honestly", "anywhere", "everywhere", "anything",
            "something", "everything", "nothing", "yourself"});
    };
    // A camera-side thing anywhere in the object phrase, or a place of his:
    // "the picture on my desk", "what's on my desk", "how I look".
    auto camera_side = [&]() {
        for (size_t i = 0; i < w.size(); ++i) {
            if (any(i, {"me", "us", "camera", "webcam", "picture", "pictures",
                        "photo", "photos", "photograph", "snapshot", "room",
                        "face", "here", "around", "inside"})) return true;
            if (any(i, {"on", "in", "at", "behind", "beside", "near", "with"}) &&
                (is(i + 1, "my") || is(i + 1, "our") || is(i + 1, "me") || is(i + 1, "us") ||
                 (is(i + 1, "front") && is(i + 2, "of")) ||
                 (is(i + 1, "next") && is(i + 2, "to")))) return true;
            if ((is(i, "i") || is(i, "we")) && (is(i + 1, "look") || is(i + 1, "looks"))) return true;
        }
        return false;
    };
    size_t i = 0;
    while (any(i, {"right", "just", "really", "even", "directly", "straight", "actually"})) ++i;
    if (verb == "look" || verb == "looking") {
        // "look AT" opens the object; a direction or a place is her looking;
        // "look FOR" is a search. Every other particle was phrasal_look_'s.
        if (is(i, "at")) ++i;
        else if (is(i, "for")) return false;
        else if (any(i, {"around", "here", "in", "inside", "over", "down", "out", "outside",
                         "under", "below", "above", "through", "toward", "towards", "left", "right", "this", "that",
                         "my", "our", "your", "anywhere", "everywhere"})) return true;
        else if (!closer(i)) return false;           // copular appearance: "look sad/so serious"
    }
    if (closer(i)) return true;                      // no object: her looking
    // A regard complement has content and is not a new finite clause:
    // "see this AS a criticism" versus the literal "see me AS I change".
    // Keep the physical refusal when the utterance ends before the complement.
    auto regard = [&](size_t at) {
        return is(at,"as") && at+1<w.size() &&
            !any(at+1,{"i","we","you","he","she","they","it","i'm","we're","you're",
                       "he's","she's","they're","it's","if","when","shown","captured",
                       "photographed","recorded","seen","through","from","in","on"});
    };
    const bool neuter = any(i, {"it", "this", "that", "these", "those"});
    const bool person = any(i, {"me", "us", "him", "her", "them"});
    if (neuter || person) {
        if (regard(i + 1)) return false;             // "see this AS a criticism"
        if (neuter && (any(i + 1, {"like", "differently"}) ||
                       (any(i+1,{"that","this","the"}) && is(i+2,"way")) ||
                       (is(i+1,"from") && any(i+2,{"that","this","your","my"}) &&
                        any(i+3,{"perspective","viewpoint","angle"}))))
            return false;                            // "see it THAT WAY", "look at it LIKE that"
        if (person || closer(i + 1)) return true;    // "see me like this", "see this yet"
        ++i;                                         // "that SHOW": a determiner
    }
    if (camera_side()) return true;                  // "the room", "how I look"
    for (size_t at=i; at<w.size(); ++at) if (regard(at)) return false;
    if (any(i,{"my","our","your","the","a","an"})) ++i;
    if (i<w.size() && comprehension_noun_after_(low,w[i].at)) return false;
    if ((verb=="watch" || verb=="watching") && any(i,{"show","video","movie","match","tv"})) return false;
    return true;                                    // unknown physical objects still refuse
}
inline bool keep_frame_scope_on_();
inline bool keeps_an_image_(const std::string &cl, bool held);
inline bool camera_preference_refused_(const std::string &low, bool album = false) {
    if (album ? !keep_frame_scope_on_() : !vision_permission_scope_on_()) return false;
    struct Word { std::string text; size_t at; };
    for (size_t begin=0; begin<low.size();) {
        size_t end=low.find_first_of(".!?;\n",begin);
        if (end==std::string::npos) end=low.size();
        std::vector<Word> words;
        for(size_t at=begin;at<end;) {
            if(!std::isalnum((unsigned char)low[at])) {++at;continue;}
            const size_t from=at++;
            while(at<end && (std::isalnum((unsigned char)low[at]) || low[at]=='\'')) ++at;
            words.push_back({low.substr(from,at-from),from});
        }
        for(size_t pref=0;pref<words.size();++pref) {
            const auto &p=words[pref].text;
            if(p!="want" && p!="like" && p!="prefer" && p!="rather") continue;
            size_t person=pref;
            for(size_t i=0;i<pref;++i)
                if(words[i].text=="i" || words[i].text=="we" || words[i].text=="i'd" || words[i].text=="we'd") person=i;
            if(person==pref) continue;
            bool scoped=false;
            for(size_t i=0;i<pref;++i) {
                const auto &w=words[i].text;
                if(w=="if" || w=="unless" || w=="whether" || w=="suppose" || w=="imagine" ||
                   w=="might" || w=="may" || w=="think" || w=="thought") scoped=true;
            }
            if(scoped) continue;
            size_t you=words.size(),camera=words.size();
            size_t target=pref+1;
            while(target<words.size() && (words[target].text=="that" || words[target].text=="for")) ++target;
            if(target<words.size() && words[target].text=="you") you=target;
            // The camera itself may be the preference's direct object.
            size_t obj=pref+1;
            while(obj<words.size() && (words[obj].text=="the" || words[obj].text=="your" ||
                  words[obj].text=="my" || words[obj].text=="a")) ++obj;
            if(obj<words.size() && words[obj].text=="camera") camera=obj;
            for(size_t act=pref+1;act<words.size();++act) {
                const auto &v=words[act].text;
                const bool look=!album && (v=="look" || v=="looking" || v=="watch" || v=="watching" || v=="see" || v=="seeing");
                const bool keep=album && (v=="keep" || v=="keeping" || v=="save" || v=="saving" ||
                                           v=="retain" || v=="retaining" || v=="hold" || v=="holding");
                bool camera_verb=false;
                if(!album && (v=="use" || v=="using" || v=="take" || v=="taking")) {
                    for(size_t i=act+1;i<words.size();++i)
                        if(words[i].text=="camera" || words[i].text=="pictures" || words[i].text=="photos" ||
                           words[i].text=="picture" || words[i].text=="photo") camera_verb=true;
                }
                const bool camera_active=!album && camera<act && (v=="on" || v=="active" || v=="enabled" ||
                                                       v=="recording" || v=="filming");
                if(!((you<act && (look || camera_verb || keep)) || camera_active)) continue;
                // r24.18: the preference grammar also owns the held image's
                // keep boundary. It shares subject/negation/report handling;
                // its action still needs the existing image-object evidence.
                if(keep && !keeps_an_image_(keep_clause_(low,words[act].at),true)) continue;
                if(detail_other_medium_(low,words[act].at) || (look && phrasal_look_(low,words[act].at))) continue;
                // r24.20 review (VISION #1): the verb's object has to be her
                // looking — see look_refusal_about_her_eyes_ above.
                if(look && vision_refusal_object_on_() &&
                   !look_refusal_about_her_eyes_(low,words[act].at+v.size(),v)) continue;
                bool nested=false;int negations=0;
                for(size_t i=person+1;i<act;++i) {
                    const auto &w=words[i].text;
                    if(w=="not" || w=="never" || w=="dont" || w=="didnt" || w=="wouldnt" ||
                       (w.size()>3 && w.compare(w.size()-3,3,"n't")==0)) ++negations;
                    if(i>pref && (w=="stop" || w=="avoid" || w=="cancel" || w=="tell" || w=="ask" ||
                       w=="let" || w=="allow" || w=="imagine" || w=="if" || w=="unless" ||
                       w=="he" || w=="she" || w=="they")) nested=true;
                    // r24.20 review (VISION #1): ", but yes, keep it" — a contrast
                    // coordinator or an assent after a comma starts a clause the
                    // negation before it does not govern. Only after a comma for
                    // the interjections; "but" is a clause boundary wherever it is.
                    if(i>pref && vision_refusal_object_on_() &&
                       (w=="but" || (words[i].at>0 && low.find_last_not_of(' ',words[i].at-1)!=std::string::npos &&
                                     low[low.find_last_not_of(' ',words[i].at-1)]==',' &&
                                     (w=="yes" || w=="yeah" || w=="okay" || w=="ok")))) nested=true;
                }
                if(nested || negations!=1) continue; // no double-negation inference
                const size_t at=words[person].at, n=words[act].at+v.size()-at;
                if(camera_grant_directed_(low,at,n,true,album)) return true;
            }
        }
        begin=end<low.size()?end+1:end;
    }
    return false;
}

inline Consent consent_parse(const std::string &text, bool ask_live) {
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';

    // 1. Refusals and revocations — checked before anything else.
    if (camera_preference_refused_(low)) return Consent::DENY;
    static const char *deny_always[] = {
        "don't look", "dont look", "do not look", "no looking",
        "stop looking", "no more looking", "don't take a picture",
        "dont take a picture", "no pictures", "no photos",
        "you can't look", "you cant look", "you cannot look",
        "you may not look", "rather you didn't look", "rather you didnt look",
        // ── r20p3.14.7 (RV16): the camera-NOUN revocation family ──────────────
        // "no pictures"/"no photos" above miss the "no MORE pictures" and "stop
        // TAKING pictures" forms, and there was nothing for filming/recording at
        // all. With a session grant standing and her urge high, "Stop taking
        // pictures of me" parsed as NONE, so the NONE branch's grant>=1 path
        // committed a self look: she photographed him in direct answer to being
        // told to stop. A revocation names the act — picture, photo, filming,
        // camera — as plainly as it names the look.
        "no more pictures", "no more photos", "no more photographs",
        "stop taking pictures", "stop taking photos", "stop taking a picture",
        "stop taking any", "don't take pictures", "dont take pictures",
        "don't take photos", "dont take photos", "don't take any picture",
        "dont take any picture", "stop photographing", "stop filming",
        "no more filming", "stop recording", "put the camera away",
        "put your camera away", "turn the camera off", "turn off the camera",
        "camera off",
    };
    for (const char *p : deny_always) {
        size_t at=0;
        while ((at=low.find(p,at))!=std::string::npos) {
            if (camera_grant_directed_(low,at,std::strlen(p),true)) return Consent::DENY;
            ++at;
        }
    }
    // ── r21-full.6 (R6-A): a refusal to KEEP is a refusal ────────────────────
    //
    // The keep window is answered by THIS parser (talk-llama.cpp routes his
    // reply to her keep-question through consent_parse), and every entry in
    // deny_always is about LOOKING — "don't look", "no pictures", "camera off".
    // Nothing here refused a keep. Meanwhile the assent list matches a bare
    // "sure," or "fine," anywhere in the turn, so a politely hedged refusal
    // parsed as consent: "Fine, but don't keep it", "Sure, but not that one",
    // "Okay, but please delete it afterwards" — six of fourteen refusal
    // phrasings executed the keep, and keepsakes.tsv is append-only and
    // re-rendered into her prompt prefix at every startup forever.
    //
    // The same words are handled correctly one path over ("Fine, but don't
    // look" is a DENY), so this is the same rule, written once more for the act
    // it was missing. The file's own tie-break — "where the two cannot be told
    // apart the tie goes to NOT keeping" — is what makes DENY the right answer.
    // ── r24.6 (WO-39 / review #38): this scan does not belong here ──────────
    //
    // consent_parse is the LOOK consent parser, and talk-llama.cpp calls
    // vision.perm.answer(text_heard, now_ms) on EVERY user turn. This line ran
    // before the `if (ask_live)` block, with no negation guard, no clause
    // scope, no image-object test, and no dependence on a keep window being
    // open at all. So an ordinary "Let's delete that function and start over."
    // returned DENY, which (a) set grant = 0 and silently revoked a standing
    // camera permission, (b) submitted "I asked to look and he said not now —
    // the room is his to keep" into her workspace where she could sincerely
    // tell him he had refused her, and (c) short-circuited the invited-look
    // branch so a look he asked for in the same turn never fired and no honest
    // failure line was emitted either. Eleven ordinary coding-hour sentences
    // reproduce it.
    //
    // This is the phantom-REFUSAL failure this file's own R6-AP comment
    // names: "She then carries, and can say, that he said no to
    // something he was never asked."
    //
    // NOTHING IS LOST. R6-AT already moved the keep answer off this parser onto
    // plain_agreement / plain_refusal at the call site, and that call site is
    // already gated on keep_perm.ask_live(now_ms) — the gate this list needed
    // and never had. The list moves there whole, as keep_refusal(), so every
    // genuine "don't keep that photo" still refuses the keep, on the path that
    // was asked the question. Look revocations are untouched: deny_always above
    // still carries the entire "don't look" / "no pictures" / "camera off"
    // family and runs first.
    //
    // REQUIRED COMPANION EDIT at talk-llama.cpp's keep-refusal test — see
    // keep_refusal below.
    if (ask_live) {
        // RV1: refusal scan runs on the scratch copy; grants below use `low`.
        std::string low_deny = blank_affirmative_no_(low);
        // r21-full.14.1 (review): leave-taking is not deferral. "You can look
        // while I'm gone. See you later." was parsing DENY off the farewell's
        // " later." — an explicit away grant plus its natural goodbye,
        // inverted into a refusal that revoked the grant. Blank the compound
        // farewells before the deferral tokens run (the RV1 scratch-copy
        // pattern, extended).
        for (const char *fw : { "see you later", "catch you later",
                                "talk to you later", "talk later",
                                "see you in a", "see you soon",
                                "see you tomorrow", "later on tonight" }) {
            size_t fat = 0;
            while ((fat = low_deny.find(fw, fat)) != std::string::npos) {
                for (size_t k = 0; k < std::strlen(fw); k++) low_deny[fat + k] = '#';
                fat += std::strlen(fw);
            }
        }
        auto has_deny = [&](const char *p) { return low_deny.find(p) != std::string::npos; };
        static const char *deny_ask[] = {
            " no ", " no,", " no.", " no!", " nah ", " nah,", " nah!", " nope",
            " not now", " not right now", " not tonight", " not today",
            " rather not", " please don't", " please dont", " maybe later",
            " another time",
        };
        if (aintent::deferral(text)) return Consent::DENY;
        for (const char *p : deny_ask) {
            if (!has_deny(p)) continue;
            size_t at=0;
            while ((at=low_deny.find(p,at))!=std::string::npos) {
                if (aintent::direct_at(low_deny,at)) return Consent::DENY;
                ++at;
            }
        }
    }

    // 2. The away-grant — YOUR exact-words family, never implied: it must say
    //    looking AND say while-gone in the same breath.
    {
        static const char *away[] = {
            "while i'm gone", "while im gone", "while i am gone",
            "while i'm away", "while im away", "while i am away",
            "when i'm gone",  "when im gone",  "when i'm away", "when im away",
            // r21-full.14 (C2): the S16 miss — "Feel free to look while I'm
            // OUT." carried an absence scope in a wording this list did not
            // know, fell through to the agreement forms, and committed an
            // immediate look while he was still mid-exit (camera at the
            // ceiling, "the room's empty now" with his shoulder in frame).
            "while i'm out",  "while im out",  "while i am out",
            "when i'm out",   "when im out",
            "while i'm not here", "when i'm not here", "while i am not here",
        };
        // r21-full.4 (RC6): the looking must be ADDRESSED TO HER and sit in the
        // same clause as the while-gone phrase — see addressed_look_grant_.
        LexGuards g_away;                                        // R7-A
        g_away.addressed_grant = true;
        g_away.no_other_medium = true;
        g_away.no_phrasal_clause = true;
        if (lex_any_(low, away, g_away)) return Consent::AWAY;
    }

    // 3. A standing grant for the session — explicit enough to stand alone.
    // ── r20p3.14 (RV6): a standing grant has to be ABOUT looking ────────────
    //
    // The away-grant two blocks up is correctly gated on has("look"); this list
    // was scanned unconditionally, and most of its entries are ordinary English.
    // Measured, with ask_live FALSE — she had asked nothing:
    //
    //   "I was up all night with the baby."                    -> SESSION
    //   "It rained all night and I barely slept."              -> SESSION
    //   "Come round any time you want."                        -> SESSION
    //   "You don't have to ask permission to disagree with me." -> SESSION
    //   "The whole session was a waste of time."               -> SESSION
    //
    // Each of those set perm.grant = 1, logged "she may look whenever she wants
    // this session", and fired the camera on the spot if her urge had already
    // crossed. The entire point of this machine is that photographing him takes
    // his words; "it rained all night" is not his words.
    //
    // The four self-naming forms stay ungated — they say "look" themselves. The
    // rest now require a looking word in the same breath, exactly as the
    // away-grant does.
    //
    // ── r20p3.14.3 (RV12): saying "look" does not settle what she looks AT ───
    //
    // RV6 assumed a self-naming form was safe because it contains the word.
    // S10 disproved it. The turn was:
    //
    //   "To implement this would first be to give you read-only web access, so
    //    you can read web pages and search for content and LOOK WHENEVER YOU
    //    WANT, whenever you're curious about something."
    //
    // has("look whenever") matched, perm.grant went to 1, and the log said
    // "she may look whenever she wants this session" — a standing CAMERA
    // permission out of a sentence about a browser. Nothing came of it in S10
    // because she never spent a look, but a standing grant plus an urge already
    // over threshold fires the camera on the spot, and a kept frame is written
    // to an append-only store. Photographing him is supposed to take his words.
    //
    // The rule is deliberately ASYMMETRIC. A clause naming a DIFFERENT medium
    // cancels the grant; a clause naming NO object at all still grants, because
    // "You can look whenever you want, I don't mind" is a real grant and always
    // has been — requiring a camera word would break every natural way of
    // saying yes. Where he names both, the grant stands: he said the word.
    //
    // Scoped to the clause around the match rather than the whole turn, so
    // "Search the web whenever you want. And you can look at me any time."
    // still grants from its second sentence.
    // r20p3.14.3 (RV12c): the "look AT ME ..." forms are added here, and the
    // reason is worth recording because it is the same lesson twice.
    //
    // "Search the web whenever you want. And you can look at me any time."
    // granted a session on r20p3.14.2 — but from the WRONG clause: "whenever
    // you want" matched in the sentence about the browser, and has("look") was
    // satisfied by the other sentence entirely. Right answer, wrong reasoning.
    // Once RV12 scopes the guard to the clause, that accident stops working and
    // the sentence would fall through to a single look — a real standing grant
    // downgraded, which is a reduction.
    //
    // So the clause that actually means it has to be able to grant on its own,
    // and "look at me any time" is how people say this. These are as
    // self-naming as the four above and they carry " me", so the guard can
    // never cancel them.
    static const char *session_always[] = {
        "look whenever", "look any time", "look anytime", "you can always look",
        "look at me whenever", "look at me any time", "look at me anytime",
    };
    LexGuards g_sess_always;                                     // R7-A
    g_sess_always.directed_camera_grant = true;
    g_sess_always.no_other_medium = true;
    // r21-full.14.1 (review): whole-word, or "You can check Outlook whenever
    // you want." grants a standing SESSION camera licence off an email app —
    // the RV6 class arriving through a substring.
    g_sess_always.whole_word      = true;
    if (lex_any_(low, session_always, g_sess_always)) return Consent::SESSION;

    // r20p3.14.3 (RV12b): the same guard, on the gated list — found by running
    // the S10 sentence through the fixed code and watching it grant anyway.
    //
    // The fix plan scoped RV12 to the ungated list, reasoning that this block
    // was already protected by needing a looking word. It is not: the guard
    // below asks whether the TURN contains "look", and the S10 turn does —
    // "...search for content and look whenever you want..." satisfies both
    // has("look") and "whenever you want", so it granted here even after the
    // ungated list had correctly refused it. A guard that asks whether a
    // looking word is present anywhere cannot distinguish what it is aimed at,
    // which is the whole finding.
    {
        // r21-full.4 (RC6): "all night" / "all session" / "the whole session" are
        // gone from this list. They are ordinary durations that say nothing about
        // looking, and with the whole-turn has("look") gate they were the exact
        // trap RV6 documented, re-armed by any sentence that happens to contain a
        // form of "look": "I was up all night looking after the baby." The four
        // remaining families all address HER, and each is now additionally
        // required to sit in a clause that grants HER a look.
        static const char *session_gated[] = {
            "whenever you want", "whenever you like", "whenever you feel",
            "any time you want", "anytime you want",  "anytime you like",
            "anytime tonight",   "don't have to ask", "dont have to ask",
            "no need to ask",    "as much as you want", "as often as you like",
        };
        LexGuards g_sess;                                        // R7-A
        g_sess.addressed_grant = true;
        g_sess.no_other_medium = true;
        g_sess.no_phrasal_clause = true;
        if (lex_any_(low, session_gated, g_sess)) return Consent::SESSION;
    }

    // 4. One look. Explicit forms always; bare assent only under a live ask.
    static const char *once_always[] = {
        "you can look now", "you may look", "you can look",
        "go ahead and look", "feel free to look", "have a look then",
        "take your look", "take a look", "have a look",
    };
    // ── r20p3.14.7 (RV17): the ONCE list needs the SAME medium guard ──────────
    // RV12 scoped the clause/medium guard onto both session lists but left the
    // one-look list a bare scan, so "If you don't believe me, you can look it up
    // online" and "You can look it up on the web" granted a single CAMERA look
    // from a sentence about a search engine — spending one of her ten looks and
    // pulling a still of him into her context, licensed by the wrong medium.
    // The same asymmetric guard applies: a competing medium in the clause
    // cancels, a bare "You can look." (no other medium) still grants.
    // r21-full.4 (RC7): negation. negated_before_ exists in this file and the
    // keep lexicons use it; consent_parse never did, so "I don't think you can
    // look at it that way" spent a look and pulled a still of him into her
    // context. "one look" is dropped outright — "One look at the schema told me
    // everything" is not an invitation, and every real one-look grant is covered
    // by the addressed forms above it.
    LexGuards g_once;                                            // R7-A
    g_once.directed_camera_grant = true;
    g_once.no_other_medium = true;
    g_once.no_negation     = true;
    g_once.no_phrasal_at   = true;
    // r21-full.14 (C2): an absence qualifier beside ANY matched agreement
    // form means the permission is for the absence, not for this moment —
    // "sure, look while I'm out" arms the away-grant, it does not fire the
    // camera at his back.
    auto absence_scoped = [&]() {
        static const char *scope[] = {
            "while i'm gone", "while im gone", "while i am gone",
            "while i'm away", "while im away", "while i am away",
            "while i'm out",  "while im out",  "while i am out",
            "when i'm gone",  "when im gone",  "when i'm away", "when im away",
            "when i'm out",   "when im out",
            "while i'm not here", "when i'm not here", "while i am not here",
        };
        // r21-full.14.1 (review): the qualifier has to be CLEAN — its own
        // clause free of negation and of the camera-exclusion family. The
        // first cut scanned the whole turn for the bare phrase, so "Sure,
        // just not while I'm gone." parsed as an AWAY grant: an explicit
        // refusal of the absence, inverted into a licence to photograph the
        // empty room. Same for "Sure, but ignore the camera while I'm gone."
        // and "…but the camera stays off while I'm away." A dirty qualifier
        // upgrades NOTHING: the base agreement stays exactly what it was (an
        // immediate yes stays an immediate yes) — refusals must fail toward
        // the narrower permission, never the wider one.
        // R19 (r22.1): RV1 for the qualifier too — "Sure, no worries, even
        // while I'm gone" is the WARMEST form of the away-grant, and the
        // bare " no " in "no worries" dirtied the qualifier and denied it.
        // The scan below runs on the blanked scratch (see qlow_neg at use).
        static const char *qneg[] = {
            " not ", " never ", " don't ", " dont ", " won't ", " wont ",
            " no ",
        };
        static const char *qexcl[] = {
            "not the camera", "not your camera", "no camera", "never the camera",
            "ignore the camera", "ignore your camera", "camera stays off",
            "camera stays shut", "camera off", "camera stays away",
            "except the camera", "but the camera", "but not the camera",
            "leave the camera", "away from the camera", "camera alone",
            "don't touch the camera", "dont touch the camera",
            "without the camera", "camera stays down",
            "eye on the camera", "watch the camera", "mind the camera",
            "look after the camera", "guard the camera",
        };
        for (const char *p : scope) {
            size_t at = 0;
            while ((at = low.find(p, at)) != std::string::npos) {
                std::string cl =
                    " " + clause_at_(low, at, ".!?;,\n", ".!?;\n") + " ";
                // The qualifier's own words are not evidence against it —
                // "while I am NOT here" carries its negation inside the
                // phrase. Blank the matched phrase, then scan what remains.
                const size_t self_at = cl.find(p);
                if (self_at != std::string::npos)
                    for (size_t k = 0; k < std::strlen(p); k++) cl[self_at + k] = '#';
                bool dirty = false;
                // R19 (r22.1): RV1 — the warm "no" compounds are blanked
                // before the negation scan. The camera exclusions below
                // still scan the original clause.
                const std::string cl_neg = blank_affirmative_no_(cl);
                for (const char *n : qneg)
                    if (cl_neg.find(n) != std::string::npos) { dirty = true; break; }
                if (!dirty)
                    for (const char *e : qexcl)
                        if (cl.find(e) != std::string::npos) { dirty = true; break; }
                if (!dirty) return true;
                at += std::strlen(p);
            }
        }
        return false;
    };
    if (lex_any_(low, once_always, g_once))
        return absence_scoped() ? Consent::AWAY : Consent::ONCE;
    if (ask_live) {
        static const char *once_ask[] = {
            " yes ", " yes,", " yes.", " yeah ", " yeah,", " yeah.",
            " yep ", " yep,", " sure ", " sure,", " sure.", " of course",
            " okay ", " okay,", " okay.", " ok ", " ok,", " ok.",
            " alright ", " alright,", " all right ", " go ahead",
            " fine ", " fine,", " fine.", " certainly",
            // r21-full.14.1 (review): ASR ends an enthusiastic assent with
            // '!' and a clipped one with '.'; "Yes!" was parsing as NOTHING
            // and the ask lapsed while he thought he had answered.
            " yes!", " yeah!", " yep!", " yep.", " sure!", " okay!", " ok!",
            " alright!", " alright.", " all right,", " all right.",
            " fine!", " go ahead!", " of course!",
        };
        for (const char *p : once_ask) {
            size_t at = 0;
            while ((at = low.find(p, at)) != std::string::npos) {
                // Assent answers THIS ask only when the speaker owns it.
                // Match coordinates stay in the same byte-preserving buffer;
                // reported/quoted yes and conditional examples cannot grant sight.
                const size_t word = low.find_first_not_of(" ", at);
                const size_t start = low.find_last_of(".!?;\n", at);
                const std::string prefix = low.substr(start == std::string::npos ? 0 : start + 1,
                    at - (start == std::string::npos ? 0 : start + 1));
                bool conditional = false;
                for (const char *w : {"if", "unless", "whether"})
                    if (aintent::has(prefix, w)) conditional = true;
                if (!conditional && aintent::direct_at(low, word) &&
                    camera_grant_directed_(low, word, std::strlen(p)))
                    return absence_scoped() ? Consent::AWAY : Consent::ONCE;
                ++at;
            }
        }
    }
    // r21-full.14.1 (review): an INVITATION scoped to the absence is a grant
    // for the absence. "Take a peek while I'm out." matched no grant lexicon
    // (peek lives in the invitation list), so S16-class phrasings fired the
    // camera at his back AND armed nothing. With a clean qualifier present,
    // the invitation IS the away grant; the invited-look path yields to it
    // (look_invited_now returns false on AWAY), so nothing fires until he is
    // actually gone. Same R7-A guards as the invited-look scan itself.
    if (absence_scoped() &&
        look_invited_scan_(text, [](const std::string &lw, size_t at, size_t n) {
            LexGuards g;
            g.directed_camera_grant = true;
            g.no_clause_negation = true;
            g.no_phrasal_at      = true;
            g.no_other_medium    = true;
            g.inclusive_we       = true;   // r24.20 review (VISION #4b): the same invitation scan
            return lex_ok_(lw, at, n, g);
        }))
        return Consent::AWAY;
    return Consent::NONE;
}

// ── r20p3.14.7 (RV14): an invited-look pattern inside a REVOCATION is not one ──
//
// look_invited is a purely lexical scan: "look at me" matches whether the words
// around it invite or forbid. "Please don't look at me right now, I'm a mess"
// carries "look at me", so the invited-look path fired the camera on the exact
// words that forbade it — the frame was grabbed, encoded into her context and a
// look spent, at the most vulnerable possible moment. The call site checks the
// invitation BEFORE the consent machine, so the DENY the same turn parses to was
// never reached. This composes the two: an invitation stands unless the turn is
// an explicit refusal, in which case the caller must route to the deny path
// (which revokes any grant and starts the refractory) instead of capturing.
// Only DENY cancels — "Don't worry, look at this" is not a refusal to be looked
// at, and still invites. ask_live is threaded through so the bare-"no" family
// is only a refusal while her own ask is still hanging in the air.
inline bool look_invited_now(const std::string &text, bool ask_live) {
    const Consent parsed = consent_parse(text, ask_live);
    if (parsed == Consent::DENY) return false;
    // r21-full.14.1 (review): an absence-scoped turn arms the away path (see
    // consent_parse); the immediate look yields. Before this, "Take a look
    // around while I'm out." both armed AWAY and fired a frame of his
    // shoulder mid-exit — the exact double the C2 comment forbids.
    if (parsed == Consent::AWAY) return false;
    if (parsed == Consent::SESSION) {
        for (const auto &c : aintent::clauses(text)) {
            if (!c.direct()) continue;
            const Consent scoped = consent_parse(c.text, ask_live);
            // A direct invitation such as "Look at this" needs no separate
            // ONCE phrase. Exclude standing grants to keep this recursion
            // bounded and prevent a session licence itself taking a picture.
            if ((scoped == Consent::NONE || scoped == Consent::ONCE) &&
                look_invited_now(c.text, ask_live)) return true;
        }
        return false;
    }
    // r21-full.5 (R5-P): the same three guards the consent path runs, at the
    // offset of the matched invitation. A phrasal "look into it", a negated
    // "don't look at that", and an invitation aimed at a diff or a spreadsheet
    // are not invitations to point a camera at the room. Applied per match, so
    // a guarded phrase earlier in the turn cannot suppress a real invitation
    // later in it.
    // r21-full.7 (R7-A): the same three guards, named rather than spelled.
    // NOT no_negation: its twenty-byte window crosses a comma, and "Don't
    // worry, look at this" is an invitation whose negation belongs to a
    // different clause — only a DENY (parsed above) cancels that. What is
    // refused here is a negation inside the SAME clause.
    return look_invited_scan_(text, [](const std::string &low, size_t at, size_t n) {
        LexGuards g;
        g.no_clause_negation = true;
        g.no_phrasal_at      = true;
        g.no_other_medium    = true;
        g.directed_camera_grant = true;
        g.inclusive_we       = true;   // r24.20 review (VISION #4b): "We should look at this."
        return lex_ok_(low, at, n, g);
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.12 (WO-V2 / S22 §D.1): the ACUITY lexicon — his words ask her to READ
//
// A MODIFIER on a look, never a trigger. With an invitation in the same turn
// the look is taken at the acuity size; with a held frame under two minutes
// old, a standing licence and no new invitation (S22 14:08:41's shape — "tell
// me what each of the bullets says word for word", no "take a look") it may
// commit an URGE re-look at the acuity size: "let me look closer". On its own
// it fires nothing.
//
// Executed over all 115 of his S22 turns (AGENTS/VISION/fix_out.txt §G): fires
// on 14:05:25 ("Can you read any of it? Take a look."), 14:08:41 ("word for
// word") and 14:09:54 (his verdict on the reading — harmless, no invitation
// and the held frame four minutes old); refuses step 80's "Read me that list."
// and step 81's "Read me what your self-description says" — both name memory,
// not sight. Same guard discipline as every lexicon since R7-A: a negation
// cancels, and the not_sight[] table vetoes the reading verbs whose object is a
// list, a mind, the docs or her own record.
inline bool acuity_look_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_ACUITY_LOOK");
        return !(e && e[0] == '0');
    }();
    return on;
}
// ── r24.12 (review): the object-only rows, and the veto every sibling has ────
//
// What was wrong. `ask[]` shipped nineteen rows that are bare OBJECT NOUNS with
// no reading verb and no clause guard — "the title", "the label", "the letters",
// "the caption", "the writing", "the text", "the words on", "the number on",
// "the headline", "the print", "the lettering", "the details of", "in detail",
// "fine detail", "bullet points", "spell it", "the sign ", "closer look",
// "zoom in" — and the three guards below (not_sight[], negated_before_, the
// his_subject lookback) ran ONLY for the entries beginning `read`, because the
// guard block sat inside `if (strncmp(p, "read", 4) == 0)`. Alone among the
// look lexicons in this file, this one never called detail_other_medium_ at
// all. Measured against the probe in R2412/review/cfix: eight ordinary
// developer sentences, none of them about sight, fired it 8/8 — "Walk me
// through the migration in detail.", "Send me the text of the licence.",
// "What's the title of that ticket?", "The print statement is wrong.", "Give
// me bullet points.", "Let's zoom in on the failure.", "We need a closer look
// at the numbers.", "How do you spell it?". Each one buys a 1920-edge look
// (image_tokens_for_edge(1920,1080,1920) = 2040 tokens), an URGE RE-look he
// never asked for while acuity_relook_due holds, and a drain that blocks the
// turn-loop thread for up to acuity_wait_ms where r24.11 blocked for one
// second. r24.11's worst case for the same sentence was nothing at all.
//
// Why the S22 replay could not see it: all 115 of his turns contain none of
// those phrases, and the ten hand negatives were all `read …` forms, so the
// object-only rows were never probed.
//
// What this does. (a) The two universal guards — negated_before_ and
// detail_other_medium_ — now run for EVERY entry, which is the RV9/R7-A
// discipline the rest of the file has had since r21-full.7. (b) The row test
// is stated once and applied: an entry must be evidence that he is asking her
// to READ something in front of the camera — a reading verb ("read the",
// "make out the", "what does it say", "the cover say"), a reading idiom that
// only means printed characters ("word for word", "letter by letter", "fine
// print"), or an imperative to HER eyes ("look closer", "look closely"). A
// bare object noun is ordinary developer speech and is gone. "zoom in" and
// "closer look" go with them: both are sight verbs whose dominant developer
// sense is figurative ("zoom in on the failure", "a closer look at the
// numbers"), and neither is needed — "Zoom in on the fine print." still
// reaches acuity through `fine print`, "Take a closer look at the label."
// through `read the`/`the label say`-shaped asks and the invitation lexicon.
// (c) "the sign " → "the sign say" and "spell it" → "letter by letter": the
// two hand positives that leaned on a deleted row keep firing, on evidence
// instead of on a noun.
//
// This is not a functional reduction under law 2: `acuity_requested` is new in
// r24.12 and unreachable at ATHENA_ACUITY_LOOK=0, so the r24.11 restore is
// byte-exact either way and no new switch is needed (law 1's "pure
// detector/lexicon" clause; a per-phrase gate here would be law 7's bloat).
// The three S22 positives are unchanged: 14:05:25 matches "can you read",
// 14:08:41 "word for word", 14:09:54 "read the " (and survives
// detail_other_medium_ because its clause names the camera).
inline bool acuity_requested(const std::string &text) {
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    static const char *ask[] = {
        "can you read", "could you read", "read any of it", "read it", "read this", "read that",
        "read the ", "read what", "read me what", "what does it say", "what does that say",
        "what does this say", "what do they say", "what's written", "what is written",
        "word for word", "letter by letter", "fine print", "small print", "the cover say",
        // r24.12 (final review): "each of the bullet" is gone — it is a bare
        // object noun of exactly the class the row trim removed, it fires on
        // "walk me through each of the bullet points in the plan", and the
        // S22 positive it was written for (14:08:41, "tell me what each of
        // the bullets says word for word") matches "word for word" anyway.
        // r24.12 (final review): both inflections, because the whole-word
        // guard above means a stem can no longer match inside its own plural
        // ("the sign say" would refuse "Tell me what the sign says").
        "the sign say", "the sign says", "the cover says",
        "make out the", "what it says",
        "look closer", "look closely",
    };
    static const char *not_sight[] = {
        "read me that list", "read me the list", "read my mind", "read the docs", "read the room",
        "read your self", "read your own", "read me your", "read between", "read it back",
        "read that back", "read me what your", "read the log", "read the file", "read the code",
        "read me my", "what your self-description", "read your memory", "read the memory",
    };
    for (const char *n : not_sight) if (low.find(n) != std::string::npos) return false;
    // A `read` with HIM as its subject is him reading, not her: "I read that
    // book last year", "I need to read the label first" (the RV9 discipline
    // look_invited applies to "look", applied to "read").
    // r24.12 (final review): `" you had "` is nine bytes and the lookback
    // window below is eight, so it could never match. Widened with the window
    // rather than dropped — "you had read the label" is his reading.
    static const char *his_subject[] = { " i ", " we ", " he ", " she ", " they ", " i've ", " we've ",
                                         " i'd ", " i had ", " you've ", " you had " };
    for (const char *p : ask) {
        size_t at = 0;
        while ((at = low.find(p, at)) != std::string::npos) {
            const size_t here = at; at += 1;
            // ── r24.12 (final review): THE WORD BOUNDARY ────────────────────
            // This is the one detector in this file that hand-rolls its scan
            // instead of going through lex_ok_, and it shipped without the
            // whole-word guard every other lexicon here has had since
            // r21-full.4 (RC6). Unanchored, "read the/this/that/it" match
            // inside thread, spread, dread, bread, misread, proofread, and
            // "fine print" inside define print — measured: "The thread that
            // owns the queue is the one to check", "Let's spread the load
            // across two workers", "I misread the timing there", "Could you
            // proofread the release note?" all fired, each buying a
            // 2,040-token acuity look and a wait of up to two minutes.
            //
            // The RIGHT boundary is tested only when the entry ends on a word
            // character. Several entries end on a space ("read the ") — there
            // the space IS the boundary, and word_at_'s unconditional right
            // test would refuse "Read the label on the bottle", which is the
            // ask itself. (That is why this is not simply a word_at_ call:
            // measured, it cost two of the ten hand positives and the S22
            // 14:09:54 line.)
            {
                const size_t plen = std::strlen(p);
                const bool lok = here == 0 ||
                    !(std::isalnum((unsigned char) low[here - 1]) || low[here - 1] == '\'');
                const size_t e = here + plen;
                const bool rok = !std::isalnum((unsigned char) p[plen - 1]) || e >= low.size() ||
                    !(std::isalnum((unsigned char) low[e]) || low[e] == '\'');
                if (!lok || !rok) continue;
            }
            if (negated_before_(low, here)) continue;
            // r24.12 (review): the competing-medium veto, for EVERY entry, not
            // just the `read` ones — "read the diff", "what does the log say",
            // "look closer at the spreadsheet" are all somewhere she has no
            // eyes. It cannot cancel a genuine ask: detail_other_medium_
            // returns false the moment the clause names the camera, which is
            // why 14:09:54 ("the current resolution of the camera … read the
            // fine-grained text") still fires.
            if (detail_other_medium_(low, here)) continue;
            if (std::strncmp(p, "read", 4) == 0) {
                if (self_directed_before_(low, here)) continue;
                // r24.12 (final review): 10, not 8 — the longest entry
                // (" you had ") is nine bytes and could never match in an
                // eight-byte window.
                const size_t back = here > 10 ? here - 10 : 0;
                const std::string win = low.substr(back, here - back);   // the bytes before `read`
                bool his = false;
                for (const char *h : his_subject)
                    if (win.size() >= std::strlen(h) &&
                        win.compare(win.size() - std::strlen(h), std::strlen(h), h) == 0) { his = true; break; }
                if (his) continue;
            }
            return true;
        }
    }
    return false;
}
// The re-look's window: an acuity request with no invitation may commit an
// URGE look at the acuity size only while the still she is holding is FRESH —
// under two minutes, the turn the picture arrived in and the next. Beyond that
// the request is about something she has not seen, and only an invitation (or
// her own declared look) reaches the camera. Pure, so the battery can pin it.
inline constexpr long ACUITY_RELOOK_S = 120;
inline bool acuity_relook_due(bool held_valid, long held_age_s) {
    return held_valid && held_age_s >= 0 && held_age_s < ACUITY_RELOOK_S;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.12 (WO-V10 / S22 §C.5.2): the BARE imperative — "Actually, look."
//
// look_invited_now's phr[] table requires deixis or an object on every entry,
// by design (the header's own rule: bare "look" is English's favourite
// discourse marker). S2 13:53:02 "Nope, nope, stop. You're making all this
// stuff up. Actually, look." and 13:54:04 "Look right now." scored zero, and
// she narrated a room she had not seen. What makes the bare word safe is the
// CLAUSE-FINAL rule: the verb is the whole word `look`, nothing before it in
// the clause but a small lead ("actually", "just", "go ahead and", "now",
// "okay", "please", "so", "nope", "stop", …), nothing after it but a small
// tail ("now", "right", "please", "then", "already", "for real", "again",
// "closely", "closer", "and tell me what you see", …). "Look, I just think
// you're overdoing it." — the marker — carries other words and is refused;
// "look at" / "look what" are the existing table's business and are left to
// it; a first-person or noun use anywhere before the verb refuses ("I need to
// look at the calendar first", "One look at the schema", "You look tired").
// DENY/AWAY short-circuit exactly as in look_invited_now. Executed over all
// 115 of his S22 turns: only the two S2 failures change (fix_out.txt §B).
// Composed at the intake (`look_invited_now(...) || look_invited_bare(...)`),
// never a replacement. ATHENA_LOOK_BARE=0 restores r24.11.
inline bool look_bare_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_LOOK_BARE");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline bool look_invited_bare(const std::string &text, bool ask_live) {
    const Consent parsed = consent_parse(text, ask_live);
    if (parsed == Consent::DENY || parsed == Consent::AWAY) return false;
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *tail_ok[] = { "now", "right", "please", "then", "already", "for", "real", "again",
                                     "properly", "actually", "really", "this", "time", "once", "more",
                                     "athena", "and", "tell", "me", "what", "you", "see", "first", "yourself",
                                     "around", "here", "closely", "closer", "carefully", "hard", "harder" };
    static const char *lead_ok[] = { "and", "then", "just", "actually", "now", "okay", "ok", "please", "so",
                                     "go", "ahead", "come", "on", "athena", "hey", "well", "yeah", "yes", "no",
                                     "nope", "stop", "seriously", "really", "finally", "simply", "first" };
    static const char *lead_bad[] = { "don't", "dont", "not", "never", "can't", "cant", "won't", "wont",
                                      "i", "i'll", "i'm", "we", "a", "the", "one", "my", "you", "to", "they",
                                      "he", "she", "it", "if", "when", "let", "me" };
    // sentence by sentence, as the discourse marker and the imperative are
    // both sentence-shaped
    size_t from = 0;
    while (from < low.size()) {
        size_t to = low.find_first_of(".!?\n", from);
        if (to == std::string::npos) to = low.size();
        const size_t sentence_from = from;
        const std::string s = low.substr(from, to - from);
        from = to + 1;
        std::vector<std::string> tok;
        std::vector<size_t> positions;
        for (size_t at = 0; at < s.size();) {
            if (!std::isalnum((unsigned char)s[at]) && s[at] != '\'') { ++at; continue; }
            const size_t start = at++;
            while (at < s.size() && (std::isalnum((unsigned char)s[at]) || s[at] == '\'')) ++at;
            tok.push_back(s.substr(start, at - start));
            positions.push_back(sentence_from + start);
        }
        for (size_t i = 0; i < tok.size(); i++) {
            if (tok[i] != "look") continue;
            // Sentence/token splitting must not turn quoted speech into a
            // physical camera invitation. Retain the original whole-turn
            // position and use the same ownership gate as objectful looks.
            if (!camera_grant_directed_(low, positions[i], 4, false, false, true)) continue;
            bool bad = false;
            for (size_t j = 0; j < i && !bad; j++) {
                const std::string &w = tok[j];
                for (const char *b : lead_bad) if (w == b) { bad = true; break; }
                if (bad) break;
                bool ok = false;
                for (const char *l : lead_ok) if (w == l) { ok = true; break; }
                if (!ok) bad = true;
            }
            if (bad) continue;
            // "look at" / "look what" are the existing list's business; bare
            // means no object word at all
            if (i + 1 < tok.size() && (tok[i + 1] == "at" || tok[i + 1] == "what")) continue;
            // every token after `look` to the end of the sentence must be tail
            bool all_tail = true;
            for (size_t j = i + 1; j < tok.size() && all_tail; j++) {
                bool ok = false;
                for (const char *t : tail_ok) if (tok[j] == t) { ok = true; break; }
                if (!ok) all_tail = false;
            }
            if (!all_tail) continue;                  // "Look, I just think…": the marker
            return true;
        }
    }
    return false;
}

// ── r21-full.6 (R6-AT): what actually answers a yes/no question ──────────────
//
// The keep window asks him a yes/no question about ONE photograph, and it was
// reading his reply with consent_parse — a parser built for a different
// question ("may she look?"), whose entire look vocabulary therefore counted as
// agreement to keep. These two answer the question that was asked. Deliberately
// narrow and deliberately symmetric: an agreement is a short affirmative with
// no other business in it, a refusal is a short negative or an explicit
// objection, and anything longer or more complicated is neither — the window
// stays open and nothing is kept, which is this file's stated tie-break.
// r24.18: permission to keep belongs to the held photograph. The production
// intake retained an old question across replacement, and an unsolicited
// "Don't keep that photo" left that photo available to her own keep. The
// source/ownership fixture drives both actual consumers. Literal zero restores
// the r24.17 question lifetime and answer scans.
inline bool keep_frame_scope_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_KEEP_FRAME_SCOPE");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline bool plain_agreement(const std::string &text) {
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    // An explicit negation anywhere disqualifies the whole turn: "yes, but not
    // that one" is not agreement, and the cost of getting this wrong is a
    // permanent photograph.
    static const char *no_[] = {
        " no ", " no,", " no.", " nope", " nah ", " nah,", " nah.", " don't ",
        " dont ", " do not ", " rather not", " don't want", " dont want",
        " never ", " not ", " n't ", " delete", " get rid", " bin it",
        " throw it", " keep it out", " out of the", " off the",
    };
    // R19 (r22.1): the RV1 rule, applied here too — "No worries, keep it"
    // and "No problem — keep that one" are agreement built out of the word
    // "no". The negation scan runs on the blanked scratch; the affirmative
    // scan below still sees the original.
    const std::string low_neg = blank_affirmative_no_(low);
    for (const char *n : no_) if (low_neg.find(n) != std::string::npos) return false;
    // A turn whose business is LOOKING is not an answer about keeping. "Take a
    // look at that.", "You can look whenever you want.", "Go ahead and look."
    // and "Sure, look at the room." are the four shapes that carry an
    // affirmative token and grant something else entirely — and reading them as
    // agreement is the very confusion this function exists to end.
    static const char *other_business[] = {
        " look", " camera", " peek", " see me", " see us", " see the room",
        " photograph me", " webcam",
    };
    // ── r24.6 (WO-25 / review #45): "it looks good" is a YES, not other business
    //
    // The " look" entry is left-anchored only, so it swallowed "looks",
    // "looked" and "looking" — and "it looks good / that looks lovely / you
    // look happy in it" is the single most likely thing a person says when
    // agreeing to keep a photograph of himself. Right-bounding the token is NOT
    // the fix: it releases the appraisal but simultaneously admits the -ing/-s
    // look GRANTS ("Sure, keep looking.", "Yes, looking is fine.") as agreement
    // to keep a photograph permanently, which is the R6-AT/RV7 failure this
    // function exists to end. So the veto list stays at full breadth and the
    // APPRAISAL is blanked on a scratch copy first — the same span-blanking
    // idiom blank_affirmative_no_ uses two scans above. " well" and " right"
    // are deliberately NOT adjectives here: with them in, "Yes, look right at
    // me." leaks through as agreement to keep.
    static const char *look_stem_[] = { " look", " looks", " looked", " looking" };
    static const char *appraisal_[] = {
        " good", " great", " nice", " lovely", " wonderful", " cute", " sharp",
        " amazing", " perfect", " fine", " happy", " grand", " beautiful",
        " gorgeous",
    };
    std::string low_ob = low;
    for (const char *st : look_stem_) {
        const size_t sn = std::string(st).size();
        size_t at = 0;
        while ((at = low_ob.find(st, at)) != std::string::npos) {
            bool blanked = false;
            for (const char *ad : appraisal_) {
                const size_t an = std::string(ad).size();
                if (low_ob.compare(at + sn, an, ad) == 0) {
                    low_ob.replace(at, sn, std::string(sn, ' '));
                    at += sn; blanked = true; break;
                }
            }
            if (!blanked) at += 1;
        }
    }
    for (const char *o : other_business) if (low_ob.find(o) != std::string::npos) return false;
    // ...and a turn that asks its own question is changing the subject, not
    // answering hers: "Okay, so what were you saying?" A genuine keep answer
    // that also asks something ("Yes — keep it, could you?") still passes,
    // because it names the keeping.
    static const char *keeps[] = { " keep", " save", " retain", " album",
                                   " hold on to", " hold onto" };
    bool names_keeping = false;
    for (const char *k : keeps) if (low.find(k) != std::string::npos) { names_keeping = true; break; }
    if (!names_keeping && low.find('?') != std::string::npos) return false;
    static const char *yes_[] = {
        " yes", " yeah", " yep", " yup", " sure", " of course", " go ahead",
        " please do", " that's fine", " thats fine", " that is fine",
        " fine by me", " i don't mind", " go for it", " why not",
        " absolutely", " definitely", " certainly", " ok ", " ok,", " ok.",
        " okay", " alright", " all right", " sounds good", " that works",
        " i'd like that", " id like that", " keep it", " keep that",
        " keep this", " save it", " save that", " save this",
    };
    for (const char *y : yes_) {
        const size_t n = std::strlen(y);
        size_t at = 0;
        while ((at = low.find(y, at)) != std::string::npos) {
            // A keep answer names the complete affirmative, not its prefix:
            // "yesterday", "surely" and "keep items" authorize no photograph.
            // Entries ending in punctuation/space already carry their boundary.
            const size_t end = at + n;
            const bool whole = !std::isalnum((unsigned char)y[n - 1]) || end >= low.size() ||
                !(std::isalnum((unsigned char)low[end]) || low[end] == '\'');
            if (!keep_frame_scope_on_() ||
                (whole && camera_grant_directed_(low, at, n, false, true))) return true;
            ++at;
        }
    }
    return false;
}
// ── r24.6 (WO-39 / review #38): the keep refusal, on the keep path ──────────
//
// R6-A's deny_keep[] list, moved out of the LOOK consent parser and given the
// one gate it always needed: the call site in talk-llama.cpp is already
// nested inside `if (... vision.keep_perm.ask_live(now_ms))`, so this scan now
// runs only inside a LIVE KEEP WINDOW — a window that exists only after SHE
// asked to keep something.
//
// The list is carried whole, not trimmed: plain_refusal duplicates nine of the
// twenty-five but NOT "no keeping", "do not save", "don't put that in", "not in
// your album", "delete that", "get rid of that", "throw that away", "don't hold
// on to", "don't hang on to", "no albums", "not the album". Dropping any of
// those would be the capability reduction this change exists to avoid.
//
// REQUIRED COMPANION EDIT — without it this IS a regression. The keep-consent
// test in talk-llama.cpp currently reads:
//     if (kc == aseam::Consent::DENY || aseam::plain_refusal(text_heard)) {
// `kc` was the only thing carrying these entries into the keep window. It must
// become:
//     if (kc == aseam::Consent::DENY || aseam::plain_refusal(text_heard) ||
//         aseam::keep_refusal(text_heard)) {
inline bool image_noun_in_(const std::string &cl);
inline bool keep_refusal(const std::string &text, bool image_required = false) {
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    if (camera_preference_refused_(low, /*album=*/true)) return true;
    static const char *deny_keep[] = {
        "don't keep", "dont keep", "do not keep", "no keeping",
        "don't save", "dont save", "do not save", "not that one",
        "don't put that in", "dont put that in", "not in your album",
        "rather you didn't keep", "rather you didnt keep",
        "delete it", "delete that", "get rid of it", "get rid of that",
        "throw it away", "throw that away", "don't hold on to",
        "dont hold on to", "don't hang on to", "dont hang on to",
        "no albums", "not the album",
        // r24.6 (WO-39): the "keep it OUT" shapes. keeps_an_image_'s excl[]
        // already stops these writing a keepsake, so nothing was ever kept
        // over them — but on the answer path they were silence rather than a
        // refusal, so the window stayed open and note_keep_refused never ran.
        // Purely additive: a no that was being read as no-answer is now read
        // as a no.
        "out of your album", "out of my album", "out of the album",
        "off my drive", "off the internet", "keep it out",
    };
    for (const char *p : deny_keep) {
        size_t at = 0;
        while ((at = low.find(p, at)) != std::string::npos) {
            const size_t here = at++;
            if (keep_frame_scope_on_() &&
                !camera_grant_directed_(low, here, std::strlen(p), true, true)) continue;
            // Outside her question, generic "delete it" has no image owner.
            // Keep-shaped deixis or an image object in this same clause does;
            // reuse the existing object grammar, including its other-use veto.
            const std::string cl = keep_clause_(low, here);
            if (!image_required || image_noun_in_(cl) || keeps_an_image_(cl, true)) return true;
        }
    }
    return false;
}

inline bool plain_refusal(const std::string &text) {
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    // R19 (r22.1): RV1 here too — "No worries, keep that one" carried
    // " no " and refused the very keep it was inviting. The compounds are
    // blanked before the scan.
    low = blank_affirmative_no_(low);
    static const char *no_[] = {
        " no ", " no,", " no.", " no!", " nope", " nah ", " nah,", " nah.",
        " rather not", " i'd rather", " id rather", " don't keep", " dont keep",
        " do not keep", " don't save", " dont save", " delete it",
        " get rid of it", " bin it", " throw it away", " not that one",
        " please don't", " please dont", " i'd prefer not", " id prefer not",
        " let it go", " drop it", " forget it", " never mind",
    };
    for (const char *n : no_) {
        size_t at = 0;
        while ((at = low.find(n, at)) != std::string::npos) {
            if (!keep_frame_scope_on_() ||
                camera_grant_directed_(low, at, std::strlen(n), true, true)) return true;
            ++at;
        }
    }
    return false;
}

// The per-session permission state. Injected clocks (ms), no serialization
// surface — a grant is a thing said tonight.
struct LookPermission {
    bool   asked       = false;
    double asked_at_ms = 0;
    double ask_ttl_ms  = 180000;   // an unanswered ask goes stale in 3 min
    int    grant       = 0;        // 0 none, 1 session, 2 session + away
    uint64_t epoch=0; // revocation generation; main-loop owned

    bool ask_live(double now_ms) const {
        return asked && (now_ms - asked_at_ms) <= ask_ttl_ms;
    }
    void note_her_ask(double now_ms) { asked = true; asked_at_ms = now_ms; }

    // Feed his turn through the lexicon and settle state. DENY also revokes a
    // standing grant — "stop looking" means from now on, not just this once.
    Consent answer(const std::string &his_text, double now_ms) {
        Consent c = consent_parse(his_text, ask_live(now_ms));
        // Declining this delivered request does not revoke a standing grant.
        if (c == Consent::DENY && ask_live(now_ms) && consent_parse(his_text, false) != Consent::DENY)
            c = Consent::DEFER;
        switch (c) {
            case Consent::ONCE:    asked = false; break;
            case Consent::SESSION: asked = false; if (grant < 1) grant = 1; break;
            case Consent::AWAY:    asked = false; grant = 2; break;
            case Consent::DENY:    asked = false; grant = 0; ++epoch; break;
            case Consent::DEFER:   asked = false; break;
            case Consent::NONE:    break;
        }
        return c;
    }
};

// ── r20 P3: the perceptual hash — visual prediction error in 64 bits ────────
// Classic dHash (Krawetz): box-downsample the frame to a 9×8 luma grid, one
// bit per horizontal gradient. Pure math over an RGB8 buffer — no ML, no
// dependencies, deterministic, testable with synthetic frames. The Hamming
// distance between two looks IS the "did the room change while I was blind"
// signal; the substrate turns it into surprise and the change line.
inline uint64_t dhash_rgb(const unsigned char *rgb, int nx, int ny) {
    if (!rgb || nx < 9 || ny < 8) return 0;
    double cell[8][9];
    for (int cy = 0; cy < 8; cy++) {
        const int y0 = (int) ((int64_t) cy       * ny / 8);
        const int y1 = (int) ((int64_t) (cy + 1) * ny / 8);
        for (int cx = 0; cx < 9; cx++) {
            const int x0 = (int) ((int64_t) cx       * nx / 9);
            const int x1 = (int) ((int64_t) (cx + 1) * nx / 9);
            double s = 0.0; long n = 0;
            for (int y = y0; y < y1; y++) {
                const unsigned char *row = rgb + 3 * ((size_t) y * (size_t) nx);
                for (int x = x0; x < x1; x++) {
                    const unsigned char *p = row + 3 * (size_t) x;
                    s += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    n++;
                }
            }
            cell[cy][cx] = n ? s / (double) n : 0.0;
        }
    }
    uint64_t h = 0; int bit = 0;
    for (int cy = 0; cy < 8; cy++)
        for (int cx = 0; cx < 8; cx++, bit++)
            if (cell[cy][cx] > cell[cy][cx + 1]) h |= (1ULL << bit);
    return h;
}
inline int hamming64(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b; int n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.12 (WO-V2 / S22 §D.1): the WORKING COPY — what mtmd is handed is a view
//
// The cost of a look is entirely encode + prefill, and it scales with the
// pixels HANDED TO mtmd, not with the pixels captured (capture is ~0.3-0.5 s
// at any size; S22 measured 640x480 = 300 tokens at 5.0 s + 14.2 s, and the
// 864x1080 self-portrait = 918 tokens at 18.7 s + 33 s, so ~20 ms/token to
// encode and a 1080p frame — 2,040 tokens — extrapolates to ≈115 s). With
// WO-V1 the camera delivers 1920x1080 again; a still that costs two minutes
// never lands in the turn that asked for it (every encode over the 1-s drain
// wait DEFERS). So: capture at the camera's best, keep the FULL frame for the
// hold and the album, and hand mtmd a copy resampled to a working long edge
// (--vision-work-edge, 800 → 800x450, which smart_resize rounds to 800x448 =
// 25×14 = 350 tokens ≈ 22 s: the same field of view, better SNR than the raw
// VGA frame S22 lived on). An ACUITY look — his words asked her to READ —
// encodes the acuity size instead (--vision-acuity-edge 1920 = the full frame,
// 2,040 tokens, or a centred crop at native pixels) with its own wait budget.
//
// Pure math over an RGB8 buffer, beside dhash_rgb for the same reason: the
// battery drives it with synthetic frames, and talk-llama.cpp is compiled by
// no test binary. Box (area-average) downscale only — never an upscale, so a
// frame smaller than the edge is handed over untouched (a 640x480 still is
// 300 tokens either way). A step edge survives the box filter to within one
// output column, which is the property the fixture pins.
inline void resample_dims(int nx, int ny, int edge, int *nx2, int *ny2) {
    if (nx2) *nx2 = nx;
    if (ny2) *ny2 = ny;
    if (nx <= 0 || ny <= 0 || edge <= 0) return;
    const int longest = nx >= ny ? nx : ny;
    if (longest <= edge) return;                                   // never upscale
    const double s = (double) edge / (double) longest;
    int w = nx >= ny ? edge : (int) (nx * s + 0.5);
    int h = nx >= ny ? (int) (ny * s + 0.5) : edge;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (nx2) *nx2 = w;
    if (ny2) *ny2 = h;
}
// Returns the resampled RGB8 buffer (nx2 × ny2 × 3), or EMPTY when no resample
// is needed (the caller keeps its original bitmap) or the input is malformed.
inline std::vector<unsigned char> resample_rgb(const unsigned char *rgb, int nx, int ny, int edge,
                                               int *nx2_out, int *ny2_out) {
    std::vector<unsigned char> out;
    int nx2 = nx, ny2 = ny;
    resample_dims(nx, ny, edge, &nx2, &ny2);
    if (nx2_out) *nx2_out = nx2;
    if (ny2_out) *ny2_out = ny2;
    if (!rgb || nx <= 0 || ny <= 0 || (nx2 == nx && ny2 == ny)) return out;
    out.resize((size_t) nx2 * (size_t) ny2 * 3);
    for (int oy = 0; oy < ny2; oy++) {
        const int y0 = (int) ((int64_t) oy       * ny / ny2);
        int       y1 = (int) ((int64_t) (oy + 1) * ny / ny2);
        if (y1 <= y0) y1 = y0 + 1;
        for (int ox = 0; ox < nx2; ox++) {
            const int x0 = (int) ((int64_t) ox       * nx / nx2);
            int       x1 = (int) ((int64_t) (ox + 1) * nx / nx2);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1 && y < ny; y++) {
                const unsigned char *row = rgb + 3 * ((size_t) y * (size_t) nx);
                for (int x = x0; x < x1 && x < nx; x++) {
                    const unsigned char *p = row + 3 * (size_t) x;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            }
            unsigned char *q = out.data() + 3 * ((size_t) oy * (size_t) nx2 + (size_t) ox);
            q[0] = (unsigned char) (n ? (r + n / 2) / n : 0);
            q[1] = (unsigned char) (n ? (g + n / 2) / n : 0);
            q[2] = (unsigned char) (n ? (b + n / 2) / n : 0);
        }
    }
    return out;
}
// The --vision-acuity-mode crop alternative: a centred window of cw × ch at
// NATIVE pixels (a held-up object is centred), clamped to the frame. EMPTY when
// the frame already fits inside the window (nothing to crop) or on bad input.
inline std::vector<unsigned char> crop_rgb_center(const unsigned char *rgb, int nx, int ny,
                                                  int cw, int ch, int *nx2_out, int *ny2_out) {
    std::vector<unsigned char> out;
    if (nx2_out) *nx2_out = nx;
    if (ny2_out) *ny2_out = ny;
    if (!rgb || nx <= 0 || ny <= 0 || cw <= 0 || ch <= 0) return out;
    const int w = cw < nx ? cw : nx, h = ch < ny ? ch : ny;
    if (w == nx && h == ny) return out;
    const int x0 = (nx - w) / 2, y0 = (ny - h) / 2;
    out.resize((size_t) w * (size_t) h * 3);
    for (int y = 0; y < h; y++)
        std::memcpy(out.data() + 3 * ((size_t) y * (size_t) w),
                    rgb + 3 * ((size_t) (y0 + y) * (size_t) nx + (size_t) x0), (size_t) w * 3);
    if (nx2_out) *nx2_out = w;
    if (ny2_out) *ny2_out = h;
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.6 (WO-37 / S19 D4): the dHash cannot see the light change
//
// A dHash bit is `cell[cy][cx] > cell[cy][cx+1]` — a monotone COMPARISON. Any
// order-preserving photometric transform leaves every one of the 64 bits
// intact. Measured against the real 640x480 room keepsake:
//
//     linear gain x1.5 / x2.5 / x4.0     dhash D0   D0   D0
//     +40 luma offset                    dhash D0
//     gamma 0.5 / 0.35                   dhash D3 / D4
//     a person entering 30% of frame     dhash D11
//     a different image entirely         dhash D30-36
//
// So the one visual event both parties discussed at length in S19 — "I think
// the image was too dark... take a look again" answered by "Ah, much better.
// The light caught it this time" — is STRUCTURALLY INVISIBLE to the change
// detector: gain, offset and gamma all measure D0-D4 on the table above, and
// acon::LOOK_CHANGE_BITS is 6. No threshold rescues it, because the dHash is
// not measuring the thing that moved.
// r24.13 (WO-V2, rider): this sentence said "With LOOK_CHANGE_BITS = 12" and
// concluded that the threshold "sits ABOVE a person filling a third of the
// frame" — the value r24.6 drafted against. The constant has been 6 since (it
// is 6 in r24.11 too, so the comment was already stale when WO-37 shipped),
// which is BELOW the D11 of a person filling a third of the frame and still
// above every photometric case in the table. Corrected here rather than left
// for a reader to trip over; the code was always right.
//
// The dHash is not replaced — it is the right instrument for structural change
// and its invariance to exposure is a FEATURE for that job. What is added is a
// second channel that sees exactly what the first is blind to, reported in the
// same 0..64 units so one threshold can serve both and the two can be combined
// without a second constant to keep in sync.
struct PhotoSig {
    bool  valid = false;
    float mean  = 0.0f;         // mean luma, 0..255
    float hist[8] = {0,0,0,0,0,0,0,0};   // 8-bin luma histogram, normalized
};

// Mean luma plus a coarse (8-bin) luma histogram. Subsampled on a fixed stride
// so the cost does not scale with the frame — a 1080p frame and a 480p frame do
// the same amount of work, and the result is deterministic.
inline PhotoSig photo_sig_rgb(const unsigned char *rgb, int nx, int ny) {
    PhotoSig s;
    if (!rgb || nx <= 0 || ny <= 0) return s;
    const int sx = (nx / 160) < 1 ? 1 : (nx / 160);
    const int sy = (ny / 120) < 1 ? 1 : (ny / 120);
    double sum = 0.0; long n = 0; long bins[8] = {0,0,0,0,0,0,0,0};
    for (int y = 0; y < ny; y += sy) {
        const unsigned char *row = rgb + 3 * ((size_t) y * (size_t) nx);
        for (int x = 0; x < nx; x += sx) {
            const unsigned char *p = row + 3 * (size_t) x;
            const double l = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            sum += l; n++;
            int b = (int) (l / 32.0); if (b < 0) b = 0; if (b > 7) b = 7;
            bins[b]++;
        }
    }
    if (!n) return s;
    s.valid = true;
    s.mean  = (float) (sum / (double) n);
    for (int i = 0; i < 8; i++) s.hist[i] = (float) ((double) bins[i] / (double) n);
    return s;
}

// Distance between two photometric signatures, in dHash-BIT-EQUIVALENT units
// (0..64), so it can be compared against LOOK_CHANGE_BITS directly.
//
// Two terms, and the larger wins rather than their sum: a pure gain moves the
// mean a lot and can leave the histogram SHAPE similar, while a light coming on
// in one corner barely moves the mean and moves the histogram a great deal.
// Summing them would make a change that shows in both look twice as big as it
// is; taking the max means each term answers its own question.
//
//   mean term: |dmean| / 255 * 128  — a +40 offset (the measured "too dark"
//              case) scores 20, an imperceptible +2 scores 1.
//   hist term: EARTH-MOVER distance over the 8 bins (sum of |cumulative
//              differences|, 0..7 bins) * 9.
//
// The histogram term is EMD and not bin-wise L1, and that is not a refinement —
// bin-wise L1 is WRONG here and I measured it being wrong. Hard 32-level bins
// are not shift-invariant: a uniform +12 luma offset, which is barely
// perceptible and is not an event, pushes ~27% of pixels across a bin boundary
// and scores L1 0.54 -> 13, above any usable threshold, while the mean term
// correctly scores it 6. EMD asks how FAR the mass moved rather than how much
// of it crossed an arbitrary line, so the same +12 offset scores 3 and a bright
// card landing over a quarter of a dark frame — mass moving five bins — scores
// 11. The mean term keeps the uniform case; EMD keeps the redistribution case.
//
// The scale factors were fitted to the measured distribution, not chosen: they
// put every real exposure event above 7 and every no-change control below 3.
inline int photo_delta(const PhotoSig &a, const PhotoSig &b) {
    if (!a.valid || !b.valid) return 0;         // unknown is never "changed"
    auto absd = [](double v) { return v < 0.0 ? -v : v; };   // no <cmath> needed
    const double dm = absd((double) a.mean - (double) b.mean) / 255.0 * 128.0;
    double ca = 0.0, cb = 0.0, emd = 0.0;
    for (int i = 0; i < 8; i++) {
        ca += a.hist[i]; cb += b.hist[i];
        emd += absd(ca - cb);
    }
    const double dh = emd * 9.0;
    double d = dm > dh ? dm : dh;
    if (d > 64.0) d = 64.0;
    return (int) (d + 0.5);
}

// The detector is the two channels together. `hamming` keeps its SENTINEL
// meaning: a negative value is "no prior scene", not a distance, and it must
// stay negative on the way out so acon's `const bool first = hamming < 0;`
// still reads it. Only the CHANGED verdict is widened.
inline int look_change_score(int hamming, int photo) {
    if (hamming < 0) return hamming;            // sentinel passes through intact
    return hamming > photo ? hamming : photo;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.14 (WO-R3 / CONSCIOUSNESS.md §"Gaps that remain real"): THE REGION CHANNEL
//
// What was wrong. r24.13 (WO-V2) got the eye from a bit to a magnitude and a
// direction: she can say HOW MUCH the room changed and whether the light went
// up or down. She still cannot say WHERE, and — the part that matters more —
// she cannot tell "one thing moved over there" from "the lights changed", which
// are perceptually different events that a person separates instantly. Both
// instruments are whole-frame by construction: `dhash_rgb` reduces the frame to
// 64 monotone comparisons and `hamming64` reduces THOSE to a popcount, and
// `photo_sig_rgb` is one global histogram. The 8x8 grid the dHash bits come
// from already carries where the structure moved and the popcount throws it
// away — measured over the real keepsakes, an entering figure and a whole-scene
// swap can both score in the twenties while the bits that flipped sit in
// completely different places on the grid.
//
// What this adds. A second, independent decomposition of the SAME buffer, on a
// RGN_G x RGN_G tile grid, computed once per look beside the two whole-frame
// signatures and never in place of them. Per tile it keeps
//   * a PhotoSig — the shipped struct, so the shipped `photo_delta` is what
//     prices a tile's light (one implementation of EMD in the build, not two),
//   * a contrast-normalised RGN_SUB x RGN_SUB grid of sub-cell luma, which is
//     the structural half. The point of the normalisation is that the
//     photometric term already measures the affine part, so the structural term
//     must not, or a webcam's auto-exposure would read as a thing that moved.
//     BE EXACT ABOUT HOW FAR THAT GOES (review B§5). Under c -> a*c + b the
//     mean and every deviation scale correctly, but the divisor is
//     `v + RGN_FLAT_K`, which becomes `a*v + 8` and not `a*(v + 8)` — so the
//     term is EXACT under an offset and only APPROXIMATE under a gain. The +8
//     is what breaks the gain, deliberately: it is the same +8 that makes a
//     flat wall normalise toward zero, which is the behaviour four lines below
//     this one. The residual is bounded and worth writing down. For a pure
//     per-tile gain `a` on a tile of contrast `v`,
//          d(a,v) = 256*v*|a-1| / ((a*v + 8)*(v + 8))
//     which is maximised at v = 8/sqrt(a) and takes the value
//          d_max(a) = 32*(sqrt(a) - 1)/(sqrt(a) + 1).
//     That is under LOOK_CHANGE_BITS for a < 2.14 and under the FIGURE floor
//     (2*gate) for a < 4.84, and it peaks on a LOW-TEXTURE tile — a flat tile
//     still scores 0 and a busy one scores little, so the exposed band is
//     narrow and in the middle. A global gain moves the whole-frame score too
//     and routes to the changed arm; the residual's exposure is to a LOCAL
//     gain of roughly 2x or more, which is what the localised-nuisance family
//     added to the adversary set below is there to price.
//
// Why a grid and not segmentation. A tile grid is not segmentation, not object
// permanence and not tracking — see region_read's own note. It is the largest
// amount of spatial organisation that can be had for one extra pass over a
// buffer that is already in cache, with no model, no library and no state
// carried between looks beyond one signature.
//
// MEASURED, over the seven real 640x480 keepsakes from S22 (the frames her
// camera actually produced) and 8,400 adversary pairs in which THE CAMERA AND
// THE SENSOR changed and the room did not — sensor noise to sigma 14,
// auto-exposure drift, auto-white-balance drift, a 1-3 px camera nudge, a
// defocus, and a real JPEG encode at each source file's OWN quantiser tables.
// SAY WHAT THAT SET IS AND IS NOT (review C§5). Every one of those six families
// is a GLOBAL transform, so none of them can concentrate delta into four or
// fewer of sixteen tiles while the other twelve stay under the gate — which is
// what FIGURE means. The zero below is therefore evidence about global nuisance
// and about nothing else; it is NOT evidence about the localised class, and the
// localised class is measured separately three paragraphs down.
//
//   controlled set (126 pairs)  QUIET 49/49 · FIGURE 27/28 · GROUND 49/49
//   adversary set  (8,400)      false FIGURE 0        (95% CI [0, 0.046%])
//                               — SIX GLOBAL FAMILIES, see above
//   what she SAYS (8,400)       a clause r24.13 could not have made, on a room
//                               the camera re-shot unchanged: 0; byte-identical
//                               on 93.4%
//   4x4 vs 3x3                  FIGURE recall 27/28 vs 19/28 — a quadrant event
//                               does not fit a 3x3 grid (four of nine tiles is
//                               neither a minority nor a majority), which is why
//                               RGN_G is 4; 6x6 and 8x8 reach 27/28 and 28/28
//                               and cost 0.60% and 2.22% false FIGURE
//   cost at 640x480             1.16 ms, beside dhash_rgb's 0.52 ms (2.2x), on a
//                               loaded two-core box, against a MEASURED 5,000 ms
//                               encode + prefill (S22 S1-diag 18:41:12): 0.023%.
//                               The ratio is the stable number; the absolute one
//                               moves with the box, and the fixture re-measures
//                               it and prints it rather than quoting this line.
//
// AND THE LOCALISED CLASS, WHICH THE SIX FAMILIES CANNOT PRODUCE (review C§5).
// A shadow crossing part of a wall, a sun line moving, a monitor redrawing in
// one corner: the room does not change, but the light on part of it does, and
// that IS the concentration FIGURE names. Measured in the fixture over 840
// pairs of two families — a soft-edged rectangle of 1/16 to 1/8 of the frame at
// 0.45x to 1.9x, and one TILE of the grid at a gain of 1.5x / 2x / 3x / 5x
// (review B§5's probe):
//
//   FIGURE on a localised nuisance          623/840 = 74.17%   NOT ZERO
//   …and under the whole-frame gate, so it
//     reaches WO-R2's note                  623/840 = 74.17%
//   of those, "one part of the room is
//     brighter/darker than I left it, X"    617/840 = 73.45%   TRUE — the light
//                                                              there did change
//   of those, "SOMETHING MOVED X"           6/840   =  0.71%   FALSE, and this
//                                                              is the number
//                                                              WO-R2's risk
//                                                              turns on
//
// So the honest statement is not "false FIGURE is zero". It is: on global
// camera and sensor nuisance it is zero over 8,400 pairs, and on localised
// light change it is three-quarters — but the clause the localised verdict
// actually renders is the LIGHT one on 617 of those 623, because the note's
// verb is chosen by the loudest tile's signed luma shift against LOOK_LIT_LUMA
// and a light change moves it. The false MOTION claim — "something moved" on a
// room where nothing did — is 6 in 840 of a deliberately aggressive model, and
// it is pinned and printed in the fixture rather than left out of the summary.
//
// AND THE SAME TWO FAMILIES ON THE REAL KEEPSAKES ARE WORSE (r24.14 fix 4).
// The table above is the FIXTURE's synthetic room. h/mkadv.py grew the same two
// families and h/fpr.py the same split, and re-running them over the seven real
// 640x480 keepsakes — 840 localised pairs, the same count — gives:
//
//                                  in-fixture        keepsake-built
//   FIGURE on localised nuisance   623/840 = 74.17%  578/840 = 68.81%  CI [65.60,71.85]
//   the TRUE light clause          617/840 = 73.45%  520/840 = 61.90%
//   the FALSE MOTION claim           6/840 =  0.71%   30/840 =  3.57%  CI [ 2.51, 5.05]
//
// The two confidence intervals for the false motion claim DO NOT OVERLAP, so
// this is a real difference and not sampling noise: on real frames she makes
// that claim about five times more often than the synthetic room suggests,
// because a shadow over already-dark or high-contrast content can move a tile's
// structure without moving its mean luma past LOOK_LIT_LUMA. The honest bracket
// is 0.71% - 3.57% and the upper end is the one to plan against. The global
// zero reproduces exactly on the same run (0/2,520 false FIGURE), which is what
// says the two halves are measuring what they claim to.
// Raw: out/adv_localised.csv, out/h4_fpr_localised.txt.
//
// Restore: ATHENA_LOOK_REGIONS=0 — the pass is never run, every signature stays
// invalid, and every consumer falls back to the r24.13 arm it already had.
static constexpr int   RGN_G       = 4;    // tiles per side
static constexpr int   RGN_SUB     = 4;    // sub-cells per tile side
static constexpr int   RGN_CELLS   = RGN_G * RGN_SUB;    // sub-cells per frame side
static constexpr int   RGN_NC      = RGN_SUB * RGN_SUB;  // sub-cells per tile
// Below this much luma variation a tile is FLAT and its structure is noise.
// It is a denominator, never a threshold, so nothing steps across it: a tile
// whose contrast is far under it normalises to ~0 and compares equal to any
// other flat tile, which is exactly the right answer for a blank wall re-shot
// by a noisy sensor. 8 luma is the value LOOK_LIT_LUMA already calls the edge
// of perceptibility; it is repeated here as its own constant because it is
// doing a different job and the two must be free to move apart.
static constexpr float RGN_FLAT_K  = 8.0f;
// The structural distance is a mean over normalised sub-cells, so it lands in
// roughly [0, 2]. 32 puts it in the same 0..64 bit-equivalent units photo_delta
// returns, which is what lets ONE threshold — LOOK_CHANGE_BITS — serve the
// whole-frame channel, the photometric channel and a tile. Fitted the same way
// photo_delta's scales were: every real event above the gate, every no-change
// control below it (a full frame swap saturates, a sigma-8 re-shoot lands at 1).
static constexpr float RGN_STRUCT_SCALE = 32.0f;

// r24.14 (WO-R3): the switch. A free function because this header has no cfg_,
// exactly like acuity_look_on_ / capture_mjpeg_on_ beside it. EE1: set-but-
// empty or non-numeric is UNSET is default-ON; only a literal 0 turns it off.
inline bool look_regions_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_LOOK_REGIONS");
        return !(e && e[0] == '0');
    }();
    return on;
}

// One frame, decomposed. 1,668 bytes; two live (the rig's previous scene, and
// the capture slot the worker fills) — the same lifetime rule PhotoSig has.
// (1,732 in the implementation round, before review B§6 removed the unread
// `contrast` array below.)
struct RegionSig {
    bool     valid = false;
    PhotoSig tile[RGN_G][RGN_G];              // per-tile light: photo_delta's own input
    // r24.14 (review B§7): `cell` is zero-initialised, and that is not
    // decoration. region_sig_rgb has four early returns (`!rgb`, a frame under
    // RGN_CELLS on either side, and the defensive `tn <= 0`) and each one hands
    // back an INVALID signature that is then stored in EncodeSlot::region and
    // copied into VisionRig::scene_region; without this initialiser those
    // objects carry 1,088 bytes of indeterminate float for their whole
    // lifetime, and a default-constructed EncodeSlot carries them at
    // ATHENA_LOOK_REGIONS=0 for the whole session. Every consumer today refuses
    // on `valid` before it touches the array, so nothing reads it — but
    // IEEE-754 has no trap representation, so neither ASan nor UBSan would ever
    // say so, and the next comparison written without a `valid` check would get
    // a plausible verdict out of garbage. Zeroing 1.1 kB once per look is free
    // beside the pass itself and makes the struct safe to copy and compare
    // unconditionally. Behaviour at every knob setting is unchanged: no path
    // that reads this array can be reached with an invalid signature.
    float    cell[RGN_G][RGN_G][RGN_NC] = {};   // per-tile structure, contrast-normalised
    // r24.14 (review B§6): a `float contrast[RGN_G][RGN_G]` member stood here,
    // written for all sixteen tiles on every look and read by NOTHING — not
    // region_tile_struct, not region_tile_delta, not region_read, not
    // region_trace, not region_where_words, not talk-llama.cpp, not the
    // fixture. Its own declaration said "kept for the diag line" and there is
    // no diag line: WO-R3 decided, deliberately and with its reasons written
    // down, to add no diagnostic field at all this round. Built-but-never-wired
    // is law 6's defect, and of the two repairs — build the consumer the author
    // meant, or admit it was not needed — this took the second. Wiring it would
    // have changed the operator's look line, which is quoted verbatim as an
    // example in three round documents and is the line S23's static-room
    // measurement is meant to read; re-opening that decision inside a fix is
    // scope the review did not ask for. If S23 wants the per-tile contrast it
    // is one accumulation away (`v` in region_sig_rgb, already computed) and
    // should arrive with the measurement that needs it.
};

// ONE pass over the buffer. Cell (cy,cx) of the RGN_CELLS x RGN_CELLS grid
// belongs to tile (cy/RGN_SUB, cx/RGN_SUB), so the sub-cell means and the tile
// histograms are accumulated together and the signature costs one traversal.
//
// Safety: the only pointer arithmetic is the same row/pixel form dhash_rgb
// uses, with size_t promotion so a large frame cannot overflow the offset, and
// the cell bounds come from the same int64 interpolation. A frame with fewer
// than RGN_CELLS pixels on a side cannot be tiled and returns invalid — which
// every consumer reads as "no region opinion", never as "nothing changed".
inline RegionSig region_sig_rgb(const unsigned char *rgb, int nx, int ny) {
    RegionSig s;
    if (!rgb || nx < RGN_CELLS || ny < RGN_CELLS) return s;
    double csum[RGN_G][RGN_G][RGN_NC];
    long   cn  [RGN_G][RGN_G][RGN_NC];
    long   bins[RGN_G][RGN_G][8];
    double tsum[RGN_G][RGN_G];
    long   tn  [RGN_G][RGN_G];
    for (int ty = 0; ty < RGN_G; ty++)
        for (int tx = 0; tx < RGN_G; tx++) {
            tsum[ty][tx] = 0.0; tn[ty][tx] = 0;
            for (int i = 0; i < RGN_NC; i++) { csum[ty][tx][i] = 0.0; cn[ty][tx][i] = 0; }
            for (int i = 0; i < 8; i++) bins[ty][tx][i] = 0;
        }
    for (int cy = 0; cy < RGN_CELLS; cy++) {
        const int y0 = (int) ((int64_t) cy       * ny / RGN_CELLS);
        const int y1 = (int) ((int64_t) (cy + 1) * ny / RGN_CELLS);
        const int ty = cy / RGN_SUB, iy = cy % RGN_SUB;
        for (int cx = 0; cx < RGN_CELLS; cx++) {
            const int x0 = (int) ((int64_t) cx       * nx / RGN_CELLS);
            const int x1 = (int) ((int64_t) (cx + 1) * nx / RGN_CELLS);
            const int tx = cx / RGN_SUB, ix = cx % RGN_SUB;
            const int ci = iy * RGN_SUB + ix;
            double sum = 0.0; long n = 0;
            for (int y = y0; y < y1; y++) {
                const unsigned char *row = rgb + 3 * ((size_t) y * (size_t) nx);
                for (int x = x0; x < x1; x++) {
                    const unsigned char *p = row + 3 * (size_t) x;
                    const double l = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    sum += l; n++;
                    int b = (int) (l / 32.0); if (b < 0) b = 0; if (b > 7) b = 7;
                    bins[ty][tx][b]++;
                }
            }
            csum[ty][tx][ci] += sum; cn[ty][tx][ci] += n;
            tsum[ty][tx]     += sum; tn[ty][tx]     += n;
        }
    }
    for (int ty = 0; ty < RGN_G; ty++)
        for (int tx = 0; tx < RGN_G; tx++) {
            if (tn[ty][tx] <= 0) return s;            // a tile with no pixels: no opinion
            s.tile[ty][tx].valid = true;
            s.tile[ty][tx].mean  = (float) (tsum[ty][tx] / (double) tn[ty][tx]);
            for (int i = 0; i < 8; i++)
                s.tile[ty][tx].hist[i] = (float) ((double) bins[ty][tx][i] / (double) tn[ty][tx]);
            double c[RGN_NC], m = 0.0;
            for (int i = 0; i < RGN_NC; i++) {
                c[i] = cn[ty][tx][i] ? csum[ty][tx][i] / (double) cn[ty][tx][i] : 0.0;
                m += c[i];
            }
            m /= (double) RGN_NC;
            double v = 0.0;
            for (int i = 0; i < RGN_NC; i++) v += (c[i] > m ? c[i] - m : m - c[i]);
            v /= (double) RGN_NC;
            const double k = v + (double) RGN_FLAT_K;
            for (int i = 0; i < RGN_NC; i++) s.cell[ty][tx][i] = (float) ((c[i] - m) / k);
        }
    s.valid = true;
    return s;
}

// One tile's structural distance, in the same 0..64 bit-equivalent units
// photo_delta returns. The tile mean is removed and the tile contrast divides,
// so a per-tile OFFSET — which is half of what a webcam's auto-exposure and
// auto-white-balance do to a still room — cancels out exactly and this term
// reports zero. The other half, a per-tile GAIN, does NOT cancel exactly: the
// divisor is `v + RGN_FLAT_K`, so a gain `a` leaves a residual bounded by
// 32*(sqrt(a) - 1)/(sqrt(a) + 1) — under the gate for a < 2.14, under the
// FIGURE floor for a < 4.84 (review B§5; the derivation is in the block header
// above, and the localised-gain family in the fixture's adversary set measures
// what it costs). Measured over the seven real keepsakes: sigma-8 sensor noise plus
// +/-8 luma of exposure drift and +/-3% of white balance scores 2 at the median
// and never more than 7 across 1,400 draws; a card landing in one tile scores
// 19 to 52. The gate between them is LOOK_CHANGE_BITS, unchanged, at 6.
inline int region_tile_struct(const RegionSig &a, const RegionSig &b, int ty, int tx) {
    if (!a.valid || !b.valid) return 0;
    if (ty < 0 || tx < 0 || ty >= RGN_G || tx >= RGN_G) return 0;
    double d = 0.0;
    for (int i = 0; i < RGN_NC; i++) {
        const double e = (double) a.cell[ty][tx][i] - (double) b.cell[ty][tx][i];
        d += e < 0.0 ? -e : e;
    }
    d = d / (double) RGN_NC * (double) RGN_STRUCT_SCALE;
    if (d > 64.0) d = 64.0;
    return (int) (d + 0.5);
}
// One tile's change: structure or light, whichever moved — look_change_score's
// own "each term answers its own question" rule, applied to a tile.
inline int region_tile_delta(const RegionSig &a, const RegionSig &b, int ty, int tx) {
    if (ty < 0 || tx < 0 || ty >= RGN_G || tx >= RGN_G) return 0;
    const int sd = region_tile_struct(a, b, ty, tx);
    const int pd = (a.valid && b.valid) ? photo_delta(a.tile[ty][tx], b.tile[ty][tx]) : 0;
    return sd > pd ? sd : pd;
}
// The SIGNED luma shift of one tile — the direction, per region. photo_delta
// takes an absolute value, exactly as it does whole-frame, which is why WO-V2
// needed an out-parameter there and why this is its own function here.
inline int region_tile_dmean(const RegionSig &a, const RegionSig &b, int ty, int tx) {
    if (!a.valid || !b.valid) return 0;
    if (ty < 0 || tx < 0 || ty >= RGN_G || tx >= RGN_G) return 0;
    const float d = b.tile[ty][tx].mean - a.tile[ty][tx].mean;
    return (int) (d < 0.0f ? d - 0.5f : d + 0.5f);
}

// ── r24.14 (WO-R3): FIGURE AND GROUND ───────────────────────────────────────
//
// Three verdicts and an abstention, from the G*G tile deltas alone:
//
//   QUIET   nothing crossed the gate anywhere.
//   GROUND  half the frame or more crossed it AND the median tile crossed it —
//           the whole scene moved together, which is what a light change, an
//           exposure step and a completely different room all look like.
//   FIGURE  a QUARTER of the frame or fewer crossed it, the loudest tile is at
//           least TWICE the gate, and it stands at least a gate above the
//           median. One thing moved and the rest of the room held still.
//   MIXED   anything else — and MIXED SAYS NOTHING. That is deliberate: it is
//           where a camera nudge lands (135 of 1,400 nudge pairs), and an
//           abstention there is the difference between a channel that is quiet
//           when it does not know and one that guesses.
//
// The FIGURE rule was built as a LADDER and each rung MEASURED against the
// 8,400-pair adversary set (h/fpr.py; the fixture re-runs the same ablation on
// its own corpus and prints it). READ IT IN BOTH ORDERS — the order the clauses
// are added in is what decides which one gets the credit, and one ordering on
// its own is how a later reader learns to keep the wrong clause (review C§8):
//   "a minority of tiles moved", alone                     479 / 8400 = 5.702%
//     + the loudest stands a gate above the median            1 / 8400 = 0.012%
//     + the loudest is at least twice the gate (SHIPPED)      0 / 8400 = 0.000%
//   "a minority of tiles moved", alone                     479 / 8400 = 5.702%
//     + the loudest is at least twice the gate                0 / 8400 = 0.000%
//     + the loudest stands a gate above the median (SHIPPED)  0 / 8400 = 0.000%
// AT THE SHIPPED FLOOR THE TWICE-THE-GATE RUNG DOES ALL THE REJECTING and the
// median rung is entailed by it (see the redundancy paragraph below), which is
// what the second ordering shows and what the fixture asserts. The "factor of
// ~475" the first ordering suggests for the median rung is an artefact of
// adding it first: measured against a rule that has no floor. What the median
// rung is FOR is stated rather than priced — on a noisy re-shoot the loudest
// tile drifts just over the gate AND SO DOES EVERYTHING ELSE, so what says a
// thing moved is the SPREAD, not the peak — and it becomes load-bearing again
// the instant anyone weakens the floor, which is exactly what the first
// ordering prices. THE CLAUSE TO KEEP IS THE FLOOR. The twice-the-gate rung
// costs one true FIGURE of twenty-eight (a translated patch whose loudest tile
// scored seven, one over the gate) and buys the last false positive; it is
// written as 2 * gate rather than as a constant of its own so it cannot drift
// away from the threshold it is twice of.
//
// BE HONEST ABOUT WHAT IS THEN REDUNDANT. With the twice-the-gate rung in
// place the median rung can no longer FAIL: n_over <= T/4 leaves at least 3T/4
// of the tiles under the gate, so the median is under it, and
// 2*gate - (gate-1) > gate. It is kept because it is the clause that states
// what the rule MEANS, and because it becomes load-bearing again the instant
// anyone weakens the floor — which is exactly what the ladder above prices.
// The fixture measures that redundancy rather than asserting it.
//
// A fourth clause was in the first draft and is NOT here: "and the median tile
// is below the gate". That one is unreachable for ANY gate and any grid — same
// argument, no floor needed — so it could never fail and never told anyone
// anything. Checked over all 8,526 measured pairs: it never once changed a
// verdict. One redundant clause in a perceptual rule is one place for a later
// reader to believe a guarantee that comes from somewhere else; a redundancy
// that is measured, printed and explained is not that.
//
// WHAT THIS IS NOT. It is not segmentation: a tile is a fixed square of the
// frame, so a thing that straddles two tiles is two events and a thing smaller
// than a sixteenth of the frame is diluted inside one. It is not tracking and
// not object permanence: nothing here follows anything between looks, and the
// only state kept is the previous look's signature. And it cannot tell a room
// that moved from a CAMERA that moved — a nudge is measured here as a change in
// the world, and the only reason it does not speak is that it lands in MIXED.
enum RegionShape : int { RGN_QUIET = 0, RGN_FIGURE = 1, RGN_GROUND = 2, RGN_MIXED = 3 };

struct RegionRead {
    bool valid  = false;
    int  shape  = RGN_QUIET;
    int  hi     = 0;      // the loudest tile's delta
    int  med    = 0;      // the median tile's delta
    int  n_over = 0;      // tiles at or past the gate
    int  hi_ty  = 0, hi_tx = 0;
    int  ty0 = 0, ty1 = 0, tx0 = 0, tx1 = 0;   // bounding box of the over-tiles
    int  dmean_hi = 0;    // SIGNED luma shift of the loudest tile: direction, per region
};

inline RegionRead region_read(const RegionSig &a, const RegionSig &b, int gate) {
    RegionRead r;
    if (!a.valid || !b.valid || gate <= 0) return r;
    r.valid = true;
    int flat[RGN_G * RGN_G];
    int n = 0;
    r.ty0 = RGN_G; r.tx0 = RGN_G; r.ty1 = -1; r.tx1 = -1;
    for (int ty = 0; ty < RGN_G; ty++)
        for (int tx = 0; tx < RGN_G; tx++) {
            const int v = region_tile_delta(a, b, ty, tx);
            flat[n++] = v;
            if (v > r.hi) { r.hi = v; r.hi_ty = ty; r.hi_tx = tx; }
            if (v >= gate) {
                r.n_over++;
                if (ty < r.ty0) r.ty0 = ty;
                if (ty > r.ty1) r.ty1 = ty;
                if (tx < r.tx0) r.tx0 = tx;
                if (tx > r.tx1) r.tx1 = tx;
            }
        }
    for (int i = 1; i < n; i++) {                    // insertion sort: n is 16
        const int k = flat[i]; int j = i - 1;
        while (j >= 0 && flat[j] > k) { flat[j + 1] = flat[j]; j--; }
        flat[j + 1] = k;
    }
    r.med      = flat[n / 2];
    r.dmean_hi = region_tile_dmean(a, b, r.hi_ty, r.hi_tx);
    const int T = RGN_G * RGN_G;
    if (r.n_over == 0)                                            r.shape = RGN_QUIET;
    else if (r.n_over * 2 >= T && r.med >= gate)                   r.shape = RGN_GROUND;
    else if (r.n_over * 4 <= T && r.hi >= 2 * gate
             && r.hi - r.med >= gate)                              r.shape = RGN_FIGURE;
    else                                                           r.shape = RGN_MIXED;
    return r;
}

// r24.14 (WO-R3): the forensic half — what the region channel saw, for the
// operator's console and the S23 grep sheet, NEVER for a clause she speaks
// from. Numerals are correct here for the same reason "change Δ%d" is: this is
// a log line and not the field. Returned as a std::string with a leading space
// so a caller can append it unconditionally; empty for an invalid read.
inline std::string region_trace(const RegionRead &r) {
    if (!r.valid) return std::string();
    const char *sh = r.shape == RGN_FIGURE ? "figure"
                   : r.shape == RGN_GROUND ? "ground"
                   : r.shape == RGN_MIXED  ? "mixed"  : "quiet";
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  ", regions %s (loudest %d at r%dc%d luma %+d, median %d, %d of %d over)",
                  sh, r.hi, r.hi_ty, r.hi_tx, r.dmean_hi, r.med, r.n_over, RGN_G * RGN_G);
    return std::string(buf);
}

// ── r24.14 (WO-R1): WHERE, in words ─────────────────────────────────────────
// The place of the change, from the bounding box of the tiles that crossed the
// gate. NO NUMBER REACHES THIS — it is a fixed phrase table, so there is
// nothing for spell_count to spell and the digits-in-frame assertion is
// satisfied by construction rather than by a speller (S14/F18).
//
// "what I could see" is not decoration. The frame's left is the LEFT OF THE
// IMAGE, which is her left only if the reader adopts the camera's point of
// view; a webcam facing a person mirrors it. She has no way to resolve that and
// must not pretend to, so the phrase names the picture and not the room. That
// is the honest form of the only spatial claim this channel can support.
//
// ── r24.14 (review B§3): AND THE FALLBACK ABSTAINS ──────────────────────────
// The whole-frame phrase belongs to GROUND and only to GROUND. It shipped as
// the span fallback's answer too, and the fallback is reachable ONLY on a
// FIGURE read — GROUND returns above it, and the seam maps MIXED and every
// invalid read to LOOK_SHAPE_NONE with an empty `where`, so those never get
// here. FIGURE means "one thing moved and the rest of the room held still",
// so both consuming clauses then said both things in one sentence:
//   "something moved ACROSS THE WHOLE of what I could see while I was not
//    looking — THE REST of the room is as I left it"
//   "the room is not as I left it — ONE PART of it moved, ACROSS THE WHOLE of
//    what I could see, and the rest is as it was"
// Reachable, not exotic: two over-tiles at (1,0) and (2,3) straddle both
// mid-lines, so all four half-plane tests fail, and the box is 2x4 = 8 tiles,
// which is half the grid. A hand at mid-left and a cat at mid-right do it.
//
// The fix is the one MIXED already models. A FIGURE read whose over-tiles
// straddle both mid-lines and whose box covers half the grid is a channel that
// knows something moved and does NOT know where; that is the situation MIXED is
// in, and MIXED's answer is silence. So it returns "" — and an empty `where`
// refuses to arm the note (note_look_taken requires !where.empty()) and skips
// the WO-R1 arm (take_look_change_note requires it too), which falls back to
// r24.13's sentence with its light clause intact. Nothing is invented and
// nothing contradicts itself.
//
// A phrase was considered instead of silence — "in more than one part of what I
// could see" — and rejected, because WO-R1's FIGURE arm hard-codes "one part of
// it moved, " in front of whatever this returns, and "one part of it moved, in
// more than one part of what I could see" is worse than saying nothing.
//
// The small central box keeps its phrase: reaching the fallback at all means
// the box straddles both mid-lines, so under half the grid really is "in the
// middle". MEASURED over the fixture's controlled + localised corpus: the
// half-the-grid branch is reached on ZERO of the FIGURE reads there and the
// middle branch on the ones the box centres — the section prints both counts.
inline const char *region_where_words(const RegionRead &r) {
    if (!r.valid || r.n_over == 0) return "";
    if (r.shape == RGN_GROUND)     return "across the whole of what I could see";
    const bool up = r.ty1 <  RGN_G / 2;
    const bool dn = r.ty0 >= (RGN_G + 1) / 2;
    const bool lf = r.tx1 <  RGN_G / 2;
    const bool rt = r.tx0 >= (RGN_G + 1) / 2;
    if (up && lf) return "in the upper left of what I could see";
    if (up && rt) return "in the upper right of what I could see";
    if (dn && lf) return "in the lower left of what I could see";
    if (dn && rt) return "in the lower right of what I could see";
    if (up)       return "along the top of what I could see";
    if (dn)       return "along the bottom of what I could see";
    if (lf)       return "on the left of what I could see";
    if (rt)       return "on the right of what I could see";
    const int span = (r.ty1 - r.ty0 + 1) * (r.tx1 - r.tx0 + 1);
    return span * 2 >= RGN_G * RGN_G ? ""                       // review B§3: abstain
                                     : "in the middle of what I could see";
}


// ── r20 P3: the album lexicons ──────────────────────────────────────────────
// Keeping is deliberate and announced. HER forms declare it; HIS forms invite
// it. Bare "keep it" is deliberately absent — English uses it for everything
// ("keep it up", "keep it down", "keep it together") and a keepsake should
// never be an idiom's side effect.
// ── r21-full.6 (R6-B): one image-object test, used by every keep door ───────
//
// R5-R built this predicate and wired it into keep_asked's clause-scoped
// FALLBACK only. The three doors that actually write keepsakes.tsv — the
// primary ask[] list, keep_declared and keep_invited — were left as bare
// whole-turn substring scans, and keepsakes.tsv is append-only and re-rendered
// into her prompt prefix at every startup, so a false keep is permanent.
// Measured: keep_declared 10% precision, keep_invited 29%, keep_asked 52%.
// "I'll keep it short", "Let me save this file first", "Can I keep going for
// another minute?", "Keep that one in mind for later" all wrote the file.
//
// The discriminator is the same one R5-R found: a keeping verb has to carry an
// IMAGE object, or close on a bare demonstrative. "keep this one" closes;
// "keep it short" does not. Hoisted to a free function so every door asks it.
// ── r24.7 (WO-81(3) / S20 §5.14): `held` — the anaphora arm ────────────────
// S20, the family-photo keep: "Can I keep this one as well?" armed nothing —
// the object tokens [this one as well] are four, and the bare-demonstrative
// arm below caps at three, so her ask fell through and only his wording saved
// the keep. With a FRESH LOOK actually held, "this one" can only mean the
// frame in hand — so `held` lifts the all-deictic cap to the collector's own
// bound (six) and nothing else. Every token must still be deixis or
// politeness from deic[]: "keep it down", "keep it under wraps" carry content
// words and still fail, held or not. Default false = the r24.6 test,
// byte-identical; only keep_asked passes true, and only when the rig holds a
// fresh frame — arming on imageless talk stays impossible by construction.
inline bool keeps_an_image_(const std::string &cl, bool held = false) {
    static const char *verb[] = { "keep", "keeping", "retain", "hold on to",
                                  "hold onto", "hang on to", "hang onto", "save",
                                  // r21-full.6 (R6-AM): the PAST forms. ask2[]
                                  // is written in them ("would you mind if i
                                  // held on to this"), so scoping that list to
                                  // a clause carrying an image object silently
                                  // killed every one of its honest asks until
                                  // the verb list could see the tense it uses.
                                  "held on to", "held onto", "hung on to",
                                  "hung onto", "saved", "kept" };
    static const char *image[] = { "picture", "pictures", "photo", "photos",
                                   "photograph", "image", "images", "frame",
                                   "still", "album", "shot", "snap" };
    // A bare demonstrative counts only when it CLOSES the phrase: "can I
    // keep this one" is a keep-ask, "I'll keep it short" is not, and the
    // difference is entirely what follows the pronoun.
    static const char *deic[] = { "this", "that", "it", "these", "those",
                                  "them", "one", "the", "my", "mine",
                                  // r21-full.6 (R6-B): sentence adverbs and
                                  // politeness are stances, not objects. "keep
                                  // that one anyway", "keep this too", "keep
                                  // that, if you don't mind" all name the same
                                  // image; what follows the demonstrative is
                                  // how he feels about it, not what it is.
                                  "anyway", "too", "please", "instead", "as",
                                  "well", "though", "then", "now", "if", "you",
                                  "dont", "mind", "for", "me", "us" };
    // ── r21-full.6 (R6-AL): EXCLUSION is not an invitation ───────────────────
    // English says "keep it" and "keep it OUT OF the album" with the same verb
    // and the same object, and only the preposition tells them apart. Nothing
    // here modelled that, and the bare-"album" short-circuit on the next line
    // fired before any verb or object test ran — so "Please keep that photo out
    // of your album", "Keep that picture off the internet", "Keep that one away
    // from the album, okay?" and "You can keep that, but it never goes in the
    // album" all read as invitations to keep. Six of seven exclusion phrasings
    // wrote keepsakes.tsv, which is append-only, re-rendered into the prompt at
    // every startup, and reached on a path (keep_invited) that never consults
    // consent_parse — so deny_keep[] could not save them either. These are the
    // sentences people actually use to refuse; the file's own tie-break says
    // the tie goes to NOT keeping, and this is not even a tie.
    {
        static const char *excl[] = {
            "out of the", "out of your", "out of my", "out of our",
            "off the", "off your", "off my", "off of",
            "away from", "not in the", "not in your", "never goes in",
            "doesn't go in", "does not go in", "dont go in", "don't go in",
            "nowhere near", "somewhere i can delete", "so i can delete",
            "where i can delete", "i can delete it", "delete it later",
            "but it never", "but not in", "just not in", "except the album",
            "not the album", "not in an album", "off the album",
            "out of that", "off that",
        };
        for (const char *e : excl)
            if (cl.find(e) != std::string::npos) return false;
    }
    if (cl.find("album") != std::string::npos) return true;
    // R6-B: two idioms that name an image with no keeping VERB at all.
    if (cl.find("a keeper") != std::string::npos) return true;
    // R6-B: ...and a use the object is plainly being kept FOR. "Hold on to that
    // one, we will need it in the meeting" has an image-shaped object and a
    // purpose that is not an image. This is the competing-medium argument
    // detail_other_medium_ already makes for looking, made once for keeping.
    {
        static const char *other_use[] = {
            "meeting", "retro", "sprint", "standup", "in mind", "for later",
            "notes", "the file", "the draft", "the doc", "the ticket",
            "the agenda", "the invoice", "the receipt", "the email",
            "the thread", "the branch", "the commit", "the spreadsheet",
        };
        for (const char *u : other_use)
            if (cl.find(u) != std::string::npos) return false;
    }
    for (const char *v : verb) {
        size_t at = cl.find(v);
        if (at == std::string::npos) continue;
        // whole word on the left
        if (at > 0 && std::isalnum((unsigned char) cl[at - 1])) continue;
        const size_t n = std::char_traits<char>::length(v);
        std::vector<std::string> tok;
        std::string cur;
        // R6-B: the OBJECT ends at a comma. "keep that, if you don't mind" is a
        // demonstrative object followed by a politeness clause, and reading the
        // politeness as part of the object is what made it fail the test.
        size_t stop = cl.find(',', at + n);
        if (stop == std::string::npos) stop = cl.size();
        for (size_t i = at + n; i <= stop && tok.size() < 6; i++) {
            const char c = (i < stop) ? cl[i] : ' ';
            if (std::isalnum((unsigned char) c)) cur += (char) std::tolower((unsigned char) c);
            else if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
        }
        if (tok.empty()) continue;
        bool has_image = false;
        for (size_t i = 0; i < tok.size() && i < 4; i++)
            for (const char *g : image) if (tok[i] == g) { has_image = true; break; }
        if (has_image) return true;
        // r24.7 (WO-81(3)): three is the tightest cap that holds every real
        // form when nothing is held; with a fresh frame in hand the politeness
        // tail may run to the collector's own bound ("this one as well",
        // "that one for me too") without changing what the tokens must BE.
        if (tok.size() <= (held ? (size_t) 6 : (size_t) 3)) {
            bool all_deic = true;
            for (const auto &t : tok) {
                bool ok = false;
                for (const char *g : deic) if (t == g) { ok = true; break; }
                if (!ok) { all_deic = false; break; }
            }
            if (all_deic) return true;
        }
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.12 (WO-V4 / S22 §C.5.6): the bounded self-keep — an idiom is not a keep
//
// S2 14:20:45: "…And we start with good news. I'll hold onto that." — about
// the evening ritual, a PROMISE — and the log reads `[said] …` → `[vision]
// KEPT ks-1788199525.jpg`: a photograph of a book cover, taken 895 s earlier,
// went into her permanent album off a figure of speech. keep_declared has
// "i'll hold onto that" in keep[], and keeps_an_image_ accepts a bare
// demonstrative object ("that") — the spec (CHANGES-CONSCIOUSNESS.md, the F3
// paragraph) says "never idioms", and the hold window was refreshed by
// image_still_in_play on the bare verb "keep" ("we keep it", about the
// ritual) inside the 4×window lifetime.
//
// The rule that restores the spec: a keep whose object is a BARE DEICTIC
// (that / it / this one) needs an IMAGE ANTECEDENT — an image noun in the
// same sentence or the one before (picture, photo, image, still, frame, shot,
// album, what I saw/see, the one you took, what you showed me, your face…),
// OR a look that landed within ATHENA_KEEP_DEICTIC_S (180 s: the turn the
// picture arrived in and the next). "I'll keep this picture." keeps; "What I
// saw just now… I'll keep that." keeps; "I want to keep this one." thirty
// seconds after a look keeps; "It's a promise. I'll hold onto that." does
// not. `look_age_s` is the caller's `time(0) − held().taken_at`; the default
// (-1) means the caller did not ask the question and is the r24.11 detector
// byte for byte — every existing fixture and every r24.11 caller keeps its
// verdict. The one production caller (the self-keep path in talk-llama.cpp,
// gated on held_fresh) always passes the real age. ATHENA_KEEP_ANTECEDENT=0
// restores r24.11 for that caller too.
inline bool keep_antecedent_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_KEEP_ANTECEDENT");
        return !(e && e[0] == '0');
    }();
    return on;
}
// ── r24.13 (WO-W6): the hold window goes back to being loose ────────────────
// r24.12's WO-V4 made ONE decision correctly — a bare-deictic keep needs an
// image antecedent — and took a second one with it that it did not need. Both
// halves hung off `ATHENA_KEEP_ANTECEDENT`, so narrowing the KEEP also
// narrowed `image_still_in_play`, whose own comment calls it "deliberately
// loose, because the cost of a false positive is one frame living a few
// minutes longer in /tmp".
//
// Measured over all 264 real S22 turns (`w_vis`, WIDEN §1.4):
//     r24.11: in_play=39  keep@30s=1  keep@900s=1  self_recall=4
//     r24.12: in_play=25  keep@30s=1  keep@900s=0  self_recall=3
// `keep_declared`'s antecedent test alone takes `keep@900s` from 1 to 0 — it
// carries the whole correctness of the fix on its own. The `in_play` 39 -> 25
// is therefore COLLATERAL, and what it cost is a false NEGATIVE whose price is
// a lost keepsake rather than a few minutes of /tmp: "keep it" / "keep that"
// said right after a look no longer refreshes the hold.
//
// So the two decisions stop sharing one knob. ATHENA_KEEP_ANTECEDENT continues
// to own `keep_declared`, unchanged. ATHENA_KEEP_INPLAY_VERBS owns the hold
// window: ON (default) is the UNION — r24.12's image nouns and keep-shaped
// forms plus r24.11's bare verbs — and `=0` is r24.12's `w2`-only list exactly.
inline bool keep_inplay_verbs_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_KEEP_INPLAY_VERBS");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline long keep_deictic_window_s_() {
    static const long v = []() {
        const char *e = ::getenv("ATHENA_KEEP_DEICTIC_S");
        if (!e || !e[0]) return 180L;
        char *end = nullptr;
        const long n = ::strtol(e, &end, 10);
        return (end && end != e && *end == '\0' && n >= 0 && n <= 3600) ? n : 180L;
    }();
    return v;
}
inline bool image_noun_in_(const std::string &cl) {
    static const char *nouns[] = { "picture", "pictures", "photo", "photos", "photograph", "image", "images",
                                   "frame", "still", "album", "shot", "snap", "snapshot", "what i saw", "what i see",
                                   "what i'm seeing", "what i just saw", "this view", "the view", "keepsake",
                                   "looked at", "the one you took", "what you showed me", "what you're holding",
                                   "what you held up", "your face", "the room i saw", "the scene" };
    for (const char *n : nouns) if (cl.find(n) != std::string::npos) return true;
    return false;
}
inline bool keep_declared(const std::string &text, long look_age_s = -1) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *keep[] = {
        "i want to keep this",  "i want to keep that",  "i want to keep it",
        "i'll keep this",       "i'll keep that",       "i'll keep it",
        "i will keep this",     "i will keep that",     "i will keep it",
        "i'm keeping this",     "i'm keeping that",     "im keeping this",
        "let me keep this",     "let me keep that",     "let me keep it",
        "i'd like to keep",     "i would like to keep",
        "worth keeping",        "i'll hold onto this",  "i'll hold on to this",
        // r19.8 (F3): the retain/save family. S8 used "retain" twice and it was
        // in neither lexicon.
        "i want to retain this","i want to retain that","i'll retain this",
        "i will retain this",   "i'm retaining this",   "let me retain this",
        "i want to save this",  "i want to save that",  "i'll save this",
        "i will save this",     "let me save this",     "i'm saving this",
        "this one's a keeper",  "that one's a keeper",  "worth saving",
        "i'll hold on to that", "i'll hold onto that",

        "going in my album",    "into my album",        "in my album now",
        "this one goes in the album", "that one goes in the album",
        "keeping this one",     "keeping that one",
    };
    // RV2: "I don't think I'll keep this one" is not a declaration to keep.
    // R6-B: ...and neither is "I'll keep it short" or "let me save this file".
    // A declaration writes keepsakes.tsv immediately, with no window and no ask,
    // so it needs the strongest evidence of the three, not the weakest: the
    // keeping verb must carry an image object in its own clause.
    // ── r24.6 (WO-39 / review #39): ONE occurrence answers BOTH tests ────────
    // has_unnegated_ walks EVERY occurrence of p and returns true if ANY one
    // survives the negation guard; `at` was low.find(p), the FIRST occurrence,
    // negated or not. So when occurrence #1 is negated and #2 is not, the
    // negation guard passed on #2 while keeps_an_image_ was evaluated on #1's
    // clause — the clause that REFUSES. "I don't want you to keep that picture.
    // Keep that picture out of your album." was written to keepsakes.tsv
    // permanently, and neither occurrence passes both tests on its own.
    // Strictly more capable, not less: a genuine keep in the SECOND clause of a
    // two-clause turn also failed before and now works.
    // r24.12 (WO-V4): the sentence boundaries, for the antecedent test — the
    // image noun may sit in the keep's own sentence or the one before it.
    const bool antecedent = keep_antecedent_on_() && look_age_s >= 0;
    std::vector<size_t> sent_end;                          // index one past each terminator
    if (antecedent)
        for (size_t i = 0; i < low.size(); i++)
            if (low[i] == '.' || low[i] == '!' || low[i] == '?' || low[i] == '\n') sent_end.push_back(i + 1);
    for (const char *p : keep) {
        size_t at = 0;
        while ((at = low.find(p, at)) != std::string::npos) {
            const size_t here = at;
            at += 1;
            if (negated_before_(low, here)) continue;
            const std::string cl = keep_clause_(low, here);
            if (!keeps_an_image_(cl)) continue;                                   // R7-A
            if (!antecedent) return true;                                         // r24.11
            // ── r24.12 (WO-V4): the antecedent test ─────────────────────────
            // album forms and image nouns in the clause are self-evidencing
            if (cl.find("album") != std::string::npos || image_noun_in_(cl)) return true;
            // a bare deictic: the sentence before names an image, or the look is fresh
            size_t si = 0;
            while (si < sent_end.size() && sent_end[si] <= here) si++;          // the sentence `here` is in
            const size_t s_from = si == 0 ? 0 : sent_end[si - 1];
            const size_t s_to   = si < sent_end.size() ? sent_end[si] : low.size();
            if (image_noun_in_(low.substr(s_from, s_to - s_from))) return true;
            if (si > 0) {
                const size_t p_from = si == 1 ? 0 : sent_end[si - 2];
                if (image_noun_in_(low.substr(p_from, s_from - p_from))) return true;
            }
            // ── r24.12 (review): the NEXT-sentence window is gone ────────────
            // It scanned the sentence AFTER the keep as a third antecedent
            // window. WO-V4 states the rule as "an image noun in the same or
            // previous sentence, or a look that landed within
            // ATHENA_KEEP_DEICTIC_S", the VISION report did not declare a
            // third, and it widened the one mechanism this work order exists to
            // narrow. Deleted rather than documented, because this file has
            // already settled the tie it turns on: a declaration writes
            // keepsakes.tsv immediately, permanently, with no window and no ask
            // (see the RV2 note above — "a wrong keep is a permanent consent
            // violation, while a missed keep costs one exchange"), and a noun
            // that arrives after the keep cannot be what he meant when he said
            // it. Nothing measured changes: the 14:19:51 turn this WO was
            // written for ends on "I'll hold onto that." and has no next
            // sentence, and a keep whose image noun really is in the following
            // sentence is still admitted whenever the look is fresh (the
            // deictic window below). ATHENA_KEEP_ANTECEDENT=0 restores r24.11
            // (no antecedent test at all), unchanged.
            if (look_age_s >= 0 && look_age_s <= keep_deictic_window_s_()) return true;
            // else: "I'll hold onto that" about a promise, a ritual, a phrase — not a keep
        }
    }
    return false;
}
// r21-full.14 (A3): a filing label spoken with a keep ("Keep the picture.
// File it under the octopus evening.") belongs in the keepsake's gist — it is
// the name he will fish with next session ("look at the octopus evening one
// again"), and S16's recall failed partly because the label lived only in
// dialogue. Returns the label text or empty.
inline std::string filed_under_label(const std::string &text) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *forms[] = {
        "file it under ", "file this under ", "file that under ",
        "file the picture under ", "filed under ", "call it ", "label it ",
        "picture as ", "photo as ", "image as ", "photograph as ", "snapshot as ",
    };
    for (const char *p : forms) {
        size_t at = 0;
        while ((at = low.find(p, at)) != std::string::npos) {
            const size_t here = at;
            at += strlen(p);
            // r21-full.14.1 (review): a negated christening is not one, and a
            // negated FIRST match must not swallow the real label after it —
            // "Don't call it that — call it the harbor morning." now yields
            // the harbor morning. Also whole-word on the left: "recall it"
            // must never read as "call it".
            if (here > 0 && (std::isalnum((unsigned char) low[here - 1]))) continue;
            if (negated_before_(low, here) || !aintent::direct_at(text,here)) continue;
            size_t s = here + strlen(p);
            size_t e = s;
            while (e < text.size() && e - s < 60 &&
                   text[e] != '.' && text[e] != '!' && text[e] != '?' &&
                   text[e] != ';' && text[e] != '\n') e++;
            std::string label = text.substr(s, e - s);
            while (!label.empty() && (label.front() == ' ' || label.front() == '"' ||
                                      label.front() == '\'')) label.erase(label.begin());
            while (!label.empty() && (label.back() == ' ' || label.back() == '"' ||
                                      label.back() == '\'' || label.back() == ',')) label.pop_back();
            if (label.size() < 3) continue;
            // "call it a night" is an idiom, not a filing label; same for the
            // pronoun heads — a label has to NAME something.
            std::string ll;
            ll.reserve(label.size());
            for (unsigned char c : label) ll += (char) ::tolower(c);
            static const char *idiom[] = {
                "a night", "a day", "it a day", "quits", "even", "that",
                "it", "this", "them", "what you want", "whatever",
            };
            bool idio = false;
            for (const char *w : idiom) if (ll == w) { idio = true; break; }
            if (idio) continue;
            return label;
        }
    }
    return "";
}

// r21-full.14 (A2): `fresh_look` widens the lexicon with the definite-article
// forms RV7b removed. "Keep the picture. File it under the octopus evening."
// was a real invitation two minutes after a committed look, and it was
// silently dropped; she then said "Done. It's kept." with nothing behind it,
// and the false claim became a memory row (S16). The article is ambiguous in
// a vacuum and RESOLVABLE with a frame in hand — which is exactly what the
// flag asserts (the call site gates it on held_fresh). A purpose tail right
// after the noun still refuses: "save the picture for the deck" is about a
// different picture, RV7b's own counterexample, preserved.
inline bool keep_invited(const std::string &text, bool fresh_look = false) {
    const std::string normalized = aintent::lower(text);
    for (const char *verb : {"keep", "save", "retain"})
        for (const char *det : {"this", "that", "the"})
            for (const char *modifier : {"new", "latest", "current", "fresh", "last", "first", "second"}) {
                const std::string prefix=std::string(verb)+" "+det+" "+modifier+" ";
                size_t at=normalized.find(prefix);
                if (at==std::string::npos || !aintent::direct_at(text,at)) continue;
                for (const char *noun : {"picture", "photo", "photograph", "image", "snapshot", "shot"}) {
                    const size_t noun_at=at+prefix.size();
                    if (!word_at_(normalized,noun_at,std::strlen(noun)) || normalized.compare(noun_at,std::strlen(noun),noun)!=0) continue;
                    std::string shaped=normalized;
                    shaped.erase(at+std::strlen(verb)+std::strlen(det)+2,std::strlen(modifier)+1);
                    return keep_invited(shaped,fresh_look);
                }
            }
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    if (fresh_look) {
        static const char *ctx[] = {
            "keep the picture", "keep the photo", "keep the image",
            "keep the shot",    "save the picture", "save the photo",
            "save the image",   "keep the one",
        };
        for (const char *p : ctx) {
            // r21-full.14.1 (review): the widened forms pass the SAME doors
            // as every other keep anchor. The first cut skipped them all, so
            // "Don't save the photo." KEPT the photo — a remembered consent
            // violation — and "Keep the picture out of your album." kept it
            // too. Negation first, then the exclusion shape in the keep clause.
            //
            // ── r24.6 (WO-39 / review #39): per-occurrence, and the WHOLE body
            // moves inside the loop — `tb` is computed from `at`, so a two-line
            // swap that only replaced the find/has_unnegated_ pair would leave
            // the purpose-tail test bound to the wrong anchor.
            //
            // The bespoke " out of" exclusion is replaced by keeps_an_image_ on
            // the same clause. That is required, not cosmetic: iterating
            // occurrences exposes clause #2 to whatever filter is here, and
            // " out of" alone lets "Keep the picture out of your album, please.
            // Keep the picture off my drive too." through, where excl[]'s
            // "off my" refuses it. Strictly stronger, and it is the filter every
            // sibling door already uses.
            size_t at = 0;
            while ((at = low.find(p, at)) != std::string::npos) {
                const size_t here = at;
                at += 1;
                if (negated_before_(low, here)) continue;
                if (vision_permission_scope_on_() &&
                    !camera_grant_directed_(low, here, std::strlen(p), false, true)) continue;
                if (!keeps_an_image_(keep_clause_(low, here))) continue;
                // The purpose tail, bounded at the SENTENCE — a next sentence's
                // "For the record, …" cannot veto an honest keep — and covering
                // the tails the old two literals missed ("for later", "for when
                // the deck is ready").
                const size_t tb = here + strlen(p);
                size_t te = low.find_first_of(".!?;\n", tb);
                if (te == std::string::npos) te = low.size();
                const std::string tail = low.substr(tb, std::min<size_t>(32, te - tb));
                if (tail.find(" for the ")  != std::string::npos ||
                    tail.find(" for your ") != std::string::npos ||
                    tail.find(" for when ") != std::string::npos ||
                    tail.find(" for later") != std::string::npos) continue;
                return true;
            }
        }
    }
    static const char *keep[] = {
        // r20p3.14 (RV7): "you can keep it" / "you should keep it" / "retain it"
        // are gone. The comment below already says bare "keep it" is excluded
        // because English uses it for everything — and these are prefixes of
        // exactly those idioms. Measured, within the ten-minute hold window:
        //   "You should keep it simple."              -> kept the frame
        //   "You can keep it under a hundred lines."  -> kept the frame
        //   "You should keep it up, you're doing well."-> kept the frame
        //   "Retain it for six years, legally."       -> kept the frame
        // keepsakes.tsv is append-only and is re-rendered into the permanent
        // prompt prefix at every startup, so each of those was permanent. The
        // demonstrative forms below cover every real invitation.
        "keep that one",   "keep this one",
        "you can keep that", "you should keep that",
        // RV7b: the definite-article forms go too. "Save the picture for the
        // deck" is about a different picture, and keepsakes.tsv is append-only.
        // The demonstratives below cover every real invitation, and the file's
        // own rule is that where the two cannot be told apart the tie goes to
        // NOT keeping.
        "keep that picture", "keep this picture",
        "keep that image",   "keep that photo",
        "for your album",  "one for the album", "one for your album",
        // r19.8 (F3): his retain/save family — the exact words used in S8.
        // Every form is DEMONSTRATIVE — it points at the image in hand. A bare
        // "you can retain" was tried and rejected: S8's "You can retain images
        // that you want to remember in your memory" is him explaining the
        // capability, and it would have kept the frame before she had said she
        // wanted it. The general statement must keep nothing.
        "retain this one", "retain that one",  "retain this picture",
        "retain that picture", "retain this image", "retain that image",
        "you can retain this", "you can retain that",
        "save that one",   "save this one",    "you can save it",
        // RV7b: "you should save it" and "save the picture" leave with the
        // other bare-it and definite-article forms.
        "save that picture", "save this picture",
        "save that image", "save this image",
        // R6-B: the photo/photograph forms the picture/image forms already had.
        "save that photo", "save this photo", "save that photograph",
        "save this photograph", "keep that photo", "keep this photo",
        "keep that photograph", "keep this photograph",
        "hold on to that one", "hold onto that one", "that one's a keeper",
        // r21-full.6 (R6-AM): the ALBUM forms, which are the plainest
        // invitation English has and were the one family the demonstrative
        // rule never covered — "Put that in the album" kept nothing. Every one
        // is demonstrative and names the album, which is the same evidence bar
        // the picture/photo forms above already meet, and the R6-AL exclusion
        // list vetoes the negated shapes ahead of them.
        "put that in the album", "put this in the album",
        "put that in your album", "put this in your album",
        "that goes in the album", "this goes in the album",
        "that can go in the album", "this can go in the album",
        "that could go in the album", "this could go in the album",
        "that one goes in the album", "this one goes in the album",

    };

    // RV2: a refusal that names the picture is not an invitation to keep it.
    // R6-B: and neither is "Keep that one in mind for later" or "Save this one
    // for the retro" — the demonstrative forms this list is built from are the
    // same ones English uses for ideas. Same clause, same image-object test.
    // ── r24.6 (WO-39 / review #39): ONE occurrence answers BOTH tests ────────
    // has_unnegated_ walks EVERY occurrence of p and returns true if ANY one
    // survives the negation guard; `at` was low.find(p), the FIRST occurrence,
    // negated or not. So when occurrence #1 is negated and #2 is not, the
    // negation guard passed on #2 while keeps_an_image_ was evaluated on #1's
    // clause — the clause that REFUSES. "I don't want you to keep that picture.
    // Keep that picture out of your album." was written to keepsakes.tsv
    // permanently, and neither occurrence passes both tests on its own.
    // Strictly more capable, not less: a genuine keep in the SECOND clause of a
    // two-clause turn also failed before and now works.
    for (const char *p : keep) {
        size_t at = 0;
        while ((at = low.find(p, at)) != std::string::npos) {
            if (!negated_before_(low, at) &&
                (!vision_permission_scope_on_() ||
                 camera_grant_directed_(low, at, std::strlen(p), false, true)) &&
                keeps_an_image_(keep_clause_(low, at))) return true;              // R7-A
            at += 1;
        }
    }
    return false;
}

// ── r19.8 (F2): keeping is a two-turn act, exactly like looking ─────────────
//
// S8: she asked "Does that feel okay to you? Me keeping a picture of you?" and
// he answered "Yeah, that's fine. You can retain this one." Neither half
// matches a one-turn lexicon, and no lexicon ever will — that is simply not how
// permission works in speech. Looking already solves this: she asks, a consent
// window opens, and `consent_parse(text, ask_live=true)` reads his answer,
// widening to bare assent ("yeah", "sure", "that's fine") only while her
// question is still hanging in the air.
//
// Verified against his real words: consent_parse("Yeah, that's fine. You can
// retain this one.", ask_live=true) already returns ONCE. The machinery works;
// keeping simply never used it. This wires it up.
// r24.7 (WO-81(3)): `fresh_look` widens ONLY the R7-A image test's bare-
// demonstrative arm (keeps_an_image_'s `held` — see its comment), and only
// for this detector: her ASK arms a consent window, his answer still decides,
// and the window can only arm at all while the rig actually holds a frame.
// Default false = the r24.6 detector, byte-identical for every other caller.
inline bool keep_asked(const std::string &text, bool fresh_look = false) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    auto has = [&](const char *p) { return low.find(p) != std::string::npos; };
    (void) has;   // R6-AM: ask2[] is now clause-scoped; `ask[]` below still uses it
    // Her interrogative forms. Every one of them is ABOUT keeping and is a
    // question — a statement like "I keep coming back to it" must never arm the
    // window, so no bare "keep" form appears here.
    static const char *ask[] = {
        "can i keep",        "may i keep",         "could i keep",
        "am i allowed to keep", "is it okay if i keep", "is it ok if i keep",
        "would it be okay if i keep", "would it be alright if i keep",
        "do you mind if i keep",  "would you mind if i keep",
        "me keeping",        "if i kept",          "can i hold on to",
        "can i hold onto",   "is it okay for me to keep",
        "okay if i keep",    "alright if i keep",
        "can i save",        "may i save",
        // r24.12 (WO-V9 / S22 §C.5.7): the DELIBERATIVE forms. S1 18:41:40
        // "Should I keep this one too, or just let it be what we saw tonight?"
        // matched nothing here — the list had can/may/could and no "should I"
        // / "want me to" — so the window never armed, his "Don't keep that
        // one." fell to keep_invited (where negated_before_ refused it) and
        // nothing recorded his no: no `he said no to keeping it`, no
        // note_keep_refused, the frame stayed held. Same clause + image test
        // as every row above, so "Should I keep going?", "Should I keep it
        // short?", "Want me to keep an eye on the time?" arm nothing, and her
        // 18:55:10 "Should I keep this one open for a while" — keeping a
        // recalled image OPEN, not a keepsake ask — is refused by
        // keeps_an_image_ on "open"/"while". No switch (a lexicon table).
        "should i keep",     "shall i keep",       "do you want me to keep",
        "want me to keep",   "should i hold on to", "should i hold onto",
        "should i save",     "would you like me to keep",
    };
    // R6-B: the primary list gets the clause test R5-R built for the fallback.
    // "Can I keep going for another minute?", "Could I keep the meeting to
    // fifteen minutes?", "Can I save you some time?" all armed the three-minute
    // keep window, in which any ordinary "sure" writes a permanent keepsake.
    // Eleven of twenty-three hits were false; every one arrived through here.
    for (const char *p : ask) {
        const size_t at = low.find(p);
        if (at == std::string::npos) continue;
        // r21-full.14.1 (review): the keep scope (comma on the left), not the
        // sentence — "The album from the 90s was great, can I keep going for
        // a bit?" was arming the window off the PRECEDING clause's "album",
        // the exact asymmetry the file's own header documents as load-bearing.
        if (keeps_an_image_(keep_clause_(low, at), fresh_look)) return true;   // R7-A
    }
    // ── r21-full.5 (R5-R): the same clause, and an image object ─────────────
    //
    // The fallback was `asks_permission && about_keeping` as two whole-turn
    // scans with no relation between them, and `about_keeping` was satisfied by
    // a bare "keep". English uses "keep" for everything — keep going, keep it
    // short, keep quiet, keep an eye on — and "is that okay?" is her commonest
    // check-in, so 10 of 11 ordinary sentences she says armed the keepsake
    // window: "I'll keep going with the refactor, then. Is that okay?" armed
    // it, and his next "sure" wrote a still of him into keepsakes.tsv, which is
    // append-only and re-rendered into the prompt prefix at every startup
    // forever. That is the permanent-consent-violation class this file has
    // already fixed twice (RV2, RV7), arriving through the third door.
    //
    // Two things fix it, both of which the rest of this file already does:
    // scope the halves to the SAME CLAUSE, and require the keeping word to
    // carry an image object. Then add the natural asks the old list missed and
    // which the scoping makes safe — so the honest asks she could not make
    // before are now available, and the accidental ones are gone.
    static const char *ask2[] = {
        "would you mind if i held on to this", "would you mind if i held onto this",
        "can i hang on to that", "can i hang onto that",
        "is it alright if i put that in my album", "is it okay if i put that in my album",
        "do you mind if i keep the picture", "do you mind if i keep the photo",
        "mind if i keep this one", "mind if i keep that one",
    };
    // ── r21-full.6 (R6-AM): ...and this list never got either of them ────────
    // R6-B scoped `ask[]` and the clause fallback to a clause carrying an image
    // object, and left `ask2[]` a bare whole-turn substring scan. Three of its
    // ten entries name no image at all — "hang on to that", "held on to this",
    // "keep this one" — so "Mind if I keep this one open in a tab?", "Can I
    // hang on to that thought for a second?" and "Would you mind if I held on
    // to this idea until we finish?" armed the three-minute keep window on
    // ordinary turns. Inside that window his next reply is read as an answer
    // about a photograph. Same clause scoping, same image test, same
    // discipline: two lines, and the list keeps every honest ask it had.
    for (const char *p : ask2) {
        const size_t at = low.find(p);
        if (at == std::string::npos) continue;
        if (keeps_an_image_(keep_clause_(low, at), fresh_look)) return true;   // R7-A (14.1: keep scope)
    }

    // Clause-scoped fallback: the permission question and the keeping must be
    // in the same clause or in adjacent ones ("I'd like to keep that picture.
    // Is that okay?"), and the keeping must carry an IMAGE object.
    std::vector<std::string> cls;
    {
        size_t from = 0;
        while (from <= low.size()) {
            size_t to = low.find_first_of(".!?;\n", from);
            if (to == std::string::npos) to = low.size();
            cls.push_back(low.substr(from, to - from));
            if (to >= low.size()) break;
            from = to + 1;
        }
    }
    // R6-B: hoisted to keeps_an_image_ above; kept as a thin alias so the
    // clause-scoped fallback below reads unchanged.
    auto keeps_an_image = [fresh_look](const std::string &cl) {
        return keeps_an_image_(cl, fresh_look);                    // r24.7 (WO-81(3))
    };
    auto asks_permission_in = [](const std::string &cl) {
        static const char *q[] = { "does that feel okay", "is that okay", "is that alright",
                                   "would that be okay", "would that be alright",
                                   "that all right with you" };
        for (const char *g : q) if (cl.find(g) != std::string::npos) return true;
        return false;
    };
    for (size_t i = 0; i < cls.size(); i++) {
        if (!asks_permission_in(cls[i])) continue;
        if (keeps_an_image(cls[i])) return true;
        if (i > 0 && keeps_an_image(cls[i - 1])) return true;
        if (i + 1 < cls.size() && keeps_an_image(cls[i + 1])) return true;
    }
    return false;
}

// The keep-consent window. Same shape and the same TTL as LookPermission: an
// unanswered ask goes stale rather than lingering as a standing licence.
struct KeepPermission {
    bool   asked       = false;
    double asked_at_ms = 0;
    double ask_ttl_ms  = 180000;      // 3 minutes, matching the look ask
    bool ask_live(double now_ms) const {
        return asked && (now_ms - asked_at_ms) <= ask_ttl_ms;
    }
    void note_her_ask(double now_ms) { asked = true; asked_at_ms = now_ms; }
    void clear() { asked = false; asked_at_ms = 0; }
};

// F3: is this turn still about the image she just took? Used only to refresh
// the hold window, never to keep anything — deliberately loose, because the
// cost of a false positive is one frame living a few minutes longer in /tmp.
inline bool image_still_in_play(const std::string &text) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    // ── r24.12 (WO-V4 / S22 §C.5.6): no bare keeping VERBS ───────────────────
    // The spec says the hold "restarts whenever either of them mentions THE
    // IMAGE"; the list refreshed it on the verbs alone, so his "we keep it"
    // about the evening ritual (14:19:44) and her earlier turns kept a
    // fifteen-minute-old frame of a book cover alive for "I'll hold onto
    // that". The verbs go; the image nouns stay; the keep-shaped forms that
    // DO name the image ("keep that one", "keep the picture", "retain this
    // one" — the S8 sentence F3 was built for — "keepsake", "the still") come
    // in. ATHENA_KEEP_ANTECEDENT=0 restores the r24.11 list.
    static const char *w[] = {
        "keep", "keeping", "kept", "retain", "save", "saving", "album",
        "picture", "photo", "image", "snapshot", "the one you took",
        "what you saw", "looked at", "seeing me", "saw me",
    };
    static const char *w2[] = {
        "album", "picture", "photo", "image", "snapshot", "the one you took",
        "what you saw", "looked at", "seeing me", "saw me",
        "keep that one", "keep this one", "keep the picture", "keep the photo",
        "retain this one", "retain that one", "save this one", "save that one",
        "keepsake", "the still",
    };
    if (keep_antecedent_on_()) {
        for (const char *p : w2) if (low.find(p) != std::string::npos) return true;
        // ── r24.13 (WO-W6): …and the bare verbs come back, under their own
        // switch. The UNION, not a replacement: everything `w2` matches still
        // matches, and `w`'s keeping verbs match again on top of it, which is
        // r24.11's list restored for the ONE decision that never needed
        // narrowing. `keep_declared` is untouched and still refuses "I'll hold
        // onto that" about a promise at any look age past the deictic window —
        // that is where the correctness lives, and this function's own comment
        // prices its own false positives at one frame in /tmp.
        // ATHENA_KEEP_INPLAY_VERBS=0 restores r24.12's w2-only list exactly.
        if (keep_inplay_verbs_on_())
            for (const char *p : w) if (low.find(p) != std::string::npos) return true;
        return false;
    }
    for (const char *p : w) if (low.find(p) != std::string::npos) return true;
    return false;
}

// Wanting to look back — hers or his request. Generic forms ("look at it


// again") reach for the freshest keepsake; named forms are matched to a gist.
inline bool recall_determiner_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_DETERMINER");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline bool recall_requested(const std::string &text) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    // Every form names the ALBUM: an "again", a "pull up", or the album/kept
    // words themselves. Bare "look at the picture" is deliberately absent —
    // that is a live look at a physical picture on his wall, the camera's
    // business, not the album's.
    static const char *rec[] = {
        "look at that again",   "look at it again",   "look at that one again",
        "see it again",         "see that again",     "see that one again",
        "look at the picture again", "look at that picture again",
        "see the picture again", "see that picture again",
        "pull up the picture",  "pull up that picture", "pull it up again",
        "pull up the photo",
        // r21-full.9 (R9-C): the bare form. In S13 he said "I have injected
        // your self-image picture into your keepsakes folder. Pull it up." —
        // and nothing opened, because the deictic imperative only existed here
        // with "again" on it. Her reply about "seeing myself" was generated
        // with NO image in front of her at all. Guarded below by the
        // competing-medium veto, so "pull it up on GitHub" stays a browser.
        "pull it up", "pull that up", "pull this up", "pull the image up",
        // r21-full.10 (F7): the PLURAL family. S14: "You now have both the
        // original version of your self-image and the updated version as well
        // in your keepsake album. Pull them up and take a look." — recall only
        // fired because "your self-image" happened to be in the sentence; on
        // any phrasing without it, the plural imperative named nothing here.
        "pull them up", "pull these up", "pull those up", "pull them both up",
        // r24.12 (WO-V6 / S22 §C.5.4): the plural DEICTICS. rec[] knew "look at
        // that/it/that one again" and the F7 "pull them up", but not the plural
        // she actually said — S1 18:43:43 "Okay, let me look at those again."
        // matched nothing. Purely additive; the same guards apply.
        "look at those again", "look at these again", "look at them again",
        "look at both again", "see those again", "see them again", "see both again",
        "look at both of them again", "look at the two again",
        // ...and the self-image family, now that the album can carry one.
        "your self-image", "your self image", "your portrait",
        "picture of yourself", "image of yourself", "look at yourself",
        "open the album",       "open my album",      "open your album",
        "look at your keepsake", "look at my keepsake", "the keepsake of",
        "the picture i kept",   "the picture you kept", "the image i kept",
        "the one i kept",       "what i kept",
        "let me look back at",  "look back at the picture",
        // r24.7 (WO-80(2) / S20 D2): the noun forms the existing families
        // implied but never spelled. S20, 23:31:49 — "Hey, show yourself the
        // picture of flowers again." matched NOTHING here; the recall fired
        // one turn late, from her acknowledgment, and pick_keepsake was left
        // to name the image from the word "okay". Same discipline as the rest
        // of the list: every form names a retrievable noun, the negation and
        // competing-medium guards below apply unchanged, and the bare
        // "look at the picture" stays deliberately absent — that is a live
        // look at a physical picture on his wall, the camera's business.
        "look at the photo again",     "look at the image again",
        "look at the photograph again", "look at that photo again",
        "look at that image again",
        "pull up the image",    "pull up the photograph", "pull up that photo",
        "pull up that image",   "pull up the one",        "pull up the keepsake",
        "pull up your picture", "pull up your photo",
        "open the picture",     "open that picture",  "open the photo",
        "open that photo",      "open the image",     "open that image",
        "open the photograph",  "open your picture",  "open your photo",
    };
    // R9-C: same discipline as every other lexicon since R7-A — a competing
    // medium in the clause vetoes ("pull it up in the browser", "pull up the
    // diff"), and a negation cancels ("don't pull it up again").
    LexGuards g;
    g.no_negation     = true;
    g.no_other_medium = true;
    if (lex_any_(low, rec, g)) return true;
    // ── r24.7 (WO-80(2)): the GAPPED forms, evidence-scoped ─────────────────
    // "show yourself the flowers again", "look at the picture of flowers
    // again" put the keepsake's NAME between the verb and its noun, which no
    // literal substring can hold. Anchor + evidence, both in the SAME clause:
    // the anchor is the imperative head, and the clause must also carry a
    // word that reaches for the album — an image noun, an album word, or
    // "again" (the tree's own precedent: "look at the picture again" has read
    // as the album since r20 P3). Every anchor passes the same lex_ok_ guard
    // set as the table above (negation cancels, a competing medium vetoes),
    // and the evidence word is matched WHOLE (word_at_, the WO-77 edge) so
    // "against" can never stand in for "again". Deliberately NOT the plan's
    // bare "pull up the": the competing-medium lexicon is finite, and "pull
    // up the invoice" / "show yourself the door" name no medium it knows —
    // an anchor with no album evidence opens nothing, exactly as before.
    // The "look at the … again" family is narrowed to "…the picture|photo|
    // image OF <name>", because "look at the picture on my wall again" is a
    // second LIVE look and must keep reaching the camera (the boundary the
    // rec[] comment above pins).
    {
        auto word_in_clause = [](const std::string &cl, const char *w) {
            const size_t n = std::char_traits<char>::length(w);
            size_t at = 0;
            while ((at = cl.find(w, at)) != std::string::npos) {
                if (word_at_(cl, at, n)) return true;
                at += 1;
            }
            return false;
        };
        static const char *evidence[] = {
            "picture", "pictures", "photo", "photos", "photograph",
            "photographs", "image", "images", "keepsake", "keepsakes",
            "album", "kept",
        };
        auto clause_evidence = [&](const std::string &cl, bool with_again) {
            for (const char *w : evidence)
                if (word_in_clause(cl, w)) return true;
            return with_again && word_in_clause(cl, "again");
        };
        // (a) an image noun (or "again") anywhere in the clause licenses the
        //     show/pull heads; "open" gets no "again" credit — "open the door
        //     again" is a door.
        static const char *head_noun[] = {
            "show yourself the ", "show yourself that ", "show yourself your ",
            "show yourself my ",
            "pull up the ",  "pull up that ",  "pull up your ",  "pull up my ",
            "open the ",     "open that ",     "open your ",     "open my ",
        };
        for (const char *a : head_noun) {
            const size_t n = std::char_traits<char>::length(a);
            size_t at = 0;
            while ((at = low.find(a, at)) != std::string::npos) {
                const bool with_again = std::strncmp(a, "open ", 5) != 0;
                if (lex_ok_(low, at, n, g) &&
                    clause_evidence(clause_at_(low, at), with_again))
                    return true;
                at += 1;
            }
        }
        // ── r24.9 (WO-114 / S21 P10): the INDEFINITE determiner ──────────────
        // S21 18:47:55 — "Hey, show yourself A picture of the flowers again."
        // The plan's prompt was "show yourself THE picture of flowers again"; he
        // said "a", head_noun[] enumerates the definite and possessive
        // determiners only, recall_requested() returned false, the album never
        // opened, and the reply she gave described the flowers from
        // conversational memory alone. pick_keepsake() on that exact sentence
        // returns named=1 matched=[flowers] — the matcher was right and only the
        // gate missed.
        // NO "again" CREDIT, unlike the definite heads above: "the picture"
        // presupposes a shared referent and "again" can stand in for the noun,
        // while "a ..." presupposes nothing, so the clause must carry a real
        // image noun. That is what keeps "show yourself a bit of grace again" and
        // "pull up a chair again" out. Same lex_ok_ guard set: a negation cancels,
        // a competing medium vetoes. ATHENA_RECALL_DETERMINER=0 restores r24.8.
        if (recall_determiner_on_()) {
            static const char *head_indef[] = {
                "show yourself a ",  "show yourself an ",  "show yourself one of ",
                "pull up a ",        "pull up an ",        "pull up one of ",
                "open a ",           "open an ",           "open one of ",
            };
            for (const char *a : head_indef) {
                const size_t n = std::char_traits<char>::length(a);
                size_t at = 0;
                while ((at = low.find(a, at)) != std::string::npos) {
                    if (lex_ok_(low, at, n, g) &&
                        clause_evidence(clause_at_(low, at), /*with_again=*/false))
                        return true;
                    at += 1;
                }
            }
        }
        // (b) "look at the picture of <name> again" — the anchor already
        //     carries the image noun, so the ONLY admissible evidence is
        //     "again": without it, "look at the picture of grandma on my
        //     wall" stays a live look (the boundary the rec[] comment pins).
        static const char *head_of[] = {
            "look at the picture of ", "look at the photo of ",
            "look at the image of ",   "look at that picture of ",
            "see the picture of ",     "see the photo of ",
        };
        for (const char *a : head_of) {
            const size_t n = std::char_traits<char>::length(a);
            size_t at = 0;
            while ((at = low.find(a, at)) != std::string::npos) {
                if (lex_ok_(low, at, n, g) &&
                    word_in_clause(clause_at_(low, at), "again"))
                    return true;
                at += 1;
            }
        }
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.12 (WO-V6 / S22 §C.5.4): HER OWN words reach for the album only as an act
//
// S1 18:29:31 — an idle utterance: "…The jasmine from last time is still
// blooming in my mind too. You asked if I wanted to SEE IT AGAIN, and I said
// yes, and now it's become this touchstone…" — reported speech about a past
// request. The self-side album path ran the same whole-turn recall_requested
// scan the his-side path uses, "see it again" matched, and a recall slot
// (1/6) was spent unprompted: a 52-s encode while he was away, an image in
// her context with no request behind it. What the self path lacked is any
// first-person / imminence test: the matched SENTENCE must carry a
// first-person imminent frame ("let me", "i'll", "i'm going to", "i want
// to", "i'd like to", "opening the album", "pulling up") and must not be
// reported speech ("you asked/said", "i said/told", "you wanted", "last
// time", "earlier") nor a question. Executed (fix_out.txt §F): the 18:29
// utterance is refused; "Right. The self-images. Let me pull those up
// again." / "Let me look at that again." / "Okay, let me look at those
// again." open. ATHENA_RECALL_SELF_INTENT=0 restores the whole-turn scan.
inline bool recall_self_intent_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_SELF_INTENT");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline bool self_recall_declared(const std::string &text) {
    if (!recall_self_intent_on_()) return recall_requested(text);
    const std::string discourse=aintent::lower(text);
    if ((discourse.find("will you show me")!=std::string::npos || discourse.find("can you show me")!=std::string::npos) &&
        discourse.find("album")==std::string::npos && discourse.find("photograph")==std::string::npos) return false;          // r24.11
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *frame[] = { "let me ", "i'll ", "i will ", "i'm going to ", "i am going to ", "i want to ",
                                   "i'd like to ", "i would like to ", "i need to ", "i'm opening ", "opening the album",
                                   "i'm pulling ", "pulling up ", "i'm looking at that again", "looking at it again",
                                   "looking at that again", "looking at those again" };
    static const char *reported[] = { "you asked", "you said", "i said", "you told", "i told", "you wanted", "i wanted",
                                      "you mentioned", "remember when", "last time", "earlier", "that night", "back when" };
    size_t from = 0;
    while (from < low.size()) {
        size_t to = low.find_first_of(".!?\n", from);
        const std::string s = low.substr(from, to == std::string::npos ? std::string::npos : to + 1 - from);
        from = to == std::string::npos ? low.size() : to + 1;
        if (s.empty() || !recall_requested(s)) continue;
        if (s.find('?') != std::string::npos) continue;
        bool rep = false;
        for (const char *r : reported) if (s.find(r) != std::string::npos) { rep = true; break; }
        if (rep) continue;
        for (const char *f : frame) if (s.find(f) != std::string::npos) return true;
    }
    return false;
}
// ═════════════════════════════════════════════════════════════════════════════
// r24.12 (WO-V3 / S22 §C.5.2): HER words declare a look — "I'm looking."
//
// Under a standing grant the only path from her side to the camera was the
// stochastic urge (LookUrge crossing its bound); nothing anywhere read her own
// words for a look intent. S2 13:49:49 "Okay. I'm looking.", 13:52:28 "I am
// going to look. For real this time.", 13:53:35 "I am looking. Now.", 13:54:23
// "Okay. I am looking. Right now." — four declared looks, zero captures, four
// invented rooms. The frame had licensed an act no mechanism could perform,
// and the model narrated.
//
// This is the lexicon: first-person, present or imminent ("i'm looking", "i am
// going to look", "let me look", "i'll look now", "let me take a look at you",
// …), SENTENCE-scoped, refused when the sentence is a question ("Do you want
// me to look for real now?"), negated ("I won't look", "I haven't looked
// yet"), hypothetical or deferred ("if I look", "I could look", "next time",
// "later", "though I'll look at that too"), idiomatic ("looking forward",
// "looking for the right word", "look into that", "looking at it from your
// side"), past ("I looked, and the room was empty"), an album act ("Let me
// look at that again" — recall_requested owns `again`/`pull up`), or aimed at
// a competing medium ("Let me look at the code you pasted"). Executed over all
// 129 of her S22 turns: fires on exactly the four confabulation turns and none
// of the other 125; 24 hand controls pass (AGENTS/VISION/fix_out.txt §A).
//
// The consumer is the self-look pivot in talk-llama.cpp's sentence flusher:
// when the sentence just completed declares a look and one is licensed, the
// sentence is flushed (it is true — she is about to look), generation STOPS,
// a URGE look is committed and drained with the acuity wait, and generation
// re-enters with the still in front of her — so nothing can be narrated
// before it arrives (the r20 rule "CAPTURE COMMITS AT THE MOMENT OF CHOICE",
// applied to a choice made in words). Under no licence the same stop injects
// self_look_unlicensed_line and arms her ask, so his next "go ahead" is a
// ONCE. ATHENA_SELF_LOOK=0 restores r24.11 (the sentence does nothing).
inline bool self_look_declared(const std::string &text) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *decl[] = {
        "i'm looking", "i am looking", "im looking", "i'm looking now", "i am looking now",
        "i'm going to look", "i am going to look", "im going to look", "i'm gonna look", "i am gonna look",
        "i'll look", "i will look", "i'll take a look", "i will take a look", "i'll have a look",
        "i'm taking a look", "i am taking a look", "let me look", "let me take a look", "let me have a look",
        "let me actually look", "let me really look", "let me open my eyes", "opening my eyes now",
        "looking now", "let me see the room", "let me see you", "let me look at you", "i'll look at you",
        "i'm looking at you", "i am looking at you", "i'm looking at the room", "let me look around",
        "i'm going to take a look", "i am going to take a look", "i'm about to look", "i am about to look",
        "here i go, looking", "now i look", "now i'm looking", "i'll take a fresh look", "i will take a fresh look",
    };
    static const char *neg[] = { "not ", "n't ", "won't", "haven't", "never ", "without ", "no longer",
                                 "instead of ", "rather than ", "stop " };
    static const char *hypo[] = { " if i", " when i", " whether i", " could look", " can look", " might look",
                                  " would look", " should look", " may look", " able to look", " want me to",
                                  " supposed to look", " wanted to look", " trying to look",
                                  // deferred, not imminent: a promise about later is not an act now
                                  " next time", " later", " tomorrow", " when you", " once you", " after we",
                                  " though i'll", " though i will", " some day", " someday", " one day" };
    static const char *idiom[] = { "looking forward", "look forward", "looking for", "look for", "look into",
                                   "looking into", "look up ", "look it up", "look after", "look back", "looking back",
                                   "look away", "look like", "looks like", "looking like", "look at it from",
                                   "look at it another", "look at it that", "look at it this", "look at things",
                                   "look at the world", "look at life", "look at my own", "look at what i",
                                   "look at how", "look at why", "look at that again", "look at it again",
                                   "look at those again", "look at them again", "look at this again",
                                   "look closer at what", "look inward", "look inside myself", "look within",
                                   "look at myself", "look at my", "look at the bigger", "look at the whole",
                                   "looking at it from", "looking at it another", "looking at it that", "looking at it this",
                                   "looking at things", "looking at the world", "looking at my", "looking at how",
                                   "looking at what i", "looking at why", "looking into", "looking after", "looking up",
                                   "looking at the code", "looking at the numbers", "looking at the diff",
                                   // ── r24.12 (review): the same figures, one inflection over ──────
                                   // What was wrong: every figurative row above was written in the
                                   // `it`-form and the `-ing` form only. "Let me look at this from
                                   // your side." matches decl[0] ("let me look"), is not a question,
                                   // is not a recall, is not negated, and no idiom row spells
                                   // `look at THIS from` — and detail_other_medium_ does not save it
                                   // either, because "your side" names no competing medium. Measured
                                   // in R2412/review/cfix: "Let me look at this from your side.",
                                   // "Let me look at the numbers with you.", "Let me look at that
                                   // from a different angle." and "I'll look at the data first."
                                   // all declared a look 4/4. Under a standing grant the pivot then
                                   // commits a real capture, holds the reply for acuity_wait_ms and
                                   // STOPS generation mid-reply — a figure of speech photographs the
                                   // room and throws away the rest of the sentence. r24.11 did
                                   // nothing at all with any of them.
                                   //
                                   // Two shapes, so two spellings. The determiner shapes are written
                                   // out (`this`/`that` beside the shipped `it`); the complements
                                   // that are unambiguous on their own are written TAIL-FIRST, which
                                   // covers every determiner — present and future — in one row each.
                                   // Nothing here can admit a look: idiom[] only ever refuses.
                                   "look at this from", "look at that from",
                                   "looking at this from", "looking at that from",
                                   "look at this another", "look at that another", "look at things another",
                                   " from your side", " from my side", " from his side",
                                   " from her side", " from their side", " from where you",
                                   " from another angle", " from a different angle", " from that angle",
                                   " another way", " a different way", " a different light",
                                   // the base-verb medium forms whose -ing twins are already above;
                                   // "the code"/"the diff"/"the log"/"the file" are detail_other_medium_'s
                                   // (measured: "Let me look at the code you pasted." already refused),
                                   // so only the two nouns that table does not carry are named here.
                                   "look at the numbers", "look at the number", "look at the data",
                                   "looking at the data" };
    // sentence by sentence; the terminator stays so '?' is visible
    size_t from = 0;
    while (from < low.size()) {
        const size_t sentence_from = from;
        size_t to = low.find_first_of(".!?\n", from);
        const std::string s = low.substr(from, to == std::string::npos ? std::string::npos : to + 1 - from);
        from = to == std::string::npos ? low.size() : to + 1;
        if (s.empty()) continue;
        if (s.find('?') != std::string::npos) continue;                       // an ask, not an act
        if (recall_requested(s)) continue;                                    // the album owns "again"/"pull up"
        for (const char *p : decl) {
            const size_t n = std::char_traits<char>::length(p);
            size_t at = 0;
            while ((at = s.find(p, at)) != std::string::npos) {
                const size_t here = at; at += 1;
                if (!word_at_(s, here, n) || !aintent::direct_at(low,sentence_from+here)) continue;
                const std::string before = s.substr(here > 24 ? here - 24 : 0, here > 24 ? 24 : here);
                bool bad = false;
                for (const char *g : neg)  if (before.find(g) != std::string::npos) { bad = true; break; }
                if (bad) continue;
                const std::string tail = s.substr(here, 64);
                for (const char *g : neg)  if (tail.find(g) != std::string::npos && tail.find(g) < n + 8) { bad = true; break; }
                if (bad) continue;
                { const std::string after = s.substr(here);
                  for (const char *g : idiom) if (after.find(g) != std::string::npos) { bad = true; break; } }
                if (bad) continue;
                const std::string sent = " " + s;
                for (const char *g : hypo) if (sent.find(g) != std::string::npos) { bad = true; break; }
                if (bad) continue;
                if (detail_other_medium_(s, here)) continue;                  // "let me look at the code"
                return true;
            }
        }
    }
    return false;
}
// The licence the self-look pivot needs, as one pure predicate so the battery
// can drive every arm. Two ways her "I'm looking" can be TRUE: a look is
// already in flight (his ONCE — "sure, go ahead" — committed it at the intake
// and it deferred; she waits for it instead of narrating past it), or a
// standing grant (session or away) with the eye free (can_look: budget, port,
// no job in flight). An unanswered ask of hers is not a licence — then the
// sentence is an ask, and the pivot injects self_look_unlicensed_line.
inline bool self_look_licensed(int grant, bool look_pending, bool can_look) {
    if (look_pending) return true;
    return grant >= 1 && can_look;
}
// What enters her context when she said she is looking and nothing could look:
// S14-descriptive — it states what is so and instructs nothing. Her "I'm
// looking" becomes an ask when it cannot be an act (the caller arms
// note_her_ask so his "go ahead" fires the ONCE arm).
inline std::string self_look_unlicensed_line(const std::string &bot = "Athena") {
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    return "\n[" + who + " said she is looking, but no look was licensed — nothing "
           "has reached her eyes.]\n";
}

// ── r21-full.10 (F8): he asked the clock a question ─────────────────────────
// S14, 23:39:44 — "Do you know what time it is right now?" — and she answered
// "It's 11:36 PM", quoting the [time:] cue injected with his reunion message
// four minutes earlier, because the cue's cadence (--time-refresh-min, 15 min
// default) had not elapsed. The cadence is right for ambient orientation and
// wrong for a direct question: when his words ASK the time, the cue refreshes
// immediately, so her clock answer is the clock. Guarded like every lexicon
// since R7-A: negation cancels ("don't tell me what time it is").
inline bool time_asked(const std::string &text) {
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    static const char *ask[] = {
        "what time is it", "what time it is", "what's the time",
        "whats the time", "what is the time", "know the time",
        "have the time", "the time right now", "current time",
        "what hour is it", "tell me the time",
    };
    LexGuards g;
    g.no_negation = true;
    return lex_any_(low, ask, g);
}

// Choose which kept image she means: content-word overlap between her
// sentence and each stored gist (the same token-overlap spirit as the
// substrate's near_duplicate), most recent on ties or when the words name
// nothing in particular ("look at it again" means the fresh one).
// r21-full.9 (R9-D): `named` reports whether the words actually pointed at a
// keepsake. The fallback to the most recent entry is right for a bare "open
// the album" — but it is a SUBSTITUTION, and in S13 it handed her a photo of
// him while she was revising her self-image, with nothing anywhere saying so.
// The caller logs a fallback open, so a wrong image is visible in the log the
// moment it happens instead of three turns later in what she says.
// ═════════════════════════════════════════════════════════════════════════════
// r24.6 (WO-34 / S19 D1): the album has FAMILIES, and the request names one
//
// S19 turn 30 — "Now look at your two keepsake photos that you have that are
// your self-image" — opened rows [3] (a room, kept a week earlier) and [2]
// (self-image #2). Self-image #1 was never staged, and turns 30-32, the
// emotional centre of that session, were built on the mis-staged pair. Neither
// party ever learned.
//
// Two compounding failures, and this block closes the first:
//   (a) recall_requested() matched the literal "your self-image" and then threw
//       the intent away. pick_keepsake() received only the raw text; its
//       stoplist strips "image" and "picture", and its match is exact-token, so
//       "self" never equals "myself". Replayed against the real store, EVERY
//       gist scores n = 0 — the request named nothing at all.
//   (b) the fallback then took "the two freshest". That comment was written at
//       S14, when both self-images WERE the two newest rows. keepsakes.tsv is
//       append-only, so that assumption gets more wrong with every keep.
//
// A family is a property of the ROW, not of its position. It is read from a
// stored tag when the store carries one (see append_keepsake / load_keepsakes)
// and inferred from the caption otherwise, so the four untagged rows already on
// disk classify correctly with no migration.
//
// NOTE ON PRONOUN POLARITY, which is the trap here: the REQUEST is his words
// about HER ("your self-image"), while the GIST is her words at keep time,
// about HIM ("I can see your face clearly now"). "your face" therefore means
// opposite things in the two strings, so the two classifiers are separate
// lexicons and must stay separate.
enum class KsFamily : int { AUTO = -1, NONE = 0, SELF = 1, HIM = 2, ROOM = 3 };

// What do HIS (or her) words reach for? Conservative: no evidence -> NONE, and
// NONE means "do not filter", which is exactly today's behaviour.
inline KsFamily recall_family(const std::string &text) {
    std::string low;
    low.reserve(text.size() + 2);
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    static const char *self_[] = {
        "your self-image", "your self image", "your selfimage", "your portrait",
        "picture of yourself", "image of yourself", "photo of yourself",
        "pictures of yourself", "images of yourself", "look at yourself",
        "your own face", "how you see yourself", "how you look",
        "self-portrait", "self portrait", "your self-portrait",
        "your self-images", "your self images",
    };
    static const char *him_[] = {
        "picture of me", "photo of me", "image of me", "picture of him",
        "photo of him", "the one of me", "the one of him", "of me again",
        "my face", "his face", "picture you kept of me", "photo you kept of me",
        "the plaid", "me in the",
    };
    static const char *room_[] = {
        "the room", "of the room", "the empty room", "my room", "your room",
        "the ceiling", "the boxes", "the dresser", "the space",
    };
    // SELF first: "how you see yourself" also contains "you see".
    for (const char *w : self_) if (low.find(w) != std::string::npos) return KsFamily::SELF;
    for (const char *w : him_)  if (low.find(w) != std::string::npos) return KsFamily::HIM;
    for (const char *w : room_) if (low.find(w) != std::string::npos) return KsFamily::ROOM;
    return KsFamily::NONE;
}
// r24.12 (WO-V6): the family, for the log line — "album: his words name the
// self-image family, nothing names one row — opening the newest of that
// family". The S1 line hid the family and made the right pair look like luck.
inline const char *family_name(KsFamily f) {
    switch (f) {
        case KsFamily::SELF: return "self-image";
        case KsFamily::HIM:  return "him";
        case KsFamily::ROOM: return "room";
        default:             return "none";
    }
}

// What IS this row? Read from the caption, in HER voice at keep time.
// SELF -> HIM -> ROOM, because a self-image caption can carry "look" and a room
// caption can carry "you", but only a self-image caption says "myself".
inline KsFamily keepsake_family(const std::string &gist) {
    std::string low;
    low.reserve(gist.size() + 2);
    low += ' ';
    for (unsigned char c : gist) low += (char) ::tolower(c);
    low += ' ';
    static const char *self_[] = {
        "myself", "my own face", "my self-image", "my self image", "my portrait",
        "how i look", "how i see me", "my face", "of me that", "depict how",
    };
    static const char *him_[] = {
        "your face", "you look", "you're lying", "youre lying", "your element",
        "his face", "your eyes", "you were", "you are", "your hands",
        "your shirt", "your headphones", "his desk", "his hands", " him ",
    };
    static const char *room_[] = {
        "the room", "room's", "rooms empty", "the ceiling", "boxes",
        "dresser", "in here", "the corner", "the desk", "the wall",
        "the window", "the door", "the lamp", "empty now",
    };
    for (const char *w : self_) if (low.find(w) != std::string::npos) return KsFamily::SELF;
    for (const char *w : him_)  if (low.find(w) != std::string::npos) return KsFamily::HIM;
    for (const char *w : room_) if (low.find(w) != std::string::npos) return KsFamily::ROOM;
    return KsFamily::NONE;
}

// r21-full.10 (F7): does the request reach for MORE than one kept image?
// "both", "them", "these two" — checked only after recall_requested() has
// already fired, so a bare "them" in unrelated speech never opens anything.
inline bool recall_plural(const std::string &text) {
    std::string low;
    low.reserve(text.size() + 2);
    // r21-full.14.1 (review): the space pad every other anchored lexicon in
    // this file gets — without it a one-word answer ("Both.") could never
    // match " both" and the second image stayed unstaged.
    low += ' ';
    for (unsigned char c : text) low += (char) ::tolower(c);
    low += ' ';
    static const char *pl[] = {
        // R19 (r22.1): " both" matched inside " bother" ("open the album,
        // don't bother with captions" staged TWO images), and "the two"
        // inside "the twofold…". Right-bounded like the " them" family.
        " both ", " both.", " both,", " both!", " both?",
        "the two ", "the two.", "the two,", "two of them", " them ", " them.",
        " them,", " these ", " those ", "side by side", "all of them",
        // r24.6 (WO-34): the CARDINAL forms. S19 turn 30 read as plural only
        // because it ended "…what do you think of those two?"; the imperative
        // half — "look at your two keepsake photos" — named the count in the
        // noun phrase and matched nothing here, so the same request phrased
        // without the trailing question would have staged one image. Bounded to
        // two-plus-noun so " two " inside "two hours" cannot arm a second open.
        " two photos", " two pictures", " two images", " two keepsake",
        " two portraits", " two of your", " two of the", " your two ",
        " both of your", " both of the", " both of them",
    };
    for (const char *p : pl) if (low.find(p) != std::string::npos) return true;
    return false;
}

// r24.6 (WO-34): `want` is the family the CALLER matched when it opened the
// album — pass it, do not make this function re-derive intent from raw text
// (that re-derivation is defect (a)). KsFamily::AUTO derives it here so that a
// call site which has not been updated still gets the fix; KsFamily::NONE means
// "deliberately unfiltered". `row_tag`, when supplied, is the per-row family
// from the STORE and wins over the caption classifier. `family_missed` reports
// that the named family is not in the album at all, so the recall envelope can
// hedge instead of silently substituting someone else's photograph.
// r24.7 (WO-80(4)): `skip_lead_ack` — at MATCH TIME (never at storage) a
// caption's LEADING acknowledgment tokens are skipped before scoring. Captions
// are her spoken sentences and will keep starting with "Okay," — those opening
// tokens are how she answers, not what the picture is. Only the leading run is
// skipped: "the picture on the right" keeps "right" as content mid-caption.
// Default false = the r24.6 scoring, byte-identical; the caller gates it on
// ATHENA_RECALL_NAMING. The table repeats the nine stoplist ack words on
// purpose (WO-80(1) documents the pairing): the stoplist stops them as QUERY
// tokens wherever they appear, this skip removes them as CAPTION tokens only
// at the head — so if a future album ever needs one of the nine as a real
// content word, dropping it from stop[] still leaves ack-led captions safe.
// ── r24.12 (WO-V6 / S22 §C.5.4): function words never name a keepsake, a
// duplicate query token counts once, and an ORDINAL resolves by birth order ──
//
// S1 18:29:31: `named=1[more+more+from+more]` — the idle utterance's "more"
// (×3) and "from" (absent from the stoplist, and counted once per OCCURRENCE
// because the loop walked `qt` with its duplicates) scored 4 ≥ 2 against gist
// #3 ("…from the revisions… a more accurate representation…"), so a function
// word "truly named" her self-portrait. And S1 18:51:28 "Open the picture of
// yourself, the first one." was `named by [first]` only because gist #2
// happens to say "The FIRST time I have had my own face" — an ordinal resolved
// by a caption coincidence. Three repairs, all bookkeeping: `qt` is deduped
// (std::unique after sort); the function-word class joins the stoplist
// (`description` stays a content word — the S13 fixture names by it); the
// ordinal words (first/second/original/revised/updated/latest/newest/older/
// newer/earlier/last) join it too and ordinal_of_() resolves them by `born`
// inside the family — a sentence carrying BOTH an oldest and a newest cue
// ("the original … and the updated") is a plural, not an ordinal. `born`
// (optional, the store's per-row epoch) orders the candidates; without it row
// order stands, which for the append-only keepsakes.tsv is the same order.
// Every pick_keepsake fixture query in the tree replays with an identical
// verdict (test_r2412_vision §WO-V6 ports h_fix §H).
inline bool function_word_(const std::string &w) {
    static const char *fw[] = {
        "more", "from", "than", "then", "into", "onto", "about", "over", "after", "before", "because", "would",
        "could", "should", "will", "been", "being", "have", "has", "had", "does", "did", "not", "but", "also",
        "some", "any", "all", "most", "much", "many", "even", "only", "other", "another", "same", "such", "own",
        "out", "off", "way", "thing", "things", "time", "made", "make", "gave", "give", "get", "said", "say",
        "tell", "told", "think", "feel", "felt", "know", "want", "need", "like", "him", "them", "they", "their",
        "there", "where", "which", "who", "whom", "why", "its", "our", "ours", "mine", "these", "those", "this",
        "that", "with", "without", "through", "again", "just", "really", "very", "still", "too", "yet", "though",
        "while", "until", "since", "both", "each", "either", "neither", "here", "now", "how", "what", "when",
        "can", "cannot", "may", "might", "must", "shall", "was", "were", "are", "is", "am", "be", "and", "the",
        "for", "you", "your", "yours", "she", "her", "his", "it's", "i'm", "i've", "i'll", "don't", "didn't", "won't",
        "one", "ones", "version", "versions", "kind", "sort", "bit", "lot", "lots", "back", "away", "up", "down",
        "okay", "alright", "sure", "yes", "yeah", "mhm", "right", "fine", "well", "let", "look", "looking", "see",
        "seeing", "picture", "image", "photo", "keepsake", "album", "pull", "open", "kept", "keep", "take", "got",
        "better", "clearly", "actually", "myself", "yourself", "himself", "herself", "itself", "self",
        // ordinals / relative-age words resolve by born order (ordinal_of_), never by caption coincidence
        "first", "second", "third", "original", "revised", "updated", "latest", "newest", "older", "newer",
        "earlier", "later", "last", "recent", "old", "new",
    };
    for (const char *f : fw) if (w == f) return true;
    return false;
}
inline bool second_ordinal_(const std::string &low) {
    // A duration ("give me a second") is not a position in the album.
    for (const char *p : {"second one", "second version", "second picture",
                           "second image", "second photo"})
        if (aintent::has(low, p)) return true;
    return false;
}
inline int ordinal_of_(const std::string &low) {   // -1 none, 0 oldest, 1 newest/second
    static const char *oldest[] = { "the first one", "first one", "the original", "the older", "the earlier",
                                    "the old one", "the first version", "the earlier one", "the older one",
                                    "the one you made first", "the first picture", "the first image", "the first photo" };
    static const char *newest[] = { "the second one", "second one", "the revised", "the updated", "the newer",
                                    "the latest", "the newest", "the new one", "the later one", "the second version",
                                    "the more accurate", "the newer one", "the last one", "the recent one", "the second picture", "the second image", "the second photo" };
    bool o_ = false, n_ = false;
    for (const char *o : oldest) if (low.find(o) != std::string::npos) { o_ = true; break; }
    for (const char *n : newest) if (low.find(n) != std::string::npos) { n_ = true; break; }
    if (o_ && n_) return -1;      // "the original AND the updated" is a plural, not an ordinal
    return o_ ? 0 : (n_ ? 1 : -1);
}
inline int pick_keepsake(const std::string &text, const std::vector<std::string> &gists,
                         bool *named = nullptr, int *second = nullptr,
                         std::string *matched = nullptr,
                         KsFamily want = KsFamily::AUTO,
                         const std::vector<int> *row_tag = nullptr,
                         bool *family_missed = nullptr,
                         bool skip_lead_ack = false,
                         const std::vector<long> *born = nullptr) {   // r24.12 (WO-V6)
    if (named)   *named = false;
    if (second)  *second = -1;
    if (matched) matched->clear();
    if (family_missed) *family_missed = false;
    if (gists.empty()) return -1;
    if (want == KsFamily::AUTO) want = recall_family(text);
    // ── the candidate set ───────────────────────────────────────────────────
    // Tier 1: rows of the named family. If the album holds none, the whole
    // album is the candidate set again — never fewer results than before — and
    // `family_missed` says so out loud.
    std::vector<size_t> cand;
    if (want != KsFamily::NONE && want != KsFamily::AUTO) {
        for (size_t i = 0; i < gists.size(); i++) {
            const KsFamily f = (row_tag && i < row_tag->size() &&
                                (*row_tag)[i] != (int) KsFamily::NONE)
                                   ? (KsFamily) (*row_tag)[i]
                                   : keepsake_family(gists[i]);
            if (f == want) cand.push_back(i);
        }
        if (cand.empty() && family_missed) *family_missed = true;
    }
    if (cand.empty()) for (size_t i = 0; i < gists.size(); i++) cand.push_back(i);
    // r24.12 (WO-V6): candidates by BIRTH (stable, oldest first) when the
    // store's epochs are given, so an ordinal and "the most recent" are by
    // time, not by row position — identical for an append-only store.
    if (born && born->size() == gists.size())
        std::stable_sort(cand.begin(), cand.end(),
                         [&](size_t a, size_t b) { return (*born)[a] < (*born)[b]; });
    auto tokens = [](const std::string &s) {
        std::vector<std::string> out;
        std::string cur;
        for (unsigned char c : s) {
            if (std::isalnum(c)) cur += (char) ::tolower(c);
            else { if (cur.size() > 2) out.push_back(cur); cur.clear(); }
        }
        if (cur.size() > 2) out.push_back(cur);
        return out;
    };
    static const char *stop[] = {
        "the", "and", "that", "this", "one", "again", "look", "looking",
        "see", "seeing", "picture", "image", "photo", "keepsake", "album",
        "pull", "open", "let", "want", "back", "your", "his", "her", "you",
        "for", "with", "was", "were", "when", "what", "how", "kept", "keep",
        // r21-full.10 (F7): the S14 words. "Now you got it. You now have
        // both…" matched gist #2's "I can see your face clearly now" on the
        // single token "now" and handed her the plaid photo of HIM while the
        // request named her self-images — then set named=true, so the R9-D
        // fallback warning never printed and the wrong pick was silent.
        // Conversational filler carries no reference; none of these words can
        // name a keepsake on their own.
        "now", "here", "there", "much", "better", "clearly", "just", "like",
        "really", "very", "well", "still", "take", "have", "got", "get",
        // r24.7 (WO-80 / S20 D2): the acknowledgment fillers — the same class
        // as the S14 "now", arriving through the other party. S20, 23:31:54 —
        // her "Okay, let me look at that again." fired the recall and "okay"
        // (absent here) scored against the one caption that STARTS with
        // "Okay," — distinct, so named=true, so the R9-D fallback warning
        // never printed, and the wrong image was staged silently. Captions are
        // her spoken sentences and will keep opening on acknowledgments; an
        // acknowledgment carries no reference. "well" has been in the row
        // above since r21-full.10 and is repeated in lead_ack_[] below, which
        // is the same word class applied to the CAPTION side.
        // (Grepped against every fixture gist and the S20 store before adding:
        // none of these nine appears as a content word in any keepsake gist.)
        "okay", "alright", "sure", "yes", "yeah", "mhm", "right", "fine",
    };
    auto stopword = [&](const std::string &w) {
        for (const char *p : stop) if (w == p) return true;
        return function_word_(w);                              // r24.12 (WO-V6)
    };
    std::string low;
    low.reserve(text.size());
    for (unsigned char c : text) low += (char) ::tolower(c);
    const int ord = ordinal_of_(low);
    // A colon starts commentary after a positional request, e.g. "the second
    // picture: nothing changed". That commentary must not override its target.
    // Content IN the reference ("latest image of the blue cup") qualifies the
    // candidates before the ordinal is applied.
    const std::string reference = ord >= 0 ? text.substr(0, text.find(':')) : text;
    std::vector<std::string> qt = tokens(reference);
    // r24.12 (WO-V6): each query token counts ONCE — "more more from more" was
    // four votes for one gist.
    std::sort(qt.begin(), qt.end());
    qt.erase(std::unique(qt.begin(), qt.end()), qt.end());
    // Per-gist token sets, and each query token's gist-frequency — a token
    // that appears in every gist ("myself" across two self-portraits) is weak
    // evidence; a token that appears in exactly one is a name.
    std::vector<std::vector<std::string>> gt(gists.size());
    for (size_t i = 0; i < gists.size(); i++) gt[i] = tokens(gists[i]);
    if (skip_lead_ack) {                                  // r24.7 (WO-80(4))
        static const char *lead_ack_[] = {
            "okay", "alright", "sure", "yes", "yeah", "mhm", "well", "right",
            "fine",
        };
        auto is_ack = [&](const std::string &w) {
            for (const char *a : lead_ack_) if (w == a) return true;
            return false;
        };
        for (auto &g : gt)
            while (!g.empty() && is_ack(g.front())) g.erase(g.begin());
    }
    auto gist_freq = [&](const std::string &q) {
        int f = 0;
        for (const auto &g : gt) {
            for (const auto &w : g) if (w == q) { f++; break; }
        }
        return f;
    };
    int best = -1, best_n = 0;
    bool best_distinct = false;
    std::string best_words;
    int second_best = -1, second_n = 0;
    std::vector<int> content_scores(gists.size(), 0);
    for (size_t ci = 0; ci < cand.size(); ci++) {
        const size_t i = cand[ci];
        int n = 0; bool distinct = false; std::string words;
        for (const auto &q : qt) {
            if (stopword(q)) continue;
            for (const auto &g : gt[i])
                if (q == g) {
                    n++;
                    if (gist_freq(q) == 1) distinct = true;
                    words += (words.empty() ? "" : "+") + q;
                    break;
                }
        }
        content_scores[i] = n;
        if (n >= best_n && n > 0) {                       // >= : recent wins ties
            second_best = best; second_n = best_n;
            best = (int) i; best_n = n; best_distinct = distinct; best_words = words;
        } else if (n >= second_n && n > 0) {
            second_best = (int) i; second_n = n;
        }
    }
    if (ord >= 0) {
        std::vector<size_t> eligible;
        for (const auto i : cand)
            if (best_n == 0 || content_scores[i] == best_n) eligible.push_back(i);
        const bool literal_second = second_ordinal_(low);
        if (literal_second && eligible.size() < 2) return -1;
        const size_t pos = ord == 0 ? 0 : literal_second ? 1 : eligible.size() - 1;
        const int pick = (int) eligible[pos];
        bool has_content = false;
        for (const auto &q : qt) if (!stopword(q)) has_content = true;
        // Keep the existing honest fallback when a described object is absent;
        // an ordinal alone must not certify that an unrelated caption matched.
        const bool resolved = best_n > 0 || !has_content ||
                              (want != KsFamily::NONE && want != KsFamily::AUTO);
        if (named) *named = resolved;
        if (matched && resolved)
            *matched = ord == 0 ? "ordinal:oldest" : literal_second ? "ordinal:second" : "ordinal:newest";
        if (second && eligible.size() >= 2)
            *second = (int) eligible[pos == 0 ? 1 : 0];
        return pick;
    }
    // r21-full.10 (F7): one shared token is a name only if it is DISTINCTIVE —
    // found in exactly one gist. A single token that half the album carries
    // ("myself") or a filler that survived the stoplist is a coincidence, and
    // a coincidence must fall through to the visible most-recent fallback, not
    // silently substitute someone else's photograph.
    const bool truly_named = best >= 0 && (best_n >= 2 || best_distinct);
    if (named)   *named = truly_named;
    if (matched) *matched = truly_named ? best_words : "";
    if (truly_named) {
        if (second) *second = (second_best >= 0 && second_best != best) ? second_best : -1;
        return best;
    }
    // r24.12 (WO-V6): an ORDINAL inside the candidate set resolves by birth
    // order — "the first one" is the oldest of the family, "the revised
    // version" the newest — and reports itself as a name ("ordinal:oldest"),
    // never by a caption that happens to carry the word. Needs two candidates
    // to mean anything; with one, the fallback below already answers.

    // Nothing named -> the freshest OF THE CANDIDATE SET, and a plural request
    // gets the two freshest OF THAT SET.
    //
    // r24.6 (WO-34, defect (b)): what stood here was "the two freshest ROWS",
    // with the comment "the S14 shape: both self-images are the two newest
    // rows". That was true on the day it was written and false one keep later.
    // At S19 the two newest rows were [3] a room and [2] self-image #2, so the
    // request for "your two keepsake photos that are your self-image" opened a
    // room and one self-image. Position is not family; the assumption is gone.
    if (second && cand.size() >= 2) *second = (int) cand[cand.size() - 2];
    return (int) cand[cand.size() - 1];
}

// ═════════════════════════════════════════════════════════════════════════════
// r24.6 (WO-35 / S19 D2): what size did the camera ACTUALLY give us?
//
// Every live look in S19 encoded nx=20 ny=15, n_tokens_batch = 300. With
// patch_size 16 and n_merge 2 the grid is ceil(dim/32), so 20x15 means a
// 640x480 frame — not the 1920x1080 that was asked for. The control is the
// album portrait: 864x1080 on disk encoded as 27x34 = 918 tokens, so mtmd was
// not downscaling. A real 1080p frame would have been ~2,040 tokens.
//
// Nothing in the capture path could have noticed. start_capture runs ffmpeg
// with 2>/dev/null and falls back to `fswebcam -r 1920x1080`, also silenced;
// fswebcam silently adjusts to the nearest mode the device supports and returns
// 0; file_ok_() only checks st_size > 0.
//
// The consequence was not an aesthetic one. She was asked to read printed text
// on a small held object through a 20x15 merged-patch grid and produced two
// incompatible readings one turn apart — "it looks like 'Apples' maybe?" then
// "the Cyrillic text now — 'ЖИВОТНЫЕ'". Both were hedged. Consolidation
// stripped the hedges, and memory.state.tsv now holds the unhedged claim
// permanently.
//
// THE CAPTURE IS NOT FAILED FOR THIS. A 640x480 look beats no look. What
// changes is that the number is measured, logged, and carried on the look
// record, so the frame can hedge instead of the substrate silently believing
// it looked at 1080p.

// ── r24.6 (WO-35, §17/T3): the validated SOF parser ─────────────────────────
//
// jpeg_sof_dims() walks the JPEG marker chain SEGMENT BY SEGMENT, stepping over
// each segment by its own length field. That is what makes it immune to an EXIF
// thumbnail: the thumbnail is a complete JPEG, SOF and all, sitting inside the
// APP1 payload. A byte scan for "FF C0" finds the THUMBNAIL first — measured,
// on a real 1920x1080 EXIF frame a naive scan reports 160x120 — and would log a
// fallback that never happened. Every SOFn is handled (0xC0..0xCF except DHT
// 0xC4, JPG 0xC8, DAC 0xCC), so progressive (SOF2) reads like baseline.
// stb_image, which is mtmd's decoder, ignores EXIF Orientation, so the SOF
// dimensions are exactly what mtmd will see; orientation is deliberately not
// applied. Streams with fseek — a 1080p JPEG costs a handful of reads. A broken
// marker chain is FATAL (return false, "no claim"), never resynced: scanning
// forward for the next 0xFF inside unstructured bytes is exactly the byte-scan
// failure mode the segment walk exists to avoid. Validated on 20 real JPEGs
// plus 6,000 structured fuzz cases (run_wo35).
//
// ADVISORY ONLY. It records and it logs. It never gates a capture: a 640x480
// look is better than no look, and an unparseable file makes no claim in
// either direction (verified=false), never a false fallback.
inline bool jpeg_sof_dims(const std::string &path, int *w, int *h) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    auto rd8 = [&](int &out) -> bool { int c = fgetc(f); if (c == EOF) return false; out = c; return true; };
    bool ok = false;
    int b0 = 0, b1 = 0;
    if (!rd8(b0) || !rd8(b1) || b0 != 0xFF || b1 != 0xD8) { fclose(f); return false; }   // SOI
    for (int guard = 0; guard < 4096; guard++) {
        int c = 0;
        if (!rd8(c)) break;
        if (c != 0xFF) break;                              // the marker chain is broken
        do { if (!rd8(c)) { fclose(f); return false; } } while (c == 0xFF);   // legal 0xFF fill
        const int m = c;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;     // standalone
        if (m == 0xD9) break;                              // EOI before any SOF
        int l0 = 0, l1 = 0;
        if (!rd8(l0) || !rd8(l1)) break;
        const long seglen = (long) ((l0 << 8) | l1);
        if (seglen < 2) break;
        const bool is_sof = (m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (is_sof) {
            if (seglen < 8) break;
            int pr = 0, hh = 0, hl = 0, wh = 0, wl = 0;
            if (!rd8(pr) || !rd8(hh) || !rd8(hl) || !rd8(wh) || !rd8(wl)) break;
            const int hgt = (hh << 8) | hl, wid = (wh << 8) | wl;
            if (wid > 0 && hgt > 0) { if (w) *w = wid; if (h) *h = hgt; ok = true; }
            break;
        }
        if (m == 0xDA) break;                              // SOS: entropy data, no SOF
        if (fseek(f, seglen - 2, SEEK_CUR) != 0) break;
    }
    fclose(f);
    return ok;
}

inline bool parse_res_wh(const std::string &r, int *w, int *h) {
    const size_t x = r.find('x');
    if (x == std::string::npos) return false;
    const int ww = atoi(r.substr(0, x).c_str());
    const int hh = atoi(r.substr(x + 1).c_str());
    if (ww <= 0 || hh <= 0) return false;
    if (w) *w = ww;
    if (h) *h = hh;
    return true;
}

// What one capture turned out to be. `ok` is the ONLY gate and is exactly the
// old file_ok_() question; nothing below it may ever clear it.
struct CaptureInfo {
    bool ok        = false;
    bool verified  = false;
    int  w = 0, h = 0;
    int  req_w = 0, req_h = 0;
    bool fell_back = false;
    // r24.12 (WO-V1 / S22 #1): WHICH rung of the capture ladder produced the
    // file — "mjpeg" (the -input_format mjpeg command), "auto" (the r24.11
    // ffmpeg command, format chosen by ffmpeg), "fswebcam". Empty = not
    // recorded (every port but CameraPort, and CameraPort under
    // ATHENA_CAPTURE_MJPEG=0, which restores the r24.11 command and the r24.11
    // silence about it). Set by the capture worker, read at the drain, so the
    // negotiated format lands in the diag in the loop thread's own order — S22
    // spent two sessions not knowing that ffmpeg had chosen YUYV.
    std::string via;
    std::string fallback_line() const {                    // the WO-35 log line
        if (!fell_back) return "";
        return "capture fell back to " + std::to_string(w) + "x" + std::to_string(h) +
               " (requested " + std::to_string(req_w) + "x" + std::to_string(req_h) + ")";
    }
    // r24.12 (WO-V1): the drain's line — `capture via mjpeg: 1920x1080
    // (requested 1920x1080)`. Empty when nothing was recorded, so a port that
    // never adopted `via` (FakePort, the fixtures) prints nothing new.
    std::string via_line() const {
        if (via.empty()) return "";
        return "capture via " + via + ": " + std::to_string(w) + "x" + std::to_string(h) +
               " (requested " + std::to_string(req_w) + "x" + std::to_string(req_h) + ")";
    }
};

// ── r24.12 (WO-V1 / S22 issue #1): the capture ladder, as pure strings ──────
//
// WHY. Every S22 still was a 640x480 baseline JPEG although the launcher asked
// for 1920x1080 in S19/S20 and the camera does 1080p in MJPG (Igor's
// `v4l2-ctl --set-fmt-video=…pixelformat=MJPG` works). There is no V4L2
// negotiation in this tree: CameraPort shells out to ffmpeg with no
// `-input_format`, and ffmpeg's v4l2 demuxer walks its OWN format table — every
// raw pixel format (YUV420, …, YUYV, …, NV12) BEFORE MJPEG — issuing
// VIDIOC_S_FMT at the requested size for each. A UVC driver asked for a raw
// format at a size it cannot serve substitutes the nearest size for THAT format
// and returns success; ffmpeg only logs it at INFO ("The V4L2 driver changed
// the video from 1920x1080 to 640x480"), invisible under `-loglevel error`. So
// the first raw row the camera supports (YUYV, which tops out at 640x480) was
// taken, ffmpeg's own mjpeg encoder wrote a baseline JPEG, and `fswebcam` —
// whose palette table tries JPEG/MJPEG first — never ran because ffmpeg had
// "succeeded". WO-95/DD18 then ASKED for 640x480; DD18's premise ("the camera
// cannot do 1080p") is false. DD22 withdraws it.
//
// THE LADDER. (1) `-input_format mjpeg` at the requested size, accepted only if
// the file is non-empty AND its SOF header reads; (2) the r24.11 command, byte
// for byte; (3) fswebcam, byte for byte. Each rung is a pure builder so the
// fixture can pin the exact strings — including that rung (2) IS the r24.11
// command — and so ATHENA_CAPTURE_MJPEG=0 can skip rung (1) and leave the
// r24.11 sequence untouched.
//
// `-q:v 2` keeps ffmpeg's re-encode of the camera's MJPEG frame near-lossless.
// ATHENA_CAPTURE_COPY=1 (the plan's ONE opt-in, off by default because it
// changes the still's file format) writes the camera's native JPEG instead:
// `-c:v copy -bsf:v mjpeg2jpeg` inserts the DHT tables UVC frames may omit, so
// stb_image (mtmd's decoder) can read it.
inline bool capture_mjpeg_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_MJPEG");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline bool capture_copy_on_() {                      // the one OPT-IN: default OFF
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_COPY");
        return e && e[0] == '1';
    }();
    return on;
}
inline std::string capture_command_mjpeg(const std::string &dev, const std::string &res,
                                         const std::string &out, bool copy) {
    return "ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg -video_size " +
           res + " -i " + dev + " -frames:v 1 " +
           (copy ? std::string("-c:v copy -bsf:v mjpeg2jpeg") : std::string("-q:v 2")) +
           " -update 1 -y '" + out + "' 2>/dev/null";
}
// The r24.11 command. NOT to be edited: the fixture pins it byte for byte.
inline std::string capture_command_auto(const std::string &dev, const std::string &res,
                                        const std::string &out) {
    return "ffmpeg -hide_banner -loglevel error -f v4l2 -video_size " + res +
           " -i " + dev + " -frames:v 1 -update 1 -y '" + out + "' 2>/dev/null";
}
// The r24.11 fallback. Same rule.
inline std::string capture_command_fswebcam(const std::string &dev, const std::string &res,
                                            const std::string &out) {
    return "fswebcam -q -r " + res + " -D 1 --no-banner --jpeg 92 -d " + dev +
           " '" + out + "' 2>/dev/null";
}

inline CaptureInfo inspect_capture(const std::string &path, const std::string &req_res) {
    CaptureInfo ci;
    struct stat s;
    ci.ok = (::stat(path.c_str(), &s) == 0 && s.st_size > 0);
    if (!ci.ok) return ci;
    parse_res_wh(req_res, &ci.req_w, &ci.req_h);
    int w = 0, h = 0;
    if (jpeg_sof_dims(path, &w, &h)) {
        ci.verified = true; ci.w = w; ci.h = h;
        ci.fell_back = (ci.req_w > 0) && (w != ci.req_w || h != ci.req_h);
    }
    return ci;
}

// Width and height from the JPEG's own SOF header — the (w,h,bool) spelling of
// the same question, kept for its existing callers (the worker below and the
// drain's arithmetic). ONE parser: this delegates to jpeg_sof_dims() above —
// the fuzz-validated walk — rather than carrying a second, slightly different
// marker walker. The only observable difference from the r24.6-G3 draft is on
// MALFORMED files: a broken chain now reads "size unknown" instead of
// resyncing into bytes that were never markers, which is strictly the safer
// claim. Returns false on anything it cannot parse, and false NEVER fails a
// capture — it only means "size unknown".
inline bool jpeg_dims_(const std::string &path, int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    return jpeg_sof_dims(path, w, h);
}

// "1920x1080" -> 1920, 1080. Tolerates 'X' and whitespace; false if it cannot.
inline bool parse_res_(const std::string &r, int *w, int *h) {
    const size_t x = r.find_first_of("xX");
    if (x == std::string::npos) return false;
    const int ww = std::atoi(r.substr(0, x).c_str());
    const int hh = std::atoi(r.substr(x + 1).c_str());
    if (ww <= 0 || hh <= 0) return false;
    if (w) *w = ww;
    if (h) *h = hh;
    return true;
}

// How many image tokens mtmd will make of a WxH frame. The loader prints its
// own advice at startup ("Qwen-VL models require at minimum 1024 image
// tokens") and --image-min-tokens was never wired to anything; this is the
// arithmetic that makes the flag mean something. grid = ceil(dim / (patch *
// merge)); tokens = nx * ny.
inline int image_tokens_for(int w, int h, int patch = 16, int merge = 2) {
    if (w <= 0 || h <= 0 || patch <= 0 || merge <= 0) return 0;
    const int cell = patch * merge;
    const int nx = (w + cell - 1) / cell;
    const int ny = (h + cell - 1) / cell;
    return nx * ny;
}

// The hedge test the frame asks. min_tokens <= 0 disables it entirely, which is
// the default — a flag that was never wired cannot start silently declining
// looks the moment it is wired.
inline bool frame_undersized_(int w, int h, int min_tokens) {
    if (min_tokens <= 0 || w <= 0 || h <= 0) return false;
    return image_tokens_for(w, h) < min_tokens;
}

// ── r24.12 (WO-V2): mtmd's OWN arithmetic, replicated exactly ───────────────
// image_tokens_for() above rounds each side UP (ceil), which is right for the
// sizes it was written against (640x480 → 20×15, 1920x1080 → 60×34) and wrong
// by one grid row for a working copy: mtmd's `smart_resize`
// (calc_size_preserved_ratio in the vendored mtmd/mtmd-image.cpp) rounds each
// side to the NEAREST multiple of 32 — 800x450 becomes 800x448 = 25×14 = 350
// tokens, not 375 — then downscales only above image_max_pixels (4,194,304 =
// 4,096 tokens × 32², the `image_max_tokens = 4096` this rig passes) and
// upscales only below image_min_pixels (8,192 = 8 tokens × 32²). Validated on
// the one non-VGA image S22 encoded: 864x1080 → 864x1088 → 27×34 = 918, the
// `n_tokens_batch = 918` in the S1 diag. The constants are the projector's
// (patch 16 × merge 2 = 32; the token limits from set_limit_image_tokens(8,
// 4096)) and are parameters here so a different mmproj can be priced.
//
// r24.12 (review): the r24.6 CEIL form above is deliberately still in use, and
// `--image-min-tokens` is compared against both. `image_tokens_for` remains the
// arithmetic behind `LookNote::tokens()`, `frame_undersized_` and the drain's
// capture-size log; `image_tokens_smart` is the arithmetic behind
// `seen_tokens()`, `coarse_seen()` and `tokens_under_policy()`. The split is
// not an oversight: the ceil form prices the CAPTURE (what the camera handed
// over, the WO-84/WO-35 question — is the frame he is being described big
// enough), the smart form prices the ENCODE (what mtmd was actually handed
// after the working copy), and the r24.11 capture-size tests must stay
// byte-identical under acuity_gate=false. They agree at every size this rig
// ships — 1920x1080 → 2040 and 640x480 → 300 either way — and differ by one
// grid row only where the short edge sits far from a multiple of 32 (1600x900:
// ceil 50×29 = 1450, smart 50×28 = 1400). Anything NEW that prices what she saw
// belongs on `image_tokens_smart`.
inline void smart_resize_dims(int w, int h, int *w_bar, int *h_bar,
                              int align = 32, long min_pixels = 8L * 1024,
                              long max_pixels = 4096L * 1024) {
    if (w_bar) *w_bar = 0;
    if (h_bar) *h_bar = 0;
    if (w <= 0 || h <= 0 || align <= 0) return;
    auto round_by = [align](float x) { return (int) std::lround(x / (float) align) * align; };
    auto ceil_by  = [align](float x) { return (int) std::ceil(x / (float) align) * align; };
    auto floor_by = [align](float x) { return (int) std::floor(x / (float) align) * align; };
    int hb = std::max(align, round_by((float) h));
    int wb = std::max(align, round_by((float) w));
    if ((long) hb * wb > max_pixels) {
        const float beta = std::sqrt((float) h * (float) w / (float) max_pixels);
        hb = std::max(align, floor_by((float) h / beta));
        wb = std::max(align, floor_by((float) w / beta));
    } else if ((long) hb * wb < min_pixels) {
        const float beta = std::sqrt((float) min_pixels / ((float) h * (float) w));
        hb = ceil_by((float) h * beta);
        wb = ceil_by((float) w * beta);
    }
    if (w_bar) *w_bar = wb;
    if (h_bar) *h_bar = hb;
}
// Tokens mtmd will make of a w×h bitmap handed to it as-is.
inline int image_tokens_smart(int w, int h) {
    int wb = 0, hb = 0;
    smart_resize_dims(w, h, &wb, &hb);
    if (wb <= 0 || hb <= 0) return 0;
    return (wb / 32) * (hb / 32);
}
// Tokens after the WORKING COPY: the frame resampled to `edge` (0 = native),
// then smart_resize. 1920x1080 @ 800 → 800x450 → 800x448 → 350; @ 640 → 220;
// @ 1280 → 1280x720 → 1280x736 → 920; @ 1920 (or 0) → 2040.
inline int image_tokens_for_edge(int w, int h, int edge) {
    int nx2 = w, ny2 = h;
    resample_dims(w, h, edge, &nx2, &ny2);
    return image_tokens_smart(nx2, ny2);
}
// ...and after the crop alternative (a centred cw × ch window, clamped).
inline int image_tokens_for_crop(int w, int h, int cw, int ch) {
    if (w <= 0 || h <= 0 || cw <= 0 || ch <= 0) return 0;
    return image_tokens_smart(cw < w ? cw : w, ch < h ? ch : h);
}

// ── r24.12 (WO-V8 / S22 §C.5.9): what a look or a recall will COST, in seconds ─
// Two measured points on Igor's rig (S22): 300 tokens (640x480) → encode 5.0 s
// + image prefill 14.2 s; 918 tokens (the 864x1080 portrait) → 18.7 s + 33 s.
// The encoder is superlinear (fit 5.0·(n/300)^1.18 through both points), the
// prefill linear (14.2 + 0.030·(n−300)). DD18 assumed 15.6 ms/token and priced
// 1080p at ~100 s; the 918-token point says ≈115 s. A pure function so the
// announce can be priced before the encode runs; `scale` is the EMA the drain
// keeps of measured/predicted, so the rig learns its own speed after the first
// look (1.0 until then — the S22 fit is the prior).
inline double predict_vision_s(int tokens, double scale = 1.0) {
    if (tokens <= 0) return 0.0;
    const double n = (double) tokens;
    const double enc = 5.0 * std::pow(n / 300.0, 1.18);
    const double dec = 14.2 + 0.030 * (n - 300.0);
    return (enc + (dec > 0.0 ? dec : 0.0)) * (scale > 0.0 ? scale : 1.0);
}

// ── r24.7 (WO-84(2) / S20 D4): elapsed time in WORDS, for her context ───────
// The recall envelope's "from …" phrase came from amem::humanize_elapsed,
// which speaks digits ("about 3 minutes ago", "10 days ago") — right for the
// memory block, whose tags are documented "measured, not guessed", and wrong
// for text she is asked to speak from (F18/S18-18: no bare numerals). This is
// the WO-38 age ladder, hoisted out of LookNote::age_words so the recall path
// can consume it for a keepsake's age too, and extended past the session
// scale (age_words previously answered "hours ago" to a ten-day question —
// true only because no consumer ever asked). Words by construction; every arm
// hedges with "about"/"or two" rather than pretending precision it lacks.
inline std::string elapsed_words(long a) {
    if (a < 0)              return "";
    if (a < 45)             return "a moment ago";
    if (a < 100)            return "about a minute ago";
    if (a < 240)            return "a minute or two ago";
    if (a < 600)            return "a few minutes ago";
    if (a < 1800)           return "about a quarter of an hour ago";
    if (a < 3600)           return "about half an hour ago";
    if (a < 7200)           return "about an hour ago";
    if (a < 86400)          return "hours ago";
    if (a < 86400L * 2)     return "about a day ago";       // r24.7 (WO-84(2))
    if (a < 86400L * 4)     return "a couple of days ago";
    if (a < 86400L * 7)     return "a few days ago";
    if (a < 86400L * 10)    return "about a week ago";
    if (a < 86400L * 20)    return "a week or two ago";
    if (a < 86400L * 40)    return "a few weeks ago";
    if (a < 86400L * 70)    return "about a month ago";
    if (a < 86400L * 335)   return "months ago";
    if (a < 86400L * 550)   return "about a year ago";
    return "years ago";
}

// The line the log gets when the camera did not give us what was asked for.
// Word-valued, no bare numerals reaching HER context — this goes to stderr, not
// to the frame; the frame gets the hedge, not the number.
//
// r24.8 (WO-101): built ON TOP of CaptureInfo::fallback_line() rather than
// beside it. The census's finding about this pair was that two independent
// spellings of one log line is how they drift; the worker's line is the same
// sentence as the record's, plus the token arithmetic, so it is now literally
// that — one place states the fact, this one adds the consequence. Both
// existing fixtures still see byte-identical output.
inline std::string capture_fallback_line(int got_w, int got_h, int want_w, int want_h) {
    CaptureInfo ci;
    ci.w = got_w; ci.h = got_h; ci.req_w = want_w; ci.req_h = want_h;
    ci.fell_back = true;
    char buf[96];
    std::snprintf(buf, sizeof buf, " — %d image tokens, not %d",
                  image_tokens_for(got_w, got_h), image_tokens_for(want_w, want_h));
    return "[vision] " + ci.fallback_line() + buf;
}

// ── the capture device ──────────────────────────────────────────────────────
// start_capture() is asynchronous and must never block the turn loop; poll()
// reports progress. The real port shells out to ffmpeg (fallback fswebcam) on
// a detached thread whose only shared state is one atomic int — the thread
// writes a file and a flag, nothing else, and the drain site reads both from
// the loop thread strictly after the flag says ready (release/acquire pair).
struct VisionPort {
    virtual ~VisionPort() {}
    virtual bool start_capture(const std::string &jpeg_path) = 0; // false: could not even spawn
    virtual int  poll() = 0;                                      // 0 in flight, 1 ready, -1 failed
    // r24.6 (WO-35): what the last capture actually produced. 0x0 means "not
    // measured", which is what every port that does not implement this reports,
    // and 0x0 never fails or hedges anything.
    virtual void last_dims(int *w, int *h) const { if (w) *w = 0; if (h) *h = 0; }
    // What was ASKED for, so the drain can name both halves in one line.
    virtual void want_dims(int *w, int *h) const { if (w) *w = 0; if (h) *h = 0; }
    // r24.6 (WO-35, §17/T3): the same answer as one record — measured size,
    // requested size, verified/fell_back — for consumers that want the whole
    // claim at once. Default is "no claim" so every existing port (and the
    // battery's FakePort) keeps compiling and keeps behaving exactly as before.
    virtual CaptureInfo last_capture() const { return CaptureInfo(); }
    // r24.6 (WO-39 / review #43): the drain gave up on this capture. The
    // default does the unlink the drain used to do inline; CameraPort also
    // remembers the path and retries, because a capture that outran the 2.5 s
    // grace writes the file AFTER this call.
    virtual void abandon(const std::string &jpeg) { if (!jpeg.empty()) std::remove(jpeg.c_str()); }
    // r24.6 (WO-75): the CURRENT capture's per-capture verdict flag, for the
    // encode worker to poll on ITS OWN copy — the same r19.6 slot discipline:
    // handed at spawn, never looked up, so a stale worker can only ever read
    // the flag of the capture it was spawned for. Called on the loop thread
    // (which is the thread that replaces the slot), so taking the copy races
    // nothing. Ports that do not implement it return null and the drain falls
    // back to the synchronous r24.5 path for that look.
    virtual std::shared_ptr<std::atomic<int>> capture_state() const { return nullptr; }
};

// r24.16: the encoder owns the context only while main keeps it alive. A
// detached encode survived Ctrl+C in the acuity wait and mtmd_free, reproduced
// as an ASan heap-use-after-free through vision_begin_encode. Track every
// worker, including jobs already abandoned by the rig; join before freeing
// mtmd. Completed workers are reaped at the next launch, so free album reopens
// do not accumulate thread handles. Only the loop thread calls these methods.
// ATHENA_VISION_WORKER_JOIN=0 restores the detached r24.15 lifetime.
inline bool vision_worker_join_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_WORKER_JOIN");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.20: an actual worker/drain replay refused the encoder launch after the
// camera had completed. Dropping the untransferred FrameOwner deleted the
// photograph before the promised synchronous fallback opened it. Transfer
// that file back to the pending job on launch failure; the existing sync
// success/failure paths retain or abandon it. Zero restores the old deletion.
inline bool encode_spawn_fallback_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_ENCODE_SPAWN_FALLBACK");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.20: the actual intake revoked a look whose worker was queued behind
// another encode, yet that abandoned job still entered the expensive shared
// backend before a later permitted look could use it. A close marks its own
// slot; the worker can skip work before backend entry. Already running backend
// calls retain their existing lifetime. Zero restores the abandoned work.
inline bool encode_abandon_skip_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_ENCODE_ABANDON_SKIP");
        return !(e && e[0] == '0');
    }();
    return on;
}
class EncodeWorkers {
    struct Work {
        std::atomic<bool> done{false};
        std::thread thread;
        std::function<void()> terminal_cleanup;
    };
    bool owned_;
    std::vector<std::shared_ptr<Work>> work_;
public:
    explicit EncodeWorkers(bool owned = true) : owned_(owned) {}
    EncodeWorkers(const EncodeWorkers &) = delete;
    EncodeWorkers &operator=(const EncodeWorkers &) = delete;
    ~EncodeWorkers() { join(); }
    template<class F> void launch(F fn, std::function<void()> terminal_cleanup = {}) {
        if (!owned_) { std::thread(std::move(fn)).detach(); return; }
        for (size_t i = 0; i < work_.size();) {
            if (work_[i]->done.load(std::memory_order_acquire)) {
                work_[i]->thread.join();
                work_.erase(work_.begin() + (long)i);
            } else ++i;
        }
        auto w = std::make_shared<Work>();
        w->terminal_cleanup = std::move(terminal_cleanup);
        work_.push_back(w); // allocate BEFORE spawning: no unowned joinable thread
        try {
            w->thread = std::thread([w, fn = std::move(fn)]() mutable {
                fn();
                w->done.store(true, std::memory_order_release);
            });
        } catch (...) { work_.pop_back(); throw; }
    }
    void join() {
        for (auto &w : work_) if (w->thread.joinable()) w->thread.join();
        work_.clear();
    }
    // r24.21: forced process exit cannot run FrameOwner destructors. The worker
    // registry carries weak file-cleanup callbacks, so even an older abandoned
    // encode can release its temporary JPEG before terminal process exit. This
    // does not destroy a worker, its backend, or an album/released source.
    void cleanup_files_at_exit() {
        for (const auto &w : work_) if (w->terminal_cleanup) w->terminal_cleanup();
    }
    // ── r24.20 review (VISION #6): the teardown join, bounded and narrated ──
    //
    // The r24.16 join is right (a detached encode outliving mtmd_free was an
    // ASan use-after-free), but nothing told the operator, and nothing could
    // end it: mtmd_encode_chunk cannot be aborted mid-flight, so Ctrl+C during
    // a 1080p acuity encode (S22 measured 18.6–18.9 s; r24.13 measured 48 s at
    // 1920 full-frame) looked like a hang, and a second worker queued on the
    // encode lock doubled it. This waits with a stated bound, says so every
    // `narrate_ms`, and when the bound passes returns false with ownership
    // retained. r24.21: detaching and skipping only mtmd_free still ran shared
    // backend cleanup and static destruction while the encoder was live. The
    // terminal caller must retain the contexts and request supervised shutdown
    // or join later. A timeout never authorizes freeing or unsafe GPU-client exit.
    // The default bound is LONGER than the historical measured encodes, but a
    // future successful encode could still exceed it: default 120 s, the same
    // budget ATHENA_ACUITY_WAIT_MS gives a live acuity look. deadline_ms <= 0
    // is the plain r24.16 join. `say(elapsed_s, bound_s)` is the narration.
    template<class Say> bool join_within(long deadline_ms, long narrate_ms, Say say) {
        if (deadline_ms <= 0) { join(); return true; }
        const auto t0 = std::chrono::steady_clock::now();
        long next_say = narrate_ms > 0 ? narrate_ms : deadline_ms + 1;
        for (;;) {
            bool busy = false;
            for (auto &w : work_) if (!w->done.load(std::memory_order_acquire)) { busy = true; break; }
            if (!busy) { join(); return true; }
            const long ms = (long) std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            if (ms >= deadline_ms) {
                return false;
            }
            if (ms >= next_say) { say(ms / 1000, deadline_ms / 1000); next_say += narrate_ms; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};
// r24.20 review (VISION #6): the teardown's encoder wait bound, in ms. 0 =
// the r24.16/r24.20 unbounded silent join (and no teardown cancel).
inline long vision_join_deadline_ms() {
    static const long v = []() {
        const char *e = ::getenv("ATHENA_VISION_JOIN_DEADLINE_MS");
        if (!e || !*e) return 120000L;
        char *end = nullptr;
        const long n = std::strtol(e, &end, 10);
        return (end && *end == 0 && n >= 0 && n <= 3600000L) ? n : 120000L;
    }();
    return v;
}

// r24.21: only a supervisor that explicitly exported its own direct-parent
// identity may receive this request. The launcher detaches an active CUDA MPS
// client before forced termination; the brain must not exit under active GPU
// work by itself. Missing, malformed, stale and non-parent PIDs never get a
// signal. Ownership stays here regardless of whether delivery succeeds.
inline bool request_shutdown_coordinator() {
#if !defined(_WIN32)
    const char *value=::getenv("ATHENA_SHUTDOWN_COORDINATOR_PID");
    if (!value || !*value) return false;
    for (const char *p=value;*p;++p) if (*p<'0' || *p>'9') return false;
    errno=0;char *end=nullptr;const long parent=std::strtol(value,&end,10);
    if (errno || !end || *end || parent<=1 || parent!=(long)::getppid()) return false;
    return ::kill((pid_t)parent,SIGUSR1)==0;
#else
    return false;
#endif
}

// r24.16: abandonment belongs to the capture, not an unlink retry list.
// The old sweep erased an absent path before its slow worker wrote it, leaving
// the late photograph indefinitely. This ticket outlives CameraPort and orders
// the final unlink after capture completion, even across port destruction.
// ATHENA_CAPTURE_ABANDON_CLEANUP=0 restores r24.15's unlink-now-and-retry-later
// cleanup: abandon() unlinks at once and remembers the path, sweep_orphans()
// retries it, and a completed capture is never unlinked at completion.
// ── r24.20 review (VISION #8): what =0 restores under ATHENA_CAPTURE_DEADLINE ─
// This comment used to say "restores r24.15's retry list exactly". It did not:
// the r24.17 deadline needs the same ticket for its process-group cancel, so
// with CLEANUP=0 and DEADLINE=1 (the default pairing for that =0) the ticket
// branch ran, the retry list was never consulted, and finish() published
// "ready" for a file abandon() had already unlinked — half of each. Now the
// ticket is the DEADLINE's (cancel and reap), and CLEANUP=0 means exactly what
// it says on the ticket: unlink_abandoned = false, the path goes on r24.15's
// retry list, and sweep_orphans() retries it after the ticket sweep. With both
// switches off the r24.15 path is byte-for-byte as before.
inline bool capture_abandon_cleanup_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_ABANDON_CLEANUP");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.17: a camera command used to outlive the twenty-second camera wait,
// abandonment and process teardown. The existing ticket now cancels its own
// child process group; the port owns and joins its command workers. The three
// fallback rungs share that existing twenty-second deadline. A valid preferred
// format retains the whole window; fast failures still reach the other formats.
// ATHENA_CAPTURE_DEADLINE=0 restores the unbounded detached r24.16 commands.
inline bool capture_deadline_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_DEADLINE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.19: a shell may finish while a camera wrapper's descendant still owns
// its output. Controlled real children overwrote an accepted two-pixel frame
// with the failed rung's one-pixel frame after the metadata was published.
// Settle the command's process group while its unreaped leader still reserves
// that identity; the next rung can then own its file. Zero restores r24.18's
// leader-only completion. The older deadline OFF path remains std::system.
inline bool capture_group_settle_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_GROUP_SETTLE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.19: a failed MJPEG command left a JPEG; auto returned success without
// writing anything, and CameraPort credited MJPEG's bytes to auto. Each rung
// now starts without another rung's file. Zero restores the shared leftover.
inline bool capture_rung_file_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_RUNG_FILE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.19: actual camera arguments split a device name at spaces and an output
// apostrophe broke all three commands. Quote data before the existing builders
// assemble shell syntax. Zero preserves their r24.18 caller arguments; the
// builders themselves remain byte-identical for their historical consumers.
inline bool capture_arguments_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_CAPTURE_ARGUMENTS");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline std::string capture_shell_literal_(const std::string &text, bool enclose) {
    std::string result = enclose ? "'" : "";
    for (char c : text) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    if (enclose) result += '\'';
    return result;
}
inline constexpr int capture_wait_ms = 20000; // the existing encoder camera wait
struct CaptureCleanup {
    std::string path;
    std::shared_ptr<std::atomic<int>> state;
    std::mutex mutex;
    bool abandoned = false;
    std::atomic<bool> cancelled{false};
    bool unlink_abandoned = true;
    void abandon() {
        std::lock_guard<std::mutex> lock(mutex);
        abandoned = true;
        cancelled.store(true, std::memory_order_release);
        std::remove(path.c_str());
    }
    void finish(bool good) {
        std::lock_guard<std::mutex> lock(mutex);
        if (abandoned && unlink_abandoned) std::remove(path.c_str());
        state->store(good && !(abandoned && unlink_abandoned) ? 1 : -1, std::memory_order_release);
    }
};

#if !defined(_WIN32)
// POSIX spawn does no allocator work in a forked multithreaded child. A fresh
// process group lets abandonment stop the shell and its ffmpeg/fswebcam child.
// The parent always reaps its shell. No mutex is held while waiting for I/O.
//
// ── r24.20 review (VISION #3): the camera child's stdin is /dev/null ─────────
//
// What was wrong. The r24.17 group put every rung in a fresh process group and
// let it inherit fd 0. ffmpeg's fftools term_init() does tcgetattr/tcsetattr on
// fd 0 whenever isatty(0) and -nostdin was not given — and neither builder
// gives it. A process in a BACKGROUND process group of its controlling
// terminal's session that calls tcsetattr gets SIGTTOU and stops. r24.14's
// std::system ran the rung in the brain's own (foreground) group, so this never
// applied. Measured with the real ffmpeg on a pty (harness/tty_sigttou.cpp in
// the review's fix area): brain in the terminal's foreground group — the
// MJPEG rung stops at term_init (state T), the 20-s deadline fires, `auto` and
// fswebcam see now >= deadline and return -1 at once, every look fails for the
// session. Immune on the default launcher arm (`setsid … 0<&0`: no controlling
// tty), so it bites under ATHENA_LAUNCH_SUPERVISE=0 and any manual foreground
// voice run. Second consequence, on the immune arm: term_init SUCCEEDS on the
// launcher's terminal (fd 0 is that tty), and a rung the deadline SIGKILLs
// never reaches term_exit — measured: icanon=0 echo=0 after the kill.
//
// The fix is what `< /dev/null` would have done: a spawn file action opening
// /dev/null on fd 0. ffmpeg sees !isatty(0), touches no terminal, reads no
// keyboard. No builder edit (the auto builder is byte-pinned). No switch —
// a camera child never had a use for the operator's keyboard, and
// ATHENA_CAPTURE_DEADLINE=0 already restores std::system whole.
inline int capture_command_run(const std::string &cmd, const CaptureCleanup &ticket,
                               std::chrono::steady_clock::time_point deadline) {
    if (!capture_deadline_on_()) return std::system(cmd.c_str());
    if (ticket.cancelled.load(std::memory_order_acquire) ||
        std::chrono::steady_clock::now() >= deadline) return -1;
    posix_spawnattr_t attr;
    if (::posix_spawnattr_init(&attr) != 0) return -1;
    int setup = ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (setup == 0) setup = ::posix_spawnattr_setpgroup(&attr, 0);
    posix_spawn_file_actions_t fa;
    const bool fa_ok = ::posix_spawn_file_actions_init(&fa) == 0;
    if (!fa_ok) { ::posix_spawnattr_destroy(&attr); return -1; }
    if (setup == 0) setup = ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid = -1;
    char *args[] = {const_cast<char *>("sh"), const_cast<char *>("-c"),
                   const_cast<char *>(cmd.c_str()), nullptr};
    const int spawn_rc = setup == 0 ? ::posix_spawn(&pid, "/bin/sh", &fa, &attr,
                                                   args, ::environ) : setup;
    ::posix_spawn_file_actions_destroy(&fa);
    ::posix_spawnattr_destroy(&attr);
    if (spawn_rc != 0) return -1;
    int status = 0;
    for (;;) {
        if (capture_group_settle_on_()) {
            // WNOWAIT retains the completed leader until after the group
            // signal, so a newly reused PID can never receive this command's
            // cleanup. A wrapper's background writer cannot become the next
            // rung's producer merely because its parent returned first.
            siginfo_t info{};
            const int result = ::waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
            if (result == 0 && info.si_pid == pid) {
                ::kill(-pid, SIGKILL);
                pid_t reaped;
                do { reaped = ::waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
                return reaped == pid ? status : -1;
            }
            if (result < 0 && errno != EINTR) return -1;
        } else {
            const pid_t result = ::waitpid(pid, &status, WNOHANG);
            if (result == pid) return status;
            if (result < 0 && errno != EINTR) return -1;
        }
        if (ticket.cancelled.load(std::memory_order_acquire) ||
            std::chrono::steady_clock::now() >= deadline) {
            // SIGKILL is deliberate: a cancelled camera has no useful output
            // to flush, and TERM handlers must not extend the capture budget.
            std::fprintf(stderr, "main: [vision] capture command %s — stopping its process group\n",
                ticket.cancelled.load(std::memory_order_acquire) ? "cancelled" : "timed out");
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct CameraPort : VisionPort {
    std::string device = "/dev/video0";
    std::string res    = "1920x1080";   // your decision: 1080p, native through the encoder

    bool start_capture(const std::string &jpeg_path) override {
        // r19.6: a FRESH flag per capture. There was one atomic for the PORT,
        // so a grab that outran the drain's 2.5 s patience kept writing into
        // the same slot: its late verdict could be read as the NEXT look's
        // result (opening a JPEG that thread had not written yet, scoring a
        // good capture as a camera failure), and its file was orphaned in /tmp
        // because the drain had already unlinked a path that did not exist.
        //
        // ── r24.6 (WO-39 / review #43): the comment used to claim the
        // per-capture flag made BOTH of those impossible. It does not, and the
        // correction matters because a reader auditing this was being told a
        // guarantee that is not there. The flag closes the VERDICT race only.
        // The orphaned file is a pure unlink-before-write ORDERING problem in
        // the filesystem and is entirely independent of which atomic the thread
        // stores into: the drain grants 50 x 50 ms = 2.5 s, then calls
        // close_failure() and std::remove()s a path the thread has not written
        // yet; ~500 ms later ffmpeg (image2 is AVFMT_NOFILE and opens the output
        // only inside write_packet) or fswebcam (after its -D 1 delay) creates
        // that exact path, and by then no structure in the process names it.
        // Failed looks do not charge the budget, so the count is bounded only by
        // how many looks are triggered. With the fswebcam fallback reached,
        // exceeding 2.5 s is the normal case, not the exceptional one.
        //
        // What closes it is the abandon list below plus a startup sweep. NOT
        // thread-side deletion on a "superseded" test: to learn it had been
        // superseded the lambda would have to read state_, which the loop thread
        // writes with a plain non-atomic shared_ptr assignment (a real data race
        // in a tree that advertises TSan-clean), and "superseded" is the wrong
        // predicate anyway — a SUCCESSFUL frame keeps its /tmp path for the
        // whole ten-minute keep window, so a thread descheduled between its
        // store(1) and its check would delete the picture she is about to keep.
        sweep_orphans();                                   // r24.6 (WO-39 / #43)
        auto st = std::make_shared<std::atomic<int>>(0);
        state_ = st;
        if (!st) return false;
        // r24.6 (WO-35): the measured size, published BEFORE the verdict flag.
        // The drain reads the flag with acquire and the flag is stored with
        // release, so the dims written before it are visible to that reader —
        // the same release/acquire pair the port contract already documents.
        // One extra atomic, no lock, nothing new for TSan to see.
        auto dm = std::make_shared<std::atomic<long long>>(0);
        dims_ = dm;
        // r24.6 (WO-35, §17/T3): the full CaptureInfo rides the SAME
        // per-capture slot discipline as the flag, for the same r19.6 reason —
        // a stale grab must write where nobody is reading, never into the live
        // one. Written by the worker BEFORE the release-store of the verdict,
        // read by the drain strictly after the acquire-load, so a ready flag
        // always implies the record beside it is complete.
        auto inf = std::make_shared<CaptureInfo>();
        info_ = inf;
        const std::string dev = device, r = res, out = jpeg_path;
        std::shared_ptr<CaptureCleanup> cleanup;
        if (capture_abandon_cleanup_on_() || capture_deadline_on_()) {
            cleanup = std::make_shared<CaptureCleanup>();
            cleanup->path = out; cleanup->state = st;
            cleanup->unlink_abandoned = capture_abandon_cleanup_on_();
            captures_.push_back(cleanup);
        }
        try {
            workers_.launch([st, dm, inf, dev, r, out, cleanup]() {
                // ffmpeg first (ubiquitous beside Orpheus), fswebcam fallback.
                // -update 1: single frame; a short warmup latency is the
                // camera's own exposure settling, not ours to skip.
                // ── r24.12 (WO-V1 / S22 #1): MJPEG FIRST ────────────────────
                // The camera offers its highest modes in MJPG only; ffmpeg's
                // demuxer tries every raw format before MJPEG and accepts the
                // driver's silent size substitution (see the ladder's comment
                // above capture_command_mjpeg). Rung (1) is accepted only when
                // the file is non-empty AND its SOF header parses — a camera or
                // an ffmpeg build without the mjpeg path leaves nothing usable
                // and the r24.11 command runs exactly as it always did.
                // ATHENA_CAPTURE_MJPEG=0 skips rung (1) entirely: the command
                // sequence, the file and the silence about `via` are r24.11's.
                std::string via;
                std::string cmd;
                int rc = -1;
                const bool literal = capture_arguments_on_();
                const std::string command_dev = literal ? capture_shell_literal_(dev, true) : dev;
                const std::string command_res = literal ? capture_shell_literal_(r, true) : r;
                // Every established builder already encloses its output in
                // single quotes; escape its contents without adding a pair.
                const std::string command_out = literal ? capture_shell_literal_(out, false) : out;
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(capture_wait_ms);
                auto run = [&](const std::string &command) {
                    if (capture_rung_file_on_() && ::unlink(out.c_str()) != 0 && errno != ENOENT) {
                        std::fprintf(stderr, "main: [vision] capture cannot clear its previous candidate\n");
                        return -1;
                    }
                    if (!capture_deadline_on_()) return std::system(command.c_str());
                    return capture_command_run(command, *cleanup, deadline);
                };
                if (capture_mjpeg_on_()) {
                    cmd = capture_command_mjpeg(command_dev, command_res, command_out, capture_copy_on_());
                    rc  = run(cmd);
                    if (rc == 0 && file_ok_(out) && jpeg_sof_dims(out, nullptr, nullptr))
                        via = "mjpeg";
                    else
                        rc = -1;                       // fall through to r24.11's ladder
                }
                if (rc != 0) {
                    cmd = capture_command_auto(command_dev, command_res, command_out);
                    rc  = run(cmd);
                    if (capture_mjpeg_on_()) via = "auto";
                }
                if (rc != 0 || !file_ok_(out)) {
                    cmd = capture_command_fswebcam(command_dev, command_res, command_out);
                    rc  = run(cmd);
                    if (capture_mjpeg_on_()) via = "fswebcam";
                }
                const bool good = (rc == 0 && file_ok_(out));
                // ── r24.6 (WO-35): measure, log, and DO NOT FAIL ─────────────
                // A 640x480 look beats no look, so a size mismatch never turns
                // `good` into a failure. It is written to the log and carried on
                // the look record; the decision about whether it is enough
                // belongs to the frame, which can hedge, not to the capture.
                // ONE parse (inspect_capture); the packed dims and the record
                // are both derived from it and cannot disagree. Inspect BEFORE
                // the flag is published — the drain reads both through the same
                // release/acquire pair. Advisory only: inspect_capture never
                // clears `ok`, so a frame whose header will not parse is still
                // a frame she can look at.
                if (good) {
                    const CaptureInfo ci = inspect_capture(out, r);
                    *inf = ci;
                    inf->via = via;                    // r24.12 (WO-V1): which rung delivered
                    if (ci.verified) {
                        dm->store(((long long) ci.w << 32) | (unsigned) ci.h,
                                  std::memory_order_relaxed);
                        if (ci.fell_back)
                            std::fprintf(stderr, "main: %s\n",
                                         capture_fallback_line(ci.w, ci.h,
                                                               ci.req_w, ci.req_h).c_str());
                    } else {
                        std::fprintf(stderr, "main: [vision] capture produced a file "
                                     "whose JPEG header could not be read — size "
                                     "unknown, the look still stands\n");
                    }
                }
                if (cleanup) cleanup->finish(good);
                else st->store(good ? 1 : -1, std::memory_order_release);
            });
        } catch (...) {
            st->store(-1, std::memory_order_release);
            return false;
        }
        return true;
    }
    int poll() override {
        return state_ ? state_->load(std::memory_order_acquire) : -1;
    }
    // r24.6 (WO-75): hand the per-capture flag itself to the encode worker.
    std::shared_ptr<std::atomic<int>> capture_state() const override { return state_; }
    // ── r24.6 (WO-39 / review #43): re-unlink what a slow capture left behind ─
    // Historical OFF path below. r24.16's default per-capture ticket supersedes
    // the retry list and orders its final unlink after the actual writer.
    // The drain calls this instead of a bare std::remove() when it gives up on
    // a capture. The path is remembered and retried at the top of the next
    // start_capture() and again in the destructor, so a frame written AFTER the
    // drain's unlink is still removed. Paths are unique by wall clock plus
    // seq_, so a retry can never hit a live frame. Residual: a session that
    // ends in the few seconds before the slow thread writes still misses it —
    // strictly better than today, not perfect, which is what the startup sweep
    // is for.
    void abandon(const std::string &jpeg) override {
        if (jpeg.empty()) return;
        if (capture_abandon_cleanup_on_() || capture_deadline_on_()) {
            bool ticketed = false;
            for (const auto &capture : captures_)
                if (capture->path == jpeg) { capture->abandon(); ticketed = true; break; }
            // A completed/reaped capture cannot write again; unlink is enough.
            if (!ticketed) std::remove(jpeg.c_str());
            // r24.20 review (VISION #8): CLEANUP=0 under the deadline is still
            // r24.15's cleanup — the ticket cancels, the retry list remembers.
            if (!capture_abandon_cleanup_on_() && orphans_.size() < 64) orphans_.push_back(jpeg);
            return;
        }
        std::remove(jpeg.c_str());
        if (orphans_.size() < 64) orphans_.push_back(jpeg);
    }
    void sweep_orphans() {
        if (capture_abandon_cleanup_on_() || capture_deadline_on_()) {
            for (size_t i = 0; i < captures_.size();) {
                if (captures_[i]->state->load(std::memory_order_acquire) != 0)
                    captures_.erase(captures_.begin() + (long)i);
                else ++i;
            }
            // r24.20 review (VISION #8): the retry list is empty unless
            // CLEANUP=0 put something on it; then it is retried as in r24.15.
        }
        for (size_t i = 0; i < orphans_.size(); ) {
            if (std::remove(orphans_[i].c_str()) == 0 || !exists_(orphans_[i]))
                orphans_.erase(orphans_.begin() + (long) i);
            else i++;
        }
    }
    void shutdown() {
        if (capture_abandon_cleanup_on_() || capture_deadline_on_()) {
            // In-flight captures have no future consumer after port teardown.
            for (const auto &capture : captures_)
                if (capture->state->load(std::memory_order_acquire) == 0) capture->abandon();
        }
        workers_.join();
        sweep_orphans();
    }
    ~CameraPort() override { shutdown(); }
    void last_dims(int *w, int *h) const override {
        const long long v = dims_ ? dims_->load(std::memory_order_relaxed) : 0;
        if (w) *w = (int) (v >> 32);
        if (h) *h = (int) (v & 0xFFFFFFFFll);
    }
    void want_dims(int *w, int *h) const override {
        int ww = 0, wh = 0;
        parse_res_(res, &ww, &wh);
        if (w) *w = ww;
        if (h) *h = wh;
    }
    // r24.6 (WO-35, §17/T3): the whole claim, from the same per-capture slot.
    CaptureInfo last_capture() const override { return info_ ? *info_ : CaptureInfo(); }

  private:
    static bool file_ok_(const std::string &p) {
        struct stat s;
        return ::stat(p.c_str(), &s) == 0 && s.st_size > 0;
    }
    static bool exists_(const std::string &p) {
        struct stat s;
        return ::stat(p.c_str(), &s) == 0;
    }
    EncodeWorkers workers_{capture_deadline_on_()};
    std::vector<std::shared_ptr<CaptureCleanup>> captures_;
    std::vector<std::string> orphans_;                     // r24.6 (WO-39 / #43)
    std::shared_ptr<std::atomic<int>> state_ = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<long long>> dims_ =
        std::make_shared<std::atomic<long long>>(0);            // r24.6 (WO-35)
    std::shared_ptr<CaptureInfo>      info_  = std::make_shared<CaptureInfo>();
};
#endif // !_WIN32

// Deterministic port for the battery: a scripted sequence of poll results.
struct FakePort : VisionPort {
    int  next_result   = 1;     // what poll() settles to
    int  polls_to_wait = 0;     // how many polls report "in flight" first
    bool spawn_ok      = true;
    int  captures      = 0;
    int  got_w = 0, got_h = 0;  // r24.6 (WO-35): 0x0 = "not measured"
    int  ask_w = 0, ask_h = 0;
    void last_dims(int *w, int *h) const override { if (w) *w = got_w; if (h) *h = got_h; }
    void want_dims(int *w, int *h) const override { if (w) *w = ask_w; if (h) *h = ask_h; }
    bool start_capture(const std::string &) override {
        if (!spawn_ok) return false;
        captures++;
        waited_ = 0;
        return true;
    }
    int poll() override {
        if (waited_ < polls_to_wait) { waited_++; return 0; }
        return next_result;
    }
  private:
    int waited_ = 0;
};

// ── the one-deep job, and the rig that owns it ──────────────────────────────
struct VisionJob {
    enum class Trig { NONE, INVITED, URGE, AWAY, RECALL };
    Trig        trig = Trig::NONE;
    aev::ActionRef action;
    uint64_t grant_epoch=0;
    bool pending_announced=false;
    std::string jpeg;             // capture destination (RECALL: the kept file itself)
    time_t      asked_at  = 0;    // wall clock at commit (the look's identity)
    double      t0_ms     = 0;    // injected monotonic ms at commit
    std::string recall_gist;      // r20 P3: her gist at keep-time (RECALL only)
    std::string recall_when;      // r20 P3: pre-humanized "when" (RECALL only)
    // r24.6 (WO-35, §17/T3): what the camera actually delivered. 0/0 +
    // !cap_verified means "no claim was made", which is not the same as "it
    // fell back".
    int         cap_w = 0, cap_h = 0;
    bool        cap_verified  = false;
    bool        cap_fell_back = false;
    // ── r24.12 (WO-V2): the ENCODE policy this job was committed under ──────
    // enc_edge: the long edge of the working copy handed to mtmd (0 = native,
    // which is r24.11). acuity: his words asked her to READ, so the job was
    // committed at the acuity size and waits the acuity budget. crop: the
    // --vision-acuity-mode crop alternative (a centred window at native
    // pixels). wait_ms: the drain's wait budget for THIS job (-1 = the ordinary
    // ATHENA_ENC_WAIT_MS); an acuity look and a look she declared herself wait
    // longer, because the whole point of both is that her reply is generated
    // with the picture in front of her.
    int         enc_edge  = 0;
    bool        acuity    = false;
    bool        crop      = false;
    int         crop_w = 0, crop_h = 0;
    int         wait_ms   = -1;
    bool        active() const { return trig != Trig::NONE; }
};

// ── r24.6 (WO-75): the per-capture encode slot ──────────────────────────────
// The CPU CLIP encode measured 4,246 ms at 640x480 and extrapolates to ~32 s
// at the 1080p DD11 holds — inside vision_drain_eval, which runs on the loop
// thread. WO-75 moves the PURE step (bitmap → dhash compute → tokenize →
// encode → copy-out) onto a detached worker; the eval — which mutates n_past,
// the batch and the KV — stays on the loop thread exactly where it was.
//
// One slot per capture, NEVER reused — the r19.6 per-capture-flag discipline
// one layer up (§17.2 item: "a late encode must write to a slot nobody is
// reading, never into the live one"). The worker captures its shared_ptr BY
// VALUE at spawn and never looks anything up, so a worker that outlives its
// look writes into a slot with no reader and touches nothing live. The slot
// is header-pure: mtmd types stay out (this header compiles standalone in the
// battery), so the chunks ride as an owned opaque pointer plus its deleter.
struct EncodeSlot {
    std::atomic<int>   state{0};          // 0 in flight, 1 ready, -1 failed
    std::atomic<bool>  cancelled{false};  // r24.20: this job's close, never a later slot's
    void              *chunks = nullptr;  // mtmd_input_chunks*, owned
    void             (*free_chunks)(void *) = nullptr;   // mtmd_input_chunks_free
    std::vector<float> embd;              // OUR COPY of mtmd_get_output_embd()
                                          // — mtmd_context::image_embd_v is ONE
                                          // shared buffer the next encode
                                          // reallocates, so the copy happens on
                                          // the worker, under the encode lock.
    size_t             img_chunk_i = 0;   // which chunk the copy belongs to
    uint64_t           dhash = 0;         // computed on the worker,
    bool               dhash_valid   = false;   // APPLIED on the loop thread,
    bool               dhash_applied = false;   // exactly once, !is_recall only
    PhotoSig           photo;             // WO-37's second channel, same rule
    bool               photo_valid   = false;
    // r24.14 (WO-R3): the region decomposition, computed on the worker beside
    // the two whole-frame signatures and applied by the loop thread under the
    // SAME once-per-look, look-path-only rule. Empty (valid == false) when
    // ATHENA_LOOK_REGIONS=0, when the frame is too small to tile, or on a
    // recall — and every consumer reads that as "no region opinion", never as
    // "nothing changed".
    RegionSig          region;
    bool               region_valid  = false;
    int                cam = 1;           // camera verdict the worker saw
                                          // (1 ok, 0 timed out, -1 failed)
    double             encode_ms = 0, bitmap_ms = 0, cam_wait_ms = 0;
    int                npos = 0, ntok = 0;
    // r24.12 (WO-V2): the bitmap mtmd was actually handed — the working copy's
    // dimensions after resample/crop (the decoded file's when neither ran).
    // 0x0 = not recorded (a slot built by a path that never resampled). The
    // drain writes them onto the LookNote so the hedge can compare what SHE
    // saw against --image-min-tokens, instead of the camera's frame size.
    int                enc_w = 0, enc_h = 0;
    int                src_w = 0, src_h = 0;   // the decoded file, before the copy
    ~EncodeSlot() { if (chunks && free_chunks) free_chunks(chunks); }
};

// r24.6 (WO-75 / shape F): frame lifetime is STRUCTURAL, not a call site.
// The old drain unlinked un-kept frames inline, and the drain's own abandon
// path unlinked a job's JPEG on the decode_tokens failure path. With deferral,
// vision.pending() is now true ACROSS turns, so that call-site unlink could
// delete a frame the encode worker has not yet opened — a use-after-unlink the
// defer path makes reachable (§17.2 item 1). The owner unlinks in its
// destructor unless release()d, and the destructor can only run once the LAST
// holder — the worker included — has dropped its reference, so the unlink is
// ordered after the worker's final read by construction. release() is taken
// exactly when the keep path assumes ownership (hold_frame). An album file is
// never ours to unlink. This also closes WO-39 #43's orphaned-/tmp leak for
// the async path: the worker waits for the CAPTURE VERDICT before anything
// else, and the verdict is stored only after the file is fully written, so
// unlink-before-write cannot happen through this owner. (The one residue — a
// worker that gives up on a capture still in flight — reports cam == 0 and the
// DRAIN routes that path through VisionPort::abandon() on the loop thread,
// where the WO-39 retry list lives.)
struct FrameOwner {
    std::string       path;
    std::atomic<bool> released{false};
    bool              is_album = false;   // a recall's file is never ours
    void unlink_if_owned() const {
        if (!is_album && !released.load() && !path.empty()) std::remove(path.c_str());
    }
    ~FrameOwner() { unlink_if_owned(); }
    void release() { released.store(true); }
};

// r24.6 (WO-75): the pure decision core of the async drain, factored here so
// the battery can pin it (talk-llama.cpp is compiled by no test binary).
//   state      the slot's published state (0 in flight, 1 ready, -1 failed)
//   defers     defers already charged to THIS look (before this visit)
//   defer_max  the bound — pending() gates compaction and can_look(), so an
//              unbounded defer wedges both (§17.2 item 3)
// A merely SLOW encode must never produce the failure line (§17.2 item 4):
// DEFER carries the "taken, not yet seen" line instead, and only the bounded
// give-up (CLOSE_DEFER_CAP) closes the look — with the never-arrived line,
// which is true, not the camera-failure line, which would not be.
struct EncodeDrainStep {
    enum What { DEFER, CLOSE_DEFER_CAP, FAIL, READY } what = READY;
    bool inject_pending = false;   // first defer of this look says so honestly
    bool count_defer    = false;
};
inline EncodeDrainStep encode_drain_step(int state, int defers, int defer_max) {
    EncodeDrainStep s;
    if (state == 0) {
        if (defers + 1 <= defer_max) {
            s.what = EncodeDrainStep::DEFER;
            s.count_defer    = true;
            s.inject_pending = (defers == 0);   // once per look, not per visit
        } else {
            s.what = EncodeDrainStep::CLOSE_DEFER_CAP;
        }
        return s;
    }
    s.what = state < 0 ? EncodeDrainStep::FAIL : EncodeDrainStep::READY;
    return s;
}

// ── r24.12: the rig's own disable-only switches (this header is a leaf and
// cannot see acon::env_on_; the recall_determiner_on_ idiom is the precedent).
// ATHENA_RECALL_SECOND_DEFER (WO-V5): the staged second image survives a
// DEFER and is committed at the drain's success tail; =0 → clear-on-defer.
inline bool recall_second_defer_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_SECOND_DEFER");
        return !(e && e[0] == '0');
    }();
    return on;
}
// ATHENA_RECALL_REFUSED_NOTE (WO-V8): a recall his words (or hers) asked for
// and can_recall() refused is logged and enters her context as a clause
// (album_refused_line); =0 → r24.11's silent drop.
// r24.12 (review): NAMING — every sibling reader in this block carries the
// trailing underscore (capture_mjpeg_on_, acuity_look_on_, look_bare_on_,
// keep_antecedent_on_, recall_self_intent_on_, recall_second_defer_on_,
// recall_same_file_on_) and this one does not. Left as it is on purpose: the
// name is spelled twice in talk-llama.cpp's album-refusal arms, which are
// another slice's file this round, and introducing a second spelling to fix a
// naming slip would trade a NIT for the duplicate-reader defect the same review
// raised against athena_recall_second_defer_on. One rename plus those two call
// sites, in one edit, whenever talk-llama.cpp is next open.
inline bool recall_refused_note_on() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_REFUSED_NOTE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// ATHENA_RECALL_SAME_FILE (WO-V12): a re-open of a file whose picture is still
// in her context is not charged to the album budget; =0 → every recall charges.
inline bool recall_same_file_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_SAME_FILE");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.18: same-file close_success already exempted a still-present album
// picture, but exhausted-budget admission prevented reaching that exemption.
// Selection may now consider a resident image; commit checks the selected
// path itself. No new file bypasses the budget. Zero restores r24.17 admission.
inline bool recall_context_access_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_CONTEXT_ACCESS");
        return !(e && e[0] == '0');
    }();
    return on;
}
// r24.20: opening an already resident album page, then rolling back that
// repeat, hid the earlier surviving image from recall admission at the cap.
// The actual drain/intake replay retained that older image in context but
// refused it as spent. Review then reproduced the same loss when repeated
// free reopens trimmed the older note. Any surviving opening supplies access;
// trimming preserves a representative with the same rollback standing, along
// with live image metadata and the held photograph's earned description.
// Zero restores oldest-first trimming and the latest-matching-note decision.
inline bool recall_resident_history_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_RECALL_RESIDENT_HISTORY");
        return !(e && e[0] == '0');
    }();
    return on;
}
// ── r24.13 (WO-X1 / ARCH §1.5 row 1): the acuity drain's own tape ───────────
// What was wrong: r24.12 gave a declared or acuity look its OWN wait budget
// (VisionJob::wait_ms, stamped acuity_wait_ms = 120 s), and the drain's wait
// loop polls only the encoder slot and g_stop_requested. Measured by ARCH:
// predict_vision_s(2040) = 48.0 s of encode inside that wait, with no barge
// poll, no recorder, and an audio.clear() on both idle look-drain branches at
// the end of it — a forty-eight-second deaf window ending in the ring being
// discarded. r24.11's ceiling on the same path was one second. On 31 Aug at
// 13:54:57, under r24.12 with an acuity edge, "you still have not activated
// the camera" lands inside that window and is lost.
//
// The fix adds no mechanism: acmp::OutageTape — the recorder WO-C8 built for
// the OTHER blocking window, compaction — is armed around the drain's wait as
// well. This is the RULE, stated once and pure, so the fixture pins what
// production runs rather than a paraphrase of it (the WO-C9 lesson: the
// arming rule used to be locals of main() and test_compact_pump could only
// pin a paraphrase).
//
// Four terms, and every one of them is load-bearing:
//   * look_tape_on   — ATHENA_VISION_LOOK_TAPE, this order's own switch.
//   * seam_on        — ATHENA_OUTAGE_TAPE_SEAM. The tape's consumer IS the
//                      seam (outage_tape_settle); with the seam off there is
//                      nobody to take a tape, so arming one would record into
//                      a buffer nothing ever reads. The tape's global off
//                      switch still turns this off, as the work order says.
//   * !tape_running  — outage_mic is a SINGLE recorder. If one is already
//                      rolling (a compaction tape whose consumer has not taken
//                      it yet) it is ALREADY covering this window and its own
//                      consumer will take it: do not arm over it, do not stop
//                      a thread another path is draining. This is the guard
//                      ARCH calls required, not optional.
//   * wait_ms        — the EFFECTIVE wait budget for this job (the job's own
//                      when it has one, else the ordinary ATHENA_ENC_WAIT_MS).
//                      An ordinary look's budget is 1,000 ms and must NOT arm
//                      a recorder: a one-second drain does not need one, and
//                      arming there would change r24.12 on the common path.
//   * her_audio_settled — r24.13 (review A§1). THE fifth term, and the reason
//                      this order shipped a blocker. The drain is entered from
//                      athena_vision_seam_drain, which plays the announce
//                      ("I'm reading it — one moment.") a few statements
//                      earlier; WO-X1 argued the window was announce-safe
//                      because "vision_drain_eval runs after the announce".
//                      It runs after the announce was WRITTEN, not after it was
//                      HEARD. arm(now) re-bases last_drain_ms to the arm, so
//                      the first drain_once takes audio.get(now2 - now) — a
//                      window that begins at the arm and therefore holds the
//                      remainder of her own sentence, above TAPE_SOUND_GATE,
//                      through trim_to_speech, into replay_armed, and out as
//                      HIS turn. That is worse than the deafness WO-X1 fixes:
//                      r24.12 lost his words, this invents them.
//                      The term is a PROOF, never a deadline: talk-llama's
//                      stream_tts_done_proven reports whether orpheus wrote the
//                      mini-session's `.done` (the same completion
//                      stream_tts_oneshot and the turn loop take), and only the
//                      proven branch may arm. An announce that runs longer than
//                      any bound anyone picks therefore cannot reach the tape —
//                      the window simply does not open, which is r24.12's own
//                      deaf drain and nothing worse. NO DEFAULT ARGUMENT: this
//                      is precisely the call-site assumption that produced the
//                      defect, so every caller states it, the way
//                      vision_drain_eval's `live_turn` is stated.
// ATHENA_VISION_LOOK_TAPE=0 → this returns false everywhere, nothing is armed,
// nothing is started, nothing is stopped, and the drain is r24.12 exactly,
// including its audio.clear().
static constexpr int LOOK_TAPE_MIN_WAIT_MS = 1000;   // = the ordinary ATHENA_ENC_WAIT_MS
inline bool look_tape_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_LOOK_TAPE");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline bool look_tape_due(bool look_tape_on, bool seam_on, bool tape_running,
                          int wait_ms, bool her_audio_settled) {
    if (!look_tape_on || !seam_on || tape_running) return false;
    if (!her_audio_settled) return false;   // r24.13 (review A§1)
    return wait_ms > LOOK_TAPE_MIN_WAIT_MS;
}

// ── r24.13 (review D§4 / D§5): where the fifth term COMES FROM ──────────────
//
// What was wrong. A§1 computed `her_audio_settled` from a file-scope
// `g_her_tts_unproven` in talk-llama.cpp, written only by
// `stream_tts_done_proven` and read once at the tape's open. Two of the five
// `vision_drain_eval` call sites — the away-look and the deferred-look drains —
// play no announce and so never write it, and the main turn loop's own reply
// wait does not go through that function either. So the term was decided by a
// flag left over from some earlier, unrelated utterance: one timed-out
// `heard_ok` chime latched the tape shut for every subsequent no-announce
// drain, for the rest of the session, and nothing printed. WO-X1 could be
// silently dead. The comment asserted the opposite ("the next proven utterance
// clears it") and there is no next proven utterance on a path that never
// announces.
//
// The term is a property of THIS drain, so it is composed from what this drain
// knows, and the composition is stated once — here, beside `look_tape_due`,
// for the same reason that rule is here (the WO-C9 lesson: production and the
// fixture must ask ONE function, not two that agree today).
//
//   * announced          — did this drain play an announce at all. A drain
//                          with none (the away look, the deferred look, a
//                          `no_fillers` seam) has nothing to be unproven about
//                          and MUST be able to arm: refusing there is r24.12's
//                          deaf window for no reason at all.
//   * announce_proven    — if it did announce, did orpheus write that
//                          session's `.done` (talk-llama's
//                          `stream_tts_done_proven`). Meaningless when
//                          `announced` is false, and never consulted there.
//   * unwaited_outstanding — is there an utterance of hers this process
//                          STARTED and has never seen finish. The tree has
//                          exactly one: the pivot acknowledgment, played at
//                          `aseam::pivot_ack_early` and reaped non-blockingly
//                          by `pivot_ack_done_reap`. It is separate from the
//                          announce's own proof because a `.done` on disk is
//                          not bound to the utterance that wrote it — the
//                          announce's poll can take the ack's late `.done` and
//                          declare her settled while the announce is still
//                          playing (D§5, reproduced with a stub orpheus at
//                          601 ms against a 1,400 ms announce). It refuses
//                          from either direction, which is what makes that
//                          race unable to open the window.
//
// A REFUSAL, never a permission: it can only take the window away, exactly as
// the fifth term of `look_tape_due` can. NO DEFAULT ARGUMENTS, for the reason
// stated at that term.
inline bool her_audio_settled(bool announced, bool announce_proven,
                              bool unwaited_outstanding) {
    if (unwaited_outstanding) return false;     // r24.13 (review D§5)
    return !announced || announce_proven;       // r24.13 (review D§4)
}

// r24.16: failed image evaluation is not a new accessible scene. Both drain
// paths used to replace the comparison baseline before headroom/eval succeeded;
// the next successful look then compared with an image she never received.
// ATHENA_VISION_SCENE_COMMIT=0 restores that pre-eval progression.
inline bool vision_scene_commit_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_SCENE_COMMIT");
        return !(e && e[0] == '0');
    }();
    return on;
}

// r24.18: source identity and context presence already existed for album
// pictures, but live descriptions used two turn booleans and a free-standing
// caption. An actual intake/tail replay gave a new photo its predecessor's
// caption, then named the room from later portrait discussion. The same replay
// lost its age hedge at a context cut. Apply the existing note identity to
// every image. Literal zero restores r24.17's caption/description/hedge paths.
inline bool look_source_ownership_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_LOOK_SOURCE_OWNERSHIP");
        return !(e && e[0] == '0');
    }();
    return on;
}
// A successful album request used to skip the camera's permission answer too:
// "No photos. Pull up your self-image" opened the album and kept grant=2.
// These are independent acts. Zero restores the r24.17 routing gate.
inline bool vision_intake_scope_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_VISION_INTAKE_SCOPE");
        return !(e && e[0] == '0');
    }();
    return on;
}

class VisionRig {
  public:
    // Wiring (set once at startup by talk-llama.cpp; FakePort in the battery).
    VisionPort *port      = nullptr;
    bool        enabled   = false;      // mmproj loaded AND ATHENA_VISION != 0
    int         look_max  = 10;         // your decision (--look-max / ATHENA_LOOK_MAX)
    int         recall_max = 3;         // separate album budget (--recall-max / ATHENA_RECALL_MAX)
    // ── r24.12 (WO-V2): the encode policy (set once at startup from the flags /
    // env; see the comment above resample_rgb). work_edge 0 = native = r24.11.
    // Every commit_* below stamps the pending job with it; the worker reads the
    // job, never these fields, so a change mid-session cannot reach a look in
    // flight. acuity_wait_ms is the drain's wait budget for a look whose reply
    // must not be generated before the picture arrives (an acuity look, a look
    // she declared herself — WO-V3).
    int         work_edge      = 0;     // --vision-work-edge  (ATHENA_VISION_WORK_EDGE)
    int         acuity_edge    = 1920;  // --vision-acuity-edge
    bool        acuity_crop    = false; // --vision-acuity-mode crop
    int         acuity_wait_ms = 120000;// ATHENA_ACUITY_WAIT_MS
    // r24.12 (WO-V8): the rig's own measure of how fast this machine sees —
    // an EMA of measured/predicted (predict_vision_s) updated at every drain,
    // 1.0 until the first look (the S22 fit is the prior).
    double      cost_scale     = 1.0;
    void note_vision_cost(int tokens, double measured_s) {
        if (tokens <= 0 || measured_s <= 0.0) return;
        const double pred = predict_vision_s(tokens, 1.0);
        if (pred <= 0.0) return;
        const double ratio = measured_s / pred;
        cost_scale = cost_scale <= 0.0 ? ratio : 0.7 * cost_scale + 0.3 * ratio;
        if (cost_scale < 0.2) cost_scale = 0.2;
        if (cost_scale > 5.0) cost_scale = 5.0;
    }
    // Seconds a job of `tokens` image tokens is expected to take on THIS rig.
    double predict_s(int tokens) const { return predict_vision_s(tokens, cost_scale); }
    // The tokens a file of w×h will cost under the policy `acuity` selects.
    int tokens_under_policy(int w, int h, bool acuity) const {
        if (w <= 0 || h <= 0) return 0;
        if (acuity && acuity_look_on_()) {
            if (acuity_crop) return image_tokens_for_crop(w, h, acuity_edge, acuity_edge * 9 / 16);
            return image_tokens_for_edge(w, h, acuity_edge);
        }
        return image_tokens_for_edge(w, h, work_edge);
    }

    // ── r20p3.10: the durable record of a look ──────────────────────────────
    //
    // An image reaches her context as ~46 encoded positions (measured, S8:
    // "46 image pos in 17.9s"). Those positions cannot be reconstructed from
    // text, and re-encoding costs a full mmproj pass — 9,693 ms in that same
    // session. So an image cannot cross a context compaction the way words can.
    //
    // What CAN cross is what she said about it. That already exists and is
    // already hers rather than a caption: talk-llama.cpp captures her words on
    // the turn after a look into athena_last_look_gist, and uses them as the
    // gist when she keeps the picture. Until now that gist was discarded for
    // every look she did NOT keep — which is most of them.
    //
    // A LookNote keeps it for all of them, for the life of the session, so the
    // reload block after a compaction can carry "at 23:59 I looked, and this is
    // what I saw" in her own words. The picture goes; the seeing stays.
    //
    // Kept pictures need none of this to survive ACROSS sessions — keepsakes.tsv
    // is append-only and keepsakes_as_entries() already renders each one into
    // her memory block marked "[a kept image — I can open it and look again]".
    // What the note adds for a keepsake is the within-session half: a keep made
    // after the prefix was built is not in the prefix, so without this it would
    // vanish at the first cut and reappear only next session.
    std::string source_session="legacy-vision";
    struct ActionState {
        aev::ActionRef action;
        bool captured=false,encoded=false,admitted=false,kept=false,recalled=false;
        std::string outcome="requested",description,kept_file;
        long wall=0,epoch=0,capture_observed_wall=0;
        uint64_t revision=0,grant_epoch=0;
        bool admitted_once=false;
    };
    std::function<void(const ActionState&)> action_sink; // foreground only; never called by encoder/audio workers
    std::vector<ActionState> action_history;
    aev::ActionRef last_completed_action;
    aev::SourceRef input_source; // foreground accepted utterance; empty means autonomous/legacy origin
    std::function<void(long,const std::string&)> epoch_sink;
    bool admission_allowed(const VisionJob&j)const {
        return job_.active()&&job_.action.id==j.action.id&&
            (j.trig==VisionJob::Trig::RECALL||j.grant_epoch==perm.epoch);
    }
    void record_action(const VisionJob&j,const std::string&outcome,bool captured,bool encoded,bool admitted) {
        if(j.action.id.empty())return; // no synthetic completion for an inactive legacy job
        auto it=std::find_if(action_history.begin(),action_history.end(),[&](const ActionState&a){return a.action.id==j.action.id;});
        if(it==action_history.end()){action_history.push_back({});it=action_history.end()-1;it->action=j.action;it->wall=j.asked_at;}
        if(captured&&!it->captured)it->capture_observed_wall=(long)time(nullptr);
        it->captured|=captured;it->encoded|=encoded;it->admitted=admitted;it->admitted_once|=admitted;it->recalled=j.trig==VisionJob::Trig::RECALL;
        it->outcome=outcome;it->epoch=ctx_gen_;it->grant_epoch=j.grant_epoch;++it->revision;
        if(action_sink)action_sink(*it);
        if(action_history.size()>256)action_history.erase(action_history.begin());
    }
    void pending_announced(){if(job_.active())job_.pending_announced=true;}
    struct LookNote {
        long        when   = 0;      // epoch at capture
        std::string gist;            // HER words about it, from the turn after
        bool        kept   = false;
        bool        recall = false;  // an album re-open rather than a fresh look
        std::string file;            // keepsake filename once kept
        bool        described() const { return !gist.empty(); }
        // ── r24.6 (WO-35): how much of the world this look actually resolved ──
        // 0x0 means the port did not measure it — which is what every look
        // before r24.6 reports, and it must read as "unknown", never as "small".
        int         w = 0, h = 0;
        int         want_w = 0, want_h = 0;
        // §17/T3 spelling of the same fact, kept in lockstep by note_look()
        // and note_look_dims() — a look taken through a 640x480 fallback is a
        // look she should hedge about; 0/0 + !cap_verified asserts nothing.
        int         cap_w = 0, cap_h = 0;
        bool        cap_verified = false;
        bool measured()  const { return w > 0 && h > 0; }
        // ── r24.6 (WO-38 / S19 D6): how old this still is ────────────────────
        // Turn 29 asserted present-tense continuous sight from a 90-second-old
        // frame — "But looking at you now, you're just a person. Tired, holding
        // a children's book, sitting in a dim room" — contradicting her own
        // turn-28 "The lighting is way better." An inner thought at 14:57:36
        // asserted "The light shifts in the room when he speaks", 27 minutes
        // after her last still. Nothing anywhere held the age of the frame, so
        // there was nothing for a continuous-sight assertion to contradict.
        // Word-valued: no bare numeral reaches her context (F18/S18-18).
        long age_s(long now_wall) const { return when > 0 ? now_wall - when : -1; }
        // r24.7 (WO-84(2)): the ladder is elapsed_words() now — one scale,
        // shared with the recall envelope's "from …" phrase. Identical strings
        // for every in-session age; past a day it answers in day/week words
        // where the old final arm said "hours ago" to any question at all.
        std::string age_words(long now_wall) const { return elapsed_words(age_s(now_wall)); }
        bool fell_back() const {
            return measured() && want_w > 0 && want_h > 0 && (w != want_w || h != want_h);
        }
        int  tokens() const { return image_tokens_for(w, h); }
        bool coarse(int min_tokens) const { return frame_undersized_(w, h, min_tokens); }
        // ── r24.12 (WO-V2): what mtmd was HANDED, beside what the camera gave ─
        // With the working copy the two differ by design: a 1920x1080 capture
        // is encoded as 800x450 (350 tokens). The capture size stays in w/h
        // (the WO-35 record, the fallback test); enc_w/enc_h is the size her
        // eyes actually received. 0x0 = not recorded (a note from a path that
        // never resampled — r24.11 notes, the fixtures) and then the capture
        // size answers, which is what it always did.
        int  enc_w = 0, enc_h = 0;
        bool acuity_asked  = false;   // his words asked her to READ this one
        int  seen_w() const { return enc_w > 0 ? enc_w : w; }
        int  seen_h() const { return enc_h > 0 ? enc_h : h; }
        // The tokens she actually got, by mtmd's own rounding when the encode
        // size is known; the r24.6 ceil arithmetic on the capture otherwise.
        int  seen_tokens() const {
            return enc_w > 0 && enc_h > 0 ? image_tokens_smart(enc_w, enc_h) : tokens();
        }
        bool coarse_seen(int min_tokens) const {
            return min_tokens > 0 && seen_w() > 0 && seen_h() > 0 && seen_tokens() < min_tokens;
        }
        // r24.12 (WO-V12): the source file and context generation. r24.18
        // records these for live images too; neither field is persisted.
        std::string opened;
        long        ctx_gen = 0;
        aev::ActionRef action;
    };
    std::vector<LookNote> look_notes;   // this session, oldest first, bounded

    // Bounded by the budgets themselves plus slack for recalls and for the
    // away-grant. A look she never described still gets a note — "I looked and
    // said nothing about it" is true and worth carrying.
    size_t look_notes_max() const {
        return (size_t) std::max(4, look_max + recall_max + 4);
    }
    // r24.6 (WO-35, §17/T3): the size may ride the note from birth. The old
    // 2-arg call still compiles and asserts nothing — defaults are "no claim".
    void note_look(long when_wall, bool is_recall, int cap_w = 0, int cap_h = 0,
                   bool cap_verified = false, const std::string &source_file = std::string()) {
        LookNote n; n.when = when_wall; n.recall = is_recall;
        n.action=last_completed_action;
        if (look_source_ownership_on_()) { n.opened = source_file; n.ctx_gen = ctx_gen_; }
        n.cap_w = cap_w; n.cap_h = cap_h; n.cap_verified = cap_verified;   // r24.6 (WO-35)
        if (cap_verified) { n.w = cap_w; n.h = cap_h; }    // one fact, both spellings
        look_notes.push_back(n);
        // ── r24.12 (review): the rollback mark moves with the vector ─────────
        // What was wrong: note_snapshot() stores an INDEX into look_notes, and
        // this trim erases from the FRONT — so after e erasures snap_notes_
        // named a note e positions later than the one it was taken at, and
        // note_rollback() left up to e post-snapshot recall notes with
        // ctx_gen == ctx_gen_ ("still in context") after the barge rollback had
        // removed their pictures. recall_in_context() then answers true for a
        // keepsake that is gone, and close_success() charges nothing for
        // re-opening it (WO-V12). Reachable because WO-V12's free re-open does
        // NOT increment recalls_used, so look_notes is no longer bounded by
        // look_max + recall_max and a session that re-opens the same keepsake
        // can push past look_notes_max() (25 at --look-max 15 --recall-max 6)
        // and start trimming. Decrementing here keeps the mark on the note it
        // was taken at; a mark of zero is already "everything", so it floors.
        // Nothing restores: this is a defect in a r24.12 mechanism that does
        // not exist at ATHENA_RECALL_SAME_FILE=0.
        while (look_notes.size() > look_notes_max()) {
            size_t drop = 0;
            if (recall_resident_history_on_()) {
                // r24.20: the bounded visual record is also the resident
                // album index. A repeated opening after the snapshot must
                // not evict the only surviving opening before it. Drop the
                // first note whose removal leaves its source represented;
                // a pre-snapshot copy survives everything a later copy does.
                // Live images still in context and the held photograph's
                // description need their own record too: fourth review
                // reproduced a newer live note being evicted for an older
                // album page, even while its file was still held for a keep.
                // Keep the newest note for the drain's following metadata
                // writes. Unique resident files are bounded by the existing
                // album budget; repeated reopens need no larger record.
                for (size_t i = 0; i + 1 < look_notes.size(); ++i) {
                    const LookNote &old = look_notes[i];
                    const bool held_source = !old.recall && !old.opened.empty() &&
                                             old.opened == held_.jpeg;
                    bool represented = !held_source && (old.opened.empty() || !image_in_context(old));
                    for (size_t j = 0; old.recall && !represented && j < look_notes.size(); ++j)
                        represented = i != j && look_notes[j].recall &&
                            look_notes[j].opened == old.opened && image_in_context(look_notes[j]) &&
                            (i >= snap_notes_ || j < snap_notes_);
                    if (represented) { drop = i; break; }
                }
            }
            look_notes.erase(look_notes.begin() + (long)drop);
            if (drop < snap_notes_) snap_notes_--;
        }
    }
    // r24.6 (WO-35): the drain calls this right after a committed capture, with
    // whatever the port measured. Lands on the most recent note, which is the
    // one note_look() just pushed. Zero dims are stored as zero and mean
    // "unknown"; nothing downstream may read that as "small".
    //
    // ── r24.8 (WO-111): the parameter that was considered and refused ───────
    // WO-101's aseam::CaptureRead carries a `verified` flag that no production
    // consumer read; this function's `got_w > 0 && got_h > 0` was named as the
    // inference that replaced it, and passing the flag in here was the proposed
    // repair. It is not one. `CaptureRead::verified` is not a second source of
    // truth about the same fact — it IS this expression, and read_capture's own
    // body proves it without reference to any port: the record arm is entered
    // only under `if (ci.verified)`, and inspect_capture sets that flag solely
    // from jpeg_sof_dims(), which returns true only for `wid > 0 && hgt > 0`;
    // the pair arm then assigns `r.verified = (r.got_w > 0 && r.got_h > 0)`
    // verbatim. So the flag equalled the inference for every input at every
    // port, and a parameter carrying it could never have changed an outcome —
    // dead wiring that reads as live, which is a worse census row than the one
    // it would have closed. The field was dropped instead; the question kept.
    // See CaptureRead below.
    void note_look_dims(int got_w, int got_h, int want_w, int want_h) {
        if (look_notes.empty()) return;
        LookNote &n = look_notes.back();
        n.w = got_w; n.h = got_h; n.want_w = want_w; n.want_h = want_h;
        // §17/T3 spelling stays in lockstep: measured dims are a verified claim.
        if (got_w > 0 && got_h > 0) {
            n.cap_w = got_w; n.cap_h = got_h; n.cap_verified = true;
        }
    }
    // r24.12 (WO-V2): the drain records what mtmd was handed (the slot's
    // enc_w/enc_h) and whether the look was an acuity one, on the note
    // note_look() just pushed. 0x0 records nothing, as note_look_dims does.
    void note_look_encoded(int enc_w, int enc_h, bool acuity_asked) {
        if (look_notes.empty()) return;
        LookNote &n = look_notes.back();
        if (enc_w > 0 && enc_h > 0) { n.enc_w = enc_w; n.enc_h = enc_h; }
        n.acuity_asked = acuity_asked;
    }
    // r24.12 (WO-V12): a RECALL note remembers the album file it opened and the
    // context generation it landed in, so a re-open of the same file while its
    // embeddings still survive is not charged again (see close_success).
    void note_recall_opened(const std::string &file) {
        if (look_notes.empty() || file.empty()) return;
        LookNote &n = look_notes.back();
        if (!n.recall) return;
        n.opened  = file;
        n.ctx_gen = ctx_gen_;
    }
    // The context was CUT (a compaction cut, the overflow truncation): every
    // image landed before this is gone from her eyes. A ROLL restores the
    // context byte for byte and is not a cut.
    void note_context_cut() { ctx_gen_++; ctx_revision_++;if(epoch_sink)epoch_sink(ctx_revision_,"cut");for(auto&a:action_history){a.admitted=false;a.epoch=ctx_gen_;++a.revision;if(action_sink)action_sink(a);} }
    long context_revision() const { return ctx_revision_; }
    bool image_in_context(const LookNote &n) const { return n.ctx_gen == ctx_gen_; }
    // Independent publication-repair checkpoint. It must not overwrite the
    // barge snapshot: the two snapshots can bracket different image admits.
    struct AdmissionCheckpoint {long generation=0;std::set<std::string> actions,notes;};
    static std::string admission_note_key(const LookNote&n){return n.action.id.empty()?"legacy/"+std::to_string(n.when)+"/"+n.opened:n.action.id;}
    AdmissionCheckpoint admission_checkpoint() const {
        AdmissionCheckpoint mark;mark.generation=ctx_gen_;
        for(const auto&a:action_history)if(a.admitted)mark.actions.insert(a.action.id);
        for(const auto&n:look_notes)if(image_in_context(n))mark.notes.insert(admission_note_key(n));
        return mark;
    }
    void restore_admission_checkpoint(const AdmissionCheckpoint&mark){
        ctx_gen_=mark.generation;++ctx_revision_;
        for(auto&n:look_notes)n.ctx_gen=mark.notes.count(admission_note_key(n))?ctx_gen_:-1;
        for(auto&a:action_history){a.admitted=mark.actions.count(a.action.id)>0;a.epoch=ctx_gen_;++a.revision;if(action_sink)action_sink(a);}
        if(epoch_sink)epoch_sink(ctx_revision_,"publication-repair rollback");
    }
    // A barge ROLLBACK restores the snapshot the loop took at the last idle (or
    // pivot, or post-compaction): only the notes pushed SINCE that snapshot
    // describe pictures that are now gone. note_snapshot() marks where the
    // notes stood; note_rollback() takes the standing of everything after it.
    void note_snapshot() { snap_notes_ = look_notes.size(); snap_epoch_++; }
    void note_rollback() {
        for (size_t i = snap_notes_; i < look_notes.size(); i++) {
            look_notes[i].ctx_gen = -1;
            for(auto&a:action_history)if(a.action.id==look_notes[i].action.id){a.admitted=false;++a.revision;if(action_sink)action_sink(a);}
        }
        // A rollback can remove only the age clause, with every image still
        // present. Its memo must retry too, without invalidating those images.
        //
        // ── r24.20 review (VISION #5): only when the rollback REMOVED it ────
        // r24.18 bumped the memo's revision on EVERY rollback. The rollback
        // restores the last idle/pivot snapshot, and every snapshot is taken
        // BEFORE the turn's clause is decoded (idle → his turn + clause; pivot
        // snapshot → pivot field + clause). So a clause entered since the last
        // snapshot is in the rolled-back region and IS lost; a clause entered
        // in an earlier epoch survives the rollback — and r24.18 re-decoded it
        // beside the surviving copy, once per barge for as long as the note was
        // stale (the R5-AR stutter class). The memo now records the snapshot
        // epoch the clause was entered in (stale_look_clause, when it updates
        // `said`), and the revision moves exactly when that epoch is the one
        // being rolled back. A cut still bumps unconditionally (a cut removes
        // the clause whatever its epoch). ATHENA_STALE_CLAUSE_EPOCH=0 restores
        // the r24.18 bump-on-every-rollback.
        if (!stale_clause_epoch_on_() || clause_epoch_ == snap_epoch_) {ctx_revision_++;if(epoch_sink)epoch_sink(ctx_revision_,"rollback");}
    }
    // r24.20 review (VISION #5): the clause is entering her context now, in
    // the current snapshot epoch. `const` because stale_look_clause's rig is —
    // this is memo state, the rig-side half of the caller's `said`.
    void note_stale_clause_entered() const { clause_epoch_ = snap_epoch_; }
    static bool stale_clause_epoch_on_() {
        static const bool on = []() {
            const char *e = ::getenv("ATHENA_STALE_CLAUSE_EPOCH");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    // Is this album file's picture still in her context — opened this session,
    // with no cut since it landed?
    bool recall_in_context(const std::string &file) const {
        if (file.empty()) return false;
        for (size_t i = look_notes.size(); i-- > 0; )
            if (look_notes[i].recall && look_notes[i].opened == file &&
                (!recall_resident_history_on_() || image_in_context(look_notes[i])))
                return look_notes[i].ctx_gen == ctx_gen_;
        return false;
    }
    // The most recent fresh (non-recall) look, or nullptr. The frame asks this
    // whether to hedge about resolution and about age.
    const LookNote *last_fresh_look() const {
        for (size_t i = look_notes.size(); i-- > 0; )
            if (!look_notes[i].recall) return &look_notes[i];
        return nullptr;
    }
    // Her words land only on the newest image still in context. OFF uses the
    // prior reverse search for any unnamed note. Neither path overwrites a
    // description already earned from an earlier image.
    void note_look_words(const std::string &words) {
        if(words.empty())return;
        for(size_t i=look_notes.size();i-->0;){
            auto&n=look_notes[i];
            if(look_source_ownership_on_()&&(i+1!=look_notes.size()||!image_in_context(n)))return;
            if(n.described())continue;
            n.gist=words;
            for(auto&a:action_history)if(a.action.id==n.action.id){a.description=words;++a.revision;if(action_sink)action_sink(a);break;}
            return;
        }
    }
    // The keep landed. A supplied source identifies the actual held frame;
    // old callers and OFF retain the most recent unkept live note fallback.
    void note_look_kept(const std::string &file, const std::string &source_file = std::string()) {
        // r20p3.11 (RV5): recalls are skipped. A keep always acts on held_,
        // and hold_frame() is never called for a recall — so a recall note can
        // never be the frame being kept. Without this, opening the album
        // between a look and its keep marked the ALBUM entry as newly kept and
        // left the picture she actually kept rendering as merely looked at: a
        // possession she does not have, attached to the wrong picture, in her
        // own recollection after a cut.
        for (size_t i = look_notes.size(); i-- > 0; ) {
            if (look_source_ownership_on_() && !source_file.empty() &&
                look_notes[i].opened != source_file) continue;
            if (!look_notes[i].kept && !look_notes[i].recall) {
                look_notes[i].kept = true; look_notes[i].file = file;
                for(auto&a:action_history)if(a.action.id==look_notes[i].action.id){a.kept=true;a.kept_file=file;++a.revision;if(action_sink)action_sink(a);break;}
                return;
            }
        }
    }
    std::string frame_dir = "/tmp";     // un-kept frames live here briefly, then unlink

    // ── r24.6 (WO-39 / review #43): the startup sweep ────────────────────────
    // The abandon list closes the leak WITHIN a session; a process that dies
    // between the drain's give-up and the slow thread's write still leaves the
    // file. Called once at startup, this removes every athena-look-*.jpg in
    // frame_dir whose embedded wall clock is older than `older_than_s`
    // (default: one day). NOTE the prefix — the fix plan says
    // "/tmp/athena-frame-*", but commit_() writes frame_dir + "/athena-look-"
    // + <wall> + "-" + <seq> + ".jpg" (see commit_()). A sweep on "athena-frame-"
    // would have matched nothing and reported success.
    //
    // Deliberately parses the epoch out of the NAME rather than stat()ing:
    // mtime can be refreshed by a filesystem the process does not control, and
    // a keepsake never lives here (append_keepsake copies to
    // <memory_dir>/keepsakes/), so no kept image is reachable by this scan.
    // Returns how many it removed, so the caller can log it honestly.
    int sweep_stale_frames(long now_wall, long older_than_s = 86400) const {
        int n = 0;
#if !defined(_WIN32)
        DIR *d = ::opendir(frame_dir.c_str());
        if (!d) return 0;
        static const char *PFX = "athena-look-";
        const size_t plen = std::strlen(PFX);
        while (struct dirent *e = ::readdir(d)) {
            const std::string nm = e->d_name;
            if (nm.compare(0, plen, PFX) != 0) continue;
            if (nm.size() < plen + 5 ||
                nm.compare(nm.size() - 4, 4, ".jpg") != 0) continue;
            const long born = std::strtol(nm.c_str() + plen, nullptr, 10);
            if (born <= 0 || now_wall - born < older_than_s) continue;
            if (std::remove((frame_dir + "/" + nm).c_str()) == 0) n++;
        }
        ::closedir(d);
#else
        (void) now_wall; (void) older_than_s;
#endif
        return n;
    }

    // Session state (per process run == per session, like the interject cap).
    int  looks_used   = 0;
    int  recalls_used = 0;

    bool can_look()   const { return enabled && port && !job_.active() && looks_used   < look_max; }
    // A recall needs no camera — the file already exists — so no port gate.
    bool can_recall(const std::string &file = std::string()) const {
        if (!enabled || job_.active()) return false;
        if (recalls_used < recall_max) return true;
        if (!recall_context_access_on_() || !recall_same_file_on_()) return false;
        if (!file.empty()) return recall_in_context(file);
        // With no selected path this is only admission to the selector.
        // recall_in_context shares its surviving-opening policy with selected
        // admission and close_success; OFF keeps the newer-note override.
        for (const auto &n : look_notes)
            if (n.recall && !n.opened.empty() && image_in_context(n) &&
                recall_in_context(n.opened)) return true;
        return false;
    }
    bool eyes_spent() const { return enabled && looks_used >= look_max; }
    bool pending()    const { return job_.active(); }
    const VisionJob &job() const { return job_; }

    // ── r24.6 (WO-75): the live encode slot for the pending look ────────────
    // Loop thread owns BOTH pointers; the worker holds its own copies by
    // value. close_success()/close_failure() drop them, so every existing
    // close site — the drain, the decode-failure abandon path — releases the
    // slot without knowing it exists. The FrameOwner then unlinks the frame
    // when the last holder (possibly a still-running worker) lets go.
    std::shared_ptr<EncodeSlot> enc_slot;
    std::shared_ptr<FrameOwner> frame_owner;
    int enc_defers = 0;
    // ── r24.6 (WO-75, verifier pass): the defer bound, sized so it cannot
    // misfire on a LEGITIMATE encode. pending() gates compaction and
    // can_look(), so the bound is what keeps a wedged encode from wedging both
    // (§17.2 item 3) — but defers are spent at the PRE-REPLY seam, one per
    // user turn (the idle seam and the wake predicate both gate on
    // drain_due(), which is false while the encode is in flight, so they
    // never spend one). At the old cap of 4, an impatient user speaking every
    // ~5 s (short interjection + 0.8 s endpoint + ASR + this drain's own 1 s
    // wait) spends the 4 defers by ~t=20 s and the FIFTH visit at ~t=25 s
    // closes the look as never-arrived — inside the ~32 s a real 1080p encode
    // measurably takes (§16.4: 2,040 tok x ~15.6 ms/tok). 8 puts the earliest
    // possible close at ~8 visits x >=5 s >= 40 s of wall clock under one
    // visit per turn, past any legitimate encode this rig can produce, while
    // still bounding a truly wedged encode to under a minute of pending().
    // ATHENA_ENC_DEFER_MAX overrides (1..64); the default is the safe one.
    static int enc_defer_max_() {
        if (const char *e = ::getenv("ATHENA_ENC_DEFER_MAX")) {
            char *end = nullptr;
            const long v = ::strtol(e, &end, 10);
            if (end && end != e && *end == '\0' && v >= 1 && v <= 64) return (int) v;
        }
        return 8;
    }
    static inline const int ENC_DEFER_MAX = enc_defer_max_();
    bool enc_resolved() const {
        return enc_slot && enc_slot->state.load(std::memory_order_acquire) != 0;
    }
    // May a drain visit actually land something right now? True for the sync
    // path (no slot — the drain does its own waiting) and for a slot whose
    // worker has published a verdict. False while the encode is still in
    // flight, so the loop's wake predicate does not spin the defer budget
    // away at ~1 Hz against a 32 s encode: the deferred look wakes the loop
    // when it can land, not while it cannot.
    bool drain_due() const {
        return pending() && (!enc_slot || enc_resolved());
    }
    // The dhash/photometric pair, computed on the worker and APPLIED here on
    // the loop thread — LOOK path only, exactly once per look (WO-75 must-not-
    // break: WO-36 and the G5 look-expectation depend on the once-per-look
    // update; a recall never touches scene continuity; a defer touches
    // nothing). Returns the WO-37 change score (-1 sentinel: no prior scene).
    // r24.13 (WO-V2 / ARCH §1.2): `dmean_out` — the SIGNED mean-luma difference
    // from the previous look. photo_delta returns a magnitude (its own absd),
    // and the previous mean is only in scope INSIDE this method, because the
    // very next line overwrites scene_photo: an out-parameter is required here,
    // where a second subtraction at the call site would work on the sync path.
    // Both look paths must produce it or the two would disagree about the same
    // look — the WO-37 call-site-parity rule. Defaulted, so every r24.12 caller
    // is unchanged; 0 with no prior scene, which is the "no direction" answer.
    // r24.14 (WO-R3): `region_out` — the figure/ground read against the PREVIOUS
    // look, taken on the same line as the two scalars and for the same reason
    // the WO-V2 dmean is an out-parameter: the previous signature is only in
    // scope inside this method, because the lines below overwrite it. Both look
    // paths must produce it or the two would disagree about the same look (the
    // WO-37 call-site-parity rule). Defaulted, so every r24.13 caller compiles
    // and behaves unchanged; invalid with no prior scene, which every consumer
    // reads as "no region opinion".
    int apply_scene_from_slot(EncodeSlot &s, bool is_recall,
                              int *hamming_out = nullptr, int *photo_out = nullptr,
                              int *dmean_out = nullptr, RegionRead *region_out = nullptr,
                              int gate = 0) {
        int hamming = -1, photo = 0, change = -1, dmean = 0;
        RegionRead region;
        if (!is_recall && s.dhash_valid && !s.dhash_applied) {
            if (scene_valid) {
                hamming = hamming64(s.dhash, scene_hash);
                if (s.photo_valid) {
                    photo = photo_delta(s.photo, scene_photo);
                    if (scene_photo.valid) {
                        const float d = s.photo.mean - scene_photo.mean;
                        dmean = (int) (d < 0.0f ? d - 0.5f : d + 0.5f);   // no <cmath> in this header
                    }
                }
                // r24.14 (WO-R3): read BEFORE scene_region is overwritten below.
                if (s.region_valid && gate > 0) region = region_read(scene_region, s.region, gate);
            }
            change = look_change_score(hamming, photo);
            scene_hash  = s.dhash;
            if (s.photo_valid) scene_photo = s.photo;
            if (s.region_valid) scene_region = s.region;
            scene_valid = true;
            s.dhash_applied = true;
        }
        if (hamming_out) *hamming_out = hamming;
        if (photo_out)   *photo_out   = photo;
        if (dmean_out)   *dmean_out   = dmean;
        if (region_out)  *region_out  = region;
        return change;
    }

    // r20 P2: the standing permission state (seam-owned, dies with the object).
    LookPermission perm;

    // Commit an invited look: fire the grab NOW (capture is the moment of
    // choice), queue the job for the drain seam. False = nothing committed
    // (budget spent, port dead, or a job already in flight — one deep, always).
    // r24.12 (WO-V2): `acuity` — his words asked her to READ (acuity_requested
    // at the call site); the job is committed at the acuity size with the
    // acuity wait budget. Defaulted, so every r24.11 caller is unchanged.
    bool commit_invited(time_t now_wall, double now_ms, bool acuity = false) {
        return commit_(VisionJob::Trig::INVITED, now_wall, now_ms, acuity);
    }

    // r20 P2: commit a look SHE wanted. Requires consent to already stand —
    // either a fresh ONCE (the caller just parsed his yes) or a session grant
    // — and honors the absence rule: while he is away, only the away-grant
    // (his exact words) licenses a look. `fresh_once` is the caller saying
    // "his yes just landed"; everything else reads the standing grant.
    bool commit_self(time_t now_wall, double now_ms, bool he_present, bool fresh_once,
                     bool acuity = false) {
        const bool licensed = fresh_once || perm.grant >= 1;
        if (!licensed) return false;
        if (!he_present && perm.grant < 2) return false;   // the absence rule
        return commit_(he_present ? VisionJob::Trig::URGE : VisionJob::Trig::AWAY,
                       now_wall, now_ms, acuity);
    }

    // r20 P3: reopen a kept image. No camera, no permission gate — her own
    // album observes no one — but its own smaller budget, and the same
    // one-deep queue and serialized drain as live sight.
    // r24.12 (WO-V2): a recall encodes at the working edge too — the two
    // 864x1080 portraits cost 918 tokens / 52 s each in S1 and deferred every
    // time; at edge 800 they are 500 tokens — unless the request was itself an
    // acuity one ("look closely at your portrait"), which opens the full file.
    bool commit_recall(time_t now_wall, double now_ms, const std::string &kept_file,
                       const std::string &gist, const std::string &when,
                       bool acuity = false, const VisionJob *request = nullptr) {
        if (!can_recall(kept_file)) return false;
        VisionJob j;
        j.trig        = VisionJob::Trig::RECALL;
        j.jpeg        = kept_file;
        j.asked_at    = now_wall;
        j.t0_ms       = now_ms;
        j.recall_gist = gist;
        j.recall_when = when;
        stamp_policy_(j, acuity);
        if (request) {
            // A queued member of a plural request retains the policy that
            // was selected for that request, even across later turns/config.
            j.acuity = request->acuity; j.enc_edge = request->enc_edge;
            j.crop = request->crop; j.crop_w = request->crop_w; j.crop_h = request->crop_h;
            j.wait_ms = request->wait_ms;
        }
        j.action.id=source_session+"/vision/"+std::to_string(seq_++);j.action.kind=aev::ActionKind::RECALL;j.action.target=kept_file;
        j.action.source = request ? request->action.source : input_source;
        j.action.request = request ? request->action.request : input_source.event.str();
        j.grant_epoch=perm.epoch;job_ = j;record_action(j,"source-open-requested",false,false,false);
        return true;
    }
    // r24.12 (WO-V3): a look she DECLARED holds her reply until the still
    // arrives — the drain waits the acuity budget for it instead of the 1-s
    // ordinary wait. Applied to the pending job by the self-look pivot right
    // after its commit; no-op with nothing pending.
    void hold_reply_for_pending(int wait_ms) {
        if (job_.active() && wait_ms > 0) job_.wait_ms = wait_ms;
    }

    // ── r20 P3: scene continuity, and the keep window ────────────────────────
    // scene_hash is the previous LOOK's dHash; recalls never touch it — a
    // memory is not the room. The held frame is the last successful look's
    // file, kept alive for five minutes so a keep can still reach it; it is
    // deleted when a newer look replaces it, when the window lapses, or at
    // teardown. One held frame, ever — the album is deliberate, not a buffer.
    uint64_t scene_hash  = 0;
    bool     scene_valid = false;
    // r24.6 (WO-37): the photometric half of the same "did the room change"
    // question. Kept beside the dHash and under the same rule — recalls never
    // touch it, because a memory is not the room.
    PhotoSig scene_photo;
    // r24.14 (WO-R3): the region half, under the identical rule — one signature
    // for the previous LOOK, never written by a recall, never persisted. 1,668
    // bytes on the rig; the only region state that outlives a look.
    RegionSig scene_region;
    // r19.8 (F3): 10 minutes, and REFRESHED whenever the image is mentioned.
    //
    // S8 missed the keep by TWO SECONDS: the frame was captured at 23:58:36,
    // the window ran from the capture, and the end-of-turn check landed at
    // 00:03:34 against an expiry of 00:03:36. Two long turns and a pivot had
    // eaten almost the whole budget. The window exists so a frame does not live
    // forever in /tmp — but a frame that is still being talked about is not
    // stale, so mention refreshes it, and the base is doubled.
    double   keep_window_ms = 600000;


    struct HeldFrame {
        std::string jpeg;
        time_t      taken_at = 0;
        double      t_ms     = 0;
        std::string source_id,keep_id;
        double      born_ms  = 0;   // R6-AV: when it was taken; bounds the refresh
        bool valid() const { return !jpeg.empty(); }
    };
    const HeldFrame &held() const { return held_; }
    std::string keep_identity() {
        if(held_.keep_id.empty())held_.keep_id=source_session+"/keep/"+std::to_string(seq_++);
        return held_.keep_id;
    }
    // A description may first arrive after an away look, with no live turn
    // flag. Conversely, later album discussion or a rolled-back image cannot
    // name the room merely because an old flag survived. The newest image
    // which actually remains in context owns the next description.
    bool live_description_pending() const {
        return !look_notes.empty() && !look_notes.back().recall &&
               !look_notes.back().described() && image_in_context(look_notes.back());
    }
    std::string held_description() const {
        if (!held_.valid()) return std::string();
        for (size_t i = look_notes.size(); i-- > 0; )
            if (!look_notes[i].recall && look_notes[i].opened == held_.jpeg)
                return look_notes[i].gist;
        return std::string();
    }
    bool held_fresh(double now_ms) const {
        return held_.valid() && (now_ms - held_.t_ms) <= keep_window_ms;
    }
    void hold_frame(const std::string &jpeg, time_t taken_at, double now_ms) {
        discard_held();
        held_.jpeg = jpeg; held_.taken_at = taken_at; held_.t_ms = now_ms;
        held_.source_id=last_completed_action.id.empty()?source_session+"/held/"+std::to_string(seq_++):last_completed_action.id;
        held_.born_ms = now_ms;                                     // R6-AV
        // ── r24.6 (WO-39 / review #40): a new photograph is not the one he
        // refused ─────────────────────────────────────────────────────────────
        // keep_refused_ stored only a timestamp and nothing ever cleared it, so
        // it stood for FORTY MINUTES of wall clock across arbitrarily many
        // later looks. The call-site comment at talk-llama.cpp says "she may
        // still declare a keep on any frame he has not refused, which is every
        // frame but this one" — a mechanism claim the code did not implement.
        // 100% of the latch's reachable effect fell on frames he never refused:
        // discard_held() runs one line BEFORE note_keep_refused() at its only
        // call site, so at the instant the latch is set the refused frame is
        // already unlinked and held_ is already cleared. Her one path with real
        // agency — a keep off her own words, no ask, no window — was silently
        // dead for the next forty minutes, with no log line and no
        // note_keep_missed().
        //
        // Clearing it HERE cannot reopen the R6-AU hole it was written for:
        // that hole was "the refused frame stays held and keep_declared at the
        // bottom of the same turn executes the keep he just refused", and
        // between the refusal and the next capture the latch still stands AND
        // held_fresh is false, so the gate is doubly closed across exactly the
        // interval R6-AU cared about. hold_frame runs only after a successful
        // new capture with a fresh unique filename.
        keep_refused_ = false; keep_refused_at_ms_ = 0.0;
    }
    // F3: the conversation is still about this image — the clock restarts. Only
    // ever extends a LIVE hold; a lapsed frame stays lapsed, so this can never
    // resurrect something already deleted.
    // ── r21-full.6 (R6-AV): the hold has a LIFETIME, not just a timeout ──────
    // touch_held is reached from both his text and hers whenever the turn
    // mentions keeping, saving, a picture, a photo, an image, an album, or
    // "looked at" — which is ordinary talk. Measured, "The album came out in
    // 1997." forty-five minutes later still had the frame alive and keepable,
    // and the header budgets this as "one frame living a few minutes longer in
    // /tmp". Refreshing is right — the conversation IS still about the image —
    // but it may not be unbounded: an un-kept photograph of him is not
    // something to keep alive all evening because the word "album" keeps coming
    // up. The extension stops at four times the window from the moment it was
    // taken, after which the ordinary lapse at the top of the loop takes it.
    void touch_held(double now_ms) {
        if (!held_.valid()) return;
        if (now_ms - held_.born_ms > 4.0 * keep_window_ms) return;
        held_.t_ms = now_ms;
    }

    // The keep executes: caller takes ownership of the file (moves it into
    // the album); the hold is released without deleting.
    HeldFrame take_held() {
        HeldFrame h = held_;
        held_ = HeldFrame();
        if (keep_frame_scope_on_()) keep_perm.clear();
        return h;
    }
    void discard_held() {
        if (held_.valid()) std::remove(held_.jpeg.c_str());
        held_ = HeldFrame();
        if (keep_frame_scope_on_()) keep_perm.clear();
    }

    // The drain site reports what happened; the budget is charged on success
    // only (a dead camera does not spend her eyes). Returns the closed job so
    // the caller can unlink the frame / build the failure line.
    VisionJob close_success() {
        VisionJob j = job_;
        if(!admission_allowed(j)){close_failure();return {};}
        last_completed_action=j.action;
        record_action(j,"admitted",j.trig!=VisionJob::Trig::RECALL,true,true);
        job_ = VisionJob();
        // ── r24.12 (WO-V12 / S22 §C.5.9): a same-file re-open is not re-charged
        // while its embeddings survive. S1 18:47:41 spent recall 3/6 on a
        // forced repeat of the image that had landed at 18:44:21 (the plural's
        // second was dropped, §C.5.5) — the picture was still in her context.
        // The cost of a recall is real (52 s, ~100 positions) but the BUDGET
        // is about how many kept images she may open in a night, and a file
        // whose picture is already in front of her is not another opening.
        // Charged again once a cut has happened since it landed (the picture
        // is gone then). ATHENA_RECALL_SAME_FILE=0 restores r24.11: every
        // recall charges. Self-initiated recalls are NOT exempt — the cost is
        // the same whoever asked (plan §E.6).
        if (j.trig == VisionJob::Trig::RECALL) {
            if (!(recall_same_file_on_() && recall_in_context(j.jpeg)))
                recalls_used++;                                  // the album's own budget
            else
                last_recall_free_ = true;
        }
        else if (j.trig != VisionJob::Trig::NONE)   looks_used++;    // every real look spends
        // r24.6 (WO-75): drop the slot with the job. A caller that needs the
        // slot past the close (the drain does — it releases the frame into the
        // hold) keeps its own by-value copies; the RIG's references end with
        // the job so no later look can ever read a stale slot.
        enc_slot.reset(); frame_owner.reset(); enc_defers = 0;
        return j;
    }
    // r24.12 (WO-V12): did the last close_success leave the album budget
    // untouched (a re-open of a picture still in context)? Consumed by the
    // drain's log line, once.
    bool take_recall_free() { const bool f = last_recall_free_; last_recall_free_ = false; return f; }
    VisionJob close_failure() {
        VisionJob j = job_;
        record_action(j,j.trig!=VisionJob::Trig::RECALL&&j.grant_epoch!=perm.epoch?"revoked":"failed-or-cancelled",false,false,false);
        if (encode_abandon_skip_on_() && enc_slot)
            enc_slot->cancelled.store(true, std::memory_order_release);
        // r24.17: defer-cap close can abandon a still-running camera, not only
        // an encoder. Cancel the writer through its ticket before dropping the
        // frame owner, so a closed look cannot later create an orphaned photo.
        if (capture_deadline_on_() && port && j.trig != VisionJob::Trig::NONE &&
            j.trig != VisionJob::Trig::RECALL && port->poll() == 0) port->abandon(j.jpeg);
        job_ = VisionJob();
        // r24.6 (WO-75): same rule. The FrameOwner unlinks the frame when the
        // last holder — possibly a worker still mid-encode — lets go, which is
        // what makes the defer-cap close safe (§17.2 item 1: never unlink a
        // frame the worker may not have opened yet).
        enc_slot.reset(); frame_owner.reset(); enc_defers = 0;
        // ── r24.12 (WO-V5 / S22 §C.5.5): a staged second image dies with the
        // FIRST's failure, and only there. The seam used to clear it on the
        // way out of the recall turn whatever happened, and a DEFER (every
        // keepsake encode exceeds the 1-s drain wait) is "whatever happened":
        // the plural path had delivered its second image in no session since
        // r24.6. Now the second outlives a defer — the drain's success tail
        // commits it when the first lands (commit_second_recall) — and is
        // cleared here when the first can never land. Under
        // ATHENA_RECALL_SECOND_DEFER=0 the seam clears it as before and this
        // line changes nothing that could still be armed.
        if (recall_second_defer_on_() && j.trig == VisionJob::Trig::RECALL) second_recall.clear();
        return j;
    }

    // trace fragment for the per-turn diagnostic line: "look=3/10 recall=1/3"
    std::string trace() const {
        return "look=" + std::to_string(looks_used) + "/" + std::to_string(look_max) +
               " recall=" + std::to_string(recalls_used) + "/" + std::to_string(recall_max);
    }

  private:
    HeldFrame held_;
    bool   keep_refused_       = false;   // R6-AU
    double keep_refused_at_ms_ = 0.0;
public:
    KeepPermission keep_perm;   // F2: her keep-ask window, mirroring LookPermission
    // ── r21-full.6 (R6-AU): a refusal he has spoken, remembered for as long as
    // the frame it was about could live. Her own keep-declaration path writes
    // the album with no ask and no window; without this it could execute a keep
    // he had explicitly refused moments earlier.
    void note_keep_refused(double now_ms) { keep_refused_at_ms_ = now_ms; keep_refused_ = true; }
    bool keep_refused_fresh(double now_ms) const {
        return keep_refused_ && (now_ms - keep_refused_at_ms_) <= 4.0 * keep_window_ms;
    }
private:

    // r24.12 (WO-V2): the ONE place the encode policy meets a job. An acuity
    // request selects the acuity size and the acuity wait; everything else is
    // the working edge with the ordinary wait (-1). Under ATHENA_ACUITY_LOOK=0
    // every job is a working-edge job — and with the working edge at 0 that is
    // r24.11 exactly: native pixels, the 1-s drain wait.
    void stamp_policy_(VisionJob &j, bool acuity) const {
        const bool ac = acuity && acuity_look_on_();
        j.acuity   = ac;
        j.enc_edge = ac ? acuity_edge : work_edge;
        j.crop     = ac && acuity_crop;
        if (j.crop) { j.crop_w = acuity_edge; j.crop_h = acuity_edge * 9 / 16; }
        j.wait_ms  = ac ? acuity_wait_ms : -1;
    }
    bool commit_(VisionJob::Trig trig, time_t now_wall, double now_ms, bool acuity = false) {
        if (!can_look()) return false;
        VisionJob j;
        j.trig     = trig;
        j.jpeg     = frame_dir + "/athena-look-" + std::to_string((long) now_wall) +
                     "-" + std::to_string(seq_++) + ".jpg";
        j.asked_at = now_wall;
        j.t0_ms    = now_ms;
        stamp_policy_(j, acuity);                          // r24.12 (WO-V2)
        j.action.id=source_session+"/vision/"+std::to_string(seq_++);j.action.kind=aev::ActionKind::CAPTURE;j.action.target=j.jpeg;j.action.source=input_source;j.action.request=trig==VisionJob::Trig::INVITED?input_source.event.str():std::string();
        j.grant_epoch=perm.epoch;
        record_action(j,"requested",false,false,false);record_action(j,"authorized",false,false,false);
        if (!port->start_capture(j.jpeg)){record_action(j,"capture-start-failed",false,false,false);return false;}
        job_ = j;record_action(j,"capturing",false,false,false);
        return true;
    }
    VisionJob job_;
    int       seq_ = 0;
    long      ctx_gen_ = 0;                                // r24.12 (WO-V12)
    long      ctx_revision_ = 0;                           // r24.18: context-side clause ownership
    long      snap_epoch_ = 0;                             // r24.20 review (VISION #5): note_snapshot() count
    mutable long clause_epoch_ = -1;                       // r24.20 review (VISION #5): epoch the stale clause entered in
    size_t    snap_notes_ = 0;                             // r24.12 (WO-V12): look_notes.size() at the last snapshot
    bool      last_recall_free_ = false;                   // r24.12 (WO-V12): the last close charged nothing

public:
    // ── r21-full.10 (F7): the second half of a plural recall ─────────────────
    // "Pull them BOTH up" cannot be honoured by a one-deep job queue in one
    // commit. The caller stages the second image here; the pre-reply drain
    // commits and drains it back-to-back after the first, so both images are
    // in front of her for the SAME reply — each charged against the recall
    // budget like any other recall. S14's confabulated "seeing them side by
    // side" happened precisely because she was asked about two images and
    // handed one (the wrong one): the plural path exists so the honest answer
    // is possible.
    struct SecondRecall {
        std::string file, gist, when;
        bool        armed = false;
        VisionJob   request;
        bool        request_captured = false;
        void clear() { *this = SecondRecall{}; }
    } second_recall;
    bool stage_second_recall(const std::string &file, const std::string &gist,
                             const std::string &when) {
        if (job_.trig != VisionJob::Trig::RECALL) return false;
        second_recall.file = file; second_recall.gist = gist; second_recall.when = when;
        second_recall.request = job_;
        second_recall.request_captured = true;
        second_recall.armed = true;
        return true;
    }
    // ── r24.12 (WO-V5 / S22 §C.5.5): the second image is committed where the
    // FIRST lands — the drain's success tail — so the seam, the idle seam and
    // the pivot seam all honour it, and a deferred first image no longer
    // discards the second. Commits the staged second as an ordinary recall
    // (budget checked inside commit_recall; a refused commit clears the
    // staging honestly — she has the first image and never a phantom second)
    // and clears the staging either way. False when nothing was staged or
    // the commit was refused. Under ATHENA_RECALL_SECOND_DEFER=0 the caller
    // never asks (the r24.11 seam commits it itself, and only on drained_ok).
    bool commit_second_recall(time_t now_wall, double now_ms) {
        if (!second_recall.armed) return false;
        const SecondRecall s = second_recall;
        second_recall.clear();
        return commit_recall(now_wall, now_ms, s.file, s.gist, s.when,
                             s.request.acuity, s.request_captured ? &s.request : nullptr);
    }
};

// ── the context envelope ────────────────────────────────────────────────────
// The text that carries the image into her context. `marker` is
// mtmd_default_marker() at the call site ("<__media__>"); tests pass a stub.
// One marker, exactly once — mtmd_tokenize replaces it with the image chunk.
// The clock phrase keeps the look anchored to the moment of CAPTURE, which may
// be several seconds before the eval lands (the honest capture→comprehension
// gap, proposal §2).
// P2: the envelope names WHO initiated — an invited look, her own permitted
// wish, or a look taken under the away-grant while the room was empty. The
// register stays factual; her feelings about what she sees are hers to have.
// ── r21-full.5 (R5-S): these four envelopes name her, and they name a number ─
// Both were hard literals. `--bot-name` defaults to "LLaMA" and is settable, so
// three of the four announced a name the rest of the session did not use; and
// `--look-max` / ATHENA_LOOK_MAX is a first-class knob clamped to 0..64, so at
// any setting but ten the spent-eyes line instructed her to state a false fact
// about her own body — inside the one subsystem whose sibling file exists to
// keep her attribution honest. Defaults preserve the exact previous strings.
inline std::string look_envelope(time_t asked_at, const std::string &marker,
                                 VisionJob::Trig trig = VisionJob::Trig::INVITED,
                                 const std::string &bot = "Athena") {
    char hm[16] = {0};
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &asked_at);
#else
    localtime_r(&asked_at, &tmv);
#endif
    std::strftime(hm, sizeof(hm), "%H:%M", &tmv);
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    const std::string head =
        trig == VisionJob::Trig::URGE
            ? "\n[" + who + " looks through the camera — her own wish, permitted — one still frame, taken at "
        : trig == VisionJob::Trig::AWAY
            ? "\n[While he was away, " + who + " looked around — one still frame, taken at "
            : "\n[" + who + " looks through the camera — one still frame, taken at ";
    return head + hm + "] " + marker + "\n";
}

// r20 P3: the remember-envelope — a kept image reopened. The when and the
// gist frame the image so she meets it as a memory of hers, not a fresh
// capture; her CURRENT state does the rest (the same room read differently on
// a different night is the point, not a bug).
// ── r24.6 (WO-38 / S19 D5): the caption is PROVENANCE, not perception ───────
//
// 14:32:32 — "[Athena remembers — a kept image from about a week ago: Okay, I
// looked. The room's empty now—just the ceiling, some boxes stacked up...]"
// and her answer: "The first one—the empty room—it feels like a pause... It's
// just the ceiling, the boxes, the quiet." The caption's nouns, in the
// caption's order, minus the dresser.
//
// Brightened, ks-1786936610.jpg shows A BARE SHOULDER AND ARM at the
// bottom-right. This file's own S16 comment documents the original error
// (the S16 case: "camera at the ceiling, 'the room's empty now' with his
// shoulder in frame"). That false caption was kept, re-rendered into the
// permanent prompt prefix at every startup with salience 10, and handed back to
// her seven days later AS THE FRAME FOR THE IMAGE — head position, colon, then
// the picture. She recited it.
//
// Two changes, and neither removes anything:
//   * THE IMAGE LEADS. The marker moves ahead of the caption, so the pixels are
//     what she meets first and the words are a gloss on them. The old order put
//     a week-old sentence between her and the photograph.
//   * THE CAPTION IS ATTRIBUTED. "what I wrote about it then: ..." is a claim
//     about a past authorship, and a past authorship cannot be read as a
//     present description. It is also true, which the old bare colon was not.
inline std::string recall_envelope(const std::string &when, const std::string &gist,
                                   const std::string &marker,
                                   const std::string &bot = "Athena") {
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    std::string head = "\n[" + who + " remembers — a kept image";
    if (!when.empty()) head += " from " + when;
    head += "]\n" + marker + "\n";
    if (!gist.empty())
        head += "[what " + who + " wrote about it then: \"" + gist + "\"]\n";
    return head;
}

// What enters the context when the camera failed her. Access-true: she tried,
// the device gave nothing, and her reply should be allowed to say so.
inline std::string look_failed_line(const std::string &bot = "Athena") {
    return "\n[" + (bot.empty() ? std::string("Athena") : bot) +
           " tried to look, but the camera returned nothing.]\n";
}

// ── r24.6 (WO-75): the third arm beside envelope/failure ────────────────────
// A deferred look must say something TRUE. "Let me take a look." is spoken
// BEFORE the drain; on a defer she has promised a look with nothing in
// context — the S19 D6 shape exactly — and the failure line ("the camera
// returned nothing") would be false about a merely slow encode. This line is
// S14-descriptive: it states what is so — the picture exists and has not
// reached her yet — and instructs nothing.
// r24.17: capture and comprehension are different completed acts. The drain
// reads the camera's acquired verdict, never the encoder's unpublished fields.
// ATHENA_LOOK_CAPTURE_STATE=0 restores the completed-capture claim at every defer.
inline bool look_capture_state_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_LOOK_CAPTURE_STATE");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline std::string look_pending_line(const std::string &bot = "Athena", bool captured = true) {
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    if (look_capture_state_on_() && !captured)
        return "\n[" + who + " has asked the camera for a picture, but it has not "
               "returned one yet. Nothing from this look has reached her eyes.]\n";
    return "\n[" + who + " has taken the picture, but it has not reached her "
           "eyes yet — it is still on its way. What it holds is not known to "
           "her.]\n";
}

// r24.6 (WO-75): the honest close after the defer bound. The look truly ends
// here — pending() gates compaction and can_look(), so a look that can never
// land must not stay pending forever — but the camera DID return a frame, so
// the camera-failure line would be a false statement. This one is true.
inline std::string look_never_arrived_line(const std::string &bot = "Athena", bool captured = true) {
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    if (look_capture_state_on_() && !captured)
        return "\n[" + who + " tried to look, but no picture had returned when "
               "the attempt ended — she never saw it.]\n";
    return "\n[" + who + " took a picture, but it never finished arriving — "
           "she never saw it.]\n";
}

// ── r24.20 review (VISION #7): the close for a look his "no" withdrew ────────
//
// What was wrong. Since r24.18 (ATHENA_VISION_INTAKE_SCOPE) a revocation drops
// a pending non-recall look through athena_vision_drop_failed_look. The drain's
// CLOSE_DEFER_CAP pairs its close_failure() with look_never_arrived_line, and
// the two pre-decode-failure sites cannot decode anything; the DENY site CAN —
// the context is healthy — but only the Mind got a note. So a 1080p acuity
// look deferred at the seam had decoded "[Athena has taken the picture, but it
// has not reached her eyes yet — it is still on its way.]", he said "don't
// look at me" the next turn, the job was dropped, and her context promised a
// picture that never lands with nothing saying it was withdrawn. The model can
// and will "see" it. The close is rendered only when the pending line IS in
// context (the drain decodes it on the first defer: enc_defers > 0), in the
// arm the pending line took. S14-descriptive, no numeral (F18).
// ATHENA_LOOK_WITHDRAWN_CLOSE=0 restores the silent drop.
inline bool look_withdrawn_close_on_() {
    static const bool on = []() {
        const char *e = ::getenv("ATHENA_LOOK_WITHDRAWN_CLOSE");
        return !(e && e[0] == '0');
    }();
    return on;
}
inline std::string look_withdrawn_line(const std::string &bot = "Athena", bool captured = true) {
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    if (look_capture_state_on_() && !captured)
        return "\n[" + who + " had asked the camera for a picture that had not yet "
               "returned. He asked her not to look, so she withdrew the request. "
               "Nothing from it reached her eyes.]\n";
    return "\n[" + who + " had taken a picture that had not yet reached her eyes. "
           "He asked her not to look, so she set it aside unseen. Nothing from it "
           "is in front of her.]\n";
}

// ── r24.12 (WO-V8 / S22 §C.5.9): the announce, priced ───────────────────────
// The seam's announce ("Let me take a look." / "Let me find it.") said nothing
// about duration; S1's four portrait recalls were 52 s of silence each. This
// chooses the line by the job's PREDICTED cost (predict_vision_s × the rig's
// learned scale) against ATHENA_VISION_ANNOUNCE_S: an acuity look always says
// it is reading (its wait is the acuity budget); a recall or a look predicted
// past the threshold says it will take a while; everything else is the r24.11
// line. threshold <= 0 (ATHENA_VISION_ANNOUNCE_S=0) is the r24.11 line always.
// TTS only — never context — so the words are hers to say and no F18 numeral
// is at stake; pure so the battery can pin every arm.
inline std::string vision_announce_line(bool recall, bool acuity, double predicted_s, double threshold_s) {
    if (!recall && acuity && threshold_s > 0.0) return "I'm reading it — one moment.";
    if (threshold_s > 0.0 && predicted_s > threshold_s)
        return recall ? "One moment — I'm opening the album, this one takes me a little while."
                      : "Give me a moment, I'm looking closely.";
    return recall ? "Let me find it." : "Let me take a look.";
}

// ── r24.7 (WO-84(1) / S20 D4): the WO-35 fields get their consumer ──────────
// S20 detected the 640x480 fallback 4/4 and she was never told — cap_w/h,
// fell_back(), coarse() had zero consumers, so her hedged descriptions were
// the model's own honesty, not policy. This is the clause: appended to the
// look's context right after the image lands (the drain knows the real size
// only then — the envelope itself is built before the camera answers, so the
// hedge cannot ride inside look_envelope()). S14-descriptive, no numerals,
// echoable in her voice; a pure function of the note so the battery can pin
// it (talk-llama.cpp is compiled by no test binary — the WO-75 discipline).
//
// EMPTY unless the note itself claims smallness: fell_back() and coarse()
// are both false when nothing was measured (measured() gates each), so
// verified=false — the FakePort, an unparseable JPEG — renders NO hedge.
// Absence of knowledge is not coarseness.
// ── r24.12 (WO-V2, Igor's decision): the --image-min-tokens arm is REACHABLE
// and ACUITY-GATED. At `--image-min-tokens 0` (every launcher so far) the
// coarse() arm never fired; at the acuity token count (2040) it would fire on
// EVERY working-edge look (350 tokens) — a hedge on an ordinary look that was
// never asked to read. With `acuity_gate` (ATHENA_ACUITY_LOOK on) the coarse
// arm fires only when the note says his words asked her to READ this one
// (acuity_asked) AND what mtmd was handed (seen_tokens — the working copy, by
// mtmd's own rounding) is below the floor: "asked to read, served small".
// The fell_back() arm — the camera itself substituted a smaller frame — is
// untouched: that is a different fact and WO-84's own case. acuity_gate=false
// is the r24.11 test byte for byte.
inline std::string capture_hedge_line(const VisionRig::LookNote &n, int min_tokens,
                                      const std::string &bot = "Athena",
                                      bool acuity_gate = false) {
    const bool coarse = acuity_gate ? (n.acuity_asked && n.coarse_seen(min_tokens))
                                    : n.coarse(min_tokens);
    if (!(n.fell_back() || coarse)) return "";
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    return "\n[The picture that reached " + who + " is small — fine detail "
           "may escape her.]\n";
}

// ── r24.8 (WO-100(b) / S19 D6): the OTHER thing the WO-84 hedge never said ──
// LookNote::age_words() was built for exactly this and has had no caller since
// r24.6, so nothing in the tree has ever contradicted a continuous-sight claim.
// S19 turn 29: "But looking at you now, you're just a person. Tired, holding a
// children's book, sitting in a dim room" — present tense, continuous, from a
// still that was NINETY SECONDS old, and contradicting her own turn-28 report
// of the same room. At 14:57:36 an inner thought asserted "The light shifts in
// the room when he speaks", twenty-seven minutes after her last still. The
// picture is in her context for as long as the embeddings survive a cut; what
// was missing is any statement of WHEN it was taken.
//
// THE THRESHOLD — 45 s, and it is not a new number. It is elapsed_words()'s own
// first rung boundary, the point at which her age vocabulary stops answering
// "a moment ago" and starts answering "about a minute ago". Below it the clause
// would hedge a frame by calling it a moment old, which agrees with the claim
// it exists to contradict; at and above it every word the ladder can return
// says the picture is not now. The floor is set by the seam, not by taste: a
// look commits, waits out the camera (2.5 s of drain grace, fswebcam's own
// -D 1 behind that), encodes and lands inside one turn, so a still that has
// reached 45 s has outlived the exchange it was taken for — while 45 leaves
// twice the margin needed to catch S19 turn 29 at 90 s. Nothing in S20 argues
// for longer: all four of its fallbacks were described within a turn of their
// look. A parameter, not a constant fixed at the call site, so the battery can
// drive both sides of the boundary.
//
// S14-descriptive and F18-clean: it states where her eyes last were and stops,
// and every age it can speak is a word. A RECALL is refused outright — an album
// page has an age too, and it is not the age of anything her eyes are seeing.
// ── r24.8 (WO-101): the drain's capture read, as one function ───────────────
// The r24.7 drain asked the port two questions (last_dims + want_dims) and
// could not ask the third — `verified` and the requested half live only on the
// CaptureInfo the async worker publishes on the same per-capture slot as the
// verdict flag, and nothing read it. This asks ONE question and keeps the pair
// as the fallback arm: a port that does not implement last_capture() (the
// battery's FakePort, and every port in a tree that never adopted it) answers
// with an unverified record, and the pair then reads exactly what it read in
// r24.7. `use_record=false` is ATHENA_CAPTURE_RECORD=0 and is the r24.7 path to
// the byte, log line included.
//
// Extracted rather than written at the call site because talk-llama.cpp is
// compiled by no test binary — the WO-75 discipline, and the same reason
// capture_hedge_line() is a free function.
// ── r24.8 (WO-111): what this record does NOT carry, and why ────────────────
// WO-101 shipped a `bool verified` here, assigned on both arms and read by no
// production consumer — the drain used got_*/req_*/log, and note_look_dims()
// re-derived the same verdict from the dims. A field written everywhere and
// read nowhere is the census's own inverse class, introduced by the round that
// was closing it, so it had to go one way or the other: give it a production
// reader, or stop carrying it.
//
// It is gone, because the reader would have been decorative. The two candidate
// answers were never two sources of truth about one fact — the flag WAS the
// dims test, provably, from this function's own body and without reference to
// any port:
//
//   * the record arm is entered only under `if (ci.verified)`, and
//     inspect_capture() sets that flag solely from jpeg_sof_dims(), which
//     returns true only when `wid > 0 && hgt > 0`. So the record arm always
//     leaves got_w/got_h positive;
//   * the fallback arm assigned `r.verified = (r.got_w > 0 && r.got_h > 0)`
//     verbatim — the inference, spelled out.
//
// So it equalled `got_w > 0 && got_h > 0` for every input at every port,
// including the ones that answer no record at all (the battery's FakePort, any
// tree that never adopted last_capture()). Passing it into note_look_dims()
// would have added a parameter that could not change an outcome: wiring that
// reads as live and is not, which is the exact shape the r24.8 census exists to
// find. Keeping it as an accessor was tried and REFUSED BY THE AUDIT — as a
// member function it became a producer whose only callers were fixtures, which
// is the S19 trap the wiring gate names in its own header ("a test caller does
// not count -- that is exactly the trap"). A dark field turned into a dark
// function is not a repair.
//
// The question keeps its name where it is actually asked: LookNote::measured(),
// on the note that the frame hedges from. This struct carries what the port
// SAID and nothing derived from it. `from_record` stays, and is not the same
// case: it is not derivable from the dims — it answers "which arm spoke", which
// is this function's own control flow and the reason ATHENA_CAPTURE_RECORD=0
// can restore the r24.7 path exactly.
struct CaptureRead {
    // 0x0 is "unknown" and no consumer may read unknown as small — WO-84's
    // rule, in this file. That IS the verification claim; there is no second
    // spelling of it here.
    int  got_w = 0, got_h = 0;
    int  req_w = 0, req_h = 0;
    bool from_record = false;   // answered by last_capture(), not by the pair
    std::string log;            // CaptureInfo::fallback_line(), or empty
    // r24.12 (WO-V1): CaptureInfo::via_line(), or empty — the negotiated
    // format, logged at the drain beside `this look's …`. Empty for every port
    // that never recorded `via` and under ATHENA_CAPTURE_MJPEG=0, so nothing
    // new is printed on the r24.11 path.
    std::string via_log;
};
inline CaptureRead read_capture(const VisionPort *port, bool use_record) {
    CaptureRead r;
    if (!port) return r;
    if (use_record) {
        const CaptureInfo ci = port->last_capture();
        if (ci.verified) {
            r.got_w = ci.w;     r.got_h = ci.h;
            r.req_w = ci.req_w; r.req_h = ci.req_h;
            r.from_record = true;
        }
        // fell_back is only ever set on a verified record, so this is empty
        // whenever the record made no claim — silence, never a guess.
        r.log = ci.fallback_line();
        if (ci.verified) r.via_log = ci.via_line();   // r24.12 (WO-V1)
    }
    if (!r.from_record) {
        port->last_dims(&r.got_w, &r.got_h);
        port->want_dims(&r.req_w, &r.req_h);
    }
    return r;
}

inline constexpr long STALE_LOOK_S = 45;

// ── r24.8 (WO-109): the question, hoisted out of the clause that asks it ────
// The age hedge below was not the only reader of "is the still she is holding
// still now?". WO-99's "I looked just now" is a claim about the SAME still, and
// the C36 restore row can hand that note back across up to three turns — the
// busy-opening case its own comment cites as the reason the row exists — so one
// ninety-second-old frame could compose both at once, in one context: "I looked
// just now" and "the last thing her eyes took in was a still, about a minute
// ago". A probe printed exactly that pair, side by side, from one note.
//
// The note is the half that yields. The age clause carries the true fact and is
// the S19-turn-29 defence — turn 29 asserted present-tense sight from a
// ninety-second frame, which is the whole reason WO-100 exists — so a guard
// that silenced the age clause would delete the contradiction and keep the
// contradicted claim.
//
// It is hoisted HERE rather than re-spelled at the note's read for the reason
// WO-101 has just finished proving: two spellings of one threshold drift. This
// is the only comparison against STALE_LOOK_S in the tree, both readers call
// it, and acon never learns the number at all — the FACT crosses into the
// frame (acon::FieldOptions::still_stale), never the seconds. The frame's
// reader cannot see this header, and a copy of 45 into acon is precisely the
// duplication WO-101 removed; a boolean cannot drift from its own source.
//
// What it does NOT ask: whether a look is pending, and whether the clause has
// already been said this rung. Those are the age CLAUSE's rules about when to
// speak (a frame is landing; the same sentence forty times), not facts about
// the still. The note yields because it is FALSE — "just now" is not true of a
// frame that has outlived the exchange it was taken for — and it is equally
// false on the turns when the clause happens to be silent.
inline bool still_is_stale(const VisionRig::LookNote &n, long now_wall,
                           long older_than_s = STALE_LOOK_S) {
    if (n.recall) return false;                    // an album page is not a camera frame
    if (n.when <= 0 || now_wall <= 0) return false; // unknown age asserts nothing
    return n.age_s(now_wall) >= older_than_s;
}
// The same question of the rig: `last_fresh_look()`, never `look_notes.back()`,
// so a recall she opened after her last look cannot answer for the camera.
// FALSE when she has never looked — nothing has aged, and nothing claims to.
inline bool still_is_stale(const VisionRig &rig, long now_wall,
                           long older_than_s = STALE_LOOK_S) {
    const VisionRig::LookNote *n = rig.last_fresh_look();
    return n && still_is_stale(*n, now_wall, older_than_s);
}

inline std::string stale_look_hedge_line(const VisionRig::LookNote &n, long now_wall,
                                         long older_than_s = STALE_LOOK_S,
                                         const std::string &bot = "Athena") {
    // r24.8 (WO-109): the recall test, the unknown-age test and the comparison
    // this clause used to spell inline are one call now, so the note's read and
    // this clause can never disagree about which stills are stale.
    if (!still_is_stale(n, now_wall, older_than_s)) return "";
    const std::string w = n.age_words(now_wall);
    if (w.empty()) return "";
    const std::string who = bot.empty() ? std::string("Athena") : bot;
    return "\n[The last thing " + who + "'s eyes took in was a still, " + w +
           ". Nothing has reached them since.]\n";
}

// The turn's whole decision, so the call site is two lines and the battery can
// drive every arm of it. Returns the clause to enter, or "" for silence.
//
//  * a look already in flight says nothing — a frame is about to land and the
//    clause would be false before she read it;
//  * `last_fresh_look()` and not `look_notes.back()`, so an album recall is
//    never described as what her eyes are seeing;
//  * ONCE PER RUNG. The age is a standing fact, not news: the embeddings sit in
//    her context until a cut, so a clause entered on every turn would be the
//    same sentence forty times. `said` is the caller's cross-turn memo, keyed
//    by the still's capture time as well as its words — a NEW look that ages
//    into a rung its predecessor already used says it again, because that is a
//    different still and a different fact.
inline std::string stale_look_clause(const VisionRig &rig, bool look_pending,
                                     long now_wall, const std::string &bot,
                                     std::string *said,
                                     long older_than_s = STALE_LOOK_S) {
    const bool source_owned = look_source_ownership_on_();
    if (look_pending && !(source_owned && rig.pending() &&
                          rig.job().trig == VisionJob::Trig::RECALL)) return "";
    const VisionRig::LookNote *n = rig.last_fresh_look();
    if (!n) return "";
    std::string line;
    if (source_owned) {
        const bool present = rig.image_in_context(*n);
        if (present && !still_is_stale(*n, now_wall, older_than_s)) return "";
        const std::string who = bot.empty() ? std::string("Athena") : bot;
        // Keep the established envelope opener: FieldEchoGuard recognizes it
        // before a model echo can become ordinary speech.
        line = "\n[The picture that reached " + who + " through the camera";
        const std::string age = n->age_words(now_wall);
        if (!age.empty()) line += " was requested " + age;
        else line += " has no recorded request time";
        line += present ? ". Its image remains in her current context."
                        : ". Its image is no longer in her current context.";
        if (!present && n->described())
            line += " Her words about it remain in this session's visual record.";
        line += " The camera supplies separate still frames.]\n";
    } else {
        line = stale_look_hedge_line(*n, now_wall, older_than_s, bot);
    }
    if (line.empty()) return "";
    // The memo lives outside the LLM context. A cut can remove the very
    // clause it remembers, while leaving its age rung unchanged. A rollback
    // can remove this image without changing the global generation. Both
    // existing receipts therefore contribute to this context-only memo key.
    const std::string key = std::to_string((long long) n->when) +
        (source_owned ? ":" + std::to_string(rig.context_revision()) + ":" +
                        std::to_string(n->ctx_gen) + ":" + n->opened : std::string()) + line;
    if (said) {
        if (key == *said) return "";
        *said = key;
        // r24.20 review (VISION #5): the rig-side half of the memo — which
        // snapshot epoch this clause entered in, so a rollback bumps the
        // revision only when it rolls that epoch back (see note_rollback).
        if (VisionRig::stale_clause_epoch_on_()) rig.note_stale_clause_entered();
    }
    return line;
}

// ── r24.8 (WO-113): eyes_spent_line MOVED to athena_seam.h ──────────────────
// It was here, and it was a live F18 violation at shipped defaults: its private
// word[] table stopped at "twelve", so `--look-max 15` printed a bare 15 into
// her context, and `recalls_left` went through std::to_string unconditionally —
// a bare numeral at EVERY value, including the default 3. F18/S18-18 says no
// numeral she did not hear may reach her, and this line is tokenised straight
// into her context at the one moment she is told her eyes are spent.
//
// The fix needed acon::spell_number (0-99, one spelling of every word in the
// build — see WO-110), and this header is a leaf: it includes no athena header
// and cannot see acon. The function needs no VisionRig — it is a pure
// (bot, look_max, recalls_left) -> string — so it moved to athena_seam.h,
// which already includes athena_consciousness.h. Both call sites
// (talk-llama.cpp's spent-budget injection, and the two fixtures) already
// include athena_seam.h, so nothing else moved.

} // namespace aseam
