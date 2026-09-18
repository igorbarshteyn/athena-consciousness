// r24.22: bounded clause/source view used by action and conversational controls.
#pragma once
#include "athena_evidence.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <locale.h>
#include <string>
#include <vector>
namespace aintent {
inline bool word(char c){return (unsigned char)c>=128||std::isalnum((unsigned char)c)||c=='\''||c=='-';}
inline std::string trim(std::string s){
    const auto a=s.find_first_not_of(" \t\r\n,;.!?");if(a==std::string::npos)return "";
    return s.substr(a,s.find_last_not_of(" \t\r\n,;.!?")-a+1);
}
// Decode UTF-8 as code points; never pass bytes from a multibyte name to
// tolower(). Locale lifetime is process-long and never mutates global locale.
inline std::string lower(const std::string&s){
    static locale_t unicode=[](){auto l=newlocale(LC_CTYPE_MASK,"C.UTF-8",nullptr);if(!l)l=newlocale(LC_CTYPE_MASK,"en_US.UTF-8",nullptr);return l;}();
    std::string out;
    for(size_t i=0;i<s.size();){const unsigned char lead=s[i];uint32_t cp=lead;size_t n=1;
        if(lead>=0xc2&&lead<=0xdf){cp=lead&31;n=2;}else if(lead>=0xe0&&lead<=0xef){cp=lead&15;n=3;}else if(lead>=0xf0&&lead<=0xf4){cp=lead&7;n=4;}
        bool valid=i+n<=s.size();for(size_t k=1;k<n&&valid;++k){const auto c=(unsigned char)s[i+k];if((c&192)!=128)valid=false;else cp=(cp<<6)|(c&63);}
        if(!valid||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)){out+=s[i++];continue;}
        i+=n;if(cp==0x2018||cp==0x2019)cp='\'';else if(cp<128)cp=(uint32_t)std::tolower((unsigned char)cp);else if(unicode)cp=(uint32_t)towlower_l((wint_t)cp,unicode);
        if(cp<128)out+=(char)cp;else if(cp<0x800){out+=(char)(0xc0|(cp>>6));out+=(char)(0x80|(cp&63));}
        else if(cp<0x10000){out+=(char)(0xe0|(cp>>12));out+=(char)(0x80|((cp>>6)&63));out+=(char)(0x80|(cp&63));}
        else{out+=(char)(0xf0|(cp>>18));out+=(char)(0x80|((cp>>12)&63));out+=(char)(0x80|((cp>>6)&63));out+=(char)(0x80|(cp&63));}
    }return out;
}
inline bool has(const std::string&s,const std::string&w){
    size_t p=0;while((p=s.find(w,p))!=std::string::npos){
        if((p==0||!word(s[p-1]))&&(p+w.size()==s.size()||!word(s[p+w.size()])))return true;
        ++p;}
    return false;
}
struct Clause {
    size_t begin=0,end=0;
    std::string text,low;
    bool quoted=false,reported=false,hypothetical=false;
    bool direct()const{return !quoted&&!reported&&!hypothetical;}
};
inline std::vector<Clause> clauses(const std::string&s){
    std::vector<Clause> out;size_t from=0;bool quote=false;
    auto push=[&](size_t end){
        if(end<=from)return;
        Clause c;c.begin=from;c.end=end;c.text=s.substr(from,end-from);c.low=lower(trim(c.text));c.quoted=(!c.low.empty() && (c.low.front()=='"' || c.low.front()=='\'')) || c.low.rfind("\xe2\x80\x9c",0)==0;
        // Reported speech is scoped to this sentence, never a global turn veto.
        for(const char*p:{"he said", "she said", "they said", "you said", "i said", "the script says", "the note says", "the line reads", "for example", "the way you let me", "you let me"})
            if(has(c.low,p))c.reported=true;
        for(const char*p:{"suppose", "imagine that", "hypothetically", "in the story", "in this fictional", "i dreamed", "i dreamt", "in my dream", "i imagined"})
            if(has(c.low,p))c.hypothetical=true;
        if(!c.low.empty())out.push_back(std::move(c));
        from=end;
    };
    for(size_t i=0;i<s.size();++i){
        const bool single=s[i]=='\''&&(i==0||!word(s[i-1]));
        const bool single_close=s[i]=='\''&&quote&&(i+1==s.size()||!word(s[i+1]));
        if(s[i]=='"'||single||single_close){quote=!quote;
            if(!quote && i>0 && (s[i-1]=='.'||s[i-1]=='!'||s[i-1]=='?'))push(i+1);
        }
        if(s.compare(i,3,"\xe2\x80\x9c")==0||s.compare(i,3,"\xe2\x80\x9d")==0){quote=!quote;i+=2;continue;}
        if(!quote&&!(s[i]=='.'&&i+1<s.size()&&std::isdigit((unsigned char)s[i+1])&&(i==0||std::isdigit((unsigned char)s[i-1])||std::isspace((unsigned char)s[i-1])))&&(s[i]=='.'||s[i]=='!'||s[i]=='?'||s[i]=='\n'||s[i]==';'))push(i+1);
    }
    push(s.size());return out;
}
inline bool direct_at(const std::string&s,size_t at){
    for(const auto &c:clauses(s))if(at>=c.begin&&at<c.end){
        if(!c.direct())return false;
        bool quote=false;
        for(size_t i=c.begin;i<at;++i){
            if(s[i]=='"')quote=!quote;
            else if(s[i]=='\'' && ((i==0||!word(s[i-1])) || (quote&&(i+1==s.size()||!word(s[i+1])))))quote=!quote;
            else if(s.compare(i,3,"\xe2\x80\x9c")==0||s.compare(i,3,"\xe2\x80\x9d")==0){quote=!quote;i+=2;}
        }
        return !quote;
    }
    return false;
}
// Keep quoted names/values, but a quoted assertion or instruction cannot own a
// first-person promise. The unmodified source remains in the episode/transcript.
inline bool quoted_assertion(const std::string&words){
    const auto t=lower(trim(words));
    for(const char*p:{"i will", "i'll", "i am", "i'm", "i have", "i've", "i want", "i promise", "i prefer", "i like", "i love", "i hate", "my favorite",
        "we will", "we'll", "we finished", "we decided", "we have", "he is", "she is", "they are", "you are", "you should", "you must", "let me", "ask me", "remind me"})
        if(has(t,p))return true;
    return false;
}
inline std::string direct_text(const std::string&text){
    std::string out;for(const auto&c:clauses(text))if(c.direct()){
        size_t from=0,i=0;
        while(i<c.text.size()){
            size_t width=0;std::string close;
            if(c.text[i]=='"'){width=1;close="\"";}
            else if(c.text[i]=='\''&&(i==0||!word(c.text[i-1]))){width=1;close="'";}
            else if(c.text.compare(i,3,"“")==0){width=3;close="”";}
            if(!width){++i;continue;}
            const auto end=c.text.find(close,i+width);if(end==std::string::npos)break;
            if(quoted_assertion(c.text.substr(i+width,end-i-width))){out+=c.text.substr(from,i-from);out+="[quoted speech]";from=end+close.size();}
            i=end+close.size();
        }
        out+=c.text.substr(from);out+=' ';
    }return trim(out);
}
// Conservative action matching: subject overlap alone cannot satisfy a
// requested predicate. Inflections share a small linguistic stem, no S23 values.
inline std::vector<std::string> content_words(const std::string&raw){
    std::vector<std::string> out;const auto text=lower(raw);size_t at=0;
    while(at<text.size()){while(at<text.size()&&!word(text[at]))++at;const auto begin=at;while(at<text.size()&&word(text[at]))++at;
        if(begin==at)continue;
        auto w=text.substr(begin,at-begin);
        if(has("a an the i me my you your he she it its we our they their to of for about whether if how what when will would have has had be is are was were do did does next please ask remind",w))continue;
        if(w.size()>4&&w.compare(w.size()-2,2,"'s")==0)w.resize(w.size()-2);
        if(w.size()>5&&w.compare(w.size()-3,3,"ing")==0)w.resize(w.size()-3);
        else if(w.size()>4&&w.compare(w.size()-2,2,"ed")==0)w.resize(w.size()-2);
        else if(w.size()>3&&w.back()=='s')w.pop_back();
        if(w.size()>2)out.push_back(w);
    }return out;
}
inline bool requested_action_matches(const std::string&action,const std::string&cue,const std::string&words){
    const auto a=content_words(action),c=content_words(cue),w=content_words(words);
    auto present=[&](const std::string&x){return std::find(w.begin(),w.end(),x)!=w.end();};
    const bool bound=has(lower(action),lower(cue));
    if(bound&&!std::all_of(c.begin(),c.end(),present))return false;
    size_t predicates=0,matches=0,total=0;
    for(const auto&x:a){if(present(x))++total;if(std::find(c.begin(),c.end(),x)!=c.end())continue;++predicates;if(present(x))++matches;}
    // For a distinct subject in the action, require its complete content phrase;
    // loose two-anchor overlap is not sufficient to invent successful follow-through.
    return predicates>0&&matches>0&&(bound||total==a.size());
}
struct ConversationalAct {std::string name="answer";bool required_question=false;};
inline ConversationalAct conversational_act(const std::string&text){
    for(const auto&c:clauses(text))if(c.direct()){
        const auto&t=c.low;
        if(has(t,"ask me")||has(t,"ask three questions")||has(t,"three questions"))return {"requested questions",true};
        if(has(t,"explain")||has(t,"walk me through")||has(t,"in detail")||has(t,"tell me the story"))return {"requested explanation",false};
        if(has(t,"correction")||has(t,"i meant")||has(t,"speech recognition")||has(t,"i said"))return {"repair",false};
        if(has(t,"write a poem")||has(t,"tell a story")||has(t,"perform"))return {"creative exploration",false};
        if(has(t,"hello")||has(t,"good morning"))return {"greeting",false};
    }return {};
}
struct SpeechShape {size_t question_marks=0;bool ends_question=false;};
inline SpeechShape speech_shape(const std::string&text){
    SpeechShape s;s.question_marks=(size_t)std::count(text.begin(),text.end(),'?');
    const auto end=text.find_last_not_of(" \t\r\n\"'");s.ends_question=end!=std::string::npos&&text[end]=='?';return s;
}
inline bool deferral(const std::string&s){
    for(const auto &c:clauses(s))if(c.direct()){
        std::string t=c.low;
        for(const char*p:{"well, ","no, ","sorry, ","maybe "})if(t.rfind(p,0)==0)t=trim(t.substr(std::char_traits<char>::length(p)));
        if(t=="later"||t=="not now"||t=="not right now"||t=="another time"||t=="maybe later"||t=="rather not"||t=="not tonight"||t=="not today")return true;
    }return false;
}
inline bool terminal(const std::string&s,const std::string&bot){
    for(const auto &c:clauses(s))if(c.direct()){
        std::string t=c.low;
        // Only conventional ASR wrappers, preserving meaningful bracketed words.
        for(char &ch:t)if(ch=='['||ch==']'||ch=='('||ch==')'||ch==','||ch=='-')ch=' ';
        std::string norm;bool space=true;for(char ch:t){if(std::isspace((unsigned char)ch)){if(!space)norm+=' ';space=true;}else{norm+=ch;space=false;}}t=trim(norm);
        if(t.rfind("okay ",0)==0)t=t.substr(5);
        if(t.rfind("ok ",0)==0)t=t.substr(3);
        if(t.rfind("well ",0)==0)t=t.substr(5);
        for(const char *g:{"goodbye ","good bye ","bye "}){
            const std::string prefix=g;if(t.rfind(prefix,0)!=0)continue;
            const auto rest=trim(t.substr(prefix.size()));const auto name=lower(bot);
            if(rest==name||rest==name+" for now"||rest==name+" talk to you later")return true;
        }
    }return false;
}
} // namespace aintent

