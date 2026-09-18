// r24.22: main-loop-owned source journal. Legacy narrative stores remain canonical.
#pragma once
#include "athena_evidence.h"
#include "athena_memory.h"
#include <map>
#include <functional>
#include <random>
#include <chrono>
#include <iomanip>
#include <memory>
#include <limits>
#if !defined(_WIN32)
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace acont {
inline std::string hex(const std::string &s) {
    static const char *d="0123456789abcdef"; std::string r; r.reserve(s.size()*2);
    for(unsigned char c:s){r+=d[c>>4];r+=d[c&15];} return r;
}
inline bool unhex(const std::string&s,std::string&r){
    r.clear();if(s.size()%2)return false;
    auto n=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    for(size_t i=0;i<s.size();i+=2){int a=n(s[i]),b=n(s[i+1]);if(a<0||b<0)return false;r+=char(a*16+b);}return true;
}
inline std::string checksum(const std::string&s){
    uint64_t h=14695981039346656037ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}
    std::ostringstream o;o<<std::hex<<h;return o.str(); // integrity checksum, not authentication
}
inline std::string escape(const std::string&s){
    std::string o;for(char c:s){switch(c){case '\\':o+="\\\\";break;case '\t':o+="\\t";break;case '\n':o+="\\n";break;case '\r':o+="\\r";break;default:o+=c;}}return o;
}
inline bool unescape(const std::string&s,std::string&o){
    o.clear();for(size_t i=0;i<s.size();++i){if(s[i]!='\\'){o+=s[i];continue;}
        if(++i==s.size())return false;
        switch(s[i]){case '\\':o+='\\';break;case 't':o+='\t';break;case 'n':o+='\n';break;case 'r':o+='\r';break;default:return false;}}
    return true;
}
inline std::vector<std::string> columns(const std::string&s){
    std::vector<std::string> v;size_t a=0;for(;;){size_t b=s.find('\t',a);v.push_back(s.substr(a,b==std::string::npos?b:b-a));if(b==std::string::npos)break;a=b+1;}return v;
}
struct Record {
    std::string kind,id,parent;
    std::map<std::string,std::string> fields;
    std::string encode() const {
        std::string r=escape(kind)+"\t"+escape(id)+"\t"+escape(parent);
        for(const auto &f:fields) { r+='\t'+escape(f.first)+"\t"+escape(f.second); }
        return r;
    }
    static bool decode(const std::string&s,Record&r){
        auto c=columns(s);if(c.size()<3||c.size()%2!=1)return false;
        Record out;if(!unescape(c[0],out.kind)||!unescape(c[1],out.id)||!unescape(c[2],out.parent)||out.id.empty())return false;
        for(size_t i=3;i<c.size();i+=2){std::string k,v;if(!unescape(c[i],k)||!unescape(c[i+1],v)||!out.fields.emplace(k,v).second)return false;}
        r=std::move(out);return true;
    }
    std::string get(const std::string&k)const{auto i=fields.find(k);return i==fields.end()?"":i->second;}
};
inline double mono(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline std::string number(double x){std::ostringstream o;o<<std::setprecision(17)<<x;return o.str();}
inline std::string new_session(){
    std::random_device rd;std::ostringstream o;o<<std::hex;
    for(int i=0;i<4;++i)o<<std::setw(8)<<std::setfill('0')<<rd();
    return o.str();
}
// Serialized fragments never exceed the legacy 65536-byte line limit. A commit
// references the complete checksummed payload; a torn/uncommitted source cannot
// enter retrieval. Retry uses the same transaction identity. Unknown rows remain
// byte-for-byte on disk because this writer never rewrites the journal.
class Journal {
    std::string path_;
    std::map<std::string,Record> records_;
    std::vector<Record> pending_;
    std::string session_=new_session();
    uint64_t next_=0;
    bool loaded_=true;
    static constexpr size_t fragment_bytes=12000;
    static constexpr size_t max_record_bytes=16*1024*1024;
public:
    explicit Journal(const std::string&dir="") {if(!dir.empty())path_=amem::join_path(dir,"continuity.v1.tsv");}
    aev::EventId next(){return {session_,++next_};}
    const std::map<std::string,Record>&records()const{return records_;}
    const std::vector<Record>&pending()const{return pending_;}
    bool load(){
        if(path_.empty())return true;
        std::ifstream f(path_,std::ios::binary);
        if(!f){loaded_=errno==ENOENT;return loaded_;}
        std::string line,tx,payload,digest;size_t sequence=0;
        bool over=false, terminated=false;
        while(amem::read_store_line_(f,line,over,terminated)){
            if(over || !terminated){tx.clear();payload.clear();continue;}
            auto c=columns(line);
            if(c.size()==4&&c[0]=="1"&&c[1]=="prepare") {tx=c[2];digest=c[3];payload.clear();sequence=0;continue;}
            if(c.size()==5&&c[0]=="1"&&c[1]=="fragment"&&c[2]==tx&&!tx.empty()){
                std::string part;
                if(c[3]!=std::to_string(sequence)||!unhex(c[4],part)||payload.size()+part.size()>max_record_bytes){tx.clear();payload.clear();continue;}
                payload+=part;++sequence;continue;
            }
            if(c.size()==4&&c[0]=="1"&&c[1]=="commit"&&c[2]==tx&&!tx.empty()){
                Record r;
                if(c[3]==digest&&checksum(payload)==digest&&Record::decode(payload,r)&&r.id.size()<=256){
                    auto it=records_.find(r.id);
                    if(it==records_.end())records_.emplace(r.id,std::move(r));
                    else if(it->second.encode()!=payload){loaded_=false;return false;}
                }
                tx.clear();payload.clear();
            }
        }
        loaded_=!f.bad();return loaded_;
    }
    bool enqueue(const Record&r){
        if(r.id.empty()||r.id.size()>256||r.encode().size()>max_record_bytes)return false;
        auto i=records_.find(r.id);if(i!=records_.end())return i->second.encode()==r.encode();
        for(const auto&p:pending_)if(p.id==r.id)return p.encode()==r.encode();
        pending_.push_back(r);return true;
    }
    static std::string transaction(const Record&r){
        const std::string p=r.encode(),d=checksum(p),tx=hex(r.id);
        // Leading newline isolates any incomplete final line from a failed write.
        std::string out="\n1\tprepare\t"+tx+"\t"+d+"\n";
        size_t n=0;for(size_t b=0;b<p.size();b+=fragment_bytes)
            out+="1\tfragment\t"+tx+"\t"+std::to_string(n++)+"\t"+hex(p.substr(b,fragment_bytes))+"\n";
        return out+"1\tcommit\t"+tx+"\t"+d+"\n";
    }
    bool flush(){
        if(!loaded_)return false;
        if(pending_.empty())return true;
        if(path_.empty()){for(auto&r:pending_) { records_[r.id]=r; }
        pending_.clear();return true;}
#if !defined(_WIN32)
        int fd=::open(path_.c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return false;
        if(::flock(fd,LOCK_EX|LOCK_NB)!=0){::close(fd);return false;}
        bool ok=true;
        for(const auto&r:pending_){const auto wire=transaction(r);size_t n=0;
            while(n<wire.size()){ssize_t w=::write(fd,wire.data()+n,wire.size()-n);if(w<0&&errno==EINTR)continue;if(w<=0){ok=false;break;}n+=size_t(w);}if(!ok)break;}
        if(ok&&::fsync(fd)!=0)ok=false;
        if(::close(fd)!=0)ok=false;
        const size_t slash=path_.find_last_of('/');const std::string dir=slash==std::string::npos?".":path_.substr(0,slash);
        int dfd=::open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(dfd<0)ok=false;else{if(::fsync(dfd)!=0)ok=false;if(::close(dfd)!=0)ok=false;}
        if(!ok)return false;
#else
        return false; // supported release target is Linux; never claim durable storage here
#endif
        for(auto&r:pending_) { records_[r.id]=r; }
        pending_.clear();return true;
    }
    bool input(const aev::InputEvidence&e){
        Record r;r.kind="input";r.id=e.source.event.str();r.parent=e.source.parent;
        r.fields={{"actor",e.source.actor},{"producer",e.source.producer},{"lexical_hex",hex(e.lexical)},
                  {"raw_asr_hex",hex(e.original_asr)},{"asr_transform",e.asr_transform},{"version",std::to_string(e.source.version)},
                  {"origin",std::to_string(int(e.origin))},{"wall",std::to_string(e.wall_time)},
                  {"emotion",e.acoustic.raw_tag},{"calibration",e.acoustic.calibration_id}};
        for(size_t n=0;n<e.asr_events.size();++n)r.fields["asr_event."+std::to_string(n)]=e.asr_events[n];
        r.fields["tone_corrected"]=e.acoustic.explicitly_corrected?"1":"0";
        if(e.acoustic.posterior_available) {
            for(size_t i=0;i<e.acoustic.posterior.size();++i)r.fields["posterior."+std::to_string(i)]=number(e.acoustic.posterior[i]);
            r.fields["probability"]=number(e.acoustic.probability);r.fields["share"]=number(e.acoustic.share);r.fields["evidence"]=number(e.acoustic.evidence);
        }
        if(e.acoustic.calibrated_reliability)r.fields["reliability"]=number(*e.acoustic.calibrated_reliability);
        for(size_t i=0;i<e.capture.size();++i){const auto &c=e.capture[i];const std::string k="capture."+std::to_string(i)+".";
            if(c.first_sample)r.fields[k+"first"]=std::to_string(*c.first_sample);
            if(c.end_sample)r.fields[k+"end"]=std::to_string(*c.end_sample);
            if(c.start_mono)r.fields[k+"start_mono"]=number(*c.start_mono);
            if(c.end_mono)r.fields[k+"end_mono"]=number(*c.end_mono);
            r.fields[k+"publication_cursor"]=c.publication_cursor?"1":"0";}
        if(e.measured_gap)r.fields["gap"]=number(*e.measured_gap);
        if(e.decoded_mono)r.fields["decoded"]=number(*e.decoded_mono);
        if(e.admitted_mono)r.fields["admitted"]=number(*e.admitted_mono);
        for(size_t i=0;i<e.blocking.size();++i){const auto &b=e.blocking[i];const std::string k="block."+std::to_string(i)+".";
            unsigned relation=0;for(const auto&span:e.capture)relation|=span.relative_to(b.capture);
            r.fields[k+"relation"]=std::to_string(relation);
            r.fields[k+"id"]=b.id;r.fields[k+"kind"]=std::to_string(int(b.kind));r.fields[k+"outcome"]=b.outcome;
            if(b.capture.first_sample)r.fields[k+"first_sample"]=std::to_string(*b.capture.first_sample);
            if(b.capture.end_sample)r.fields[k+"end_sample"]=std::to_string(*b.capture.end_sample);
            r.fields[k+"acknowledgement"]=b.acknowledgement;
            if(b.capture.start_mono)r.fields[k+"start"]=number(*b.capture.start_mono);
            if(b.capture.end_mono)r.fields[k+"end"]=number(*b.capture.end_mono);}
        return enqueue(r);
    }
};

inline bool read_exact(const std::string&path,std::string&bytes,bool&exists) {
    errno=0;std::ifstream f(path,std::ios::binary);exists=bool(f);
    if(!f){bytes.clear();return errno==ENOENT;}
    std::ostringstream o;o<<f.rdbuf();if(f.bad()||o.bad())return false;bytes=o.str();return true;
}
inline bool durable_write(const std::string&path,const std::string&bytes) {
    if(!amem::atomic_write(path,bytes))return false;
#if !defined(_WIN32)
    int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    bool ok=::fsync(fd)==0;if(::close(fd)!=0)ok=false;
    const auto slash=path.find_last_of('/');const auto dir=slash==std::string::npos?std::string("."):path.substr(0,slash);
    fd=::open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return false;
    if(::fsync(fd)!=0)ok=false;
    if(::close(fd)!=0)ok=false;
    return ok;
#else
    return false;
#endif
}
inline bool projection_name(const std::string&p) {
    for(const char*n:{"memory.state.tsv","memory.txt","personality.ledger","personality.txt","keepsakes.tsv","people.tsv"})if(p==n)return true;
    if(p.rfind("keepsakes/ks-",0)==0&&p.size()<160&&p.size()>16&&p.substr(p.size()-4)==".jpg") {
        const auto name=p.substr(10,p.size()-14);return std::all_of(name.begin(),name.end(),[](char c){return std::isalnum((unsigned char)c)||c=='-';});
    }
    return false;
}
inline bool safe_projection_target(const std::string&dir,const std::string&name) {
#if !defined(_WIN32)
    const auto path=amem::join_path(dir,name);size_t from=path[0]=='/'?1:0;
    for(size_t at=from;at<=path.size();++at)if(at==path.size()||path[at]=='/'){
        if(at==0)continue;
        const auto prefix=path.substr(0,at);struct stat st{};
        if(::lstat(prefix.c_str(),&st)==0){if(S_ISLNK(st.st_mode))return false;}
        else if(errno!=ENOENT)return false;
    }
    return true;
#else
    (void)dir;(void)name;return false;
#endif
}
struct ProjectionFile { std::string name,bytes; };
// A prepared transaction contains exact after-images and predecessor checksums.
// Recovery may finish our write; it must never overwrite a subsequent user edit.
inline bool finish_projection(Journal&j,const std::string&dir,const Record&prepared,
                              const std::function<bool(size_t)>&before_write={}) {
    // A completion record (for example an album label) shares the prepared
    // transaction's integrity boundary with its files. Two separately flushed
    // records can otherwise leave a durable label with no recoverable image.
    Record completion;const auto encoded=prepared.get("completion_record_hex");
    if(!encoded.empty()){
        std::string bytes;
        if(!unhex(encoded,bytes)||!Record::decode(bytes,completion)||completion.id.size()>256||
           completion.id==prepared.id||completion.id==prepared.id+"/commit")return false;
        const auto old=j.records().find(completion.id);
        if(old!=j.records().end()&&old->second.encode()!=bytes)return false;
    }
    if(j.records().count(prepared.id+"/commit")){
        // An older reader can complete the files while ignoring the optional
        // embedded record. Repair that record only; already committed files
        // may since have been edited by the user and are never rewritten here.
        return encoded.empty()||j.records().count(completion.id)||
               (j.enqueue(completion)&&j.flush());
    }
    const std::string count=prepared.get("files");
    if(count.empty()||count.size()>2||!std::all_of(count.begin(),count.end(),[](char c){return c>='0'&&c<='9';}))return false;
    const size_t n=std::stoul(count);if(n>16)return false;
    for(size_t i=0;i<n;++i) {
        const auto k=std::to_string(i)+".";const auto name=prepared.get(k+"name");
        if(!projection_name(name)||!safe_projection_target(dir,name))return false;
        std::string after;if(!unhex(prepared.get(k+"after_hex"),after)||checksum(after)!=prepared.get(k+"after_hash"))return false;
        std::string current;bool exists=false;const auto path=amem::join_path(dir,name);
        if(!read_exact(path,current,exists))return false;
        if(exists&&current==after){if(!durable_write(path,after))return false;continue;}
        if((exists?"1":"0")!=prepared.get(k+"existed")||checksum(current)!=prepared.get(k+"before_hash"))return false;
        if(before_write&&!before_write(i))return false;
        if(!durable_write(path,after))return false;
    }
    Record done{"projection-commit",prepared.id+"/commit",prepared.id,{{"sources",prepared.get("sources")},{"credits",prepared.get("credits")}}};
    if(!encoded.empty()&&!j.enqueue(completion))return false;
    return j.enqueue(done)&&j.flush();
}
inline bool project(Journal&j,const std::string&dir,const std::string&id,
                    const std::vector<ProjectionFile>&files,const std::string&sources,
                    const std::string&credits="",const std::function<bool(size_t)>&before_write={},
                    const Record*completion=nullptr) {
    const auto completion_hex=completion?hex(completion->encode()):std::string();
    if(completion){
        if(completion->id.empty()||completion->id.size()>256||completion->id==id||completion->id==id+"/commit")return false;
        const auto old=j.records().find(completion->id);
        if(old!=j.records().end()&&old->second.encode()!=completion->encode())return false;
        for(const auto&r:j.pending())if(r.id==completion->id&&r.encode()!=completion->encode())return false;
    }
    const auto prior=j.records().find(id);
    if(prior!=j.records().end()) {
        const auto &p=prior->second;
        if(p.kind!="projection-prepare"||p.get("files")!=std::to_string(files.size())||p.get("sources")!=sources||p.get("credits")!=credits)return false;
        if(p.get("completion_record_hex")!=completion_hex)return false;
        for(size_t i=0;i<files.size();++i)
            if(p.get(std::to_string(i)+".name")!=files[i].name||p.get(std::to_string(i)+".after_hex")!=hex(files[i].bytes))return false;
        return finish_projection(j,dir,p,before_write);
    }
    Record r{"projection-prepare",id,"",{{"files",std::to_string(files.size())},{"sources",sources},{"credits",credits}}};
    if(completion)r.fields["completion_record_hex"]=completion_hex;
    if(files.size()>16)return false;
    for(size_t i=0;i<files.size();++i) {
        if(!projection_name(files[i].name))return false;
        const auto k=std::to_string(i)+".";std::string current;bool exists=false;
        if(!read_exact(amem::join_path(dir,files[i].name),current,exists))return false;
        r.fields[k+"name"]=files[i].name;r.fields[k+"existed"]=exists?"1":"0";
        r.fields[k+"before_hash"]=checksum(current);r.fields[k+"after_hex"]=hex(files[i].bytes);
        r.fields[k+"after_hash"]=checksum(files[i].bytes);
    }
    return j.enqueue(r)&&j.flush()&&finish_projection(j,dir,r,before_write);
}
inline bool recover_projections(Journal&j,const std::string&dir) {
    std::vector<Record> pending;
    for(const auto&kv:j.records())if(kv.second.kind=="projection-prepare"&&
        (!j.records().count(kv.first+"/commit")||!kv.second.get("completion_record_hex").empty()))pending.push_back(kv.second);
    for(const auto&p:pending)if(!finish_projection(j,dir,p))return false;
    return true;
}
template<class Known>
inline bool projection_rows(const std::string&path,const std::string&known_rows,Known known,std::string&bytes) {
    std::ostringstream out;out<<known_rows;
    errno=0;std::ifstream in(path,std::ios::binary);
    if(!in&&errno!=ENOENT)return false;
    if(in){std::string line;bool over=false,terminated=false;
        while(amem::read_store_line_(in,line,over,terminated,&out)){
            if(!over&&!known(line)){out.write(line.data(),line.size());if(terminated)out.put('\n');}
        }
        if(in.bad()||!in.eof())return false;
    }
    bytes=out.str();return bool(out);
}

// One runtime writer per directory, called only on the foreground thread.
inline Journal& session_journal(const std::string&dir) {
    static std::map<std::string,std::unique_ptr<Journal>> writers;
    auto &p=writers[dir];if(!p){p=std::make_unique<Journal>(dir);p->load();}return *p;
}
inline aev::EventId event_id(const std::string&s) {
    const auto p=s.rfind(':');if(p==std::string::npos)return {};
    const auto n=s.substr(p+1);if(n.empty()||n.size()>20||!std::all_of(n.begin(),n.end(),[](char c){return c>='0'&&c<='9';}))return {};
    try{return {s.substr(0,p),std::stoull(n)};}catch(...){return {};}
}
// Extraction fragments retain the original source coordinates and lineage.
// For extraction work, [begin,end) describes exactly the retained text bytes,
// as in pack_extraction (not a containing parent span). Optional fields keep
// older rows readable; contradictory coordinates remain in the journal but
// cannot be silently reinterpreted as a different byte span.
inline bool extract_source_ref_(const Record&r,size_t bytes,aev::SourceRef&source) {
    source.event=event_id(r.parent);if(!source.event.valid())return false;
    source.actor=r.get("source_actor");if(source.actor.empty())source.actor=r.get("actor");
    source.producer=r.get("producer");source.parent=r.get("source_parent");
    auto number=[&](const char*key,uint64_t fallback,uint64_t&out){
        const auto value=r.get(key);if(value.empty()){out=fallback;return true;}
        if(!std::all_of(value.begin(),value.end(),[](char c){return c>='0'&&c<='9';}))return false;
        try{out=std::stoull(value);return true;}catch(...){return false;}
    };
    uint64_t begin=0,end=0;
    if(!number("version",1,source.version)||!source.version||!number("begin",0,begin)||
       begin>std::numeric_limits<size_t>::max()||bytes>std::numeric_limits<size_t>::max()-(size_t)begin)return false;
    if(!number("end",begin+bytes,end)||end!=begin+bytes)return false;
    source.begin=(size_t)begin;source.end=(size_t)end;return true;
}
inline bool queue_turn(Journal&j,const amem::Turn&t) {
    if(!t.source.event.valid())return false;
    const auto id=t.source.event.str()+"/work";
    if(t.text.size()>std::numeric_limits<size_t>::max()-t.source.begin||!t.source.version)return false;
    const size_t end=t.source.end?t.source.end:t.source.begin+t.text.size();
    if(end!=t.source.begin+t.text.size())return false;
    const auto actor=t.source.actor.empty()?t.speaker:t.source.actor;
    const Record record{"extract-source",id,t.source.event.str(),
        {{"actor",t.speaker},{"producer",t.source.producer},{"text_hex",hex(t.text)},
         {"version",std::to_string(t.source.version)},{"begin",std::to_string(t.source.begin)},
         {"end",std::to_string(end)},{"source_actor",actor},{"source_parent",t.source.parent},
         {"wall",std::to_string((long)time(nullptr))}}};
    auto same=[&](const Record&r){
        aev::SourceRef source;
        return r.kind=="extract-source"&&r.get("actor")==t.speaker&&r.get("text_hex")==record.get("text_hex")&&
            extract_source_ref_(r,t.text.size(),source)&&source.event.str()==t.source.event.str()&&
            source.actor==actor&&source.producer==t.source.producer&&source.parent==t.source.parent&&
            source.version==t.source.version&&source.begin==t.source.begin&&source.end==end;
    };
    auto existing=j.records().find(id);if(existing!=j.records().end())return same(existing->second);
    for(const auto&r:j.pending())if(r.id==id)return same(r);
    return j.enqueue(record);
}
struct Coverage { std::string id; size_t begin=0,end=0; };
inline std::string coverage_text(const std::vector<Coverage>&v) {
    std::string r;for(const auto&s:v)r+=hex(s.id)+"\t"+std::to_string(s.begin)+"\t"+std::to_string(s.end)+"\n";return r;
}
inline std::vector<Coverage> coverage_read(const std::string&s) {
    std::vector<Coverage> v;std::istringstream in(s);std::string line;
    while(std::getline(in,line)){const auto c=columns(line);if(c.size()!=3)continue;
        Coverage a;if(!unhex(c[0],a.id))continue;
        try{size_t p=0;a.begin=std::stoull(c[1],&p);if(p!=c[1].size())continue;
            a.end=std::stoull(c[2],&p);if(p!=c[2].size()||a.end<a.begin)continue;
        }catch(...){continue;}v.push_back(a);
    }return v;
}
inline std::map<std::string,size_t> covered_prefixes(const Journal&j) {
    std::map<std::string,std::vector<std::pair<size_t,size_t>>> spans;
    for(const auto&kv:j.records())if(kv.second.kind=="projection-commit")
        for(const auto&s:coverage_read(kv.second.get("sources")))spans[s.id].push_back({s.begin,s.end});
    std::map<std::string,size_t> result;
    for(auto &kv:spans){std::sort(kv.second.begin(),kv.second.end());size_t end=0;
        for(const auto&p:kv.second){if(p.first>end)break;end=std::max(end,p.second);}result[kv.first]=end;}
    return result;
}
inline std::vector<amem::Turn> pending_turns(const Journal&j) {
    auto covered=covered_prefixes(j);std::vector<std::pair<std::pair<long,std::string>,amem::Turn>> sorted;
    for(const auto&kv:j.records())if(kv.second.kind=="extract-source") {
        const auto&r=kv.second;amem::Turn t;t.speaker=r.get("actor");
        if(!unhex(r.get("text_hex"),t.text))continue;
        if(!extract_source_ref_(r,t.text.size(),t.source)||t.text.empty()||
           covered[r.parent]>=t.source.end)continue;
        long wall=0;try{wall=std::stol(r.get("wall"));}catch(...){}
        // Fixed-width sequence tie-breaker retains event order within one second.
        std::ostringstream order;order<<t.source.event.session<<':'<<std::setw(20)<<std::setfill('0')<<t.source.event.sequence;
        sorted.push_back({{wall,order.str()},t});
    }
    std::sort(sorted.begin(),sorted.end(),[](const auto&a,const auto&b){return a.first<b.first;});
    std::vector<amem::Turn> v;for(auto &p:sorted)v.push_back(std::move(p.second));return v;
}
inline bool state_projection(const std::string&path,const std::vector<amem::MemEntry>&v,std::string&out) {
    return projection_rows(path,amem::state_rows_text_(v),[](const std::string&s){amem::MemEntry e;return amem::parse_state_row_(s,e,false,false);},out);
}
inline bool ledger_projection(const std::string&path,const std::vector<amem::LedgerRow>&v,std::string&out) {
    std::ostringstream rows;for(const auto&r:v)rows<<r.session<<'\t'<<r.weight<<'\t'<<amem::flatten_ws(r.axis)<<'\t'
        <<amem::flatten_ws(r.evidence)<<r.opaque_suffix<<'\n';
    return projection_rows(path,rows.str(),[](const std::string&s){amem::LedgerRow r;return amem::parse_ledger_row_(s,r);},out);
}
// Exact source-report receipts supplement the legacy topic told ledger. An
// admission is never a report. An unmatched paraphrase remains unknown here;
// these conservative receipts neither disable recall nor suppress creativity.
inline std::string report_space(const std::string&s){
    std::string out;bool space=false;
    for(unsigned char c:s){if(c==' '||c=='\n'||c=='\r'||c=='\t'){space=!out.empty();continue;}if(space){out+=' ';space=false;}out+=char(c);}
    return out;
}
inline size_t record_exact_reports(Journal&j,const std::vector<aev::AdmissionRef>&sources,
                                  const std::string&confirmed,const std::string&receipt,uint64_t epoch){
    if(confirmed.empty()||receipt.empty())return 0;
    const auto words=report_space(confirmed);size_t count=0;
    for(const auto&a:sources){
        if(!a.decoded||a.epoch!=epoch||a.record_id.empty()||a.kind=="generation guidance")continue;
        const auto content=report_space(a.content);
        // Short common fragments cannot identify which source was reported.
        if(content.size()<32||words.find(content)==std::string::npos)continue;
        const auto id=receipt+"/source-report/"+checksum(a.record_id+"/"+std::to_string(a.source.version));
        if(j.enqueue({"source-report",id,a.record_id,{{"speech_receipt",receipt},{"version",std::to_string(a.source.version)},
             {"epoch",std::to_string(epoch)},{"kind",a.kind},{"complete",a.complete?"1":"0"},
             {"proof","exact confirmed text match; acoustic reception unverified"},{"content_hash",checksum(a.content)}}}))++count;
    }
    return count;
}
} // namespace acont
