#include "PCH.h"
#include "MenuPage.h"

#include "Game.h"
#include "GhostRemove.h"
#include "LiveInfo.h"
#include "MenuGuard.h"
#include "Protocol.h"
#include "Rescue.h"
#include "Server.h"
#include "SaveDialog.h"
#include "Settings.h"
#include "SkyFix.h"

#include "SKSEMenuFramework.h"

#include <shellapi.h>

#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>

// Everything below runs on the game's main thread: SKSE Menu Framework calls the render functions from its hook in the
// game's main loop (SkyrimSE.exe 1.6.1170: function 0x645ea0 calls 0x643c00 at 0x646937, the call the framework hooks,
// RELOCATION_ID(35556, 36555) + 0x371), and SKSE runs its tasks from the same function (0x646a3c). The render functions
// only read the copies in g and queue SKSE tasks; the tasks do the game reads and actions (LiveInfo, Rescue, SkyFix, Game).
namespace MenuPage
{
	namespace
	{
		namespace im = ImGuiMCP;
		using clock = std::chrono::steady_clock;

		// ------------------------------------------------------------------ state shared by the pages and their tasks
		struct State
		{
			std::mutex lock;
			std::string status;  // the last thing a button did (or is doing)

			// shared, so a frame takes the latest copy without copying thousands of strings
			std::shared_ptr<const LiveInfo::Effects>               effects = std::make_shared<LiveInfo::Effects>();
			std::shared_ptr<const LiveInfo::Followers>             followers = std::make_shared<LiveInfo::Followers>();
			std::shared_ptr<const std::vector<LiveInfo::QuestRow>> quests = std::make_shared<std::vector<LiveInfo::QuestRow>>();
			LiveInfo::QuestDetail quest;
			RE::FormID            questSel = 0;

			std::deque<LiveInfo::VMSample> samples;  // the last 60 (3 minutes)
			std::filesystem::path         report;  // where the last report was saved
			std::filesystem::path         sent;    // where the last "What to send" file was saved
			RE::FormID                    spawned = 0;
			std::string                   spawnedName;
			int                           iniStage = -1;
		};
		State g;

		std::atomic<bool> g_effectsBusy{ false }, g_followersBusy{ false }, g_questsBusy{ false }, g_questBusy{ false };
		clock::time_point g_effectsAt{}, g_followersAt{}, g_questsAt{}, g_questAt{}, g_iniAt{};
		std::atomic<bool> g_sampling{ false };
		std::atomic<bool> g_samplerStarted{ false };
		const clock::time_point g_start = clock::now();

		// a confirmation asked by a button; drawn by the page at its top level (OpenPopup and BeginPopupModal need the same ID stack)
		struct Ask
		{
			bool                  open = false;
			std::string           text;
			std::function<void()> yes;
		};
		Ask g_ask;

		constexpr im::ImVec4 kRed{ 1.0f, 0.45f, 0.40f, 1.0f };
		constexpr im::ImVec4 kGreen{ 0.55f, 0.90f, 0.55f, 1.0f };
		constexpr im::ImVec4 kYellow{ 1.0f, 0.85f, 0.40f, 1.0f };

		void SetStatus(std::string a_text)
		{
			spdlog::info("F1 page: {}", a_text);
			std::scoped_lock l(g.lock);
			g.status = std::move(a_text);
		}

		// queues a_fn as an SKSE task (it runs on the main thread, next time the game runs its task queue)
		void Queue(std::function<void()> a_fn)
		{
			if (auto* task = SKSE::GetTaskInterface()) {
				task->AddTask([fn = std::move(a_fn)]() {
					try {
						fn();
					} catch (const std::exception& e) {
						spdlog::error("F1 page task failed: {}", e.what());
					} catch (...) {
						spdlog::error("F1 page task failed (unknown error)");
					}
				});
			}
		}

		bool Due(clock::time_point& a_at, std::chrono::milliseconds a_every)
		{
			const auto now = clock::now();
			if (a_at != clock::time_point{} && now - a_at < a_every) {
				return false;
			}
			a_at = now;
			return true;
		}

		std::string Hex(RE::FormID a_id) { return std::format("{:08X}", a_id); }

		// ------------------------------------------------------------------ small drawing helpers
		void Tip(const char* a_text)
		{
			if (im::IsItemHovered(im::ImGuiHoveredFlags_AllowWhenDisabled)) {
				im::BeginTooltip();
				im::PushTextWrapPos(im::GetFontSize() * 30.0f);
				im::TextUnformatted(a_text);
				im::PopTextWrapPos();
				im::EndTooltip();
			}
		}
		void Tip(const std::string& a_text) { Tip(a_text.c_str()); }

		void Line(const std::string& a_text) { im::TextUnformatted(a_text.c_str()); }
		void Wrapped(const std::string& a_text) { im::TextWrapped("%s", a_text.c_str()); }
		void Dim(const std::string& a_text) { im::TextDisabled("%s", a_text.c_str()); }
		void Colored(const im::ImVec4& a_col, const std::string& a_text) { im::TextColored(a_col, "%s", a_text.c_str()); }

		void AskFirst(std::string a_text, std::function<void()> a_yes)
		{
			g_ask.text = std::move(a_text);
			g_ask.yes = std::move(a_yes);
			g_ask.open = true;
		}

		// the top of every page: the last result
		void StatusLine()
		{
			std::string s;
			{
				std::scoped_lock l(g.lock);
				s = g.status;
			}
			if (!s.empty()) {
				Colored(kYellow, s);
				Tip("What the last button did. Most actions happen in the game when you close this menu.");
			}
		}

		// the bottom of every page: the confirmation, if a button asked for one
		void DrawAsk()
		{
			if (g_ask.open) {
				im::OpenPopup("Are you sure?##ReSaverPlus");
				g_ask.open = false;
			}
			if (im::BeginPopupModal("Are you sure?##ReSaverPlus", nullptr, im::ImGuiWindowFlags_AlwaysAutoResize)) {
				im::PushTextWrapPos(im::GetFontSize() * 26.0f);
				im::TextUnformatted(g_ask.text.c_str());
				im::PopTextWrapPos();
				im::Spacing();
				if (im::Button("Yes, do it")) {
					auto fn = std::move(g_ask.yes);
					g_ask.yes = nullptr;
					im::CloseCurrentPopup();
					if (fn) {
						fn();
					}
				}
				im::SameLine();
				if (im::Button("No")) {
					g_ask.yes = nullptr;
					im::CloseCurrentPopup();
				}
				im::EndPopup();
			}
		}

		bool NeedGame()
		{
			if (!Game::InGame()) {
				Colored(kYellow, "Load a save first.");
				return false;
			}
			return true;
		}

		// ------------------------------------------------------------------ sorting and searching (every list on the page)
		// case-insensitive A-Z comparison without copying the strings (the lists are sorted again every frame)
		int CompareI(std::string_view a, std::string_view b)
		{
			const auto n = std::min(a.size(), b.size());
			for (std::size_t i = 0; i < n; ++i) {
				const int x = std::tolower(static_cast<unsigned char>(a[i])), y = std::tolower(static_cast<unsigned char>(b[i]));
				if (x != y) {
					return x < y ? -1 : 1;
				}
			}
			return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
		}
		template <class T>
		int Compare3(const T& a, const T& b)
		{
			return a < b ? -1 : b < a ? 1 : 0;
		}

		std::string Lower(std::string s)
		{
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}
		// a_lowNeedle already lower case
		bool ContainsI(std::string_view a_text, const std::string& a_lowNeedle)
		{
			if (a_lowNeedle.empty()) {
				return true;
			}
			if (a_lowNeedle.size() > a_text.size()) {
				return false;
			}
			for (std::size_t i = 0; i + a_lowNeedle.size() <= a_text.size(); ++i) {
				std::size_t k = 0;
				while (k < a_lowNeedle.size() && std::tolower(static_cast<unsigned char>(a_text[i + k])) == static_cast<unsigned char>(a_lowNeedle[k])) {
					++k;
				}
				if (k == a_lowNeedle.size()) {
					return true;
				}
			}
			return false;
		}

		// the search box above a list; returns the search text in lower case
		std::string SearchBox(const char* a_id, char* a_buf, std::size_t a_size, const char* a_hint)
		{
			im::SetNextItemWidth(im::GetFontSize() * 18.0f);
			im::InputTextWithHint(a_id, a_hint, a_buf, a_size);
			Tip("Shows only the rows that contain this text (upper / lower case does not matter).");
			if (a_buf[0]) {
				im::SameLine();
				im::PushID(a_id);
				if (im::SmallButton("Clear")) {
					a_buf[0] = '\0';
				}
				im::PopID();
			}
			return Lower(a_buf);
		}

		// The sortable tables: click a column's header to sort by it, again to reverse. ImGui keeps the choice (and the
		// column widths) for each table. Read right after the table's columns are set up.
		struct Sort
		{
			int  column = 0;
			bool descending = false;
		};
		Sort ReadSort()
		{
			Sort s;
			if (auto* specs = im::TableGetSortSpecs(); specs && specs->Specs && specs->SpecsCount > 0) {
				s.column = specs->Specs[0].ColumnIndex;
				// ImGui keeps the direction in ONE byte (ImGuiSortDirection : ImU8, or a ": 8" bit field in older versions); the
				// header declares a 4-byte enum, so only its low byte is read (the other three are padding in the DLL's struct)
				s.descending = static_cast<std::uint8_t>(static_cast<unsigned>(specs->Specs[0].SortDirection) & 0xFFu) ==
				               static_cast<std::uint8_t>(im::ImGuiSortDirection_Descending);
				specs->SpecsDirty = false;  // sorted again every frame anyway: the lists change while the page is open
			}
			return s;
		}

		// sorts a_rows (indices into the list) with a_cmp(a, b) -> -1 / 0 / 1 for the ascending order; ties keep the list's order
		template <class Cmp>
		void Order(std::vector<std::size_t>& a_rows, const Sort& a_sort, Cmp a_cmp)
		{
			std::stable_sort(a_rows.begin(), a_rows.end(), [&](std::size_t a, std::size_t b) {
				const int c = a_cmp(a, b);
				return a_sort.descending ? c > 0 : c < 0;
			});
		}

