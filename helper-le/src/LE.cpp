#include "PCH.h"
#include "LE.h"

// SKSE's TESForm::CopyFromEx (GameForms.cpp, never called by the helper) calls CopyFromBase on member
// components directly, which the compiler can bind to this declaration; the game's own version is only
// reached through its vtables. An empty body lets the DLL link; nothing in the game calls this one.
void BaseFormComponent::CopyFromBase(BaseFormComponent*) {}

namespace le
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		// walks an SKSE tList (items are pointers; the head node is part of the list)
		template <class T, class F>
		void ForEach(const tList<T>& a_list, F a_fn)
		{
			for (auto it = a_list.Begin(); !it.End(); ++it) {
				if (T* item = it.Get()) {
					a_fn(item);
				}
			}
		}
	}

	TESForm* Lookup(const std::string& a_plugin, std::uint32_t a_local)
	{
		auto* dh = Data();
		if (!dh || a_plugin.empty()) {
			return nullptr;
		}
		const auto want = Lower(a_plugin);
		for (UInt32 i = 0; i < dh->modList.loadedModCount && i < 0xFF; ++i) {
			const auto* mod = dh->modList.loadedMods[i];
			if (mod && Lower(mod->name) == want) {
				return LookupFormByID((i << 24) | (a_local & 0xFFFFFF));
			}
		}
		return nullptr;
	}

	TESForm* Lookup(const std::string& a_plugin, const std::string& a_localHex)
	{
		std::uint32_t local = 0;
		try {
			local = static_cast<std::uint32_t>(std::stoul(a_localHex, nullptr, 16));
		} catch (...) {
			return nullptr;
		}
		return Lookup(a_plugin, local);
	}

	std::string PluginOf(const TESForm* a_form)
	{
		auto* dh = Data();
		if (!a_form || !dh || IsDynamic(a_form)) {
			return {};
		}
		const auto index = a_form->formID >> 24;
		if (index >= dh->modList.loadedModCount || index >= 0xFF) {
			return {};
		}
		const auto* mod = dh->modList.loadedMods[index];
		return mod ? std::string(mod->name) : std::string();
	}

	std::uint32_t LocalOf(const TESForm* a_form) { return a_form ? (a_form->formID & 0xFFFFFF) : 0; }

	std::string NameOf(TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		if (auto* full = Cast<TESFullName>(a_form, RTTI_TESForm, RTTI_TESFullName)) {
			if (const char* n = full->name.data; n && *n) {
				return n;
			}
		}
		if (a_form->formType == kFormType_Quest) {
			return EditorID(static_cast<TESQuest*>(a_form));
		}
		return {};
	}

	std::vector<std::pair<std::uint16_t, std::uint8_t>> Objectives(TESQuest* a_quest)
	{
		std::vector<std::pair<std::uint16_t, std::uint8_t>> out;
		if (!a_quest) {
			return out;
		}
		// TESQuest::Objective +0x14 is the objective state (SE's BGSQuestObjective::state), +0x10 its index
		ForEach(a_quest->objectives, [&](TESQuest::Objective* o) {
			out.push_back({ o->index, static_cast<std::uint8_t>(o->unk14 & 0xFF) });
		});
		return out;
	}

	TESNPC* BaseNPC(Actor* a_actor)
	{
		auto* base = a_actor ? a_actor->baseForm : nullptr;
		return base && base->formType == kFormType_NPC ? static_cast<TESNPC*>(base) : nullptr;
	}

	std::vector<std::pair<TESFaction*, SInt8>> Factions(Actor* a_actor)
	{
		std::map<TESFaction*, SInt8> ranks;
		if (auto* npc = BaseNPC(a_actor)) {
			for (UInt32 i = 0; i < npc->actorData.factions.count; ++i) {
				const auto& fi = npc->actorData.factions.arr.entries[i];
				if (fi.faction) {
					ranks[fi.faction] = fi.rank;
				}
			}
		}
		if (a_actor) {
			if (auto* xf = static_cast<ExtraFactionChanges*>(a_actor->extraData.GetByType(kExtraData_FactionChanges))) {
				for (UInt32 i = 0; i < xf->factions.count; ++i) {
					const auto& fi = xf->factions.arr.entries[i];
					if (fi.faction) {
						ranks[fi.faction] = fi.rank;  // the game's change wins (a rank below 0 = removed)
					}
				}
			}
		}
		std::vector<std::pair<TESFaction*, SInt8>> out;
		for (const auto& [f, r] : ranks) {
			if (r >= 0) {
				out.push_back({ f, r });
			}
		}
		return out;
	}

	int FactionRank(Actor* a_actor, TESFaction* a_faction)
	{
		for (const auto& [f, r] : Factions(a_actor)) {
			if (f == a_faction) {
				return r;
			}
		}
		return -1;
	}

	std::vector<Item> Inventory(TESObjectREFR* a_ref)
	{
		std::map<TESForm*, Item> items;
		if (!a_ref) {
			return {};
		}
		// what the base object starts with
		if (auto* base = a_ref->baseForm) {
			if (auto* cont = Cast<TESContainer>(base, RTTI_TESForm, RTTI_TESContainer)) {
				for (UInt32 i = 0; i < cont->numEntries; ++i) {
					auto* e = cont->entries ? cont->entries[i] : nullptr;
					if (e && e->form && e->form->formType != kFormType_LeveledItem) {
						auto& it = items[e->form];
						it.form = e->form;
						it.count += static_cast<SInt32>(e->count);
					}
				}
			}
		}
		// the changes: count delta and the stacks with their own extra data
		auto* xc = static_cast<ExtraContainerChanges*>(a_ref->extraData.GetByType(kExtraData_ContainerChanges));
		if (xc && xc->data && xc->data->objList) {
			ForEach(*xc->data->objList, [&](InventoryEntryData* e) {
				if (!e->type) {
					return;
				}
				auto& it = items[e->type];
				it.form = e->type;
				it.count += e->countDelta;
				if (e->extendDataList) {
					ForEach(*e->extendDataList, [&](BaseExtraList* xl) { it.extras.push_back(xl); });
				}
			});
		}
		std::vector<Item> out;
		for (auto& [f, it] : items) {
			out.push_back(std::move(it));
		}
		return out;
	}

	bool MenuOpen()
	{
		if (PauseCount() > 0) {
			return true;
		}
		auto* mm = MenuManager::GetSingleton();
		if (!mm) {
			return false;
		}
		BSFixedString journal("Journal Menu");  // released by its destructor (himika's StringCache::Ref)
		return mm->IsMenuOpen(&journal);
	}
}
