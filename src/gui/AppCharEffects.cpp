// ReSaver++ - Character effects (Tools > Character effects, Edit player): a paper doll of what the character wears with the
// number of effects each item gives, the save's own screenshot, the effects grouped by where they come from (cards), and
// the stuck ones at the top (an item's enchantment still running while nothing worn carries it, a mod gone from the load
// order). Skyrim and Fallout 4; the reading and the rules are in core/CharEffects.h, removal goes through the game's
// verified removal code (cf::RemovePlayerActiveEffects / fo4cf::RemoveActiveEffects).
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "core/CharEffects.h"
#include "core/LoadOrder.h"
#include "core/PluginDB.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace ess;

namespace
{
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
	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	const char* GroupText(chfx::Group a_g, bool a_fo4)
	{
		switch (a_g) {
		case chfx::Group::kWorn: return tr("Worn items");
		case chfx::Group::kNotWorn: return tr("Items not worn");
		case chfx::Group::kSpells: return tr("Spells, powers and shouts");
		case chfx::Group::kConsumables: return a_fo4 ? tr("Chems, food and drink") : tr("Potions, food and ingredients");
		case chfx::Group::kAbilities: return a_fo4 ? tr("Abilities (race, perks, mods)") : tr("Abilities (race, standing stone, perks, mods)");
		case chfx::Group::kDiseases: return tr("Diseases and addictions");
		case chfx::Group::kOthers: return tr("Put on you by others");
		case chfx::Group::kUnknown: return tr("Unknown (plugin missing or not read)");
		default: return "";
		}
	}
	const char* GroupNote(chfx::Group a_g)
	{
		switch (a_g) {
		case chfx::Group::kWorn: return tr("Enchantments of the things you wear. They come back when you put the item on again, so they are not stuck.");
		case chfx::Group::kNotWorn: return tr("Enchantments of items you do not wear. Those marked at the top as stuck are the ones to remove.");
		case chfx::Group::kSpells: return tr("Spells, powers, shouts and scrolls you cast on yourself.");
		case chfx::Group::kConsumables: return tr("What you drank or ate. These usually run out by themselves.");
		case chfx::Group::kAbilities: return tr("Constant effects of your race, standing stone, perks and mods. A mod gives them again if they are removed while it is installed.");
		case chfx::Group::kDiseases: return tr("Diseases and addictions.");
		case chfx::Group::kOthers: return tr("Effects someone else put on you: hits, spells, enchanted weapons. They run out by themselves.");
		case chfx::Group::kUnknown: return tr("The plugin of these is not found or plugin names are not loaded, so what they are is not known. The form ID and plugin are shown.");
		default: return "";
		}
	}
	const char* SlotText(chfx::Slot a_s)
	{
		switch (a_s) {
		case chfx::Slot::kHead: return tr("Head");
		case chfx::Slot::kBody: return tr("Body");
		case chfx::Slot::kHands: return tr("Hands");
		case chfx::Slot::kFeet: return tr("Feet");
		case chfx::Slot::kAmulet: return tr("Amulet");
		case chfx::Slot::kRing: return tr("Ring");
		case chfx::Slot::kShield: return tr("Shield");
		case chfx::Slot::kRightHand: return tr("Right hand");
		case chfx::Slot::kLeftHand: return tr("Left hand");
		case chfx::Slot::kAmmo: return tr("Ammo");
		case chfx::Slot::kChest: return tr("Chest armor");
		case chfx::Slot::kLeftArm: return tr("Left arm");
		case chfx::Slot::kRightArm: return tr("Right arm");
		case chfx::Slot::kLeftLeg: return tr("Left leg");
		case chfx::Slot::kRightLeg: return tr("Right leg");
		case chfx::Slot::kNeck: return tr("Neck");
		case chfx::Slot::kPipBoy: return tr("Pip-Boy");
		case chfx::Slot::kWeapons: return tr("Weapons");
		case chfx::Slot::kOther: return tr("Other slots");
		case chfx::Slot::kUnknown: return tr("Slot not known");
		default: return "";
		}
	}

