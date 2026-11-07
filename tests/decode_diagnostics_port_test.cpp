// Invented payloads only; decoding preserves the original raw bytes.
#include "core/ChangeFormData.h"
#include <cstdio>
using namespace ess;
namespace {
int passed=0,failed=0;
void Check(bool b,const char* name){if(b)++passed;else{++failed;std::printf("FAIL: %s\n",name);}}
ChangeForm Form(const Writer& w,std::uint32_t flags=1u<<6,std::uint8_t type=0){ChangeForm f;f.refID.data=0x400014;f.typeField=type;f.changeFlags=flags;f.data=w.buf;f.length1=std::uint32_t(w.size());return f;}
void Ranges(const cf::Node& n,std::size_t size){Check(n.offset<=size&&n.size<=size-n.offset,"node span is inside the decompressed body");for(const auto& child:n.kids){Check(child.offset>=n.offset&&child.offset+child.size<=n.offset+n.size,"child span is inside its group");Ranges(child,size);}}
const cf::Node* Find(const cf::Node& n,const std::string& label){if(n.label==label)return &n;for(const auto& k:n.kids)if(const auto* found=Find(k,label))return found;return nullptr;}
}
int main(){
    SaveFile s;
    Writer valid;valid.u8(3u<<2);valid.u8(24);RefID{0x400014}.write(valid);for(int i=0;i<4;++i)valid.f32(float(i));valid.u8(36);valid.i16(7);valid.u8(28);RefID{0x400015}.write(valid);
    auto f=Form(valid);const auto original=f.data;auto d=cf::Decode(s,f);
    Check(d.complete&&d.consumed==valid.size()&&d.failureCategory.empty(),"valid payload completes without a failure category");
    Check(f.data==original,"decode does not change the raw payload");Ranges(d.root,d.bodySize);
    Check(d.extraTrace.size()==3,"all extra elements have traces");
    if(d.extraTrace.size()==3){Check(d.extraTrace[0].type==24&&d.extraTrace[0].offset==1&&d.extraTrace[0].predecessor==-1,"first trace has byte offset and no predecessor");Check(d.extraTrace[1].type==36&&d.extraTrace[1].offset==21&&d.extraTrace[1].predecessor==24,"second trace has exact predecessor");Check(d.extraTrace[2].offset==24&&d.extraTrace[2].predecessor==36,"third trace follows a counted scalar");}
    const auto* pos=Find(d.root,"POS");Check(pos&&pos->offset==5&&pos->size==12,"vector byte span excludes the preceding ref");
    const auto* count=Find(d.root,"COUNT");Check(count&&count->offset==22&&count->size==2,"scalar byte span is exact");
    for(std::size_t end=0;end<valid.size();++end){auto cut=f;cut.data.resize(end);cut.length1=std::uint32_t(end);auto shortRead=cf::Decode(s,cut);Check(!shortRead.complete&&!shortRead.error.empty(),"every truncated prefix is explicitly incomplete");Check(shortRead.consumed<=end,"truncation never reports consuming beyond its boundary");}
    Writer unknown;unknown.u8(3u<<2);unknown.u8(4);unknown.u8(22);unknown.u8(255);auto bad=cf::Decode(s,Form(unknown));
    Check(!bad.complete&&bad.failureCategory=="extra-unknown-type","unknown extra type is distinguished");Check(bad.failurePath.find("EXTRADATA")!=std::string::npos,"failed nesting path is retained");
    Check(bad.extraTrace.size()==3&&bad.extraTrace.back().predecessor==22,"nested predecessor records actual read order");
    Writer truncated;truncated.u8(4);truncated.u8(28);auto shortRef=cf::Decode(s,Form(truncated));
    Check(shortRef.failureCategory=="buffer-underflow"&&shortRef.failurePath.find("ReferenceHandle")!=std::string::npos,"truncated reference reports its specific group");
    Writer excessive;excessive.u16(std::uint16_t((1025u<<2)|1u));auto many=cf::Decode(s,Form(excessive));Check(many.failureCategory=="excessive-array-count","impossible extra count is separate from unknown types");
    Writer opaque;opaque.u8(99);auto unsupported=cf::Decode(s,Form(opaque,0,63));Check(!unsupported.supported&&unsupported.failureCategory=="unsupported","unsupported body type is not a decode failure");
    Writer tail;tail.u32(0);tail.u8(0);tail.u8(99);auto leftovers=cf::Decode(s,Form(tail,0,8));Check(!leftovers.complete&&leftovers.failureCategory=="unparsed-tail","unparsed tail has its own category");
    auto compressed=Form(opaque);compressed.length2=100;auto decompression=cf::Decode(s,compressed);Check(decompression.failureCategory=="decompression","decompression failure is distinct");
    auto again=cf::Decode(s,f);Check(again.extraTrace.front().predecessor==-1&&again.extraTrace.size()==3,"trace state is per decode, not global");
    Writer emptyList;auto recoveredList=cf::Decode(s,Form(emptyList,1,std::uint8_t(ChangeFormType::FLST)));
    Check(recoveredList.complete&&recoveredList.failurePath.empty(),"successful FLST fallback discards the rejected attempt's failure path");
    std::printf("Decode diagnostics: %d passed, %d failed\n",passed,failed);return failed?1:0;
}
