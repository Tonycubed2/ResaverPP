// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant page for Starfield. The old (broken) save and the good one (the save open in ReSaver Plus) are both read
// from their files, without the game. What the old save has on top is listed with ticks, like the Fallout 4 page. The
// level and experience, the credits and the items are written into the open save; perks and quests are put into the
// running game by ReSaverHelper for Starfield (version 2, APPLY): the game makes a save first, its load order gives the
// form ids, then one APPLY record per perk rank or quest step.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

using namespace ess;
namespace tsf = ess::transplant::sf;

namespace
{
	constexpr int kTransplantHelperSF = 2;  // the Starfield helper with APPLY

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

	std::string Summary(const tsf::Snapshot& s)
	{
		if (!s.valid) {
			return s.error.empty() ? tr("not chosen yet") : s.error;
		}
		std::size_t items = 0, done = 0;
		for (const auto& [k, i] : s.items) {
			items += static_cast<std::size_t>(std::max<std::int64_t>(0, i.count));
		}
		for (const auto& [k, q] : s.quests) {
			done += q.completed ? 1 : 0;
		}
		return Fmt(tr("%s  level %d, %s credits, %zu perks, %zu items, %zu quests completed"), s.player.c_str(), s.level,
			s.credits >= 0 ? std::to_string(s.credits).c_str() : "?", s.perks.size(), items, done);
	}

	int CategoryIndex(tsf::Kind k) { return k == tsf::Kind::kPerk ? 1 : k == tsf::Kind::kItem ? 2 : k == tsf::Kind::kQuest ? 3 : 0; }
	const char* CategoryOf(tsf::Kind k)
	{
		switch (CategoryIndex(k)) {
		case 1: return tr("Perks and skills");
		case 2: return tr("Items");
		case 3: return tr("Quests");
		default: return tr("Character: level, experience, credits");
		}
	}
}

// the names the snapshots need (which forms are perks and actor values), in the open save's load order
tsf::Lookup App::transplantSFLookup() const
{
	tsf::Lookup lk;
	if (!_db || !_db->loaded() || !_dbDone || !_save) {
		return lk;
	}
	auto db = _db;
	const auto* save = _save.get();
	lk.perkKind = [db, save](std::uint32_t f) {
		if (const auto* d = db->find(f)) {
			return db->type(*d) == "PERK" ? 1 : 0;
		}
		return db->wasRead(save->pluginOf(f)) ? 0 : -1;  // a plugin that was read does not have it as a perk
	};
	lk.isActorValue = [db, save](std::uint32_t f) {
		if (const auto* d = db->find(f)) {
			return db->type(*d) == "AVIF";
		}
		return (f > 0 && f < 0x800) || !db->wasRead(save->pluginOf(f));  // the engine's own, or a plugin not read
	};
	return lk;
}

// the old save, read from its file (kept: it is read again when the good save or the names change)
void App::transplantSFLoad(const std::filesystem::path& a_path)
{
	try {
		_tsfOldSave = std::shared_ptr<SaveFile>(SaveFile::load(a_path).release());
		_tsfOldPath = a_path;
		_tsfOldError.clear();
		if (!_tsfOldSave->isSF()) {
			_tsfOldError = platform::ToUtf8(a_path.filename()) + tr(" is not a Starfield save");
			_tsfOldSave.reset();
		}
	} catch (const std::exception& e) {
		_tsfOldSave.reset();
		_tsfOldError = tr("Could not read ") + platform::ToUtf8(a_path.filename()) + ": " + e.what();
	}
	_status = _tsfOldError.empty() ? tr("Read the old save ") + platform::ToUtf8(a_path.filename()) : _tsfOldError;
	_tsfGoodOf = nullptr;  // (read both again)
}

