// athena_inner_worker.h — r21-full.1 (Fix 1): the inner voice's worker thread.
//
// WHY THIS FILE EXISTS. The r21 inner voice originally decoded the LLM
// cooperatively from the endpointer's on_listen hook — a hook whose contract is
// "must not block" (silero-endpointer.h). A 397B prompt prefill takes hundreds
// of milliseconds to seconds, and because audio.get(poll_ms) returns only the
// most-recent window of the capture ring, any stall longer than one poll drops
// audio from the VAD feed: speech onset detected late, speech/silence accounting
// corrupted, mistimed endpoint. The adversarial review (F1) rated this the one
// real-time-critical defect in r21. This file is the fix: the decode moves to a
// dedicated worker thread, and the pump on the audio path becomes a NON-BLOCKING
// mailbox — post a seed, poll a flag, drain a result. The audio loop's timing is
// then independent of model speed by construction, which is what makes shipping
// ATHENA_INNER_VOICE on-by-default defensible.
//
// WHY IT IS A SEPARATE HEADER. The threading/mailbox logic is exactly the part
// a latency regression would hide in, so it must be testable without a model.
// The LLM specifics are injected as a functor (`InnerGenFn`); talk-llama.cpp
// passes the real llama decode loop, and test_inner_worker.cpp passes a stub
// whose "decode" sleeps — letting the battery assert, with wall-clock numbers,
// that a slow generator can no longer stall the simulated audio loop. (The
// remaining on-hardware step is measuring the real 397B numbers on the target
// rig; the harness pins the DESIGN property — the pump never blocks — which is
// the part that holds regardless of hardware.)
//
// CONTRACT.
//   * post()/busy()/take_done()/permit() are called from the audio thread and
//     are non-blocking: they take a mutex that the worker only ever holds for
//     mailbox reads/writes, never across a decode.
//   * The generation functor must call keep_going() at least once per model
//     decode and abort promptly when it returns false. keep_going()'s fast
//     path is two relaxed atomic loads; while suspend() is in force it BLOCKS
//     (r21-full.9, R9-A) — so it must only ever be consulted while holding no
//     lock the audio path or the reply decode needs. generate_ satisfies this:
//     it consults keep_going strictly between guarded decodes.
//   * permit(false) — the seam's "he spoke" / "a real turn began" signal —
//     aborts the in-flight generation at its next keep_going() check, drops any
//     queued request, and DISCARDS an undrained finished result. That matches
//     the r21 cooperative pump's boundary semantics exactly: a rumination that
//     had not been ingested by the time he spoke was abandoned.
//   * Results are published only if the generation COMPLETED while permitted,
//     and are drained exactly once.
//   * stop() (also run by the destructor) aborts any in-flight work and joins.
//
// Thread-safety of the functor's innards is the caller's business: in
// production the functor serialises every llama_decode against the main
// context's decodes with the seam's shared decode mutex, so an inner decode and
// a reply decode can never overlap on the GPU backend.
#pragma once

