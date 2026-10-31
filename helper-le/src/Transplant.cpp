#include "PCH.h"
#include "Transplant.h"
#include "Game.h"
#include "LE.h"
#include "Papyrus.h"
#include "Protocol.h"

// Transplant for Skyrim LE: the same snapshot file and records as the SE helper (Transplant.h), read
// from SKSE 1.7.3's game classes and applied through Papyrus (the game's own functions), so nothing is
// written into game memory that a script could not change too. Differences from SE, each one logged:
//  - no "marker" records in a snapshot (LE's headers have no list of every reference); a save file
//    still gives them to ReSaver++
//  - no "jobj" records (the player's journal list is not in LE's headers): ReSaver++ uses the quests'
//    objectives instead; "questobj" shows objectives through Papyrus
//  - potions, poisons and enchantments made in the game cannot be made again (no LE function for it);
//    an item that carried one comes over without it
//  - a poison on a weapon is not put back; a weapon's custom name, temper and charge are
using papyrus::Arg;

namespace Transplant
{
	namespace
	{
		using Record = std::vector<std::string>;

		constexpr std::pair<const char*, UInt32> kAVs[] = {
			{ "Health", 24 },
			{ "Magicka", 25 },
			{ "Stamina", 26 },
			{ "CarryWeight", 32 },
			{ "DragonSouls", 133 },
		};
		constexpr int    kSkills = 18;  // One-handed ... Enchanting: actor values 6..23
		constexpr UInt32 kFirstSkill = 6;

		std::string F(float a_v) { return std::format("{:.4f}", a_v); }

		std::uint32_t Hex(const std::string& s)
		{
			try {
				return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
			} catch (...) {
				return 0;
			}
		}
		float Num(const std::string& s)
		{
			try {
				return std::stof(s);
			} catch (...) {
				return 0.0f;
			}
		}

		std::vector<std::string> Split(const std::string& s, char c)
		{
			std::vector<std::string> out;
			std::string              cur;
			for (char ch : s) {
				if (ch == c) {
					out.push_back(cur);
					cur.clear();
				} else {
					cur += ch;
				}
			}
			out.push_back(cur);
			return out;
		}

		// plugin, local id, runtime id
		void PutForm(Record& r, const TESForm* a_form)
		{
			r.push_back(le::PluginOf(a_form));
			r.push_back(std::format("{:06X}", le::LocalOf(a_form)));
			r.push_back(std::format("{:08X}", a_form ? a_form->formID : 0));
		}

		TESForm* Lookup(const std::string& a_plugin, const std::string& a_local) { return le::Lookup(a_plugin, a_local); }

		template <class T>
		T* As(TESForm* a_form)
		{
			return a_form && a_form->formType == T::kTypeID ? static_cast<T*>(a_form) : nullptr;
		}

		// "<plugin>:<local>:<magnitude>:<area>:<duration>" for each effect, comma separated
		std::string Effects(MagicItem* a_item)
		{
			std::string out;
			if (!a_item) {
				return out;
			}
			for (UInt32 i = 0; i < a_item->effectItemList.count; ++i) {
				const auto* e = a_item->effectItemList.arr.entries[i];
				if (!e || !e->mgef) {
					continue;
				}
				out += std::format("{}{}:{:06X}:{}:{}:{}", out.empty() ? "" : ",", le::PluginOf(e->mgef), le::LocalOf(e->mgef), e->magnitude, e->area, e->duration);
			}
			return out;
		}

		// an enchantment or a poison on an item
		std::string MagicField(MagicItem* a_item, char a_kind, std::int64_t a_amount)
		{
			if (!a_item) {
				return {};
			}
			if (le::IsDynamic(a_item)) {
				return std::format("C:{}:{}:{}", a_kind, a_amount, Effects(a_item));
			}
			return std::format("F:{}:{:06X}:{}", le::PluginOf(a_item), le::LocalOf(a_item), a_amount);
		}

		std::string KindOf(TESForm* o)
		{
			if (!o) {
				return "Other";
			}
			if (o->formID == 0xF) {
				return "Gold";
			}
			switch (o->formType) {
			case kFormType_Weapon: return "Weapons";
			case kFormType_Armor: return "Armor and clothing";
			case kFormType_Ammo: return "Arrows and bolts";
			case kFormType_Book: return "Books and spell tomes";
			case kFormType_Potion: return "Potions and food";
			case kFormType_Ingredient: return "Ingredients";
			case kFormType_SoulGem: return "Soul gems";
			case kFormType_Key: return "Keys";
			case kFormType_ScrollItem: return "Scrolls";
			case kFormType_Light: return "Torches and lights";
			case kFormType_Misc: return "Miscellaneous";
			default: return "Other";
			}
		}