void App::transplantSFRead()
{
	const auto lk = transplantSFLookup();
	_tsfGood = _save ? tsf::FromSave(*_save, platform::ToUtf8(_path.filename()), *_save, lk) : tsf::Snapshot{};
	if (_tsfOldSave && _save) {
		_tsfOld = tsf::FromSave(*_tsfOldSave, platform::ToUtf8(_tsfOldPath.filename()), *_save, lk);
	} else {
		_tsfOld = {};
		_tsfOld.error = _tsfOldError;
	}
	_tsfChanges.clear();
	_tsfResults.clear();
	if (!_tsfOld.valid || !_tsfGood.valid) {
		return;
	}
	_tsfChanges = tsf::Diff(_tsfGood, _tsfOld, *_save);
	std::stable_sort(_tsfChanges.begin(), _tsfChanges.end(), [](const tsf::Change& a, const tsf::Change& b) {
		if (CategoryIndex(a.kind) != CategoryIndex(b.kind)) {
			return CategoryIndex(a.kind) < CategoryIndex(b.kind);
		}
		return a.kind == tsf::Kind::kQuest && a.order < b.order;  // quests in the old save's order
	});
	for (auto& c : _tsfChanges) {
		if (c.kind != tsf::Kind::kPerk && c.kind != tsf::Kind::kItem && c.kind != tsf::Kind::kQuest) {
			continue;
		}
		std::string n;
		if (_db && c.goodFormID) {
			if (const auto* f = _db->find(c.goodFormID)) {
				n = _db->name(*f);
				if (n.empty()) {
					n = _db->edid(*f);
				}
				// a quest with no name of its own is not in the journal: a background quest (radiant job, scene, mod setup)
				if (c.kind == tsf::Kind::kQuest && _db->name(*f).empty() && c.available) {
					c.risk = 2;
					c.tick = false;
					c.why = tr("A background quest (no name: the journal never listed it). Setting its stage can start or end things you never "
							   "saw. Leave it unless you know it.");
				}
			}
		}
		if (!n.empty()) {
			c.detail = c.label.substr(0, 8) + "  -  " + c.plugin;
			c.label = n + c.label.substr(8);
		} else {
			c.label = (c.kind == tsf::Kind::kPerk ? tr("Perk ") : c.kind == tsf::Kind::kItem ? tr("Item ") : tr("Quest ")) + c.label;
		}
	}
}

// ---- into the running game -----------------------------------------------------------------------

// step 1: the game saves (a save to go back to), and that save's plugin list is the game's load order
void App::transplantSFStartGame(std::vector<std::size_t> a_run)
{
	_tsfRun = std::move(a_run);
	_tsfPos = 0;
	_tsfRunning = true;
	_tsfGameNote.clear();
	for (auto i : _tsfRun) {
		_tsfResults.erase(i);
	}
	_status = tr("Transplant: the game saves first (a save to go back to; its load order gives the form ids)...");
	_live.request({ "SAVEGAME", "none" }, [this](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_tsfRunning = false;
			_status = tr("The game could not save: ") + LiveLink::Error(r);
			return;
		}
		_tsfWaitSave = LiveLink::Field(r, 0) + ".sfs";
		_tsfWaitStart = ImGui::GetTime();
		_tsfWaitSize = -1;
	});
}

// each frame while waiting: the save the game made, read when it is complete
void App::transplantSFPollSave()
{
	if (_tsfWaitSave.empty()) {
		return;
	}
	std::vector<std::filesystem::path> dirs = watchFolders();
	dirs.push_back(savesFolder());
	if (!_path.empty()) {
		dirs.push_back(_path.parent_path());
	}
	std::error_code ec;
	for (const auto& d : dirs) {
		const auto p = d / platform::FromUtf8(_tsfWaitSave);
		if (!std::filesystem::is_regular_file(p, ec)) {
			continue;
		}
		const auto size = static_cast<std::int64_t>(std::filesystem::file_size(p, ec));
		if (size != _tsfWaitSize) {  // still being written
			_tsfWaitSize = size;
			return;
		}
		try {
			auto s = SaveFile::load(p);
			_tsfGameOrder = s->plugins;
			_tsfWaitSave.clear();
			auto       sameList = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                if (a.size() != b.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < a.size(); ++i) {
                    if (Lower(a[i]) != Lower(b[i])) {
                        return false;
                    }
                }
                return true;
			};
			const bool same = _save && sameList(_tsfGameOrder.full, _save->plugins.full) && sameList(_tsfGameOrder.lite, _save->plugins.lite) &&
							  sameList(_tsfGameOrder.medium, _save->plugins.medium);
			_tsfGameNote = same ? tr("The game's load order is the good save's.")
								: tr("The game's load order is not the good save's: the form ids are worked out from the game's.");
			_tsfGameNote += tr(" The game saved as ") + platform::ToUtf8(p.filename()) + tr(" first (load it to go back).");
			transplantSFStep();
		} catch (const std::exception&) {
			// not complete yet: try again next frame
		}
		return;
	}
	if (ImGui::GetTime() - _tsfWaitStart > 45.0) {
		_tsfWaitSave.clear();
		_tsfRunning = false;
		_tsfNeedOrder = true;
		_status = tr("The save the game made was not found in the saves folders ReSaver Plus knows, so the game's load order could not be "
				  "checked. Choose the saves folder in Settings, or put them in with the good save's load order.");
	}
}

