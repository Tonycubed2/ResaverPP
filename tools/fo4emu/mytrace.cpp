#include "core/ChangeFormFO4.h"
#include <cstdio>
using namespace ess;
int main(int argc, char** argv){ auto s=SaveFile::load(argv[1]); unsigned want=strtoul(argv[2],0,16); unsigned lo=atoi(argv[3]), hi=atoi(argv[4]);
 for(auto&c:s->changeForms) if(c.typeCode()==1 && s->formIDOf(c.refID)==want){ std::vector<std::pair<std::uint32_t,char>> t; auto a=fo4cf::DecodeActor(*s,c,&t); std::printf("%s stop %zu\n",a.error.c_str(),a.stoppedAt); for(auto&x:t) if(x.first>=lo&&x.first<=hi) std::printf("%u:%c ",x.first,x.second); std::puts("");}}