		const char* DisplayName(BaseExtraList* a_xl)
		{
			auto* x = a_xl ? static_cast<ExtraTextDisplayData*>(a_xl->GetByType(kExtraData_TextDisplayData)) : nullptr;
			return x && x->name.data && *x->name.data ? x->name.data : nullptr;
		}

		// ---- a chain of Papyrus calls, one after the other -----------------------------------------
		// each step gets "next" and calls it when its call came back (or at once when it could not call)
		using Step = std::function<void(std::function<void()>)>;
		void RunChain(std::shared_ptr<std::vector<Step>> a_steps, std::size_t a_i = 0)
		{
			if (a_i >= a_steps->size()) {
				return;
			}
			(*a_steps)[a_i]([a_steps, a_i] { RunChain(a_steps, a_i + 1); });
		}
		Step Call(TESForm* a_obj, const char* a_class, const char* a_fn, papyrus::Args a_args)
		{
			const auto id = a_obj ? a_obj->formID : 0;
			std::string cls = a_class, fn = a_fn;
			return [id, cls, fn, a_args](std::function<void()> a_next) {
				auto* obj = le::Lookup(id);
				if (!papyrus::CallMethod(obj, cls.c_str(), fn.c_str(), a_args, [a_next](const papyrus::Result&) { a_next(); })) {
					spdlog::warn("Transplant: could not call {}.{} on {:08X}", cls, fn, id);
					a_next();
				}
			};
		}
	}

	// ---- snapshot ---------------------------------------------------------------------------------

