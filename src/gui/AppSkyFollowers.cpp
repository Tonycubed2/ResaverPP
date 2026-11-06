// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim followers, beast forms and quests between stages (core/FollowersSky): the window (Tools > Followers and beast
// form) and the Save check findings. Fixes in the file are only the verified edits FollowersSky offers (the follower
// counts, the teammate flag); everything else goes to the running game through ReSaverHelper, done by the game's own
// scripts: SETSTAGE 000750BA 400 (DismissFollower), FOLLOWERSET (SetFollower, helper 13), TEAMMATE (helper 13),
// GATHER <id> (helper 12.2), SETSTAGE <quest> 100 (ShiftBack), BEASTREVERT (helper 13).
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "core/FollowersSky.h"
#include "imgui.h"

#include <algorithm>
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

	void Colored(std::uint32_t a_col, const std::string& a_text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, a_col);
		ImGui::TextWrapped("%s", a_text.c_str());
		ImGui::PopStyleColor();
	}

	constexpr int kSkyFixHelper = 13;  // FOLLOWERSET, TEAMMATE, BEASTREVERT
}

// what the window and the Save check show, read once per save and edit
struct App::SkyFollowersView
{
	const void*                   save = nullptr;
	std::uint64_t                 gen = UINT64_MAX;
	const void*                   db = nullptr;
	skyf::FollowerState           fol;
	std::vector<skyf::BeastState> beasts;
	std::vector<skyf::StuckStage> stages;
	std::string                   gameNote;  // the last answer from the game
};

void App::skyfRefresh()
{
	if (!_save || fo4()) {
		return;
	}
	if (!_skyf) {
		_skyf = std::make_shared<SkyFollowersView>();
	}
	auto& v = *_skyf;
	if (v.save == _save.get() && v.gen == _editGen && v.db == _db.get()) {
		return;
	}
	v.save = _save.get();
	v.gen = _editGen;
	v.db = _db.get();
	const auto* db = _db.get();
	v.fol = skyf::ReadFollowers(*_save, [db](std::uint32_t ref) -> std::uint32_t {
		const auto* f = db ? db->find(ref) : nullptr;
		return f ? f->base : 0;
	});
	v.beasts = skyf::ReadBeastForms(*_save);
	v.stages = skyf::FindStuckStages(*_save, [db](std::uint32_t q) {
		std::vector<std::pair<std::uint16_t, std::uint8_t>> st;
		if (const auto* d = db ? db->quest(q) : nullptr) {
			for (const auto& s : d->stages) {
				st.emplace_back(s.index, s.flags);
			}
		}
		return skyf::ShutdownOnlyStages(st);
	});
}

// the running game's form id for a form of the save: asked from the game (RESOLVE) for plugin forms, since the game's load
// order can differ from the save's; Skyrim.esm forms and forms made in game are used as they are
void App::skyfInGame(std::uint32_t a_formID, std::function<void(std::uint32_t)> a_then)
{
	const std::uint32_t top = a_formID >> 24;
	std::string         plugin;
	std::uint32_t       local = 0;
	if (top == 0xFE) {
		const std::uint32_t li = (a_formID >> 12) & 0xFFF;
		if (li < _save->plugins.lite.size()) {
			plugin = _save->plugins.lite[li];
			local = a_formID & 0xFFF;
		}
	} else if (top != 0xFF && top != 0 && top < _save->plugins.full.size()) {
		plugin = _save->plugins.full[top];
		local = a_formID & 0xFFFFFF;
	}
	if (plugin.empty()) {
		a_then(a_formID);
		return;
	}
	_live.request({ "RESOLVE", plugin, Fmt("%06X", local) }, [this, a_formID, a_then, plugin](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			if (_skyf) {
				_skyf->gameNote = Fmt(tr("The game does not know %08X (%s): %s"), a_formID, plugin.c_str(), LiveLink::Error(r).c_str());
			}
			return;
		}
		a_then(static_cast<std::uint32_t>(std::strtoul(LiveLink::Field(r, 0).c_str(), nullptr, 16)));
	});
}

