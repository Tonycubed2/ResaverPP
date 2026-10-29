// Synthetic single/batched reference contrasts; no save or file operations.
#include "core/Ops.h"
#include <cstdio>
#include <set>
#include <tuple>
using namespace ess;
namespace {
int passed=0,failed=0;
void Check(bool b,const char* name){if(b)++passed;else{++failed;std::printf("FAIL: %s\n",name);}}
Variable Ref(EID id,VType type=VType::kRef){Variable v;v.type=type;v.value=id;return v;}
Variable Wrap(Variable value){Variable v;v.type=VType::kVariant;v.inner=std::make_unique<Variable>(std::move(value));return v;}
using Key=std::tuple<int,std::uint64_t,std::string>;
std::set<Key> Keys(const std::vector<ops::Referrer>& rows){std::set<Key> out;for(const auto& x:rows)out.emplace(int(x.kind),x.id,x.where);return out;}
}
int main(){
    const EID a=0xF123456789ABCDEF,b=0xABCDEF1234567890,c=100;
    Papyrus p;p.strings={"","SyntheticScript","OnUpdate"};
    ScriptInstance i;i.id=a;i.script=1;i.data=InstanceData{};i.data->vars={Ref(a),Ref(b),Ref(b,VType::kInt),Ref(b,VType::kStruct),Ref(a,VType::kRefArray),Wrap(Wrap(Ref(b)))};p.instances.push_back(i);
    i.id=b;i.data->vars={Ref(a),Ref(b)};p.instances.push_back(i);
    Reference r;r.id=88;r.script=1;r.data=InstanceData{};r.data->vars={Ref(a),Ref(b)};p.references.push_back(r);
    ArrayInfo array;array.id=c;array.type=VType::kRef;array.vars={Ref(c),Ref(b)};p.arrays.push_back(array);
    ActiveScript t;t.id=100;t.data=ActiveScriptData{};StackFrame frame;frame.baseName=1;frame.event=2;frame.owner=Ref(c);frame.vars={Ref(a),Wrap(Ref(b))};t.data->frames.push_back(frame);p.activeScripts.push_back(t);
    const auto before=p.instances[0].data->vars[1].value;
    Check(ops::FindReferrers(p,a).size()==3,"legacy target A has three nonself owners");
    Check(ops::FindReferrers(p,b).size()==6,"legacy target B has six matching locations");
    Check(ops::FindReferrers(p,c).size()==1,"legacy target C retains the thread owner");
    Check(ops::FindReferrers(p,std::set<EID>{}).empty(),"empty targets produce no edges");
    std::set<Key> unionSingle;
    for(EID target:{a,b,c}){
        auto one=Keys(ops::FindReferrers(p,target));auto batchOne=Keys(ops::FindReferrers(p,std::set<EID>{target}));
        Check(one==batchOne,"single-target overload preserves legacy result");unionSingle.insert(one.begin(),one.end());
    }
    const auto rows=ops::FindReferrers(p,std::set<EID>{a,b,c});
    Check(Keys(rows)==unionSingle,"batch equals union of individual nonself searches");
    Check(Keys(rows).contains({int(ops::Referrer::Kind::kInstance),a,"SyntheticScript var[1]"}),"target owners may refer to other targets");
    Check(!Keys(rows).contains({int(ops::Referrer::Kind::kInstance),a,"SyntheticScript var[0]"}),"identity self edge excluded by default");
    Check(!Keys(rows).contains({int(ops::Referrer::Kind::kInstance),a,"SyntheticScript var[2]"}),"integers equal to an ID are not references");
    Check(Keys(rows).contains({int(ops::Referrer::Kind::kInstance),a,"SyntheticScript var[5]"}),"nested variants are followed");
    Check(Keys(rows).contains({int(ops::Referrer::Kind::kThread),100,"SyntheticScript.OnUpdate (owner)"}),"thread ID namespace is not an element self edge");
    const auto self=Keys(ops::FindReferrers(p,std::set<EID>{a,b,c},true));
    Check(self.contains({int(ops::Referrer::Kind::kInstance),a,"SyntheticScript var[0]"}),"explicit self mode includes owner edge");
    Check(self.size()==unionSingle.size()+4,"only four identity self edges added");
    Check(ops::FindReferrers(p,std::set<EID>{0xDEADBEEF}).empty(),"unknown targets produce no edges");
    Check(before==p.instances[0].data->vars[1].value,"query preserves the model");
    std::printf("Batched referrers: %d passed, %d failed\n",passed,failed);return failed?1:0;
}
