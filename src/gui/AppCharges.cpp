// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Mods view: the enchanted and charged items of the selected mod found in inventories. Skyrim keeps
// an item's own enchantment and its max charge (ExtraEnchantment) and its charge left (ExtraCharge) in
// the save, on that one item, so xEdit never shows them. An item with an enchantment but max charge 0
// can never be recharged with soul gems; it can be fixed here.
#include "App.h"

#include <chrono>

#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	bool EqualNoCase(const std::string& a, const std::string& b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}
}

const std::vector<cf::ChargedItem>& App::chargedItems()
{
	if (_chargedGen != _editGen) {
		_charged = _save ? cf::FindChargedItems(*_save) : std::vector<cf::ChargedItem>{};
		_chargedGen = _editGen;
	}
	return _charged;
}

// The finished result only (empty while the scan runs): pollPathIdles fills it a few actors per frame.
const std::vector<cf::StuckPathIdle>& App::stuckPathIdles()
{
	return _stuckPath;
}

void App::pollPathIdles()
{
	if (_stuckPathGen != _editGen) {  // another save, or an edit: start again
		_stuckPathGen = _editGen;
		_stuckPath.clear();
		_stuckPathScan.clear();
		_stuckPathNext = 0;
		_stuckPathDone = false;
		_stuckPathMs = 0;
	}
	if (!_save || _stuckPathDone) {
		return;
	}
	const auto  t0 = std::chrono::steady_clock::now();
	const auto& forms = _save->changeForms;
	while (_stuckPathNext < forms.size()) {
		const auto& c = forms[_stuckPathNext++];
		const auto  f0 = std::chrono::steady_clock::now();
		try {
			if (const auto n = cf::CountStuckPathIdles(*_save, c)) {
				_stuckPathScan.push_back({ c.refID.data, c.refID.isZero() ? 0 : _save->formIDOf(c.refID), n });
			}
		} catch (...) {
		}
		if (const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count(); ms > 200) {
			_log.push_back(Fmt("Path idle check: change form %08X took %.0f ms to read", c.refID.isZero() ? 0 : _save->formIDOf(c.refID), ms));
		}
		if ((_stuckPathNext & 15) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(25)) {
			break;  // the rest next frame: the window stays responsive
		}
	}
	_stuckPathMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (_stuckPathMs > 30000 && _stuckPathNext < forms.size()) {
		_log.push_back(Fmt("Path idle check stopped after 30 seconds (%zu of %zu change forms read); what it found so far is shown",
			_stuckPathNext, forms.size()));
		_stuckPathNext = forms.size();
	}
	if (_stuckPathNext >= forms.size()) {
		_stuckPathDone = true;
		_stuckPath = std::move(_stuckPathScan);
		_stuckPathScan.clear();
		_log.push_back(Fmt("Path idle check: %zu change forms read in %.0f ms, %zu character(s) waiting for an idle", forms.size(),
			_stuckPathMs, _stuckPath.size()));
	}
}

void App::drawModCharged(const std::string& a_plugin)
{
	if (!_save) {
		return;
	}
	std::vector<const cf::ChargedItem*> mine;
	for (const auto& c : chargedItems()) {
		if (EqualNoCase(_save->pluginOf(c.inv.item), a_plugin) || (c.inv.ench && EqualNoCase(_save->pluginOf(c.inv.ench), a_plugin))) {
			mine.push_back(&c);
		}
	}
	std::size_t broken = 0;
	for (const auto* c : mine) {
		broken += cf::NeedsChargeFix(c->inv) ? 1 : 0;
	}
	ImGui::SeparatorText(Fmt("Enchanted and charged items from this mod (%zu)", mine.size()).c_str());
	if (mine.empty()) {
		ImGui::TextDisabled("(none in any inventory)");
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
	ImGui::TextWrapped("Items whose base record or enchantment comes from this mod, in any inventory (yours, followers', containers). "
					   "Enchantment and Max charge are stored on the item itself in the save (ExtraEnchantment), Charge is what is left "
					   "(ExtraCharge); xEdit does not show these. \"From the plugin\": the base record's own enchantment and EAMT apply. "
					   "An item with an enchantment but max charge 0 can never be recharged with soul gems.");
	ImGui::PopStyleColor();
	if (broken) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::TextWrapped("%zu item(s) cannot be recharged.", broken);
		ImGui::PopStyleColor();
	}

	if (ImGui::BeginTable("charged", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 2.2f);
		ImGui::TableSetupColumn("Held by", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("Enchantment", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("Max charge", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("Charge", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableHeadersRow();
		for (const auto* c : mine) {
			const auto& e = c->inv;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(formText(e.item).c_str());
			if (e.count > 1) {
				ImGui::SameLine();
				ImGui::TextDisabled("x%d", e.count);
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(c->holder == 0x14 ? "Player" : formText(c->holder).c_str());
			ImGui::TableNextColumn();
			if (!e.hasEnchant) {
				ImGui::TextDisabled("from the plugin");
			} else if (!e.enchRaw) {
				ImGui::TextDisabled("(none)");
			} else {
				ImGui::TextUnformatted(formText(e.ench).c_str());
				if ((e.ench >> 24) == 0xFF) {
					ImGui::SameLine();
					ImGui::TextDisabled("(made in game)");
				}
			}
			ImGui::TableNextColumn();
			if (e.hasEnchant) {
				ImGui::Text("%u", e.maxCharge);
			} else {
				ImGui::TextDisabled("EAMT");
			}
			ImGui::TableNextColumn();
			if (e.hasCharge) {
				ImGui::Text("%.0f", e.charge);
			} else {
				ImGui::TextDisabled("full");
			}
			ImGui::TableNextColumn();
			if (e.hasEnchant && e.enchRaw && e.maxCharge == 0) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
				ImGui::TextUnformatted("Cannot be recharged (max charge 0)");
				ImGui::PopStyleColor();
			} else if (cf::NeedsChargeFix(e)) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				ImGui::TextUnformatted("Charge above its max");
				ImGui::PopStyleColor();
			} else if (e.hasEnchant && !e.enchRaw) {
				ImGui::TextDisabled("No enchantment set");
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Good());
				ImGui::TextUnformatted("OK");
				ImGui::PopStyleColor();
			}
		}
		ImGui::EndTable();
	}

	if (broken) {
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("Max charge to give them", &_chargeFixValue, 100, 500);
		tips::Tip("The maximum enchantment charge given to the broken items. A grand or black soul gem holds 3000.");
		_chargeFixValue = std::clamp(_chargeFixValue, 1, 65535);
		ImGui::SameLine();
		ImGui::TextDisabled("(a grand or black soul gem holds 3000)");
		if (ImGui::Button(Fmt("Fix %zu item(s) so they can be recharged", broken).c_str())) {
			std::vector<std::pair<std::uint32_t, std::uint32_t>> only;
			for (const auto* c : mine) {
				if (cf::NeedsChargeFix(c->inv)) {
					only.push_back({ c->holderRefID, c->inv.item });
				}
			}
			const auto n = cf::FixCharges(*_save, static_cast<std::uint16_t>(_chargeFixValue), only);
			markEdited(Fmt("Gave %zu item(s) of %s a max charge of %d", n, a_plugin.c_str(), _chargeFixValue));
		}
		tips::Tip("Give these enchanted items a maximum charge so they can be recharged with soul gems again (their max charge was lost, often after a mod was removed).");
		ImGui::TextDisabled("Only the edited save in memory changes; write it with File > Save As.");
	}
}