namespace aintent {
// r24.22 review reconstruction: the quantity immediately governing a unit
// includes ordinary compound numbers and fractions. No substring of thirteen
// or thirty-five becomes three/five, and half a minute is not one minute.
inline std::optional<double> duration_amount(std::string t){
    t=lower(t);const auto first=t.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};
    t=t.substr(first,t.find_last_not_of(" \t\r\n")-first+1);if(t.front()=='-')return {};
    if(std::all_of(t.begin(),t.end(),[](char c){return (c>='0'&&c<='9')||c=='.';})){
        char *end=nullptr;const double n=std::strtod(t.c_str(),&end);
        if(end==t.c_str()+t.size()&&end!=t.c_str()&&std::isfinite(n)&&n>=0)return n;
        return {};
    }
    for(char &c:t)if(c=='-')c=' ';
    std::vector<std::string> w;size_t p=0;
    while(p<t.size()){while(p<t.size()&&std::isspace((unsigned char)t[p]))++p;const auto a=p;while(p<t.size()&&!std::isspace((unsigned char)t[p]))++p;if(p>a)w.push_back(t.substr(a,p-a));}
    const std::vector<std::string> small={"zero","one","two","three","four","five","six","seven","eight","nine","ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen"};
    const std::vector<std::string> tens={"twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"};
    auto number=[&](const std::string&v)->int{if(v=="a"||v=="an")return 1;auto s=std::find(small.begin(),small.end(),v);if(s!=small.end())return (int)(s-small.begin());auto q=std::find(tens.begin(),tens.end(),v);return q==tens.end()?-1:20+10*(int)(q-tens.begin());};
    if(w.size()==1&&number(w[0])>=0)return number(w[0]);
    if(w.size()==2&&number(w[0])>=20&&number(w[1])>0&&number(w[1])<10)return number(w[0])+number(w[1]);
    for(const auto&f:std::vector<std::pair<std::string,double>>{{"half",.5},{"quarter",.25}}){
        for(const auto&s:{f.first,f.first+" a",f.first+" an",f.first+" of a",f.first+" of an","a "+f.first,"a "+f.first+" of a","a "+f.first+" of an"})if(t==s)return f.second;
        const std::string suffix=" and a "+f.first;
        if(t.size()>suffix.size()&&t.compare(t.size()-suffix.size(),suffix.size(),suffix)==0){
            const auto whole=duration_amount(t.substr(0,t.size()-suffix.size()));if(whole)return *whole+f.second;
        }
    }
    if(w.size()>=2&&w[1]=="hundred"&&number(w[0])>0&&number(w[0])<10){
        if(w.size()==2)return number(w[0])*100;
        size_t k=w[2]=="and"?3:2;std::string rest;
        for(;k<w.size();++k){if(!rest.empty())rest+=' ';rest+=w[k];}
        const auto tail=duration_amount(rest);if(tail&&*tail<100)return number(w[0])*100+*tail;
    }
    return {};
}
inline std::optional<double> duration(const std::string&raw){
    const auto low=lower(raw);
    const std::vector<std::pair<std::string,double>> units={{"second",1},{"seconds",1},{"sec",1},{"minute",60},{"minutes",60},{"hour",3600},{"hours",3600}};
    struct Part { size_t begin,end;double seconds,unit; };
    std::vector<Part> parts;
    for(const auto&u:units){size_t at=0;while((at=low.find(u.first,at))!=std::string::npos){
        const size_t p=at++;if(p==0||word(low[p-1])||(p+u.first.size()<low.size()&&word(low[p+u.first.size()])))continue;
        size_t e=p;while(e&&std::isspace((unsigned char)low[e-1]))--e;
        std::vector<size_t> starts;size_t a=e;
        for(int n=0;n<6&&a;n++){
            while(a&&(word(low[a-1])||low[a-1]=='.'))--a;
            starts.push_back(a);if(!a||!std::isspace((unsigned char)low[a-1]))break;
            while(a&&std::isspace((unsigned char)low[a-1]))--a;
        }
        for(auto i=starts.rbegin();i!=starts.rend();++i){
            const auto q=duration_amount(low.substr(*i,e-*i));
            if(q&&std::isfinite(*q*u.second)){parts.push_back({*i,p+u.first.size(),*q*u.second,u.second});break;}
        }
    }}
    if(parts.empty())return {};
    std::sort(parts.begin(),parts.end(),[](const Part&a,const Part&b){return a.begin<b.begin;});
    double total=parts.front().seconds;size_t end=parts.front().end;double unit=parts.front().unit;
    // Only adjacent components of one duration are added. A later time in a
    // different clause, an alternative, or a conditional remains separate.
    for(size_t i=1;i<parts.size();++i){
        const auto&p=parts[i];if(p.begin<end)continue;
        const auto between=low.substr(end,p.begin-end);
        const auto begin_join=between.find_first_not_of(" \t\r\n,");
        const auto join=begin_join==std::string::npos?std::string():between.substr(begin_join,between.find_last_not_of(" \t\r\n,")-begin_join+1);
        if(!join.empty()&&join!="and")break;
        if(!std::isfinite(total+p.seconds))return {};
        total+=p.seconds;end=p.end;unit=p.unit;
    }
    // Ordinary post-unit fractions: "a minute and a half". This does not
    // absorb a fraction with its own unit ("a minute and half an hour").
    const auto tail=trim(low.substr(end));
    for(const auto&f:std::vector<std::pair<std::string,double>>{{"and a half",.5},{"and a quarter",.25}})
        if(tail==f.first){if(!std::isfinite(total+f.second*unit))return {};total+=f.second*unit;break;}
    return total;
}
struct AvailabilityRequest {
    bool quiet=false, until_input=false, absent=false, reopen=false;
    std::optional<double> seconds;
};
// One addressee parser for quiet and initiative arrangements. Callers still
// require Clause::direct(); stripping a courtesy never makes a quote direct.
inline std::string addressed(std::string t,const std::string&bot) {
    for(int n=0;n<5;++n){
        const auto old=t;const auto name=lower(bot);
        if(!name.empty()&&t.rfind(name,0)==0&&t.size()>name.size()&&!word(t[name.size()]))t=trim(t.substr(name.size()));
        for(const char*p:{"please ","please, ","well, ","okay, ","ok, ","hey ","hey, ","just ","kindly ","could you ","would you ","can you ","will you ","i need you to ","i want you to ","i would like you to ","i'd like you to ","let's ","let us "})
            if(t.rfind(p,0)==0){t=trim(t.substr(std::char_traits<char>::length(p)));break;}
        if(t==old)break;
    }
    return t;
}
inline AvailabilityRequest availability_request(const std::string&s,const std::string&bot="Athena",bool narration_scope=false){
    AvailabilityRequest r;
    for(const auto&c:clauses(s))if(c.direct()){
        auto t=c.low;
        if(t.rfind("if ",0)==0||t.rfind("when ",0)==0||t.find("this function")!=std::string::npos)continue;
        // Scope an imperative to the addressee. A phrase occurring in a
        // quotation, third-party description or negated wish is not a hold.
        t=addressed(t,bot);
        auto begins=[&](const char*p){const size_t n=std::char_traits<char>::length(p);return t.compare(0,n,p)==0&&(t.size()==n||!word(t[n]));};
        bool q=false;std::optional<double> default_seconds;
        for(const char*p:{"work quietly","quiet for","be quiet","stay quiet","keep quiet","remain quiet","let me think","let me just think","give me a minute","give me a moment","give me a second","give me one sec","sit in silence","sit quietly","no need to talk","hold on","hang on"})q=q||begins(p);
        // Preserve the direct, ordinary holds supported by the legacy
        // conversational ingress. Source scoping still rejects narrated uses.
        for(const char*p:{"give me a sec","i need a minute","i need a moment","i need to think","need a minute","need a moment","need to think","let me sit with"})q=q||begins(p);
        for(const char*p:{"one sec","one second","one moment","a sec","a second","one minute"})q=q||t==p;
        for(const char*p:{"sit here in silence","just sit with you","just sit here with","enjoy the quiet","no words needed","be quiet together","stop talking for a while"})
            if(begins(p)){q=true;default_seconds=150;}
        for(const char*p:{"spend some time by yourself","spend time by yourself","hang out on your own","hang out by yourself","sit by yourself","sit with yourself","go wander","go and wander","wander for a bit","have some time to yourself","be by yourself","be on your own","have the room to yourself"})
            if(begins(p)){q=true;default_seconds=300;}
        for(const char*p:{"take some time","take a little time","take your time"})if(begins(p)){
            const auto tail=trim(t.substr(std::char_traits<char>::length(p)));
            bool alone=tail.empty();
            for(const char*a:{"by yourself","on your own","to yourself","in silence","quietly"})
                alone=alone||tail.rfind(a,0)==0;
            if(alone){q=true;default_seconds=300;}
        }
        for(const char*p:{"don't say anything","dont say anything","don't start a conversation"})if(begins(p)){
            const auto tail=trim(t.substr(std::char_traits<char>::length(p)));
            // A temporal hold governs when to speak. "Anything false" or
            // "anything about the party" restricts content, not all speech.
            bool hold=tail.empty();
            for(const char*pause:{"now","right now","yet","just yet","more","else","at all"})hold=hold||tail==pause;
            for(const char*when:{"for ","until ","till ","before ","unless ","while "})hold=hold||tail.rfind(when,0)==0;
            if(hold)q=true;
        }
        const bool quiet_quantity=has(t,"of quiet")||has(t,"of silence");
        if(quiet_quantity&&duration(t)){
            if(begins("give me")||begins("i need")||begins("i would like")||begins("i'd like")||begins("can i have"))q=true;
            const auto blank=t.find(' ');
            if(duration_amount(t.substr(0,blank)))q=true;
        }
        const bool away=has(t,"i'll step out")||has(t,"i will step out")||has(t,"i'm stepping out")||has(t,"i'll step away")||
            has(t,"i'm stepping away")||has(t,"i am stepping away")||has(t,"i will step away")||
            has(t,"i'll be back")||has(t,"i will be back")||has(t,"i'm going to leave")||has(t,"i am going to leave")||
            has(t,"i'm leaving")||has(t,"i'll leave you")||has(t,"i will leave you")||has(t,"be right back")||has(t,"i'll be gone")||
            (narration_scope&&(begins("i'm going out")||begins("i am going out")));
        if(q){r.quiet=true;const auto d=duration(t);if(d)r.seconds=d;else if(!r.seconds&&default_seconds)r.seconds=default_seconds;r.until_input=!r.seconds.has_value()||has(t,"until i speak")||has(t,"until i talk");}
        if(away){r.absent=true;r.seconds=duration(t);}
        if(has(t,"you can talk now")||has(t,"you may speak now")||has(t,"let's talk again")||has(t,"i'm back")||has(t,"i am back"))r.reopen=true;
    }return r;
}
struct InteractionAvailability {
    bool typed=false, until_input=false, absent=false;
    double quiet_until=-1e9;
    aev::SourceRef owner;
    bool quiet(double now)const{return until_input||now<quiet_until;}
    bool unsolicited(double now)const{return !absent&&!quiet(now);}
    void accept(const std::string&s,const aev::SourceRef&source,double now,const std::string&bot="Athena"){
        const bool same_person=owner.actor.empty()||owner.actor==source.actor;
        if(same_person&&owner.event.str()!=source.event.str()){until_input=false;absent=false;}
        const auto r=availability_request(s,bot);typed=true;
        if(r.reopen&&same_person){until_input=false;quiet_until=-1e9;absent=false;}
        if(r.quiet){owner=source;until_input=r.until_input;quiet_until=r.seconds?now+*r.seconds:-1e9;}
        if(r.absent){owner=source;absent=true;}
    }
};
enum class Narration { ORDINARY, LISTENING, AWAY, REVOKED };
struct NarrationRequest {
    Narration mode=Narration::ORDINARY;
    bool changed=false, returning=false;
    std::optional<double> seconds;
};
inline NarrationRequest narration_request(const std::string&s,const std::string&bot="Athena") {
    NarrationRequest r;
    const bool announced_away=availability_request(s,bot,true).absent;
    for(const auto&c:clauses(s))if(c.direct()){
        auto t=addressed(c.low,bot);
        auto begins=[&](const char*p){const size_t n=std::char_traits<char>::length(p);return t.compare(0,n,p)==0&&(t.size()==n||!word(t[n]));};
        if(begins("i'm back")||begins("i am back"))r.returning=true;
        if(begins("stop thinking aloud")||begins("stop talking to yourself")||begins("keep working quietly")){
            r.changed=true;r.mode=Narration::REVOKED;continue;
        }
        if(begins("talk normally again")||begins("talk again")||begins("resume our conversation")||begins("you can talk now")||begins("you may speak now")){
            r.changed=true;r.mode=Narration::ORDINARY;continue;
        }
        // Permission must govern her act at the start of this direct clause.
        // An away preface is allowed only with an explicit subsequent grant.
        bool away=announced_away;
        for(const char*p:{"while i'm out,", "while i am out,", "while i'm away,", "while i am away,"})
            if(t.rfind(p,0)==0){away=true;t=trim(t.substr(std::char_traits<char>::length(p)));break;}
        bool grant=false;
        for(const char*p:{"you can ","you may ","feel free to "})
            if(t.rfind(p,0)==0){grant=true;t=trim(t.substr(std::char_traits<char>::length(p)));break;}
        const bool action=begins("think aloud")||begins("keep thinking aloud")||begins("keep exploring this aloud")||begins("continue thinking aloud")||begins("keep talking without waiting for a reply");
        // An addressed imperative is also a request. Conditional permission
        // has no current effect until its condition is actually established.
        if(!grant&&!action)continue;
        if(action&&!has(t,"if")&&!has(t,"unless")){r.changed=true;r.mode=away?Narration::AWAY:Narration::LISTENING;r.seconds=duration(t);}
    }
    return r;
}
struct NarrationArrangement {
    Narration mode=Narration::ORDINARY;
    std::string id,actor;
    uint64_t epoch=0;
    double created=0,until=-1;
    bool active(double now)const{return (mode==Narration::LISTENING||mode==Narration::AWAY)&&(until<0||now<until);}
    bool away(double now)const{return active(now)&&mode==Narration::AWAY;}
    void accept(const std::string&s,const aev::SourceRef&source,double now,const std::string&bot) {
        if(!actor.empty()&&actor!=source.actor)return;
        const auto r=narration_request(s,bot);
        if(mode==Narration::AWAY){mode=Narration::ORDINARY;++epoch;} // actual human return
        if(r.changed){mode=r.mode;actor=source.actor;id=source.event.str();created=now;until=r.seconds?now+*r.seconds:-1;++epoch;}
    }
};
} // namespace aintent

