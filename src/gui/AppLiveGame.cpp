// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Work on the running game: no save needs to be open. Through ReSaverHelper (the optional SKSE plugin, version
// 12) in the game: bring every follower to the player, switch the player's controls back on, save the game, or
// all three at once - the same as holding Ctrl in the game for 8 seconds (that one works without ReSaver Plus).
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <cctype>
#include <cstdlib>

namespace
{
	constexpr int kRescueHelper = 12;

	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}  // the helper version with GATHER / CONTROLS / SAVEGAME / RESCUE

	void Colored(std::uint32_t a_col, const std::string& a_text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, a_col);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(a_text.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}
}

void App::liveListFollowers()
{
	if (_liveGameBusy) {
		return;
	}
	_liveGameBusy = true;
	_liveGameResult = "Looking for your followers...";
	_liveGameResultOk = true;
	_live.request({ "GETOPTION", "sGatherSkip" }, [this](const LiveLink::Reply& r) { _gatherSkipRaw = LiveLink::Ok(r) ? LiveLink::Field(r, 0) : ""; });
	_live.request({ "FOLLOWERS" }, [this](const LiveLink::Reply& r) {
		_liveGameBusy = false;
		if (!LiveLink::Ok(r)) {
			// a helper before 12.2 has no list: gather the way it does
			liveGameAction("GATHER", "Bringing your followers");
			return;
		}
		_liveFollowers.clear();
		const auto count = static_cast<std::size_t>(std::atoi(LiveLink::Field(r, 0).c_str()));
		for (std::size_t i = 0; i < count; ++i) {
			const auto rec = LiveLink::Field(r, i + 1);
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
			LiveFollower f{ p[0], p[1], p[2], p[3], p[4] };
			f.wasSkipped = p[5] == "1";
			f.bring = !f.wasSkipped;
			_liveFollowers.push_back(std::move(f));
		}
		_liveFollowersShown = true;
		_liveGameResult = _liveFollowers.empty() ? "No followers found." : "Tick the ones to bring, then press Bring.";
	});
}

void App::liveGameAction(const std::string& a_command, const std::string& a_what, const std::string& a_ids)
{
	if (_liveGameBusy) {
		return;
	}
	_liveGameBusy = true;
	_liveGameResult = a_what + "...";
	_liveGameResultOk = true;
	std::vector<std::string> cmd{ a_command, liveNotifyMode() };
	if (!a_ids.empty()) {
		cmd.push_back(a_ids);
	}
	_live.request(cmd, [this, a_command](const LiveLink::Reply& r) {
		_liveGameBusy = false;
		_liveGameResultOk = LiveLink::Ok(r);
		if (!_liveGameResultOk) {
			_liveGameResult = "The game said: " + LiveLink::Error(r);
		} else if (a_command == "GATHER") {
			const auto n = std::atoi(LiveLink::Field(r, 0).c_str());
			_liveGameResult = n == 0 ? std::string("No followers found to bring (nobody is following you right now).")
									 : std::to_string(n) + " follower(s) brought next to you: " + LiveLink::Field(r, 1);
		} else if (a_command == "CONTROLS") {
			_liveGameResult = "Your controls are switched back on.";
		} else if (a_command == "SAVEGAME") {
			_liveGameResult = "The game was saved as " + LiveLink::Field(r, 0) + " (a new save: nothing was overwritten).";
		} else {
			_liveGameResult = LiveLink::Field(r, 0);
		}
		_status = _liveGameResult;
	});
}