#include <atomic>
#include <cctype>                 // r24.12 (WO-I2): join_carry_
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace athena {

// Runs ONE full generation for `prompt`. Must call keep_going() frequently (at
// least once per model decode) and abort promptly when it returns false.
// Returns true iff the generation COMPLETED and `out` holds the finished text;
// false = aborted or failed (out is discarded by the worker).
using InnerGenFn = std::function<bool(const std::string &prompt, bool dream,
                                      const std::function<bool()> &keep_going,
                                      std::string &out)>;

class InnerWorker {
public:
    explicit InnerWorker(bool cancel_epoch = true) : cancel_epoch_(cancel_epoch) {}
    ~InnerWorker() { stop(); }
    InnerWorker(const InnerWorker &) = delete;
    InnerWorker &operator=(const InnerWorker &) = delete;

    // Spawn the worker. Call once; `fn` owns the model specifics.
    void start(InnerGenFn fn) {
        std::lock_guard<std::mutex> lk(mx_);
        if (running_) return;
        fn_      = std::move(fn);
        quit_.store(false, std::memory_order_relaxed);
        running_ = true;
        th_ = std::thread([this] { loop_(); });
    }

    // Abort in-flight work and join. Safe to call repeatedly / if never started.
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (!running_) return;
            quit_.store(true, std::memory_order_relaxed);
            has_req_ = false;
            has_done_ = false;
        }
        cv_.notify_all();
        pause_cv_.notify_all();                                    // R9-A
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> lk(mx_);
        running_ = false;
    }

    // The phase gate. true = the room is quiet (WaitingForSpeech); decoding is
    // allowed. false = a real turn began: abort the in-flight generation, drop
    // any queued request, discard any undrained result.
    void permit(bool allowed) {
        {
            // R9-A: the pause predicate is evaluated under mx_; every flip the
            // predicate reads happens under the same lock, or a store between
            // the waiter's predicate check and its sleep is a lost wakeup.
            std::lock_guard<std::mutex> lk(mx_);
            permit_.store(allowed, std::memory_order_relaxed);
            if (!allowed) {
                cancel_locked_();
            } else {
                // A quiet room is the resume signal: the main loop is back in
                // its listening wait, her reply decode is finished, and a
                // parked thought may pick up where it stopped.
                paused_.store(false, std::memory_order_relaxed);
            }
        }
        pause_cv_.notify_all();
    }

    // r24.18: a completed thought can be waiting in the mailbox when the next
    // audio poll observes his speech. permit(false) used to erase that already
    // finished product as well as unfinished work. Take only the product that
    // was PUBLISHED before this cancellation, atomically with closing the gate.
    // A generation still returning from the model cannot slip through here;
    // its epoch is cancelled. The caller must retain the original request ID
    // and run the ordinary product gates before claiming any consequence.
    // ATHENA_INNER_FINISHED_RETENTION=0 leaves the live pump on permit(false);
    // ATHENA_INNER_FINISHED_EXIT=0 independently keeps the old teardown. Partial
    // output and interrupted generation stay lost under the existing epoch gate.
    bool interrupt_and_take_done(std::string &out, bool &dream) {
        bool ready = false;
        {
            std::lock_guard<std::mutex> lk(mx_);
            permit_.store(false, std::memory_order_relaxed);
            ready = has_done_;
            if (ready) {
                out = std::move(done_text_);
                dream = done_dream_;
            }
            cancel_locked_();
        }
        pause_cv_.notify_all();
        return ready;
    }

    // ── r21-full.9 (R9-A): her own turn PARKS the thought; it does not die ───
    //
    // R5-Z introduced suspend() with the words "a thought interrupted by her
    // own turn is not a thought that had to be destroyed" — and then
    // implemented it as permit_=false, which aborts the in-flight generation
    // at its next keep_going(). The request had already been consumed when the
    // generation started, so the seed was gone: the thought WAS destroyed,
    // just quietly. Measured over the S13 session, that made the inner voice
    // structurally mute in ordinary conversation: 20 true quiet windows, 17 of
    // them ended by HER taking the floor, none longer than 29 s — against a
    // ~10 s contended generation, every attempt was killed at the boundary and
    // the ledger recorded 29 armed requests and zero delivered ruminations.
    //
    // suspend() now sets `paused_`. The generation PARKS at its next
    // keep_going(): the worker blocks on a condition variable, holding no GPU
    // mutex (generate_ only consults keep_going between guarded decodes), so
    // her reply decodes with zero contention — the same GPU guarantee R5-Z/R6-H
    // priced at 83% turn inflation. When the room is quiet again, permit(true)
    // unparks it and the SAME generation continues from the token it stopped
    // at. A thought now accumulates across the short silences of a lively
    // conversation instead of restarting from nothing in each one.
    //
    // permit(false) — HIS speech — still aborts, wakes a parked generation so
    // it aborts promptly, and clears the mailbox: a turn of his changes the
    // ground the thought stood on, which is the r21 contract unchanged.
    void suspend() { paused_.store(true, std::memory_order_relaxed); }
    void resume()  {
        {
            std::lock_guard<std::mutex> lk(mx_);
            paused_.store(false, std::memory_order_relaxed);
        }
        pause_cv_.notify_all();
    }
    bool permitted() const { return permit_.load(std::memory_order_relaxed); }
    bool paused()    const { return paused_.load(std::memory_order_relaxed); }

    // Queue a generation. Non-blocking. false if the worker is already busy
    // (request queued, generating, or a result awaiting drain) or not running.
    //
    // r21-full.10 (F1): `carry_prefix` is text that is logically PART of the
    // generation but was prefilled inside the prompt — the first-person opener
    // the frame ends on. The model only ever continues it, so the finished
    // thought is prefix+continuation; the worker prepends it at publish time,
    // and take_done() hands back the whole thought. Callers that prefill
    // nothing pass nothing (the default keeps every existing call site and
    // test source-compatible).
    bool post(const std::string &prompt, bool dream, const std::string &carry_prefix = "") {
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (!running_ || has_req_ || generating_ || has_done_) return false;
            req_prompt_ = prompt;
            req_dream_  = dream;
            req_prefix_ = carry_prefix;
            has_req_    = true;
        }
        cv_.notify_all();
        return true;
    }

    // A request queued, a generation running, or a result pending drain.
    bool busy() const {
        std::lock_guard<std::mutex> lk(mx_);
        return has_req_ || generating_ || has_done_;
    }

    // Drain a finished rumination. Non-blocking; true at most once per post.
    bool take_done(std::string &out, bool &dream) {
        std::lock_guard<std::mutex> lk(mx_);
        if (!has_done_) return false;
        out   = std::move(done_text_);
        dream = done_dream_;
        has_done_ = false;
        return true;
    }

    // ── r24.12 (WO-I2 / S22 §C.3.1): reunite the carried prefix and the
    // continuation. The frame no longer ends on the opener's trailing space
    // (acon::opener_for_prompt — a lone-space token is a digit's context), so
    // the prefix ends on a WORD and the model's first token carries its own
    // leading space (" flipped", " 30", ","). Exactly one space is inserted
    // where two alphanumerics would otherwise touch — the rare no-space
    // continuation — and nothing else is touched. Under the r24.11 prefix
    // (trailing space) this is `prefix + out` byte-for-byte, so the
    // ATHENA_INNER_OPENER_NOSPACE=0 path and every existing caller are
    // unchanged. Pure and public so the fixture can pin it.
    // ── r24.12 (review): the belt did not fire after a comma ────────────────
    // What was wrong: the left-hand test was `isalnum(prefix.back())`, so a
    // prefix ending in punctuation was joined raw. Three of acon::kSelfRefLead's
    // eight openers end in a comma ("Right now,", "Underneath the words,",
    // "Here, now,") and two of kInnerWake's twelve do ("If I'm honest,", "The
    // thing is,") — after opener_for_prompt trims the trailing space, WO-I2's
    // own openers are exactly the case this belt is for, and
    // join_carry_("Here, now,", "the light is even.") returned
    // "Here, now,the light is even." — the de-spacing WO-I2 exists to prevent,
    // produced by its own belt. Proved by the fixture's new punctuation-PREFIX
    // pins (the shipped pins were all punctuation-CONTINUATION).
    // What this does: the left-hand test becomes "the prefix does not already
    // end in a space", which is the question actually being asked. Every
    // shipped pin is unchanged — ("If I'm honest, ", "a finished thought")
    // still inserts nothing (the prefix ends in a space); ("There is a",
    // " 30% chance…") and ("If I'm honest, what I", "'m after") still insert
    // nothing (out.front() is not alnum). Nothing restores here: under
    // ATHENA_INNER_OPENER_NOSPACE=0 the prefix keeps its trailing space and
    // this is `prefix + out` byte-for-byte, exactly as before.
    static std::string join_carry_(const std::string &prefix, const std::string &out) {
        if (!prefix.empty() && !out.empty() &&
            prefix.back() != ' ' &&
            std::isalnum((unsigned char) out.front()))
            return prefix + " " + out;
        return prefix + out;
    }