		constexpr int kListTable = im::ImGuiTableFlags_Sortable | im::ImGuiTableFlags_RowBg | im::ImGuiTableFlags_BordersInnerH |
		                           im::ImGuiTableFlags_BordersOuter | im::ImGuiTableFlags_Resizable | im::ImGuiTableFlags_SizingStretchProp;
		constexpr int kNameColumn = im::ImGuiTableColumnFlags_WidthStretch | im::ImGuiTableColumnFlags_DefaultSort |
		                            im::ImGuiTableColumnFlags_PreferSortAscending;
		constexpr int kTextColumn = im::ImGuiTableColumnFlags_WidthStretch | im::ImGuiTableColumnFlags_PreferSortAscending;
		constexpr int kCountColumn = im::ImGuiTableColumnFlags_WidthStretch | im::ImGuiTableColumnFlags_PreferSortDescending;
		constexpr int kButtonColumn = im::ImGuiTableColumnFlags_WidthStretch | im::ImGuiTableColumnFlags_NoSort;

		// the height of a scrolling table with a_rows rows (header included), at most a_max rows high
		im::ImVec2 TableHeight(std::size_t a_rows, std::size_t a_max)
		{
			const float row = im::GetFrameHeightWithSpacing();
			return im::ImVec2{ 0.0f, row * static_cast<float>(std::clamp<std::size_t>(a_rows, 2, a_max)) + row * 0.5f };
		}

		// ------------------------------------------------------------------ 1. Rescue
		void __stdcall RenderRescue()
		{
			StatusLine();
			Wrapped("For a game that is stuck. The same as holding Ctrl for " + std::to_string(Settings::Get().rescueHoldSeconds) +
					" seconds in the game, or the buttons in ReSaver Plus.");
			im::Spacing();
			if (!NeedGame()) {
				DrawAsk();
				return;
			}
			if (im::Button("Save now")) {
				Queue([] {
					const auto name = Rescue::SaveGame();
					SetStatus("Saved as " + name + " (a new save).");
				});
			}
			Tip("Saves the game as a new save, ReSaverPP_Rescue_<date>_<time>. Your other saves are not touched.");
			im::SameLine();
			if (im::Button("Get my controls back")) {
				Queue([] {
					Rescue::RestoreControls();
					SetStatus("Your controls are switched back on.");
				});
			}
			Tip("EnablePlayerControls: moving, fighting, menus, activating, looking around, sneaking, the journal. For a scene or a mod "
				"that left you unable to move.\n\nThere is nothing to give back afterwards: in Skyrim this does not force anything, and "
				"scenes and mods can still switch your controls off when they need to. (\"Give the controls back to the game\" in "
				"ReSaver Plus is for Fallout 4 only.)");
			if (im::Button("Bring my followers")) {
				Queue([] {
					const auto names = Rescue::GatherFollowers(nullptr);
					std::string s;
					for (const auto& n : names) {
						s += (s.empty() ? "" : ", ") + n;
					}
					SetStatus(names.empty() ? std::string("No followers to bring.") : std::format("{} follower(s) brought to you: {}", names.size(), s));
				});
			}
			Tip("Every follower (vanilla, DLC and follower mods: anyone following you or set as your teammate) is moved next to you, "
				"in a half circle behind you, wherever they are. The ones listed in sGatherSkip are left alone (ReSaver Plus lets you "
				"pick them). To bring just one, use Followers.");
			im::SameLine();
			if (im::Button("All three")) {
				SetStatus("Saving, switching your controls on, then bringing your followers...");
				std::thread([] { SetStatus(Rescue::RunAll()); }).detach();
			}
			Tip("Saves first (a new save: the game as it was), switches your controls on, then brings your followers. Exactly what "
				"holding Ctrl does.");
			DrawAsk();
		}

		// ------------------------------------------------------------------ 2. My character (active effects)
		const char* kRunningTip =
			"RUNNING: this effect is acting on your character right now (its condition, if it has one, is true). \"constant\" "
			"means it lasts as long as its source is on you (a worn item, an ability, a perk); otherwise the time left is shown.";
		const char* kOffTip =
			"SWITCHED OFF: this effect is on your character but does NOTHING right now, because its condition is not met. Many mods "
			"and abilities add a set of effects, each with its own condition, and the game only turns on the ones whose condition "
			"is true (for example tiredness stages, day / night). The game switches it on and off by itself. It is not stuck, and "
			"removing it would not help: whatever gave it adds it again.";
		const char* kEndedTip =
			"ENDED: this effect is over (its time ran out, or something removed it) and the game has not cleared it from the list "
			"yet. It does not affect your character. The game clears these by itself.";

		void RequestEffects()
		{
			if (!Due(g_effectsAt, 1000ms) || g_effectsBusy.exchange(true)) {
				return;
			}
			Queue([] {
				auto v = std::make_shared<const LiveInfo::Effects>(LiveInfo::ReadEffects());
				{
					std::scoped_lock l(g.lock);
					g.effects = std::move(v);
				}
				g_effectsBusy = false;
			});
		}

		std::string TimeText(const LiveInfo::Effect& e)
		{
			if (e.duration <= 0.0f) {
				return "constant";
			}
			const float left = std::max(0.0f, e.duration - e.elapsed);
			return left >= 120.0f ? std::format("{:.0f} min", left / 60.0f) : std::format("{:.0f} s", left);
		}

		void RemoveSourceAsk(const LiveInfo::Source& s, bool a_onlyStuck)
		{
			const auto title = s.spellName + (s.itemName.empty() ? "" : " (" + s.itemName + ")");
			std::string what;
			if (a_onlyStuck) {
				what = "Remove the " + std::to_string(s.stuckCount) + " stuck effect(s) of " + title + "?\n\nEach one is dispelled, as a script's Dispel() does.";
			} else if (s.group == LiveInfo::Group::kWorn && s.itemWorn) {
				what = "Take off " + s.itemName + "?\n\nIts enchantment ends with it. You can put it on again.";
			} else if (s.group == LiveInfo::Group::kAbilities || s.group == LiveInfo::Group::kDiseases) {
				what = "Remove " + s.spellName + " from your character (Actor.RemoveSpell)?\n\nIf a mod, perk or race gives it, it may come back.";
			} else {
				what = "Remove all " + std::to_string(s.effects.size()) + " effect(s) of " + title + "?\n\nEach one is dispelled, as a script's Dispel() does.";
			}
			AskFirst(what, [spell = s.spell, item = s.item, other = s.fromOther, a_onlyStuck] {
				Queue([=] {
					SetStatus(LiveInfo::RemoveSource(spell, item, other, a_onlyStuck));
					g_effectsAt = {};
				});
			});
		}

		// the source the player clicked in the table (its effects and buttons are shown below the table)
		struct SourceKey
		{
			RE::FormID spell = 0, item = 0;
			bool       other = false, set = false;
			bool       Is(const LiveInfo::Source& s) const { return set && s.spell == spell && s.item == item && s.fromOther == other; }
		};
		SourceKey g_fxSel;
		char      g_fxFilter[128] = "";
		bool      g_fxGroups = true;

		std::string SourceTitle(const LiveInfo::Source& s)
		{
			return s.spellName + (s.itemName.empty() ? "" : "  (" + s.itemName + (s.itemWorn ? ", worn" : "") + ")");
		}

		bool SourceMatches(const LiveInfo::Source& s, const std::string& a_low)
		{
			if (a_low.empty() || ContainsI(s.spellName, a_low) || ContainsI(s.itemName, a_low) || ContainsI(s.plugin, a_low) ||
				ContainsI(s.itemPlugin, a_low) || ContainsI(s.kind, a_low) || ContainsI(Hex(s.spell), a_low)) {
				return true;
			}
			return std::ranges::any_of(s.effects, [&](const LiveInfo::Effect& e) { return ContainsI(e.name, a_low); });
		}

		// order of the state column: running, switched off, ended
		int StateRank(const LiveInfo::Effect& e) { return e.ended() ? 2 : e.off() ? 1 : 0; }
		float TimeLeft(const LiveInfo::Effect& e) { return e.duration <= 0.0f ? FLT_MAX : std::max(0.0f, e.duration - e.elapsed); }