	std::filesystem::path Snapshot(std::string& a_error, std::size_t& a_lines)
	{
		auto* player = le::Player();
		auto* dh = le::Data();
		spdlog::info("Snapshot: starting (Legendary Edition)");
		if (!player || !dh) {
			a_error = "no player (load a save first)";
			return {};
		}
		std::vector<Record> out;
		auto*               base = le::BaseNPC(player);
		std::string         playerName = le::NameOf(base);
		if (playerName.empty()) {
			playerName = le::NameOf(player);
		}
		std::string safe;
		for (char c : playerName) {
			safe += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
		}
		if (safe.empty()) {
			safe = "player";
		}
		SYSTEMTIME t{};
		GetLocalTime(&t);
		const auto stamp = std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		out.push_back({ "RSH-SNAPSHOT", "1", "1.9.32.0", playerName });

		auto section = [](const char* a_name, auto&& a_fn) {
			try {
				a_fn();
			} catch (const std::exception& e) {
				spdlog::error("Snapshot: {} failed: {}", a_name, e.what());
			} catch (...) {
				spdlog::error("Snapshot: {} failed (the game data could not be read); left out", a_name);
			}
		};

		section("plugins", [&] {
			for (UInt32 i = 0; i < dh->modList.loadedModCount && i < 0xFF; ++i) {
				if (const auto* mod = dh->modList.loadedMods[i]) {
					out.push_back({ "plugin", std::string(mod->name) });
				}
			}
		});

		section("level and skills", [&] {
			auto* sd = player->skills ? player->skills->data : nullptr;
			out.push_back({ "player", std::to_string(le::GetLevel(player)), F(sd ? sd->levelPoints : 0.0f), F(sd ? sd->levelPointsMax : 0.0f),
				std::to_string(player->numPerkPoints) });
			for (int i = 0; i < kSkills; ++i) {
				out.push_back({ "skill", std::to_string(i), F(player->actorValueOwner.GetBase(kFirstSkill + i)), F(sd ? sd->levelData[i].points : 0.0f),
					F(sd ? sd->levelData[i].pointsMax : 0.0f), std::to_string(sd ? sd->legendaryLevel[i] : 0) });
			}
			for (const auto& [name, av] : kAVs) {
				out.push_back({ "av", name, F(player->actorValueOwner.GetBase(av)) });
			}
		});

		section("perks", [&] {
			for (UInt32 i = 0; i < dh->perks.count; ++i) {
				auto* perk = dh->perks.arr.entries[i];
				if (perk && le::HasPerk(player, perk)) {
					Record r{ "perk" };
					PutForm(r, perk);
					r.push_back(le::NameOf(perk));
					out.push_back(std::move(r));
				}
			}
		});

		section("spells", [&] {
			for (UInt32 i = 0; i < dh->spellItems.count; ++i) {
				auto* spell = dh->spellItems.arr.entries[i];
				if (!spell) {
					continue;
				}
				const auto type = spell->data.type;
				if ((type == SpellItem::kTypeSpell || type == SpellItem::kTypePower || type == SpellItem::kTypeLesserPower) && le::HasSpell(player, spell)) {
					Record r{ "spell" };
					PutForm(r, spell);
					r.push_back(le::NameOf(spell));
					out.push_back(std::move(r));
				}
			}
		});

		section("shouts and words", [&] {
			if (base && base->spellList.unk04) {
				const auto* d = base->spellList.unk04;
				for (UInt32 i = 0; d->shouts && i < d->numShouts; ++i) {
					if (auto* shout = d->shouts[i]) {
						Record r{ "shout" };
						PutForm(r, shout);
						r.push_back(le::NameOf(shout));
						out.push_back(std::move(r));
					}
				}
			}
			for (UInt32 i = 0; i < dh->wordOfPowers.count; ++i) {
				auto* word = dh->wordOfPowers.arr.entries[i];
				if (!word) {
					continue;
				}
				const bool known = (word->flags & 0x40) != 0;    // TESForm::RecordFlags::kKnown
				const bool unlocked = (word->flags & 0x01) != 0;  // kUnlocked
				if (known || unlocked) {
					Record r{ "word" };
					PutForm(r, word);
					r.push_back(le::NameOf(word));
					r.push_back(known ? "1" : "0");
					r.push_back(unlocked ? "1" : "0");
					out.push_back(std::move(r));
				}
			}
		});

		section("factions", [&] {
			for (const auto& [faction, rank] : le::Factions(player)) {
				if (!le::IsDynamic(faction)) {
					Record r{ "faction" };
					PutForm(r, faction);
					r.push_back(le::NameOf(faction));
					r.push_back(std::to_string(rank));
					out.push_back(std::move(r));
				}
			}
		});
		spdlog::info("Snapshot: map markers are not read on Legendary Edition (a save file gives them)");

		section("quests", [&] {
			for (UInt32 i = 0; i < dh->quests.count; ++i) {
				auto* quest = dh->quests.arr.entries[i];
				if (!quest) {
					continue;
				}
				std::string objectives;
				for (const auto& [index, st] : le::Objectives(quest)) {
					if (st != 0) {
						objectives += std::format("{}{}:{}", objectives.empty() ? "" : ",", index, st);
					}
				}
				Record r{ "quest" };
				PutForm(r, quest);
				r.push_back(le::EditorID(quest) + "|" + le::NameOf(quest));
				r.push_back(le::Running(quest) ? "1" : "0");
				r.push_back(le::Completed(quest) ? "1" : "0");
				r.push_back(std::to_string(quest->GetCurrentStageID()));
				r.push_back("");
				r.push_back(objectives);
				r.push_back(std::to_string(quest->unk07C.type));
				out.push_back(std::move(r));
			}
		});

		// followers: the characters the running quests' aliases hold (follower mods' own systems and the
		// vanilla follower quest) that follow the player: a teammate or in the follower faction
		section("followers", [&] {
			auto*                   followerFaction = As<TESFaction>(le::Lookup("Skyrim.esm", 0x05C84E));
			auto*                   dialogueFollower = le::Lookup("Skyrim.esm", 0x0750BA);
			std::set<std::uint32_t> seen;
			for (UInt32 qi = 0; qi < dh->quests.count; ++qi) {
				auto* quest = dh->quests.arr.entries[qi];
				if (!quest || !le::Running(quest) || le::IsDynamic(quest)) {
					continue;
				}
				for (UInt32 ai = 0; ai < quest->aliases.count; ++ai) {
					auto* alias = quest->aliases.arr.entries[ai];
					auto* refAlias = alias ? le::Cast<BGSRefAlias>(alias, RTTI_BGSBaseAlias, RTTI_BGSRefAlias) : nullptr;
					auto* actor = refAlias ? refAlias->GetActorReference() : nullptr;
					if (!actor || actor == player || le::IsDynamic(actor) || !seen.insert(actor->formID).second || actor->IsDead(1)) {
						continue;
					}
					const int   rank = followerFaction ? le::FactionRank(actor, followerFaction) : -1;
					std::string aliases;
					bool        inVanilla = false;
					if (auto* xa = static_cast<ExtraAliasInstanceArray*>(actor->extraData.GetByType(kExtraData_AliasInstanceArray))) {
						for (UInt32 k = 0; k < xa->aliases.count; ++k) {
							auto* inst = xa->aliases.arr.entries[k];
							if (!inst || !inst->quest || !inst->alias || le::IsDynamic(inst->quest)) {
								continue;
							}
							inVanilla = inVanilla || inst->quest == dialogueFollower;
							aliases += std::format("{}{}*{:06X}*{}", aliases.empty() ? "" : "|", le::PluginOf(inst->quest), le::LocalOf(inst->quest), inst->alias->aliasId);
						}
					}
					if (!le::IsTeammate(actor) && rank < 0 && !inVanilla) {
						continue;
					}
					std::string items;
					for (const auto& it : le::Inventory(actor)) {
						if (it.form && it.count > 0 && !le::IsDynamic(it.form) && !le::PluginOf(it.form).empty()) {
							items += std::format("{}{}*{:06X}*{}", items.empty() ? "" : "|", le::PluginOf(it.form), le::LocalOf(it.form), it.count);
						}
					}
					Record r{ "follower" };
					PutForm(r, actor);
					const auto name = le::NameOf(le::BaseNPC(actor));
					r.push_back(name.empty() ? le::NameOf(actor) : name);
					r.push_back(le::IsTeammate(actor) ? "1" : "0");
					r.push_back(std::to_string(rank));
					r.push_back(aliases);
					r.push_back(items);
					out.push_back(std::move(r));
				}
			}
		});

		section("inventory", [&] {
			for (const auto& it : le::Inventory(player)) {
				auto* obj = it.form;
				if (!obj || it.count <= 0) {
					continue;
				}
				if (le::IsDynamic(obj)) {
					if (auto* alch = As<AlchemyItem>(obj)) {
						out.push_back({ "potion", le::NameOf(alch), std::to_string(it.count), alch->IsPoison() ? "1" : "0", Effects(alch) });
					}
					continue;
				}
				bool questItem = false;
				for (auto* xl : it.extras) {
					questItem = questItem || (xl && xl->HasType(kExtraData_AliasInstanceArray));
				}
				SInt32 special = 0;
				for (auto* xl : it.extras) {
					if (!xl) {
						continue;
					}
					const char* name = DisplayName(xl);
					auto*       xHealth = static_cast<ExtraHealth*>(xl->GetByType(kExtraData_Health));
					auto*       xCharge = static_cast<ExtraCharge*>(xl->GetByType(kExtraData_Charge));
					auto*       xEnch = static_cast<ExtraEnchantment*>(xl->GetByType(kExtraData_Enchantment));
					auto*       xPoison = static_cast<ExtraPoison*>(xl->GetByType(kExtraData_Poison));
					if (!name && !xHealth && !xCharge && !xEnch && !xPoison) {
						continue;
					}
					auto*      xCount = static_cast<ExtraCount*>(xl->GetByType(kExtraData_Count));
					const auto n = xCount ? static_cast<SInt32>(xCount->count) : 1;
					special += n;
					Record r{ "item" };
					PutForm(r, obj);
					r.push_back(le::NameOf(obj));
					r.push_back(std::to_string(n));
					r.push_back(name ? name : "");
					r.push_back(xHealth ? F(xHealth->health) : "");
					r.push_back(xCharge ? F(xCharge->charge) : "");
					r.push_back(xEnch ? MagicField(xEnch->enchant, obj->formType == kFormType_Weapon ? 'w' : 'a', xEnch->maxCharge) : "");
					r.push_back(xPoison ? MagicField(xPoison->poison, 'p', xPoison->unk08) : "");
					r.push_back(questItem ? "1" : "0");
					r.push_back(KindOf(obj));
					out.push_back(std::move(r));
				}
				if (it.count - special > 0) {
					Record r{ "item" };
					PutForm(r, obj);
					r.push_back(le::NameOf(obj));
					r.push_back(std::to_string(it.count - special));
					r.insert(r.end(), { "", "", "", "", "", questItem ? "1" : "0", KindOf(obj) });
					out.push_back(std::move(r));
				}
			}
		});

		section("equipped", [&] {
			for (const auto& it : le::Inventory(player)) {
				if (!it.form || le::IsDynamic(it.form)) {
					continue;
				}
				for (auto* xl : it.extras) {
					if (!xl) {
						continue;
					}
					const bool right = xl->HasType(kExtraData_Worn), left = xl->HasType(kExtraData_WornLeft);
					if (!right && !left) {
						continue;
					}
					const char* name = DisplayName(xl);
					Record      r{ "equip" };
					PutForm(r, it.form);
					r.push_back(le::NameOf(it.form));
					r.push_back(left ? "left" : "right");
					r.push_back(name ? name : "");
					out.push_back(std::move(r));
				}
			}
			for (const bool left : { false, true }) {
				auto* f = player->GetEquippedObject(left);
				if (auto* spell = As<SpellItem>(f); spell && !le::IsDynamic(spell)) {
					Record r{ "equipspell" };
					PutForm(r, spell);
					r.push_back(le::NameOf(spell));
					r.push_back(left ? "left" : "right");
					out.push_back(std::move(r));
				}
			}
			if (auto* power = player->equippedShout; power && !le::IsDynamic(power)) {
				Record r{ power->formType == kFormType_Shout ? "equipshout" : "equipspell" };
				PutForm(r, power);
				r.push_back(le::NameOf(power));
				r.push_back("voice");
				out.push_back(std::move(r));
			}
		});

		// the look: RaceMenu saves it as a preset, loaded again by "look"
		if (GetModuleHandleW(L"skee.dll")) {
			const auto preset = std::format("ReSaverPP_{}_{}", safe, stamp);
			if (papyrus::CallStatic("CharGen", "SavePreset", { Arg::String(preset) })) {
				out.push_back({ "look", preset, "RaceMenu" });
			}
		}

		const auto dir = rshlog::Dir();
		if (dir.empty()) {
			a_error = "no folder for the snapshot";
			return {};
		}
		const auto      folder = dir / L"ReSaverHelper";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		const auto    path = folder / std::format("snapshot_{}_{}.txt", safe, stamp);
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		if (!f) {
			a_error = "cannot write " + path.string();
			spdlog::error("Snapshot: {}", a_error);
			return {};
		}
		for (const auto& r : out) {
			f << proto::Join(r) << "\n";
		}
		a_lines = out.size();
		spdlog::info("Snapshot: {} record(s) written to {}", out.size(), path.string());
		return path;
	}

