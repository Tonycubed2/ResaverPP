#include "PCH.h"
#include "LiveInfo.h"
#include "Game.h"
#include "Protocol.h"
#include "Rescue.h"
#include "Settings.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <unordered_map>

namespace LiveInfo
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		std::string Hex(RE::FormID a_id) { return std::format("{:08X}", a_id); }

		std::string NameOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			const char* n = a_form->GetName();
			if (n && *n) {
				return n;
			}
			const char* e = a_form->GetFormEditorID();
			return e && *e ? std::string(e) : Hex(a_form->GetFormID());
		}

		RE::VMHandle HandleOf(RE::TESForm* a_form, RE::VMTypeID a_type)
		{
			auto* vm = VM::GetSingleton();
			auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			return policy && a_form ? policy->GetHandleForObject(a_type, a_form) : 0;
		}

		constexpr auto kActorType = static_cast<RE::VMTypeID>(RE::Character::FORMTYPE);

		// calls a_fn with the Papyrus function's return value (as Game.cpp's Callback)
		class Callback : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Callback(std::function<void(const RE::BSScript::Variable&)> a_fn) :
				_fn(std::move(a_fn))
			{}
			void operator()(RE::BSScript::Variable a_result) override
			{
				if (_fn) {
					_fn(a_result);
				}
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			std::function<void(const RE::BSScript::Variable&)> _fn;
		};

		// ---------------------------------------------------------------- effects
		// MagicSystem::SpellType values (CommonLib RE/M/MagicSystem.h)
		constexpr int kDisease = 1, kAbility = 4, kPoison = 5, kEnchantment = 6, kPotion = 7, kIngredient = 8, kAddiction = 10,
					  kStaffEnchantment = 12;

		struct Worn
		{
			std::set<RE::FormID> items;    // base objects with a worn stack (ExtraWorn 0x16 / ExtraWornLeft 0x17)
			std::set<RE::FormID> carried;  // base objects in the player's inventory changes
			std::set<RE::FormID> ench;     // enchantments a worn stack carries (its own ExtraEnchantment, or the base EITM)
		};

		Worn ReadWorn(RE::PlayerCharacter* a_player)
		{
			Worn w;
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList) {
				return w;
			}
			for (auto* entry : *changes->entryList) {
				if (!entry || !entry->object) {
					continue;
				}
				const auto id = entry->object->GetFormID();
				w.carried.insert(id);
				if (!entry->extraLists) {
					continue;
				}
				for (auto* xl : *entry->extraLists) {
					if (!xl || !(xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>())) {
						continue;
					}
					w.items.insert(id);
					if (auto* xe = xl->GetByType<RE::ExtraEnchantment>(); xe && xe->enchantment) {
						w.ench.insert(xe->enchantment->GetFormID());
					}
					if (auto* ef = entry->object->As<RE::TESEnchantableForm>(); ef && ef->formEnchanting) {
						w.ench.insert(ef->formEnchanting->GetFormID());
					}
				}
			}
			return w;
		}

		struct Key
		{
			RE::FormID spell = 0, item = 0;
			bool       other = false;
			auto       operator<=>(const Key&) const = default;
		};

		struct Raw
		{
			RE::ActiveEffect* ae = nullptr;
			Key               key;
			int               type = -1;
			bool              ench = false, other = false, byPlayer = false;
		};

		// every active effect of the player, with the source it is grouped under (as ReSaver Plus's Character effects:
		// one card per magic item, and per source item for enchantments and for effects put on by someone else)
		std::vector<Raw> ReadRaw(RE::PlayerCharacter* a_player)
		{
			std::vector<Raw> out;
			auto*            list = static_cast<RE::MagicTarget*>(a_player)->GetActiveEffectList();
			if (!list) {
				return out;
			}
			for (auto* ae : *list) {
				if (!ae) {
					continue;
				}
				Raw r;
				r.ae = ae;
				r.type = ae->spell ? static_cast<int>(ae->spell->GetSpellType()) : -1;
				const auto caster = ae->caster.get();
				r.other = caster && caster.get() != a_player;
				r.byPlayer = caster && caster.get() == a_player;
				r.ench = ae->flags.any(RE::ActiveEffect::Flag::kEnchanting) || r.type == kEnchantment || r.type == kStaffEnchantment;
				r.key.spell = ae->spell ? ae->spell->GetFormID() : 0;
				r.key.item = (r.ench || r.other) && ae->source ? ae->source->GetFormID() : 0;
				r.key.other = r.other;
				out.push_back(r);
			}
			return out;
		}

		Group GroupOf(const Raw& r, const Worn& w)
		{
			if (r.other) {
				return Group::kOthers;
			}
			if (r.ench) {
				return (r.key.item && w.items.contains(r.key.item)) || w.ench.contains(r.key.spell) ? Group::kWorn : Group::kNotWorn;
			}
			if (!r.ae->spell) {
				return Group::kUnknown;
			}
			switch (r.type) {
			case kAbility: return Group::kAbilities;
			case kDisease:
			case kAddiction: return Group::kDiseases;
			case kPoison:
			case kPotion:
			case kIngredient: return Group::kConsumables;
			default: return Group::kSpells;
			}
		}

		// an item's constant enchantment still on the player while nothing worn carries it (ReSaver Plus's stuck rule):
		// put on by the player, never running out (duration 0), not ended
		bool StuckEffect(const Raw& r, Group a_group)
		{
			if (a_group != Group::kNotWorn || !r.ae->spell || r.ae->spell->GetCastingType() != RE::MagicSystem::CastingType::kConstantEffect) {
				return false;  // a weapon's enchantment works on hits: not stuck
			}
			return r.byPlayer && r.ae->duration == 0.0f && !r.ae->flags.any(RE::ActiveEffect::Flag::kDispelled);
		}

		RE::ActiveEffect* FindByUid(RE::PlayerCharacter* a_player, std::uint16_t a_uid)
		{
			auto* list = static_cast<RE::MagicTarget*>(a_player)->GetActiveEffectList();
			if (!list) {
				return nullptr;
			}
			for (auto* ae : *list) {
				if (ae && ae->usUniqueID == a_uid) {
					return ae;
				}
			}
			return nullptr;
		}

		std::string EffectName(const RE::ActiveEffect* a_ae)
		{
			const auto* base = a_ae->GetBaseObject();
			auto        n = NameOf(base);
			return n.empty() ? std::string("(no magic effect)") : n;
		}

		// ---------------------------------------------------------------- VM
		// "Script.Function" of a stack's current frame. Read only from an SKSE task: SKSE runs its tasks from the game's main
		// loop (SkyrimSE.exe 1.6.1170 0x645ea0, call at +0xB9C), after the VM has run its code tasklets on that same thread
		// (0x6463e2 -> 0x9c0ac0 -> VirtualMachine::UpdateTasklets 0x1444880, which runs every tasklet in turn), so no stack
		// is executing while this reads it. Guarded anyway: a bad pointer counts as "unreadable" instead of crashing.
		bool TopName(const RE::BSScript::Stack* a_stack, char* a_out, std::size_t a_size)
		{
			__try {
				const auto* frame = a_stack->top;
				if (!frame) {
					return false;
				}
				const auto* fn = frame->owningFunction.get();
				if (!fn) {
					return false;
				}
				const char* script = fn->GetObjectTypeName().c_str();
				const char* func = fn->GetName().c_str();
				_snprintf_s(a_out, a_size, _TRUNCATE, "%s.%s", script ? script : "?", func ? func : "?");
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	const char* GroupName(Group a_group)
	{
		switch (a_group) {
		case Group::kWorn: return "Worn items";
		case Group::kNotWorn: return "Items not worn";
		case Group::kSpells: return "Spells, powers and shouts";
		case Group::kConsumables: return "Potions, food and ingredients";
		case Group::kAbilities: return "Abilities (race, standing stone, perks, mods)";
		case Group::kDiseases: return "Diseases and addictions";
		case Group::kOthers: return "Put on you by others";
		default: return "Unknown (plugin missing)";
		}
	}

	Effects ReadEffects()
	{
		Effects v;
		auto*   player = RE::PlayerCharacter::GetSingleton();
		if (!player || !Game::InGame()) {
			return v;
		}
		v.valid = true;
		const auto              worn = ReadWorn(player);
		const auto              raw = ReadRaw(player);
		std::map<Key, std::size_t> at;
		for (const auto& r : raw) {
			auto it = at.find(r.key);
			if (it == at.end()) {
				Source s;
				s.group = GroupOf(r, worn);
				s.spell = r.key.spell;
				s.item = r.key.item;
				s.fromOther = r.other;
				s.spellType = r.type;
				s.spellName = r.ae->spell ? NameOf(r.ae->spell) : std::string("(no magic item)");
				s.itemName = r.key.item ? NameOf(r.ae->source) : std::string();
				s.itemWorn = r.key.item && worn.items.contains(r.key.item);
				s.itemCarried = r.key.item && worn.carried.contains(r.key.item);
				it = at.emplace(r.key, v.sources.size()).first;
				v.sources.push_back(std::move(s));
			}
			auto&  s = v.sources[it->second];
			Effect e;
			e.uid = r.ae->usUniqueID;
			e.name = EffectName(r.ae);
			e.magnitude = r.ae->magnitude;
			e.elapsed = r.ae->elapsedSeconds;
			e.duration = r.ae->duration;
			e.flags = r.ae->flags.underlying();
			e.stuck = StuckEffect(r, s.group);
			s.running += !e.ended() && !e.off();
			s.stuckCount += e.stuck;
			++v.total;
			(e.ended() ? v.ended : e.off() ? v.off : v.running)++;
			v.stuck += e.stuck;
			s.effects.push_back(std::move(e));
		}
		for (auto& s : v.sources) {
			if (!s.stuckCount) {
				continue;
			}
			if (!s.item) {
				s.stuckWhy = "no item is named as its source, and nothing you wear carries this enchantment";
			} else if (s.itemCarried) {
				s.stuckWhy = "you carry " + s.itemName + " but do not wear it, and nothing you wear carries this enchantment";
			} else {
				s.stuckWhy = (s.itemName.empty() ? Hex(s.item) : s.itemName) + " is not in your inventory any more, and nothing you wear carries this enchantment";
			}
		}
		std::stable_sort(v.sources.begin(), v.sources.end(), [](const Source& a, const Source& b) {
			if (a.group != b.group) {
				return a.group < b.group;
			}
			return a.effects.size() > b.effects.size();
		});
		return v;
	}

	std::string RemoveEffect(std::uint16_t a_uid)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* ae = player ? FindByUid(player, a_uid) : nullptr;
		if (!ae) {
			return "that effect is not on you any more";
		}
		const auto name = EffectName(ae);
		spdlog::info("F1 page: Dispel(false) on the player's effect {} ({}, uid {})", name, ae->spell ? Hex(ae->spell->GetFormID()) : "-", a_uid);
		ae->Dispel(false);  // what ActiveMagicEffect.Dispel() does (its native 0x9e5ca0: xor edx,edx; jmp 0x5ac880)
		return "removed: " + name;
	}

	std::string RemoveSource(RE::FormID a_spell, RE::FormID a_item, bool a_fromOther, bool a_onlyStuck)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return "no player";
		}
		const auto               worn = ReadWorn(player);
		const Key                key{ a_spell, a_item, a_fromOther };
		std::vector<std::uint16_t> uids;
		Group                    group = Group::kUnknown;
		RE::MagicItem*           spell = nullptr;
		RE::TESBoundObject*      item = nullptr;
		for (const auto& r : ReadRaw(player)) {
			if (r.key != key) {
				continue;
			}
			group = GroupOf(r, worn);
			spell = r.ae->spell;
			item = r.ae->source;
			if (!a_onlyStuck || StuckEffect(r, group)) {
				uids.push_back(r.ae->usUniqueID);
			}
		}
		if (uids.empty()) {
			return a_onlyStuck ? "nothing stuck from that source any more" : "nothing from that source is on you any more";
		}
		if (!a_onlyStuck && group == Group::kWorn && item && worn.items.contains(item->GetFormID())) {
			auto* eq = RE::ActorEquipManager::GetSingleton();
			if (!eq) {
				return "the game's equip manager is not there";
			}
			spdlog::info("F1 page: UnequipObject {} ({}) from the player", NameOf(item), Hex(item->GetFormID()));
			eq->UnequipObject(player, item);  // the same arguments as Actor.UnequipItem's native (0x9eb409 -> 0x6ca010)
			return "took off " + NameOf(item);
		}
		if (!a_onlyStuck && (group == Group::kAbilities || group == Group::kDiseases)) {
			if (auto* sp = spell ? spell->As<RE::SpellItem>() : nullptr) {
				spdlog::info("F1 page: Actor::RemoveSpell {} ({}) on the player", NameOf(sp), Hex(sp->GetFormID()));
				const bool ok = player->RemoveSpell(sp);  // what Actor.RemoveSpell queues (task 0x5d -> 0x6c14b0)
				return ok ? "removed " + NameOf(sp) : "the game did not remove " + NameOf(sp) + " (it is not in your spell list: a mod or perk adds it)";
			}
		}
		std::size_t n = 0;
		for (const auto uid : uids) {  // found again each time: dispelling one can change the list
			if (auto* ae = FindByUid(player, uid)) {
				ae->Dispel(false);
				++n;
			}
		}
		spdlog::info("F1 page: dispelled {} effect(s) of {} / {}{}", n, Hex(a_spell), Hex(a_item), a_onlyStuck ? " (stuck ones)" : "");
		return std::format("removed {} effect(s)", n);
	}

	// ---------------------------------------------------------------- followers
	Followers ReadFollowers()
	{
		Followers f;
		auto*     player = RE::PlayerCharacter::GetSingleton();
		if (!player || !Game::InGame()) {
			return f;
		}
		f.valid = true;
		for (const auto& rec : Rescue::ListFollowers()) {  // id*Plugin.esp:localid*name*why*where*skip
			std::vector<std::string> p(1);
			for (char c : rec) {
				if (c == '*') {
					p.emplace_back();
				} else {
					p.back() += c;
				}
			}
			if (p.size() < 6) {
				continue;
			}
			Follower fl;
			try {
				fl.id = static_cast<RE::FormID>(std::stoul(p[0], nullptr, 16));
			} catch (...) {
				continue;
			}
			fl.key = p[1];
			fl.name = p[2];
			fl.why = p[3];
			fl.where = p[4];
			fl.skip = p[5] == "1";
			fl.vanilla = fl.why == "vanilla follower quest";
			if (auto* a = RE::TESForm::LookupByID<RE::Actor>(fl.id)) {
				fl.teammate = a->IsPlayerTeammate();
			}
			f.list.push_back(std::move(fl));
		}
		if (auto* race = player->GetRace()) {
			const char* e = race->GetFormEditorID();
			f.race = e && *e ? e : Hex(race->GetFormID());
			f.werewolf = race->GetFormID() == 0x000CDD84;  // WerewolfBeastRace, Skyrim.esm
			if (auto* dh = RE::TESDataHandler::GetSingleton()) {
				f.vampireLord = race == dh->LookupForm<RE::TESRace>(0x00283A, "Dawnguard.esm");  // DLC1VampireBeastRace
			}
		}
		return f;
	}

	// ---------------------------------------------------------------- quests
	std::vector<QuestRow> ReadRunningQuests()
	{
		std::vector<QuestRow> out;
		auto*                 dh = RE::TESDataHandler::GetSingleton();
		if (!dh || !Game::InGame()) {
			return out;
		}
		for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
			if (!q || !Game::Running(q)) {
				continue;
			}
			QuestRow r;
			r.id = q->GetFormID();
			r.editorID = q->GetFormEditorID() ? q->GetFormEditorID() : "";
			r.name = q->GetName() ? q->GetName() : "";
			r.stage = q->GetCurrentStageID();
			out.push_back(std::move(r));
		}
		std::ranges::sort(out, [](const QuestRow& a, const QuestRow& b) {
			if (a.name.empty() != b.name.empty()) {
				return !a.name.empty();  // named quests (the journal's) first
			}
			return a.name != b.name ? a.name < b.name : a.editorID < b.editorID;
		});
		return out;
	}

	QuestDetail ReadQuest(RE::FormID a_quest)
	{
		QuestDetail d;
		auto*       q = RE::TESForm::LookupByID<RE::TESQuest>(a_quest);
		if (!q) {
			return d;
		}
		d.found = true;
		d.id = a_quest;
		d.editorID = q->GetFormEditorID() ? q->GetFormEditorID() : "";
		d.name = q->GetName() ? q->GetName() : "";
		d.stage = q->GetCurrentStageID();
		d.running = Game::Running(q);
		d.completed = q->IsCompleted();
		// The quest's stages: ONE list whose head node is inline at TESQuest+0xE8 (item at +0xE8, next node at +0xF0;
		// CommonLib names the two words executedStages / waitingStages). SetStage (SkyrimSE.exe 1.6.1170 0x3c9d00) walks
		// it this way and compares each item's first u16 with the stage it is asked for.
		struct StageNode
		{
			RE::TESQuestStage* item;
			StageNode*         next;
		};
		std::set<std::uint16_t> seen;
		for (auto* node = reinterpret_cast<const StageNode*>(&q->executedStages); node; node = node->next) {
			if (node->item && seen.insert(node->item->data.index).second) {
				d.stages.push_back({ node->item->data.index, static_cast<std::uint8_t>(node->item->data.flags.underlying()) });
			}
			if (d.stages.size() > 4096) {
				break;  // never loop forever on a broken list
			}
		}
		std::ranges::sort(d.stages, {}, &QuestDetail::Stage::index);
		for (auto* o : q->objectives) {
			if (o) {
				d.objectives.push_back({ o->index, static_cast<std::uint8_t>(o->state.underlying()), o->displayText.c_str() ? o->displayText.c_str() : "" });
			}
		}
		std::ranges::sort(d.objectives, {}, &QuestDetail::Objective::index);
		return d;
	}

	// ---------------------------------------------------------------- VM
	VMSample SampleVM(double a_time)
	{
		VMSample s;
		s.time = a_time;
		auto* vm = VM::GetSingleton();
		if (!vm || !Game::InGame()) {
			return s;
		}
		s.valid = true;
		std::unordered_map<std::string, std::uint32_t> byScript;
		{
			RE::BSSpinLockGuard guard(vm->runningStacksLock);  // VM+0x9318, guards allRunningStacks / waitingLatentReturns
			s.stacks = vm->allRunningStacks.size();
			s.latent = vm->waitingLatentReturns.size();
			char buf[256];
			for (auto& [id, stack] : vm->allRunningStacks) {
				if (stack && TopName(stack.get(), buf, sizeof(buf))) {
					++byScript[buf];
				} else {
					++s.unreadable;
				}
			}
		}
		{
			RE::BSSpinLockGuard guard(vm->suspendQueueLock);  // VM+0x92F0
			s.suspended = vm->suspendQueue1.numEntries + vm->suspendQueue2.numEntries + vm->overflowSuspendArray1.size() +
			              vm->overflowSuspendArray2.size();
		}
		{
			RE::BSSpinLockGuard guard(vm->funcQueueLock);  // VM+0x200
			s.queuedCalls = vm->uiWaitingFunctionMessages;
		}
		{
			RE::BSSpinLockGuard guard(vm->frozenStacksLock);  // VM+0x9384: IsCompletelyFrozen (0x1444a20) takes it to read 0x939C
			s.frozen = vm->frozenStacksCount;
			s.freezeState = vm->freezeState.underlying();
		}
		if (auto* svm = RE::SkyrimVM::GetSingleton()) {
			// the freeze request: 0x9c3590 writes it under frozenLock (+0x678); the save (0x9c9dc9), loading and the
			// console's ToggleScripts (0x36abac, "Script processing %s") call it
			auto&               data = svm->GetVMRuntimeData();
			RE::BSSpinLockGuard guard(data.frozenLock);
			s.freezeAsked = data.isFrozen;
		}
		s.topScripts.assign(byScript.begin(), byScript.end());
		std::ranges::sort(s.topScripts, [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
		if (s.topScripts.size() > 12) {
			s.topScripts.resize(12);
		}
		return s;
	}

	// ---------------------------------------------------------------- tests
	std::string Spawn(int a_which, std::function<void(RE::FormID)> a_spawned)
	{
		static constexpr RE::FormID kBase[3] = { 0x00023ABE, 0x000CF89D, 0x00023AB5 };  // EncWolf, EncDeer, EncSabreCat (Skyrim.esm)
		static constexpr const char* kName[3] = { "wolf", "deer", "sabre cat" };
		if (a_which < 0 || a_which > 2) {
			return "no such creature";
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* dh = RE::TESDataHandler::GetSingleton();
		auto* npc = dh ? dh->LookupForm<RE::TESNPC>(kBase[a_which], "Skyrim.esm") : nullptr;
		auto* vm = VM::GetSingleton();
		const auto h = HandleOf(player, kActorType);
		if (!npc) {
			return std::format("the {} ({:08X} in Skyrim.esm) is not in this game", kName[a_which], kBase[a_which]);
		}
		if (!player || !player->GetParentCell() || !vm || !h) {
			return "no player in a cell";
		}
		const float heading = player->GetAngleZ();
		const float x = std::sin(heading) * 300.0f, y = std::cos(heading) * 300.0f;
		auto        cb = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{ new Callback([x, y, a_spawned, which = a_which](const RE::BSScript::Variable& a_result) {
            auto* ref = a_result.IsObject() ? a_result.Unpack<RE::TESObjectREFR*>() : nullptr;
            auto* vm2 = VM::GetSingleton();
            auto* pl = RE::PlayerCharacter::GetSingleton();
            if (!ref || !vm2 || !pl) {
                spdlog::info("F1 page: PlaceAtMe gave no reference for the {}", kName[which]);
                a_spawned(0);
                return;
            }
            const auto rh = HandleOf(ref, kActorType);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
            float      fx = x, fy = y;
            if (rh) {
                vm2->DispatchMethodCall(rh, "ObjectReference", "MoveTo",
                    RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(pl), std::move(fx), std::move(fy), 0.0f, false), none);
            }
            spdlog::info("F1 page: spawned a {} {:08X}, moved 300 units in front of the player", kName[which], ref->GetFormID());
            a_spawned(ref->GetFormID());
        }) };
		if (!vm->DispatchMethodCall(h, "ObjectReference", "PlaceAtMe", RE::MakeFunctionArguments(static_cast<RE::TESNPC*>(npc), static_cast<std::int32_t>(1), false, false), cb)) {
			return "the game did not take the call to PlaceAtMe";
		}
		spdlog::info("F1 page: PlaceAtMe {:08X} ({}) dispatched", npc->GetFormID(), kName[a_which]);
		return {};
	}

	std::string Kill(RE::FormID a_ref)
	{
		auto* a = RE::TESForm::LookupByID<RE::Actor>(a_ref);
		if (!a || a == RE::PlayerCharacter::GetSingleton()) {
			return "no character to kill";
		}
		if (a->IsDead()) {
			return NameOf(a) + " is already dead";
		}
		auto*      vm = VM::GetSingleton();
		const auto h = HandleOf(a, kActorType);
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		if (!vm || !h || !vm->DispatchMethodCall(h, "Actor", "Kill", RE::MakeFunctionArguments(static_cast<RE::Actor*>(nullptr)), none)) {
			return "no script object for this character";
		}
		spdlog::info("F1 page: Kill() on {:08X} ({}) dispatched", a_ref, NameOf(a));
		return {};
	}

	RE::FormID TargetActor(std::string& a_name)
	{
		RE::NiPointer<RE::TESObjectREFR> ref;
		if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
			ref = pick->GetActiveTarget().get();
		}
		if (!ref || !ref->As<RE::Actor>()) {
			ref = RE::Console::GetSelectedRef();
		}
		auto* a = ref ? ref->As<RE::Actor>() : nullptr;
		if (!a || a == RE::PlayerCharacter::GetSingleton()) {
			a_name.clear();
			return 0;
		}
		a_name = NameOf(a);
		return a->GetFormID();
	}

	// ---------------------------------------------------------------- report
	std::filesystem::path LogFolder()
	{
		auto dir = SKSE::log::log_directory();
		return dir ? *dir : std::filesystem::path();
	}

	std::filesystem::path PapyrusLog()
	{
		const auto dir = LogFolder();
		return dir.empty() ? dir : dir.parent_path() / "Logs" / "Script" / "Papyrus.0.log";
	}

	std::filesystem::path WriteReport(const VMSample& a_vm, const std::string& a_extra, std::string& a_error)
	{
		const auto dir = LogFolder();
		if (dir.empty()) {
			a_error = "the SKSE log folder is not known";
			return {};
		}
		const auto path = dir / "ReSaverHelper-report.txt";
		std::string r;
		auto        line = [&r](const std::string& a_text) { r += a_text + "\n"; };
		const auto  now = std::time(nullptr);
		std::tm     tm{};
		localtime_s(&tm, &now);
		char when[64];
		std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
		line("ReSaverHelper report - written from the in-game ReSaver Plus page (F1)");
		line(std::string("Written: ") + when);
		line(std::format("Helper version: {}", proto::kHelperVersion));
		line(std::format("Game: {} {}", REL::Module::IsVR() ? "VR" : REL::Module::IsAE() ? "AE" : "SE", REL::Module::get().version().string()));
		if (!a_extra.empty()) {
			r += a_extra;
		}
		const auto& s = Settings::Get();
		line(std::format("Settings: bListen={} bRescueHotkey={} iRescueHoldSeconds={} sNotify={} bRefreshJournalOnLoad={} iDiagnosticStage={} (at start: {})",
			s.listen ? 1 : 0, s.rescueHotkey ? 1 : 0, s.rescueHoldSeconds, s.notify, s.refreshOnLoad ? 1 : 0, Settings::DiagnosticStageInIni(),
			Settings::StartupDiagnosticStage()));
		if (auto* dh = RE::TESDataHandler::GetSingleton()) {
			line(std::format("Plugins loaded: {} full, {} light", dh->GetLoadedModCount(), dh->GetLoadedLightModCount()));
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player && Game::InGame()) {
			auto* cell = player->GetParentCell();
			auto* ws = player->GetWorldspace();
			const auto pos = player->GetPosition();
			line(std::format("Player: {} at {:.0f}, {:.0f}, {:.0f}", NameOf(player), pos.x, pos.y, pos.z));
			line(std::format("Cell: {} [{}] {} {}", cell ? NameOf(cell) : "(none)", cell ? Hex(cell->GetFormID()) : "-",
				cell && cell->IsInteriorCell() ? "interior" : "exterior", ws ? "in " + NameOf(ws) + " [" + Hex(ws->GetFormID()) + "]" : ""));
			line("");
			line("Characters loaded around you with a problem:");
			std::size_t problems = 0, seen = 0;
			if (auto* pl = RE::ProcessLists::GetSingleton()) {
				pl->ForAllActors([&](RE::Actor* a) {
					++seen;
					if (!a || a == player) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					std::string why;
					if (!a->GetActorBase()) {
						why += " no base NPC;";
					}
					if (a->IsDeleted()) {
						why += " deleted;";
					}
					if (a->IsDisabled()) {
						why += " disabled;";
					}
					if (a->IsDead() && a->IsPlayerTeammate()) {
						why += " dead but still your teammate;";
					}
					if (!why.empty()) {
						++problems;
						auto* c = a->GetParentCell();
						line(std::format("  {} [{}] in {}:{}", NameOf(a), Hex(a->GetFormID()), c ? NameOf(c) : "(no cell)", why));
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			line(problems ? std::format("  ({} of {} loaded characters)", problems, seen) : std::format("  none ({} loaded characters)", seen));
			line("");
			line("Followers:");
			const auto f = ReadFollowers();
			for (const auto& fl : f.list) {
				line(std::format("  {} [{}] {} - {} - {}{}{}", fl.name, Hex(fl.id), fl.key, fl.why, fl.where, fl.teammate ? " - teammate" : "",
					fl.skip ? " - in sGatherSkip" : ""));
			}
			if (f.list.empty()) {
				line("  none");
			}
			line(std::format("Player race: {}{}{}", f.race, f.werewolf ? " (werewolf form)" : "", f.vampireLord ? " (Vampire Lord form)" : ""));
		} else {
			line("No save loaded.");
		}
		line("");
		if (a_vm.valid) {
			line("Script health (Papyrus VM, last sample):");
			line(std::format("  script threads: {}  waiting on a timer/latent call: {}  waiting their turn (suspended): {}  calls not started: {}",
				a_vm.stacks, a_vm.latent, a_vm.suspended, a_vm.queuedCalls));
			line(std::format("  freeze state: {} (0 = normal)  frozen stacks: {}  unreadable stacks: {}", a_vm.freezeState, a_vm.frozen, a_vm.unreadable));
			line("  most threads in:");
			for (const auto& [name, n] : a_vm.topScripts) {
				line(std::format("    {:6}  {}", n, name));
			}
		} else {
			line("Script health: not sampled (open Troubleshooting > Script health first)");
		}
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << r;
		if (!out) {
			a_error = "could not write " + path.string();
			return {};
		}
		spdlog::info("F1 page: report written to {}", path.string());
		return path;
	}
}