		// the clicked source: its buttons and its effects (a sortable table too)
		void DrawSourceDetail(const LiveInfo::Source& s)
		{
			const auto id = Hex(s.spell) + Hex(s.item) + (s.fromOther ? "o" : "");
			im::PushID(id.c_str());
			im::SeparatorText(SourceTitle(s).c_str());
			Dim(std::format("{}  -  {}  -  magic item {}{}{}", s.kind, s.plugin.empty() ? std::string("(no plugin)") : s.plugin, Hex(s.spell),
				s.item ? ", from the item " + Hex(s.item) + (s.itemPlugin.empty() || s.itemPlugin == s.plugin ? "" : " (" + s.itemPlugin + ")") : std::string(),
				s.fromOther ? ", put on you by someone else" : ""));
			const bool  worn = s.group == LiveInfo::Group::kWorn && s.itemWorn;
			const bool  ability = s.group == LiveInfo::Group::kAbilities || s.group == LiveInfo::Group::kDiseases;
			const char* whole = worn ? "Take it off" : ability ? "Remove it" : "Remove all";
			if (im::SmallButton(whole)) {
				RemoveSourceAsk(s, false);
			}
			Tip(worn ? "Takes the item off (as Actor.UnequipItem): its enchantment ends with it." :
			    ability ? "Removes the ability or disease itself (Actor.RemoveSpell), so its effects go and do not come back." :
			              "Dispels every effect of this source (asks first).");
			if (s.stuckCount) {
				im::SameLine();
				if (im::SmallButton("Remove the stuck ones")) {
					RemoveSourceAsk(s, true);
				}
				Tip("Dispels only the stuck effects of this source (asks first). STUCK: " + s.stuckWhy + ".");
			}
			const bool perEffect = !ability;
			const auto flags = kListTable | (s.effects.size() > 12 ? im::ImGuiTableFlags_ScrollY : 0);
			if (im::BeginTable("fx", 5, flags, s.effects.size() > 12 ? TableHeight(13, 13) : im::ImVec2{ 0.0f, 0.0f })) {
				im::TableSetupScrollFreeze(0, 1);
				im::TableSetupColumn("Effect", kNameColumn, 3.0f);
				im::TableSetupColumn("Strength", kCountColumn, 1.0f);
				im::TableSetupColumn("Time left", kCountColumn, 1.0f);
				im::TableSetupColumn("State", kTextColumn, 1.2f);
				im::TableSetupColumn("", kButtonColumn, 1.0f);
				const auto sort = ReadSort();
				im::TableHeadersRow();
				std::vector<std::size_t> rows(s.effects.size());
				for (std::size_t k = 0; k < rows.size(); ++k) {
					rows[k] = k;
				}
				Order(rows, sort, [&](std::size_t a, std::size_t b) {
					const auto& x = s.effects[a];
					const auto& y = s.effects[b];
					int c = 0;
					switch (sort.column) {
					case 1: c = Compare3(x.magnitude, y.magnitude); break;
					case 2: c = Compare3(TimeLeft(x), TimeLeft(y)); break;
					case 3: c = Compare3(StateRank(x), StateRank(y)); break;
					default: break;
					}
					return c ? c : CompareI(x.name, y.name);
				});
				for (const auto k : rows) {
					const auto& e = s.effects[k];
					im::PushID(static_cast<int>(e.uid));
					im::TableNextRow();
					im::TableNextColumn();
					if (e.stuck) {
						Colored(kRed, e.name + "  (stuck)");
						Tip("STUCK: " + s.stuckWhy + ".");
					} else {
						Line(e.name);
					}
					im::TableNextColumn();
					Line(std::format("{:.1f}", e.magnitude));
					im::TableNextColumn();
					Line(TimeText(e));
					im::TableNextColumn();
					if (e.ended()) {
						Dim("ended");
						Tip(kEndedTip);
					} else if (e.off()) {
						Dim("switched off");
						Tip(kOffTip);
					} else {
						Line("running");
						Tip(kRunningTip);
					}
					im::TableNextColumn();
					const bool canRemove = perEffect && !(worn && !e.stuck);
					im::BeginDisabled(!canRemove);
					if (im::SmallButton("Remove")) {
						AskFirst("Remove " + e.name + " from your character?\n\nIt is dispelled, as a script's Dispel() does.", [uid = e.uid] {
							Queue([uid] {
								SetStatus(LiveInfo::RemoveEffect(uid));
								g_effectsAt = {};
							});
						});
					}
					im::EndDisabled();
					Tip(canRemove ? "Dispels this one effect (asks first)." :
					    perEffect ? "It comes from something you wear: take the item off instead (button above)." :
					                "An ability's effects come back while you have the ability: remove the ability itself (button above).");
					im::PopID();
				}
				im::EndTable();
			}
			im::PopID();
		}

		void __stdcall RenderEffects()
		{
			StatusLine();
			if (!NeedGame()) {
				DrawAsk();
				return;
			}
			RequestEffects();
			std::shared_ptr<const LiveInfo::Effects> vp;
			{
				std::scoped_lock l(g.lock);
				vp = g.effects;
			}
			const auto& v = *vp;
			if (!v.valid) {
				Dim("Reading your effects...");
				DrawAsk();
				return;
			}
			Line(std::format("{} effects from {} sources: {} running, {} switched off, {} ended.", v.total, v.sources.size(), v.running, v.off, v.ended));
			Tip("Running: acting on you now. Switched off: their condition is not met right now, so they do nothing until it is "
				"(normal for many mods). Ended: over, not yet cleared by the game. Hover a state below for more.");
			im::Spacing();
			// stuck first, A-Z
			if (v.stuck) {
				im::SeparatorText(std::format("Stuck on your character ({})", v.stuck).c_str());
				std::vector<std::size_t> stuck;
				for (std::size_t i = 0; i < v.sources.size(); ++i) {
					if (v.sources[i].stuckCount) {
						stuck.push_back(i);
					}
				}
				Order(stuck, Sort{}, [&](std::size_t a, std::size_t b) { return CompareI(v.sources[a].spellName, v.sources[b].spellName); });
				for (const auto i : stuck) {
					const auto& s = v.sources[i];
					im::PushID(static_cast<int>(100000 + i));
					Colored(kRed, std::format("{}: {} effect(s)", s.spellName, s.stuckCount));
					Tip("An item's enchantment that never runs out is still on you, but " + s.stuckWhy +
						". Such effects stay until something removes them.");
					im::SameLine();
					if (im::SmallButton("Remove them")) {
						RemoveSourceAsk(s, true);
					}
					Tip("Dispels only the stuck effects of this source (asks first).");
					im::PopID();
				}
			} else {
				Colored(kGreen, "Nothing stuck: every item enchantment on you comes from something you wear.");
			}
			im::Spacing();
			const auto low = SearchBox("##fxsearch", g_fxFilter, sizeof(g_fxFilter), "search: name, item, type, plugin, effect");
			im::SameLine();
			im::Checkbox("Group by kind", &g_fxGroups);
			Tip("On: worn items, items not worn, spells, potions, abilities, diseases, put on you by others, each sorted the way you "
				"chose. Off: one list, sorted only the way you chose.");
			std::vector<std::size_t> rows;
			for (std::size_t i = 0; i < v.sources.size(); ++i) {
				if (SourceMatches(v.sources[i], low)) {
					rows.push_back(i);
				}
			}
			if (!low.empty()) {
				Dim(std::format("{} of {} sources match.", rows.size(), v.sources.size()));
			}
			const LiveInfo::Source* selected = nullptr;
			std::size_t             groups = 0;
			if (g_fxGroups) {
				bool seen[static_cast<int>(LiveInfo::Group::kCount)]{};
				for (const auto i : rows) {
					auto& b = seen[static_cast<int>(v.sources[i].group)];
					groups += !b;
					b = true;
				}
			}
			if (im::BeginTable("sources", 5, kListTable | im::ImGuiTableFlags_ScrollY, TableHeight(rows.size() + groups + 1, 18))) {
				im::TableSetupScrollFreeze(0, 1);
				im::TableSetupColumn("Name", kNameColumn, 3.2f);
				im::TableSetupColumn("Type", kTextColumn, 1.2f);
				im::TableSetupColumn("Plugin", kTextColumn, 1.8f);
				im::TableSetupColumn("Effects", kCountColumn, 0.7f);
				im::TableSetupColumn("Running", kCountColumn, 0.7f);
				const auto sort = ReadSort();
				im::TableHeadersRow();
				Order(rows, sort, [&](std::size_t a, std::size_t b) {
					const auto& x = v.sources[a];
					const auto& y = v.sources[b];
					int         c = 0;
					switch (sort.column) {
					case 1: c = CompareI(x.kind, y.kind); break;
					case 2: c = CompareI(x.plugin, y.plugin); break;
					case 3: c = Compare3(x.effects.size(), y.effects.size()); break;
					case 4: c = Compare3(x.running, y.running); break;
					default: break;
					}
					if (!c) {
						c = CompareI(x.spellName, y.spellName);
					}
					return c ? c : CompareI(x.itemName, y.itemName);
				});
				if (g_fxGroups) {  // groups in their fixed order, the player's sort inside each
					std::stable_sort(rows.begin(), rows.end(), [&](std::size_t a, std::size_t b) { return v.sources[a].group < v.sources[b].group; });
				}
				int group = -1;
				for (const auto i : rows) {
					const auto& s = v.sources[i];
					if (g_fxGroups && static_cast<int>(s.group) != group) {
						group = static_cast<int>(s.group);
						const auto n = std::ranges::count_if(rows, [&](std::size_t k) { return v.sources[k].group == s.group; });
						im::TableNextRow();
						im::TableNextColumn();
						Colored(kYellow, std::format("{} ({})", LiveInfo::GroupName(s.group), n));
					}
					const auto id = Hex(s.spell) + Hex(s.item) + (s.fromOther ? "o" : "");  // stable while the list changes
					im::TableNextRow();
					im::TableNextColumn();
					const bool sel = g_fxSel.Is(s);
					if (sel) {
						selected = &s;
					}
					if (s.stuckCount) {
						im::PushStyleColor(im::ImGuiCol_Text, kRed);
					}
					if (im::Selectable((SourceTitle(s) + "##" + id).c_str(), sel, im::ImGuiSelectableFlags_SpanAllColumns)) {
						g_fxSel = { s.spell, s.item, s.fromOther, true };
						selected = &s;
					}
					if (s.stuckCount) {
						im::PopStyleColor();
					}
					if (im::IsItemHovered(im::ImGuiHoveredFlags_AllowWhenDisabled)) {  // the text only for the row under the mouse
						Tip(std::format("Click to see its effects and the buttons to remove them (below the list).\n\nMagic item {} ({}){}{}{}",
						Hex(s.spell), s.plugin.empty() ? std::string("no plugin") : s.plugin,
						s.item ? "\nFrom the item " + Hex(s.item) + (s.itemPlugin.empty() ? "" : " (" + s.itemPlugin + ")") : std::string(),
						s.fromOther ? "\nPut on you by someone else" : "", s.stuckCount ? std::format("\n{} stuck effect(s)", s.stuckCount) : std::string()));
					}
					im::TableNextColumn();
					Line(s.kind);
					im::TableNextColumn();
					Line(s.plugin);
					im::TableNextColumn();
					Line(std::to_string(s.effects.size()));
					im::TableNextColumn();
					Line(std::to_string(s.running));
				}
				im::EndTable();
			}
			if (!selected && g_fxSel.set) {
				for (const auto& s : v.sources) {
					if (g_fxSel.Is(s)) {
						selected = &s;  // hidden by the search, still shown below
					}
				}
			}
			if (selected) {
				DrawSourceDetail(*selected);
			} else {
				Dim(g_fxSel.set ? "That source is not on you any more." : "Click a row to see its effects and the buttons to remove them.");
			}
			DrawAsk();
		}

