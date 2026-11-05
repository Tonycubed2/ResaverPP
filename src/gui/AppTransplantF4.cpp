// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant page for Fallout 4. The old (broken) save and the good one (the save open in ReSaver Plus) are both
// read from their files, without the game. What the old save has on top is listed with ticks, like the Skyrim
// page. What a save can hold (level, experience, S.P.E.C.I.A.L., caps, perk ranks) is written into the open save;
// the rest (items, new perks, quest stages) is put into the running game by the Fallout 4 ReSaverHelper (13+):
// the game makes a save first, its load order gives the form ids, then one APPLY record per change.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

using namespace ess;
namespace tf4 = ess::transplant::fo4;

namespace
{
	constexpr int kTransplantHelper = 13;  // the Fallout 4 helper with APPLY

	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextUnformatted(s.c_str());
		ImGui::PopStyleColor();
	}

	void Wrapped(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	void Dot(std::uint32_t col)
	{
		const float sz = ImGui::GetTextLineHeight();
		const auto  p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + sz * 0.5f, p.y + sz * 0.55f), sz * 0.33f, col);
		ImGui::Dummy(ImVec2(sz, sz));
		ImGui::SameLine();
	}

	std::uint32_t RiskCol(int r) { return r == 0 ? theme::Good() : r == 1 ? theme::Caution() : theme::Bad(); }

	std::string Summary(const tf4::Snapshot& s)
	{
		if (!s.valid) {
			return s.error.empty() ? "not chosen yet" : s.error;
		}
		std::size_t items = 0, done = 0;
		for (const auto& [k, i] : s.items) {
			items += static_cast<std::size_t>(std::max<std::int64_t>(0, i.count));
		}
		for (const auto& [k, q] : s.quests) {
			done += q.completed ? 1 : 0;
		}
		return Fmt("%s  level %d, %s caps, %zu perks, %zu items, %zu quests completed", s.player.c_str(), s.level,
			s.caps >= 0 ? std::to_string(s.caps).c_str() : "?", s.perks.size(), items, done);
	}

	const char* CategoryOf(tf4::Kind k)
	{
		switch (k) {
		case tf4::Kind::kPerk: return "Perks";
		case tf4::Kind::kItem: return "Items";
		case tf4::Kind::kQuest: return "Quests";
		default: return "Character: level, experience, S.P.E.C.I.A.L., caps";
		}
	}
	int CategoryIndex(tf4::Kind k) { return k == tf4::Kind::kPerk ? 1 : k == tf4::Kind::kItem ? 2 : k == tf4::Kind::kQuest ? 3 : 0; }
}

// the old save, read from its file
void App::transplantF4Load(const std::filesystem::path& a_path)
{
	try {
		auto s = SaveFile::load(a_path);
		_tf4Old = tf4::FromSave(*s, platform::ToUtf8(a_path.filename()));
		_tf4OldPath = a_path;
		_tf4Dirty = true;
		_status = _tf4Old.valid ? "Read the old save " + platform::ToUtf8(a_path.filename()) : _tf4Old.error;
	} catch (const std::exception& e) {
		_tf4Old = {};
		_tf4Old.error = "Could not read " + platform::ToUtf8(a_path.filename()) + ": " + e.what();
		_status = _tf4Old.error;
	}
}

// a form's name from the plugins (the open save's load order: the good save's)
std::string App::transplantF4Name(const tf4::Change& a_c) const
{
	if (!_db || !a_c.goodFormID) {
		return {};
	}
	if (a_c.kind == tf4::Kind::kQuest) {
		if (const auto* d = _db->quest(a_c.goodFormID)) {
			return !d->name.empty() ? d->name : d->edid;
		}
	}
	const auto* f = _db->find(a_c.goodFormID);
	if (!f) {
		return {};
	}
	auto n = _db->name(*f);
	return n.empty() ? _db->edid(*f) : n;
}

