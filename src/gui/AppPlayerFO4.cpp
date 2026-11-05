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

	// ---- caps ----
	const bool hChar = ImGui::CollapsingHeader("Caps", ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip("The bottle caps your character carries.");
	if (hChar) {
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
	ImGui::TextDisabled("Perks, level and XP for Fallout 4 saves come in a later version.");
	ImGui::End();
}