		// ------------------------------------------------------------------ 3. Followers
		void RequestFollowers(bool a_now)
		{
			if (a_now) {
				g_followersAt = {};
			}
			if (!Due(g_followersAt, 3000ms) || g_followersBusy.exchange(true)) {
				return;
			}
			Queue([] {
				auto f = std::make_shared<const LiveInfo::Followers>(LiveInfo::ReadFollowers());
				{
					std::scoped_lock l(g.lock);
					g.followers = std::move(f);
				}
				g_followersBusy = false;
			});
		}

		void __stdcall RenderFollowers()
		{
			StatusLine();
			if (!NeedGame()) {
				DrawAsk();
				return;
			}
			RequestFollowers(false);
			std::shared_ptr<const LiveInfo::Followers> fp;
			{
				std::scoped_lock l(g.lock);
				fp = g.followers;
			}
			const auto& f = *fp;
			if (!f.valid) {
				Dim("Looking for your followers...");
				DrawAsk();
				return;
			}
			if (f.list.empty()) {
				Line("Nobody is following you right now.");
			} else if (im::BeginTable("followers", 4, kListTable)) {
				im::TableSetupColumn("Name", kNameColumn, 1.4f);
				im::TableSetupColumn("Where", kTextColumn, 1.6f);
				im::TableSetupColumn("Why", kTextColumn, 1.0f);
				im::TableSetupColumn("", kButtonColumn, 2.6f);
				const auto sort = ReadSort();
				im::TableHeadersRow();
				std::vector<std::size_t> rows(f.list.size());
				for (std::size_t i = 0; i < rows.size(); ++i) {
					rows[i] = i;
				}
				Order(rows, sort, [&](std::size_t a, std::size_t b) {
					const auto& x = f.list[a];
					const auto& y = f.list[b];
					const int   c = sort.column == 1 ? CompareI(x.where, y.where) : sort.column == 2 ? CompareI(x.why, y.why) : 0;
					return c ? c : CompareI(x.name, y.name);
				});
				for (const auto i : rows) {
					const auto& fl = f.list[i];
					im::PushID(static_cast<int>(fl.id));
					im::TableNextRow();
					im::TableNextColumn();
					Line(fl.name);
					Tip(std::format("{} ({}){}", fl.key, Hex(fl.id), fl.skip ? "\nListed in sGatherSkip: \"Bring my followers\" leaves them alone." : ""));
					im::TableNextColumn();
					Line(fl.where);
					im::TableNextColumn();
					Line(fl.why + (fl.teammate && fl.why != "teammate" ? ", teammate" : ""));
					Tip("vanilla follower quest: the game's own follower (DialogueFollower). follower faction: a follower mod or DLC "
						"follower (CurrentFollowerFaction). teammate: set as your teammate (Actor.SetPlayerTeammate).");
					im::TableNextColumn();
					if (im::SmallButton("Bring")) {
						Queue([id = fl.id] {
							const std::vector<RE::FormID> one{ id };
							const auto names = Rescue::GatherFollowers(&one);
							SetStatus(names.empty() ? std::string("They could not be moved (see ReSaverHelper.log).") : names.front() + " was brought to you.");
						});
					}
					Tip("Moves them next to you, wherever they are (Papyrus MoveTo).");
					im::SameLine();
					if (im::SmallButton(fl.teammate ? "Teammate off" : "Teammate on")) {
						Queue([id = fl.id, on = !fl.teammate, name = fl.name] {
							const auto why = SkyFix::Teammate(id, on);
							SetStatus(why.empty() ? name + (on ? ": teammate flag on." : ": teammate flag off.") : why);
							g_followersAt = {};
						});
					}
					Tip("Actor.SetPlayerTeammate. A teammate follows you into fights, sneaks with you and can be asked for favors. "
						"Switch it off for someone who is not with you any more but still acts like it.");
					im::SameLine();
					if (fl.vanilla) {
						if (im::SmallButton("Dismiss")) {
							AskFirst("Dismiss " + fl.name + "?\n\nThe vanilla follower quest's own dismissal (DialogueFollower stage 400): they go home.",
								[name = fl.name] {
									Queue([name] {
										Game::SetStage(0x000750BA, 400, [name](bool a_ok) {
											SetStatus(a_ok ? name + " was dismissed." : "The game refused the dismissal (stage 400 of DialogueFollower).");
											g_followersAt = {};
										});
									});
								});
						}
						Tip("DialogueFollower stage 400 = DismissFollower(), what saying \"It's time for us to part ways\" does.");
					} else {
						if (im::SmallButton("Take back")) {
							AskFirst("Make " + fl.name + " your vanilla follower again?\n\nDialogueFollowerScript.SetFollower, what \"Follow me\" "
										 "does. Refused while someone else is your vanilla follower.",
								[id = fl.id] {
									Queue([id] {
										const auto why = SkyFix::SetFollower(id);
										SetStatus(why.empty() ? std::string("Taken back as your follower.") : why);
										g_followersAt = {};
									});
								});
						}
						Tip("For a vanilla follower who stopped following (the game still thinks you have a follower, or they were left in "
							"a faction). Follower mods have their own ways: use those first.");
					}
					im::PopID();
				}
				im::EndTable();
			}
			if (im::SmallButton("Look again")) {
				RequestFollowers(true);
			}
			im::Spacing();
			im::SeparatorText("Beast form");
			Line("Your race: " + f.race + (f.werewolf ? " (werewolf form)" : f.vampireLord ? " (Vampire Lord form)" : ""));
			if (im::Button("Get out of werewolf form")) {
				AskFirst("Turn back from werewolf form?\n\nThe transformation script's own ShiftBack(), after its \"already busy\" flags are reset.", [] {
					Queue([] {
						const auto why = SkyFix::BeastRevert("werewolf");
						SetStatus(why.empty() ? std::string("Turning you back (close the menu).") : why);
					});
				});
			}
			Tip("PlayerWerewolfChangeScript.ShiftBack(): your race from before, your powers, armor and controls. Also finishes a turn "
				"back that stopped half way.");
			im::SameLine();
			if (im::Button("Get out of Vampire Lord form")) {
				AskFirst("Turn back from Vampire Lord form?\n\nThe transformation script's own ShiftBack(), after its \"already busy\" flags are reset.", [] {
					Queue([] {
						const auto why = SkyFix::BeastRevert("vampire");
						SetStatus(why.empty() ? std::string("Turning you back (close the menu).") : why);
					});
				});
			}
			Tip("DLC1PlayerVampireChangeScript.ShiftBack() (Dawnguard).");
			DrawAsk();
		}

		// ------------------------------------------------------------------ 4. Quests
		char g_questFilter[128] = "";

		void RequestQuests(bool a_now)
		{
			if (a_now) {
				g_questsAt = {};
			}
			if (!Due(g_questsAt, 5000ms) || g_questsBusy.exchange(true)) {
				return;
			}
			Queue([] {
				auto q = std::make_shared<const std::vector<LiveInfo::QuestRow>>(LiveInfo::ReadRunningQuests());
				{
					std::scoped_lock l(g.lock);
					g.quests = std::move(q);
				}
				g_questsBusy = false;
			});
		}

		void RequestQuest(RE::FormID a_id, bool a_now)
		{
			if (a_now) {
				g_questAt = {};
			}
			if (!a_id || !Due(g_questAt, 1000ms) || g_questBusy.exchange(true)) {
				return;
			}
			Queue([a_id] {
				auto d = LiveInfo::ReadQuest(a_id);
				{
					std::scoped_lock l(g.lock);
					if (g.questSel == a_id) {
						g.quest = std::move(d);
					}
				}
				g_questBusy = false;
			});
		}

		const char* ObjectiveState(std::uint8_t a_state)
		{
			switch (a_state) {
			case 1: return "shown";
			case 2: return "done (hidden)";
			case 3: return "done";
			case 4: return "failed (hidden)";
			case 5: return "failed";
			default: return "not shown";
			}
		}

