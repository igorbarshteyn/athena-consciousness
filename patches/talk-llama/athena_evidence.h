// r24.22: value-only evidence shared by input, memory, vision and speech.
// No Mind, device, model, locks or persistence dependencies.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aev {
struct EventId {
    std::string session;
    uint64_t sequence = 0;
    bool valid() const { return !session.empty() && sequence != 0; }
    std::string str() const { return valid() ? session + ":" + std::to_string(sequence) : ""; }
};
struct SourceRef {
    EventId event;
    uint64_t version = 1;
    std::string actor, producer, parent;
    size_t begin = 0, end = 0;
};
enum class Domain { UNKNOWN, REAL, TEST, QUOTED, HYPOTHETICAL, IMAGINED };
enum class InputOrigin { LEGACY, ORDINARY, PIVOT, TAPE, TERMINAL };
enum class BlockKind { ROLL, CUT, LOOK_ENCODE };
struct CaptureSpan {
    std::optional<uint64_t> first_sample, end_sample;
    std::optional<double> start_mono, end_mono;
    // Cursor is sample publication, not a promise of word-level acoustic time.
    bool publication_cursor = true;
    // Bitset relative to a known operation interval: before=1, during=2,
    // after=4; zero means unknown. A broad interval may straddle boundaries.
    unsigned relative_to(const CaptureSpan&operation)const {
        auto classify=[](auto a,auto b,auto x,auto y){unsigned relation=0;if(a<x)relation|=1;if(a<y&&b>x)relation|=2;if(b>y)relation|=4;return relation;};
        if(first_sample&&end_sample&&operation.first_sample&&operation.end_sample)
            return classify(*first_sample,*end_sample,*operation.first_sample,*operation.end_sample);
        if(start_mono&&end_mono&&operation.start_mono&&operation.end_mono)
            return classify(*start_mono,*end_mono,*operation.start_mono,*operation.end_mono);
        return 0;
    }
};
struct BlockingSpan {
    std::string id;
    BlockKind kind = BlockKind::ROLL;
    CaptureSpan capture;
    std::string outcome, acknowledgement;
};
struct AcousticObservation {
    std::array<float, 9> posterior{};
    bool posterior_available = false;
    std::string raw_tag, calibration_id;
    float evidence = 0, share = 0, probability = 0;
    std::optional<float> calibrated_reliability;
    bool explicitly_corrected = false;
};
struct InputEvidence {
    SourceRef source;
    std::string lexical, original_asr;
    std::vector<std::string> asr_events;
    std::string asr_transform;
    std::optional<std::string> learning_lexical; // qualified direct real-world clauses; raw speech remains complete
    std::string corrected_acoustic_source;
    std::string supersedes_input, revised_lexical; // explicit, resolved ASR correction; original stays immutable
    InputOrigin origin = InputOrigin::LEGACY;
    std::string domain_id = "unknown";
    std::vector<CaptureSpan> capture;
    std::vector<BlockingSpan> blocking;
    std::optional<double> decoded_mono, admitted_mono, measured_gap;
    long wall_time = 0;
    AcousticObservation acoustic;
};
struct AsrTrace {
    std::string id,outcome="failed";
    std::string lexical, uncertain_span; // same decode only; never persisted as a new fact
    double begin=0,end=0;
    size_t original_samples=0,submitted_samples=0;
};
inline AsrTrace&last_asr_trace(){static thread_local AsrTrace trace;return trace;}
struct ReplyTiming {
    double preparation=0,prefill_begin=0,prefill_end=0,first_token=0,first_sent=0,receipt=0;
    std::string stop="not ended";
    std::string first_sent_session, first_sent_text_hash;
};
struct ClaimQualifier {
    Domain domain = Domain::UNKNOWN;
    std::string speaker, subject, epistemic_status;
    std::vector<SourceRef> sources;
};
struct AdmissionRef {
    SourceRef source;
    uint64_t epoch = 0;
    std::string record_id, rendered;
    std::string kind, content; // exact admitted content, without the source wrapper
    bool complete = false, decoded = false;
};
enum class ActionKind { CAPTURE, ENCODE, ADMIT, KEEP, RECALL, SPEAK };
struct ActionRef {
    std::string id, request, target;
    ActionKind kind = ActionKind::CAPTURE;
    SourceRef source;
};
} // namespace aev