void App::transplantSFStep()
{
	if (_tsfPos >= _tsfRun.size()) {
		std::size_t failed = 0;
		for (auto i : _tsfRun) {
			if (auto it = _tsfResults.find(i); it != _tsfResults.end() && !it->second.first) {
				++failed;
			}
		}
		_tsfRunning = false;
		_status = Fmt(tr("Transplant: %zu of %zu sent to the game%s. Quests may keep running their scripts for a while; then save in the game, open "
					  "that save here and compare again: anything still listed did not take."),
			_tsfRun.size() - failed, _tsfRun.size(), failed ? Fmt(tr(", %zu could not be (see the list)"), failed).c_str() : "");
		_live.request({ "NOTIFY", liveNotifyMode(), Fmt("ReSaver Plus: transplant done (%zu of %zu). Save when the quests are quiet.", _tsfRun.size() - failed, _tsfRun.size()) },
			[](const LiveLink::Reply&) {});
		return;
	}
	const auto idx = _tsfRun[_tsfPos];
	if (idx >= _tsfChanges.size()) {
		++_tsfPos;
		transplantSFStep();
		return;
	}
	const auto& ch = _tsfChanges[idx];
	_status = Fmt(tr("Transplanting %zu of %zu: %s"), _tsfPos + 1, _tsfRun.size(), ch.label.c_str());
	const auto& order = _tsfGameOrder.full.empty() && _save ? _save->plugins : _tsfGameOrder;
	const auto  id = tsf::RuntimeID(order, ch.plugin, ch.local);
	auto        records = tsf::GameRecords(ch, id);
	if (!id || records.empty()) {
		_tsfResults[idx] = { false, !id ? ch.plugin + tr(" is not in the game's load order") : tr("nothing to do") };
		++_tsfPos;
		transplantSFStep();
		return;
	}
	transplantSFSend(idx, std::move(records), {});
}

// the records of one change one after the other (a quest: setstage, then completequest; a perk: one per rank)
void App::transplantSFSend(std::size_t a_idx, std::vector<ess::transplant::Record> a_rest, std::string a_said)
{
	if (a_rest.empty()) {
		_tsfResults[a_idx] = { true, tr("sent: ") + a_said };
		++_tsfPos;
		transplantSFStep();
		return;
	}
	std::vector<std::string> cmd{ "APPLY" };
	cmd.insert(cmd.end(), a_rest.front().begin(), a_rest.front().end());
	a_rest.erase(a_rest.begin());
	_live.request(cmd, [this, a_idx, rest = std::move(a_rest), said = std::move(a_said)](const LiveLink::Reply& r) mutable {
		if (!LiveLink::Ok(r) || LiveLink::Field(r, 0) == "0") {
			_tsfResults[a_idx] = { false, !LiveLink::Ok(r) ? LiveLink::Error(r) : LiveLink::Field(r, 1) };
			++_tsfPos;
			transplantSFStep();
			return;
		}
		said += (said.empty() ? "" : "; ") + LiveLink::Field(r, 1);
		transplantSFSend(a_idx, std::move(rest), std::move(said));
	});
}

// ---- the window -----------------------------------------------------------------------------------