		void __stdcall RenderQuests()
		{
			StatusLine();
			if (!NeedGame()) {
				DrawAsk();
				return;
			}
			const auto prog = Game::RefreshProgress();
			im::BeginDisabled(prog.running);
			if (im::Button("Refresh the journal")) {
				Queue([] {
					const auto n = Game::StartRefresh("ini");
					SetStatus(n ? std::format("Putting {} lost objective(s) back in the journal. Close the menu and wait for the message.", n) :
					              std::string("Nothing to put back: the journal already lists every objective (or a refresh is already running)."));
				});
			}
			im::EndDisabled();
			Tip("For quests or objectives missing from the journal: each objective a running quest shows that the journal's list does "
				"not hold is hidden and shown again, one quest at a time, so the game lists it again. Objectives already listed are left "
				"alone (no on-screen message for them). Keep menus closed until it says it is done.");
			if (prog.running) {
				im::SameLine();
				Dim(std::format("{} of {} done", prog.done, prog.total));
			}
			im::Spacing();
			im::SeparatorText("Fix a stuck quest");
			RequestQuests(false);
			std::shared_ptr<const std::vector<LiveInfo::QuestRow>> rowsp;
			RE::FormID                      sel = 0;
			LiveInfo::QuestDetail           d;
			{
				std::scoped_lock l(g.lock);
				rowsp = g.quests;
				sel = g.questSel;
				d = g.quest;
			}
			const auto& all = *rowsp;
			const auto  filter = SearchBox("##questfilter", g_questFilter, sizeof(g_questFilter), "search: name, editor ID, plugin, form ID");
			std::vector<std::size_t> rows;
			for (std::size_t i = 0; i < all.size(); ++i) {
				const auto& r = all[i];
				if (filter.empty() || ContainsI(r.name, filter) || ContainsI(r.editorID, filter) || ContainsI(r.plugin, filter) || ContainsI(Hex(r.id), filter)) {
					rows.push_back(i);
				}
			}
			im::SameLine();
			Dim(filter.empty() ? std::format("{} running quests", all.size()) : std::format("{} of {} running quests", rows.size(), all.size()));
			if (im::BeginTable("quests", 4, kListTable | im::ImGuiTableFlags_ScrollY, TableHeight(rows.size() + 1, 12))) {
				im::TableSetupScrollFreeze(0, 1);
				im::TableSetupColumn("Name", kNameColumn, 2.4f);
				im::TableSetupColumn("Editor ID", kTextColumn, 2.0f);
				im::TableSetupColumn("Plugin", kTextColumn, 1.6f);
				im::TableSetupColumn("Stage", kCountColumn, 0.6f);
				const auto sort = ReadSort();
				im::TableHeadersRow();
				Order(rows, sort, [&](std::size_t a, std::size_t b) {
					const auto& x = all[a];
					const auto& y = all[b];
					int         c = 0;
					switch (sort.column) {
					case 1: c = CompareI(x.editorID, y.editorID); break;
					case 2: c = CompareI(x.plugin, y.plugin); break;
					case 3: c = Compare3(x.stage, y.stage); break;
					default:
						if (x.name.empty() != y.name.empty()) {
							return (x.name.empty() ? 1 : -1) * (sort.descending ? -1 : 1);  // quests with no name last, both ways
						}
						c = CompareI(x.name, y.name);
						break;
					}
					return c ? c : CompareI(x.editorID, y.editorID);
				});
				for (const auto i : rows) {
					const auto& r = all[i];
					im::TableNextRow();
					im::TableNextColumn();
					const auto label = (r.name.empty() ? std::string("(no name)") : r.name) + "##" + Hex(r.id);
					if (im::Selectable(label.c_str(), r.id == sel, im::ImGuiSelectableFlags_SpanAllColumns)) {
						{
							std::scoped_lock l(g.lock);
							g.questSel = r.id;
							g.quest = {};
						}
						RequestQuest(r.id, true);
					}
					if (im::IsItemHovered(im::ImGuiHoveredFlags_AllowWhenDisabled)) {
						Tip(std::format("{} ({}, {}). Click to see its stages and objectives below.", r.editorID, Hex(r.id), r.plugin.empty() ? std::string("no plugin") : r.plugin));
					}
					im::TableNextColumn();
					Line(r.editorID);
					im::TableNextColumn();
					Line(r.plugin);
					im::TableNextColumn();
					Line(std::to_string(r.stage));
				}
				im::EndTable();
			}
			if (!sel) {
				Dim("Pick a quest above.");
				DrawAsk();
				return;
			}
			RequestQuest(sel, false);
			if (!d.found || d.id != sel) {
				Dim("Reading the quest...");
				DrawAsk();
				return;
			}
			const auto title = (d.name.empty() ? d.editorID : d.name) + " [" + d.editorID + ", " + Hex(d.id) + "]";
			Line(title);
			Line(std::format("Stage {}. {}{}", d.stage, d.running ? "Running" : "Not running", d.completed ? ", completed." : "."));
			// stages
			if (im::TreeNodeEx("Stages##q", im::ImGuiTreeNodeFlags_DefaultOpen)) {
				if (d.stages.empty()) {
					Dim("This quest has no stages.");
				}
				for (std::size_t i = 0; i < d.stages.size(); ++i) {
					const auto& st = d.stages[i];
					im::PushID(static_cast<int>(i));
					std::string note = st.index == d.stage ? "current" : "";
					if (st.flags & 2) {
						note += " start-up";
					}
					if (st.flags & 4) {
						note += " shut-down";
					}
					if (i % 5 != 0) {
						im::SameLine();
					}
					if (im::SmallButton(std::format("Set {}", st.index).c_str())) {
						AskFirst(std::format("Set {} to stage {}?\n\nQuest.SetStage: the stage's own script runs, as if you had played to it "
											 "(rewards, follow-up quests). It cannot be undone.",
									 title, st.index),
							[id = d.id, stage = st.index] {
								Queue([id, stage] {
									Game::SetStage(id, stage, [stage](bool a_ok) {
										SetStatus(a_ok ? std::format("Stage {} set.", stage) : std::format("The game refused stage {}.", stage));
										g_questAt = {};
									});
								});
							});
					}
					Tip(note.empty() ? std::string("Set the quest to this stage (asks first).") :
					                   "Set the quest to this stage (asks first). This stage: " + note + " (start-up / shut-down: marked so in the plugin).");
					if (!note.empty()) {
						im::SameLine();
						Dim("(" + note + ")");
					}
					im::PopID();
				}
				im::TreePop();
			}
			// objectives
			if (im::TreeNodeEx("Objectives##q", im::ImGuiTreeNodeFlags_DefaultOpen)) {
				if (d.objectives.empty()) {
					Dim("This quest has no objectives.");
				}
				for (std::size_t i = 0; i < d.objectives.size(); ++i) {
					const auto& o = d.objectives[i];
					im::PushID(static_cast<int>(1000 + i));
					const bool done = o.state == 2 || o.state == 3;
					im::BeginDisabled(done);
					if (im::SmallButton("Complete")) {
						AskFirst(std::format("Complete objective {} of {}?\n\n\"{}\"\n\nQuest.SetObjectiveCompleted. The quest's scripts may react to it.",
									 o.index, title, o.text),
							[id = d.id, index = o.index] {
								Queue([id, index] {
									Game::CompleteObjective(id, index, [index](bool a_ok) {
										SetStatus(a_ok ? std::format("Objective {} completed.", index) : std::format("Objective {} is not completed.", index));
										g_questAt = {};
									});
								});
							});
					}
					im::EndDisabled();
					im::SameLine();
					Line(std::format("{}  {}  ({})", o.index, o.text.empty() ? std::string("(no text)") : o.text, ObjectiveState(o.state)));
					im::PopID();
				}
				im::TreePop();
			}
			im::BeginDisabled(!d.running);
			if (im::Button("Stop quest")) {
				AskFirst("Stop " + title + "?\n\nQuest.Stop: the game stops it and cleans up after it (its aliases are emptied, its objectives "
										   "leave the journal). No rewards. It cannot be undone.",
					[id = d.id] {
						Queue([id] {
							Game::Stop(id, [](bool a_ok) {
								SetStatus(a_ok ? std::string("Quest stopped.") : std::string("The quest is still running."));
								g_questAt = {};
								g_questsAt = {};
							});
						});
					});
			}
			im::EndDisabled();
			Tip("For a quest that is broken and keeps you from going on (asks first).");
			DrawAsk();
		}

		// ------------------------------------------------------------------ 5. Troubleshooting (live)
		void StartSampler()
		{
			g_sampling = true;
			if (g_samplerStarted.exchange(true)) {
				return;
			}
			std::thread([] {
				while (true) {
					std::this_thread::sleep_for(3s);
					if (!g_sampling || !Game::InGame()) {
						continue;
					}
					const double t = std::chrono::duration<double>(clock::now() - g_start).count();
					Queue([t] {
						auto s = LiveInfo::SampleVM(t);
						if (!s.valid) {
							return;
						}
						std::scoped_lock l(g.lock);
						if (!g.samples.empty()) {
							g.samples.back().topScripts.clear();  // only the newest sample's list is shown (the page copies the samples every frame)
						}
						g.samples.push_back(std::move(s));
						while (g.samples.size() > 60) {
							g.samples.pop_front();
						}
					});
				}
			}).detach();
			spdlog::info("F1 page: script health sampling started (every 3 seconds)");
		}

		struct Trend
		{
			const char* word = "not known yet";
			im::ImVec4  color = kYellow;
			double      perMinute = 0;
		};

		// over the last minute (20 samples): least-squares slope of the thread count
		Trend TrendOf(const std::deque<LiveInfo::VMSample>& a_s)
		{
			Trend t;
			const std::size_t n = std::min<std::size_t>(a_s.size(), 20);
			if (n < 5) {
				return t;
			}
			double sx = 0, sy = 0, sxx = 0, sxy = 0;
			for (std::size_t i = a_s.size() - n; i < a_s.size(); ++i) {
				const double x = a_s[i].time, y = a_s[i].stacks;
				sx += x;
				sy += y;
				sxx += x * x;
				sxy += x * y;
			}
			const double den = n * sxx - sx * sx;
			const double slope = den != 0 ? (n * sxy - sx * sy) / den : 0;  // threads per second
			t.perMinute = slope * 60.0;
			const double mean = sy / n;
			const double big = std::max(10.0, mean * 0.10);
			if (t.perMinute > big) {
				t.word = "growing";
				t.color = kRed;
			} else if (t.perMinute < -big) {
				t.word = "falling";
				t.color = kGreen;
			} else {
				t.word = "steady";
				t.color = kGreen;
			}
			return t;
		}

		char g_scriptFilter[128] = "";

