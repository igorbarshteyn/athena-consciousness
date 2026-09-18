// r24.22: exact, source-qualified propositions in the companion journal.
#pragma once
#include "athena_continuity.h"
#include "athena_intent.h"
namespace aqual {
inline std::string norm(const std::string&s) {
    const auto low=aintent::lower(s);std::string r;bool space=true;
    for(unsigned char c:low){if(std::isspace(c)){if(!space)r+=' ';space=true;}else{r+=(char)c;space=false;}}
    return aintent::trim(r);
}
inline std::set<std::string> terms(const std::string&s) {
    std::set<std::string> out;std::string w;
    auto emit=[&]{if(w.size()>2)out.insert(w);w.clear();};
    for(unsigned char c:norm(s)){if(std::isalnum(c)||c>=128||c=='-')w+=(char)c;else emit();}emit();return out;
}
inline std::set<std::string> superseded_inputs(const acont::Journal&j) {
    std::set<std::string> out;
    for(const auto&kv:j.records())if(kv.second.kind=="asr-correction") {
        const auto previous=kv.second.get("previous");
        out.insert(previous);
        // A recognized correction owns both its immutable utterance and the
        // corrected interpretation derived from it. Retiring that utterance
        // must retire the companion source used by its facts and recall rows.
        const auto interpretation=j.records().find(previous+"/asr-correction");
        if(interpretation!=j.records().end() && interpretation->second.kind=="asr-correction" &&
           interpretation->second.parent==previous)out.insert(interpretation->first);
    }
    return out;
}
// An explicit recognition correction and an unambiguous source match are both
// required. "I said" by itself is reported speech, never a deletion command.
inline void resolve_revision(const acont::Journal&j,aev::InputEvidence&e) {
    const auto lo=norm(e.lexical);
    if(!(aintent::has(lo,"speech recognition")||aintent::has(lo,"transcription")||aintent::has(lo,"misheard")||aintent::has(lo,"mishearing")))return;
    // Locate in original-byte coordinates, not whitespace/Unicode-normalized
    // coordinates. "I asked" has a different length from "I said".
    std::string rawlow=e.lexical;for(char&c:rawlow)if((unsigned char)c<128)c=(char)std::tolower((unsigned char)c);
    size_t marker=rawlow.find("i said");size_t width=6;if(marker==std::string::npos){marker=rawlow.find("i asked");width=7;}
    if(marker==std::string::npos)return;
    std::string corrected=e.lexical.substr(marker+width);
    const auto first=corrected.find_first_not_of(" \t\r\n");if(first==std::string::npos)return;
    corrected=corrected.substr(first,corrected.find_last_not_of(" \t\r\n")-first+1);
    // Repair discourse, outside the corrected quotation, is not a word Athena
    // misheard: retain it in the immutable correction input, not its replacement.
    for(int n=0;n<3;++n){const auto low=norm(corrected);size_t skip=0;
        for(const char*lead:{"you,", "sorry,", "to you,"})if(low.rfind(lead,0)==0){skip=std::char_traits<char>::length(lead);break;}
        if(!skip)break;
        corrected=corrected.substr(skip);const auto at=corrected.find_first_not_of(" \t\r\n");corrected=at==std::string::npos?"":corrected.substr(at);
    }
    if(!corrected.empty()&&corrected.front()==':')corrected=aintent::trim(corrected.substr(1));
    for(const auto&q:std::vector<std::pair<std::string,std::string>>{{"\"","\""},{"'","'"},{"“","”"},{"‘","’"}})if(corrected.rfind(q.first,0)==0){
        const auto end=corrected.find(q.second,q.first.size());if(end==std::string::npos)return;
        corrected=corrected.substr(q.first.size(),end-q.first.size());break;
    }
    if(corrected.size()<8||corrected.size()>4096)return;
    const auto wanted=terms(corrected);const auto retired=superseded_inputs(j);
    std::string target;size_t best=0;bool tied=false;
    for(const auto&kv:j.records()){
        const auto&r=kv.second;if(r.kind!="input"||r.get("actor")!=e.source.actor||retired.count(r.id))continue;
        long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
        if(e.wall_time-wall>1800||wall>e.wall_time)continue;
        std::string raw;if(!acont::unhex(r.get("lexical_hex"),raw))continue;
        const auto interpretation=j.records().find(r.id+"/asr-correction");
        if(interpretation!=j.records().end() && interpretation->second.kind=="asr-correction" &&
           interpretation->second.parent==r.id)raw=interpretation->second.get("corrected");
        else if(aintent::has(norm(raw),"speech recognition")||aintent::has(norm(raw),"misheard")||
                aintent::has(norm(raw),"transcription")||aintent::has(norm(raw),"mishearing"))continue;
        if(norm(raw)==norm(corrected))continue;
        const auto old=terms(raw);size_t shared=0;for(const auto&w:wanted)shared+=old.count(w);
        if(shared<2||shared*2<std::min(wanted.size(),old.size()))continue;
        if(shared>best){best=shared;target=r.id;tied=false;}else if(shared==best)tied=true;
    }
    if(!target.empty()&&!tied){e.supersedes_input=target;e.revised_lexical=corrected;}
}
inline bool entity_mentioned(const std::string&q,const std::string&entity) {
    if(entity.empty())return false;
    if(aintent::has(q,entity))return true;
    return aintent::has(q,entity+"'s");
}
// Only resolve an explicit possessive attribute here. An open-ended question
// keeps the full family; lack of an indexed predicate never proves world absence.
inline std::string queried_attribute(const std::string&q,const std::string&entity) {
    const auto at=q.find(entity+"'s ");if(at==std::string::npos)return "";
    const auto from=at+entity.size()+3;size_t end=from;
    while(end<q.size()&&aintent::word(q[end]))++end;
    return q.substr(from,end-from);
}
inline std::string domain_of(const acont::Journal&j,const std::string&source) {
    auto d=j.records().find(source+"/domain");return d==j.records().end()?"unknown":d->second.get("domain");
}
inline std::vector<amem::Turn> learning_turns(const acont::Journal&j,const std::vector<amem::Turn>&raw) {
    std::vector<amem::Turn> out;const auto retired=superseded_inputs(j);
    for(const auto&t:raw){
        if(retired.count(t.source.event.str()))continue;
        auto v=t;v.text.clear();
        for(const auto&c:aintent::clauses(t.text))if(c.direct()){
            const auto at=j.records().find(t.source.event.str()+"/clause/"+std::to_string(t.source.begin+c.begin));
            const auto domain=at==j.records().end()?domain_of(j,t.source.event.str()):at->second.get("domain");
            if(domain=="real"||domain=="unknown")v.text+=c.text;
        }
        // Fiction remains in exact source retrieval and qualified facts. It is
        // not independent evidence of real progress/personality/other people.

        if(!v.text.empty())out.push_back(std::move(v));
    }
    return out;
}
inline bool correction(const std::string&s) {
    for(const char*w:{"correction","i meant","i changed","change that","actually","not anymore","i counted","i have counted","now counted"})if(aintent::has(s,w))return true;
    return false;
}
struct Fact {
    std::string id,key,actor,subject,predicate,value,unit,frequency,status,domain,source,previous,excerpt;
    size_t begin=0,end=0;
    long wall=0;uint64_t version=1;
    bool current=true,conflict=false;
};
inline Fact fact_of(const acont::Record&r) {
    Fact f;f.id=r.id;f.key=r.get("key");f.actor=r.get("actor");f.subject=r.get("subject");f.predicate=r.get("predicate");
    f.value=r.get("value");f.unit=r.get("unit");f.frequency=r.get("frequency");f.status=r.get("status");
    f.domain=r.get("domain");f.source=r.parent;f.previous=r.get("supersedes");f.excerpt=r.get("excerpt");f.conflict=r.get("conflict")=="1";
    try{f.wall=std::stol(r.get("wall"));f.version=std::stoull(r.get("version"));f.begin=std::stoull(r.get("begin"));f.end=std::stoull(r.get("end"));}catch(...){}
    return f;
}
inline std::vector<Fact> facts(const acont::Journal&j) {
    std::vector<Fact> out;std::set<std::string> superseded;const auto revised=superseded_inputs(j);
    for(const auto&kv:j.records())if(kv.second.kind=="qualified-fact"){
        auto f=fact_of(kv.second);if(!f.previous.empty())superseded.insert(f.previous);
        for(size_t i=0;i<kv.second.fields.size();++i){const auto previous=kv.second.get("supersedes."+std::to_string(i));
            if(previous.empty())break;
            superseded.insert(previous);
        }
        out.push_back(std::move(f));}
    std::map<std::string,std::set<std::string>> current_values;
    for(auto&f:out){f.current=!superseded.count(f.id)&&!revised.count(f.source);if(f.current)current_values[f.key].insert(norm(f.value));}
    // Conflict describes the active alternatives, not an interpretation that
    // has since been corrected. Retain the historical row's stored flag.
    for(auto&f:out)if(f.current)f.conflict=current_values[f.key].size()>1;
    std::sort(out.begin(),out.end(),[](const Fact&a,const Fact&b){if(a.wall!=b.wall)return a.wall<b.wall;if(a.source!=b.source){auto x=acont::event_id(a.source),y=acont::event_id(b.source);if(x.session==y.session)return x.sequence<y.sequence;}return a.id<b.id;});return out;
}
inline std::string render(const Fact&f) {
    return "["+f.id+" version "+std::to_string(f.version)+"; "+(f.current?(f.conflict?"conflicting assertion":"current assertion"):"earlier assertion")+
        "; subject "+f.subject+"; predicate "+f.predicate+"; value "+f.value+"; unit "+f.unit+"; frequency "+f.frequency+"; domain "+f.domain+"; speaker "+f.actor+"; source "+f.source+"; "+f.status+"] "+f.excerpt;
}
inline bool input(acont::Journal&j,const aev::InputEvidence&e) {
    if(!e.supersedes_input.empty()&&!j.enqueue({"asr-correction",e.source.event.str()+"/asr-correction",e.source.event.str(),{{"previous",e.supersedes_input},{"corrected",e.revised_lexical},{"actor",e.source.actor}}}))return false;
    const auto existing=facts(j);std::string domain="real",domain_owner;
    long last_wall=0;uint64_t last_seq=0;
    for(const auto&kv:j.records())if(kv.second.kind=="input-domain"&&kv.second.get("actor")==e.source.actor){
        const auto &r=kv.second;long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
        const auto ev=acont::event_id(r.parent);
        if(wall>last_wall||(wall==last_wall&&ev.sequence>last_seq)){last_wall=wall;last_seq=ev.sequence;domain=r.get("domain");domain_owner=r.get("domain_owner");}
    }
    bool corrected=false;size_t ordinal=0;bool ok=true;std::string count_subject;
    std::vector<Fact> all=existing;
    // A recognition repair edits an earlier interpretation, not the current
    // conversation's real/test setting. Carry an unambiguous source domain
    // across a later topic switch; do not guess when the source mixed domains.
    std::set<std::string> revision_domains;
    if(!e.supersedes_input.empty())for(const auto&kv:j.records())
        if(kv.second.kind=="source-clause" && kv.second.parent==e.supersedes_input)
            revision_domains.insert(kv.second.get("domain"));
    const std::string revision_domain=revision_domains.size()==1?*revision_domains.begin():"";
    const std::string active_domain=domain,active_domain_owner=domain_owner;
    if(!revision_domain.empty()) {
        domain=revision_domain;
        domain_owner=domain.rfind("test:",0)==0?domain.substr(5):"";
    }
    for(const auto&cl:aintent::clauses(e.revised_lexical.empty()?e.lexical:e.revised_lexical)) {
        const auto &lo=cl.low;std::string local_domain=domain;
        if((aintent::has(lo,"test story")||aintent::has(lo,"compaction test story"))||aintent::has(lo,"fictional story")||aintent::has(lo,"imaginary story")){
            // Repeating the corrected source's own story marker keeps that
            // story's identity. It is not the opening of another live story.
            if(revision_domain.empty()||domain!=revision_domain||domain.rfind("test:",0)!=0){
                domain_owner=e.source.event.str();domain="test:"+domain_owner;
            }
            local_domain=domain;
        }
        if(aintent::has(lo,"back to real life")||aintent::has(lo,"end of the test story")||aintent::has(lo,"outside the story")){domain="real";domain_owner.clear();local_domain=domain;}
        if(cl.quoted||cl.reported)local_domain="quoted:"+e.source.event.str();
        else if(cl.hypothetical && local_domain=="real")local_domain="hypothetical:"+e.source.event.str();
        // A first-person current-state disclosure or actual request is an
        // interaction event even while a test story is the active topic.
        if(cl.direct()&&(lo.rfind("i am ",0)==0||lo.rfind("i'm ",0)==0||lo.rfind("my ",0)==0||lo.rfind("please ",0)==0||lo.rfind("can you ",0)==0||lo.rfind("could you ",0)==0||!aintent::requested_acts(cl.text).empty()))local_domain="real";
        ok=j.enqueue({"source-clause",e.source.event.str()+"/clause/"+std::to_string(cl.begin),e.source.event.str(),{{"domain",local_domain},{"begin",std::to_string(cl.begin)},{"end",std::to_string(cl.end)},{"text",cl.text}}})&&ok;
        corrected=corrected||(cl.direct()&&correction(lo));
        // Only explicit declarative copulas/possessions are structured. Everything
        // else remains in its exact input source, queryable without invented slots.
        if(cl.text.find('?')!=std::string::npos)continue;
        const auto raw=aintent::trim(cl.text);std::string rawlo=raw;
        for(char&c:rawlo)if((unsigned char)c<128)c=(char)std::tolower((unsigned char)c);
        size_t pos=std::string::npos;std::string verb;
        std::string special_subject,special_value,special_predicate;
        if(cl.direct()&&(rawlo.rfind("i counted ",0)==0||rawlo.rfind("i have counted ",0)==0)) {
            count_subject=norm(raw.substr(rawlo.rfind("i have",0)==0?15:10));
            if(count_subject.rfind("the ",0)==0)count_subject=count_subject.substr(4);
            // The next explicit numeric clause can supply the count. Merely
            // having counted does not invent its value.
            continue;
        }
        if(rawlo.rfind("there may be ",0)==0||rawlo.rfind("there are ",0)==0) {
            const size_t start=rawlo.rfind("there may",0)==0?13:10;
            const auto rest=raw.substr(start);const auto space=rest.find(' ');
            static const std::set<std::string> numbers={"zero","one","two","three","four","five","six","seven","eight","nine","ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen","twenty"};
            const auto number=norm(rest.substr(0,space));
            if(numbers.count(number)||(!number.empty()&&std::all_of(number.begin(),number.end(),[](char c){return std::isdigit((unsigned char)c)||c=='.'||c==',';}))) {
                special_value=number;special_predicate="count";
                if(space!=std::string::npos){special_subject=norm(rest.substr(space+1));const auto loc=special_subject.find(" in ");if(loc!=std::string::npos)special_subject=special_subject.substr(0,loc);}
                else special_subject=count_subject;
            }
        }
        const auto moved=rawlo.find(" moved from ");
        if(moved!=std::string::npos){const auto to=rawlo.find(" to ",moved+12);if(to!=std::string::npos){special_subject=norm(raw.substr(0,moved));special_value=raw.substr(to+4);special_predicate="is";corrected=cl.direct();}}
        for(const char*v:{" is "," are "," has "," measures "," contains "," stays on "}){
            const auto at=rawlo.find(v);if(at!=std::string::npos&&(pos==std::string::npos||at<pos)){pos=at;verb=v;}}
        if(pos==std::string::npos&&special_subject.empty())continue;
        std::string subject=special_subject.empty()?norm(raw.substr(0,pos)):special_subject;
        std::string value=special_subject.empty()?aintent::trim(raw.substr(pos+verb.size())):special_value;
        for(const char*prefix:{"actually, ","correction, ","correction: ","for our test story, ","in our test story, ","in the test story, ","for this compaction test story, ","one correction to our test story: "})
            if(subject.rfind(prefix,0)==0)subject=aintent::trim(subject.substr(std::char_traits<char>::length(prefix)));
        if(subject.empty()||subject.size()>180||value.empty()||subject=="there"||subject.rfind("if ",0)==0||subject.rfind("whether ",0)==0)continue;
        std::string predicate=special_predicate.empty()?aintent::trim(verb):special_predicate;
        if(predicate=="stays on")predicate="is";
        if(subject.size()>4&&subject.substr(subject.size()-4)==" one"){
            const auto prefix=subject.substr(0,subject.size()-4);std::set<std::string> matches;
            for(const auto&old:all)if(old.actor==e.source.actor&&old.domain==local_domain&&old.subject.rfind(prefix+" ",0)==0)matches.insert(old.subject);
            if(matches.size()==1)subject=*matches.begin();else continue;
        }
        if(predicate=="count"&&subject.rfind("the ",0)==0)subject=subject.substr(4);
        const auto owner=subject.find("'s ");
        if(owner!=std::string::npos){predicate=subject.substr(owner+3)+"/"+predicate;subject=subject.substr(0,owner);}
        const std::string key=acont::checksum(e.source.actor+"\n"+local_domain+"\n"+subject+"\n"+predicate);
        Fact f;f.id=e.source.event.str()+"/fact/"+std::to_string(++ordinal);f.key=key;f.actor=e.source.actor;f.subject=subject;f.predicate=predicate;
        f.value=value;f.domain=local_domain;f.source=e.source.event.str()+(e.revised_lexical.empty()?"":"/asr-correction");f.wall=e.wall_time;f.begin=cl.begin;f.end=cl.end;f.excerpt=cl.text;
        f.status="asserted";
        for(const char*w:{"possibly","maybe","not counted","uncounted","i think","i guess","about","approximately","may be"})if(aintent::has(lo,w)){f.status="uncertain";break;}
        if((predicate=="count"&&!count_subject.empty())||(aintent::has(lo,"counted")&&!aintent::has(lo,"not counted")&&!aintent::has(lo,"uncounted")))f.status="counted";
        for(const char*u:{"centimeters","centimetres","centimeter","centimetre","meters","metres","meter","metre","millimeters","millimetres","feet","inches","cm","mm","m"})if(aintent::has(norm(value),u)){f.unit=u;break;}
        for(const char*w:{"daily","weekly","monthly","yearly","every day","every week","per day","per week"})if(aintent::has(norm(value),w)){f.frequency=w;break;}
        std::vector<std::string> supersedes;
        const bool revises=corrected||(predicate=="count"&&f.status=="counted");
        for(auto it=all.rbegin();it!=all.rend();++it)if(it->key==key&&it->current){
            f.version=std::max(f.version,it->version+1);
            if(revises){
                if(f.previous.empty())f.previous=it->id;
                supersedes.push_back(it->id);it->current=false;
            }
            else if(norm(it->value)!=norm(f.value))f.conflict=true;
            if(!revises)break;
        }
        acont::Record assertion{"qualified-fact",f.id,f.source,{{"key",key},{"actor",f.actor},{"subject",subject},{"predicate",predicate},
            {"value",value},{"unit",f.unit},{"frequency",f.frequency},{"status",f.status},{"domain",local_domain},
            {"supersedes",f.previous},{"conflict",f.conflict?"1":"0"},{"version",std::to_string(f.version)},
            {"wall",std::to_string(e.wall_time)},{"begin",std::to_string(cl.begin)},{"end",std::to_string(cl.end)},{"excerpt",cl.text}}};
        // A correction resolves the active alternatives for this source-owned
        // property, not merely the last alternative. Keep every source and the
        // singular predecessor for older readers; exact additional links make
        // the whole correction reproducible after a restart.
        for(size_t i=0;i<supersedes.size();++i)assertion.fields["supersedes."+std::to_string(i)]=supersedes[i];
        ok=j.enqueue(assertion)&&ok;
        all.push_back(f);
    }
    // Markers inside a resolved replacement describe that earlier statement.
    // They can change its clause domains without changing the live topic.
    if(!e.supersedes_input.empty()){domain=active_domain;domain_owner=active_domain_owner;}
    return j.enqueue({"input-domain",e.source.event.str()+"/domain",e.source.event.str(),
        {{"actor",e.source.actor},{"wall",std::to_string(e.wall_time)},{"domain",domain},{"domain_owner",domain_owner}}})&&ok;
}
// r26.1: one explicit citation grammar, shared by admission and replay.
// Decorations never override journal ownership/domain. Unknown decorations,
// multiple citations and misplaced citations fail closed without guessing IDs.
struct CandidateCitation {
    std::string id, body;
    bool present=false, valid=true;
};
inline CandidateCitation candidate_citation(const std::string&raw) {
    CandidateCitation c;c.body=amem::trim_copy(raw);
    const auto at=c.body.find("[source ");if(at==std::string::npos)return c;
    c.present=true;
    const auto end=c.body.find(']',at);
    if(end==std::string::npos||end-at>300||c.body.find("[source ",at+1)!=std::string::npos){c.valid=false;return c;}
    const bool leading=at==0, trailing=amem::trim_copy(c.body.substr(end+1)).empty();
    if(!leading&&!trailing){c.valid=false;return c;}
    const auto fields=c.body.substr(at+8,end-at-8);const auto semi=fields.find(';');
    c.id=amem::trim_copy(fields.substr(0,semi));
    if(c.id.empty()||c.id.find_first_of(" \t\r\n[]")!=std::string::npos){c.valid=false;return c;}
    if(semi!=std::string::npos){
        const auto extra=amem::trim_copy(fields.substr(semi+1));
        if(extra.rfind("domain ",0)!=0||extra.find(';')!=std::string::npos){c.valid=false;return c;}
        const auto domain=extra.substr(7);
        if(domain!="real"&&domain!="unknown"&&domain!="imagined"&&domain!="quoted"&&domain!="hypothetical"&&domain!="fictional"){c.valid=false;return c;}
    }
    c.body=amem::trim_copy(leading?c.body.substr(end+1):c.body.substr(0,at));return c;
}
inline bool assertion_source(const std::string&text) {
    // Mechanical evidence deliberately does not pretend to solve entailment.
    // Questions, conditional/quoted clauses remain retrievable as discussion.
    if(text.find('?')!=std::string::npos)return false;
    for(const auto&cl:aintent::clauses(text))if(!cl.direct())return false;
    return !amem::trim_copy(text).empty();
}
inline void qualify_candidate(amem::MemEntry&e,const acont::Journal&j,const std::vector<amem::Turn>&offered) {
    const auto citation=candidate_citation(e.gist);const amem::Turn*source=nullptr;
    std::string gist=citation.body;
    if(citation.valid)for(const auto&t:offered){
        const bool match=citation.present ? t.source.event.str()==citation.id :
            !gist.empty()&&norm(t.text).find(norm(gist))!=std::string::npos;
        if(match){if(source){source=nullptr;break;}source=&t;}
    }
    e.qualified_source.clear();e.source_clause.clear();e.evidence_excerpt.clear();
    e.source_speaker.clear();e.source_act.clear();e.evidence_begin=e.evidence_end=0;
    e.source_resolved=source!=nullptr;e.source_learning=false;e.claim_support="unresolved";e.gist=gist;
    if(!source){e.source_domain="unresolved";e.gist="[Unresolved model summary; not independent evidence of the described event] "+gist;return;}
    e.qualified_source=source->source.event.str();e.source_domain=domain_of(j,e.qualified_source);e.source_clause=source->text;
    e.source_speaker=source->speaker;e.source_act=assertion_source(source->text)?"assertion":"question-or-qualified-discussion";
    const auto retired=superseded_inputs(j);if(retired.count(e.qualified_source))e.source_domain="superseded-asr";
    // Optional byte span is checked against the actually OFFERED source, not
    // the unoffered remainder. An exact quote is evidence of those words only.
    bool span_valid=true;
    if(gist.rfind("[evidence ",0)==0){
        const auto close=gist.find(']'),colon=gist.find(':',10);
        span_valid=false;
        if(close!=std::string::npos&&colon!=std::string::npos&&colon<close){
            const auto a=gist.substr(10,colon-10),b=gist.substr(colon+1,close-colon-1);
            auto digits=[](const std::string&v){return !v.empty()&&v.find_first_not_of("0123456789")==std::string::npos;};
            if(digits(a)&&digits(b))try{
                const size_t begin=std::stoull(a),end=std::stoull(b);
                if(begin>=source->source.begin&&end>=begin&&end-source->source.begin<=source->text.size()){
                    const size_t lo=begin-source->source.begin,hi=end-source->source.begin;
                    span_valid=amem::u8::clip_len(source->text,lo)==lo&&amem::u8::clip_len(source->text,hi)==hi&&hi>lo;
                    if(span_valid){e.evidence_begin=begin;e.evidence_end=end;e.evidence_excerpt=source->text.substr(lo,hi-lo);}
                }
            }catch(...){}
            gist=amem::trim_copy(gist.substr(close+1));
        }
    }
    // With no explicit span, only literal bytes can establish one. Normalized
    // matching is citation resolution alone; it cannot invent byte offsets.
    if(e.evidence_excerpt.empty()&&span_valid&&!gist.empty()){
        const auto at=source->text.find(gist);
        if(at!=std::string::npos){e.evidence_begin=source->source.begin+at;e.evidence_end=e.evidence_begin+gist.size();e.evidence_excerpt=gist;}
    }
    const bool exact=span_valid&&!e.evidence_excerpt.empty()&&norm(e.evidence_excerpt)==norm(gist);
    e.claim_support=exact?"exact-source-words":e.source_act=="assertion"?"derived-unverified":"question-or-discussion-not-answer";
    const auto eligible=learning_turns(j,{*source});std::string real;for(const auto&t:eligible)real+=t.text;
    bool asserted_span=false;
    if(exact)for(const auto&cl:aintent::clauses(source->text)){
        const size_t begin=source->source.begin+cl.begin,end=source->source.begin+cl.end;
        // Quoting one word out of a question or omitting an assertion's
        // negation is not a supported new preference. The exact assertion
        // clause is the smallest mechanically checkable learning unit.
        if(e.evidence_begin>=begin&&e.evidence_end<=end&&assertion_source(cl.text)&&
           norm(cl.text)==norm(e.evidence_excerpt))asserted_span=true;
    }
    if(asserted_span)e.source_act="assertion";
    e.source_learning=exact&&asserted_span&&!retired.count(e.qualified_source)&&
        !real.empty()&&norm(real).find(norm(e.evidence_excerpt))!=std::string::npos;
    e.gist="[Derived from source "+e.qualified_source+"; domain "+e.source_domain+"; speaker "+source->speaker+"] "+
        (exact?"[Exact source words; pronouns belong to that speaker] ":"[Unverified interpretation; citation does not establish claim support] ")+gist;
}
// Review old generated summaries without editing their historical rows. The
// versioned review withdraws unsupported authority and links the actual reply.
inline void review_derived_memories(acont::Journal&j,std::vector<amem::MemEntry>&rows) {
    for(auto&m:rows){
        if(m.gist.find("[Exact source words;")!=std::string::npos||m.gist.find("[Unverified interpretation;")!=std::string::npos)continue;
        const std::string prefix="[Derived from source ";const auto at=m.gist.find(prefix);
        if(at==std::string::npos)continue;
        const auto end=m.gist.find(';',at+prefix.size());
        if(end==std::string::npos)continue;
        const auto id=m.gist.substr(at+prefix.size(),end-at-prefix.size());
        const auto source=j.records().find(id+"/work");if(source==j.records().end())continue;
        std::string raw;if(!acont::unhex(source->second.get("text_hex"),raw)||raw.find('?')==std::string::npos)continue;
        std::string replies;
        for(const auto&kv:j.records())if(kv.second.kind=="speech-outcome"&&kv.second.get("input")==id&&!kv.second.get("confirmed_hex").empty())
            replies+=(replies.empty()?"":" ")+kv.second.parent;
        const auto review=m.id+"/support-review-v1/"+acont::checksum(m.gist);
        j.enqueue({"summary-review",review,id,{{"memory_id",m.id},{"original",m.gist},{"version","1"},
            {"supersedes_authority",m.id},{"verdict","question citation does not support an attributed answer"},
            {"question_source",id},{"answer_sources",replies},{"source_words",raw}}});
        m.gist="[Source review "+review+": unverified interpretation of a question; use answer sources "+replies+" for the actual response] "+m.gist;
        m.source_learning=false;
    }
    j.flush();
}
inline void project_current(const acont::Journal&j,std::vector<amem::MemEntry>&state,long now,
                            const std::string&state_path="") {
    (void)now;const auto ff=facts(j);std::map<std::string,std::set<std::string>> expected;
    for(const auto&f:ff){
        // An older projection may have rendered this source current before a
        // correction. Recognize precisely those generated renderings only.
        for(bool current:{false,true})for(bool conflict:{false,true}){auto prior=f;prior.current=current;prior.conflict=conflict;
            expected["q22-"+f.key].insert(render(prior));expected["q22-"+f.key+"-"+acont::checksum(f.id)].insert(render(prior));
            for(const auto&base:{"q22-"+f.key,"q22-"+f.key+"-"+acont::checksum(f.id)})expected[base+"-source-"+acont::checksum(f.id)].insert(render(prior)+" [conflicts with an externally edited projection; resolution required]");}
    }
    // The legacy loader scrubs, clips and optionally heals a narrative gist.
    // A long qualified rendering therefore differs after restart even though
    // nobody edited it. Only the exact persisted rendering proves ownership:
    // accepting its clipped prefix alone would hide edits beyond that prefix.
    std::map<std::string,std::set<std::string>> persisted;
    const bool read_ok=state_path.empty()||amem::scan_state_rows_(state_path,[&](const amem::MemEntry&e){
        if(expected.count(e.id))persisted[e.id].insert(e.gist);
    },false,false,true);
    auto loaded_gist=[](const std::string&raw){
        auto gist=amem::scrub_memory_fact_(raw);
        if(amem::gist_heal_on())amem::heal_gist_tail(gist);
        return gist;
    };
    std::set<std::string> edited;
    state.erase(std::remove_if(state.begin(),state.end(),[&](amem::MemEntry&e){auto it=expected.find(e.id);if(it==expected.end())return false;
        const auto disk=persisted.find(e.id);
        if(read_ok&&disk!=persisted.end()&&disk->second.size()==1){
            const auto&raw=*disk->second.begin();
            if(loaded_gist(raw)==e.gist){
                if(it->second.count(raw))return true;
                // Preserve a genuine external edit in full, including a tail
                // the bounded live loader could not show.
                e.gist=raw;edited.insert(e.id);return false;
            }
        }
        if(it->second.count(e.gist))return true;
        edited.insert(e.id);return false;
    }),state.end());
    for(const auto&f:ff)if(f.current){
        amem::MemEntry e;e.id="q22-"+f.key+(f.conflict?"-"+acont::checksum(f.id):"");
        // A human/other writer edited the managed projection. Retain it and
        // render the source as a separately identified, explicit conflict.
        const bool conflict=edited.count(e.id);if(conflict)e.id+="-source-"+acont::checksum(f.id);
        e.born=f.wall;e.last_recall=f.wall;e.gist=render(f)+(conflict?" [conflicts with an externally edited projection; resolution required]":"");e.salience=6;e.S=1;
        auto old=std::find_if(state.begin(),state.end(),[&](const amem::MemEntry&row){return row.id==e.id;});
        if(old==state.end())state.push_back(std::move(e));else if(old->gist==e.gist){} // idempotent conflict projection; never overwrite its external edit
    }
}

} // namespace aqual