std::string App::transplantF4Group(const tf4::Change& a_c) const
{
	if (a_c.kind == tf4::Kind::kItem && _db && a_c.goodFormID) {
		if (const auto* f = _db->find(a_c.goodFormID)) {
			const auto t = _db->type(*f);
			return t == "WEAP" ? "Weapons" : t == "ARMO" ? "Armor and clothing" : t == "AMMO" ? "Ammunition" : t == "ALCH" ? "Aid: food, drink, chems"
				   : t == "BOOK" ? "Books and magazines" : t == "MISC" ? "Junk and miscellaneous" : t == "KEYM" ? "Keys" : t == "NOTE" ? "Holotapes and notes"
				   : t == "CMPO" ? "Components" : "Other";
		}
		return "Other";
	}
	if (a_c.kind == tf4::Kind::kQuest && !a_c.journal) {
		return "~ Background quests (not in the Pip-Boy): normally leave these unticked";
	}
	return a_c.kind == tf4::Kind::kLevel || a_c.kind == tf4::Kind::kXP || a_c.kind == tf4::Kind::kSpecial || a_c.kind == tf4::Kind::kCaps ? std::string() : a_c.plugin;
}

// ---- into the running game -----------------------------------------------------------------------

// step 1: the game saves (a save to go back to), and that save's plugin list is the game's load order
void App::transplantF4StartGame(std::vector<std::size_t> a_run)
{
	_tf4Run = std::move(a_run);
	_tf4Pos = 0;
	_tf4Running = true;
	_tf4GameNote.clear();
	for (auto i : _tf4Run) {
		_tf4Results.erase(i);
	}
	_status = "Transplant: the game saves first (a save to go back to; its load order gives the form ids)...";
	_live.request({ "SAVEGAME", "none" }, [this](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_tf4Running = false;
			_status = "The game could not save: " + LiveLink::Error(r);
			return;
		}
		_tf4WaitSave = LiveLink::Field(r, 0) + ".fos";
		_tf4WaitStart = ImGui::GetTime();
		_tf4WaitSize = -1;
	});
}

// each frame while waiting: the save the game made, read when it is complete
void App::transplantF4PollSave()
{
	if (_tf4WaitSave.empty()) {
		return;
	}
	std::vector<std::filesystem::path> dirs = watchFolders();
	dirs.push_back(savesFolder());
	if (!_path.empty()) {
		dirs.push_back(_path.parent_path());
	}
	std::error_code ec;
	for (const auto& d : dirs) {
		const auto p = d / platform::FromUtf8(_tf4WaitSave);
		if (!std::filesystem::is_regular_file(p, ec)) {
			continue;
		}
		const auto size = static_cast<std::int64_t>(std::filesystem::file_size(p, ec));
		if (size != _tf4WaitSize) {  // still being written: wait for the size to stay the same for a frame or two
			_tf4WaitSize = size;
			return;
		}
		try {
			auto s = SaveFile::load(p);
			_tf4GameOrder = s->plugins;
			_tf4WaitSave.clear();
			_tf4GameNote = _save && tf4::SameLoadOrder(_tf4GameOrder, _save->plugins)
							   ? "The game's load order is the good save's."
							   : "The game's load order is not the good save's: the form ids are worked out from the game's.";
			_tf4GameNote += " The game saved as " + platform::ToUtf8(p.filename()) + " first (load it to go back).";
			transplantF4Step();
		} catch (const std::exception&) {
			// not complete yet: try again next frame
		}
		return;
	}
	if (ImGui::GetTime() - _tf4WaitStart > 45.0) {
		_tf4WaitSave.clear();
		_tf4Running = false;
		_tf4NeedOrder = true;
		_status = "The save the game made was not found in the saves folders ReSaver Plus knows, so the game's load order could not be "
				  "checked. Choose the saves folder in Settings, or put them in with the good save's load order.";
	}
}

void App::transplantF4Step()
{
	if (_tf4Pos >= _tf4Run.size()) {
		std::size_t failed = 0;
		for (auto i : _tf4Run) {
			if (auto it = _tf4Results.find(i); it != _tf4Results.end() && !it->second.first) {
				++failed;
			}
		}
		_tf4Running = false;
		_status = Fmt("Transplant: %zu of %zu sent to the game%s. Quests may keep running their scripts for a while; then save in the game, open "
					  "that save here and compare again: anything still listed did not take.",
			_tf4Run.size() - failed, _tf4Run.size(), failed ? Fmt(", %zu could not be (see the list)", failed).c_str() : "");
		_live.request({ "NOTIFY", liveNotifyMode(), Fmt("ReSaver Plus: transplant done (%zu of %zu). Save when the quests are quiet.", _tf4Run.size() - failed, _tf4Run.size()) },
			[](const LiveLink::Reply&) {});
		return;
	}
	const auto idx = _tf4Run[_tf4Pos];
	if (idx >= _tf4Changes.size()) {
		++_tf4Pos;
		transplantF4Step();
		return;
	}
	const auto& ch = _tf4Changes[idx];
	_status = Fmt("Transplanting %zu of %zu: %s", _tf4Pos + 1, _tf4Run.size(), ch.label.c_str());
	const auto& order = _tf4GameOrder.full.empty() && _save ? _save->plugins : _tf4GameOrder;
	const auto  id = ch.kind == tf4::Kind::kCaps ? ch.goodFormID : tf4::RuntimeID(order, ch.plugin, ch.local);
	auto        records = tf4::GameRecords(ch, id, _tf4MarkCompleted);
	if (!id || records.empty()) {
		_tf4Results[idx] = { false, !id ? ch.plugin + " is not in the game's load order" : "nothing to do" };
		++_tf4Pos;
		transplantF4Step();
		return;
	}
	transplantF4Send(idx, std::move(records), {});
}

