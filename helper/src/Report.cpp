#include "PCH.h"
#include "Report.h"

#include <numbers>

// Ported from the Snapshot mod (tonycubed2, Snapshot 1.1.0 Main.cpp WriteReport), so players who have ReSaver Plus get
// the same report from Collect all logs without installing Snapshot.

namespace Report
{
	namespace
	{
		constexpr float kRadius = 10000.0f;  // outdoors: NPCs this close to the player are listed in full

		std::string S(const char* a) { return a ? a : ""; }

		std::string PluginOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "";
			}
			if (const auto file = a_form->GetFile(0)) {
				return std::string(file->GetFilename());
			}
			return a_form->IsDynamicForm() ? "created in game" : "?";
		}

		std::string Label(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return "(none)";
			}
			std::string out = "\"" + S(a_form->GetName()) + "\"";
			const auto  ed = S(a_form->GetFormEditorID());
			if (!ed.empty()) {
				out += " [" + ed + "]";
			}
			return out + std::format(" {:08X} <{}>", a_form->GetFormID(), PluginOf(a_form));
		}

		std::string RefLabel(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return "(none)";
			}
			const auto base = a_ref->GetBaseObject();
			return std::format("\"{}\" ref {:08X} <{}>  base {:08X} <{}>", S(a_ref->GetDisplayFullName()), a_ref->GetFormID(), PluginOf(a_ref),
				base ? base->GetFormID() : 0u, PluginOf(base));
		}

		std::string Pos(RE::TESObjectREFR* a_ref)
		{
			const auto p = a_ref->GetPosition();
			const auto r = a_ref->GetAngle();
			return std::format("x {:.1f}  y {:.1f}  z {:.1f}  facing {:.0f} deg", p.x, p.y, p.z, r.z * 180.0f / std::numbers::pi_v<float>);
		}

		std::string Where(RE::TESObjectREFR* a_ref)
		{
			std::string out;
			const auto  cell = a_ref->GetParentCell();
			if (cell) {
				out += "cell " + Label(cell);
				if (cell->IsInteriorCell()) {
					out += " (interior)";
				} else if (const auto c = cell->GetCoordinates()) {
					out += std::format(" (exterior grid {},{})", c->cellX, c->cellY);
				}
			} else {
				out += "cell (not loaded)";
			}
			if (const auto ws = a_ref->GetWorldspace()) {
				out += " | worldspace " + Label(ws);
			}
			std::string chain;
			for (auto loc = a_ref->GetCurrentLocation(); loc; loc = loc->parentLoc) {
				chain += (chain.empty() ? "" : " > ") + Label(loc);
			}
			if (!chain.empty()) {
				out += " | location " + chain;
			}
			return out;
		}

		const char* ObjState(RE::QUEST_OBJECTIVE_STATE a_s)
		{
			switch (a_s) {
			case RE::QUEST_OBJECTIVE_STATE::kDormant:
				return "dormant";
			case RE::QUEST_OBJECTIVE_STATE::kDisplayed:
				return "DISPLAYED";
			case RE::QUEST_OBJECTIVE_STATE::kCompleted:
				return "completed";
			case RE::QUEST_OBJECTIVE_STATE::kCompletedDisplayed:
				return "completed (shown)";
			case RE::QUEST_OBJECTIVE_STATE::kFailed:
				return "failed";
			case RE::QUEST_OBJECTIVE_STATE::kFailedDisplayed:
				return "failed (shown)";
			default:
				return "?";
			}
		}

		struct Out
		{
			std::ofstream f;
			std::size_t   lines = 0;
			void          L(const std::string& a_line)
			{
				f << a_line << '\n';
				++lines;
			}
			void H(const std::string& a_title)
			{
				f << "\n================================================================================\n"
				  << a_title << "\n================================================================================\n";
				lines += 4;
			}
		};

		std::vector<RE::Actor*> AllActors()
		{
			std::vector<RE::Actor*> out;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSReadLockGuard l{ lock };
			if (map) {
				for (auto& [id, form] : *map) {
					if (form && form->Is(RE::FormType::ActorCharacter) && !form->IsDeleted()) {
						if (const auto a = form->As<RE::Actor>()) {
							out.push_back(a);
						}
					}
				}
			}
			std::sort(out.begin(), out.end(), [](auto x, auto y) { return x->GetFormID() < y->GetFormID(); });
			return out;
		}

		void Factions(Out& o, RE::Actor* a_actor)
		{
			a_actor->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
				if (a_faction) {
					o.L(std::format("    {} rank {}{}", Label(a_faction), a_rank, a_rank < 0 ? " (removed)" : ""));
				}
				return false;
			});
		}

		void ActorDetail(Out& o, RE::Actor* a, RE::Actor* a_player, bool a_isPlayer)
		{
			auto base = a->GetActorBase();
			o.L("--------------------------------------------------------------------------------");
			o.L(RefLabel(a));
			o.L("  base NPC   " + Label(base));
			o.L("  race       " + Label(a->GetRace()) +
				std::format("   sex {}   level {}", base && base->GetSex() == RE::SEX::kFemale ? "female" : "male", a->GetLevel()));
			o.L("  position   " + Pos(a));
			o.L("  where      " + Where(a));
			if (!a_isPlayer) {
				o.L(std::format("  distance to player {:.0f} units   3D loaded {}", a->GetDistance(a_player), a->Is3DLoaded()));
			}
			o.L(std::format("  state      dead {} | disabled {} | in combat {} | hostile to player {} | essential {} | protected {} | AI enabled {} | "
							"teammate {} | summoned {} | sneaking {} | mounted {} | weapon drawn {} | ghost {}",
				a->IsDead(false), a->IsDisabled(), a->IsInCombat(), (a_isPlayer || !a->Is3DLoaded()) ? false : a->IsHostileToActor(a_player), a->IsEssential(),
				a->IsProtected(), a->IsAIEnabled(), a->IsPlayerTeammate(), a->IsSummoned(), a->IsSneaking(), a->IsOnMount(),
				a->AsActorState() ? a->AsActorState()->IsWeaponDrawn() : false, a->IsGhost()));
			if (const auto target = a->GetActorRuntimeData().currentCombatTarget.get()) {
				o.L("  combat target " + RefLabel(target.get()));
			}
			if (!a_isPlayer) {
				const auto pkg = a->GetCurrentPackage();
				o.L("  CURRENT PACKAGE " + (pkg ? Label(pkg) + "  type " + S(pkg->GetObjectTypeName()) : std::string("(none)")));
				o.L("  packages on NPC (in priority order):");
				if (base) {
					for (auto p : base->aiPackages.packages) {
						if (p) {
							o.L("    " + Label(p) + "  type " + S(p->GetObjectTypeName()));
						}
					}
				}
			}
			o.L("  factions (base, then script changes):");
			Factions(o, a);
			o.L("  stats (current / base / permanent):");
			if (const auto av = a->AsActorValueOwner()) {
				for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::ActorValue::kTotal); ++i) {
					const auto v = static_cast<RE::ActorValue>(i);
					o.L(std::format("    {:<28} {:>10.2f} {:>10.2f} {:>10.2f}", S(RE::ActorValueList::GetActorValueName(v)), av->GetActorValue(v),
						av->GetBaseActorValue(v), av->GetPermanentActorValue(v)));
				}
			}
			o.L("  equipped: right " + Label(a->GetEquippedObject(false)) + " | left " + Label(a->GetEquippedObject(true)));
			o.L("  spells:");
			if (base && base->actorEffects) {
				for (std::uint32_t i = 0; i < base->actorEffects->numSpells; ++i) {
					if (const auto sp = base->actorEffects->spells[i]) {
						o.L("    (base) " + Label(sp));
					}
				}
			}
			for (auto sp : a->GetActorRuntimeData().addedSpells) {
				if (sp) {
					o.L("    (added) " + Label(sp));
				}
			}
			o.L("  active magic effects:");
			if (const auto mt = a->AsMagicTarget()) {
				if (const auto list = mt->GetActiveEffectList()) {
					for (auto ae : *list) {
						if (!ae) {
							continue;
						}
						o.L(std::format("    {} from {}  magnitude {:.1f}  {:.0f}/{:.0f} s{}", Label(ae->GetBaseObject()), Label(ae->spell), ae->magnitude,
							ae->elapsedSeconds, ae->duration, ae->flags.any(RE::ActiveEffect::Flag::kInactive) ? "  (inactive)" : ""));
					}
				}
			}
		}
	}

	std::filesystem::path Write(std::string& a_error, std::size_t& a_lines)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto dh = RE::TESDataHandler::GetSingleton();
		if (!player || !dh || !player->GetParentCell()) {
			a_error = "no game loaded (load a save first)";
			return {};
		}
		const auto dir = SKSE::log::log_directory();
		if (!dir) {
			a_error = "no folder for the report";
			return {};
		}
		const auto      folder = *dir / "ReSaverHelper";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		SYSTEMTIME t{};
		GetLocalTime(&t);
		const auto path =
			folder / std::format("report_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		Out o;
		o.f.open(path, std::ios::out | std::ios::trunc);
		if (!o.f) {
			a_error = "cannot write " + path.string();
			return {};
		}
		spdlog::info("Report: writing {}", path.string());

		o.L("Game state report (ReSaverHelper, the same report as the Snapshot mod)");
		o.L(std::format("Taken {:04}-{:02}-{:02} {:02}:{:02}:{:02}   game runtime {}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
			REL::Module::get().version().string()));
		if (const auto cal = RE::Calendar::GetSingleton()) {
			char buf[256]{};
			cal->GetTimeDateString(buf, sizeof(buf), true);
			o.L(std::format("Game date: {}   days passed {:.2f}   hour {:.2f}   timescale {:.0f}", buf, cal->GetDaysPassed(), cal->GetHour(), cal->GetTimescale()));
		}
		if (const auto sky = RE::Sky::GetSingleton()) {
			o.L("Weather: " + Label(sky->currentWeather));
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			std::string menus;
			for (auto m : { "Console", "Dialogue Menu", "InventoryMenu", "ContainerMenu", "BarterMenu", "MapMenu", "Journal Menu", "Sleep/Wait Menu",
					 "MessageBoxMenu", "Lockpicking Menu", "Crafting Menu", "Book Menu", "TweenMenu", "StatsMenu", "MagicMenu", "FavoritesMenu", "Loading Menu",
					 "Training Menu", "GiftMenu" }) {
				if (ui->IsMenuOpen(m)) {
					menus += std::string(menus.empty() ? "" : ", ") + m;
				}
			}
			o.L("Open menus: " + (menus.empty() ? std::string("none") : menus));
		}

		// ---- player
		o.H("PLAYER");
		ActorDetail(o, player, player, true);
		o.L("  perks:");
		for (auto perk : dh->GetFormArray<RE::BGSPerk>()) {
			if (perk && player->HasPerk(perk)) {
				o.L("    " + Label(perk));
			}
		}
		o.L("  bounties (crime gold per crime faction):");
		for (auto fac : dh->GetFormArray<RE::TESFaction>()) {
			if (fac && fac->TracksCrimes()) {
				if (const auto gold = player->GetCrimeGoldValue(fac); gold > 0) {
					o.L(std::format("    {}  {} gold", Label(fac), gold));
				}
			}
		}
		o.L("  inventory (count, worn):");
		for (auto& [obj, data] : player->GetInventory()) {
			o.L(std::format("    {:>6}  {}{}", data.first, Label(obj), data.second && data.second->IsWorn() ? "  [WORN]" : ""));
		}

		// ---- followers
		const auto        actors = AllActors();
		static const auto followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);  // CurrentFollowerFaction
		o.H("FOLLOWERS (player teammates, CurrentFollowerFaction members, summons)");
		for (auto a : actors) {
			if (a == player) {
				continue;
			}
			if (a->IsPlayerTeammate() || a->IsSummonedByPlayer() || (followerFaction && a->IsInFaction(followerFaction))) {
				ActorDetail(o, a, player, false);
			}
		}

		// ---- nearby
		const auto pcell = player->GetParentCell();
		const bool inside = pcell && pcell->IsInteriorCell();
		o.H(inside ? "NPCs IN THE SAME INTERIOR (dungeon / building) AS THE PLAYER" : std::format("NPCs WITHIN {:.0f} UNITS OF THE PLAYER (outdoors)", kRadius));
		int nearby = 0;
		for (auto a : actors) {
			if (a == player || a->IsDisabled()) {
				continue;
			}
			const bool hit = inside ? a->GetParentCell() == pcell :
			                          (a->GetParentCell() && !a->GetParentCell()->IsInteriorCell() && a->GetWorldspace() == player->GetWorldspace() &&
										  a->GetDistance(player) <= kRadius);
			if (hit) {
				ActorDetail(o, a, player, false);
				++nearby;
			}
		}
		o.L(std::format("{} NPCs listed", nearby));

		// ---- quests
		o.H("RUNNING QUESTS (full detail)");
		std::vector<RE::TESQuest*> completed;
		for (auto q : dh->GetFormArray<RE::TESQuest>()) {
			if (!q) {
				continue;
			}
			if (q->IsCompleted()) {
				completed.push_back(q);
			}
			// running = the quest's Enabled flag (what Papyrus IsRunning reports); also quests caught mid start/stop
			if (!(q->IsEnabled() || q->IsStarting() || q->IsStopping())) {
				continue;
			}
			o.L("--------------------------------------------------------------------------------");
			o.L("QUEST " + Label(q));
			o.L(std::format("  stage {} | running {} | starting {} | stopping {} | completed {} | failed {} | active in journal {} | already run {} | type {} | flags {:04X}",
				q->GetCurrentStageID(), q->IsEnabled(), q->IsStarting(), q->IsStopping(), q->IsCompleted(), q->data.flags.all(RE::QuestFlag::kFailed), q->IsActive(),
				q->alreadyRun, static_cast<int>(q->GetType()), static_cast<std::uint16_t>(q->data.flags.underlying())));
			for (auto obj : q->objectives) {
				if (obj) {
					o.L(std::format("  objective {:>4}  {:<18} {}", obj->index, ObjState(obj->state.get()), obj->displayText.c_str() ? obj->displayText.c_str() : ""));
				}
			}
			for (auto sc : q->scenes) {
				if (sc && sc->isPlaying) {
					o.L("  scene PLAYING: " + Label(sc));
				}
			}
			RE::BSReadLockGuard l{ q->aliasAccessLock };
			for (auto al : q->aliases) {
				if (!al) {
					continue;
				}
				std::string filled = "(empty / not a reference alias)";
				if (const auto it = q->refAliasMap.find(al->aliasID); it != q->refAliasMap.end()) {
					if (const auto r = it->second.get()) {
						filled = RefLabel(r.get());
						if (r->GetParentCell()) {
							filled += "  @ " + Where(r.get());
						}
					}
				}
				o.L(std::format("  alias {:>3} {:<32} {}", al->aliasID, al->aliasName.c_str() ? al->aliasName.c_str() : "", filled));
			}
		}
		o.H("COMPLETED QUESTS");
		for (auto q : completed) {
			o.L(std::format("  stage {:>4}  {}", q->GetCurrentStageID(), Label(q)));
		}

		// ---- every NPC's factions
		o.H(std::format("EVERY NPC IN MEMORY ({} actors): location and factions", actors.size()));
		for (auto a : actors) {
			o.L(RefLabel(a) + std::format("  | dead {} disabled {}", a->IsDead(false), a->IsDisabled()));
			o.L("    " + Where(a));
			Factions(o, a);
		}

		// ---- load order and SKSE plugins
		o.H("LOAD ORDER");
		// (GetLoadedMods / GetLoadedLightMods: the multi-runtime CommonLib's way to the compiled file lists, SE / AE / VR)
		if (const auto* mods = dh->GetLoadedMods()) {
			for (std::uint32_t i = 0; i < dh->GetLoadedModCount(); ++i) {
				if (const auto* f = mods[i]) {
					o.L(std::format("  {:02X}     {}", f->GetCompileIndex(), f->GetFilename()));
				}
			}
		}
		if (const auto* small = dh->GetLoadedLightMods()) {
			for (std::uint32_t i = 0; i < dh->GetLoadedLightModCount(); ++i) {
				if (const auto* f = small[i]) {
					o.L(std::format("  FE {:03X} {}", f->GetSmallFileCompileIndex(), f->GetFilename()));
				}
			}
		}
		o.H("SKSE PLUGINS (Data\\SKSE\\Plugins\\*.dll)");
		for (auto& e : std::filesystem::directory_iterator("Data\\SKSE\\Plugins", ec)) {
			if (e.is_regular_file(ec) && e.path().extension() == ".dll") {
				o.L(std::format("  {:<48} {:>10} bytes", e.path().filename().string(), e.file_size(ec)));
			}
		}
		o.L("\n(end of report)");
		o.f.close();
		a_lines = o.lines;
		spdlog::info("Report: {} lines written", a_lines);
		return path;
	}
}