private:
    void cancel_locked_() {
        // r24.15: cancellation is an event, not the room's current level.
        // A blocked decode missed false -> true and published stale material.
        // ATHENA_INNER_CANCEL_EPOCH=0 restores those level-only checks.
        if (cancel_epoch_) epoch_.fetch_add(1, std::memory_order_relaxed);
        has_req_ = false;
        has_done_ = false;
        paused_.store(false, std::memory_order_relaxed);
    }

    void loop_() {
        for (;;) {
            std::string prompt;
            std::string prefix;
            bool dream = false;
            uint64_t epoch = 0;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [&] { return quit_.load(std::memory_order_relaxed) || has_req_; });
                if (quit_.load(std::memory_order_relaxed)) return;
                prompt = std::move(req_prompt_);
                prefix = std::move(req_prefix_);
                dream  = req_dream_;
                has_req_    = false;
                generating_ = true;
                epoch = epoch_.load(std::memory_order_relaxed);
            }
            std::string out;
            // The mailbox mutex is NOT held here — the decode can take seconds
            // and the audio thread must be able to post/permit/drain meanwhile.
            const bool done = fn_ &&
                fn_(prompt, dream,
                    [this, epoch] {
                        // R9-A: fast path is the old two relaxed loads. The
                        // parked path BLOCKS — callers must consult keep_going
                        // only while holding no lock the audio path needs,
                        // which generate_ satisfies (it is called strictly
                        // between guarded decodes).
                        if (paused_.load(std::memory_order_relaxed)) {
                            std::unique_lock<std::mutex> lk(mx_);
                            pause_cv_.wait(lk, [this, epoch] {
                                return !paused_.load(std::memory_order_relaxed) ||
                                        quit_.load(std::memory_order_relaxed) ||
                                       !permit_.load(std::memory_order_relaxed) ||
                                       (cancel_epoch_ && epoch_.load(std::memory_order_relaxed) != epoch);
                            });
                        }
                        return !quit_.load(std::memory_order_relaxed) &&
                                permit_.load(std::memory_order_relaxed) &&
                               (!cancel_epoch_ || epoch_.load(std::memory_order_relaxed) == epoch);
                    },
                    out);
            {
                std::lock_guard<std::mutex> lk(mx_);
                generating_ = false;
                // Publish only if it finished AND the room is still quiet — a
                // completion that raced his speech onset is discarded, exactly
                // as the cooperative pump discarded it.
                if (done && permit_.load(std::memory_order_relaxed) &&
                    !quit_.load(std::memory_order_relaxed) &&
                    (!cancel_epoch_ || epoch_.load(std::memory_order_relaxed) == epoch)) {
                    // r21-full.10 (F1): the prefilled opener is part of the
                    // thought — reunite it with its continuation here, so every
                    // consumer (ingest, journal, tests) sees the whole sentence.
                    // r24.12 (WO-I2): through join_carry_ — the prefix ends on
                    // a word now, and the join supplies the one space two
                    // touching alphanumerics need.
                    done_text_  = join_carry_(prefix, out);
                    done_dream_ = dream;
                    has_done_   = true;
                }
            }
        }
    }

    mutable std::mutex      mx_;
    std::condition_variable cv_;
    std::thread             th_;
    InnerGenFn              fn_;
    bool                    running_    = false;

    // mailbox (under mx_)
    std::string req_prompt_;
    std::string req_prefix_;   // r21-full.10 (F1): opener prefilled in the prompt
    bool        req_dream_  = false;
    bool        has_req_    = false;
    bool        generating_ = false;
    std::string done_text_;
    bool        done_dream_ = false;
    bool        has_done_   = false;

    // signals read lock-free from keep_going()'s fast path
    std::atomic<bool> quit_{false};
    std::atomic<bool> permit_{false};
    const bool cancel_epoch_;
    std::atomic<uint64_t> epoch_{0};
    // R9-A: her-turn park. Written under mx_ (except the suspend() set, which
    // can only cause an extra park, never a lost wake); the parked generation
    // sleeps on pause_cv_ and is woken by resume()/permit()/stop().
    std::atomic<bool> paused_{false};
    std::condition_variable pause_cv_;
};

} // namespace athena
