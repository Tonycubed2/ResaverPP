// Synthetic parser-boundary regressions; no files or assets.
#include "core/Papyrus.h"
#include <cstdio>
#include <string>
using namespace ess;
namespace {
int passed=0,failed=0;
void Check(bool value,const char* name){if(value)++passed;else{++failed;std::printf("FAIL: %s\n",name);}}
void Roundtrip(Papyrus& p){try{p.saveFileVersion=4;p.reindex();Writer w;p.write(w);Reader r(w.buf.data(),w.buf.size());Papyrus q;q.read(r);Writer again;q.write(again);Check(r.remaining()==0&&w.buf==again.buf,"valid minimum-width data roundtrips exactly");}catch(const std::exception& e){std::printf("Roundtrip exception: %s\n",e.what());Check(false,"valid minimum-width fixture parses");}}
void Reject(const Writer& w,std::size_t end){
    Reader r(w.buf.data(),w.buf.size());
    try{Papyrus p;p.read(r);Check(false,"impossible count rejected");}
    catch(const FormatError& e){Check(std::string(e.what()).find("count cannot fit")!=std::string::npos,"count has a specific preallocation diagnostic");Check(r.pos()==end,"count rejected before reading collection payload");}
}
void Put(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t n){for(unsigned i=0;i<4;++i)b.at(at+i)=std::uint8_t(n>>(i*8));}
}
int main(int argc,char**){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Papyrus empty;empty.saveFileVersion=4;Writer zero;empty.write(zero);Check(zero.buf.size()==56,"empty fixture offsets remain explicit");
    // Modest malformed counts also run safely against the unpatched control.
    for(auto at:{2u,6u,10u,14u,18u,26u,30u,34u,38u,46u,50u}){Writer w;w.buf=zero.buf;Put(w.buf,at,1000);Reject(w,at+4);}
    {Writer w;w.u16(0);w.u32(0);w.u32(1);w.u32(0);w.u32(0);w.u32(1000);Reject(w,w.size());}
    {Writer w;w.u16(0);w.u32(0);w.u32(0);w.u32(1);w.u64(7);w.u32(0);w.u16(0);w.u16(0);RefID{0}.write(w);w.u8(0);
     w.u32(0);w.u32(0);w.u32(0);w.u32(0);w.u64(7);w.u8(0);w.u32(0);w.u32(0);w.u32(1000);Reject(w,w.size());}
    {auto p=Papyrus{};Roundtrip(p);p.strings.assign(500,"");Roundtrip(p);}
    {Papyrus p;p.strings={""};Script s;for(int i=0;i<500;++i)s.members.push_back({0,0});p.scripts.push_back(s);Roundtrip(p);}
    {Papyrus p;p.strings={""};ScriptInstance i;i.id=7;i.data=InstanceData{};i.data->vars.assign(500,Variable{});p.instances.push_back(i);Roundtrip(p);}
    {Papyrus p;p.strings={""};ArrayInfo a;a.id=8;a.type=VType::kNull;a.length=500;a.hasData=true;a.vars.assign(500,Variable{});p.arrays.push_back(a);Roundtrip(p);}
    {Papyrus p;p.strings={""};ActiveScript t;t.id=3;t.data=ActiveScriptData{};t.data->unknown5=0;StackFrame f;f.status=0;f.code.resize(500);t.data->frames.push_back(f);p.activeScripts.push_back(t);Roundtrip(p);}
    {Papyrus p;for(unsigned i=0;i<500;++i){FunctionMessage f;f.flag=3;p.functionMessages.push_back(f);SuspendedStack s;s.id=i;p.suspended1.push_back(s);}Roundtrip(p);}
    // Only the guarded build runs maximal counts; baseline mode avoids huge reserves.
    if(argc==1){Writer w;w.u16(0);w.u32(0xFFFFFFFFu);Reject(w,w.size());}
    std::printf("Papyrus bounds: %d passed, %d failed\n",passed,failed);return failed?1:0;
}
