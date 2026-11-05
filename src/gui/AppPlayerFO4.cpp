// Edit player for Fallout 4 saves: caps, S.P.E.C.I.A.L. and the active effects (read from the player's ACHR
// change form; see core/ChangeFormFO4.h).
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
	const char* const kSpecial[7] = { "Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck" };
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
	if (!ImGui::Begin("Edit player", &_showPlayer, ImGuiWindowFlags_NoCollapse)) {
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
			_f4Error = "the player's saved data (ACHR 00000014) is not in this save";
		} else {
			_f4Actor = fo4cf::DecodeActor(*_save, *pc);
			if (!_f4Actor.decoded) {
				_f4Error = "the player's saved data could not be read: " + _f4Actor.error;
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
		for (const auto& k : _f4Actor.perks) {
			_f4PerkEdit[k.formID] = k.rank;
		}
	}
	if (!_f4Error.empty()) {
		TextColored(theme::Warn(), "Cannot edit the player: " + _f4Error);
		ImGui::End();
		return;
	}
	ImGui::NewLine();
	ImGui::TextWrapped("Changes are made in the loaded save; nothing is written until you use File > Save or Save As. Keep the original "
					   "save and test the new one before playing on with it.");
	auto done = [this](const std::string& a_why, const std::string& a_what) {
		if (a_why.empty()) {
			markChanged(a_what);
		} else {
			_error = "Could not change the player: " + a_why;
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
	const bool hChar = ImGui::CollapsingHeader("Character", ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip("Level, experience and the bottle caps your character carries.");
	if (hChar) {
		ImGui::TextUnformatted("Level");
		ImGui::SameLine(120);
		if (_f4Level < 0) {
			ImGui::TextDisabled("%u (this save keeps the game's starting level: it can be changed once the character has levelled up)",
				static_cast<unsigned>(_save->header.playerLevel));
		} else {
			ImGui::SetNextItemWidth(150);
			ImGui::InputInt("##level", &_f4LevelEdit, 1, 10);
			_f4LevelEdit = std::clamp(_f4LevelEdit, 1, 65535);
			tips::Tip("The character level. Type a new level, then press Set. The save's header (the load menu) is changed too.");
			ImGui::SameLine();
			ImGui::BeginDisabled(_f4LevelEdit == _f4Level);
			if (ImGui::SmallButton("Set##level")) {
				done(fo4cf::SetPlayerLevel(*_save, _f4LevelEdit), Fmt("Level set to %d", _f4LevelEdit));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("now %d", _f4Level);
		}
		ImGui::TextUnformatted("Experience");
		ImGui::SameLine(120);
		if (_f4XP < 0) {
			ImGui::TextDisabled("not saved for this character yet");
		} else {
			ImGui::SetNextItemWidth(150);
			ImGui::InputFloat("##xp", &_f4XPEdit, 100.0f, 1000.0f, "%.0f");
			_f4XPEdit = std::max(0.0f, _f4XPEdit);
			tips::Tip("The experience points earned so far. Type a new amount, then press Set.");
			ImGui::SameLine();
			ImGui::BeginDisabled(static_cast<double>(_f4XPEdit) == _f4XP);
			if (ImGui::SmallButton("Set##xp")) {
				done(fo4cf::SetPlayerXP(*_save, _f4XPEdit), Fmt("Experience set to %.0f", _f4XPEdit));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("now %.0f", _f4XP);
		}
		ImGui::TextUnformatted("Caps");
		ImGui::SameLine(120);
		if (_f4Caps < 0) {
			ImGui::TextDisabled("The player carries no caps in this save, so there is no caps entry to change.");
		} else {
			ImGui::SetNextItemWidth(150);
			ImGui::InputInt("##caps", &_f4CapsEdit, 100, 1000);
			_f4CapsEdit = std::max(0, _f4CapsEdit);
			tips::Tip("Type a new amount, then press Set.");
			ImGui::SameLine();
			ImGui::BeginDisabled(_f4CapsEdit == _f4Caps);
			if (ImGui::SmallButton("Set##caps")) {
				done(fo4cf::SetPlayerCaps(*_save, _f4CapsEdit), Fmt("Caps set to %d", _f4CapsEdit));
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("now %lld", static_cast<long long>(_f4Caps));
		}
	}

	// ---- S.P.E.C.I.A.L. ----
	const bool hSpecial = ImGui::CollapsingHeader("S.P.E.C.I.A.L.", ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip("The character's saved base S.P.E.C.I.A.L. values (gear, chems and food add their bonuses on top while they are active).");
	if (hSpecial && ImGui::BeginTable("special", 4, ImGuiTableFlags_SizingFixedFit)) {
		for (int i = 0; i < 7; ++i) {
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(kSpecial[i]);
			ImGui::TableNextColumn();
			if (_f4Special[i] < 0) {
				ImGui::TextDisabled("not saved");
				ImGui::PopID();
				continue;
			}
			ImGui::SetNextItemWidth(120);
			ImGui::InputInt("##v", &_f4SpecialEdit[i], 1, 1);
			_f4SpecialEdit[i] = std::clamp(_f4SpecialEdit[i], 1, 100);
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(_f4SpecialEdit[i] == _f4Special[i]);
			if (ImGui::SmallButton("Set")) {
				done(fo4cf::SetPlayerSpecial(*_save, i, static_cast<float>(_f4SpecialEdit[i])),
					Fmt("%s set to %d", kSpecial[i], _f4SpecialEdit[i]));
			}
			ImGui::EndDisabled();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("now %d", _f4Special[i]);
			ImGui::PopID();
		}
		ImGui::EndTable();
		tips::Tip("The saved base value. The character screen stops at 10 when you level up, but the saved value can be higher (some saves have 11).");
	}

	// ---- perks ----
	const bool hPerks = ImGui::CollapsingHeader(Fmt("Perks (%zu)###perks", _f4Actor.perks.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip("The perks the character has taken, with their rank. Change a rank, remove a perk or add one.");
	if (hPerks) {
		auto* pcf = [this]() -> ChangeForm* {
			const auto* pc = fo4cf::FindPlayer(*_save);
			return pc ? _save->findChangeForm(pc->refID) : nullptr;
		}();
		if (!_f4Actor.hasPerks) {
			ImGui::TextDisabled("The perks could not be read in this save: %s", _f4Actor.error.empty() ? "no perk list" : _f4Actor.error.c_str());
		} else if (pcf) {
			ImGui::SetNextItemWidth(250);
			ImGui::InputTextWithHint("##perkf", "filter (e.g. gun nut, local leader)", _f4PerkFilter, sizeof(_f4PerkFilter));
			std::string f = _f4PerkFilter;
			std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			std::uint32_t removeId = 0;
			if (ImGui::BeginTable("f4perks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
					ImVec2(0, 300))) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Perk");
				ImGui::TableSetupColumn("Form id", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Plugin");
				ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed);
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
					if (ImGui::SmallButton("Set")) {
						done(fo4cf::SetPerkRank(*_save, *pcf, k.formID, static_cast<std::uint8_t>(r)), Fmt("%s: rank %d", name.c_str(), r));
					}
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove")) {
						removeId = k.formID;
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (removeId) {
				const auto name = nameOf(removeId);
				done(fo4cf::RemovePerk(*_save, *pcf, removeId), "Perk removed: " + name);
			}
			ImGui::TextUnformatted("Add a perk (form id, hex):");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120);
			ImGui::InputTextWithHint("##addperk", "e.g. 0004D88B", _f4AddPerk, sizeof(_f4AddPerk), ImGuiInputTextFlags_CharsHexadecimal);
			tips::Tip("The perk's form id, as Find form by name (Tools menu) shows it. Only perks this save already refers to can be added.");
			ImGui::SameLine();
			ImGui::TextUnformatted("rank");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(100);
			ImGui::InputInt("##addrank", &_f4AddRank, 1, 1);
			_f4AddRank = std::clamp(_f4AddRank, 1, 255);
			ImGui::SameLine();
			ImGui::BeginDisabled(!_f4AddPerk[0]);
			if (ImGui::SmallButton("Add")) {
				const auto id = static_cast<std::uint32_t>(std::strtoul(_f4AddPerk, nullptr, 16));
				done(fo4cf::AddPerk(*_save, *pcf, id, static_cast<std::uint8_t>(_f4AddRank)), Fmt("Perk added: %s, rank %d", nameOf(id).c_str(), _f4AddRank));
				_f4AddPerk[0] = 0;
			}
			ImGui::EndDisabled();
		}
	}

	// ---- active effects ----
	const bool hFx = ImGui::CollapsingHeader(Fmt("Active effects (%zu)###fx", _f4Actor.effects.size()).c_str());
	tips::Tip("The effects running on the character: chems, food, radiation, perks' and gear's effects. Shown for reading.");
	if (hFx) {
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##f4fxf", "filter (e.g. jet, rad, buffout)", _pvFxFilter, sizeof(_pvFxFilter));
		std::string f = _pvFxFilter;
		std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		if (ImGui::BeginTable("f4fx", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
				ImVec2(0, 300))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("From");
			ImGui::TableSetupColumn("Form id");
			ImGui::TableSetupColumn("Plugin");
			ImGui::TableHeadersRow();
			for (const auto& e : _f4Actor.effects) {
				const auto id = _save->formIDOf(e.item);
				const auto name = nameOf(id);
				const auto plugin = _save->pluginOf(id);
				if (!f.empty()) {
					std::string t = name + " " + plugin + Fmt(" %08X", id);
					std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
					if (t.find(f) == std::string::npos) {
						continue;
					}
				}
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Fmt("%s  (effect %u)", name.c_str(), e.effectIndex).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%08X", id);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(plugin.c_str());
			}
			ImGui::EndTable();
		}
	}

	ImGui::NewLine();
	ImGui::End();
}
