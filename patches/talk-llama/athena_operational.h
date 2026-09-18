// r24.22: explicit completed-action assertions need operational evidence.
#pragma once
#include "athena_album.h"
namespace aop {
enum class Claim { NONE,CAPTURE,VIEW,KEEP,RECALL,DELIVERY };
// A qualifier inside a quotation cannot suppress a later owned assertion.
// Keep the same word boundaries and source grammar as the action markers.
inline bool owned_has(const std::string&s,const std::string&w,size_t end=std::string::npos){
    for(size_t at=s.find(w);at!=std::string::npos&&at<end;at=s.find(w,at+1))
        if(at+w.size()<=end && (at==0||!aintent::word(s[at-1])) &&
           (at+w.size()==s.size()||!aintent::word(s[at+w.size()])) &&
           aintent::direct_at(s,at))return true;
    return false;
}
inline bool image_words(const std::string&text){
    const auto low=aintent::lower(text);
    for(const char*w:{"image","images","picture","pictures","photo","photos","photograph","photographs","frame","album","keepsake","keepsakes","camera"})if(aintent::has(low,w))return true;
    return false;
}
inline std::vector<Claim> claims(const std::string&text,const std::string&request="") {
    std::vector<Claim> out;
    std::vector<std::pair<Claim,std::vector<std::string>>> patterns={
      {Claim::KEEP,{"i saved","i've saved","i have saved","i just saved","i've just saved","i have just saved","i kept","i've kept","i have kept","i stored","i filed","it's saved in my album","i put it in my album"}},
      {Claim::RECALL,{"i opened","i've opened","i have opened","i just opened","i've just opened","i have just opened","i pulled up","i have both pictures open"}},
      {Claim::CAPTURE,{"i used the camera","i took a picture","i took the picture","i've taken a picture","i took a photo","i took a look","i took a fresh look","i have taken a look","i've taken a look","i just looked","i've just looked","i looked at","i have looked at","i've looked at"}},
      {Claim::VIEW,{"i'm looking at","i am looking at","i can see the picture","i have the image in front of me"}},
      {Claim::DELIVERY,{"you heard every word","you heard all of it","i finished my answer"}}
    };
    // Completed passive forms describe the same operation as first-person
    // forms. Pronouns and bare acknowledgments require the current image
    // request; saving a document or the user's own file is not our camera act.
    for(const char*n:{"the picture","the image","the photo","the photograph","the keepsake","it"}){
        if(std::string(n)=="it"&&!image_words(request))continue;
        for(const char*v:{" is saved"," was saved"," has been saved"," is now saved"," has been kept"," is in my album"," is now in my album"})patterns[0].second.push_back(std::string(n)+v);
        for(const char*v:{" is open"," is now open"," has been opened"," was opened"})patterns[1].second.push_back(std::string(n)+v);
    }
    auto add=[&](Claim c){if(std::find(out.begin(),out.end(),c)==out.end())out.push_back(c);};
    for(const auto&cl:aintent::clauses(text))if(cl.direct()&&cl.text.find('?')==std::string::npos){
        // A trailing epistemic qualification belongs to the same assertion.
        // It must not become a completed operation merely by using passive
        // voice or a short acknowledgment. A following sentence stays separate.
        const auto comma=cl.low.rfind(',');bool trailing_hedge=false;
        if(comma!=std::string::npos){const auto tail=aintent::trim(cl.low.substr(comma+1));
            for(const char*p:{"i think","i believe","perhaps","maybe","probably","as far as i know","but i'm not sure"})trailing_hedge=trailing_hedge||tail==p;}
        if(trailing_hedge)continue;
        bool external=false;
        for(const char*p:{"on your computer","to your computer","on your phone","to your phone","in your folder","to your folder","by you","by him","by her","by them"})external=external||owned_has(cl.low,p);
        // Source ownership belongs to each occurrence. An earlier quotation
        // of an operation must not hide a later direct assertion of that same
        // operation from the publication gate.
        for(const auto&group:patterns)for(const auto&w:group.second)
          for(size_t at=cl.low.find(w);at!=std::string::npos;at=cl.low.find(w,at+1)){
            if((at&&aintent::word(cl.low[at-1]))||
               (at+w.size()<cl.low.size()&&aintent::word(cl.low[at+w.size()]))||
               !aintent::direct_at(cl.low,at))continue;
            const auto tail=cl.low.substr(at+w.size());
            bool uncertain=false;
            for(const char*p:{"if","suppose","whether","maybe","perhaps","i think","i believe","i don't think","i do not think","i don't believe","i do not believe","i never said","i didn't say","i did not say","i haven't said","i have not said","not sure","uncertain","might","may have","probably"})uncertain=uncertain||owned_has(cl.low,p,at);
            if(uncertain)continue;
            if(tail.rfind(" nothing",0)==0||tail.rfind(" no ",0)==0)continue;
            // Grammatical "saved" is not automatically a camera operation.
            if(group.first==Claim::KEEP||group.first==Claim::RECALL){
                if(external)continue;
                bool image=image_words(w+tail);
                for(const char*n:{"it","that","this","one","both"})image=image||(image_words(request)&&aintent::has(w+tail,n));
                if(!image)continue;
            }
            if(group.first==Claim::CAPTURE&&w.find("look")!=std::string::npos){
                bool abstract=false;for(const char*n:{"problem","idea","option","question","possibility","argument","solution"})abstract=abstract||aintent::has(tail,n);
                if(abstract&&!image_words(tail))continue;
                // Looking again at an explicitly named album image requires
                // that recalled source. It is not evidence of a fresh capture.
                const bool fresh=aintent::has(w+tail,"fresh")||aintent::has(w+tail,"camera")||aintent::has(w+tail,"live");
                const bool album=aintent::has(tail,"album")||aintent::has(tail,"keepsake")||aintent::has(tail,"keepsakes");
                if(album&&!fresh){add(Claim::RECALL);continue;}
            }

            add(group.first);
        }
        auto bare=aintent::trim(cl.low);
        if(!bare.empty()&&(bare.front()=='['||bare.front()=='<'))bare=aintent::trim(bare.substr(1));
        if(!bare.empty()&&(bare.back()==']'||bare.back()=='>'))bare=aintent::trim(bare.substr(0,bare.size()-1));
        if(image_words(request)&&!external){
            for(const char*p:{"kept","saved","stored","filed","kept it","saved it","saved in my album"})if(bare==p)add(Claim::KEEP);
            for(const char*p:{"opened","opened it","both are open","both pictures are open"})if(bare==p)add(Claim::RECALL);
        }
        if(!external&&image_words(bare)){
            for(const char*p:{"saved ","kept ","stored ","filed "})if(bare.rfind(p,0)==0)add(Claim::KEEP);
            for(const char*p:{"opened ","pulled up "})if(bare.rfind(p,0)==0)add(Claim::RECALL);
        }
    }return out;
}
inline Claim classify(const std::string&text,const std::string&request=""){const auto found=claims(text,request);return found.empty()?Claim::NONE:found.front();}
inline bool temporal_match(const std::string&low,long then,long now) {
    if(then<=0||then>now)return false;
    if(aintent::has(low,"yesterday")){
        time_t t=(time_t)now;struct tm today{};localtime_r(&t,&today);today.tm_hour=today.tm_min=today.tm_sec=0;today.tm_isdst=-1;
        const auto end=::mktime(&today);today.tm_mday-=1;today.tm_isdst=-1;const auto start=::mktime(&today);
        return then>=start&&then<end;
    }
    if(aintent::has(low,"last week"))return now-then>=6*86400L&&now-then<=14*86400L;
    if(aintent::has(low,"earlier")||aintent::has(low,"last time"))return then<=now;
    return now-then<=120;
}
struct Verdict { bool supported=true;Claim claim=Claim::NONE;std::string actual; };
inline Verdict evaluate_one(const std::string&text,const aseam::VisionRig&vision,const acont::Journal&j,long now,bool prior_delivery_complete,Claim claim,const std::string&request="") {
    Verdict v;v.claim=claim;if(v.claim==Claim::NONE)return v;
    const auto low=aintent::lower(text);const bool historical=aintent::has(low,"earlier")||aintent::has(low,"yesterday")||aintent::has(low,"last time")||aintent::has(low,"last week");
    v.supported=false;
    if(v.claim==Claim::DELIVERY){v.supported=prior_delivery_complete&&!aintent::has(low,"you heard");v.actual="I don't have confirmation that every word of that answer reached you.";return v;}
    if(v.claim==Claim::KEEP){
        const auto saved=aalb::metadata(j);std::vector<amem::Keepsake> album;
        for(const auto&m:saved){amem::Keepsake k;k.file=m.file;k.born=m.born;k.gist=m.label;album.push_back(k);}
        auto selected=aalb::select(text,album,j);
        if(!selected.constrained&&!request.empty())selected=aalb::select(request,album,j);
        for(size_t i=0;i<saved.size();++i){const auto&m=saved[i];
            if(selected.constrained && selected.index!=(int)i)continue;
            if(!historical && vision.held().valid() && vision.held().source_id!=m.source)continue;
            if(temporal_match(low,m.born,now)){v.supported=true;break;}
        }
        v.actual="I don't have a completed save for that image.";return v;
    }
    std::string requested_file;bool constrained=false;
    if(v.claim==Claim::RECALL||v.claim==Claim::VIEW){std::vector<amem::Keepsake> album;for(const auto&m:aalb::metadata(j)){amem::Keepsake k;k.file=m.file;k.born=m.born;k.gist=m.label;album.push_back(k);}
        auto selected=aalb::select(text,album,j);
        if(!selected.constrained&&!request.empty())selected=aalb::select(request,album,j);
        constrained=selected.constrained;
        if(selected.index>=0)requested_file=album[(size_t)selected.index].file;
    }
    std::set<std::string> opened;
    for(const auto&a:vision.action_history){
        const bool fresh=temporal_match(low,a.wall,now);
        if(constrained&&(requested_file.empty()||a.action.target!=requested_file))continue;
        if(v.claim==Claim::CAPTURE && !a.recalled&&a.captured&&fresh&&((aintent::has(low,"look")||aintent::has(low,"looked"))?a.admitted_once:true))v.supported=true;
        if(v.claim==Claim::VIEW && a.admitted){
            const bool fresh_required=aintent::has(low,"fresh")||aintent::has(low,"camera")||aintent::has(low,"live");
            if(!fresh_required||!a.recalled)v.supported=true;
        }
        if(v.claim==Claim::RECALL && a.recalled && a.admitted)opened.insert(a.action.target);
    }
    if(historical&&v.claim==Claim::CAPTURE)for(const auto&kv:j.records())if(kv.second.kind=="vision-action"){
        const auto&r=kv.second;long when=0;try{when=std::stol(r.get("requested_wall"));}catch(...){}
        if(r.get("captured")=="1"&&r.get("recalled")=="0"&&temporal_match(low,when,now)&&
           (!(aintent::has(low,"look")||aintent::has(low,"looked"))||r.get("admitted_once")=="1"))v.supported=true;
    }
    if(v.claim==Claim::RECALL)v.supported=opened.size()>=(aintent::has(low,"both")?2u:1u);
    v.actual=v.claim==Claim::CAPTURE?"I don't have a completed fresh camera result for that.":
        v.claim==Claim::VIEW?"I don't have that image available in my current context.":"I haven't opened the requested album source.";
    return v;
}
inline Verdict evaluate(const std::string&text,const aseam::VisionRig&vision,const acont::Journal&j,long now,bool prior_delivery_complete,const std::string&request="") {
    for(const auto&cl:aintent::clauses(text))for(const auto claim:claims(cl.text,request)){
        const auto v=evaluate_one(cl.text,vision,j,now,prior_delivery_complete,claim,request);if(!v.supported)return v;
    }
    return {};
}
} // namespace aop
