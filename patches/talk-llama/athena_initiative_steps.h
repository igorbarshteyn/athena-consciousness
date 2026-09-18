// Owned private milestones. These types cannot execute an external action.
#pragma once
#include "athena_initiative.h"
namespace ainit {
enum class ProgressEvidence { NONE, ACKNOWLEDGED_UNDERSTANDING, PRIVATE_PRODUCT_ACCEPTED, ACTION_VERIFIED };
inline const char*name(ProgressEvidence e){
    switch(e){case ProgressEvidence::ACKNOWLEDGED_UNDERSTANDING:return "acknowledged-understanding";
    case ProgressEvidence::PRIVATE_PRODUCT_ACCEPTED:return "private-product-accepted";
    case ProgressEvidence::ACTION_VERIFIED:return "action-verified";default:return "none";}
}
struct Step {
    uint64_t revision=0;
    std::string source,criterion,dependencies,action,result;
    bool credited=false,fulfilled=false;
    ProgressEvidence evidence=ProgressEvidence::NONE;
};
inline std::string private_action(const std::string&source){
    const auto s=aintent::lower(aintent::direct_text(source));
    // Only an already recorded, unconditional cognitive step is executable in
    // the private worker. Conditional human questions retain the plan system.
    for(const char*p:{"compare ","outline ","summarize ","explain ","reflect on ","consider ","sketch "})
        if(s.rfind(p,0)==0)return s;
    return "";
}
inline bool refresh_step(Step&step,const std::string&source){
    if(step.revision && step.source==source)return false;
    const auto next=step.revision+1;step={};step.revision=next?next:1;step.source=source;
    step.action=private_action(source);
    step.criterion=step.action.empty()?"a source-bound acknowledgment of understanding":"a substantive private product addressing the recorded step";
    step.dependencies=step.action.empty()?"existing response or capability outcome":"current project source and owned private request";
    return true;
}
inline std::string private_step_proposal_clause(std::string t){
    if(t.rfind("i think ",0)==0)t=aintent::trim(t.substr(8));
    for(const char*p:{"my next step is to ","next, i'll ","next i will ","i will ","i'll "}){
        const size_t n=std::char_traits<char>::length(p);
        if(t.rfind(p,0)==0){const auto action=private_action(t.substr(n));if(!action.empty())return action;}
    }
    return "";
}
inline std::string proposed_private_step(const std::string&text){
    for(const auto&cl:aintent::clauses(text))if(cl.direct()&&cl.text.find('?')==std::string::npos){
        const auto action=private_step_proposal_clause(cl.low);
        if(!action.empty())return action;
    }
    return "";
}
inline bool private_product(const Step&step,const std::string&text){
    if(step.action.empty()||step.fulfilled)return false;
    // Promising to do work is not the work. Require some direct propositional
    // content beyond an intention, acknowledgment, question, or quoted report.
    // Topic anchors and elaboration must come from that content too: an
    // unrelated observation followed by a detailed plan is still only a plan.
    // These bounded lexical checks do not validate the product's factual truth.
    std::string substantive;bool developed=false;
    for(const auto&cl:aintent::clauses(text))if(cl.direct()&&cl.text.find('?')==std::string::npos){
        auto s=cl.low;
        if(s.rfind("i think ",0)==0)s=aintent::trim(s.substr(8));
        if(!private_step_proposal_clause(s).empty())continue;
        if(s.rfind("i will ",0)==0||s.rfind("i'll ",0)==0||s.rfind("i plan ",0)==0||s.rfind("i understand",0)==0||s.rfind("i promise",0)==0)continue;
        if(!substantive.empty())substantive+=' ';
        substantive+=s;
        if(concepts(s).size()>=8)developed=true;
    }
    const auto source=concepts(step.source),result=concepts(substantive);
    size_t shared=0;for(const auto&w:source)if(std::binary_search(result.begin(),result.end(),w))++shared;
    return developed && shared>=2 && result.size()>=source.size()+5;
}
inline bool credit(Step&step,ProgressEvidence kind,const std::string&result){
    if(!step.revision||step.fulfilled||kind==ProgressEvidence::NONE||result.empty())return false;
    if(kind==ProgressEvidence::ACKNOWLEDGED_UNDERSTANDING&&step.credited)return false;
    const bool first=!step.credited;
    step.credited=true;step.evidence=kind;step.result=result;
    if(kind!=ProgressEvidence::ACKNOWLEDGED_UNDERSTANDING){step.fulfilled=true;step.action.clear();}
    return first;
}

// Cognitive work has its own eligibility, independent of human salience.
// The existing worker still grants resources; this ledger supplies spacing,
// fair rotation and evidence-based reconsideration, never an execution permit.
class WorkLedger {
public:
    struct State {
        double last=-1e9,next=0;
        unsigned failures=0;
        bool needs_review=false;
        std::string source,product;
        std::vector<std::string> steps,products;
    };
private:
    std::map<std::string,State> rows_;
public:
    const State*state(const std::string&key)const{auto i=rows_.find(key);return i==rows_.end()?nullptr:&i->second;}
    bool ready(const std::string&key,double now)const{const auto*s=state(key);return !s||now>=s->next;}
    double last(const std::string&key)const{const auto*s=state(key);return s?s->last:-1e9;}
    bool fresh_step(const std::string&key,const std::string&step)const{
        const auto*s=state(key);if(!s)return true;
        for(const auto&old:s->steps)if(repeated(concepts(step),concepts(old)))return false;
        return true;
    }
    bool fresh_product(const std::string&key,const std::string&product)const{
        const auto*s=state(key);if(!s)return true;
        for(const auto&old:s->products)if(repeated(concepts(product),concepts(old)))return false;
        return true;
    }
    void attempted(const std::string&key,const std::string&source,double now){
        auto&s=rows_[key];s.source=source;s.last=now;s.next=now+600;
    }
    void resolved(const std::string&key,double now,bool useful,const std::string&product,
                  const std::string&completed_step="",bool reconsidered=false){
        auto&s=rows_[key];s.failures=useful?0:std::min(s.failures+1,4u);
        s.next=now+600*(1u<<s.failures);
        if(!completed_step.empty())s.needs_review=true;
        else if(reconsidered)s.needs_review=false;
        if(useful&&!product.empty()){
            s.product=product;s.products.push_back(product);
            if(s.products.size()>32)s.products.erase(s.products.begin());
        }
        if(!completed_step.empty()){
            s.steps.push_back(completed_step);if(s.steps.size()>32)s.steps.erase(s.steps.begin());
        }
    }
    void retain(const std::set<std::string>&keys){
        for(auto i=rows_.begin();i!=rows_.end();)if(!keys.count(i->first))i=rows_.erase(i);else ++i;
    }
};

// Contract-only seam for a future executor; currently used by mock acceptance
// tests. No production dispatch reads these types and no tools are installed.
struct Capability {
    std::string id,version,input_schema,scope,permission,success_predicate,provenance;
    double estimated_cost=0;
    bool cancellable=false,idempotent=false,reconcilable=false;
};
struct CapabilityGrant {std::string capability,scope,human_source;uint64_t epoch=0;double until=0;bool revoked=false;};
struct EffectProposal {std::string id,capability,scope;uint64_t permission_epoch=0;Execution state=Execution::RESERVED;};
inline bool mock_dispatch_allowed(const Capability&c,const CapabilityGrant&g,const EffectProposal&p,double now){
    if(c.id.empty()||c.version.empty()||c.input_schema.empty()||c.success_predicate.empty()||c.provenance.empty())return false;
    if(g.revoked||g.human_source.empty()||now>=g.until||g.capability!=c.id||p.capability!=c.id||g.scope!=c.scope||p.scope!=g.scope||p.permission_epoch!=g.epoch)return false;
    // Even idempotent retries need the SAME effect identity and an explicitly
    // recognized state. An untrusted tool result cannot create a grant.
    return !p.id.empty()&&(p.state==Execution::RESERVED || (p.state==Execution::FAILED&&c.idempotent));
}
} // namespace ainit
