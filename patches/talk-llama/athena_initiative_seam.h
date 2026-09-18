// r24.23: journal adapter. Snapshots under Mind's mutex, I/O outside it.
#pragma once
#include "athena_consciousness.h"
#include "athena_continuity.h"
namespace ainit {
inline bool checkpoint(acont::Journal&j,acon::Mind&m,const std::string&session,double wall_origin) {
    const auto events=m.initiative_events();const auto milestones=m.project_milestones();
    if(events.empty()&&milestones.empty())return true;
    for(const auto&e:events){const auto&d=e.state;const auto&c=d.candidate;
        const auto wall=wall_origin+std::max({d.reserved,d.started,d.finished,d.response_from,d.social_at});
        if(!j.enqueue({"initiative-event",session+"/initiative/"+std::to_string(e.serial),session+"/decision/"+std::to_string(d.id),
            {{"serial",std::to_string(e.serial)},{"transition",e.kind},{"execution",name(d.execution)},{"social",name(d.social)},{"act",name(c.kind)},
             {"source",session+"/"+c.source},{"version",c.version},{"goal",c.goal},{"step",c.step},{"evidence",c.evidence},
             {"content",c.text},{"rationale",d.rationale},{"reason",d.reason},{"outcome",d.outcome},{"unfinished",d.unfinished},{"completed_step",d.completed_step},{"project_before",d.project_before},{"project_after",d.project_after},
             {"relevance",acont::number(c.relevance)},{"novelty",acont::number(c.novelty)},{"benefit",acont::number(c.benefit)},
             {"cost",acont::number(c.cost)},{"urgency",acont::number(c.urgency)},{"compared",std::to_string(c.compared)},
             {"input_epoch",std::to_string(d.input_epoch)},{"permission_epoch",std::to_string(d.permission_epoch)},
             {"arrangement",d.arrangement},{"recipient",d.recipient},{"reply_expected",d.expected_reply?"1":"0"},{"receipt",d.receipt},
             {"confirmed",d.delivered},{"source_retired",d.source_retired?"1":"0"},{"received_wall",acont::number(wall_origin+d.response_from)},{"wall",acont::number(wall)},{"closed",d.closed?"1":"0"},
             {"mode",name(m.config().initiative_policy)},{"source_status","an intention or private thought is not a world observation"}}}))return false;
    }
    for(const auto&e:milestones){const auto&p=e.project;
        if(!j.enqueue({"initiative-milestone",session+"/milestone/"+std::to_string(e.serial),session+"/"+p.step.result,
            {{"project_row",aproj::project_row_(p)},{"evidence",name(p.step.evidence)},
             {"criterion",p.step.criterion},{"step_revision",std::to_string(p.step.revision)},
             {"effect","cognitive progress; does not establish an external action"}}}))return false;
    }
    if(!j.flush())return false;
    if(!events.empty())m.acknowledge_initiative_events(events.back().serial);
    if(!milestones.empty())m.acknowledge_project_milestones(milestones.back().serial);
    return true;
}
inline void restore(acont::Journal&j,acon::Mind&m,double wall) {
    std::map<std::string,std::pair<Candidate,double>> seen;
    std::map<std::string,uint64_t> seen_serial;
    std::vector<std::string> declines,declined_texts;
    std::map<std::string,std::pair<uint64_t,Memory>> history;
    std::vector<aproj::Project> milestones;
    const acont::Record*arrangement=nullptr;
    for(const auto&kv:j.records()){
        if(kv.second.kind=="initiative-milestone") {aproj::Project p;if(aproj::parse_project_row_(kv.second.get("project_row"),p)&&p.step.revision)milestones.push_back(p);}
        if(kv.second.kind=="initiative-arrangement" && (!arrangement||kv.second.get("order")>arrangement->get("order")))arrangement=&kv.second;
    }
    for(const auto&kv:j.records())if(kv.second.kind=="initiative-event"){
        const auto&r=kv.second;
        try{
            const auto serial=std::stoull(r.get("serial").empty()?r.id.substr(r.id.rfind('/')+1):r.get("serial"));
            const double stamp=std::stod(r.get("wall"));
            if(std::isfinite(stamp) && r.get("mode")!="shadow" && (!history.count(r.parent)||serial>history[r.parent].first)){
                Memory h;h.key=r.parent;h.when=-std::max(0.0,wall-stamp);auto&d=h.decision;d.id=serial;
                auto&c=d.candidate;c.source=r.get("source");c.version=r.get("version");c.text=r.get("content");
                c.goal=r.get("goal");c.step=r.get("step");c.evidence=r.get("evidence");
                const auto factor=[&](const char*key,float&value){
                    try{const float v=std::stof(r.get(key));if(std::isfinite(v)&&v>=0&&v<=1)value=v;}catch(...){}
                };
                factor("relevance",c.relevance);factor("novelty",c.novelty);factor("benefit",c.benefit);factor("cost",c.cost);factor("urgency",c.urgency);
                try{c.compared=std::stoull(r.get("compared"));}catch(...){}

                for(auto k:{Kind::CONTACT_BID,Kind::SHARE,Kind::REPORT,Kind::SELF_NARRATION,Kind::PRIVATE_STEP,Kind::LOOK,Kind::RECALL_IMAGE})if(r.get("act")==name(k))c.kind=k;
                d.execution=Execution::UNKNOWN;
                for(auto e:{Execution::RESERVED,Execution::RUNNING,Execution::PARTIAL,Execution::SUCCEEDED,Execution::FAILED,Execution::CANCELLED,Execution::UNKNOWN,Execution::DEFERRED})if(r.get("execution")==name(e))d.execution=e;
                d.rationale=r.get("rationale");d.reason=r.get("reason");d.outcome=r.get("outcome");d.unfinished=r.get("unfinished");d.completed_step=r.get("completed_step");d.project_before=r.get("project_before");d.project_after=r.get("project_after");
                d.delivered=r.get("confirmed");d.receipt=r.get("receipt");d.confirmed=d.delivered.size();d.closed=r.get("closed")=="1";
                d.arrangement=r.get("arrangement");d.recipient=r.get("recipient");d.expected_reply=r.get("reply_expected")=="1";
                for(auto social:{Social::PENDING,Social::ENGAGED,Social::EXPLICIT_DECLINE,Social::NO_RESPONSE_OBSERVED,Social::NO_RESPONSE_EXPECTED,Social::UNAVAILABLE,Social::UNRELATED_REPLY,Social::UNVERIFIED_DELIVERY})if(r.get("social")==name(social))d.social=social;
                history[r.parent]={serial,std::move(h)};
            }
        }catch(...){} // Old/torn optional history does not replace established state.
        if(!r.get("confirmed").empty()){
            Candidate c;c.kind=Kind::SHARE;c.source=r.get("source");c.version=r.get("version");c.text=r.get("source_retired")=="1"?r.get("content"):r.get("confirmed");if(r.get("source_retired")!="1")c.source=r.parent+"/speech";c.goal=r.get("goal");c.step=r.get("step");
            try{
                // Journal keys are strings: /9 sorts after /10. Transitions
                // carry their monotone producer serial, not replay order.
                const auto serial=std::stoull(r.get("serial").empty()?r.id.substr(r.id.rfind('/')+1):r.get("serial"));
                const double saved=std::stod(r.get("received_wall"));
                if(std::isfinite(saved)&&(!seen_serial.count(r.parent)||serial>seen_serial[r.parent])){
                    seen[r.parent]={c,std::max(0.0,wall-saved)};seen_serial[r.parent]=serial;
                }
            }catch(...){}
        }
        if(r.get("social")=="explicit-decline"){declines.push_back(r.get("goal").empty()?r.get("source"):r.get("goal"));declined_texts.push_back(r.get("content"));}
    }
    std::vector<std::pair<Candidate,double>> rows;for(const auto&v:seen)rows.push_back(v.second);
    m.restore_initiative_history(rows,declines,declined_texts);
    std::vector<Memory> memories;for(const auto&v:history)memories.push_back(v.second.second);
    m.restore_initiative_memories(memories);
    m.restore_project_milestones(milestones);
    if(arrangement && arrangement->get("mode")=="revoked")m.restore_narration_restriction(arrangement->get("actor"),arrangement->parent);
    // Session-scoped listening/away grants and live tickets are deliberately
    // not restored. Historical reserved/running rows remain unresolved history,
    // never a claim of completion or permission to repeat an effect.
}
} // namespace ainit