	// where each slot's box sits around the figure (box x: 0 left column, 1 right column; y in canvas units) and the point of
	// the figure it points at; the figure faces you, so the character's right hand is on your left
	struct Place
	{
		chfx::Slot slot;
		int        column;
		float      y;
		float      ax, ay;
	};
	const std::vector<Place>& Places(bool a_fo4)
	{
		static const std::vector<Place> sky = {
			{ chfx::Slot::kHead, 0, 10, 210, 52 },
			{ chfx::Slot::kBody, 0, 95, 210, 150 },
			{ chfx::Slot::kRightHand, 0, 180, 150, 238 },
			{ chfx::Slot::kFeet, 0, 265, 190, 388 },
			{ chfx::Slot::kAmmo, 0, 350, 200, 215 },
			{ chfx::Slot::kAmulet, 1, 10, 210, 96 },
			{ chfx::Slot::kHands, 1, 95, 268, 232 },
			{ chfx::Slot::kRing, 1, 180, 274, 244 },
			{ chfx::Slot::kLeftHand, 1, 265, 270, 240 },
			{ chfx::Slot::kShield, 1, 350, 262, 205 },
		};
		static const std::vector<Place> f4 = {
			{ chfx::Slot::kHead, 0, 10, 210, 52 },
			{ chfx::Slot::kBody, 0, 80, 205, 175 },
			{ chfx::Slot::kChest, 0, 150, 215, 130 },
			{ chfx::Slot::kRightArm, 0, 220, 160, 175 },
			{ chfx::Slot::kRightLeg, 0, 290, 192, 320 },
			{ chfx::Slot::kWeapons, 0, 360, 150, 238 },
			{ chfx::Slot::kNeck, 1, 10, 210, 92 },
			{ chfx::Slot::kPipBoy, 1, 80, 262, 215 },
			{ chfx::Slot::kLeftArm, 1, 150, 258, 165 },
			{ chfx::Slot::kHands, 1, 220, 270, 240 },
			{ chfx::Slot::kRing, 1, 290, 274, 248 },
			{ chfx::Slot::kLeftLeg, 1, 360, 228, 320 },
		};
		return a_fo4 ? f4 : sky;
	}
}

struct App::CharFxView
{
	const void*      save = nullptr;
	std::uint64_t    gen = UINT64_MAX;
	const void*      db = nullptr;
	chfx::View       view;
	loadorder::Active lo;
	int              slot = -1;    // the selected doll place (chfx::Slot), -1 none
	std::uint32_t    item = 0;     // the selected worn item (0: the whole slot)
	bool             onlySel = true;
	char             filter[128] = {};
};

App::CharFxView& App::charFx()
{
	if (!_cfx) {
		_cfx = std::make_shared<CharFxView>();
	}
	auto& c = *_cfx;
	if (c.save != _save.get() || c.gen != _editGen || c.db != _db.get()) {
		if (c.save != _save.get()) {
			c.slot = -1;
			c.item = 0;
		}
		c.save = _save.get();
		c.gen = _editGen;
		c.db = _db.get();
		c.view = {};
		if (_save) {
			c.lo = currentLoadOrder();
			chfx::Options opt;
			opt.db = _db.get();
			opt.loadOrder = c.lo.known ? &c.lo.plugins : nullptr;
			c.view = chfx::Build(*_save, opt);
		}
	}
	return c;
}