		void __stdcall RenderHealth()
		{
			StatusLine();
			if (!g_samplerStarted) {
				StartSampler();  // the first time this page is opened; after that only the button starts it again
			}
			std::deque<LiveInfo::VMSample> s;
			{
				std::scoped_lock l(g.lock);
				s = g.samples;
			}
			im::SeparatorText("Script health");
			if (s.empty()) {
				Dim(Game::InGame() ? "Reading the script engine (every 3 seconds)..." : "Load a save first.");
				DrawAsk();
				return;
			}
			const auto& last = s.back();
			const auto  trend = TrendOf(s);
			Line(std::format("Script threads: {}", last.stacks));
			Tip("Every script call the game is running or keeping for later (Papyrus stacks). A few hundred is normal in a modded game.");
			im::SameLine();
			Colored(trend.color, std::format("  {}{}", trend.word, s.size() >= 5 ? std::format(" ({:+.0f} per minute)", trend.perMinute) : std::string()));
			Tip("Over the last minute. \"growing\" for minutes on end means scripts start faster than they finish: script lag. "
				"Waiting in one place, the count should stay about the same.");
			Line(std::format("Waiting on a timer or a latent call: {}", last.latent));
			Tip("Threads in Utility.Wait, RegisterForSingleUpdate and other calls that return later.");
			Line(std::format("Waiting their turn (suspended): {}", last.suspended));
			Tip("Threads the script engine paused because its time for this frame ran out; it goes on with them next frame. These are "
				"the \"suspended stacks\" a save shows. Many of them, all the time, means the engine cannot keep up.");
			Line(std::format("Calls not started yet: {}", last.queuedCalls));
			Tip("Function calls (events, mods' calls) queued for the script engine and not started yet.");
			std::vector<float> plot;
			for (const auto& x : s) {
				plot.push_back(static_cast<float>(x.stacks));
			}
			im::PlotLines("##stacks", plot.data(), static_cast<int>(plot.size()), 0, "script threads, last 3 minutes", FLT_MAX, FLT_MAX,
				im::ImVec2{ 0.0f, im::GetFontSize() * 3.0f });
			if (!last.topScripts.empty() && im::TreeNodeEx("Scripts with the most threads##top", im::ImGuiTreeNodeFlags_DefaultOpen)) {
				Tip("The script and function each thread is in right now. One script with hundreds of threads is usually the one lagging.");
				const auto filter = SearchBox("##scriptsearch", g_scriptFilter, sizeof(g_scriptFilter), "search: script or function");
				std::vector<std::size_t> rows;
				for (std::size_t i = 0; i < last.topScripts.size(); ++i) {
					if (ContainsI(last.topScripts[i].first, filter)) {
						rows.push_back(i);
					}
				}
				im::SameLine();
				Dim(std::format("the {} busiest", last.topScripts.size()));
				Tip(std::format("Up to {} script functions, the ones with the most threads. Click Threads or Script to sort by it.", LiveInfo::kTopScripts));
				if (im::BeginTable("topscripts", 2, kListTable | im::ImGuiTableFlags_ScrollY, TableHeight(rows.size() + 1, 14))) {
					im::TableSetupScrollFreeze(0, 1);
					// the count is what this list is for, so it is the column sorted by first (most first); Script sorts A-Z
					im::TableSetupColumn("Threads", im::ImGuiTableColumnFlags_WidthStretch | im::ImGuiTableColumnFlags_DefaultSort |
														im::ImGuiTableColumnFlags_PreferSortDescending,
						0.6f);
					im::TableSetupColumn("Script.Function", kTextColumn, 4.0f);
					const auto sort = ReadSort();
					im::TableHeadersRow();
					Order(rows, sort, [&](std::size_t a, std::size_t b) {
						const auto& x = last.topScripts[a];
						const auto& y = last.topScripts[b];
						const int   c = sort.column == 0 ? Compare3(x.second, y.second) : 0;
						return c ? c : CompareI(x.first, y.first);
					});
					for (const auto i : rows) {
						im::TableNextRow();
						im::TableNextColumn();
						Line(std::to_string(last.topScripts[i].second));
						im::TableNextColumn();
						Line(last.topScripts[i].first);
					}
					im::EndTable();
				}
				if (last.unreadable) {
					Dim(std::format("{} thread(s) could not be read", last.unreadable));
				}
				im::TreePop();
			} else {
				Tip("The script and function each thread is in right now. One script with hundreds of threads is usually the one lagging.");
			}
			if (g_sampling) {
				if (im::SmallButton("Stop reading")) {
					g_sampling = false;
				}
			} else if (im::SmallButton("Read again")) {
				StartSampler();
			}
			Tip("Reading starts when you first open this page and goes on every 3 seconds, also while you play, until you quit the "
				"game or stop it here (a short read on the game's main thread).");

			im::Spacing();
			im::SeparatorText("Save health");
			// two readings in a row, 3 s apart (a save or a load never shows here: SKSE runs no task while the game saves)
			const auto* prev = s.size() >= 2 ? &s[s.size() - 2] : nullptr;
			const bool  cannotPause = prev && last.freezeAsked && prev->freezeAsked && last.freezeState != 2 && prev->freezeState != 2;
			const bool  paused = last.freezeAsked && last.freezeState == 2;
			const bool  oddFreeze = prev && !last.freezeAsked && !prev->freezeAsked && last.freezeState != 0 && prev->freezeState != 0;
			if (cannotPause) {
				Colored(kRed, "Scripts were asked to pause and cannot. A save now will be bad (no script data).");
				Tip("Saving pauses the script engine first and waits for it. Here it was asked to pause and has not managed for two "
					"readings. A save waits 30 seconds, then is written WITHOUT script data. A script thread waiting for nothing does this "
					"(for example after suspended stacks were deleted from a save). Do not save over a good save: quit and check your last "
					"save with ReSaver Plus's Save check.");
			} else if (paused) {
				Colored(kYellow, "Scripts are paused (the console's ToggleScripts, \"ts\", or the game is closing).");
				Tip("Type ts in the console again to switch scripts back on.");
			} else if (oddFreeze) {
				Colored(kYellow, std::format("The script engine reports a freeze (state {}) that nothing asked for.", last.freezeState));
				Tip("Not seen in a normal game. Write a report and send it with ReSaverHelper.log.");
			} else {
				Colored(kGreen, "Normal: scripts are running and can pause for a save.");
				Tip("Read from the script engine's own freeze request and freeze state.");
			}
			Wrapped("To check the next save before you make it: close this menu, open the console, type ts (scripts pause), wait a few "
					"seconds, then come back here. \"Scripts are paused\" means a save would be fine. \"cannot pause\" means it would be "
					"bad. Type ts again afterwards.");
			Tip("ToggleScripts asks the script engine to pause exactly as saving does (the same function in the game), so it shows the "
				"same result without making a save.");
			DrawAsk();
		}

		std::string ExtraReportLines()
		{
			std::string r = std::format("SKSE Menu Framework: version {} (API {})\n", SKSEMenuFramework::GetMenuFrameworkVersion(),
				SKSEMenuFramework::GetMenuFrameworkAPIVersion());
			r += std::format("ReSaver Plus connected: {}\n", Server::Connected() ? "yes" : "no");
			r += MenuGuard::ReportText();  // version 16
			return r;
		}

		std::string Today()
		{
			const auto now = std::time(nullptr);
			std::tm    tm{};
			localtime_s(&tm, &now);
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
			return buf;
		}

