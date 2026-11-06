// applies fo4ws::SetUsed to every editable workshop (reset to 0; workshops without a current-draws entry get 5 / 7 so an
// entry is added), dumps the edited REFR bodies in dumpcf format and the expected actor value bases (raw ref, f32 bits)
#include "core/WorkshopFO4.h"
#include "core/ChangeFormData.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
using namespace ess;
int main(int argc,char**argv){
  auto s=SaveFile::load(argv[1]); std::ofstream o(argv[2],std::ios::binary); FILE* ex=std::fopen(argv[3],"w");
  auto w=[&](std::uint32_t v){o.write((char*)&v,4);}; w(s->formVersion);
  int n=0,added=0,fail=0;
  for(auto&ws:fo4ws::List(*s)){ if(!ws.readable||!ws.hasStorage) continue;
    ChangeForm* cf=nullptr; for(auto&c:s->changeForms) if(c.typeCode()==0&&s->formIDOf(c.refID)==ws.formID) cf=&c;
    auto d0=fo4cf::DecodeRefr(*s,*cf); auto b0=cf::Body(*cf); auto st0=fo4ws::ReadStorage(b0,d0,*s);
    bool add=!st0->hasBase(fo4ws::kAvCurrentDraws);
    const bool orig=std::getenv("ORIG")!=nullptr;
    bool ch=false; auto why=orig? std::string() : fo4ws::SetUsed(*s,ws.formID,add?5.0f:0.0f,add?7.0f:0.0f,&ch);
    if(!why.empty()){std::printf("FAIL %08X %s\n",ws.formID,why.c_str()); ++fail; continue;}
    if(!ch&&!orig) continue; ++n; added+=add;
    auto body=cf::Body(*cf); auto d=fo4cf::DecodeRefr(*s,*cf); auto st=fo4ws::ReadStorage(body,d,*s);
    w(cf->refID.data); o.put((char)cf->typeCode()); o.put((char)cf->version); w(cf->changeFlags); w(ws.formID); w((std::uint32_t)body.size()); o.write((const char*)body.data(),body.size());
    std::fprintf(ex,"%06x %zu",cf->refID.data,body.size()); for(auto&b:st->bases){std::uint32_t bits; std::memcpy(&bits,&b.base,4); std::fprintf(ex," %06x:%08x",b.av.data,bits);} std::fprintf(ex,"\n");
  }
  std::printf("%s: %d edited (%d with entries added), %d failed\n",argv[1],n,added,fail);
  std::fclose(ex);
}