void App::drawLiveGame()
{
	ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
	const auto* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Appearing,
		ImVec2(0.5f, 0.5f));
	if (!ImGui::Begin("Work on the running game", &_showLiveGame, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	ImGui::PushTextWrapPos(600);
	ImGui::TextDisabled("No save needs to be open: these act in the game while it runs, through ReSaverHelper (the optional SKSE plugin "
						"on the ReSaver Plus page).");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();

	const auto info = _live.info();
	const bool ready = info.connected && info.inGame && helperAtLeast(kRescueHelper);
	if (!_liveEnabled) {
		Colored(theme::Caution(), "The link to the running game is switched off in Options > Settings.");
		if (ImGui::Button("Switch it on")) {
			_liveEnabled = true;
			saveSettings();
		}
	} else if (!info.connected) {
		Colored(theme::Caution(), "Not connected to the game. Start Skyrim (with SKSE) with ReSaverHelper.dll installed in Data\\SKSE\\Plugins "
								  "(or as a mod in Mod Organizer / Vortex); this window connects by itself once the game runs.");
	} else if (!helperAtLeast(kRescueHelper)) {
		Colored(theme::Caution(), "Connected, but the game has ReSaverHelper version " + info.helperVersion +
									  ". These need version 12 or newer: update ReSaverHelper from the ReSaver Plus page" +
									  (info.runtime == "LE" ? " (the Legendary Edition one: ReSaverHelper LE)." : "."));
	} else if (!info.inGame) {
		Colored(theme::Caution(), "Connected to the game (" + info.runtime + " " + info.gameVersion + "), but no save is loaded in it yet: load "
								  "your save in the game first.");
	} else {
		Colored(theme::Good(), "Connected to the running game (" + info.runtime + " " + info.gameVersion + ", ReSaverHelper " + info.helperVersion + ").");
	}
	ImGui::Spacing();

	ImGui::BeginDisabled(!ready || _liveGameBusy);
	const ImVec2 big(296, 0);
	if (ImGui::Button("Gather followers", big)) {
		liveListFollowers();
	}
	tips::Tip("Every follower you have (vanilla, DLC, and follower mods: anyone following you or set as your teammate) is brought next "
			  "to you, wherever they are: lost, stuck, left in another city or cell. They are placed in a half circle behind you. "
			  "You get the list first: untick anyone who is not with you (a follower you left at home), and they are remembered "
			  "for the hold-Ctrl rescue too (sGatherSkip in ReSaverHelper.ini).");
	ImGui::SameLine();
	if (ImGui::Button("Get my controls back", big)) {
		liveGameAction("CONTROLS", "Switching your controls back on");
	}
	tips::Tip("Switches the player's controls back on (EnablePlayerControls): moving, fighting, menus, activating, looking around, "
			  "sneaking, the journal. For a scene or a mod that left you unable to move.");
	if (ImGui::Button("Save the game now", big)) {
		liveGameAction("SAVEGAME", "Saving the game");
	}
	tips::Tip("Saves the game as a new save, ReSaverPP_Rescue_<date>_<time>: your other saves are not touched.");
	ImGui::SameLine();
	if (ImGui::Button("All three: save, controls, followers", big)) {
		liveGameAction("RESCUE", "Saving, switching your controls on and bringing your followers");
	}
	tips::Tip("Saves the game (a new save), switches your controls back on and brings every follower next to you: the same as holding "
			  "Ctrl in the game. The save is made first, so it is the game as it was before the followers moved.");
	ImGui::EndDisabled();
	if (_liveFollowersShown && ready) {
		ImGui::Spacing();
		ImGui::SeparatorText("Your followers");
		if (_liveFollowers.empty()) {
			ImGui::TextDisabled("Nobody is following you right now.");
		} else if (ImGui::BeginTable("##followers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit,
					   ImVec2(600, 0))) {
			ImGui::TableSetupColumn("Bring");
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthFixed, 140);
			ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			for (std::size_t i = 0; i < _liveFollowers.size(); ++i) {
				auto& f = _liveFollowers[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Checkbox("##bring", &f.bring);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(f.name.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s (%s)", f.key.c_str(), f.id.c_str());
				}
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", f.why.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(f.why == "teammate" ? "Marked as your teammate, but not in the follower faction: some follower mods keep "
															 "this on while the follower waits at home."
															 : "In the game's follower faction or held by a follower quest.");
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(f.where.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::Checkbox("Remember the unticked ones (the hold-Ctrl rescue leaves them alone too)", &_liveFollowersRemember);
		std::size_t ticked = 0;
		for (const auto& f : _liveFollowers) {
			ticked += f.bring ? 1 : 0;
		}
		ImGui::BeginDisabled(_liveGameBusy || _liveFollowers.empty());
		if (ImGui::Button(("Bring " + std::to_string(ticked) + " follower(s)").c_str())) {
			if (_liveFollowersRemember) {
				// sGatherSkip: what it had for characters not in this list, plus the unticked ones
				std::vector<std::string> keep;
				std::string              one;
				auto                     flush = [&] {
                    while (!one.empty() && one.front() == ' ') {
                        one.erase(0, 1);
                    }
                    while (!one.empty() && one.back() == ' ') {
                        one.pop_back();
                    }
                    bool listed = false;
                    for (const auto& f : _liveFollowers) {
                        listed = listed || Lower(one) == Lower(f.key) || Lower(one) == Lower(f.id);
                    }
                    if (!one.empty() && !listed) {
                        keep.push_back(one);
                    }
                    one.clear();
				};
				for (char c : _gatherSkipRaw) {
					if (c == ',' || c == ';') {
						flush();
					} else {
						one += c;
					}
				}
				flush();
				for (const auto& f : _liveFollowers) {
					if (!f.bring) {
						keep.push_back(f.key);
					}
				}
				std::string v;
				for (const auto& k : keep) {
					v += (v.empty() ? "" : ",") + k;
				}
				_gatherSkipRaw = v;
				_live.request({ "SETOPTION", "sGatherSkip", v }, [](const LiveLink::Reply&) {});
			}
			std::string ids;
			for (const auto& f : _liveFollowers) {
				if (f.bring) {
					ids += (ids.empty() ? "" : ",") + f.id;
				}
			}
			_liveFollowersShown = false;
			if (ids.empty()) {
				_liveGameResult = "Nobody ticked: nobody moved.";
			} else {
				liveGameAction("GATHER", "Bringing your followers", ids);
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			_liveFollowersShown = false;
			_liveGameResult.clear();
		}
	}
	if (!_liveGameResult.empty()) {
		ImGui::Spacing();
		Colored(_liveGameBusy ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : _liveGameResultOk ? theme::Good() : theme::Bad(), _liveGameResult);
	}

	// the hold-Ctrl rescue in the game
	ImGui::Spacing();
	ImGui::SeparatorText("Hold Ctrl in the game");
	if (ready && !_rescueOptsAsked) {
		_rescueOptsAsked = true;
		_live.request({ "GETOPTION", "bRescueHotkey" }, [this](const LiveLink::Reply& r) {
			if (LiveLink::Ok(r)) {
				_rescueHotkey = LiveLink::Field(r, 0) == "1" ? 1 : 0;
			}
		});
		_live.request({ "GETOPTION", "iRescueHoldSeconds" }, [this](const LiveLink::Reply& r) {
			if (LiveLink::Ok(r)) {
				_rescueSeconds = std::atoi(LiveLink::Field(r, 0).c_str());
			}
		});
	}
	if (!info.connected) {
		_rescueOptsAsked = false;  // asked again on the next connection
	}
	ImGui::PushTextWrapPos(600);
	ImGui::TextWrapped("Holding Ctrl for %d seconds in the game saves it (a new save), switches your controls back on and brings your "
					   "followers to you, even when ReSaver Plus is not running. It works while Skyrim is the window in front; let go of "
					   "Ctrl to do it again.",
		_rescueSeconds);
	ImGui::PopTextWrapPos();
	ImGui::BeginDisabled(!ready || _rescueHotkey < 0);
	bool on = _rescueHotkey == 1;
	if (ImGui::Checkbox("On", &on)) {
		_rescueHotkey = on ? 1 : 0;
		_live.request({ "SETOPTION", "bRescueHotkey", on ? "1" : "0" }, [](const LiveLink::Reply&) {});
	}
	tips::Tip("Saved in ReSaverHelper.ini (bRescueHotkey) next to the DLL.");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(220);
	ImGui::SliderInt("seconds to hold", &_rescueSeconds, 3, 60);
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		_live.request({ "SETOPTION", "iRescueHoldSeconds", std::to_string(_rescueSeconds) }, [](const LiveLink::Reply&) {});
	}
	tips::Tip("How long Ctrl must be held (iRescueHoldSeconds in ReSaverHelper.ini). Ctrl is also Sneak, which is a press, not a hold, "
			  "so a long hold does not get in the way.");
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::SeparatorText("More");
	if (ImGui::Button("Ask the AI about the running game")) {
		_showAi = true;
	}
	tips::Tip("The AI can also look at and fix quests in the running game (stage, complete, stop, reset, refresh the journal), and read "
			  "your logs, with no save open.");
	ImGui::SameLine();
	if (ImGui::Button("Close")) {
		_showLiveGame = false;
	}
	ImGui::End();
}