		// "Open it": the file in the player's text editor (its own thread: ShellExecute can take a moment)
		void OpenButton(const std::filesystem::path& a_file)
		{
			if (im::SmallButton("Open it")) {
				std::thread([a_file] { ShellExecuteW(nullptr, L"open", a_file.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }).detach();
			}
			Tip("Opens it in your text editor (the game may lose focus).");
		}

		std::string SendText(bool a_reportBelow);

		// Builds the report on the main thread (an SKSE task), then asks where to save it: the Windows Save As dialog on
		// its own thread (SaveDialog.h). a_send: the "What to send" file (the list of files to attach, then the report),
		// proposed on the Desktop as ReSaverHelper-report-<date>.txt; else the report alone, proposed where it always went
		// (the SKSE log folder, ReSaverHelper-report.txt).
		void SaveReport(bool a_send)
		{
			if (SaveDialog::Busy()) {
				SetStatus("A Save As dialog is already open (if you do not see it, it may be behind the game: press Alt+Tab).");
				return;
			}
			LiveInfo::VMSample last;
			{
				std::scoped_lock l(g.lock);
				if (!g.samples.empty()) {
					last = g.samples.back();
				}
			}
			const auto extra = ExtraReportLines();
			SetStatus("Writing the report: choose where to save it in the Save As dialog.");
			Queue([last, extra, a_send] {
				auto                  text = LiveInfo::ReportText(last, extra);
				std::filesystem::path folder, name;
				if (a_send) {
					text = SendText(true) + "\n==================== the report ====================\n\n" + text;
					folder = SaveDialog::Desktop();
					name = "ReSaverHelper-report-" + Today() + ".txt";
				} else {
					const auto def = LiveInfo::DefaultReportPath();
					folder = def.parent_path();
					name = def.empty() ? std::filesystem::path("ReSaverHelper-report.txt") : def.filename();
				}
				if (folder.empty()) {
					folder = a_send ? LiveInfo::LogFolder() : SaveDialog::Desktop();
				}
				const bool started = SaveDialog::SaveText(folder, name, std::move(text), a_send ? "what to send" : "the report",
					[a_send](const SaveDialog::Result& a_r) {
						if (!a_r.path.empty()) {
							std::scoped_lock l(g.lock);
							(a_send ? g.sent : g.report) = a_r.path;
						}
						SetStatus(a_r.message);
					});
				if (!started) {
					SetStatus("A Save As dialog is already open (if you do not see it, it may be behind the game: press Alt+Tab).");
				}
			});
		}

		void DialogNote()
		{
			if (SaveDialog::Busy()) {
				Colored(kYellow, "The Save As dialog is open. If you do not see it, it may be behind the game: press Alt+Tab.");
			}
		}

		void __stdcall RenderReport()
		{
			StatusLine();
			Wrapped("Writes a text file with what is useful to send when asking for help: game and helper versions, where you are, "
					"characters around you with problems, your followers, script health, plugin counts.");
			const auto def = LiveInfo::DefaultReportPath();
			im::BeginDisabled(SaveDialog::Busy());
			if (im::Button("Write the report...")) {
				SaveReport(false);
			}
			im::EndDisabled();
			Tip("Asks where to save it (the Windows Save As dialog), starting in the SKSE log folder as ReSaverHelper-report.txt. "
				"Open Troubleshooting > Script health first if you want script numbers in it.");
			DialogNote();
			std::filesystem::path report;
			{
				std::scoped_lock l(g.lock);
				report = g.report;
			}
			if (!report.empty()) {
				Line("File: " + SaveDialog::PathText(report));
				OpenButton(report);
			} else {
				Dim("Proposed: " + SaveDialog::PathText(def));
			}
			Dim("In exclusive fullscreen no dialog can be shown: the report then goes to the proposed place, and this page says so.");
			DrawAsk();
		}

		// ------------------------------------------------------------------ 5b. Troubleshooting: ghost actors (GhostRemove.h)
		// The live game cannot tell a ghost actor reliably, so the ids come from ReSaver Plus's Save check (typed or pasted).
		char g_ghostText[2048] = "";
		struct GhostRow
		{
			RE::FormID  id = 0;
			bool        tick = true;
			std::string result;  // "" not done yet
		};
		std::vector<GhostRow> g_ghosts;  // drawn and changed on the main thread only (render callback and SKSE tasks)
		bool                  g_ghostRisk = false;
		std::atomic<bool>     g_ghostBusy{ false };

		void __stdcall RenderGhosts()
		{
			StatusLine();
			if (!NeedGame()) {
				DrawAsk();
				return;
			}
			Wrapped("Ghost actors are spawned characters whose base NPC is a temporary copy the save does not contain (Ultimate Deadly "
					"Encounters' clone spawns make them). On load they get no base or another character's base, which can crash the "
					"game. ReSaver Plus finds them in a save: its Save check lists their ids. Paste or type them here.");
			im::SetNextItemWidth(-FLT_MIN);
			im::InputTextMultiline("##ghostids", g_ghostText, sizeof(g_ghostText), im::ImVec2(0, im::GetFontSize() * 4.0f));
			Tip("Ids in hex, like FF003D2A, separated by commas, spaces or new lines.");
			if (im::Button("Read the ids")) {
				std::vector<GhostRow> rows;
				for (const auto id : proto::ParseIdList(g_ghostText)) {
					rows.push_back({ id, (id >> 24) == 0xFF, {} });
				}
				g_ghosts = std::move(rows);
				SetStatus(std::format("{} id(s) read.", g_ghosts.size()));
			}
			if (g_ghosts.empty()) {
				DrawAsk();
				return;
			}
			im::Spacing();
			if (im::SmallButton("Select all")) {
				for (auto& r : g_ghosts) {
					r.tick = (r.id >> 24) == 0xFF;
				}
			}
			im::SameLine();
			if (im::SmallButton("Select none")) {
				for (auto& r : g_ghosts) {
					r.tick = false;
				}
			}
			if (im::BeginTable("ghosts", 3, kListTable | im::ImGuiTableFlags_ScrollY, TableHeight(g_ghosts.size() + 1, 14))) {
				im::TableSetupScrollFreeze(0, 1);
				im::TableSetupColumn("Remove", im::ImGuiTableColumnFlags_WidthStretch | im::ImGuiTableColumnFlags_NoSort, 0.7f);
				im::TableSetupColumn("Form ID", kNameColumn, 1.2f);
				im::TableSetupColumn("Result", kTextColumn, 3.0f);
				const auto sort = ReadSort();
				im::TableHeadersRow();
				std::vector<std::size_t> rows(g_ghosts.size());
				for (std::size_t i = 0; i < rows.size(); ++i) {
					rows[i] = i;
				}
				Order(rows, sort, [&](std::size_t a, std::size_t b) {
					const auto& x = g_ghosts[a];
					const auto& y = g_ghosts[b];
					const int   c = sort.column == 2 ? CompareI(x.result, y.result) : 0;
					return c ? c : Compare3(x.id, y.id);
				});
				for (const auto i : rows) {
					auto& r = g_ghosts[i];
					im::PushID(static_cast<int>(r.id));
					im::TableNextRow();
					im::TableNextColumn();
					const bool created = (r.id >> 24) == 0xFF;
					im::BeginDisabled(!created);
					im::Checkbox("##tick", &r.tick);
					im::EndDisabled();
					im::TableNextColumn();
					Line(Hex(r.id));
					im::TableNextColumn();
					if (!created) {
						r.tick = false;
						Dim("not a created (FF) id: left alone");
					} else if (!r.result.empty()) {
						Colored(r.result == "done" ? kGreen : kYellow, r.result);
					} else {
						Dim(r.tick ? "to remove" : "left alone");
					}
					im::PopID();
				}
				im::EndTable();
			}
			im::Spacing();
			Colored(kRed, "Risk: touching a broken character can crash the game.");
			Wrapped("Make a save first. If the game crashes: load that save, open ReSaverHelper.log (the last lines name the id it was on), "
					"and leave that one out next time. If it works: save the game as a new save and open it in ReSaver Plus to check.");
			im::Checkbox("I understand the risk", &g_ghostRisk);
			std::size_t n = 0;
			for (const auto& r : g_ghosts) {
				n += r.tick;
			}
			im::BeginDisabled(!g_ghostRisk || n == 0 || g_ghostBusy);
			if (im::Button(std::format("Remove the selected ({})", n).c_str())) {
				std::vector<RE::FormID> ids;
				for (const auto& r : g_ghosts) {
					if (r.tick) {
						ids.push_back(r.id);
					}
				}
				AskFirst(std::format("Disable and mark for delete {} character(s)?\n\nThe console's disable and markfordelete on each. Make "
									 "a save first: touching a broken character can crash the game.",
							 ids.size()),
					[ids] {
						g_ghostBusy = true;
						SetStatus("Removing (close the menu).");
						for (std::size_t i = 0; i < ids.size(); ++i) {
							Queue([id = ids[i], last = i + 1 == ids.size()] {
								const auto word = std::string(GhostRemove::Word(GhostRemove::Remove(id)));
								std::size_t done = 0, total = 0;
								for (auto& r : g_ghosts) {
									if (r.id == id) {
										r.result = word;
										r.tick = false;
									}
									total += !r.result.empty();
									done += r.result == "done";
								}
								if (last) {
									g_ghostBusy = false;
									SetStatus(std::format("{} of {} done. Save the game now (a new save), then open it in ReSaver Plus.", done, total));
								}
							});
						}
					});
			}
			im::EndDisabled();
			Tip("For each ticked id: the console's disable, then markfordelete, one at a time; each step is written to ReSaverHelper.log first.");
			DrawAsk();
		}

		// ------------------------------------------------------------------ 6. Troubleshooting tests
		void __stdcall RenderStages()
		{
			StatusLine();
			if (Due(g_iniAt, 2000ms)) {
				g.iniStage = Settings::DiagnosticStageInIni();
			}
			const int ini = g.iniStage;
			Wrapped("To find out whether something in your game changes only because ReSaverHelper is installed. Each stage stops the "
					"helper after one more step of what it does when the game starts.");
			Line("This game started at: normal (no stage).");
			Tip("This page only appears at the normal stage: during a test (stages 0-4) the helper does not add it.");
			Line(ini < 0 ? std::string("Next start: normal.") : std::format("Next start: stage {}.", ini));
			im::Spacing();
			static constexpr const char* kStage[5] = {
				"0: nothing at all. The DLL is loaded and does nothing (not even its log).",
				"1: + SKSE's setup, the log file and the settings.",
				"2: + listening to SKSE's messages (it ignores them all).",
				"3: + following game loads and new games (no threads).",
				"4: + the ReSaver Plus connection (no Ctrl hotkey).",
			};
			for (int i = 0; i < 5; ++i) {
				im::PushID(i);
				if (im::SmallButton(std::format("Use stage {}", i).c_str())) {
					const bool ok = Settings::WriteDiagnosticStage(i);
					g_iniAt = {};
					SetStatus(ok ? std::format("Stage {} written to ReSaverHelper.ini. Restart the game to test it.", i) :
					               "Could not write " + Settings::IniPath().string());
				}
				im::SameLine();
				Line(kStage[i]);
				im::PopID();
			}
			if (im::SmallButton("Back to normal")) {
				const bool ok = Settings::WriteDiagnosticStage(-1);
				g_iniAt = {};
				SetStatus(ok ? std::string("The stage line is removed: the next start is normal.") : "Could not write " + Settings::IniPath().string());
			}
			Tip("Removes iDiagnosticStage from ReSaverHelper.ini. During a test this page is not there: remove the line by hand "
				"(Data\\SKSE\\Plugins\\ReSaverHelper.ini, or in your mod manager's ReSaverHelper folder).");
			Dim("Takes effect after restarting the game. ReSaver Plus cannot connect below stage 4.");
			im::Spacing();
			im::SeparatorText("How to test");
			Wrapped("1. Use the same save every time, at the same place.");
			Wrapped("2. Set stage 0 and restart the game. Load the save, spawn a creature and kill it (Test spawns), and watch what "
					"happens (for example, how the body falls).");
			Wrapped("3. Then stage 1, 2, 3, 4, restarting the game each time, and the same test.");
			Wrapped("4. Note the FIRST stage where the problem appears, and send it with ReSaverHelper.log (see What to send).");
			Wrapped("5. Set it back to normal when you are done (remove the line from ReSaverHelper.ini).");
			DrawAsk();
		}

		void __stdcall RenderSpawns()
		{
			StatusLine();
			if (!NeedGame()) {
				DrawAsk();
				return;
			}
			Wrapped("For the ragdoll test: a plain vanilla creature in front of you, then kill it and watch how the body falls.");
			static constexpr const char* kLabel[3] = { "Spawn a wolf", "Spawn a deer", "Spawn a sabre cat" };
			static constexpr const char* kTip[3] = { "EncWolf (00023ABE in Skyrim.esm)", "EncDeer (000CF89D in Skyrim.esm)",
				"EncSabreCat (00023AB5 in Skyrim.esm)" };
			static constexpr const char* kName[3] = { "wolf", "deer", "sabre cat" };
			for (int i = 0; i < 3; ++i) {
				if (i) {
					im::SameLine();
				}
				if (im::Button(kLabel[i])) {
					Queue([i] {
						const auto why = LiveInfo::Spawn(i, [i](RE::FormID a_ref) {
							{
								std::scoped_lock l(g.lock);
								g.spawned = a_ref;
								g.spawnedName = kName[i];
							}
							SetStatus(a_ref ? std::format("A {} is in front of you ({}).", kName[i], Hex(a_ref)) : std::string("The game did not place it."));
						});
						SetStatus(why.empty() ? std::format("Placing a {} (close the menu).", kName[i]) : why);
					});
				}
				Tip(std::string(kTip[i]) + ": Papyrus PlaceAtMe on you, then MoveTo 300 units in front of you.");
			}
			RE::FormID  spawned = 0;
			std::string spawnedName;
			{
				std::scoped_lock l(g.lock);
				spawned = g.spawned;
				spawnedName = g.spawnedName;
			}
			im::BeginDisabled(!spawned);
			if (im::Button(spawned ? ("Kill the " + spawnedName).c_str() : "Kill it")) {
				Queue([spawned, spawnedName] {
					const auto why = LiveInfo::Kill(spawned);
					SetStatus(why.empty() ? "The " + spawnedName + " dies when you close the menu." : why);
				});
			}
			im::EndDisabled();
			Tip("Actor.Kill() on the creature you spawned last. Close the menu to watch it fall.");
			im::SameLine();
			if (im::Button("Kill the one I look at")) {
				Queue([] {
					std::string name;
					const auto  id = LiveInfo::TargetActor(name);
					if (!id) {
						SetStatus("Nobody under your crosshair (or selected in the console).");
						return;
					}
					AskFirst("Kill " + name + " (" + Hex(id) + ")?\n\nActor.Kill(). If it is someone a quest needs, the quest may break.", [id, name] {
						Queue([id, name] {
							const auto why = LiveInfo::Kill(id);
							SetStatus(why.empty() ? name + " dies when you close the menu." : why);
						});
					});
				});
			}
			Tip("The character under your crosshair when you opened this menu, or else the one selected in the console (click it with "
				"the console open). Asks first.");
			DrawAsk();
		}

		void __stdcall RenderChecks()
		{
			StatusLine();
			const auto& s = Settings::Get();
			Line(std::format("ReSaverHelper version {}", proto::kHelperVersion));
			Line(std::format("Game: Skyrim {} {}", REL::Module::IsVR() ? "VR" : REL::Module::IsAE() ? "AE" : "SE", REL::Module::get().version().string()));
			Line(std::format("SKSE Menu Framework: version {}", SKSEMenuFramework::GetMenuFrameworkVersion()));
			im::Spacing();
			if (!s.listen) {
				Colored(kYellow, "ReSaver Plus connection: switched off (bListen=0)");
			} else if (Server::Connected()) {
				Colored(kGreen, "ReSaver Plus connection: connected");
			} else {
				Line("ReSaver Plus connection: waiting (ReSaver Plus is not connected)");
			}
			Tip("ReSaver Plus connects by itself when \"Connect to the running game\" is on in its settings. bListen=0 in "
				"ReSaverHelper.ini switches the connection off.");
			Line(std::format("Ctrl hold rescue: {}", s.rescueHotkey ? std::format("on (hold Ctrl {} s)", s.rescueHoldSeconds) : std::string("off (bRescueHotkey=0)")));
			Line(std::format("Messages: {}", s.notify == "box" ? "message box" : s.notify == "none" ? "none" : "top-left notification"));
			Line(std::string("Journal refresh after loading: ") + (s.refreshOnLoad ? "on" : "off"));
			im::Spacing();
			// version 16: menus whose interface file did not load (MenuGuard.h)
			im::SeparatorText("Menu load problems");
			Line("Menu load guard: " + MenuGuard::StatusLine());
			Tip("When a menu's interface file (Interface\\<name>.swf) does not load, the helper tries again; if it still fails, the "
				"menu does not open (with a notice) instead of crashing the game. bMenuLoadGuard / iMenuLoadRetries in ReSaverHelper.ini.");
			if (const auto probs = MenuGuard::Problems(); probs.empty()) {
				Dim("None since the game started.");
			} else {
				for (const auto& p : probs) {
					Colored(p.closed > 0 && !p.test ? kRed : kYellow,
						std::format("{}{}: failed {}, worked on retry {}, menu not opened {} (last at {})", p.movie, p.test ? " (TEST)" : "", p.failed,
							p.recovered, p.closed, p.lastTime));
					Tip(p.lastStep + (p.lastHow.empty() ? "" : "\nWhat the guard did: " + p.lastHow) + "\nDetails (files, memory, the last 50 loads) are in ReSaverHelper.log. Please send that log (Troubleshooting "
										 "tests > What to send).");
				}
			}
			if (!s.testFailMovies.empty()) {
				Colored(kYellow, "TEST: sTestFailMovies=" + s.testFailMovies + " (sTestFailMode=" + s.testFailMode + ") makes these loads fail on purpose.");
			}
			im::Spacing();
			im::SeparatorText("Where the logs are");
			const auto dir = LiveInfo::LogFolder();
			Line("Helper log: " + (dir / "ReSaverHelper.log").string());
			Line("SKSE log: " + (dir / "skse64.log").string());
			Line("Papyrus log: " + LiveInfo::PapyrusLog().string());
			Tip("Written only when bEnableLogging=1 is set under [Papyrus] in SkyrimCustom.ini (or Skyrim.ini).");
			Line("Settings: " + Settings::IniPath().string());
			DrawAsk();
		}

		// a_reportBelow: for the saved file, which has the report after the list
		std::string SendText(bool a_reportBelow)
		{
			const auto            dir = LiveInfo::LogFolder();
			std::filesystem::path report;
			{
				std::scoped_lock l(g.lock);
				report = g.report;
			}
			if (report.empty()) {
				report = LiveInfo::DefaultReportPath();
			}
			std::string t;
			t += "Please attach these files:\n";
			t += "- " + SaveDialog::PathText(dir / "ReSaverHelper.log") + "\n";
			t += a_reportBelow ? std::string("- the report: it is in this file, below the list\n") :
			                     "- " + SaveDialog::PathText(report) + " (Troubleshooting > Write a report), or save this list with the report: Save...\n";
			t += "- " + SaveDialog::PathText(dir / "skse64.log") + "\n";
			t += "- " + SaveDialog::PathText(LiveInfo::PapyrusLog()) + " (if there is one)\n";
			t += "- the newest crash log, if the game crashed (in " + SaveDialog::PathText(dir) + ", or wherever your crash logger writes)\n";
			t += "- the save where it happens (.ess and its .skse file)\n";
			t += std::format("ReSaverHelper {}, Skyrim {} {}\n", proto::kHelperVersion, REL::Module::IsVR() ? "VR" : REL::Module::IsAE() ? "AE" : "SE",
				REL::Module::get().version().string());
			return t;
		}

		void __stdcall RenderSend()
		{
			StatusLine();
			const auto t = SendText(false);
			Wrapped(t);
			if (im::Button("Copy")) {
				im::SetClipboardText(t.c_str());
				SetStatus("Copied: paste it in your message.");
			}
			Tip("Copies the list above, with the full paths, to the clipboard.");
			im::SameLine();
			im::BeginDisabled(SaveDialog::Busy());
			if (im::Button("Save...")) {
				SaveReport(true);
			}
			im::EndDisabled();
			Tip("Saves one text file with this list and the report (as Troubleshooting > Write a report makes it). Asks where "
				"(the Windows Save As dialog), starting on your Desktop as ReSaverHelper-report-<date>.txt. Attach that file and "
				"the logs listed above.");
			DialogNote();
			std::filesystem::path sent;
			{
				std::scoped_lock l(g.lock);
				sent = g.sent;
			}
			if (!sent.empty()) {
				Line("File: " + SaveDialog::PathText(sent));
				OpenButton(sent);
			}
			Dim("In exclusive fullscreen no dialog can be shown: the file then goes to your Desktop, and this page says so.");
			DrawAsk();
		}

		// ------------------------------------------------------------------ 7. About
		void __stdcall RenderAbout()
		{
			StatusLine();
			Line(std::format("ReSaverHelper version {}: the SKSE plugin of ReSaver Plus.", proto::kHelperVersion));
			Line("Nexus: nexusmods.com/skyrimspecialedition/mods/196293");
			im::Spacing();
			Wrapped("This page does in the game what ReSaver Plus does through the helper, without ReSaver Plus running: rescue a stuck "
					"game, look at and remove the magic effects on your character, manage your followers, fix a stuck quest, watch the "
					"script engine and write a report, and run the tests we ask for when something goes wrong.");
			Wrapped("Every action uses the game's own functions (the same ones scripts use) and is written to ReSaverHelper.log.");
			Wrapped("English only: the helper has no translations.");
			Dim("This page needs SKSE Menu Framework. Without it the helper works as before.");
			DrawAsk();
		}
	}

