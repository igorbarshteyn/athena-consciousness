// r24.23: elapsed-time initiative policy. No model, worker, device, or I/O.
#pragma once
#include "athena_intent.h"
#include <deque>
#include <map>
#include <set>
#include <limits>

namespace ainit {
enum class Mode { LEGACY, SHADOW, SUSTAINED };
struct ModeSetting { Mode mode=Mode::SUSTAINED; bool valid=true; };
inline ModeSetting mode_setting(const char*p) {
    if(!p)return {};
    const std::string v=p;
    if(v=="legacy"||v=="0")return {Mode::LEGACY,true};
    if(v=="shadow")return {Mode::SHADOW,true};
    if(v=="sustained")return {};
    return {Mode::SUSTAINED,false};
}
inline const char*name(Mode m){return m==Mode::LEGACY?"legacy":m==Mode::SHADOW?"shadow":"sustained";}
inline bool speech_slot(Mode mode,bool ready,bool human_pending,bool private_busy,bool vision_ready,bool fairness=true){
    return ready&&(mode!=Mode::SUSTAINED||(!human_pending&&(!fairness||(!private_busy&&!vision_ready))));
}
inline bool on(const char*key){const auto p=std::getenv(key);return !p||std::string(p)!="0";}
enum class Kind { CONTACT_BID, SHARE, REPORT, SELF_NARRATION, PRIVATE_STEP, LOOK, RECALL_IMAGE };
inline const char*name(Kind k) {
    switch(k){case Kind::CONTACT_BID:return "contact";case Kind::SHARE:return "share";case Kind::REPORT:return "report";
    case Kind::SELF_NARRATION:return "narration";case Kind::PRIVATE_STEP:return "private-step";case Kind::LOOK:return "look";case Kind::RECALL_IMAGE:return "recall-image";}return "unknown";
}
enum class Execution { RESERVED, RUNNING, PARTIAL, SUCCEEDED, FAILED, CANCELLED, UNKNOWN, DEFERRED };
enum class Social { PENDING, ENGAGED, EXPLICIT_DECLINE, NO_RESPONSE_OBSERVED, NO_RESPONSE_EXPECTED, UNAVAILABLE, UNRELATED_REPLY, UNVERIFIED_DELIVERY };
inline const char*name(Execution e){static const char*const v[]={"reserved","running","partial","succeeded","failed","cancelled","unknown","deferred"};return v[(size_t)e];}
inline const char*name(Social s){static const char*const v[]={"pending","engaged","explicit-decline","no-response-observed","no-response-expected","unavailable","unrelated-reply","unverified-delivery"};return v[(size_t)s];}
inline bool audible(Kind k){return k==Kind::CONTACT_BID||k==Kind::SHARE||k==Kind::REPORT||k==Kind::SELF_NARRATION;}
inline bool delivered_question(const std::string&s){
    for(size_t i=0;i<s.size();++i)if(s[i]=='?'&&aintent::direct_at(s,i))return true;
    return false;
}
inline bool explicit_decline(const std::string&s,const std::string&bot="Athena"){
    if(aintent::deferral(s))return true;
    for(const auto&cl:aintent::clauses(s))if(cl.direct()){
        const auto t=aintent::addressed(cl.low,bot);
        for(const char*p:{"stop asking me that","stop bringing that up","leave that topic alone","don't ask me that again","do not ask me that again"})
            if(t==p)return true;
    }
    return false;
}
inline bool short_response(const std::string&s){
    const auto t=aintent::lower(aintent::direct_text(s));
    for(const char*p:{"yes","no","yeah","nope","right","okay","ok","mm-hm","uh-huh","i see","that's interesting","that is interesting","thank you","thanks","go on","tell me more"})if(t==p)return true;
    return false;
}
inline bool contact_only(const std::string&s,const std::string&recipient=""){
    bool found=false;
    for(const auto&cl:aintent::clauses(s)){
        if(!cl.direct())return false;
        auto t=aintent::addressed(cl.low,recipient);bool contact=false;
        // Discourse prefaces do not turn a contact check into a contribution.
        bool filler=false;
        for(const char*p:{"well","so","hey","hello","hmm","okay","ok"}){
            if(t==p){filler=true;break;}
            const std::string prefix=std::string(p)+",";
            if(t.rfind(prefix,0)==0)t=aintent::addressed(aintent::trim(t.substr(prefix.size())),recipient);
        }
        if(filler)continue;
        for(const char*p:{"are you there","are you still there","are you listening","would you like to talk","want to talk","what do you think","does that make sense","can you hear me"})if(t==p)contact=true;
        if(!contact)return false;
        found=true;
    }
    return found;
}
inline std::vector<std::string> concepts(const std::string&s) {
    auto w=aintent::content_words(s);
    w.erase(std::remove_if(w.begin(),w.end(),[](const std::string&x){return aintent::has("and but that this with from just still really now think thought wonder wondered perhaps might could would seem",x);}),w.end());
    std::sort(w.begin(),w.end());w.erase(std::unique(w.begin(),w.end()),w.end());return w;
}
inline bool repeated(const std::vector<std::string>&a,const std::vector<std::string>&b) {
    if(a.empty()||b.empty())return false;
    size_t same=0;for(const auto&x:a)if(std::binary_search(b.begin(),b.end(),x))++same;
    // Reordering, inflection and extra discourse wrappers do not make news.
    // This is a conservative lexical test, not a claim of semantic inference.
    return same*4>=std::max(a.size(),b.size())*3 ||
           (same==std::min(a.size(),b.size())&&std::max(a.size(),b.size())-same<3);
}
struct Candidate {
    Kind kind=Kind::CONTACT_BID;
    std::string source,version,goal,step,evidence,text;
    std::vector<std::string> meaning;
    double born=0,expires=std::numeric_limits<double>::infinity();
    float motive=.5f;
    // Observable heuristic factors, not calibrated probabilities or utility
    // measurements. Cost is a bounded estimate of occupying the relevant lane.
    float relevance=.5f,novelty=1.f,benefit=.5f,cost=.1f,urgency=0.f;
    size_t compared=0;
    std::string rationale;
    bool verified=false;
    int provenance=0;
    std::string key()const{return source+"/"+version;}
};
inline float unit(float v){return std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;}
inline float utility(const Candidate&c){
    return 2.f*unit(c.urgency)+.35f*unit(c.relevance)+.25f*unit(c.novelty)+
        .30f*unit(c.benefit)-.20f*unit(c.cost);
}
inline Candidate choose(std::vector<Candidate> pool,const std::string&held=""){
    if(pool.empty())return {};
    auto best=pool.begin();
    for(auto i=pool.begin()+1;i!=pool.end();++i)
        if(utility(*i)>utility(*best) || (utility(*i)==utility(*best)&&i->key()<best->key()))best=i;
    // A modestly better new thought does not continually reset formation.
    for(auto i=pool.begin();i!=pool.end();++i)if(i->key()==held &&
        i->urgency>=best->urgency && utility(*i)+.08f>=utility(*best)){best=i;break;}
    Candidate out=*best;out.compared=pool.size();
    out.rationale=out.urgency>.8f?"a current obligation takes priority":
        (out.relevance>.65f?"this connects to the current conversation or an unfinished goal":
         "this is a fresh independent thought worth developing");
    out.rationale+=out.cost>.65f?"; its greater cost was included in the comparison":"; its estimated cost fits this opportunity";
    if(pool.size()>1)out.rationale+="; compared with other eligible contributions";
    return out;
}
struct Decision {
    uint64_t id=0,input_epoch=0,permission_epoch=0;
    Candidate candidate;
    std::string arrangement,receipt,delivered,recipient;
    std::string rationale,reason,outcome,unfinished,completed_step,project_before,project_after;
    Execution execution=Execution::RESERVED;
    Social social=Social::PENDING;
    double reserved=0,started=0,finished=0,response_from=0,social_at=0;
    bool expected_reply=true,contact_charged=false,learned=false,closed=false,source_retired=false;
    size_t confirmed=0;
};
struct Gates {
    double now=0,idle=0,min_silence=45,refractory=150,other_speech_start=-1e9;
    bool quiet=false,absent=false,compacting=false,human_pending=false,private_busy=false;
    bool no_reply_expected=false,away_narration=false;
    uint64_t input_epoch=0,permission_epoch=0;
    std::string arrangement,recipient;
    float uncertainty=0;
};
struct Event {
    uint64_t serial=0,decision=0;
    std::string kind;
    Decision state;
};
inline bool decision_query(const std::string&text,const std::string&bot="Athena"){
    for(const auto&cl:aintent::clauses(text))if(cl.direct()){
        const auto t=aintent::addressed(cl.low,bot);
        for(const char*p:{"why did you speak","why did you say","why did you wait","why are you quiet","why did you stay quiet","why did you choose","why did you decide","why did you work","why did you do that","what did you decide","what were you working on","what remains unfinished","why haven't you spoken","why are you silent"})
            if(t.rfind(p,0)==0 || t.rfind(std::string("can you tell me ")+p,0)==0)return true;
    }
    return false;
}
struct Memory {std::string key;Decision decision;double when=0;};
struct SocialResult {uint64_t decision=0;Social outcome=Social::PENDING;};
class Policy {
    uint64_t rng_=1,seq_=0,event_seq_=0;
    std::map<uint64_t,Decision> decisions_;
    std::deque<Event> events_;
    std::vector<Memory> history_;
    Candidate considered_;
    std::string last_wait_key_;
    double last_wait_t_=-1e9;
    struct Seen {Candidate candidate;double when=0;};
    std::deque<Seen> seen_;
    std::set<std::string> declined_;
    std::vector<std::vector<std::string>> declined_meanings_;
    Candidate selected_;
    bool ready_=false;
    double last_tick_=-1,eligible_=0,threshold_=0,last_start_=-1e9,pressure_=0;
    uint64_t active_=0,contact_episode_=0;
    int contacts_=0;
    std::string reason_="no fresh contribution is ready",shadow_key_;
    double random_(){rng_^=rng_<<13;rng_^=rng_>>7;rng_^=rng_<<17;return (rng_>>11)*(1.0/9007199254740992.0);}
    void set_reason_(const std::string&why){
        reason_=why;
        const auto key=why+"/"+considered_.key();
        if(key==last_wait_key_ || last_tick_-last_wait_t_<30)return;
        last_wait_key_=key;last_wait_t_=last_tick_;
        Decision d;d.id=++seq_;d.candidate=considered_;d.rationale=considered_.rationale;
        d.reason=why;d.reserved=d.finished=last_tick_;d.closed=true;
        d.execution=Execution::DEFERRED;d.social=Social::NO_RESPONSE_EXPECTED;
        emit_("deferred",d);
        restore_memory({"current-wait/"+std::to_string(d.id),d,last_tick_});
    }
    void emit_(const char*kind,const Decision&d){events_.push_back({++event_seq_,d.id,kind,d});}
    void resolve_(Decision&d,Social s,std::vector<SocialResult>&out){
        if(d.social!=Social::PENDING)return;
        d.social=s;d.learned=true;out.push_back({d.id,s});emit_("social",d);
    }
    void refund_(Decision&d){if(d.contact_charged){if(contacts_>0&&d.input_epoch==contact_episode_)--contacts_;d.contact_charged=false;}}
public:
    explicit Policy(uint64_t seed=1):rng_(seed?seed:1){}
    void seed(uint64_t seed){rng_=seed?seed:1;}
    uint64_t random_state()const{return rng_;}
    uint64_t active()const{return active_;}
    const auto&decisions()const{return decisions_;}
    const Decision*decision(uint64_t id)const{const auto i=decisions_.find(id);return i==decisions_.end()?nullptr:&i->second;}
    const Candidate&selected()const{return selected_;}
    const std::string&reason()const{return reason_;}
    bool ready()const{return ready_;}
    int contacts()const{return contacts_;}
    bool contact_publication_allowed(uint64_t id,const std::string&text,int limit=2)const{
        const auto*d=decision(id);
        if(!d || !audible(d->candidate.kind) || d->closed)return false;
        if(!contact_only(text,d->recipient))return true;
        // Reservation may already own one charge. It does not get charged a
        // second time here, and a SHARE label cannot exempt the actual words.
        return d->arrangement.empty() && contacts_-(d->contact_charged?1:0)<limit;
    }
    void publication_deferred(uint64_t id,double now){
        cancel(id,now);
        auto i=decisions_.find(id);if(i==decisions_.end())return;
        i->second.reason="the generated draft sought attention instead of offering the selected contribution; it was withheld before publication";
        emit_("publication-deferred",i->second);
    }
    double pressure()const{return pressure_;}
    double last_start()const{return last_start_;}
    std::vector<Event> pending_events()const{return {events_.begin(),events_.end()};}
    void acknowledge(uint64_t serial){
        while(!events_.empty()&&events_.front().serial<=serial)events_.pop_front();
        // Only persisted, resolved decisions can leave the working set.
        for(auto i=decisions_.begin();i!=decisions_.end()&&decisions_.size()>64;){
            bool pending=false;for(const auto&e:events_)if(e.decision==i->first){pending=true;break;}
            if(i->second.closed&&i->second.social!=Social::PENDING&&!pending)i=decisions_.erase(i);else ++i;
        }
    }
    bool novel(const Candidate&c,double now,double retirement=5400)const {
        if(c.source.empty()||c.text.empty()||now>=c.expires)return false;
        if(declined_.count(c.goal.empty()?c.source:c.goal))return false;
        if(c.kind==Kind::CONTACT_BID)return true;
        const auto words=c.meaning.empty()?concepts(c.text):c.meaning;
        if(words.size()<3)return false;
        for(const auto&w:declined_meanings_)if(repeated(words,w))return false;
        for(const auto&old:seen_)if(retirement<=0||now-old.when<retirement){
            if(old.candidate.key()==c.key()||repeated(words,old.candidate.meaning))return false;
        }
        return true;
    }
    void block(const char*why){ready_=false;eligible_=0;set_reason_(why);}
    void human_epoch(uint64_t epoch){
        if(epoch!=contact_episode_){contacts_=0;contact_episode_=epoch;}
        ready_=false;eligible_=0;
    }
    void reconsider(){block("waiting for the room and the current source to settle");}
    void observe_shadow(const Gates&g){
        if(!ready_ || selected_.key()==shadow_key_)return;
        shadow_key_=selected_.key();Decision d;d.id=++seq_;d.candidate=selected_;d.reserved=g.now;
        d.arrangement=g.arrangement;d.input_epoch=g.input_epoch;d.permission_epoch=g.permission_epoch;
        emit_("shadow-proposal-no-effect",d);
    }
    void update(const Candidate&c,const Gates&g,int contact_limit=2,double retirement=5400) {
        if(!std::isfinite(g.now)||!std::isfinite(g.idle))return;
        const double dt=last_tick_<0?0:std::max(0.0,g.now-last_tick_);last_tick_=g.now;considered_=c;
        // The journal retains lifetime records; this is the live retirement
        // index only. Explicit declines are stored separately and never age out.
        if(retirement>0)seen_.erase(std::remove_if(seen_.begin(),seen_.end(),[&](const Seen&s){return g.now-s.when>=retirement;}),seen_.end());
        pressure_*=std::exp(-dt*std::log(2.0)/600.0);
        const double pressure=std::clamp(std::max(pressure_,(double)g.uncertainty),0.0,1.0);
        if(g.quiet){block("holding the quiet you requested");return;}
        if(g.absent&&!g.away_narration){block("keeping my thoughts private while you are away");return;}
        if(g.compacting||g.human_pending){block("leaving the floor for incoming speech");return;}
        if(g.private_busy){ready_=false;set_reason_("letting my current private thought finish");return;}
        if(active_){block("my earlier attempt has not finished");return;}
        if(!novel(c,g.now,retirement)){block("nothing sufficiently new is ready to share");return;}
        if(c.kind==Kind::CONTACT_BID&&(contacts_>=contact_limit||g.no_reply_expected||g.absent)){
            block("letting you choose when to answer");return;
        }
        if(selected_.key()!=c.key()||selected_.kind!=c.kind){selected_=c;eligible_=0;threshold_=(15+60*random_())/std::clamp((double)c.motive,.3,1.0);ready_=false;}
        selected_=c; // refresh comparison facts without redrawing its threshold
        if(g.idle<g.min_silence||g.now-std::max(last_start_,g.other_speech_start)<g.refractory*(1+2*pressure)){
            ready_=false;set_reason_("giving the room some space before another contribution");return;
        }
        // Time is sampled by Mind at no more than one hertz. Missed/suspended
        // time cannot accumulate a burst; one eligible second is the max step.
        eligible_+=std::min(dt,1.0);
        ready_=eligible_>=threshold_;
        reason_=ready_?"a fresh thought is ready to share":"a thought is forming, without needing an answer yet";
    }
    uint64_t reserve(const Gates&g) {
        if(!ready_)return 0;
        ready_=false;eligible_=0; // consume before any downstream early exit
        if(g.quiet||g.compacting||g.human_pending||g.private_busy||(g.absent&&!g.away_narration))return 0;
        Decision d;d.id=++seq_;d.input_epoch=g.input_epoch;d.permission_epoch=g.permission_epoch;d.candidate=selected_;
        d.rationale=selected_.rationale;d.reason="selected a current contribution after the room became available";
        d.arrangement=g.arrangement;d.recipient=g.recipient;d.reserved=g.now;d.expected_reply=d.candidate.kind==Kind::CONTACT_BID&&!g.no_reply_expected;
        if(d.candidate.kind==Kind::CONTACT_BID){++contacts_;d.contact_charged=true;}
        decisions_[d.id]=d;active_=d.id;emit_("reserved",d);return d.id;
    }
    bool start(uint64_t id,const Gates&g,bool source_current=true) {
        auto i=decisions_.find(id);if(i==decisions_.end())return false;auto&d=i->second;
        if(d.execution!=Execution::RESERVED)return d.execution==Execution::RUNNING&&!d.closed;
        if(!source_current||g.now>=d.candidate.expires||g.quiet||g.compacting||g.human_pending||g.private_busy||(g.absent&&!g.away_narration)||g.input_epoch!=d.input_epoch||g.permission_epoch!=d.permission_epoch||g.arrangement!=d.arrangement){cancel(id,g.now);return false;}
        d.reason="the source and current permission were revalidated before starting";
        d.execution=Execution::RUNNING;d.started=g.now;last_start_=g.now;emit_("started",d);return true;
    }
    // The policy's receipt verdict also owns downstream learning. Keep the
    // original void API below for callers that only need the ledger update.
    bool accept_receipt(uint64_t id,const std::string&confirmed,const std::string&source,double now,bool complete,bool final=false,const std::string&spoken_words="") {
        auto i=decisions_.find(id);if(i==decisions_.end())return false;auto&d=i->second;
        if(d.closed||d.execution==Execution::RESERVED)return false;
        // Cumulative prefix, never an aggregate count or a different utterance.
        if(confirmed.size()<d.delivered.size()||confirmed.compare(0,d.delivered.size(),d.delivered)!=0)return false;
        if(!d.delivered.empty()&&!d.receipt.empty()&&!source.empty()&&d.receipt!=source)return false;
        d.delivered=confirmed;d.confirmed=confirmed.size();if(!source.empty())d.receipt=source;
        if(!d.source_retired&&!confirmed.empty()&&d.candidate.kind!=Kind::CONTACT_BID){
            auto seen=d.candidate;seen.meaning=concepts(confirmed);
            const auto intent=concepts(d.candidate.text);size_t anchors=0;
            for(const auto&w:intent)if(std::binary_search(seen.meaning.begin(),seen.meaning.end(),w))++anchors;
            if(anchors>=3&&anchors*2>=intent.size()){
                seen.text=confirmed;seen_.push_back({std::move(seen),now});d.source_retired=true;
            }
        }
        if(!confirmed.empty()){
            d.execution=complete?Execution::SUCCEEDED:Execution::PARTIAL;d.response_from=now;
            // A complete, unquoted question creates an expectation only in an
            // ordinary exchange. An explicit no-reply arrangement still wins.
            if(d.arrangement.empty()&&delivered_question(confirmed))d.expected_reply=true;
            // A model can ignore the offered thought and produce only a ping.
            // Account for the actual contact bid without marking that thought
            // told. A substantive question still has its own reply expectation.
            if(contact_only(spoken_words.empty()?confirmed:spoken_words,d.recipient)&&!d.contact_charged&&d.input_epoch==contact_episode_){++contacts_;d.contact_charged=true;}
        }
        if(final){d.closed=true;d.finished=now;if(confirmed.empty()){d.execution=Execution::FAILED;d.social=Social::UNVERIFIED_DELIVERY;refund_(d);}if(active_==id)active_=0;}
        d.reason=confirmed.empty()?"no speech delivery was confirmed":complete?"the utterance has a complete delivery receipt":"only the recorded prefix has a delivery receipt";
        emit_("receipt",d);
        return true;
    }
    void receipt(uint64_t id,const std::string&confirmed,const std::string&source,double now,bool complete,bool final=false,const std::string&spoken_words="") {
        accept_receipt(id,confirmed,source,now,complete,final,spoken_words);
    }
    void cancel(uint64_t id,double now,bool unknown=false) {
        auto i=decisions_.find(id);if(i==decisions_.end()||i->second.closed)return;auto&d=i->second;
        d.closed=true;d.finished=now;last_start_=std::max(last_start_,now);
        if(d.confirmed){d.execution=Execution::PARTIAL;}else{d.execution=unknown?Execution::UNKNOWN:Execution::CANCELLED;d.social=Social::UNVERIFIED_DELIVERY;refund_(d);}
        if(active_==id)active_=0;
        d.reason=unknown?"the attempt timed out with its outcome unresolved":"the attempt was interrupted or its eligibility changed";
        emit_("cancelled",d);
    }
    std::vector<SocialResult> respond(uint64_t owner,const std::string&text,bool relevant,bool unavailable,double now,const std::string&bot="Athena") {
        std::vector<SocialResult> out;auto i=decisions_.find(owner);if(i==decisions_.end())return out;auto&d=i->second;
        if(!d.confirmed||d.social!=Social::PENDING)return out;
        d.social_at=now;
        if(unavailable)resolve_(d,Social::UNAVAILABLE,out);
        else if(explicit_decline(text,bot)&&now-d.response_from<=120){
            declined_.insert(d.candidate.goal.empty()?d.candidate.source:d.candidate.goal);
            declined_meanings_.push_back(concepts(d.candidate.text));
            resolve_(d,Social::EXPLICIT_DECLINE,out);pressure_=std::min(1.0,pressure_+.3);
        }else if(relevant&&now-d.response_from<=120){resolve_(d,Social::ENGAGED,out);pressure_*=.5;}
        else resolve_(d,Social::UNRELATED_REPLY,out);
        return out;
    }
    std::vector<SocialResult> expire(double now,bool unavailable) {
        std::vector<SocialResult> out;
        for(auto&i:decisions_){auto&d=i.second;
            // The speech watchdog does not own private worker lifetimes. A
            // parked cognitive request resolves through its actual result or
            // abandonment, even if it takes longer than the speech deadline.
            if(!d.closed&&audible(d.candidate.kind)&&now-d.reserved>600)cancel(d.id,now,true);
            if(d.closed&&d.confirmed&&d.social==Social::PENDING&&now-d.response_from>120){
                d.social_at=now;
                resolve_(d,unavailable?Social::UNAVAILABLE:(!d.expected_reply?Social::NO_RESPONSE_EXPECTED:Social::NO_RESPONSE_OBSERVED),out);
                if(!unavailable&&d.expected_reply)pressure_=std::min(1.0,pressure_+.15);
            }
        }
        return out;
    }
    // Private work owns a decision but never takes the speech active slot.
    uint64_t begin_private(Candidate c,double now,const std::string&project_before=""){
        Decision d;d.id=++seq_;d.project_before=project_before;d.candidate=std::move(c);d.candidate.kind=Kind::PRIVATE_STEP;
        d.rationale=d.candidate.rationale;d.reason="an owned cognitive request was admitted in a private work slot";
        d.reserved=d.started=now;d.execution=Execution::RUNNING;d.expected_reply=false;
        d.social=Social::NO_RESPONSE_EXPECTED;decisions_[d.id]=d;emit_("private-started",d);return d.id;
    }
    void finish_private(uint64_t id,double now,Execution state,const std::string&reason,
                        const std::string&result,const std::string&unfinished,const std::string&completed_step="",const std::string&project_after=""){
        auto i=decisions_.find(id);if(i==decisions_.end()||i->second.closed)return;
        auto&d=i->second;d.closed=true;d.finished=now;d.execution=state;
        d.reason=reason;d.outcome=result;d.unfinished=unfinished;d.completed_step=completed_step;d.project_after=project_after;emit_("private-result",d);
    }
    void restore_memory(Memory memory){
        if(memory.key.empty())return;
        for(auto&h:history_)if(h.key==memory.key){h=std::move(memory);return;}
        history_.push_back(std::move(memory));
        std::sort(history_.begin(),history_.end(),[](const Memory&a,const Memory&b){return a.when<b.when || (a.when==b.when&&a.key<b.key);});
        if(history_.size()>64)history_.erase(history_.begin(),history_.end()-64);
    }
    std::vector<Memory> memories()const{
        auto out=history_;
        for(const auto&kv:decisions_){const auto&d=kv.second;out.push_back({"current/"+std::to_string(d.id),d,std::max(d.reserved,d.finished)});}
        return out;
    }
    std::string history(const std::string&query)const{
        auto rows=memories();const auto words=concepts(query);
        auto relevance=[&](const Memory&m){
            const auto terms=concepts(m.decision.candidate.goal+" "+m.decision.candidate.text+" "+m.decision.reason);
            size_t same=0;for(const auto&w:words)if(std::binary_search(terms.begin(),terms.end(),w))++same;return same;
        };
        std::map<std::string,size_t> ranks;
        for(const auto&row:rows)ranks[row.key]=relevance(row);
        std::stable_sort(rows.begin(),rows.end(),[&](const Memory&a,const Memory&b){
            const auto ar=ranks[a.key],br=ranks[b.key];return ar==br?a.when>b.when:ar>br;
        });
        if(rows.empty())return "I have no recorded initiative decision history yet";
        auto excerpt=[](const std::string&x,size_t n){
            if(x.size()<=n)return x;
            while(n && ((unsigned char)x[n]&0xc0)==0x80)--n;
            return x.substr(0,n)+"...";
        };
        std::string out="recorded initiative history (choices and cognitive products, not proof of world actions): ";
        size_t count=0;
        for(const auto&row:rows){
            if(count++==2)break;
            const auto&d=row.decision;if(count>1)out+="; earlier record: ";
            out+=d.execution==Execution::DEFERRED?"I waited":d.candidate.kind==Kind::PRIVATE_STEP?"I chose private work":audible(d.candidate.kind)?"I chose to speak":"I considered an action";
            if(!d.candidate.goal.empty())out+=" on "+excerpt(d.candidate.goal,70);
            out+="; why: "+excerpt(d.rationale.empty()&&d.reason.empty()?"the earlier record did not retain a reason":d.execution==Execution::DEFERRED||d.rationale.empty()?d.reason:d.rationale,170);
            if(d.execution!=Execution::DEFERRED)out+="; outcome: "+std::string(name(d.execution))+" - "+excerpt(d.reason,150);
            if(!d.outcome.empty())out+="; private product: "+excerpt(d.outcome,130);
            if(!d.unfinished.empty())out+="; unfinished: "+excerpt(d.unfinished,100);
            if(!d.delivered.empty())out+="; confirmed words: "+excerpt(d.delivered,100);
            if(d.execution==Execution::RUNNING||d.execution==Execution::RESERVED)out+="; final outcome is not recorded";
        }
        return out;
    }
    void restore_seen(Candidate c,double when){
        c.meaning=concepts(c.text);
        for(auto&s:seen_)if(s.candidate.key()==c.key()){if(when>s.when)s={std::move(c),when};return;}
        seen_.push_back({std::move(c),when});
    }
    void restore_decline(const std::string&scope){if(!scope.empty())declined_.insert(scope);}
    void restore_declined_meaning(const std::string&text){
        if(text.empty())return;
        const auto meaning=concepts(text);
        if(std::find(declined_meanings_.begin(),declined_meanings_.end(),meaning)==declined_meanings_.end())declined_meanings_.push_back(meaning);
    }
    std::string awareness()const {
        if(active_){const auto*d=decision(active_);if(d&&d->confirmed)return "some of my unprompted words were heard; the rest is still unresolved";
            if(d&&!d->arrangement.empty())return "you invited me to explore aloud without waiting for a reply; I have chosen a thought to share; " + d->rationale;
            return "I chose to share a thought; that choice is not yet evidence it was heard"+(d&&!d->rationale.empty()?"; "+d->rationale:std::string());}
        if(!decisions_.empty()){
            const Decision*spoken=nullptr;
            for(auto i=decisions_.rbegin();i!=decisions_.rend();++i)if(i->second.execution!=Execution::DEFERRED&&audible(i->second.candidate.kind)){spoken=&i->second;break;}
            if(!spoken)return reason_;
            const auto&d=*spoken;
            if(d.closed&&last_tick_-d.finished<300){
                if(d.social==Social::UNVERIFIED_DELIVERY)return "my attempt to speak has no confirmed delivery";
                if(d.execution==Execution::PARTIAL)return "only part of my unprompted thought was heard";
                if(d.social==Social::PENDING)return d.expected_reply
                    ? "my unprompted words were heard; I do not yet know your reaction"
                    : "I shared a thought without asking you for a response";
                if(d.social==Social::NO_RESPONSE_EXPECTED)return "I shared a thought without asking you for a response";
                if(d.social==Social::NO_RESPONSE_OBSERVED)return "I have no answer to my earlier question; silence leaves your reaction uncertain";
            }
        }
        return reason_;
    }
};
// Cumulative speech boundary state. The foreground calls this before both
// streaming and fallback publication. It never edits already delivered words.
class ContactPublication {
    std::string published_;
public:
    std::string proposed(const std::string&chunk)const{return published_+chunk;}
    void accepted(const std::string&chunk){published_+=chunk;}
    void restored(const std::string&confirmed){published_=confirmed;}
};
} // namespace ainit
