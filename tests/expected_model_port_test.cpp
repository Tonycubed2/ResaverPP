// In-memory adversarial tests for complete expected-result verification.
#include "core/Verify.h"
#include <cstdio>
#include <functional>
using namespace ess;
namespace {
int pass = 0, fail = 0;
void Check(bool ok, const char* name) {
    if (ok) ++pass; else { ++fail; std::printf("FAIL: %s\n", name); }
}
SaveFile Make() {
    SaveFile s; s.header.magic = "TESV_SAVEGAME"; s.header.version = 12;
    s.plugins.full = {"Synthetic.esm"}; s.formVersion = 78; s.plugins.hasLite = true;
    GlobalData p; p.type = 1001; p.papyrus = std::make_unique<Papyrus>();
    p.papyrus->strings = {"", "Script", "Var"}; p.papyrus->saveFileVersion = 4;
    Script sc; sc.name = 1; sc.members.push_back({2, 2}); p.papyrus->scripts.push_back(sc);
    ScriptInstance si; si.id = 0xF123456789ABCDEF; si.script = 1; si.data = InstanceData{};
    Variable v; v.type = VType::kInt; v.value = 42; si.data->vars.push_back(v);
    p.papyrus->instances.push_back(si); p.papyrus->reindex(); s.table3.push_back(std::move(p));
    GlobalData g; g.type = 42; g.raw = {1, 2, 3}; s.table1.push_back(std::move(g));
    ChangeForm f; f.refID.data = 0x400014; f.typeField = 8; f.version = 1;
    f.data = {3, 4}; f.length1 = 2; s.changeForms.push_back(f);
    s.formIDs = {0x14}; s.visitedWorldspaces = {3}; s.unknown3 = {9, 8}; s.cosave = {7, 6};
    return s;
}
void Mutation(const char* name, const std::function<void(SaveFile&)>& mutate) {
    auto a = Make(), b = Make(); mutate(b);
    Check(!verify::Expected(a, b).identical(), name);
}
}
int main(int argc, char** argv) {
    auto a = Make(), b = Make(); Check(verify::Expected(a, b).identical(), "equal full models pass");
    Mutation("same-count Papyrus variable corruption", [](auto& s){s.papyrus()->instances[0].data->vars[0].value++;});
    Mutation("Papyrus opaque tail corruption", [](auto& s){s.papyrus()->tail.push_back(9);});
    Mutation("changeform flags corruption", [](auto& s){s.changeForms[0].changeFlags++;});
    Mutation("changeform identity corruption", [](auto& s){s.changeForms[0].refID.data++;});
    Mutation("changeform version corruption", [](auto& s){s.changeForms[0].version++;});
    Mutation("same-size changeform body corruption", [](auto& s){s.changeForms[0].data[0]++;});
    Mutation("same-count global-data corruption", [](auto& s){s.table1[0].raw[1]++;});
    Mutation("global-data type corruption", [](auto& s){s.table1[0].type++;});
    Mutation("header screenshot corruption", [](auto& s){s.header.screenshot.push_back(9);});
    Mutation("header field corruption", [](auto& s){s.header.saveNumber++;});
    Mutation("plugin order/name corruption", [](auto& s){s.plugins.full[0] = "Other.esm";});
    Mutation("form table corruption", [](auto& s){s.formIDs[0]++;});
    Mutation("visited worlds corruption", [](auto& s){s.visitedWorldspaces[0]++;});
    Mutation("unknown body region corruption", [](auto& s){s.unknown3[0]++;});
    Mutation("unused file-location word corruption", [](auto& s){s.flt.unused[1]++;});
    Mutation("same-size co-save corruption", [](auto& s){s.cosave[1]++;});
    Mutation("missing co-save", [](auto& s){s.cosave.clear();});
    {
        auto present = Make(), absent = Make();
        present.cosave.clear(); present.hasCosave = true; absent.cosave.clear();
        Check(!verify::Expected(present, absent).identical(), "empty and absent co-saves are distinct");
    }
    Mutation("structural problem on reopening", [](auto& s){s.problems.push_back("synthetic damaged layout");});
    {auto invalid=Make();invalid.problems.push_back("synthetic invalid expected model");Check(!verify::Expected(invalid,a).identical(),"invalid expected model is not qualified by a clean reopen");}
    b.cosave[0]++; auto result = verify::Expected(a,b);
    Check(verify::Describe(result).find("cosave at byte 0") != std::string::npos, "diagnostic section and byte offset");
    for(auto compression:{Compression::kNone,Compression::kZlib,Compression::kLZ4}) {
        auto expected=Make();expected.header.compression=compression;
        auto actual=SaveFile::parse(expected.serialize(),expected.cosave);
        Check(verify::Expected(expected,*actual).identical(),"shared parser roundtrips every supported compression");
        expected.cosave.clear();expected.hasCosave=true;
        actual=SaveFile::parse(expected.serialize(),{},true);
        Check(actual->hasCosave&&actual->cosave.empty(),"explicit empty companion survives parser");
        Check(verify::Expected(expected,*actual).identical(),"empty companion participates in full model comparison");
        auto absent=SaveFile::parse(expected.serialize());
        Check(!verify::Expected(expected,*absent).identical(),"missing companion differs from present empty companion");
    }
    if(argc==2) {
        const auto path=std::filesystem::u8path(argv[1]);auto co=path;co.replace_extension(".skse");
        if(std::filesystem::exists(std::filesystem::symlink_status(path))||std::filesystem::exists(std::filesystem::symlink_status(co))) {
            std::fprintf(stderr,"One explicit new pair is required for optional disk validation\n");return 2;
        }
        auto model=Make();model.cosave.clear();model.hasCosave=true;
        model.write(path,false);auto reopened=SaveFile::load(path);
        Check(verify::Expected(model,*reopened).identical(),"actual staged disk write reopens as full expected model");
        Check(reopened->hasCosave&&std::filesystem::exists(co)&&std::filesystem::file_size(co)==0,"actual empty companion is retained");
        std::printf("Optional disk output retained for independent inspection\n");
    }
    if(argc==3) {
        if(std::string(argv[2])=="--salvage-fixture") {
            const auto path=std::filesystem::u8path(argv[1]);auto co=path;co.replace_extension(".skse");
            if(std::filesystem::exists(std::filesystem::symlink_status(path))||std::filesystem::exists(std::filesystem::symlink_status(co)))return 2;
            auto model=Make();model.cosave.clear();model.hasCosave=true;
            model.papyrus()->instances[0].script=999;
            model.problems=model.papyrus()->check();
            Check(!model.problems.empty(),"salvage fixture is explicitly structurally damaged");
            model.write(path,false);auto reopened=SaveFile::load(path);
            Check(!reopened->problems.empty(),"existing caller-gated salvage can retain a readable damaged model");
            Check(!verify::Expected(model,*reopened).identical(),"salvage is not advertised as full verification");
            std::printf("Synthetic salvage fixture retained\n");
            std::printf("Expected-result regressions: %d passed, %d failed\n",pass,fail);return fail?1:0;
        }
        if(std::string(argv[2])!="--expect-companion-conflict")return 2;
        const auto path=std::filesystem::u8path(argv[1]);
        auto model=Make();model.cosave.clear();model.hasCosave=true;
        // Only the exact synthetic pair created by the optional disk test is eligible.
        auto existing=SaveFile::load(path);
        if(!verify::Expected(model,*existing).identical())return 2;
        model.hasCosave=false;
        try{model.write(path,false);Check(false,"unexpected destination companion refuses before writing");}
        catch(const std::runtime_error& e){Check(std::string(e.what()).find("destination has a companion")!=std::string::npos,"unexpected destination companion refuses before writing");}
    }
    std::printf("Expected-result regressions: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