namespace aintent {
struct RequestedAct { std::string cue,action; size_t begin=0,end=0; };
inline std::vector<RequestedAct> requested_acts(const std::string&text) {
    std::vector<RequestedAct> out;
    std::vector<Clause> pieces;
    for(const auto&cl:clauses(text))if(cl.direct()){
        std::string rawlow=cl.text;for(char&c:rawlow)if((unsigned char)c<128)c=(char)std::tolower((unsigned char)c);
        size_t from=0,at=0;
        while((at=rawlow.find(" and when ",at))!=std::string::npos){
            if(direct_at(text,cl.begin+at)){Clause part=cl;part.begin=cl.begin+from;part.end=cl.begin+at;part.text=cl.text.substr(from,at-from);part.low=lower(trim(part.text));pieces.push_back(part);from=at+5;}
            ++at;
        }
        Clause part=cl;part.begin=cl.begin+from;part.text=cl.text.substr(from);part.low=lower(trim(part.text));pieces.push_back(part);
    }
    for(const auto&cl:pieces){
        const auto &s=cl.low;
        size_t at=s.find("ask me ");size_t width=7;
        if(at==std::string::npos){at=s.find("remind me ");width=10;}
        if(at==std::string::npos){at=s.find("ask whether ");width=4;}
        if(at==std::string::npos){at=s.find("ask if ");width=4;}
        if(at==std::string::npos)continue;
        std::string original_low=cl.text;for(char&c:original_low)if((unsigned char)c<128)c=(char)std::tolower((unsigned char)c);
        const auto original_ask=original_low.find(s.substr(at,width));
        if(original_ask==std::string::npos||!direct_at(text,cl.begin+original_ask))continue;
        if(has(s,"don't ask")||has(s,"do not ask")||has(s,"she will ask")||has(s,"he will ask")||s.rfind("if you were",0)==0)continue;
        size_t when=s.find("next time ");size_t w=10;
        if(when==std::string::npos){when=s.find("when ");w=5;}
        if(when==std::string::npos){when=s.find("if i bring up ");w=3;}
        if(when==std::string::npos)continue;
        RequestedAct r;r.begin=cl.begin;r.end=cl.end;
        if(when<at){r.cue=trim(s.substr(when+w,at-when-w));r.action=trim(s.substr(at+width));}
        else{r.action=trim(s.substr(at+width,when-at-width));r.cue=trim(s.substr(when+w));}
        for(const char*p:{"we come back to ","we return to ","i mention ","i next mention ","i bring up ","we discuss ","we talk about ","the subject is ","the topic is "})
            if(r.cue.rfind(p,0)==0)r.cue=trim(r.cue.substr(std::char_traits<char>::length(p)));
        if(r.cue.size()>3&&r.cue.compare(r.cue.size()-3,3,", i")==0)r.cue=trim(r.cue.substr(0,r.cue.size()-3));
        if(r.cue.size()>6&&r.cue.compare(r.cue.size()-6,6,"please")==0)r.cue=trim(r.cue.substr(0,r.cue.size()-6));
        for(const char*p:{"the ","a ","an "})if(r.cue.rfind(p,0)==0)r.cue=r.cue.substr(std::char_traits<char>::length(p));
        for(const char*p:{" comes up again"," comes up"," again"}){const std::string tail=p;if(r.cue.size()>tail.size()&&r.cue.compare(r.cue.size()-tail.size(),tail.size(),tail)==0)r.cue=trim(r.cue.substr(0,r.cue.size()-tail.size()));}
        // The pronoun belongs to this request's cue, never the neighboring task.
        for(const auto&binding:std::vector<std::pair<std::string,std::string>>{{"its ","the "+r.cue+"'s "},{"it ","the "+r.cue+" "}}){
            size_t pos=0;while((pos=r.action.find(binding.first,pos))!=std::string::npos){
                if(pos==0||!word(r.action[pos-1])){r.action.replace(pos,binding.first.size(),binding.second);pos+=binding.second.size();}else ++pos;
            }
        }
        if(!r.cue.empty()&&!r.action.empty()&&r.cue.size()<=256&&r.action.size()<=1024)out.push_back(r);
    }return out;
}
} // namespace aintent
