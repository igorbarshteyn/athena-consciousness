// r24.22: recoverable album writes and exact label/source selection.
#pragma once
#include "athena_vision.h"
#include "athena_qualified.h"
namespace aalb {
inline std::string normalize_label(const std::string&s) {
    std::string n=aqual::norm(s),out;
    for(unsigned char c:n)if(std::isalnum(c)||c>=128)out+=(char)c;
    // This folded key is only used inside a uniquely bound collection; it is
    // never global stemming or an undocumented alias between two collections.
    return out;
}
inline bool collection_mentioned(const std::string&query,const std::string&collection) {
    const auto wanted=normalize_label(collection);if(wanted.empty())return false;
    std::istringstream in(aqual::norm(query));std::vector<std::string> words;std::string w;
    while(in>>w)words.push_back(normalize_label(w));
    for(size_t i=0;i<words.size();++i){std::string joined;for(size_t k=i;k<words.size()&&k<i+8;++k){joined+=words[k];if(joined==wanted)return true;if(joined.size()>wanted.size())break;}}
    return false;
}
inline std::string collection_of(const std::string&label) {
    std::istringstream words(aqual::norm(label));std::string w,out;
    for(;words>>w;){bool generic=false;for(const char*x:{"our","the","a","first","second","third","fourth","new","old","picture","image","photo","photograph","arrangement","version"})if(w==x)generic=true;
        if(!generic)out+=(out.empty()?"":" ")+w;}
    return out;
}
struct Metadata { std::string id,file,label,collection,source;long born=0; };
inline std::vector<Metadata> metadata(const acont::Journal&j) {
    std::vector<Metadata> out;
    for(const auto&kv:j.records())if(kv.second.kind=="album-label"&&j.records().count(kv.second.get("projection")+"/commit")){
        const auto&r=kv.second;Metadata m;m.id=r.parent;m.file=r.get("file");m.label=r.get("label");m.collection=r.get("collection");m.source=r.get("source");
        try{m.born=std::stol(r.get("born"));}catch(...){}out.push_back(m);}
    return out;
}
inline bool keep(aseam::VisionRig&vision,acont::Journal&j,const std::string&dir,
                 const std::string&gist,const std::string&label,std::string&file) {
    file.clear();if(dir.empty()||!vision.held().valid())return false;
    const auto id=vision.keep_identity();const auto held=vision.held();const auto tx=id+"/projection";
    if(!j.flush()||!acont::recover_projections(j,dir))return false;
    auto old=j.records().find(id+"/label");
    auto prepared=j.records().find(tx);
    if(old!=j.records().end()&&prepared!=j.records().end()) {
        if(!acont::finish_projection(j,dir,prepared->second))return false;
        file=old->second.get("file");
    } else {
        std::string pixels;bool exists=false;if(!acont::read_exact(held.jpeg,pixels,exists)||!exists||pixels.empty())return false;
        const auto fname="ks-"+std::to_string((long)held.taken_at)+"-"+acont::checksum(id)+".jpg";
        if(old!=j.records().end()){
            // Older releases could commit the label before its prepare. Retry
            // only against that exact still-held source, never a newer image.
            if(old->second.kind!="album-label"||old->second.parent!=id||old->second.get("source")!=held.source_id||
               old->second.get("projection")!=tx||old->second.get("file")!=fname)return false;
            std::string existing;
            if(!acont::read_exact(amem::join_path(dir,"keepsakes/"+fname),existing,exists)||exists)return false;
        }
        const auto ksdir=amem::join_path(dir,"keepsakes");if(::mkdir(ksdir.c_str(),0700)!=0&&errno!=EEXIST)return false;
        std::string oldrows;if(!acont::read_exact(amem::join_path(dir,"keepsakes.tsv"),oldrows,exists))return false;
        if(!oldrows.empty()&&oldrows.back()!='\n')oldrows+='\n';
        long born=(long)time(nullptr);
        if(old!=j.records().end()){
            const auto b=old->second.get("born");size_t end=0;
            try{born=std::stol(b,&end);}catch(...){return false;}
            if(end!=b.size()||born<0)return false;
        }
        const auto saved_caption=old==j.records().end()?std::string():old->second.get("caption");
        const auto caption=saved_caption.empty()?amem::scrub_gist(gist.empty()?"a moment I chose to keep":gist):saved_caption;
        const auto rows=oldrows+std::to_string(born)+"\t"+fname+"\t"+caption+"\t"+std::to_string((int)aseam::keepsake_family(caption))+"\n";
        const auto label_record=old==j.records().end()?acont::Record{"album-label",id+"/label",id,{{"file",fname},{"born",std::to_string(born)},
            {"label",label},{"collection",collection_of(label)},{"source",held.source_id},{"projection",tx},{"caption",caption}}}:old->second;
        // The exact label is part of prepare, so interrupted completion can
        // recover it together with the pixels and index, without a second take.
        if(!acont::project(j,dir,tx,{{"keepsakes/"+fname,pixels},{"keepsakes.tsv",rows}},"",id,{},&label_record))return false;
        file=fname;
    }
    vision.note_look_kept(file,held.jpeg);
    (void)vision.take_held();std::remove(held.jpeg.c_str());return true;
}
struct Selection { bool constrained=false,ambiguous=false;int index=-1;std::string reason; };
inline Selection select(const std::string&text,const std::vector<amem::Keepsake>&album,const acont::Journal&j) {
    Selection s;const auto q=aqual::norm(text);const auto meta=metadata(j);
    std::vector<size_t> matches;std::set<std::string> collections;
    for(size_t i=0;i<album.size();++i){
        if(aintent::has(q,aqual::norm(album[i].file))){s.constrained=true;s.index=(int)i;s.reason="stable file";return s;}
        for(const auto&m:meta)if(m.file==album[i].file&&m.born==album[i].born){
            auto label=aqual::norm(m.label);
            for(const char*prefix:{"our ","the "})if(label.rfind(prefix,0)==0)label=label.substr(std::char_traits<char>::length(prefix));
            if(!label.empty()&&aintent::has(q,label))matches.push_back(i);
            const auto c=normalize_label(m.collection);
            if(!c.empty()&&collection_mentioned(text,m.collection))collections.insert(m.collection);
        }
    }
    std::sort(matches.begin(),matches.end());matches.erase(std::unique(matches.begin(),matches.end()),matches.end());
    if(!matches.empty()){s.constrained=true;s.ambiguous=matches.size()!=1;s.index=s.ambiguous?-1:(int)matches[0];s.reason="exact label";return s;}
    if(collections.empty()){
        for(const char*marker:{"filed under ","labelled ","labeled ","named ","called "})if(aintent::has(q,aintent::trim(marker))){s.constrained=true;s.reason="explicit label absent";break;}
        return s;
    }
    s.constrained=true;if(collections.size()!=1){s.ambiguous=true;s.reason="competing collections";return s;}
    for(size_t i=0;i<album.size();++i)for(const auto&m:meta)
        if(m.file==album[i].file&&m.born==album[i].born&&m.collection==*collections.begin())matches.push_back(i);
    auto keep_order=[&](size_t i){std::string prefix=album[i].file;uint64_t sequence=0;
        for(const auto&m:meta)if(m.file==album[i].file&&m.born==album[i].born){const auto at=m.id.rfind('/');
            if(at!=std::string::npos){prefix=m.id.substr(0,at);try{sequence=std::stoull(m.id.substr(at+1));}catch(...){}}break;}
        return std::make_tuple(album[i].born,prefix,sequence,album[i].file);
    };
    std::sort(matches.begin(),matches.end(),[&](size_t a,size_t b){return keep_order(a)<keep_order(b);});
    matches.erase(std::unique(matches.begin(),matches.end()),matches.end());
    int rank=-1;for(const auto&v:std::vector<std::pair<std::string,int>>{{"first",0},{"second",1},{"third",2},{"fourth",3}})if(aintent::has(q,v.first))rank=v.second;
    if(rank>=0){s.index=(size_t)rank<matches.size()?(int)matches[(size_t)rank]:-1;s.reason=s.index>=0?"collection ordinal":"requested source absent";return s;}
    if(matches.size()==1)s.index=(int)matches[0];else s.ambiguous=true;s.reason="collection requires disambiguation";return s;
}
} // namespace aalb
