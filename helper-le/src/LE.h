#pragma once
// Game helpers for Skyrim LE 1.9.32.0. SKSE 1.7.3's headers (himika's libSKSE copy in extern\skse)
// give the classes; the few functions and offsets they do not have come from himika's libSkyrim
// (github.com/kassent/SkyrimSouls, Skyrim/include), with the address it documents next to each one.
// Everything here runs on the game's main thread unless it says otherwise.

namespace le
{
	// ---- functions in TESV.exe 1.9.32.0 ----------------------------------------------------------
	// Actor::HasPerk (libSkyrim Forms/Actor.h)
	inline bool HasPerk(Actor* a_actor, BGSPerk* a_perk)
	{
		using F = bool(__thiscall*)(Actor*, BGSPerk*);
		return a_actor && a_perk && reinterpret_cast<F>(0x006AA190)(a_actor, a_perk);
	}
	// Actor::HasSpell - spells the actor or its base knows (libSkyrim Forms/Actor.h HasSpell_Impl)
	inline bool HasSpell(Actor* a_actor, SpellItem* a_spell)
	{
		using F = bool(__thiscall*)(Actor*, SpellItem*);
		return a_actor && a_spell && reinterpret_cast<F>(0x006E9130)(a_actor, a_spell);
	}
	// Actor::GetLevel (libSkyrim Forms/Actor.h)
	inline UInt16 GetLevel(Actor* a_actor)
	{
		using F = UInt16(__thiscall*)(Actor*);
		return a_actor ? reinterpret_cast<F>(0x006A7320)(a_actor) : 0;
	}
	// MenuManager: number of open menus that pause the game (libSkyrim Menus/MenuManager.h, +0xC8)
	inline UInt32 PauseCount()
	{
		auto* mm = MenuManager::GetSingleton();
		return mm ? *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(mm) + 0xC8) : 0;
	}

	// ActorValueOwner (vtable 03 GetBase, 04 SetBase), called through the game's vtable: a call on the
	// actorValueOwner member itself would be bound at compile time to SKSE's declaration, which has no body
	inline float GetBaseAV(Actor* a_actor, UInt32 a_av)
	{
		auto* owner = &a_actor->actorValueOwner;
		using F = float(__thiscall*)(ActorValueOwner*, UInt32);
		return reinterpret_cast<F>((*reinterpret_cast<void***>(owner))[3])(owner, a_av);
	}
	inline void SetBaseAV(Actor* a_actor, UInt32 a_av, float a_value)
	{
		auto* owner = &a_actor->actorValueOwner;
		using F = void(__thiscall*)(ActorValueOwner*, UInt32, float);
		reinterpret_cast<F>((*reinterpret_cast<void***>(owner))[4])(owner, a_av, a_value);
	}

	// ---- forms ------------------------------------------------------------------------------------
	// the game's dynamic_cast (SKSE's DYNAMIC_CAST macro, written as a function)
	template <class To>
	To* Cast(const void* a_obj, const void* a_fromRTTI, const void* a_toRTTI)
	{
		return a_obj ? static_cast<To*>(Runtime_DynamicCast(const_cast<void*>(a_obj), 0, a_fromRTTI, a_toRTTI, 0)) : nullptr;
	}

	inline PlayerCharacter* Player() { return g_thePlayer ? *g_thePlayer : nullptr; }
	inline DataHandler*     Data() { return DataHandler::GetSingleton(); }

	inline bool IsDynamic(const TESForm* a_form) { return a_form && (a_form->formID >> 24) == 0xFF; }

	inline TESForm* Lookup(std::uint32_t a_formID) { return a_formID ? LookupFormByID(a_formID) : nullptr; }

	template <class T>
	T* LookupAs(std::uint32_t a_formID)
	{
		auto* f = Lookup(a_formID);
		return f && f->formType == T::kTypeID ? static_cast<T*>(f) : nullptr;
	}

	// a plugin's form by its local id (LE has no light plugins: the load order index is the top byte)
	TESForm* Lookup(const std::string& a_plugin, std::uint32_t a_local);
	TESForm* Lookup(const std::string& a_plugin, const std::string& a_localHex);

	std::string   PluginOf(const TESForm* a_form);  // "" for forms made in the game
	std::uint32_t LocalOf(const TESForm* a_form);   // the id without the load order byte
	std::string   NameOf(TESForm* a_form);          // its name, or a quest's editor id

	// ---- quests -----------------------------------------------------------------------------------
	inline bool Running(const TESQuest* a_quest) { return a_quest && a_quest->IsRunning(); }
	inline bool Completed(const TESQuest* a_quest) { return a_quest && (a_quest->unk07C.flags & TESQuest::Data07C::kQuestFlag_Completed) != 0; }
	inline std::string EditorID(const TESQuest* a_quest)
	{
		const char* s = a_quest ? const_cast<BSString&>(a_quest->questID).Get() : nullptr;  // Get() is not const in SKSE
		return s ? s : "";
	}
	// objectives: index, state (the same values as on SE: 1 shown, 3 completed + shown, 5 failed + shown)
	std::vector<std::pair<std::uint16_t, std::uint8_t>> Objectives(TESQuest* a_quest);

	// ---- actors -----------------------------------------------------------------------------------
	inline bool IsTeammate(const Actor* a_actor) { return a_actor && (a_actor->flags1 & Actor::kFlags_IsPlayerTeammate) != 0; }
	TESNPC*     BaseNPC(Actor* a_actor);
	int         FactionRank(Actor* a_actor, TESFaction* a_faction);  // -1 when not in it (-2 when the rank is negative)
	// every faction the actor is in, with its rank (base NPC's, then the changes the game made)
	std::vector<std::pair<TESFaction*, SInt8>> Factions(Actor* a_actor);

	struct Item
	{
		TESForm*                    form = nullptr;
		SInt32                      count = 0;
		std::vector<BaseExtraList*> extras;  // the stacks with their own data (worn, tempered, named...)
	};
	// the inventory: base container plus the changes the game made
	std::vector<Item> Inventory(TESObjectREFR* a_ref);

	// menus: the Journal is open or a menu that pauses the game
	bool MenuOpen();
}
