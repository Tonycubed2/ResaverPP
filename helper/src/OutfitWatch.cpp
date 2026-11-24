#include "PCH.h"
#include "OutfitWatch.h"

#include "Game.h"

namespace OutfitWatch
{
	namespace
	{
		std::atomic<int> g_logged{ 0 };

		std::string NameOf(RE::TESForm* a_form)
		{
			if (!a_form) {
				return "(none)";
			}
			const char* n = a_form->GetName();
			const char* e = a_form->GetFormEditorID();
			std::string plugin;
			if (const auto* file = a_form->GetFile(0)) {
				plugin = std::string(file->GetFilename());
			}
			return std::format("\"{}\"{} {:08X} <{}>", n && *n ? n : "", e && *e ? std::format(" [{}]", e) : std::string(), a_form->GetFormID(),
				plugin.empty() ? "?" : plugin);
		}

		bool IsFollower(RE::Actor* a)
		{
			if (a->IsPlayerTeammate()) {
				return true;
			}
			if (!a->GetActorBase()) {
				return false;
			}
			static RE::TESFaction* current = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);  // CurrentFollowerFaction
			return current && a->GetFactionRank(current, false) >= 0;
		}

		bool InOutfit(RE::BGSOutfit* a_outfit, RE::TESForm* a_item)
		{
			if (!a_outfit || !a_item) {
				return false;
			}
			for (auto* f : a_outfit->outfitItems) {
				if (f == a_item) {
					return true;
				}
				if (auto* ll = f ? f->As<RE::TESLeveledList>() : nullptr) {
					for (std::uint32_t i = 0; i < ll->numEntries; ++i) {
						if (ll->entries[i].form == a_item) {
							return true;
						}
					}
				}
			}
			return false;
		}

		const char* SitSleep(RE::Actor* a)
		{
			switch (a->AsActorState()->GetSitSleepState()) {
			case RE::SIT_SLEEP_STATE::kNormal:
				return "standing (not sitting or in bed)";
			case RE::SIT_SLEEP_STATE::kWantToSit:
				return "going to sit";
			case RE::SIT_SLEEP_STATE::kWaitingForSitAnim:
				return "sitting down";
			case RE::SIT_SLEEP_STATE::kIsSitting:
				return "sitting";
			case RE::SIT_SLEEP_STATE::kWantToStand:
				return "getting up from a seat";
			case RE::SIT_SLEEP_STATE::kWantToSleep:
				return "going to bed";
			case RE::SIT_SLEEP_STATE::kWaitingForSleepAnim:
				return "lying down in bed";
			case RE::SIT_SLEEP_STATE::kIsSleeping:
				return "IN BED, sleeping";
			case RE::SIT_SLEEP_STATE::kWantToWake:
				return "getting out of bed";
			default:
				return "other";
			}
		}

		std::string Stack()
		{
			void*       frames[48]{};
			const auto  n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
			std::string out;
			for (USHORT i = 0; i < n; ++i) {
				HMODULE mod = nullptr;
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						static_cast<LPCWSTR>(frames[i]), &mod) ||
					!mod) {
					continue;
				}
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(mod, path, MAX_PATH);
				out += std::format("{}{}+{:X}", out.empty() ? "" : " < ", std::filesystem::path(path).filename().string(),
					reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(mod));
			}
			return out;
		}

		class Sink : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_event || !a_event->actor || g_logged > 3000) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* a = a_event->actor->As<RE::Actor>();
				if (!a || a->IsPlayerRef() || !IsFollower(a)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* npc = a->GetActorBase();
				auto* item = RE::TESForm::LookupByID(a_event->baseObject);
				if (!npc || !item || !item->Is(RE::FormType::Armor)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const bool sleepPiece = InOutfit(npc->sleepOutfit, item);
				const bool defaultPiece = InOutfit(npc->defaultOutfit, item);
				if (!sleepPiece && !defaultPiece) {
					return RE::BSEventNotifyControl::kContinue;
				}
				++g_logged;
				auto*       pkg = a->GetCurrentPackage();
				const bool  wear = pkg && pkg->packData.packFlags.all(RE::PACKAGE_DATA::GeneralFlag::kWearSleepOutfit);
				auto*       cell = a->GetParentCell();
				auto*       ui = RE::UI::GetSingleton();
				const bool  loading = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
				spdlog::warn("OutfitWatch: {} {:08X} ({}) {} {} - a piece of their {} outfit {}", a_event->equipped ? "PUT ON" : "took off",
					a->GetFormID(), a->GetDisplayFullName(), sleepPiece ? "SLEEP" : "default", NameOf(item),
					sleepPiece ? "SLEEP" : "default", NameOf(sleepPiece ? static_cast<RE::TESForm*>(npc->sleepOutfit) : npc->defaultOutfit));
				spdlog::warn("OutfitWatch:   package {}{} | {} | cell {}{} | 3D {}", NameOf(pkg), pkg ? (wear ? " (Wear Sleep Outfit: ON)" : " (Wear Sleep Outfit: off)") : "",
					SitSleep(a), NameOf(cell), loading ? " | loading screen up" : "", a->Get3D(false) ? "loaded" : "not loaded");
				spdlog::warn("OutfitWatch:   call stack: {}", Stack());
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		static Sink sink;
		if (auto* h = RE::ScriptEventSourceHolder::GetSingleton()) {
			h->AddEventSink<RE::TESEquipEvent>(&sink);
			spdlog::info("OutfitWatch: watching followers' sleep and default outfit pieces (TESEquipEvent)");
		}
	}
}