// the records of one change one after the other (a quest: setstage, then completequest)
void App::transplantF4Send(std::size_t a_idx, std::vector<ess::transplant::Record> a_rest, std::string a_said)
{
	if (a_rest.empty()) {
		_tf4Results[a_idx] = { true, "sent: " + a_said };
		++_tf4Pos;
		transplantF4Step();
		return;
	}
	std::vector<std::string> cmd{ "APPLY" };
	cmd.insert(cmd.end(), a_rest.front().begin(), a_rest.front().end());
	a_rest.erase(a_rest.begin());
	_live.request(cmd, [this, a_idx, rest = std::move(a_rest), said = std::move(a_said)](const LiveLink::Reply& r) mutable {
		if (!LiveLink::Ok(r) || LiveLink::Field(r, 0) == "0") {
			_tf4Results[a_idx] = { false, !LiveLink::Ok(r) ? LiveLink::Error(r) : LiveLink::Field(r, 1) };
			++_tf4Pos;
			transplantF4Step();
			return;
		}
		said += (said.empty() ? "" : "; ") + LiveLink::Field(r, 1);
		transplantF4Send(a_idx, std::move(rest), std::move(said));
	});
}

// ---- the window -----------------------------------------------------------------------------------

void App::drawTransplantF4()
{
	prepareWindow("Transplant", ImVec2(0.9f, 0.9f));
	if (!ImGui::Begin("Transplant", &_showTransplant, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Transplant");
	transplantF4PollSave();
	const auto info = _live.info();
	const bool live = info.connected && info.inGame && liveFO4();
	const bool helperOk = live && helperAtLeast(kTransplantHelper);
	const bool busy = _tf4Running || !_tf4WaitSave.empty();

	ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 140);
	ImGui::TextWrapped("Bring your character's progress from an old save (one that crashes or will not load) into a good one: the save open in "
					   "ReSaver Plus. Both are read from their files, so the old save never has to load. What a save can hold (level, "
					   "experience, S.P.E.C.I.A.L., caps, perk ranks) is written into the open save; items, new perks and quest stages are "
					   "put into the running game by ReSaverHelper %d or newer (the F4SE plugin) once the good save is loaded.",
		kTransplantHelper);
	ImGui::PopTextWrapPos();

	if (_save && (_save.get() != _tf4GoodOf || _tf4GoodChanged)) {
		_tf4Good = tf4::FromSave(*_save, platform::ToUtf8(_path.filename()));
		_tf4GoodOf = _save.get();
		_tf4GoodChanged = false;
		_tf4Dirty = true;
	}
	if (_tf4Dirty && _save && _tf4Old.valid && _tf4Good.valid) {
		_tf4Changes = tf4::Diff(_tf4Good, _tf4Old, *_save);
		std::stable_sort(_tf4Changes.begin(), _tf4Changes.end(), [](const tf4::Change& a, const tf4::Change& b) {
			return CategoryIndex(a.kind) != CategoryIndex(b.kind) ? CategoryIndex(a.kind) < CategoryIndex(b.kind)
				   : a.kind == tf4::Kind::kQuest && b.kind == tf4::Kind::kQuest ? a.order < b.order : false;
		});
		for (auto& c : _tf4Changes) {
			if (const auto n = transplantF4Name(c); !n.empty() && (c.kind == tf4::Kind::kPerk || c.kind == tf4::Kind::kItem || c.kind == tf4::Kind::kQuest)) {
				c.detail = c.label.substr(0, 8) + "  -  " + c.plugin;
				c.label = n + c.label.substr(8);
			} else {
				// no name (the plugins are not read): say what kind of thing it is
				c.label = (c.kind == tf4::Kind::kPerk ? "Perk " : c.kind == tf4::Kind::kItem ? "Item " : c.kind == tf4::Kind::kQuest ? "Quest " : "") + c.label;
			}
		}
		_tf4Results.clear();
		_tf4Dirty = false;
	}

	// steps 1 and 2 side by side
	if (ImGui::BeginTable("steps", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		ImGui::SeparatorText("1. The old save: where the progress is");
		ImGui::TextWrapped("%s", Summary(_tf4Old).c_str());
		if (_tf4Old.valid) {
			ImGui::TextDisabled("%s", _tf4Old.source.c_str());
		}
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("Choose the old save...") && platform::HasNativeDialogs()) {
			if (auto p = platform::OpenSaveDialog(_path.empty() ? savesFolder() : _path.parent_path())) {
				transplantF4Load(*p);
			}
		}
		tips::Tip("The save with the progress you want back: it may crash or not load at all, it is only read from its file.");
		// the other saves of the same folder, newest first
		if (!_path.empty()) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::BeginCombo("##tf4saves", "or pick one from this folder")) {
				std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> saves;
				std::error_code ec;
				for (const auto& e : std::filesystem::directory_iterator(_path.parent_path(), ec)) {
					if (e.is_regular_file(ec) && Lower(platform::ToUtf8(e.path().extension())) == ".fos" && e.path() != _path) {
						saves.emplace_back(e.last_write_time(ec), e.path());
					}
				}
				std::sort(saves.begin(), saves.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				for (std::size_t i = 0; i < saves.size() && i < 60; ++i) {
					if (ImGui::Selectable(platform::ToUtf8(saves[i].second.filename()).c_str())) {
						transplantF4Load(saves[i].second);
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::EndDisabled();
		if (_tf4Old.valid && !_tf4Old.missingParts.empty()) {
			std::string parts;
			for (const auto& m : _tf4Old.missingParts) {
				parts += (parts.empty() ? "" : ", ") + m;
			}
			Wrapped(theme::Caution(), "Could not be read from this save: " + parts + ".");
		}
		if (_tf4Old.valid && _tf4Old.skippedItems) {
			ImGui::TextDisabled("%zu item stack(s) made in the game (no plugin) are left out.", _tf4Old.skippedItems);
		}

		ImGui::TableNextColumn();
		ImGui::SeparatorText("2. The good save: the one open in ReSaver Plus");
		ImGui::TextWrapped("%s", _save ? Summary(_tf4Good).c_str() : "Open the good save first (File > Open).");
		if (_save) {
			ImGui::TextDisabled("%s", platform::ToUtf8(_path.filename()).c_str());
		}
		if (_tf4Old.valid && _tf4Good.valid && _tf4Old.player != _tf4Good.player) {
			Wrapped(theme::Bad(), "These are two different characters (" + _tf4Old.player + " / " + _tf4Good.player + ").");
		}
		if (_tf4Good.valid && !_tf4Good.missingParts.empty()) {
			std::string parts;
			for (const auto& m : _tf4Good.missingParts) {
				parts += (parts.empty() ? "" : ", ") + m;
			}
			Wrapped(theme::Caution(), "Could not be read from this save: " + parts + ".");
		}
		ImGui::BeginDisabled(busy || !_save);
		if (ImGui::SmallButton("Compare again")) {
			_tf4GoodChanged = true;
			if (!_tf4OldPath.empty()) {
				transplantF4Load(_tf4OldPath);
			}
		}
		ImGui::EndDisabled();
		tips::Tip("Read both saves again and list the differences (e.g. after a Transplant: open the save the game made afterwards and compare).");
		ImGui::EndTable();
	}

	if (!_tf4Old.valid || !_tf4Good.valid) {
		ImGui::End();
		return;
	}

	// 3. the list
	ImGui::SeparatorText(Fmt("3. What the old save has on top (%zu)", _tf4Changes.size()).c_str());
	Dot(theme::Good());
	ImGui::TextUnformatted("Safe");
	ImGui::SameLine(0, 18);
	Dot(theme::Caution());
	ImGui::TextUnformatted("Caution (hover to read why)");
	ImGui::SameLine(0, 18);
	Dot(theme::Bad());
	ImGui::TextUnformatted("Risky or cannot");
	ImGui::SameLine(0, 30);
	TextColored(theme::Good(), "[save file]");
	ImGui::SameLine();
	ImGui::TextDisabled("written into the open save");
	ImGui::SameLine(0, 18);
	TextColored(theme::Caution(), "[game]");
	ImGui::SameLine();
	ImGui::TextDisabled("put into the running game");
	ImGui::SameLine(0, 30);
	ImGui::BeginDisabled(busy);
	if (ImGui::SmallButton("Tick everything")) {
		for (auto& c : _tf4Changes) {
			c.tick = c.available;
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Untick everything")) {
		for (auto& c : _tf4Changes) {
			c.tick = false;
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##tf4filter", "search", _tf4Filter, sizeof(_tf4Filter));
	const auto needle = Lower(_tf4Filter);

	auto row = [&](std::size_t i) {
		auto& c = _tf4Changes[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(!c.available || busy);
		ImGui::Checkbox("##t", &c.tick);
		ImGui::EndDisabled();
		ImGui::SameLine();
		Dot(RiskCol(c.risk));
		TextColored(c.via == tf4::Via::kFile ? theme::Good() : theme::Caution(), c.via == tf4::Via::kFile ? "[save file]" : "[game]");
		ImGui::SameLine();
		ImGui::TextUnformatted(c.label.c_str());
		if (ImGui::IsItemHovered() && !c.why.empty()) {
			ImGui::SetTooltip("%s", c.why.c_str());
		}
		if (!c.detail.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", c.detail.c_str());
		}
		if (auto it = _tf4Results.find(i); it != _tf4Results.end()) {
			ImGui::SameLine();
			TextColored(it->second.first ? theme::Good() : theme::Bad(), it->second.first ? it->second.second : "not done: " + it->second.second);
		}
		ImGui::PopID();
	};
	auto counts = [&](const std::vector<std::size_t>& a_in) {
		std::size_t t = 0;
		for (auto i : a_in) {
			t += _tf4Changes[i].tick && _tf4Changes[i].available ? 1 : 0;
		}
		return Fmt("%zu of %zu ticked", t, a_in.size());
	};
	ImGui::BeginChild("##tf4list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3.6f), ImGuiChildFlags_Borders);
	for (int cat = 0; cat < 4; ++cat) {
		std::vector<std::size_t> in;
		for (std::size_t i = 0; i < _tf4Changes.size(); ++i) {
			const auto& c = _tf4Changes[i];
			if (CategoryIndex(c.kind) == cat && (needle.empty() || Lower(c.label + " " + c.detail).find(needle) != std::string::npos)) {
				in.push_back(i);
			}
		}
		if (in.empty()) {
			continue;
		}
		ImGui::PushID(cat);
		if (!needle.empty()) {
			ImGui::SetNextItemOpen(true);
		} else {
			ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
		}
		const bool open = ImGui::CollapsingHeader(Fmt("%s   (%s)###cat", CategoryOf(_tf4Changes[in[0]].kind), counts(in).c_str()).c_str());
		if (open) {
			ImGui::Indent();
			std::map<std::string, std::vector<std::size_t>> groups;
			for (auto i : in) {
				groups[transplantF4Group(_tf4Changes[i])].push_back(i);
			}
			if (groups.size() <= 1 || in.size() <= 12) {
				for (auto i : in) {
					row(i);
				}
			} else {
				for (const auto& [name, gi] : groups) {
					ImGui::PushID(name.c_str());
					if (!needle.empty()) {
						ImGui::SetNextItemOpen(true);
					}
					if (ImGui::TreeNodeEx("##grp", ImGuiTreeNodeFlags_SpanAvailWidth, "%s   (%s)", name.empty() ? "Other" : name.c_str(), counts(gi).c_str())) {
						for (auto i : gi) {
							row(i);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
			}
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
	if (_tf4Changes.empty()) {
		Wrapped(theme::Good(), "The good save already has everything the old one has (of what can be read from the files).");
	}
	ImGui::EndChild();

	// 4. apply: the file part, then the game part
	std::vector<std::size_t> fileRun, gameRun;
	for (std::size_t i = 0; i < _tf4Changes.size(); ++i) {
		const auto& c = _tf4Changes[i];
		if (c.tick && c.available && !(_tf4Results.contains(i) && _tf4Results.at(i).first)) {
			(c.via == tf4::Via::kFile ? fileRun : gameRun).push_back(i);
		}
	}
	ImGui::BeginDisabled(busy || fileRun.empty() || !_save);
	if (ImGui::Button(Fmt("A. Write %zu ticked thing(s) into the open save", fileRun.size()).c_str())) {
		std::size_t done = 0;
		for (auto i : fileRun) {
			const auto why = tf4::ApplyToFile(*_save, _tf4Changes[i]);
			_tf4Results[i] = { why.empty(), why.empty() ? "written into the save" : why };
			done += why.empty() ? 1 : 0;
		}
		if (done) {
			markChanged(Fmt("Transplant: %zu thing(s) written into the save", done));
		}
		_status = Fmt("Transplant: %zu of %zu written into the open save. Save it (File > Save), load it in Fallout 4, then use B for the rest.", done,
			fileRun.size());
	}
	ImGui::EndDisabled();
	tips::Tip("Level, experience, S.P.E.C.I.A.L., caps and perk ranks are written into the save open here. Nothing is on disk until you\n"
			  "use File > Save (a backup of the file is kept). Then load that save in the game for step B.");
	ImGui::SameLine(0, 20);
	ImGui::BeginDisabled(busy || gameRun.empty() || !helperOk);
	const bool goBtn = ImGui::Button(Fmt("B. Put %zu ticked thing(s) into the running game", gameRun.size()).c_str());
	ImGui::EndDisabled();
	tips::Tip("ReSaverHelper puts the items, perks and quest stages into the game loaded now (the good save, after step A).\n"
			  "The game saves first (a save to go back to); its load order gives the form ids.");
	if (goBtn) {
		confirm("Transplant into the game",
			Fmt("Put %zu ticked thing(s) into the game now?\n\n"
				"- The GOOD save (after step A, saved and loaded) must be the one loaded in Fallout 4.\n"
				"- The game makes a new save first, so you can go back.\n"
				"- After OK, switch to Fallout 4 and stay there, unpaused with menus closed, until it is done.\n"
				"- Quest stages run their scripts: give them a minute, then save in the game.",
				gameRun.size()),
			[this, gameRun] { transplantF4StartGame(gameRun); });
	}
	if (busy) {
		ImGui::SameLine();
		if (!_tf4WaitSave.empty()) {
			TextColored(theme::Caution(), Fmt("Waiting for the game's save (%.0f s): switch to Fallout 4 for a moment...", ImGui::GetTime() - _tf4WaitStart));
		} else {
			ImGui::ProgressBar(_tf4Run.empty() ? 0.0f : static_cast<float>(_tf4Pos) / static_cast<float>(_tf4Run.size()), ImVec2(260, 0),
				Fmt("%zu of %zu", _tf4Pos, _tf4Run.size()).c_str());
		}
	}
	if (_tf4NeedOrder && !busy) {
		ImGui::SameLine();
		if (ImGui::SmallButton("Put them in with the good save's load order")) {
			_tf4NeedOrder = false;
			_tf4GameOrder = {};
			_tf4GameNote = "The game's load order was not checked: the form ids are the good save's.";
			_tf4Run = gameRun;
			_tf4Pos = 0;
			_tf4Running = true;
			transplantF4Step();
		}
		tips::Tip("Only when the game runs with the same mods, in the same order, as when the good save was made.");
	}
	ImGui::Checkbox("Mark quests completed in the old save as completed", &_tf4MarkCompleted);
	tips::Tip("After setting a quest's last stage, CompleteQuest marks it completed, as it was in the old save (also when the game\n"
			  "refuses the stage). Untick to only set the stage.");
	ImGui::SameLine(0, 30);
	if (!live) {
		ImGui::TextDisabled("B needs Fallout 4 running with ReSaverHelper %d+ and a save loaded.", kTransplantHelper);
	} else if (!helperOk) {
		Wrapped(theme::Caution(), Fmt("ReSaverHelper %s is running: B needs version %d (Optional files on the mod page).", info.helperVersion.c_str(), kTransplantHelper));
	} else if (!_tf4GameNote.empty()) {
		ImGui::TextDisabled("%s", _tf4GameNote.c_str());
	} else {
		ImGui::TextDisabled("Nothing is taken from the good save: things are only added, and values only raised.");
	}
	ImGui::End();
}