void App::drawTransplantSF()
{
	prepareWindow("Transplant", ImVec2(0.9f, 0.9f));
	if (!ImGui::Begin(trId("Transplant"), &_showTransplant, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Transplant");
	transplantSFPollSave();
	std::string helperWhy;
	const bool  helperOk = sfHelperReady(kTransplantHelperSF, helperWhy);
	const bool  busy = _tsfRunning || !_tsfWaitSave.empty();

	ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 140);
	ImGui::TextWrapped("%s", tr("Bring your character's progress from an old save (one that crashes or will not load) into a good one: the save open in "
							  "ReSaver Plus. Both are read from their files, so the old save never has to load. The level and experience, the "
							  "credits and the items are written into the open save; perks, skills and quests are put into the running game by "
							  "ReSaverHelper for Starfield (version 2, the SFSE plugin) once the good save is loaded."));
	ImGui::PopTextWrapPos();

	// read both again when the open save, its edits or the plugin names change
	const bool dbReady = _db && _db->loaded() && _dbDone;
	if (_save && (_save.get() != _tsfGoodOf || _tsfGen != _editGen || _tsfDbOf != _db.get() || _tsfDbReady != dbReady) && !busy) {
		_tsfGoodOf = _save.get();
		_tsfGen = _editGen;
		_tsfDbOf = _db.get();
		_tsfDbReady = dbReady;
		transplantSFRead();
	}
	if (!dbReady) {
		Wrapped(theme::Caution(), tr("The plugin names are not read yet (Tools > Read plugin names): until they are, the perks and the experience "
								   "cannot be read and nothing has a name."));
	}

	if (ImGui::BeginTable("steps", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		ImGui::SeparatorText(tr("1. The old save: where the progress is"));
		ImGui::TextWrapped("%s", Summary(_tsfOld).c_str());
		if (_tsfOld.valid) {
			ImGui::TextDisabled("%s", _tsfOld.source.c_str());
		}
		ImGui::BeginDisabled(busy);
		if (ImGui::Button(tr("Choose the old save...")) && platform::HasNativeDialogs()) {
			if (auto p = platform::OpenSaveDialog(_path.empty() ? savesFolder() : _path.parent_path())) {
				transplantSFLoad(*p);
			}
		}
		tips::Tip(tr("The save with the progress you want back: it may crash or not load at all, it is only read from its file."));
		if (!_path.empty()) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::BeginCombo("##tsfsaves", tr("or pick one from this folder"))) {
				std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> saves;
				std::error_code ec;
				for (const auto& e : std::filesystem::directory_iterator(_path.parent_path(), ec)) {
					if (e.is_regular_file(ec) && Lower(platform::ToUtf8(e.path().extension())) == ".sfs" && e.path() != _path) {
						saves.emplace_back(e.last_write_time(ec), e.path());
					}
				}
				std::sort(saves.begin(), saves.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				for (std::size_t i = 0; i < saves.size() && i < 60; ++i) {
					if (ImGui::Selectable(platform::ToUtf8(saves[i].second.filename()).c_str())) {
						transplantSFLoad(saves[i].second);
					}
				}
				ImGui::EndCombo();
			}
		}
		ImGui::EndDisabled();
		if (_tsfOld.valid && !_tsfOld.missingParts.empty()) {
			std::string parts;
			for (const auto& m : _tsfOld.missingParts) {
				parts += (parts.empty() ? "" : ", ") + m;
			}
			Wrapped(theme::Caution(), tr("Could not be read from this save: ") + parts + ".");
		}
		if (_tsfOld.valid && _tsfOld.skippedItems) {
			ImGui::TextDisabled(tr("%zu item stack(s) made in the game (no plugin) are left out."), _tsfOld.skippedItems);
		}

		ImGui::TableNextColumn();
		ImGui::SeparatorText(tr("2. The good save: the one open in ReSaver Plus"));
		ImGui::TextWrapped("%s", _save ? Summary(_tsfGood).c_str() : tr("Open the good save first (File > Open)."));
		if (_save) {
			ImGui::TextDisabled("%s", platform::ToUtf8(_path.filename()).c_str());
		}
		if (_tsfOld.valid && _tsfGood.valid && _tsfOld.player != _tsfGood.player) {
			Wrapped(theme::Bad(), tr("These are two different characters (") + _tsfOld.player + " / " + _tsfGood.player + ").");
		}
		if (_tsfGood.valid && !_tsfGood.missingParts.empty()) {
			std::string parts;
			for (const auto& m : _tsfGood.missingParts) {
				parts += (parts.empty() ? "" : ", ") + m;
			}
			Wrapped(theme::Caution(), tr("Could not be read from this save: ") + parts + ".");
		}
		ImGui::BeginDisabled(busy || !_save);
		if (ImGui::SmallButton(tr("Compare again"))) {
			if (!_tsfOldPath.empty()) {
				transplantSFLoad(_tsfOldPath);
			}
			transplantSFRead();
		}
		ImGui::EndDisabled();
		tips::Tip(tr("Read both saves again and list the differences (e.g. after a Transplant: open the save the game made afterwards and compare)."));
		ImGui::EndTable();
	}

	if (!_tsfOld.valid || !_tsfGood.valid) {
		ImGui::End();
		return;
	}

	// 3. the list
	ImGui::SeparatorText(Fmt(tr("3. What the old save has on top (%zu)"), _tsfChanges.size()).c_str());
	Dot(theme::Good());
	ImGui::TextUnformatted(tr("Safe"));
	ImGui::SameLine(0, 18);
	Dot(theme::Caution());
	ImGui::TextUnformatted(tr("Caution (hover to read why)"));
	ImGui::SameLine(0, 18);
	Dot(theme::Bad());
	ImGui::TextUnformatted(tr("Risky or cannot"));
	ImGui::SameLine(0, 30);
	TextColored(theme::Good(), tr("[save file]"));
	ImGui::SameLine();
	ImGui::TextDisabled("%s", tr("written into the open save"));
	ImGui::SameLine(0, 18);
	TextColored(theme::Caution(), tr("[game]"));
	ImGui::SameLine();
	ImGui::TextDisabled("%s", tr("put into the running game"));
	ImGui::SameLine(0, 30);
	ImGui::BeginDisabled(busy);
	if (ImGui::SmallButton(tr("Tick everything"))) {
		for (auto& c : _tsfChanges) {
			c.tick = c.available;
		}
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(tr("Untick everything"))) {
		for (auto& c : _tsfChanges) {
			c.tick = false;
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##tsffilter", tr("search"), _tsfFilter, sizeof(_tsfFilter));
	const auto needle = Lower(_tsfFilter);

	auto row = [&](std::size_t i) {
		auto& c = _tsfChanges[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(!c.available || busy);
		ImGui::Checkbox("##t", &c.tick);
		ImGui::EndDisabled();
		ImGui::SameLine();
		Dot(RiskCol(c.risk));
		TextColored(c.via == tsf::Via::kFile ? theme::Good() : theme::Caution(), c.via == tsf::Via::kFile ? tr("[save file]") : tr("[game]"));
		ImGui::SameLine();
		ImGui::TextUnformatted(c.label.c_str());
		if (ImGui::IsItemHovered() && !c.why.empty()) {
			ImGui::SetTooltip("%s", c.why.c_str());
		}
		if (!c.detail.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", c.detail.c_str());
		}
		if (auto it = _tsfResults.find(i); it != _tsfResults.end()) {
			ImGui::SameLine();
			TextColored(it->second.first ? theme::Good() : theme::Bad(), it->second.first ? it->second.second : tr("not done: ") + it->second.second);
		}
		ImGui::PopID();
	};
	auto counts = [&](const std::vector<std::size_t>& a_in) {
		std::size_t t = 0;
		for (auto i : a_in) {
			t += _tsfChanges[i].tick && _tsfChanges[i].available ? 1 : 0;
		}
		return Fmt(tr("%zu of %zu ticked"), t, a_in.size());
	};
	ImGui::BeginChild("##tsflist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3.6f), ImGuiChildFlags_Borders);
	for (int cat = 0; cat < 4; ++cat) {
		std::vector<std::size_t> in;
		for (std::size_t i = 0; i < _tsfChanges.size(); ++i) {
			const auto& c = _tsfChanges[i];
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
		if (ImGui::CollapsingHeader(Fmt("%s   (%s)###cat", CategoryOf(_tsfChanges[in[0]].kind), counts(in).c_str()).c_str())) {
			ImGui::Indent();
			for (auto i : in) {
				row(i);
			}
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
	if (_tsfChanges.empty()) {
		Wrapped(theme::Good(), tr("The good save already has everything the old one has (of what can be read from the files)."));
	}
	ImGui::EndChild();

	// 4. apply: the file part, then the game part
	std::vector<std::size_t> fileRun, gameRun;
	for (std::size_t i = 0; i < _tsfChanges.size(); ++i) {
		const auto& c = _tsfChanges[i];
		if (c.tick && c.available && !(_tsfResults.contains(i) && _tsfResults.at(i).first)) {
			(c.via == tsf::Via::kFile ? fileRun : gameRun).push_back(i);
		}
	}
	ImGui::BeginDisabled(busy || fileRun.empty() || !_save || _readOnly);
	if (ImGui::Button(Fmt(tr("A. Write %zu ticked thing(s) into the open save"), fileRun.size()).c_str())) {
		const auto  lk = transplantSFLookup();
		std::size_t done = 0;
		for (auto i : fileRun) {
			const auto why = lk.isActorValue || _tsfChanges[i].kind != tsf::Kind::kLevel ? tsf::ApplyToFile(*_save, _tsfChanges[i], lk.isActorValue)
																					   : std::string(tr("the plugin names are not read"));
			_tsfResults[i] = { why.empty(), why.empty() ? tr("written into the save") : why };
			done += why.empty() ? 1 : 0;
		}
		if (done) {
			// (the list stays as it is, with what was done; Compare again reads the edited save)
			const auto results = _tsfResults;
			markChanged(Fmt(tr("Transplant: %zu thing(s) written into the save"), done));
			_tsfGen = _editGen;
			_tsfResults = results;
		}
		_status = Fmt(tr("Transplant: %zu of %zu written into the open save. Save it (File > Save), load it in Starfield, then use B for the rest."), done,
			fileRun.size());
	}
	ImGui::EndDisabled();
	tips::Tip(tr("The level and experience, the credits and the items are written into the save open here. Nothing is on disk until\n"
			  "you use File > Save (a backup of the file is kept). Then load that save in the game for step B."));
	ImGui::SameLine(0, 20);
	ImGui::BeginDisabled(busy || gameRun.empty() || !helperOk);
	const bool goBtn = ImGui::Button(Fmt(tr("B. Put %zu ticked thing(s) into the running game"), gameRun.size()).c_str());
	ImGui::EndDisabled();
	tips::Tip(tr("ReSaverHelper puts the perks, skills and quest stages into the game loaded now (the good save, after step A).\n"
			  "The game saves first (a save to go back to); its load order gives the form ids."));
	if (goBtn) {
		confirm(tr("Transplant into the game"),
			Fmt(tr("Put %zu ticked thing(s) into the game now?\n\n"
				"- The GOOD save (after step A, saved and loaded) must be the one loaded in Starfield.\n"
				"- The game makes a new save first, so you can go back.\n"
				"- After OK, switch to Starfield and stay there, unpaused with menus closed, until it is done.\n"
				"- Quest stages run their scripts: give them a minute, then save in the game."),
				gameRun.size()),
			[this, gameRun] { transplantSFStartGame(gameRun); });
	}
	if (busy) {
		ImGui::SameLine();
		if (!_tsfWaitSave.empty()) {
			TextColored(theme::Caution(), Fmt(tr("Waiting for the game's save (%.0f s): switch to Starfield for a moment..."), ImGui::GetTime() - _tsfWaitStart));
		} else {
			ImGui::ProgressBar(_tsfRun.empty() ? 0.0f : static_cast<float>(_tsfPos) / static_cast<float>(_tsfRun.size()), ImVec2(260, 0),
				Fmt(tr("%zu of %zu"), _tsfPos, _tsfRun.size()).c_str());
		}
	}
	if (_tsfNeedOrder && !busy) {
		ImGui::SameLine();
		if (ImGui::SmallButton(tr("Put them in with the good save's load order"))) {
			_tsfNeedOrder = false;
			_tsfGameOrder = {};
			_tsfGameNote = tr("The game's load order was not checked: the form ids are the good save's.");
			_tsfRun = gameRun;
			_tsfPos = 0;
			_tsfRunning = true;
			transplantSFStep();
		}
		tips::Tip(tr("Only when the game runs with the same mods, in the same order, as when the good save was made."));
	}
	if (!helperOk) {
		ImGui::TextDisabled("%s", (tr("B: ") + helperWhy).c_str());
	} else if (!_tsfGameNote.empty()) {
		ImGui::TextDisabled("%s", _tsfGameNote.c_str());
	} else {
		ImGui::TextDisabled("%s", tr("Nothing is taken from the good save: things are only added, and values only raised."));
	}
	ImGui::End();
}
