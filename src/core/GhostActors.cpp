// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Spawned actors whose base is another actor's temporary leveled base (see GhostActors.h).
#include "GhostActors.h"

#include "ChangeFormData.h"
#include "ModRemove.h"
#include "Papyrus.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ess::ghost
{
	namespace
	{
		const cf::Node* FindNode(const cf::Node& a_n, const char* a_label)
		{
			if (a_n.label == a_label) {
				return &a_n;
			}
			for (const auto& k : a_n.kids) {
				if (const auto* r = FindNode(k, a_label)) {
					return r;
				}
			}
			return nullptr;
		}

		void Factions(const cf::Node& a_n, std::vector<std::uint32_t>& a_out)
		{
			if (a_n.label == "FACTION" && a_n.isRef && a_n.formID) {
				a_out.push_back(a_n.formID);
			}
			for (const auto& k : a_n.kids) {
				Factions(k, a_out);
			}
		}

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}
	}

	std::vector<Actor> Find(const SaveFile& a_save)
	{
		std::vector<Actor> out;
		if (a_save.isFO4()) {
			return out;  // (Fallout 4 actors are decoded elsewhere; not checked there)
		}
		// form id -> change form type, for the created forms the save has
		std::unordered_map<std::uint32_t, std::uint8_t> created;
		for (const auto& c : a_save.changeForms) {
			if (c.refID.type() == RefID::Type::kCreated) {
				created.emplace(a_save.formIDOf(c.refID), c.typeCode());
			}
		}
		const auto isMissing = [&](std::uint32_t a_id, std::string& a_heldBy) {
			if ((a_id >> 24) != 0xFF) {
				return false;
			}
			const auto it = created.find(a_id);
			if (it == created.end()) {
				return true;
			}
			if (static_cast<ChangeFormType>(it->second) == ChangeFormType::NPC_) {
				return false;
			}
			a_heldBy = ChangeFormTypeName(it->second, false);
			return true;
		};
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::ACHR || c.refID.type() != RefID::Type::kCreated) {
				continue;
			}
			cf::Decoded d;
			try {
				d = cf::Decode(a_save, c, {}, true);  // initial data and extra data come before the inventory
			} catch (...) {
				continue;
			}
			Actor a;
			a.refRaw = c.refID.data;
			a.formID = a_save.formIDOf(c.refID);
			a.base = d.baseObject;
			a.leveledBase = d.leveledBase;
			a.leveledTemplate = d.leveledTemplate;
			std::string heldBase, heldLeveled;
			const bool  baseGone = isMissing(a.base, heldBase);
			const bool  leveledGone = isMissing(a.leveledBase, heldLeveled);
			if (!baseGone && !leveledGone) {
				continue;
			}
			a.heldBy = baseGone ? heldBase : heldLeveled;
			if (const auto* init = FindNode(d.root, "INITIAL")) {
				if (const auto* cell = FindNode(*init, "CELL"); cell && cell->isRef) {
					a.cell = cell->formID;
				}
			}
			std::vector<std::uint32_t> factions;
			if (const auto* fc = FindNode(d.root, "FactionChanges")) {
				Factions(*fc, factions);
			}
			for (const auto f : factions) {
				const auto pl = a_save.pluginOf(f);
				if ((f >> 24) == 0xFF || modremove::IsBaseGamePluginName(Lower(pl)) ||
					std::find(a.plugins.begin(), a.plugins.end(), pl) != a.plugins.end()) {
					continue;
				}
				a.plugins.push_back(pl);
			}
			out.push_back(std::move(a));
		}
		if (const auto* p = a_save.papyrus(); p && !out.empty()) {
			std::unordered_map<std::uint32_t, std::size_t> byRef;
			for (const auto& i : p->instances) {
				++byRef[i.refID.data];
			}
			for (auto& a : out) {
				if (const auto it = byRef.find(a.refRaw); it != byRef.end()) {
					a.scripts = it->second;
				}
			}
		}
		return out;
	}

	std::string Describe(const SaveFile& a_save, const Actor& a_actor)
	{
		char b[512];
		std::string s;
		std::snprintf(b, sizeof(b), "%08X (base %08X not in the save", a_actor.formID, a_actor.missing());
		s = b;
		if (!a_actor.heldBy.empty()) {
			s += "; that id is a " + a_actor.heldBy + " in the save";
		}
		if (a_actor.leveledTemplate) {
			std::snprintf(b, sizeof(b), "; built from %08X, %s", a_actor.leveledTemplate, a_save.pluginOf(a_actor.leveledTemplate).c_str());
			s += b;
		}
		s += ")";
		if (a_actor.cell) {
			std::snprintf(b, sizeof(b), " in %08X", a_actor.cell);
			s += b;
		}
		if (!a_actor.plugins.empty()) {
			s += ", made by";
			for (std::size_t i = 0; i < a_actor.plugins.size(); ++i) {
				s += (i ? ", " : " ") + a_actor.plugins[i];
			}
		}
		return s;
	}

	std::string ConsoleText(const std::vector<std::uint32_t>& a_ids)
	{
		std::string s;
		char        b[32];
		for (const auto id : a_ids) {
			std::snprintf(b, sizeof(b), "prid %08X\n", id);
			s += b;
			s += "disable\nmarkfordelete\n";
		}
		return s;
	}

	std::string RemoveArgument(const std::vector<std::uint32_t>& a_ids)
	{
		std::string s;
		char        b[16];
		for (const auto id : a_ids) {
			std::snprintf(b, sizeof(b), "%08X", id);
			s += (s.empty() ? "" : ",") + std::string(b);
		}
		return s;
	}

	std::vector<RemoveResult> ParseRemoveReply(const std::vector<std::string>& a_fields)
	{
		std::vector<RemoveResult> out;
		if (a_fields.empty()) {
			return out;
		}
		std::size_t n = 0;
		try {
			n = std::stoul(a_fields[0]);
		} catch (...) {
			return out;
		}
		if (a_fields.size() != n + 1) {
			return {};
		}
		for (std::size_t i = 1; i < a_fields.size(); ++i) {
			const auto& f = a_fields[i];
			const auto  star = f.find('*');
			if (star == std::string::npos || star == 0) {
				return {};
			}
			RemoveResult r;
			try {
				std::size_t used = 0;
				r.id = static_cast<std::uint32_t>(std::stoul(f.substr(0, star), &used, 16));
				if (used != star) {
					return {};
				}
			} catch (...) {
				return {};
			}
			r.word = f.substr(star + 1);
			out.push_back(std::move(r));
		}
		return out;
	}
}