	// ---- apply ------------------------------------------------------------------------------------

	std::string Apply(const Record& a)
	{
		auto* player = le::Player();
		if (!player || a.empty()) {
			return "no player";
		}
		auto        arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string(); };
		const auto& kind = a[0];
		try {
			if (kind == "player") {
				const int level = static_cast<int>(Num(arg(1)));
				if (auto* base = le::BaseNPC(player); base && level > le::GetLevel(player)) {
					base->actorData.level = static_cast<UInt16>((std::min)(level, 0xFFFF));
				}
				auto* sd = player->skills ? player->skills->data : nullptr;
				if (sd && !arg(2).empty()) {
					sd->levelPoints = Num(arg(2));
					if (Num(arg(3)) > 0) {
						sd->levelPointsMax = Num(arg(3));
					}
				}
				if (!arg(4).empty()) {
					const auto points = static_cast<UInt8>(std::clamp(static_cast<int>(Num(arg(4))), 0, 255));
					if (points > player->numPerkPoints) {
						player->numPerkPoints = points;
					}
				}
				return {};
			}
			if (kind == "skill") {
				const int i = static_cast<int>(Num(arg(1)));
				if (i < 0 || i >= kSkills) {
					return "bad skill";
				}
				const float level = Num(arg(2));
				if (level > player->actorValueOwner.GetBase(kFirstSkill + i)) {
					player->actorValueOwner.SetBase(kFirstSkill + i, level);
				}
				if (auto* sd = player->skills ? player->skills->data : nullptr) {
					sd->levelData[i].points = Num(arg(3));
					if (Num(arg(4)) > 0) {
						sd->levelData[i].pointsMax = Num(arg(4));
					}
					sd->legendaryLevel[i] = (std::max)(sd->legendaryLevel[i], static_cast<UInt32>(Num(arg(5))));
				}
				return {};
			}
			if (kind == "av") {
				for (const auto& [name, av] : kAVs) {
					if (arg(1) == name) {
						const float v = Num(arg(2));
						if (v > player->actorValueOwner.GetBase(av)) {
							player->actorValueOwner.SetBase(av, v);
						}
						return {};
					}
				}
				return "unknown value " + arg(1);
			}
			if (kind == "potion") {
				return "potions and poisons made in the game cannot be made again on Legendary Edition";
			}
			if (kind == "follower") {
			// fields: 1 plugin, 2 local id, 3 runtime id, 4 name, 5 teammate, 6 follower faction rank, 7 aliases, 8 items
				auto* actor = As<Character>(Lookup(arg(1), arg(2)));
				if (!actor) {
					return "not a character in this game";
				}
				auto        steps = std::make_shared<std::vector<Step>>();
				std::string notes;
				if ((actor->flags & 0x800) != 0) {  // disabled
					steps->push_back(Call(actor, "ObjectReference", "Enable", { Arg::Bool(false) }));
					notes += "enabled; ";
				}
				if (actor->IsDead(1)) {
					steps->push_back(Call(actor, "Actor", "Resurrect", {}));
					notes += "brought back to life; ";
				}
				// what they carry
				int added = 0;
				{
					std::map<TESForm*, SInt32> have;
					for (const auto& it : le::Inventory(actor)) {
						have[it.form] += it.count;
					}
					for (const auto& one : Split(arg(8), '|')) {
						const auto p = Split(one, '*');
						if (p.size() != 3) {
							continue;
						}
						auto* item = Lookup(p[0], p[1]);
						if (!item) {
							continue;
						}
						const int want = static_cast<int>(Num(p[2]));
						const int now = have.count(item) ? have[item] : 0;
						if (want > now) {
							steps->push_back(Call(actor, "ObjectReference", "AddItem", { Arg::Form(item), Arg::Int(want - now), Arg::Bool(true) }));
							added += want - now;
						}
					}
				}
				if (added) {
					notes += std::format("{} item(s) given back; ", added);
				}
				if (const int rank = static_cast<int>(Num(arg(6))); rank >= 0) {
					if (auto* fac = As<TESFaction>(le::Lookup("Skyrim.esm", 0x05C84E))) {
						steps->push_back(Call(actor, "Actor", "SetFactionRank", { Arg::Form(fac), Arg::Int(rank) }));
					}
				}
				int forced = 0;
				for (const auto& one : Split(arg(7), '|')) {
					const auto p = Split(one, '*');
					if (p.size() != 3) {
						continue;
					}
					auto* q = As<TESQuest>(Lookup(p[0], p[1]));
					if (!q) {
						continue;
					}
					if (!le::Running(q)) {
						notes += std::format("{}'s quest {} is not running; ", le::NameOf(actor), le::EditorID(q));
						continue;
					}
					CALL_MEMBER_FN(q, ForceRefTo)(static_cast<UInt32>(Num(p[2])), actor);
					++forced;
					if (q->formID == 0x000750BA) {  // the vanilla follower quest: one follower taken
						if (auto* glob = As<TESGlobal>(le::Lookup("Skyrim.esm", 0x0BCCAE))) {
							glob->value = 1.0f;
						}
					}
				}
				if (arg(5) == "1") {
					steps->push_back(Call(actor, "Actor", "SetPlayerTeammate", { Arg::Bool(true), Arg::Bool(true) }));
				}
				steps->push_back(Call(actor, "ObjectReference", "MoveTo",
					{ Arg::Form(player), Arg::Float(0.0f), Arg::Float(0.0f), Arg::Float(0.0f), Arg::Bool(true) }));
				steps->push_back(Call(actor, "Actor", "EvaluatePackage", {}));
				RunChain(steps);
				spdlog::info("follower {:08X} ({}): {}{} alias(es) set", actor->formID, le::NameOf(actor), notes, forced);
				return {};
			}
			if (kind == "controls") {
				// after the quests: a stage script can leave the player's controls off (a scene that never plays)
				papyrus::Args on;
				for (int i = 0; i < 8; ++i) {
					on.push_back(Arg::Bool(true));
				}
				on.push_back(Arg::Int(0));
				papyrus::CallStatic("Game", "EnablePlayerControls", on);
				return {};
			}
			if (kind == "questobj") {
				// shows the quest's objectives the way the other save had them, through Papyrus (LE: the
				// journal list cannot be written directly). "start": start the quest first if needed.
				auto* q = As<TESQuest>(Lookup(arg(1), arg(2)));
				if (!q) {
					return "not a quest in this game";
				}
				struct ObjStep
				{
					SInt32 index;
					int    state;
				};
				auto objs = std::make_shared<std::vector<ObjStep>>();
				for (const auto& one : Split(arg(4), ',')) {
					const auto p = Split(one, ':');
					if (p.size() == 2) {
						objs->push_back({ static_cast<SInt32>(Num(p[0])), static_cast<int>(Num(p[1])) });
					}
				}
				if (objs->empty()) {
					return {};
				}
				const auto id = q->formID;
				auto       show = [id, objs]() {
                    auto*      qq = le::LookupAs<TESQuest>(id);
                    const bool live = le::Running(qq), finished = le::Completed(qq);
                    auto       steps = std::make_shared<std::vector<Step>>();
                    int        skipped = 0;
                    for (const auto& st : *objs) {
                        if (!(st.state & 1)) {
                            continue;  // never shown in the other save
                        }
                        // only a running quest, or a completed one (its finished objectives), may be listed:
                        // a quest that did not start has no people or places set up for its objectives
                        if (!live && !(finished && st.state != 1)) {
                            ++skipped;
                            continue;
                        }
                        steps->push_back(Call(qq, "Quest", "SetObjectiveDisplayed", { Arg::Int(st.index), Arg::Bool(true), Arg::Bool(false) }));
                        if (st.state & 2) {
                            steps->push_back(Call(qq, "Quest", "SetObjectiveCompleted", { Arg::Int(st.index), Arg::Bool(true) }));
                        }
                        if (st.state & 4) {
                            steps->push_back(Call(qq, "Quest", "SetObjectiveFailed", { Arg::Int(st.index), Arg::Bool(true) }));
                        }
                    }
                    spdlog::info("questobj {:08X} ({}): running {}, completed {}; {} objective call(s), {} left out (the quest is not running)", id,
                        le::EditorID(qq), live, finished, steps->size(), skipped);
                    RunChain(steps);
				};
				if (arg(3) == "start" && !le::Running(q)) {
					if (papyrus::CallMethod(q, "Quest", "Start", {}, [id, show](const papyrus::Result& a_r) {
							spdlog::info("questobj {:08X}: Start() returned {}", id, a_r.IsBool() ? (a_r.GetBool() ? "true" : "false") : "?");
							show();
						})) {
						return {};
					}
					spdlog::info("questobj {:08X}: could not call Start()", id);
				}
				show();
				return {};
			}
			if (kind == "look") {
				if (!GetModuleHandleW(L"skee.dll")) {
					return "RaceMenu is not installed in this game";
				}
				const auto pid = player->formID;
				papyrus::CallStatic("CharGen", "LoadPreset", { Arg::String(arg(1)) }, [pid](const papyrus::Result&) {
					papyrus::CallMethod(le::Lookup(pid), "Actor", "QueueNiNodeUpdate", {});
				});
				return {};
			}

			// everything else names a form
			auto* form = Lookup(arg(1), arg(2));
			if (!form) {
				return arg(2) + " from " + arg(1) + " is not in this game (is the mod installed?)";
			}

			if (kind == "perk") {
				auto* perk = As<BGSPerk>(form);
				if (!perk) {
					return "not a perk";
				}
				if (!le::HasPerk(player, perk)) {
					papyrus::CallMethod(player, "Actor", "AddPerk", { Arg::Form(perk) });
				}
				return {};
			}
			if (kind == "spell") {
				auto* spell = As<SpellItem>(form);
				if (!spell) {
					return "not a spell";
				}
				if (!le::HasSpell(player, spell)) {
					papyrus::CallMethod(player, "Actor", "AddSpell", { Arg::Form(spell), Arg::Bool(false) });
				}
				return {};
			}
			if (kind == "shout") {
				auto* shout = As<TESShout>(form);
				if (!shout) {
					return "not a shout";
				}
				papyrus::CallMethod(player, "Actor", "AddShout", { Arg::Form(shout) });
				return {};
			}
			if (kind == "word") {
				auto* word = As<TESWordOfPower>(form);
				if (!word) {
					return "not a word of power";
				}
				if (arg(5) == "1" && !(word->flags & 0x40)) {
					papyrus::CallStatic("Game", "TeachWord", { Arg::Form(word) });
				}
				if (arg(6) == "1" && !(word->flags & 0x01)) {
					papyrus::CallStatic("Game", "UnlockWord", { Arg::Form(word) });
				}
				return {};
			}
			if (kind == "faction") {
				auto* faction = As<TESFaction>(form);
				if (!faction) {
					return "not a faction";
				}
				const int rank = static_cast<int>(Num(arg(5)));
				if (le::FactionRank(player, faction) < rank) {
					papyrus::CallMethod(player, "Actor", "SetFactionRank", { Arg::Form(faction), Arg::Int(rank) });
				}
				return {};
			}
			if (kind == "marker") {
				if (form->formType != kFormType_Reference) {
					return "not a map marker";
				}
				const int flags = static_cast<int>(Num(arg(5)));
				if (flags) {
					papyrus::CallMethod(form, "ObjectReference", "AddToMap", { Arg::Bool((flags & 2) != 0) });
				}
				return {};
			}
			if (kind == "equip") {
				bool have = false;
				for (const auto& it : le::Inventory(player)) {
					have = have || (it.form == form && it.count > 0);
				}
				if (!have) {
					return "not in the inventory (bring the item over first)";
				}
				// SKSE's EquipItemEx: 0 the game picks the hand, 1 right, 2 left (a custom-named copy cannot be picked on LE)
				const SInt32 slot = form->formType == kFormType_Weapon ? (arg(5) == "left" ? 2 : 1) : 0;
				papyrus::CallMethod(player, "Actor", "EquipItemEx", { Arg::Form(form), Arg::Int(slot), Arg::Bool(false), Arg::Bool(false) });
				return {};
			}
			if (kind == "equipspell") {
				auto* spell = As<SpellItem>(form);
				if (!spell) {
					return "not a spell";
				}
				if (!le::HasSpell(player, spell)) {
					return "the spell is not known (bring it over first)";
				}
				const SInt32 slot = arg(5) == "voice" ? 2 : (arg(5) == "left" ? 0 : 1);  // Actor.EquipSpell: 0 left, 1 right, 2 voice
				papyrus::CallMethod(player, "Actor", "EquipSpell", { Arg::Form(spell), Arg::Int(slot) });
				return {};
			}
			if (kind == "equipshout") {
				auto* shout = As<TESShout>(form);
				if (!shout) {
					return "not a shout";
				}
				papyrus::CallMethod(player, "Actor", "EquipShout", { Arg::Form(shout) });
				return {};
			}
			if (kind == "item") {
				const int  count = (std::max)(1, static_cast<int>(Num(arg(5))));
				const auto name = arg(6), temper = arg(7), charge = arg(8), ench = arg(9), poison = arg(10);
				if (name.empty() && temper.empty() && charge.empty() && ench.empty() && poison.empty()) {
					papyrus::CallMethod(player, "ObjectReference", "AddItem", { Arg::Form(form), Arg::Int(count), Arg::Bool(true) });
					return {};
				}
				// one at a time: placed at the player's feet (PlaceAtMe), given its custom name, temper, charge
				// and enchantment with SKSE's ObjectReference functions, then put into the inventory (AddItem
				// of a reference keeps its extra data)
				EnchantmentItem* e = nullptr;
				float            maxCharge = 0.0f;
				std::string      left;
				if (!ench.empty()) {
					const auto p = Split(ench, ':');
					if (p.size() >= 3 && p[0] == "F") {
						e = As<EnchantmentItem>(Lookup(p[1], p[2]));
						maxCharge = p.size() >= 4 ? Num(p[3]) : 0.0f;
					}
					if (!e) {
						left += "its enchantment (made in the game, or its mod is missing); ";
					}
				}
				if (!poison.empty()) {
					left += "the poison on it; ";
				}
				const auto pid = player->formID;
				const auto eid = e ? e->formID : 0;
				for (int n = 0; n < count; ++n) {
					papyrus::CallMethod(player, "ObjectReference", "PlaceAtMe", { Arg::Form(form), Arg::Int(1), Arg::Bool(false), Arg::Bool(false) },
						[pid, eid, maxCharge, name, temper, charge](const papyrus::Result& a_r) {
							auto* placed = a_r.form;
							if (!placed) {
								spdlog::warn("item: PlaceAtMe gave no reference");
								return;
							}
							auto steps = std::make_shared<std::vector<Step>>();
							if (eid) {
								steps->push_back(Call(placed, "ObjectReference", "SetEnchantment", { Arg::Form(le::Lookup(eid)), Arg::Float(maxCharge) }));
							}
							if (!temper.empty()) {
								steps->push_back(Call(placed, "ObjectReference", "SetItemHealthPercent", { Arg::Float(Num(temper)) }));
							}
							if (!charge.empty()) {
								steps->push_back(Call(placed, "ObjectReference", "SetItemCharge", { Arg::Float(Num(charge)) }));
							}
							if (!name.empty()) {
								steps->push_back(Call(placed, "ObjectReference", "SetDisplayName", { Arg::String(name), Arg::Bool(false) }));
							}
							steps->push_back(Call(le::Lookup(pid), "ObjectReference", "AddItem", { Arg::Form(placed), Arg::Int(1), Arg::Bool(true) }));
							RunChain(steps);
						});
				}
				if (!left.empty()) {
					spdlog::info("item {} {}: brought over without {}", arg(1), arg(2), left);
				}
				return {};
			}
			return "unknown record " + kind;
		} catch (const std::exception& e) {
			return std::string("failed: ") + e.what();
		} catch (...) {
			return "failed (the game data could not be read)";
		}
	}
}