void App::charFxRemove(const std::vector<std::size_t>& a_effects, const std::string& a_what)
{
	auto&       c = charFx();
	std::string refused;
	std::vector<std::size_t> ok;
	for (const auto i : a_effects) {
		if (i >= c.view.effects.size()) {
			continue;
		}
		const auto& e = c.view.effects[i];
		if (e.canRemove && c.view.removable) {
			ok.push_back(i);
		} else if (refused.size() < 600) {
			refused += "\n  " + c.view.name(e.item) + Fmt(tr(" (effect %u): "), e.effectIndex) + e.why;
		}
	}
	if (ok.empty()) {
		_error = tr("None of these effects can be removed:") + refused;
		return;
	}
	std::string stats;
	for (const auto i : ok) {
		if (c.view.effects[i].stat && !c.view.effects[i].why.empty() && stats.size() < 600) {
			stats += "\n  " + c.view.effects[i].why;
		}
	}
	std::string text = Fmt(tr("Remove %zu effect(s) from the player: %s?"), ok.size(), a_what.c_str());
	if (!stats.empty()) {
		text += tr("\nStats put back:") + stats;
	}
	if (!refused.empty()) {
		text += tr("\nThese stay (they cannot be removed):") + refused;
	}
	confirm(tr("Remove active effects"), text, [this, ok, a_what] {
		auto&                    cv = charFx();
		std::vector<std::string> changes;
		const auto               why = chfx::Remove(*_save, cv.view, ok, &changes);
		if (!why.empty()) {
			_error = tr("Could not remove the effects: ") + why;
			return;
		}
		std::string what = Fmt(tr("Removed %zu effect(s): %s"), ok.size(), a_what.c_str());
		for (std::size_t k = 0; k < changes.size() && k < 6; ++k) {
			what += (k ? "; " : ": ") + changes[k];
		}
		markChanged(what);
		_pvLoaded = false;  // Edit player reads its values again
	});
}

std::string App::charFxFindingText(const chfx::View& a_v, const chfx::Finding& a_f)
{
	const auto& c = a_v.cards[a_f.card];
	if (a_f.kind == chfx::Stuck::kPluginGone) {
		return Fmt(tr("%s (%08X): %zu effect(s) from %s, which is no longer in your load order"), a_v.name(c.item).c_str(), c.item, a_f.effects.size(),
			a_f.plugin == "?" ? tr("a plugin not in the save's own list") : a_f.plugin.c_str());
	}
	std::string where;
	if (!c.source) {
		where = tr("no item is named as its source and no worn item carries this enchantment");
	} else if (a_v.carried.contains(c.source)) {
		where = Fmt(tr("you carry %s but do not wear it, and no worn item carries this enchantment"), a_v.name(c.source).c_str());
	} else {
		where = Fmt(tr("%s is not in your inventory any more, and no worn item carries this enchantment"), a_v.name(c.source).c_str());
	}
	return Fmt(tr("%s (%s, %08X): %zu effect(s) still on you (%zu running): "), a_v.name(c.item).c_str(), c.plugin.c_str(), c.item, a_f.effects.size(),
	           a_f.running) +
	       where;
}

void App::charFxCheck(std::vector<CheckItem>& a_items)
{
	if (!_save) {
		return;
	}
	auto& c = charFx();
	if (!c.view.error.empty() || c.view.stuck.empty()) {
		return;
	}
	std::size_t fx = 0, notWorn = 0;
	std::string list;
	for (const auto& f : c.view.stuck) {
		fx += f.effects.size();
		notWorn += f.kind == chfx::Stuck::kNotWorn;
		if (list.size() < 900) {
			list += "\n  " + charFxFindingText(c.view, f);
		}
	}
	CheckItem it;
	it.level = notWorn ? Risk::kCaution : Risk::kInfo;
	it.title = Fmt(tr("%zu effect(s) stuck on your character (%zu source(s))"), fx, c.view.stuck.size());
	it.what = tr("Effects that keep running although nothing you wear gives them, or whose mod is gone:") + list;
	CheckOption o;
	o.label = tr("Open Character effects");
	o.risk = Risk::kSafe;
	o.why = tr("Shows them with what you wear; nothing is removed until you press Remove there.");
	o.act = [this] {
		_showCheck = false;
		shortcut(Sc::kCharEffects);
	};
	it.options.push_back(std::move(o));
	a_items.push_back(std::move(it));
}

