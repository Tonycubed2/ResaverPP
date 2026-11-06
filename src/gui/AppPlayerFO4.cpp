// Edit player for Fallout 4 saves: level, experience, caps, S.P.E.C.I.A.L., perks and the active effects (read from
// the player's ACHR change form; see core/ChangeFormFO4.h).
#include "App.h"
#include "Theme.h"
#include "Tips.h"
#include "imgui.h"
#include "core/ChangeFormFO4.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

using namespace ess;

namespace
{
	const char* const kSpecial[7] = { N_("Strength"), N_("Perception"), N_("Endurance"), N_("Charisma"), N_("Intelligence"), N_("Agility"), N_("Luck") };
	constexpr std::uint32_t kSpecialAV = 0x2C2;  // Strength .. Luck are 0x2C2 .. 0x2C8

	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}
}

void App::drawPlayerFO4()
{
	prepareWindow("Edit player", ImVec2(0.6f, 0.8f));
	if (!ImGui::Begin(trId("Edit player"), &_showPlayer, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Edit player");
	if (!_pvLoaded) {
		_pvLoaded = true;
		_f4Error.clear();
		_f4Actor = {};
		_f4Caps = -1;
		const auto* pc = fo4cf::FindPlayer(*_save);
		if (!pc) {
			_f4Error = tr("the player's saved data (ACHR 00000014) is not in this save");
		} else {
			_f4Actor = fo4cf::DecodeActor(*_save, *pc);
			if (!_f4Actor.decoded) {
				_f4Error = tr("the player's saved data could not be read: ") + _f4Actor.error;
			}
			_f4Caps = fo4cf::PlayerCaps(*_save);
		}
		for (int i = 0; i < 7; ++i) {
			_f4Special[i] = -1;
			for (const auto& v : _f4Actor.values) {
				if (v.formID == kSpecialAV + static_cast<std::uint32_t>(i)) {
					_f4Special[i] = static_cast<int>(v.base + 0.5f);
				}
			}
			_f4SpecialEdit[i] = _f4Special[i];
		}
		_f4CapsEdit = static_cast<int>(_f4Caps < 0 ? 0 : _f4Caps);
		_f4Level = fo4cf::PlayerLevel(*_save);
		_f4LevelEdit = std::max(1, _f4Level);
		_f4XP = fo4cf::PlayerXP(*_save);
		_f4XPEdit = static_cast<float>(_f4XP < 0 ? 0 : _f4XP);
		_f4PerkEdit.clear();
		_f4FxSources.clear();
		for (const auto& k : _f4Actor.perks) {
			_f4PerkEdit[k.formID] = k.rank;
		}
	}
	if (!_f4Error.empty()) {
		TextColored(theme::Warn(), tr("Cannot edit the player: ") + _f4Error);
		ImGui::End();
		return;
	}
	ImGui::NewLine();
	ImGui::TextWrapped("%s", tr("Changes are made in the loaded save; nothing is written until you use File > Save or Save As. Keep the original "
					   "save and test the new one before playing on with it."));
	auto done = [this](const std::string& a_why, const std::string& a_what) {
		if (a_why.empty()) {
			markChanged(a_what);
		} else {
			_error = tr("Could not change the player: ") + a_why;
		}
		_pvLoaded = false;  // read the values again
	};
	auto nameOf = [this](std::uint32_t a_id) {
		const auto* f = _db ? _db->find(a_id) : nullptr;
		if (!f) {
			return Fmt("%08X", a_id);
		}
		const auto n = _db->name(*f);
		return n.empty() ? _db->edid(*f) : n;
	};

	// ---- character: level, XP, caps ----
	if (std::getenv("RESAVERPP_TEST_FX")) {  // test: only the active effects open
		ImGui::SetNextItemOpen(false, ImGuiCond_Once);
	}
	const bool hChar = ImGui::CollapsingHeader(trId("Character"), ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip(tr("Level, experience and the bottle caps your character carries."));
	if (hChar) {
		ImGui::TextUnformatted(tr("Level"));
		ImGui::SameLine(120);
		if (_f4Level < 0) {
			ImGui::TextDisabled(tr("%u (this save keeps the game's starting level: it can be changed once the character has levelled up)"),
				static_cast<unsigned>(_save->header.playerLevel));
		} else {
			ImGui::SetNextItemWidth(150);
			ImGui::InputInt("##level", &_f4LevelEdit, 1, 10);
			_f4LevelEdit = std::clamp(_f4LevelEdit, 1, 65535);
			tips::Tip(tr("The character level. Type a new level, then press Set. The save's header (the load menu) is changed too."));
			ImGui::SameLine();
			ImGui::BeginDisabled(_f4LevelEdit == _f4Level);
			if (ImGui::SmallButton(tr("Set##level"))) {
				done(fo4cf::SetPlayerLevel(*_save, _f4LevelEdit), Fmt(tr("Level set to %d"), _f4LevelEdit));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled(tr("now %d"), _f4Level);
		}
		ImGui::TextUnformatted(tr("Experience"));
		ImGui::SameLine(120);
		if (_f4XP < 0) {
			ImGui::TextDisabled("%s", tr("not saved for this character yet"));
		} else {
			ImGui::SetNextItemWidth(150);
			ImGui::InputFloat("##xp", &_f4XPEdit, 100.0f, 1000.0f, "%.0f");
			_f4XPEdit = std::max(0.0f, _f4XPEdit);
			tips::Tip(tr("The experience points earned so far. Type a new amount, then press Set."));
			ImGui::SameLine();
			ImGui::BeginDisabled(static_cast<double>(_f4XPEdit) == _f4XP);
			if (ImGui::SmallButton(tr("Set##xp"))) {
				done(fo4cf::SetPlayerXP(*_save, _f4XPEdit), Fmt(tr("Experience set to %.0f"), _f4XPEdit));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled(tr("now %.0f"), _f4XP);
		}
		ImGui::TextUnformatted(tr("Caps"));
		ImGui::SameLine(120);
		if (_f4Caps < 0) {
			ImGui::TextDisabled("%s", tr("The player carries no caps in this save, so there is no caps entry to change."));
		} else {
			ImGui::SetNextItemWidth(150);
			ImGui::InputInt("##caps", &_f4CapsEdit, 100, 1000);
			_f4CapsEdit = std::max(0, _f4CapsEdit);
			tips::Tip(tr("Type a new amount, then press Set."));
			ImGui::SameLine();
			ImGui::BeginDisabled(_f4CapsEdit == _f4Caps);
			if (ImGui::SmallButton(tr("Set##caps"))) {
				done(fo4cf::SetPlayerCaps(*_save, _f4CapsEdit), Fmt(tr("Caps set to %d"), _f4CapsEdit));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled(tr("now %lld"), static_cast<long long>(_f4Caps));
		}
	}

	// ---- S.P.E.C.I.A.L. ----
	if (std::getenv("RESAVERPP_TEST_FX")) {  // test: only the active effects open
		ImGui::SetNextItemOpen(false, ImGuiCond_Once);
	}
	const bool hSpecial = ImGui::CollapsingHeader("S.P.E.C.I.A.L.", ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip(tr("The character's saved base S.P.E.C.I.A.L. values (gear, chems and food add their bonuses on top while they are active)."));
	if (hSpecial && ImGui::BeginTable("special", 4, ImGuiTableFlags_SizingFixedFit)) {
		for (int i = 0; i < 7; ++i) {
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(tr(kSpecial[i]));
			ImGui::TableNextColumn();
			if (_f4Special[i] < 0) {
				ImGui::TextDisabled("%s", tr("not saved"));
				ImGui::PopID();
				continue;
			}
			ImGui::SetNextItemWidth(120);
			ImGui::InputInt("##v", &_f4SpecialEdit[i], 1, 1);
			_f4SpecialEdit[i] = std::clamp(_f4SpecialEdit[i], 1, 100);
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(_f4SpecialEdit[i] == _f4Special[i]);
			if (ImGui::SmallButton(tr("Set"))) {
				done(fo4cf::SetPlayerSpecial(*_save, i, static_cast<float>(_f4SpecialEdit[i])),
					Fmt(tr("%s set to %d"), tr(kSpecial[i]), _f4SpecialEdit[i]));
			}
			ImGui::EndDisabled();
			ImGui::TableNextColumn();
			ImGui::TextDisabled(tr("now %d"), _f4Special[i]);
			ImGui::PopID();
		}
		ImGui::EndTable();
		tips::Tip(tr("The saved base value. The character screen stops at 10 when you level up, but the saved value can be higher (some saves have 11)."));
	}

	// ---- perks ----
	if (std::getenv("RESAVERPP_TEST_FX")) {  // test: only the active effects open
		ImGui::SetNextItemOpen(false, ImGuiCond_Once);
	}
	const bool hPerks = ImGui::CollapsingHeader(Fmt(tr("Perks (%zu)###perks"), _f4Actor.perks.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip(tr("The perks the character has taken, with their rank. Change a rank, remove a perk or add one."));
	if (hPerks) {
		auto* pcf = [this]() -> ChangeForm* {
			const auto* pc = fo4cf::FindPlayer(*_save);
			return pc ? _save->findChangeForm(pc->refID) : nullptr;
		}();
		if (!_f4Actor.hasPerks) {
			ImGui::TextDisabled(tr("The perks could not be read in this save: %s"), _f4Actor.error.empty() ? tr("no perk list") : _f4Actor.error.c_str());
		} else if (pcf) {
			ImGui::SetNextItemWidth(250);
			ImGui::InputTextWithHint("##perkf", tr("filter (e.g. gun nut, local leader)"), _f4PerkFilter, sizeof(_f4PerkFilter));
			std::string f = _f4PerkFilter;
			std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			std::uint32_t removeId = 0;
			if (ImGui::BeginTable("f4perks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
					ImVec2(0, 300))) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn(tr("Perk"));
				ImGui::TableSetupColumn(tr("Form id"), ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn(tr("Plugin"));
				ImGui::TableSetupColumn(tr("Rank"), ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableHeadersRow();
				for (const auto& k : _f4Actor.perks) {
					const auto name = nameOf(k.formID);
					const auto plugin = _save->pluginOf(k.formID);
					if (!f.empty()) {
						std::string t = name + " " + plugin + Fmt(" %08X", k.formID);
						std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
						if (t.find(f) == std::string::npos) {
							continue;
						}
					}
					ImGui::PushID(static_cast<int>(k.formID));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(name.c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%08X", k.formID);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(plugin.c_str());
					ImGui::TableNextColumn();
					auto& r = _f4PerkEdit[k.formID];
					ImGui::SetNextItemWidth(100);
					ImGui::InputInt("##r", &r, 1, 1);
					r = std::clamp(r, 1, 255);
					ImGui::TableNextColumn();
					ImGui::BeginDisabled(r == k.rank);
					if (ImGui::SmallButton(tr("Set"))) {
						done(fo4cf::SetPerkRank(*_save, *pcf, k.formID, static_cast<std::uint8_t>(r)), Fmt(tr("%s: rank %d"), name.c_str(), r));
					}
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Remove"))) {
						removeId = k.formID;
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (removeId) {
				const auto name = nameOf(removeId);
				done(fo4cf::RemovePerk(*_save, *pcf, removeId), tr("Perk removed: ") + name);
			}
			ImGui::TextUnformatted(tr("Add a perk (form id, hex):"));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120);
			ImGui::InputTextWithHint("##addperk", "e.g. 0004D88B", _f4AddPerk, sizeof(_f4AddPerk), ImGuiInputTextFlags_CharsHexadecimal);
			tips::Tip(tr("The perk's form id, as Find form by name (Tools menu) shows it. Only perks this save already refers to can be added."));
			ImGui::SameLine();
			ImGui::TextUnformatted(tr("rank"));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(100);
			ImGui::InputInt("##addrank", &_f4AddRank, 1, 1);
			_f4AddRank = std::clamp(_f4AddRank, 1, 255);
			ImGui::SameLine();
			ImGui::BeginDisabled(!_f4AddPerk[0]);
			if (ImGui::SmallButton(tr("Add"))) {
				const auto id = static_cast<std::uint32_t>(std::strtoul(_f4AddPerk, nullptr, 16));
				done(fo4cf::AddPerk(*_save, *pcf, id, static_cast<std::uint8_t>(_f4AddRank)), Fmt(tr("Perk added: %s, rank %d"), nameOf(id).c_str(), _f4AddRank));
				_f4AddPerk[0] = 0;
			}
			ImGui::EndDisabled();
		}
	}

	// ---- active effects ----
	if (std::getenv("RESAVERPP_TEST_FX")) {  // test: open this section
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	}
	const bool hFx = ImGui::CollapsingHeader(Fmt(tr("Active effects (%zu)###fx"), _f4Actor.effects.size()).c_str());
	tips::Tip(tr("The effects running on the character: chems, food, radiation, perks' and gear's effects. Remove ones that are stuck; the stat they change is put back."));
	if (hFx) {
		auto* pcf = [this]() -> ChangeForm* {
			const auto* pc = fo4cf::FindPlayer(*_save);
			return pc ? _save->findChangeForm(pc->refID) : nullptr;
		}();
		ImGui::TextWrapped("%s", tr("Remove takes an effect off the character, and when it is running and changes a stat, puts the stat back as the "
						   "game does when the effect ends. An effect from something you still wear, a perk or a mod's script can come back "
						   "when the game gives it again."));
		ImGui::TextWrapped("%s", tr("The effect's own scripts (its ActiveMagicEffect script instances, with their running threads) are removed "
						   "with it, as when the game ends an effect. If another script still holds one of them, that instance is kept "
						   "and the result says so."));
		if (!_db) {
			TextColored(theme::Warn(), tr("Plugin names are not loaded: running effects that change a stat cannot be removed (which stat is in "
										   "the plugins). Load them (Tools > Read plugin names)."));
		}
		if (!_f4Actor.complete) {
			TextColored(theme::Warn(), tr("The player's saved data does not read completely, so effects cannot be removed: ") + _f4Actor.error);
		}
		if (_f4FxSources.size() != _f4Actor.effects.size() || _f4FxSourcesDb != _db.get()) {
			_f4FxSources = fo4cf::EffectSources(*_save, _f4Actor, _db.get());
			_f4FxSourcesDb = _db.get();
		}
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##f4fxf", tr("filter (e.g. jet, rad, buffout)"), _pvFxFilter, sizeof(_pvFxFilter));
		std::string f = _pvFxFilter;
		std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		std::vector<std::size_t> remove;
		std::string              removeWhat;
		if (ImGui::BeginTable("f4fx", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
				ImVec2(0, 340))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(tr("From"));
			ImGui::TableSetupColumn(tr("Form id"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(tr("Plugin"));
			ImGui::TableSetupColumn(tr("State"));
			ImGui::TableSetupColumn(tr("When removed"));
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();
			for (std::size_t i = 0; i < _f4Actor.effects.size(); ++i) {
				const auto& e = _f4Actor.effects[i];
				const auto  id = _save->formIDOf(e.item);
				const auto  name = nameOf(id);
				const auto  plugin = _save->pluginOf(id);
				const auto& src = _f4FxSources[i];
				const auto  plan = fo4cf::PlanEffectRemoval(_f4Actor, e, &src);
				std::string state;
				if (e.data.size() < 16) {
					state = tr("not readable");
				} else if (e.started()) {
					state = Fmt(tr("running, strength %g"), e.magnitude);
				} else if (e.flags & 0x20000u) {
					state = tr("ended (switched off)");
				} else {
					state = tr("not started");
				}
				std::string when;
				if (!plan.ok) {
					when = tr("cannot be removed: ") + plan.why;
				} else if (plan.modifier && plan.amount != 0) {
					when = Fmt(tr("%s %+g (modifier %d)"), nameOf(plan.actorValue).c_str(), -plan.amount, plan.slot);
				} else {
					when = tr("no stat changes");
				}
				const auto effectName = src.mgef ? nameOf(src.mgef) : Fmt(tr("effect %u"), e.effectIndex);
				if (!f.empty()) {
					std::string t = name + " " + effectName + " " + plugin + Fmt(" %08X ", id) + when;
					std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
					if (t.find(f) == std::string::npos) {
						continue;
					}
				}
				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Fmt("%s  (%s)", name.c_str(), effectName.c_str()).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%08X", id);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(plugin.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(state.c_str());
				ImGui::TableNextColumn();
				if (plan.ok) {
					ImGui::TextUnformatted(when.c_str());
				} else {
					ImGui::TextDisabled("%s", when.c_str());
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", when.c_str());
				}
				ImGui::TableNextColumn();
				ImGui::BeginDisabled(!plan.ok || !_f4Actor.complete || !pcf);
				if (ImGui::SmallButton(tr("Remove"))) {
					remove = { i };
					removeWhat = name + " (" + effectName + ")";
				}
				tips::Tip(tr("Take this effect off the character (asks first)."));
				ImGui::EndDisabled();
				const auto same = fo4cf::EffectsFrom(*_save, _f4Actor, id);
				if (same.size() > 1) {
					bool allOk = true;
					for (const auto k : same) {
						allOk = allOk && fo4cf::PlanEffectRemoval(_f4Actor, _f4Actor.effects[k], &_f4FxSources[k]).ok;
					}
					ImGui::SameLine();
					ImGui::BeginDisabled(!allOk || !_f4Actor.complete || !pcf);
					if (ImGui::SmallButton(Fmt(tr("All %zu from this source"), same.size()).c_str())) {
						remove = same;
						removeWhat = Fmt(tr("all %zu effects from %s"), same.size(), name.c_str());
					}
					tips::Tip(allOk ? tr("Take every effect this item, spell or enchantment put on the character off it (asks first).")
					                : tr("One of this source's effects cannot be removed (see When removed)."));
					ImGui::EndDisabled();
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (!remove.empty() && pcf) {
			std::string stats;
			for (const auto i : remove) {
				const auto plan = fo4cf::PlanEffectRemoval(_f4Actor, _f4Actor.effects[i], &_f4FxSources[i]);
				if (plan.modifier && plan.amount != 0) {
					stats += Fmt("\n  %s %+g", nameOf(plan.actorValue).c_str(), -plan.amount);
				}
			}
			const auto sources = _f4FxSources;
			confirm(tr("Remove active effects"),
				Fmt(tr("Remove %s from the player?"), removeWhat.c_str()) + (stats.empty() ? std::string() : tr("\nStats put back:") + stats),
				[this, remove, sources, removeWhat, done] {
					auto* c = [this]() -> ChangeForm* {
						const auto* pc = fo4cf::FindPlayer(*_save);
						return pc ? _save->findChangeForm(pc->refID) : nullptr;
					}();
					if (!c) {
						done(tr("the player's saved data is not in this save"), "");
						return;
					}
					std::vector<std::string> changes;
					const auto               why = fo4cf::RemoveActiveEffects(*_save, *c, remove, sources, &changes);
					std::string              what = tr("Effects removed: ") + removeWhat;
					for (std::size_t k = 0; k < changes.size() && k < 6; ++k) {
						what += (k ? "; " : ": ") + changes[k];
					}
					done(why, what);
				});
		}
	}

	ImGui::NewLine();
	ImGui::End();
}