	void Register()
	{
		if (!GetModuleHandleW(L"SKSEMenuFramework")) {
			spdlog::info("F1 page: SKSE Menu Framework is not loaded; no in-game page (the helper works as before)");
			return;
		}
		SKSEMenuFramework::SetSection("ReSaver Plus");
		SKSEMenuFramework::AddSectionItem("Rescue", RenderRescue);
		SKSEMenuFramework::AddSectionItem("My character", RenderEffects);
		SKSEMenuFramework::AddSectionItem("Followers", RenderFollowers);
		SKSEMenuFramework::AddSectionItem("Quests", RenderQuests);
		SKSEMenuFramework::AddSectionItem("Troubleshooting/Script health", RenderHealth);
		SKSEMenuFramework::AddSectionItem("Troubleshooting/Write a report", RenderReport);
		SKSEMenuFramework::AddSectionItem("Troubleshooting/Ghost actors", RenderGhosts);
		SKSEMenuFramework::AddSectionItem("Troubleshooting tests/Diagnostic stages", RenderStages);
		SKSEMenuFramework::AddSectionItem("Troubleshooting tests/Test spawns", RenderSpawns);
		SKSEMenuFramework::AddSectionItem("Troubleshooting tests/Helper and connection", RenderChecks);
		SKSEMenuFramework::AddSectionItem("Troubleshooting tests/What to send", RenderSend);
		SKSEMenuFramework::AddSectionItem("About", RenderAbout);
		spdlog::info("F1 page: added to SKSE Menu Framework (version {}, API {})", SKSEMenuFramework::GetMenuFrameworkVersion(),
			SKSEMenuFramework::GetMenuFrameworkAPIVersion());
	}
}