// one command to the helper that answers OK 1 / OK 0 <why>
void App::skyfAsk(std::vector<std::string> a_command, const std::string& a_what)
{
	_status = tr("Asking the game: ") + a_what + "...";
	_live.request(std::move(a_command), [this, a_what](const LiveLink::Reply& r) {
		std::string note;
		if (!LiveLink::Ok(r)) {
			note = tr("The game could not do it: ") + LiveLink::Error(r);
		} else if (LiveLink::Field(r, 0) == "0") {
			note = tr("The game refused: ") + LiveLink::Field(r, 1);
		} else if (LiveLink::Field(r, 0) == "?") {
			note = a_what + tr(": the game is still busy with it; look in the game.");
		} else {
			note = a_what + tr(": done in the game. Check it there, then make a new save and open that one here.");
		}
		_status = note;
		if (_skyf) {
			_skyf->gameNote = note;
		}
	});
}

void App::skyfDismissInGame()
{
	skyfAsk({ "SETSTAGE", Fmt("%08X", skyf::kDialogueFollower), std::to_string(skyf::kDismissStage), liveNotifyMode(), "ReSaver++: follower dismissed" },
		tr("Dismiss the follower (DialogueFollower stage 400)"));
}

void App::drawSkyFollowers()
{
	prepareWindow("Followers and beast form", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin(trId("Followers and beast form"), &_showSkyFollowers, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Followers and beast form");
	skyfRefresh();
	if (!_skyf) {
		ImGui::End();
		return;
	}
	auto&       v = *_skyf;
	const auto  info = _live.info();
	const bool  game = info.connected && info.inGame;
	const bool  game12 = game && helperAtLeast(12), game13 = game && helperAtLeast(kSkyFixHelper), game3 = game && helperAtLeast(3);
	auto        name = [&](std::uint32_t id) { return id ? formText(id) : std::string(tr("nobody")); };

	ImGui::TextWrapped("%s", tr("Your followers and werewolf / Vampire Lord form as this save has them, checked against the game's own scripts "
								"(DialogueFollowerScript, PlayerWerewolfChangeScript, DLC1PlayerVampireChangeScript). Fixes marked \"file\" "
								"change the loaded save (write it with File > Save As); fixes marked \"game\" are done in the running game "
								"by ReSaverHelper, with the game's own scripts."));
	if (!game) {
		Colored(theme::Caution(), tr("Not connected to the running game: the \"game\" fixes need Skyrim running with ReSaverHelper and this save loaded."));
	} else if (!helperAtLeast(kSkyFixHelper)) {
		Colored(theme::Caution(), tr("The game has ReSaverHelper ") + info.helperVersion +
									  tr(": dismissing and the werewolf time-out fix work; taking a follower back, the teammate switch and "
										 "the beast form revert need version 13."));
	}
	if (!v.gameNote.empty()) {
		Colored(theme::Dim(), v.gameNote);
	}
	ImGui::Spacing();

	if (ImGui::BeginTabBar("skyftabs")) {
		// ---- followers ----
		if (ImGui::BeginTabItem(trId("Followers"))) {
			const auto& f = v.fol;
			if (!f.error.empty()) {
				Colored(theme::Bad(), f.error);
			}
			const auto* fa = f.inAlias(skyf::kFollowerAlias);
			const auto* an = f.inAlias(skyf::kAnimalAlias);
			ImGui::Text(tr("Vanilla follower quest: %s"), f.questRunning ? tr("running") : f.questInSave ? tr("not running") : tr("not in the save"));
			ImGui::Text(tr("Follower alias: %s, PlayerFollowerCount %s"), name(fa ? fa->formID : 0).c_str(),
				f.hasCount ? Fmt("%g", f.followerCount).c_str() : tr("0 (never changed in game)"));
			ImGui::Text(tr("Animal alias: %s, PlayerAnimalCount %s"), name(an ? an->formID : 0).c_str(),
				f.hasAnimalCount ? Fmt("%g", f.animalCount).c_str() : tr("0 (never changed in game)"));
			ImGui::Spacing();

			// problems first, each with what fixes it
			if (f.findings.empty()) {
				Colored(theme::Good(), tr("Nothing wrong with your followers in this save."));
			}
			for (std::size_t i = 0; i < f.findings.size(); ++i) {
				const auto& p = f.findings[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::Separator();
				ImGui::TextUnformatted(p.actor ? formText(p.actor).c_str() : tr("The vanilla follower quest"));
				ImGui::Indent();
				ImGui::TextWrapped("%s", p.text.c_str());
				using I = skyf::Issue;
				const auto id = p.actor;
				switch (p.issue) {
				case I::kCountWithoutFollower:
				case I::kFollowerWithoutCount:
				case I::kAnimalCountWithoutAnimal:
				case I::kDeadInAlias: {
					const bool animal = p.issue == I::kAnimalCountWithoutAnimal || (p.issue == I::kDeadInAlias && an && an->formID == id);
					if (ImGui::Button(tr("Fix the count (file)"))) {
						std::string done;
						if (auto why = skyf::FixFollowerCount(*_save, animal, &done); !why.empty()) {
							_error = why;
						} else {
							markChanged(done);
						}
					}
					tips::Tip(tr("Sets the count to what the alias holds, as the follower script itself does: 0 when nobody living is in it, 1 "
								 "when somebody is. One value in the save, nothing else changes."));
					break;
				}
				case I::kDismissedInAlias:
				case I::kDisabledInAlias:
				case I::kAliasMismatch:
					ImGui::BeginDisabled(!game3);
					if (ImGui::Button(tr("Dismiss in the game"))) {
						skyfDismissInGame();
					}
					ImGui::EndDisabled();
					tips::Tip(tr("Sets stage 400 of the vanilla follower quest, which runs DismissFollower(): the follower leaves your service the "
								 "normal way (factions, teammate flag, alias and count)."));
					break;
				case I::kFollowerAway:
					ImGui::BeginDisabled(!game12);
					if (ImGui::Button(tr("Bring to me (game)"))) {
						skyfInGame(id, [this](std::uint32_t rid) { skyfAsk({ "GATHER", liveNotifyMode(), Fmt("%08X", rid) }, tr("Bring the follower to you")); });
					}
					ImGui::EndDisabled();
					tips::Tip(tr("ReSaverHelper moves the follower next to you in the running game."));
					ImGui::SameLine();
					if (ImGui::Button(tr("Find NPCs (file)..."))) {
						_showActors = true;
					}
					tips::Tip(tr("Find NPCs can move it next to you in the save itself (Move to me)."));
					break;
				case I::kAliasNotTeammate:
				case I::kDismissedTeammate:
				case I::kStrayTeammate: {
					const bool on = p.issue == I::kAliasNotTeammate;
					if (!on) {
						if (ImGui::Button(tr("Switch the teammate flag off (file)"))) {
							const auto nm = formText(id);
							confirm(tr("Teammate flag"), tr("Switch the teammate flag off on ") + nm +
															 tr("?\n\nThe save gets what SetPlayerTeammate(false) leaves: the flag off (and \"can do favors\" on). The game "
																"counts your teammates again when it loads. Keep a copy of the original save."),
								[this, id, nm] {
									if (auto why = skyf::ClearTeammate(*_save, id); !why.empty()) {
										_error = nm + ": " + why;
									} else {
										markChanged(tr("Teammate flag off: ") + nm);
									}
								});
						}
						tips::Tip(tr("Two bits of this actor's saved data, the ones SetPlayerTeammate(false) changes; checked to read back completely."));
						ImGui::SameLine();
					}
					ImGui::BeginDisabled(!game13);
					if (ImGui::Button(on ? tr("Make it your teammate (game)") : tr("Teammate off (game)"))) {
						skyfInGame(id, [this, on](std::uint32_t rid) {
							skyfAsk({ "TEAMMATE", Fmt("%08X", rid), on ? "1" : "0", liveNotifyMode() }, on ? tr("Teammate on") : tr("Teammate off"));
						});
					}
					ImGui::EndDisabled();
					tips::Tip(tr("Actor.SetPlayerTeammate in the running game (ReSaverHelper 13)."));
					break;
				}
				case I::kFactionWithoutAlias:
					ImGui::BeginDisabled(!game13 || (fa && !fa->dead()));
					if (ImGui::Button(tr("Take back as your follower (game)"))) {
						skyfInGame(id, [this](std::uint32_t rid) { skyfAsk({ "FOLLOWERSET", Fmt("%08X", rid), liveNotifyMode() }, tr("Take the follower back")); });
					}
					ImGui::EndDisabled();
					tips::Tip(fa && !fa->dead() ? tr("Somebody else is your follower now: dismiss them first.")
												: tr("Runs DialogueFollowerScript.SetFollower on it, as when you hire a follower: out of the dismissed faction, your "
													 "teammate, in the Follower alias, count 1 (ReSaverHelper 13)."));
					break;
				}
				ImGui::Unindent();
				ImGui::PopID();
			}
			ImGui::Separator();
			ImGui::Spacing();

			// everybody with a follower sign
			ImGui::Text(tr("%zu actor(s) with a follower sign"), f.actors.size());
			constexpr ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
			                               ImGuiTableFlags_SizingStretchProp;
			if (ImGui::BeginTable("skyfactors", 4, tf, ImVec2(0, ImGui::GetFontSize() * 18))) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn(tr("Actor"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
				ImGui::TableSetupColumn(tr("Why"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
				ImGui::TableSetupColumn(tr("Where the save has it"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14);
				ImGui::TableHeadersRow();
				for (const auto& a : f.actors) {
					ImGui::PushID(static_cast<int>(a.refRaw));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(formText(a.formID).c_str());
					ImGui::TableNextColumn();
					std::string why;
					auto        add = [&](const char* w) { why += (why.empty() ? "" : ", ") + std::string(w); };
					if (a.inFollowerAlias) add(tr("Follower alias"));
					if (a.inAnimalAlias) add(tr("Animal alias"));
					if (a.teammate) add(tr("teammate"));
					if (a.inFaction(skyf::kCurrentFollowerFaction)) add(tr("following (CurrentFollowerFaction)"));
					if (a.inFaction(skyf::kDismissedFollowerFaction)) add(tr("dismissed"));
					if (a.inFaction(skyf::kCurrentHireling)) add(tr("hired"));
					if (a.dead()) add(tr("dead"));
					if (a.disabled) add(tr("disabled"));
					if (!a.aliases.empty()) why += Fmt(tr(", in %zu quest alias(es)"), a.aliases.size());
					ImGui::TextWrapped("%s", why.c_str());
					ImGui::TableNextColumn();
					if (a.hasPosition) {
						ImGui::TextWrapped("%s", formText(a.cell).c_str());
					} else {
						ImGui::TextDisabled("%s", tr("where its plugin places it"));
					}
					ImGui::TableNextColumn();
					if (ImGui::SmallButton(tr("go"))) {
						select(Cat::kChangeForms, a.refRaw);
					}
					tips::Tip(tr("Open this actor's change form."));
					if (!a.dead()) {
						ImGui::SameLine();
						const auto id = a.formID;
						ImGui::BeginDisabled(!game12);
						if (ImGui::SmallButton(tr("bring (game)"))) {
							skyfInGame(id, [this](std::uint32_t rid) { skyfAsk({ "GATHER", liveNotifyMode(), Fmt("%08X", rid) }, tr("Bring the follower to you")); });
						}
						ImGui::EndDisabled();
						tips::Tip(tr("ReSaverHelper moves it next to you in the running game."));
						if (a.inFollowerAlias) {
							ImGui::SameLine();
							ImGui::BeginDisabled(!game3);
							if (ImGui::SmallButton(tr("dismiss (game)"))) {
								skyfDismissInGame();
							}
							ImGui::EndDisabled();
							tips::Tip(tr("DismissFollower() in the running game (stage 400 of the vanilla follower quest)."));
						}
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}

		// ---- beast form ----
		if (ImGui::BeginTabItem(trId("Beast form"))) {
			if (v.beasts.empty()) {
				Colored(theme::Good(), tr("You have never been a werewolf or a Vampire Lord in this save."));
			}
			for (std::size_t i = 0; i < v.beasts.size(); ++i) {
				const auto& b = v.beasts[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::SeparatorText(b.kind == skyf::Beast::kWerewolf ? tr("Werewolf") : tr("Vampire Lord"));
				ImGui::Text(tr("%s: %s"), b.questEdid.c_str(), b.questRunning ? tr("running") : b.questInSave ? tr("not running") : tr("never started"));
				ImGui::Text(tr("Your race: %s"), b.playerRace ? formText(b.playerRace).c_str() : tr("the plugin's (never changed in game)"));
				ImGui::Text(tr("Beast race: %s; race the script turns you back into: %s"), name(b.beastRace).c_str(), name(b.originalRace).c_str());
				for (auto s : b.spells) {
					ImGui::BulletText(tr("still has %s"), formText(s).c_str());
				}
				for (auto s : b.factions) {
					ImGui::BulletText(tr("still in %s"), formText(s).c_str());
				}
				for (auto s : b.items) {
					ImGui::BulletText(tr("still carries %s"), formText(s).c_str());
				}
				using F = skyf::BeastState::Fix;
				if (b.fix == F::kNone) {
					Colored(theme::Good(), tr("Nothing wrong."));
				} else {
					Colored(b.fix == F::kCannot ? theme::Bad() : theme::Caution(), b.problem);
				}
				const auto quest = b.quest;
				const bool wolf = b.kind == skyf::Beast::kWerewolf;
				if (b.fix == F::kSetStage) {
					ImGui::BeginDisabled(!game3);
					if (ImGui::Button(tr("Turn me back (game: stage 100)"))) {
						skyfInGame(quest, [this](std::uint32_t rid) {
							skyfAsk({ "SETSTAGE", Fmt("%08X", rid), std::to_string(skyf::kShiftBackStage), liveNotifyMode(), "ReSaver++: turning you back" },
								tr("Turn you back"));
						});
					}
					ImGui::EndDisabled();
					tips::Tip(tr("Sets stage 100 of the transformation quest in the running game: its script's ShiftBack(), the same as when the "
								 "time runs out or you use Revert Form."));
				} else if (b.fix == F::kGameRevert) {
					ImGui::BeginDisabled(!game13);
					if (ImGui::Button(tr("Turn me back (game)"))) {
						skyfAsk({ "BEASTREVERT", wolf ? "werewolf" : "vampire", liveNotifyMode() }, tr("Turn you back"));
					}
					ImGui::EndDisabled();
					tips::Tip(tr("ReSaverHelper 13 resets the script's \"already doing it\" guards and runs its own ShiftBack(): your race back, the "
								 "claws or Vampire Lord powers, factions and armor taken off, controls, fast travel and the beast form menus back "
								 "to normal. The file is not changed: it is a lot of game state (race, controls, menus) that only the game itself "
								 "can set consistently."));
				}
				ImGui::PopID();
			}
			ImGui::EndTabItem();
		}

		// ---- quests between stages ----
		if (ImGui::BeginTabItem(trId("Quests between stages"))) {
			ImGui::TextWrapped("%s", tr("Quests whose stage script was still running when you saved (it carries on when the game loads), and, with "
										"plugin names loaded, quests still running after their shut-down stage."));
			if (v.stages.empty()) {
				Colored(theme::Good(), tr("None."));
			}
			for (std::size_t i = 0; i < v.stages.size(); ++i) {
				const auto& s = v.stages[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::Separator();
				ImGui::Text(tr("%s, stage %d"), formText(s.quest).c_str(), s.stage);
				ImGui::TextWrapped("%s", s.text.c_str());
				if (ImGui::Button(tr("Show the quest"))) {
					_cat = Cat::kQuests;
					_questSel = s.quest;
				}
				tips::Tip(tr("The Quests page, where the quest can be looked at, set to a stage or stopped in the running game."));
				if (s.shutdown) {
					ImGui::SameLine();
					ImGui::BeginDisabled(!(game && helperAtLeast(11)));
					if (ImGui::Button(tr("Stop the quest in the game"))) {
						const auto name = formText(s.quest);
						skyfInGame(s.quest, [this, name](std::uint32_t rid) { liveStop(rid, name); });
					}
					ImGui::EndDisabled();
					tips::Tip(game && helperAtLeast(11) ? tr("Quest.Stop() in the running game: what the game does when a quest ends (its "
															  "aliases are cleared, its shut-down stages run). This file is not changed.")
														: tr("Needs Skyrim running with ReSaverHelper 11 or newer and this save loaded."));
				}
				ImGui::PopID();
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}

// ---- Save check ----------------------------------------------------------------------------------

void App::skyfCheckItems(std::vector<CheckItem>& a_items)
{
	if (!_save || fo4()) {
		return;
	}
	skyfRefresh();
	if (!_skyf) {
		return;
	}
	const auto& v = *_skyf;
	const auto  info = _live.info();
	const bool  game = info.connected && info.inGame;
	const auto  needGame = [&](int a_version) {
        return game && helperAtLeast(a_version) ? std::string(tr("Done in the running game; this file is not changed. "))
                                                : Fmt(tr("Needs the game running with ReSaverHelper %d or later and this save loaded. "), a_version);
	};
	auto openWindow = [this] {
		CheckOption o;
		o.label = tr("Open Followers and beast form...");
		o.risk = Risk::kSafe;
		o.why = tr("Shows every follower, the beast form state and all fixes.");
		o.act = [this] {
			_showSkyFollowers = true;
			_showCheck = false;
		};
		return o;
	};

	// followers
	if (!v.fol.findings.empty()) {
		CheckItem it;
		using I = skyf::Issue;
		bool countOnly = true, serious = false;
		for (const auto& f : v.fol.findings) {
			countOnly &= f.issue == I::kCountWithoutFollower || f.issue == I::kFollowerWithoutCount || f.issue == I::kAnimalCountWithoutAnimal;
			serious |= f.issue != I::kStrayTeammate;
		}
		it.level = serious ? Risk::kCaution : Risk::kInfo;
		it.title = Fmt(tr("%zu follower problem(s)"), v.fol.findings.size());
		for (const auto& f : v.fol.findings) {
			it.what += (it.what.empty() ? "" : "\n") + std::string("- ") + (f.actor ? formText(f.actor) + ": " : std::string()) + f.text;
		}
		for (const bool animal : { false, true }) {
			const auto want = animal ? I::kAnimalCountWithoutAnimal : I::kCountWithoutFollower;
			if (std::any_of(v.fol.findings.begin(), v.fol.findings.end(), [&](const skyf::Finding& f) {
					return f.issue == want || (!animal && f.issue == I::kFollowerWithoutCount);
				})) {
				CheckOption o;
				o.label = animal ? tr("Fix PlayerAnimalCount (file)") : tr("Fix PlayerFollowerCount (file)");
				o.risk = Risk::kSafe;
				o.why = tr("Sets the count to what the vanilla follower quest's alias holds, the value its own script would leave. One value in "
						   "the save changes. Write the save with File > Save As and load that.");
				o.act = [this, animal] {
					std::string done;
					if (auto why = skyf::FixFollowerCount(*_save, animal, &done); !why.empty()) {
						_error = why;
					} else {
						markChanged(done);
					}
				};
				it.options.push_back(std::move(o));
			}
		}
		if (!countOnly) {
			CheckOption o;
			o.label = tr("Dismiss the vanilla follower in the game");
			o.risk = Risk::kCaution;
			o.why = needGame(3) + tr("Stage 400 of the vanilla follower quest runs DismissFollower(): the follower in the Follower alias leaves your service the "
									 "normal way. Only for the vanilla follower, not one a follower mod keeps.");
			o.enabled = game && helperAtLeast(3) && v.fol.inAlias(skyf::kFollowerAlias);
			o.act = [this] { skyfDismissInGame(); };
			it.options.push_back(std::move(o));
		}
		it.options.push_back(openWindow());
		a_items.push_back(std::move(it));
	}

	// beast form
	for (const auto& b : v.beasts) {
		if (!b.stuck()) {
			continue;
		}
		using F = skyf::BeastState::Fix;
		CheckItem it;
		it.level = b.raceIsBeast ? Risk::kRisky : Risk::kCaution;
		it.title = b.kind == skyf::Beast::kWerewolf ? (b.raceIsBeast ? tr("You are stuck as a werewolf") : tr("Werewolf transformation left things behind"))
		                                            : (b.raceIsBeast ? tr("You are stuck as a Vampire Lord") : tr("Vampire Lord transformation left things behind"));
		it.what = b.problem;
		if (b.fix == F::kSetStage || b.fix == F::kGameRevert) {
			CheckOption o;
			o.label = tr("Turn me back in the game");
			o.risk = Risk::kCaution;
			const bool stage = b.fix == F::kSetStage;
			o.why = needGame(stage ? 3 : kSkyFixHelper) +
					(stage ? tr("Sets stage 100 of the transformation quest: its script's own ShiftBack(), as when the time runs out or you use Revert Form.")
						   : tr("ReSaverHelper resets the script's \"already doing it\" guards and runs its own ShiftBack(). The file is not changed: race, "
								"controls and menus are game state only the game sets consistently. Make a save afterwards."));
			o.enabled = game && helperAtLeast(stage ? 3 : kSkyFixHelper);
			const auto quest = b.quest;
			const bool wolf = b.kind == skyf::Beast::kWerewolf;
			o.act = [this, stage, quest, wolf] {
				if (stage) {
					skyfInGame(quest, [this](std::uint32_t rid) {
						skyfAsk({ "SETSTAGE", Fmt("%08X", rid), std::to_string(skyf::kShiftBackStage), liveNotifyMode(), "ReSaver++: turning you back" },
							tr("Turn you back"));
					});
				} else {
					skyfAsk({ "BEASTREVERT", wolf ? "werewolf" : "vampire", liveNotifyMode() }, tr("Turn you back"));
				}
			};
			it.options.push_back(std::move(o));
		}
		it.options.push_back(openWindow());
		a_items.push_back(std::move(it));
	}

	// quests between stages
	if (!v.stages.empty()) {
		CheckItem it;
		it.level = Risk::kInfo;
		it.title = Fmt(tr("%zu quest(s) were between stages when you saved"), v.stages.size());
		for (const auto& s : v.stages) {
			it.what += (it.what.empty() ? "" : "\n") + Fmt("- %s, stage %d: ", formText(s.quest).c_str(), s.stage) + s.text;
		}
		CheckOption o = openWindow();
		it.options.push_back(std::move(o));
		CheckOption q;
		q.label = tr("Show the Quests page");
		q.risk = Risk::kSafe;
		q.why = tr("Only shows information. A stuck quest can be set to a stage or stopped there, in the running game.");
		const auto first = v.stages.front().quest;
		q.act = [this, first] {
			_cat = Cat::kQuests;
			_questSel = first;
			_showCheck = false;
		};
		it.options.push_back(std::move(q));
		a_items.push_back(std::move(it));
	}
}