void App::drawCharEffects()
{
	if (!_save) {
		_showCharFx = false;
		return;
	}
	prepareWindow("Character effects", ImVec2(0.85f, 0.9f));
	if (!ImGui::Begin(trId("Character effects"), &_showCharFx, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Character effects");
	auto&       c = charFx();
	const auto& v = c.view;
	static const bool testInit = [&] {  // screenshots: RESAVERPP_TEST_CFXSLOT=<slot number> selects a doll place
		if (const char* s = std::getenv("RESAVERPP_TEST_CFXSLOT")) {
			c.slot = std::atoi(s);
		}
		return true;
	}();
	(void)testInit;

	ImGui::NewLine();
	ImGui::TextWrapped("%s", tr("Every magic effect on your character, grouped by where it comes from, and what you wear. An effect stuck on you "
						   "(for example from a ring you took off, which console commands do not remove) is listed in red at the top with a "
						   "button that removes it. Changes are made in the loaded save; File > Save or Save As writes them."));
	if (!v.error.empty()) {
		TextColored(theme::Warn(), tr("Cannot read the character's effects: ") + v.error);
		ImGui::End();
		return;
	}
	if (!_db) {
		TextColored(theme::Warn(), tr("Plugin names are not loaded: items show as form IDs, slots are not known and effects that change a stat cannot be "
									   "removed (which stat is in the plugins). Load them (Tools > Read plugin names)."));
	} else if (!_db->missing.empty()) {
		ImGui::TextDisabled(tr("%zu of this save's plugins were not found here: their items show as form ID and plugin, and their effects that change a stat cannot be removed."),
			_db->missing.size());
	}
	if (!v.removable) {
		TextColored(theme::Warn(), tr("The player's saved data does not read completely here, so nothing can be removed."));
	}

	// ---- stuck ----
	if (!v.stuck.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::SeparatorText(Fmt(tr("Stuck on your character (%zu)"), v.stuck.size()).c_str());
		ImGui::PopStyleColor();
		for (std::size_t k = 0; k < v.stuck.size(); ++k) {
			const auto& f = v.stuck[k];
			ImGui::PushID(static_cast<int>(k) + 500);
			std::size_t can = 0;
			for (const auto i : f.effects) {
				can += v.effects[i].canRemove && v.removable;
			}
			ImGui::BeginDisabled(can == 0);
			const std::string label = can == f.effects.size() ? std::string(tr("Remove them"))
			                          : can == 0              ? std::string(tr("Cannot be removed here"))
			                                                  : Fmt(tr("Remove the %zu that can be"), can);
			if (ImGui::SmallButton(label.c_str())) {
				charFxRemove(f.effects, v.name(v.cards[f.card].item));
			}
			ImGui::EndDisabled();
			if (can < f.effects.size()) {
				std::string why;
				for (const auto i : f.effects) {
					if (!v.effects[i].canRemove) {
						why = v.effects[i].why;
						break;
					}
				}
				tips::Tip((Fmt(tr("%zu of these cannot be removed: "), f.effects.size() - can) + why).c_str());
			} else {
				tips::Tip(tr("Take these effects off your character (asks first); a stat they change is put back as when the effect ends."));
			}
			ImGui::SameLine();
			TextColored(theme::Bad(), charFxFindingText(v, f));
			ImGui::PopID();
		}
	} else {
		TextColored(theme::Good(), tr("Nothing stuck found: every item enchantment on you comes from something you wear."));
	}
	if (!v.loadOrderChecked) {
		ImGui::TextDisabled("%s", (std::string(tr("Gone plugins not checked: ")) + (v.loadOrderOther
																			   ? tr("your load order lacks most of this save's plugins (the save comes from another setup)")
																			   : tr("your load order is not known (Options > Settings)")))
									  .c_str());
	}
	ImGui::Separator();

	// ---- left: doll + screenshot + worn list; right: the cards ----
	const float leftW = 430.0f + (_shotTex ? 260.0f : 0.0f);
	ImGui::BeginChild("##cfxleft", ImVec2(std::min(leftW, ImGui::GetContentRegionAvail().x * 0.55f), 0), ImGuiChildFlags_ResizeX);
	{
		const auto& places = Places(v.fo4);
		const float W = 420.0f, H = 420.0f, boxW = 130.0f, boxH = 58.0f;
		const ImVec2 o = ImGui::GetCursorScreenPos();
		auto* dl = ImGui::GetWindowDrawList();
		const auto lineCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		const auto bodyCol = ImGui::GetColorU32(ImGuiCol_FrameBgHovered);
		auto P = [&](float x, float y) { return ImVec2(o.x + x, o.y + y); };
		// a plain figure: head, neck, torso, arms, legs
		dl->AddCircleFilled(P(210, 48), 24, bodyCol, 32);
		dl->AddRectFilled(P(203, 70), P(217, 92), bodyCol);
		dl->AddRectFilled(P(176, 90), P(244, 210), bodyCol, 14.0f);
		auto limb = [&](float x1, float y1, float x2, float y2, float w) {
			dl->AddLine(P(x1, y1), P(x2, y2), bodyCol, w);
			dl->AddCircleFilled(P(x1, y1), w * 0.5f, bodyCol);
			dl->AddCircleFilled(P(x2, y2), w * 0.5f, bodyCol);
		};
		limb(182, 100, 155, 172, 16);
		limb(155, 172, 150, 236, 13);
		limb(238, 100, 265, 172, 16);
		limb(265, 172, 270, 236, 13);
		limb(196, 205, 192, 300, 20);
		limb(192, 300, 190, 385, 16);
		limb(224, 205, 228, 300, 20);
		limb(228, 300, 230, 385, 16);
		dl->AddRectFilled(P(178, 382), P(202, 394), bodyCol, 4.0f);
		dl->AddRectFilled(P(218, 382), P(242, 394), bodyCol, 4.0f);

		std::set<std::uint32_t> stuckSources;
		for (const auto& f : v.stuck) {
			stuckSources.insert(v.cards[f.card].source);
		}
		for (const auto& pl : places) {
			const float bx = pl.column == 0 ? 0.0f : W - boxW, by = pl.y;
			std::vector<const chfx::Worn*> items;
			std::size_t fx = 0;
			for (const auto& w : v.worn) {
				if (w.slot == pl.slot) {
					items.push_back(&w);
					fx += w.effects;
				}
			}
			const bool sel = c.slot == static_cast<int>(pl.slot);
			const ImVec2 a = P(bx, by), b = P(bx + boxW, by + boxH);
			const ImVec2 edge = pl.column == 0 ? P(bx + boxW, by + boxH * 0.5f) : P(bx, by + boxH * 0.5f);
			dl->AddLine(edge, P(pl.ax, pl.ay), items.empty() ? lineCol : ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
			dl->AddCircleFilled(P(pl.ax, pl.ay), 3.0f, items.empty() ? lineCol : ImGui::GetColorU32(ImGuiCol_Text));
			ImGui::SetCursorScreenPos(a);
			ImGui::PushID(static_cast<int>(pl.slot) + 800);
			if (ImGui::InvisibleButton("##slot", ImVec2(boxW, boxH))) {
				c.slot = sel ? -1 : static_cast<int>(pl.slot);
				c.item = 0;
			}
			const bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			dl->AddRectFilled(a, b, ImGui::GetColorU32(sel ? ImGuiCol_HeaderActive : hov ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg), 5.0f);
			dl->AddRect(a, b, fx ? theme::Good() : ImGui::GetColorU32(ImGuiCol_Border), 5.0f, 0, fx ? 2.0f : 1.0f);
			dl->PushClipRect(a, b, true);
			dl->AddText(ImVec2(a.x + 6, a.y + 3), ImGui::GetColorU32(ImGuiCol_TextDisabled), SlotText(pl.slot));
			std::string line1 = items.empty() ? tr("(nothing)") : v.name(items[0]->item);
			if (items.size() > 1) {
				line1 += Fmt(tr(" +%zu more"), items.size() - 1);
			}
			dl->AddText(ImVec2(a.x + 6, a.y + 20), ImGui::GetColorU32(items.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text), line1.c_str());
			if (!items.empty()) {
				dl->AddText(ImVec2(a.x + 6, a.y + 37), fx ? theme::Good() : ImGui::GetColorU32(ImGuiCol_TextDisabled),
					Fmt(tr("%zu effect(s)"), fx).c_str());
			}
			dl->PopClipRect();
			if (hov) {
				std::string t = SlotText(pl.slot);
				for (const auto* w : items) {
					t += "\n" + v.name(w->item) + Fmt(" (%s, %08X): ", _save->pluginOf(w->item).c_str(), w->item) + Fmt(tr("%zu effect(s)"), w->effects);
				}
				ImGui::SetTooltip("%s", (t + "\n" + tr("Click to show only the effects of this slot.")).c_str());
			}
		}
		// the save's own screenshot next to the doll
		if (_shotTex) {
			const float sw = 250.0f, sh = sw * static_cast<float>(_shotH) / static_cast<float>(std::max(1, _shotW));
			ImGui::SetCursorScreenPos(P(W + 10, 10));
			ImGui::Image(static_cast<ImTextureID>(_shotTex), ImVec2(sw, sh));
			tips::Tip(tr("The picture the game took when it made this save."));
			ImGui::SetCursorScreenPos(P(W + 10, 18 + sh));
			ImGui::PushTextWrapPos(o.x + W + 10 + sw);
			ImGui::TextDisabled("%s", Fmt(tr("%s, level %u, %s"), _save->header.playerName.c_str(), _save->header.playerLevel, _save->header.location.c_str()).c_str());
			ImGui::PopTextWrapPos();
		}
		// worn items the doll has no place for: their slots are not known (plugin not read) or not one of the doll's
		ImGui::SetCursorScreenPos(P(0, H + 4));
		for (const auto s : { chfx::Slot::kUnknown, chfx::Slot::kOther }) {
			std::size_t n = 0, fx = 0;
			for (const auto& w : v.worn) {
				if (w.slot == s) {
					++n;
					fx += w.effects;
				}
			}
			if (!n) {
				continue;
			}
			const bool sel = c.slot == static_cast<int>(s);
			ImGui::PushID(static_cast<int>(s) + 850);
			if (ImGui::Selectable(Fmt(tr("%s: %zu worn item(s), %zu effect(s)"), SlotText(s), n, fx).c_str(), sel, 0, ImVec2(W, 0))) {
				c.slot = sel ? -1 : static_cast<int>(s);
				c.item = 0;
			}
			ImGui::PopID();
			tips::Tip(s == chfx::Slot::kUnknown ? tr("Worn items whose plugin was not read here, so which slot they take is not known. Click to show their effects.")
												: tr("Worn items in slots the doll has no place for (tail, mod slots). Click to show their effects."));
		}
		ImGui::Dummy(ImVec2(W, 0));
		if (v.fo4) {
			ImGui::TextDisabled("%s", tr("Power armor is not shown: its pieces are in the frame's inventory, not the character's."));
		}
		// every worn item, including the ones the doll has no place for
		ImGui::SeparatorText(Fmt(tr("Worn (%zu)"), v.worn.size()).c_str());
		if (ImGui::BeginTable("##worn", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn(tr("Slot"));
			ImGui::TableSetupColumn(tr("Item"));
			ImGui::TableSetupColumn(tr("Effects"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();
			for (const auto& w : v.worn) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(static_cast<int>(w.item ^ (w.left ? 0x80000000u : 0)));
				const bool sel = c.item == w.item;
				if (ImGui::Selectable(SlotText(w.slot), sel, ImGuiSelectableFlags_SpanAllColumns)) {
					c.item = sel ? 0 : w.item;
					c.slot = sel ? -1 : static_cast<int>(w.slot);
				}
				ImGui::PopID();
				tips::Tip(w.type.empty() ? tr("Its plugin was not read, so its slot is not known.")
										 : Fmt(tr("Record %s, biped slots %08X"), w.type.c_str(), w.biped).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted((v.name(w.item) + Fmt("  (%s, %08X)", _save->pluginOf(w.item).c_str(), w.item) + (w.left ? tr(", left hand") : "")).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%zu", w.effects);
			}
			ImGui::EndTable();
		}
		if (v.worn.empty()) {
			ImGui::TextDisabled("%s", tr("The save has nothing worn for this character."));
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ---- the cards ----
	ImGui::BeginChild("##cfxcards", ImVec2(0, 0));
	{
		ImGui::SetNextItemWidth(260);
		ImGui::InputTextWithHint("##cfxf", tr("filter (name, plugin, form ID)"), c.filter, sizeof(c.filter));
		tips::Tip(tr("Show only sources whose name, effect, plugin or form ID contains this text."));
		const bool hasSel = c.slot >= 0;
		if (hasSel) {
			ImGui::SameLine();
			ImGui::Checkbox(Fmt(tr("Only %s"), c.item ? v.name(c.item).c_str() : SlotText(static_cast<chfx::Slot>(c.slot))).c_str(), &c.onlySel);
			tips::Tip(tr("Show only the effects of what is selected on the doll. Click it again on the doll to show everything."));
		}
		std::set<std::uint32_t> selItems, selEnch;
		for (const auto& w : v.worn) {
			if (hasSel && (c.item ? w.item == c.item : static_cast<int>(w.slot) == c.slot)) {
				selItems.insert(w.item);
				if (w.enchantment) selEnch.insert(w.enchantment);
				if (w.baseEnchantment) selEnch.insert(w.baseEnchantment);
			}
		}
		const auto filt = Lower(c.filter);
		ImGui::TextDisabled(tr("%zu effects from %zu sources."), v.effects.size(), v.cards.size());
		for (int g = 0; g < static_cast<int>(chfx::Group::kCount); ++g) {
			const auto grp = static_cast<chfx::Group>(g);
			std::vector<std::size_t> rows;
			std::size_t nfx = 0;
			for (std::size_t ci = 0; ci < v.cards.size(); ++ci) {
				const auto& cd = v.cards[ci];
				if (cd.group != grp) {
					continue;
				}
				if (hasSel && c.onlySel && !(selItems.contains(cd.source) || (!cd.sourceWorn && selEnch.contains(cd.item)))) {
					continue;
				}
				if (!filt.empty()) {
					std::string t = v.name(cd.item) + " " + cd.plugin + Fmt(" %08X ", cd.item) + (cd.source ? v.name(cd.source) + Fmt(" %08X", cd.source) : "");
					for (const auto i : cd.effects) {
						if (v.effects[i].mgef) {
							t += " " + v.name(v.effects[i].mgef);
						}
					}
					if (Lower(t).find(filt) == std::string::npos) {
						continue;
					}
				}
				rows.push_back(ci);
				nfx += cd.effects.size();
			}
			if (rows.empty()) {
				continue;
			}
			ImGui::PushID(g + 900);
			if (grp == chfx::Group::kWorn || grp == chfx::Group::kNotWorn || (hasSel && c.onlySel)) {
				ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
			}
			const bool open = ImGui::CollapsingHeader(Fmt(tr("%s: %zu source(s), %zu effect(s)"), GroupText(grp, v.fo4), rows.size(), nfx).c_str());
			tips::Tip(GroupNote(grp));
			if (open) {
				ImGui::TextDisabled("%s", GroupNote(grp));
				std::size_t shown = 0;
				for (const auto ci : rows) {
					if (++shown > 200) {
						ImGui::TextDisabled(tr("... and %zu more sources (use the filter)"), rows.size() - 200);
						break;
					}
					const auto& cd = v.cards[ci];
					ImGui::PushID(static_cast<int>(ci));
					bool stuck = false;
					for (const auto& f : v.stuck) {
						stuck = stuck || f.card == ci;
					}
					if (stuck) {
						ImGui::PushStyleColor(ImGuiCol_Border, theme::Bad());
					}
					ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
					if (stuck) {
						ImGui::PopStyleColor();
					}
					const auto title = v.name(cd.item);
					ImGui::TextUnformatted(title.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("%s", (Fmt("%08X  ", cd.item) + cd.plugin + (cd.type.empty() ? "" : "  [" + cd.type + "]")).c_str());
					if (cd.source) {
						ImGui::TextDisabled("%s", (tr("on ") + v.name(cd.source) + Fmt(" (%s, %08X)", _save->pluginOf(cd.source).c_str(), cd.source) +
														(cd.sourceWorn ? tr(": worn") : tr(": not worn")))
													  .c_str());
					}
					if (stuck) {
						TextColored(theme::Bad(), tr("Stuck: see the top of the window."));
					}
					ImGui::BeginDisabled(!cd.removable);
					if (ImGui::SmallButton(Fmt(tr("Remove all %zu from this source"), cd.effects.size()).c_str())) {
						charFxRemove(cd.effects, title);
					}
					ImGui::EndDisabled();
					tips::Tip(cd.removable ? tr("Take every effect of this source off your character (asks first).")
										   : tr("One of these effects cannot be removed (see its Remove button)."));
					if (ImGui::BeginTable("##fx", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
						ImGui::TableSetupColumn(tr("Effect"));
						ImGui::TableSetupColumn(tr("Strength"), ImGuiTableColumnFlags_WidthFixed);
						ImGui::TableSetupColumn(tr("Time"));
						ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthFixed);
						ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
						for (const auto i : cd.effects) {
							const auto& e = v.effects[i];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted((e.mgef ? v.name(e.mgef) : Fmt(tr("effect %u"), e.effectIndex)).c_str());
							if (e.stat && !e.why.empty()) {
								tips::Tip(e.canRemove ? (tr("Removing it: ") + e.why).c_str() : e.why.c_str());
							}
							ImGui::TableNextColumn();
							ImGui::Text("%g", e.magnitude);
							ImGui::TableNextColumn();
							if (e.duration == 0) {
								ImGui::TextUnformatted(tr("constant"));
							} else if (!v.fo4 && (e.flags & 0x4100u) == 0) {
								ImGui::TextUnformatted(Fmt(tr("%.0f of %.0f s (%.0f s left)"), e.elapsed, e.duration, std::max(0.0f, e.duration - e.elapsed)).c_str());
							} else {
								ImGui::TextUnformatted(Fmt(tr("%.0f of %.0f s"), e.elapsed, e.duration).c_str());
							}
							ImGui::TableNextColumn();
							if (e.off()) {
								ImGui::TextDisabled("%s", tr("switched off"));
							} else {
								ImGui::TextUnformatted(tr("running"));
							}
							ImGui::TableNextColumn();
							ImGui::PushID(static_cast<int>(i));
							ImGui::BeginDisabled(!e.canRemove || !v.removable);
							if (ImGui::SmallButton(tr("Remove"))) {
								charFxRemove({ i }, title);
							}
							ImGui::EndDisabled();
							tips::Tip(e.canRemove ? tr("Take this effect off your character (asks first).") : (tr("Cannot be removed: ") + e.why).c_str());
							ImGui::PopID();
						}
						ImGui::EndTable();
					}
					ImGui::EndChild();
					ImGui::PopID();
				}
			}
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ImGui::End();
}
