// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Script viewer: double-click a script anywhere to see its Papyrus source, with syntax colours,
// matching block keywords, folding, find and an outline; compile it with the game's Papyrus
// compiler. Modelled on the npp-papyrus Notepad++ plugin (blu3mania/npp-papyrus).
#include "App.h"

#include "Platform.h"

#include "core/PapyrusCompiler.h"
#include "core/PapyrusSyntax.h"

#include "TextEditor.h"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unordered_set>

using namespace ess;

struct ScriptDoc
{
	std::string                           name;        // script name
	std::filesystem::path                 psc;         // source file on disk (empty: decompiled, not saved yet)
	std::string                           sourceMod;   // mod folder of the .psc
	std::optional<ScriptSources::PexFile> pex;         // the compiled script the game loads
	std::filesystem::path                 pexFile;     // a .pex opened directly
	bool                                  decompiled = false;
	std::vector<std::string>              decompileWarnings;
	std::string                           note;        // shown under the toolbar
	TextEditor                            editor;
	std::string                           savedText;
	bool                                  crlf = true;
	std::shared_ptr<psc::Context>         ctx;
	psc::Analysis                         analysis;
	bool                                  analysisDirty = true;
	double                                editTime = 0;
	std::vector<std::string>              lines;       // cached text, for find
	bool                                  linesDirty = true;
	std::vector<std::uint8_t>             pexBytes;
	bool                                  pexLoaded = false;
	std::vector<std::string>              disasm;
	std::string                           disasmError;
	// compile
	bool                                  compiling = false, compiled = false, compileOk = false;
	std::vector<papc::Message>            messages;
	std::vector<std::string>              output;
	std::string                           summary, commandLine;
	int                                   threadLine = -1;
	bool                                  showOutline = true;
	float                                 zoom = 1.0f;

	bool textModified = false;  // edited since opened / saved
	bool modified() const { return textModified || (decompiled && psc.empty()); }
};

struct CompileJob
{
	std::thread              thread;
	std::atomic<bool>        done{ false };
	papc::Result             result;
	std::weak_ptr<ScriptDoc> doc;
	std::string              script;
	std::filesystem::path    output;
};

namespace
{
	constexpr std::uint32_t kWarn = IM_COL32(255, 190, 80, 255);
	constexpr std::uint32_t kBad = IM_COL32(255, 110, 110, 255);
	constexpr std::uint32_t kGood = IM_COL32(130, 220, 130, 255);
	constexpr std::uint32_t kDim = IM_COL32(150, 150, 150, 255);

	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	std::string Fmt(const char* f, auto... args)
	{
		char buf[2048];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::string ReadAll(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	// npp-papyrus colours: DarkModeDefault theme and the default (light) theme
	TextEditor::Palette PapyrusPalette(bool a_light)
	{
		using P = TextEditor::PaletteIndex;
		TextEditor::Palette p{};
		auto                set = [&](P i, std::uint32_t rgb, int a = 255) {
            p[static_cast<int>(i)] = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
		};
		if (!a_light) {
			set(P::Default, 0xDCDCCC);
			set(P::Punctuation, 0xDCDCCC);
			set(P::Identifier, 0xDCDCCC);
			set(P::Flow, 0xDFC47D);
			set(P::Type, 0xCEDF99);
			set(P::Keyword, 0xDFC47D);
			set(P::Keyword2, 0xCEDF99);
			set(P::Comment, 0x7FBF7F);
			set(P::MultiLineComment, 0x7FBF7F);
			set(P::Doc, 0x7FBF7F);
			set(P::Number, 0x8CD0D3);
			set(P::String, 0xCC9393);
			set(P::Property, 0x56C8B4);
			set(P::Class, 0x6FAEE8);
			set(P::Function, 0xFFCFAF);
			set(P::Background, 0x3F3F3F);
			set(P::Cursor, 0xF0F0F0);
			set(P::Selection, 0x6A8CC8, 110);
			set(P::ErrorMarker, 0xD04040, 110);
			set(P::Breakpoint, 0xE0A030, 70);
			set(P::LineNumber, 0x9A9A8A);
			set(P::CurrentLineFill, 0xFFFFFF, 14);
			set(P::CurrentLineFillInactive, 0xFFFFFF, 8);
			set(P::CurrentLineEdge, 0xFFFFFF, 30);
			set(P::FoldMarker, 0xA8A898);
		} else {
			set(P::Default, 0x000000);
			set(P::Punctuation, 0x000000);
			set(P::Identifier, 0x000000);
			set(P::Flow, 0xC000C0);
			set(P::Type, 0x6060FF);
			set(P::Keyword, 0xC000C0);
			set(P::Keyword2, 0x6060FF);
			set(P::Comment, 0x008000);
			set(P::MultiLineComment, 0x008000);
			set(P::Doc, 0x008000);
			set(P::Number, 0x606060);
			set(P::String, 0xAA2200);
			set(P::Property, 0x005555);
			set(P::Class, 0x0000FF);
			set(P::Function, 0x555500);
			set(P::Background, 0xFFFFFF);
			set(P::Cursor, 0x000000);
			set(P::Selection, 0x99B4E0, 140);
			set(P::ErrorMarker, 0xFF4040, 70);
			set(P::Breakpoint, 0xFFC040, 90);
			set(P::LineNumber, 0x808080);
			set(P::CurrentLineFill, 0x000000, 10);
			set(P::CurrentLineFillInactive, 0x000000, 6);
			set(P::CurrentLineEdge, 0x000000, 30);
			set(P::FoldMarker, 0x707070);
		}
		return p;
	}

	TextEditor::LanguageDefinition PapyrusLanguage(const std::shared_ptr<psc::Context>& ctx)
	{
		TextEditor::LanguageDefinition ld;
		ld.mName = "Papyrus";
		ld.mCommentStart = ";/";
		ld.mCommentEnd = "/;";
		ld.mSingleLineComment = ";";
		ld.mPreprocChar = '\x01';  // Papyrus has no preprocessor
		ld.mCaseSensitive = false;
		ld.mAutoIndentation = true;
		ld.mLineColorizer = [ctx](const char* b, const char* e, TextEditor::PaletteIndex* out) {
			using P = TextEditor::PaletteIndex;
			const std::size_t       n = static_cast<std::size_t>(e - b);
			std::vector<psc::Kind>  k(n);
			psc::ColorLine(b, n, k.data(), *ctx);
			for (std::size_t i = 0; i < n; ++i) {
				switch (k[i]) {
				case psc::Kind::kOperator: out[i] = P::Punctuation; break;
				case psc::Kind::kFlow: out[i] = P::Flow; break;
				case psc::Kind::kType: out[i] = P::Type; break;
				case psc::Kind::kKeyword: out[i] = P::Keyword; break;
				case psc::Kind::kKeyword2: out[i] = P::Keyword2; break;
				case psc::Kind::kComment: out[i] = P::Comment; break;
				case psc::Kind::kDoc: out[i] = P::Doc; break;
				case psc::Kind::kNumber: out[i] = P::Number; break;
				case psc::Kind::kString: out[i] = P::String; break;
				case psc::Kind::kProperty: out[i] = P::Property; break;
				case psc::Kind::kClass: out[i] = P::Class; break;
				case psc::Kind::kFunction: out[i] = P::Function; break;
				default: out[i] = P::Default; break;
				}
			}
		};
		return ld;
	}

	// parents' sources for the decompiler (parameter defaults): scanned folders, then near the .pex
	pex::SourceLookup MakeLookup(const ScriptSources& a_sources, const std::filesystem::path& a_here)
	{
		const auto sources = a_sources.sourceFiles;
		return [sources, a_here](const std::string& script) -> std::string {
			std::filesystem::path p;
			if (auto it = sources.find(Lower(script)); it != sources.end()) {
				p = it->second;
			} else if (!a_here.empty()) {
				for (const auto& cand : { a_here / (script + ".psc"), a_here / "Source" / (script + ".psc"), a_here.parent_path() / "Source" / "Scripts" / (script + ".psc") }) {
					std::error_code ec;
					if (std::filesystem::exists(cand, ec)) {
						p = cand;
						break;
					}
				}
			}
			return p.empty() ? std::string() : ReadAll(p);
		};
	}

	std::string PexWhere(const ScriptSources::PexFile& p)
	{
		const auto file = platform::ToUtf8(p.file.filename());
		return p.bsaEntry.empty() ? p.mod + "  (" + file + ")" : p.mod + "  (inside " + file + ")";
	}
}

// ==========================================================================================
// opening scripts
// ==========================================================================================

bool App::ensureSourcesScanned()
{
	if (!_settingsLoaded) {
		loadSettings();
	}
	if (scanning()) {
		return false;
	}
	if (!_sources.scanned() && !_sourcesPath.empty()) {
		scanSources(platform::FromUtf8(_sourcesPath));
		return false;
	}
	return true;
}

void App::scriptItem(const std::string& a_script, const std::string& a_function, const std::string& a_state, int a_instruction)
{
	if (a_script.empty()) {
		return;
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
		ImGui::SetTooltip("Double-click to view %s%s", a_script.c_str(), a_function.empty() ? "" : " at this point");
	}
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		viewScript(a_script, a_function, a_state, a_instruction);
	}
}

void App::viewScript(const std::string& a_script, const std::string& a_function, const std::string& a_state, int a_instruction)
{
	if (!ensureSourcesScanned()) {
		// opened as soon as the scan of the mods is done
		_pendingView = PendingView{ a_script, a_function, a_state, a_instruction };
		_status = "Scanning your mods first: " + a_script + " opens when it is done...";
		return;
	}
	const auto key = Lower(a_script);
	for (std::size_t i = 0; i < _docs.size(); ++i) {
		if (Lower(_docs[i]->name) == key) {
			_docSelect = static_cast<int>(i);
			_showViewer = true;
			placeCursor(*_docs[i], a_function, a_state, a_instruction);
			return;
		}
	}
	if (auto it = _sources.sourceFiles.find(key); it != _sources.sourceFiles.end()) {
		openDoc(a_script, it->second, false, a_function, a_state, a_instruction);
		return;
	}
	if (const auto pexf = _sources.findPex(a_script)) {
		_decompileWhere = PexWhere(*pexf);
		if (_decompileAlways) {
			openDoc(a_script, {}, true, a_function, a_state, a_instruction);
		} else {
			_decompileAsk = true;
			_decompileScript = a_script;
			_decompileFn = a_function;
			_decompileState = a_state;
			_decompileInstr = a_instruction;
		}
		return;
	}
	if (!_sources.scanned()) {
		_error = "ReSaver++ does not know where your mods are yet.\n\nGo to Options > Settings, click \"Choose mods folder...\" and pick your Mod Organizer "
				 "\"mods\" folder (or the game's Data folder), then double-click the script again.";
		_showSettings = true;
	} else {
		_error = "Neither " + a_script + ".psc nor " + a_script +
				 ".pex was found in the scanned mods or the game's Data folder.\n\nThe script is probably no longer installed "
				 "(it shows as UNDEFINED in the save), so there is nothing to show.";
	}
}

void App::viewScriptFile(const std::filesystem::path& a_path)
{
	ensureSourcesScanned();
	const auto ext = Lower(platform::ToUtf8(a_path.extension()));
	const auto name = platform::ToUtf8(a_path.stem());
	for (std::size_t i = 0; i < _docs.size(); ++i) {
		const auto& d = *_docs[i];
		if ((ext == ".psc" && d.psc == a_path) || (ext == ".pex" && d.pexFile == a_path)) {
			_docSelect = static_cast<int>(i);
			_showViewer = true;
			return;
		}
	}
	if (ext == ".psc") {
		openDoc(name, a_path, false, {}, {}, -1);
	} else {
		openDoc(name, {}, true, {}, {}, -1, a_path);
	}
}

void App::openDoc(const std::string& a_script, const std::filesystem::path& a_psc, bool a_decompile, const std::string& a_function, const std::string& a_state,
	int a_instruction, const std::filesystem::path& a_pexFile)
{
	auto d = std::make_shared<ScriptDoc>();
	d->name = a_script;
	d->pexFile = a_pexFile;
	if (a_pexFile.empty()) {
		d->pex = _sources.findPex(a_script);
	}
	std::string text;
	try {
		if (!a_decompile) {
			d->psc = a_psc;
			text = ReadAll(a_psc);
			d->crlf = text.find("\r\n") != std::string::npos || text.find('\n') == std::string::npos;
			if (auto it = _sources.sourceMods.find(Lower(a_script)); it != _sources.sourceMods.end()) {
				d->sourceMod = it->second;
			}
		} else {
			if (a_pexFile.empty()) {
				if (!d->pex) {
					throw std::runtime_error("the compiled script was not found");
				}
				d->pexBytes = ScriptSources::readPex(*d->pex);
			} else {
				std::ifstream in(a_pexFile, std::ios::binary);
				d->pexBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			}
			d->pexLoaded = true;
			const auto f = pex::Read(d->pexBytes);
			if (!f.objects.empty() && !f.str(f.objects.front().name).empty()) {
				d->name = f.str(f.objects.front().name);
			}
			const auto here = !a_pexFile.empty() ? a_pexFile.parent_path() : (d->pex && d->pex->bsaEntry.empty() ? d->pex->file.parent_path() : std::filesystem::path());
			const auto lines = pex::Decompile(f, &d->decompileWarnings, MakeLookup(_sources, here));
			for (const auto& l : lines) {
				text += l + "\n";
			}
			d->decompiled = true;
		}
	} catch (const std::exception& e) {
		_error = "Could not open " + a_script + ":\n\n" + e.what();
		return;
	}
	if (!a_decompile && d->pex && !d->sourceMod.empty() && Lower(d->pex->mod) != Lower(d->sourceMod)) {
		d->note = "This source comes from " + d->sourceMod + ", but the game loads the compiled script from " + d->pex->mod +
				  ". The two may not match.";
	}

	// colours: installed script names are classes
	const std::size_t key = _sources.pexFound * 31 + _sources.sourceFiles.size() + (_save ? _save->papyrus()->scripts.size() * 7 : 0);
	if (!_scriptNamesCache || key != _scriptNamesKey) {
		auto ctx = std::make_shared<psc::Context>();
		for (const auto& n : _sources.scriptNames()) {
			ctx->scripts.insert(n);
		}
		if (_save) {
			const auto& p = *_save->papyrus();
			for (const auto& s : p.scripts) {
				ctx->scripts.insert(Lower(p.str(s.name)));
			}
		}
		_scriptNamesCache = ctx;
		_scriptNamesKey = key;
	}
	d->ctx = std::make_shared<psc::Context>(*std::static_pointer_cast<psc::Context>(_scriptNamesCache));
	const auto lines = psc::SplitLines(text);
	d->analysis = psc::Analyze(lines);
	d->ctx->properties = d->analysis.properties;
	d->editor.SetLanguageDefinition(PapyrusLanguage(d->ctx));
	d->editor.SetPalette(PapyrusPalette(_editorLight));
	d->editor.SetTabSize(4);
	d->editor.SetShowWhitespaces(false);
	auto shown = lines;
	if (shown.size() > 1 && shown.back().empty()) {
		shown.pop_back();  // the final newline
	}
	d->editor.SetTextLines(shown);
	d->savedText = d->editor.GetText();
	std::vector<TextEditor::FoldRegion> folds;
	for (const auto& b : d->analysis.blocks) {
		if (b.endLine > b.startLine) {
			folds.push_back({ b.startLine, b.endLine });
		}
	}
	d->editor.SetFoldRegions(folds);
	d->analysisDirty = false;
	_docs.push_back(d);
	_docSelect = static_cast<int>(_docs.size()) - 1;
	_showViewer = true;
	_status = "Opened " + d->name + (d->decompiled ? " (decompiled)" : "");
	placeCursor(*d, a_function, a_state, a_instruction);
}

void App::placeCursor(ScriptDoc& d, const std::string& a_function, const std::string& a_state, int a_instruction)
{
	if (a_function.empty()) {
		return;
	}
	int line = -1;
	// a thread's position: the compiled script's debug info maps the instruction to a source line
	if (a_instruction >= 0 && !d.decompiled && d.pex) {
		try {
			if (!d.pexLoaded) {
				d.pexBytes = ScriptSources::readPex(*d.pex);
				d.pexLoaded = true;
			}
			const auto f = pex::Read(d.pexBytes);
			for (const auto& df : f.debug) {
				if (Lower(f.str(df.function)) == Lower(a_function) && Lower(f.str(df.state)) == Lower(a_state) && df.type == 0 &&
					static_cast<std::size_t>(a_instruction) < df.lines.size()) {
					line = static_cast<int>(df.lines[static_cast<std::size_t>(a_instruction)]) - 1;
					// the source must match the compiled script: a call must be on that line
					for (const auto& o : f.objects) {
						for (const auto& st : o.states) {
							if (Lower(f.str(st.name)) != Lower(a_state)) {
								continue;
							}
							for (const auto& nf : st.functions) {
								if (Lower(f.str(nf.name)) != Lower(a_function) || static_cast<std::size_t>(a_instruction) >= nf.fn.code.size()) {
									continue;
								}
								const auto& in = nf.fn.code[static_cast<std::size_t>(a_instruction)];
								const int   ni = in.op == 25 ? 1 : 0;
								if ((in.op == 23 || in.op == 24 || in.op == 25) && in.args.size() > static_cast<std::size_t>(ni)) {
									const auto called = Lower(f.str(in.args[static_cast<std::size_t>(ni)].s));
									if (Lower(d.editor.LineText(line)).find(called) == std::string::npos) {
										d.note = "This source does not match the compiled script the game loads (different version?): the thread's exact "
												 "line cannot be shown, so its function is.";
										line = -1;
									}
								}
							}
						}
					}
					break;
				}
			}
		} catch (const std::exception&) {
		}
	}
	const int  fnLine = psc::FindFunction(d.analysis, a_function, a_state);
	const bool exact = line >= 0;
	if (line < 0) {
		line = fnLine;
	}
	if (line < 0) {
		_status = a_function + " was not found in " + d.name;
		return;
	}
	d.editor.ScrollToLine(line);
	if (a_instruction >= 0) {
		d.threadLine = line;
		d.editor.SetBreakpoints({ line + 1 });
		_status = exact ? "The thread is stopped at line " + std::to_string(line + 1) + " of " + d.name
						: "The thread is in " + a_function + " of " + d.name +
							  (d.decompiled ? " (decompiled code has no line numbers: the function is shown)"
											: " (the exact line needs the compiled script's debug info: the function is shown)");
	}
}

// ==========================================================================================
// the viewer window
// ==========================================================================================

void App::drawScriptViewer()
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.04f, vp->WorkPos.y + vp->WorkSize.y * 0.04f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.92f, vp->WorkSize.y * 0.9f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Script viewer###scriptviewer", &_showViewer)) {
		_viewerFocused = false;
		ImGui::End();
		return;
	}
	_viewerFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (_docs.empty()) {
		ImGui::TextDisabled("Double-click a script anywhere (Scripts, Script Instances, threads, a mod's script list) to see it here.");
		ImGui::End();
		return;
	}
	int closeAt = -1;
	if (ImGui::BeginTabBar("docs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_AutoSelectNewTabs)) {
		for (std::size_t i = 0; i < _docs.size(); ++i) {
			auto&            d = *_docs[i];
			bool             open = true;
			ImGuiTabItemFlags fl = d.modified() ? ImGuiTabItemFlags_UnsavedDocument : 0;
			if (static_cast<int>(i) == _docSelect) {
				fl |= ImGuiTabItemFlags_SetSelected;
			}
			const std::string label = d.name + (d.decompiled ? " (decompiled)" : "") + "###doc" + std::to_string(reinterpret_cast<std::uintptr_t>(&d));
			if (ImGui::BeginTabItem(label.c_str(), &open, fl)) {
				drawDoc(d);
				ImGui::EndTabItem();
			}
			if (!open) {
				closeAt = static_cast<int>(i);
			}
		}
		ImGui::EndTabBar();
	}
	_docSelect = -1;
	if (closeAt >= 0) {
		auto doc = _docs[static_cast<std::size_t>(closeAt)];
		auto close = [this, doc] {
			_docs.erase(std::remove(_docs.begin(), _docs.end(), doc), _docs.end());
			if (_docs.empty()) {
				_showViewer = false;
			}
		};
		if (doc->textModified) {
			confirm("Close " + doc->name, "Close " + doc->name + " without saving your changes?", close);
		} else {
			close();
		}
	}
	ImGui::End();
}

void App::drawDoc(ScriptDoc& d)
{
	auto&      ed = d.editor;
	auto&      io = ImGui::GetIO();
	const bool keys = _viewerFocused;

	auto findNext = [&](bool back) {
		const std::string needle = Lower(_findBuf);
		if (needle.empty()) {
			return;
		}
		if (d.linesDirty) {
			d.lines = ed.GetTextLines();
			d.linesDirty = false;
		}
		const auto cur = ed.GetCursorPosition();
		const int  n = static_cast<int>(d.lines.size());
		int        line = cur.mLine;
		int        from = ed.CharIndex(cur);
		for (int step = 0; step <= n; ++step) {
			const auto hay = Lower(d.lines[static_cast<std::size_t>(line)]);
			std::size_t at = std::string::npos;
			if (!back) {
				at = hay.find(needle, step == 0 ? static_cast<std::size_t>(std::max(0, from)) : 0);
			} else {
				const int limit = step == 0 ? from - static_cast<int>(needle.size()) - 1 : static_cast<int>(hay.size());
				if (limit >= 0) {
					at = hay.rfind(needle, static_cast<std::size_t>(limit));
				}
			}
			if (at != std::string::npos) {
				const TextEditor::Coordinates a(line, ed.ColumnOfIndex(line, static_cast<int>(at)));
				const TextEditor::Coordinates b(line, ed.ColumnOfIndex(line, static_cast<int>(at + needle.size())));
				ed.SetCursorPosition(b);
				ed.SetSelection(a, b);
				return;
			}
			line = back ? (line + n - 1) % n : (line + 1) % n;
		}
		_status = "\"" + std::string(_findBuf) + "\" was not found in " + d.name;
	};

	// ---- toolbar
	if (ImGui::Button("Save") || (keys && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
		saveDoc(d, false);
	}
	ImGui::SameLine();
	if (ImGui::Button("Save as...") || (keys && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
		saveDoc(d, true);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	const bool busy = _compileJob != nullptr;
	ImGui::BeginDisabled(busy);
	if (ImGui::Button(busy ? "Compiling..." : "Compile (F7)") || (!busy && keys && ImGui::IsKeyPressed(ImGuiKey_F7, false))) {
		startCompile(d);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Compile with the game's Papyrus compiler.\nOutput: %s", platform::ToUtf8(compileOutputFolder(d)).c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Compile settings...")) {
		_showCompileSettings = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(180);
	if (_findFocus || (keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))) {
		ImGui::SetKeyboardFocusHere();
		_findFocus = false;
	}
	if (ImGui::InputTextWithHint("##find", "Find (Ctrl+F)", _findBuf, sizeof(_findBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
		findNext(io.KeyShift);
		ImGui::SetKeyboardFocusHere(-1);
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Up) || (keys && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F3, false))) {
		findNext(true);
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Down) || (keys && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F3, false))) {
		findNext(false);
	}
	ImGui::SameLine();
	if (ImGui::Button("Go to line") || (keys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))) {
		_gotoLine = ed.GetCursorPosition().mLine + 1;
		_gotoOpen = true;
		ImGui::OpenPopup("gotoline");
	}
	if (ImGui::BeginPopup("gotoline")) {
		ImGui::SetNextItemWidth(120);
		if (_gotoOpen) {
			ImGui::SetKeyboardFocusHere();
			_gotoOpen = false;
		}
		if (ImGui::InputInt("Line", &_gotoLine, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
			ed.ScrollToLine(std::clamp(_gotoLine, 1, ed.GetTotalLines()) - 1);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Fold all")) {
		ed.FoldAll();
	}
	ImGui::SameLine();
	if (ImGui::Button("Unfold all")) {
		ed.UnfoldAll();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Outline", &d.showOutline);
	ImGui::SameLine();
	if (ImGui::Checkbox("Light", &_editorLight)) {
		for (auto& x : _docs) {
			x->editor.SetPalette(PapyrusPalette(_editorLight));
		}
		saveSettings();
	}

	// ---- where it came from
	if (d.decompiled) {
		const std::string from = !d.pexFile.empty() ? platform::ToUtf8(d.pexFile) : d.pex ? PexWhere(*d.pex) : std::string("?");
		ImGui::TextDisabled("Decompiled from %s. Comments and the original layout are not stored in a .pex.%s", from.c_str(),
			d.psc.empty() ? " Save it as a .psc to keep it." : "");
		if (!d.decompileWarnings.empty()) {
			TextColored(kWarn, Fmt("%zu part(s) could not be rebuilt cleanly (marked ; in the code).", d.decompileWarnings.size()));
		}
	} else {
		ImGui::TextDisabled("%s%s", platform::ToUtf8(d.psc).c_str(), d.sourceMod.empty() ? "" : ("   (" + d.sourceMod + ")").c_str());
		if (d.pex) {
			ImGui::SameLine();
			ImGui::TextDisabled("   compiled: %s", PexWhere(*d.pex).c_str());
		}
	}
	if (!d.note.empty()) {
		TextColored(kWarn, d.note);
	}
	if (!d.analysis.problems.empty()) {
		TextColored(kWarn, "Blocks: " + d.analysis.problems.front() + (d.analysis.problems.size() > 1 ? Fmt("  (+%zu more)", d.analysis.problems.size() - 1) : ""));
	}

	if (!ImGui::BeginTabBar("views")) {
		return;
	}
	if (ImGui::BeginTabItem("Source")) {
		const float bottom = d.compiled ? 150.0f : 0.0f;
		const float statusH = ImGui::GetTextLineHeightWithSpacing();
		const float h = ImGui::GetContentRegionAvail().y - bottom - statusH - (d.compiled ? ImGui::GetStyle().ItemSpacing.y : 0);

		// outline
		if (d.showOutline) {
			ImGui::BeginChild("outline", ImVec2(230, h), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
			const auto cur = ed.GetCursorPosition().mLine;
			const char* kinds[] = { "P", "F", "E", "S", "V" };
			const ImU32 kc[] = { IM_COL32(86, 200, 180, 255), IM_COL32(255, 207, 175, 255), IM_COL32(223, 196, 125, 255), IM_COL32(206, 223, 153, 255),
				IM_COL32(170, 170, 170, 255) };
			for (std::size_t i = 0; i < d.analysis.outline.size(); ++i) {
				const auto& it = d.analysis.outline[i];
				ImGui::PushID(static_cast<int>(i));
				const int t = static_cast<int>(it.type);
				ImGui::PushStyleColor(ImGuiCol_Text, kc[t]);
				ImGui::TextUnformatted(kinds[t]);
				ImGui::PopStyleColor();
				ImGui::SameLine();
				const std::string label = (it.state.empty() || it.type == psc::OutlineItem::Type::kState ? "" : it.state + ".") + it.name + (it.native ? "  (native)" : "");
				if (ImGui::Selectable(label.c_str(), it.line == cur)) {
					ed.ScrollToLine(it.line);
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
					ImGui::SetTooltip("%s", it.detail.c_str());
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::SameLine();
		}

		// error markers from the last compile
		TextEditor::ErrorMarkers marks;
		for (const auto& m : d.messages) {
			const auto stem = Lower(platform::ToUtf8(platform::FromUtf8(m.file).stem()));
			if (stem == Lower(d.name) && m.line > 0) {
				auto& s = marks[m.line];
				s += (s.empty() ? "" : "\n") + m.text;
			}
		}
		ed.SetErrorMarkers(marks);

		// matching block keywords + find matches
		{
			std::vector<TextEditor::Highlight> hl;
			const auto                         cur = ed.GetCursorPosition();
			const int                          idx = ed.CharIndex(cur);
			if (const auto* b = psc::BlockAt(d.analysis, cur.mLine, idx)) {
				for (const auto& w : b->keywords) {
					hl.push_back({ w.line, w.begin, w.end, _editorLight ? IM_COL32(255, 200, 0, 110) : IM_COL32(255, 220, 80, 70) });
				}
			}
			const std::string needle = Lower(_findBuf);
			if (needle.size() >= 2) {
				if (d.linesDirty) {
					d.lines = ed.GetTextLines();
					d.linesDirty = false;
				}
				for (int l = 0; l < static_cast<int>(d.lines.size()) && hl.size() < 5000; ++l) {
					const auto hay = Lower(d.lines[static_cast<std::size_t>(l)]);
					for (auto at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) {
						hl.push_back({ l, static_cast<int>(at), static_cast<int>(at + needle.size()), _editorLight ? IM_COL32(120, 200, 255, 110) : IM_COL32(90, 160, 255, 80) });
					}
				}
			}
			ed.SetHighlights(std::move(hl));
		}

		// the editor (fixed-width font; Ctrl+mouse wheel zooms)
		ImGui::BeginGroup();
		if (monoFont) {
			monoFont->Scale = d.zoom;
			ImGui::PushFont(monoFont);
		}
		ed.Render("##code", ImVec2(0, h));
		const bool hovered = ImGui::IsItemHovered();
		if (monoFont) {
			ImGui::PopFont();
			monoFont->Scale = 1.0f;
		}
		if (hovered && io.KeyCtrl && io.MouseWheel != 0) {
			d.zoom = std::clamp(d.zoom + io.MouseWheel * 0.1f, 0.6f, 2.5f);
		}
		if (ed.IsTextChanged()) {
			d.textModified = true;
			d.analysisDirty = true;
			d.linesDirty = true;
			d.editTime = ImGui::GetTime();
			if (d.threadLine >= 0) {
				d.threadLine = -1;
				ed.SetBreakpoints({});
			}
		}
		// Ctrl+double-click on a script name opens it (as in npp-papyrus)
		TextEditor::Coordinates at;
		if (ed.TakeCtrlDoubleClick(at)) {
			const auto line = ed.LineText(at.mLine);
			int        i = ed.CharIndex(at), b = i, e = i;
			auto       isw = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':'; };
			while (b > 0 && isw(line[static_cast<std::size_t>(b - 1)])) {
				--b;
			}
			while (e < static_cast<int>(line.size()) && isw(line[static_cast<std::size_t>(e)])) {
				++e;
			}
			if (e > b) {
				const auto word = line.substr(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b));
				if (d.ctx->scripts.contains(Lower(word)) || _sources.sourceFiles.contains(Lower(word))) {
					viewScript(word);
				} else if (const int fl = psc::FindFunction(d.analysis, word); fl >= 0) {
					ed.ScrollToLine(fl);  // a function of this script
				} else {
					_status = word + " is not a known script or a function of " + d.name;
				}
			}
		}
		const auto pos = ed.GetCursorPosition();
		ImGui::TextDisabled("Ln %d, Col %d   |   %d lines   |   %s%s   |   Ctrl+double-click a script name to open it, Ctrl+wheel to zoom", pos.mLine + 1, pos.mColumn + 1,
			ed.GetTotalLines(), d.textModified ? "modified" : d.psc.empty() ? "not saved yet" : "saved", d.compiling ? "   |   compiling..." : "");
		ImGui::EndGroup();

		// compiler messages
		if (d.compiled) {
			ImGui::BeginChild("compile", ImVec2(0, 0), ImGuiChildFlags_Borders);
			TextColored(d.compileOk ? kGood : kBad, d.summary);
			for (std::size_t i = 0; i < d.messages.size(); ++i) {
				const auto& m = d.messages[i];
				const auto  stem = platform::ToUtf8(platform::FromUtf8(m.file).stem());
				const bool  here = Lower(stem) == Lower(d.name);
				ImGui::PushID(static_cast<int>(i));
				const std::string label = (here ? std::string() : stem + ".psc ") + Fmt("line %d, col %d:  ", m.line, m.column) + m.text;
				if (ImGui::Selectable(label.c_str())) {
					if (here) {
						ed.ScrollToLine(m.line - 1);
						ed.SetCursorPosition(TextEditor::Coordinates(std::max(0, m.line - 1), std::max(0, ed.ColumnOfIndex(m.line - 1, m.column))));
					} else if (stem != "<unknown>") {
						viewScript(stem);
						for (auto& x : _docs) {
							if (Lower(x->name) == Lower(stem)) {
								x->editor.ScrollToLine(m.line - 1);
							}
						}
					}
				}
				ImGui::PopID();
			}
			if (d.messages.empty() && !d.compileOk && !d.output.empty()) {
				for (const auto& l : d.output) {
					ImGui::TextUnformatted(l.c_str());
				}
			}
			if (ImGui::TreeNode("Compiler output")) {
				ImGui::TextWrapped("%s", d.commandLine.c_str());
				for (const auto& l : d.output) {
					ImGui::TextUnformatted(l.c_str());
				}
				ImGui::TreePop();
			}
			ImGui::EndChild();
		}
		ImGui::EndTabItem();
	}

	// background re-analysis after edits (folds, outline, properties)
	if (d.analysisDirty && ImGui::GetTime() - d.editTime > 0.3) {
		d.analysisDirty = false;
		d.analysis = psc::Analyze(ed.GetTextLines());
		d.textModified = ed.GetText() != d.savedText;  // undone back to the saved text
		std::vector<TextEditor::FoldRegion> folds;
		for (const auto& b : d.analysis.blocks) {
			if (b.endLine > b.startLine) {
				folds.push_back({ b.startLine, b.endLine });
			}
		}
		ed.SetFoldRegions(folds);
		if (d.analysis.properties != d.ctx->properties) {
			d.ctx->properties = d.analysis.properties;
			ed.SetLanguageDefinition(PapyrusLanguage(d.ctx));
		}
	}

	const bool canDisasm = d.pex.has_value() || !d.pexFile.empty();
	if (canDisasm && ImGui::BeginTabItem("Disassembly")) {
		if (d.disasm.empty() && d.disasmError.empty()) {
			try {
				if (!d.pexLoaded) {
					if (!d.pexFile.empty()) {
						std::ifstream in(d.pexFile, std::ios::binary);
						d.pexBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
					} else {
						d.pexBytes = ScriptSources::readPex(*d.pex);
					}
					d.pexLoaded = true;
				}
				d.disasm = pex::Disassemble(pex::Read(d.pexBytes));
			} catch (const std::exception& e) {
				d.disasmError = e.what();
			}
		}
		if (!d.disasmError.empty()) {
			TextColored(kBad, d.disasmError);
		} else {
			if (ImGui::Button("Copy")) {
				std::string all;
				for (const auto& l : d.disasm) {
					all += l + "\n";
				}
				ImGui::SetClipboardText(all.c_str());
			}
			ImGui::SameLine();
			ImGui::TextDisabled("The compiled script the game loads, instruction by instruction.");
			if (monoFont) {
				ImGui::PushFont(monoFont);
			}
			ImGui::BeginChild("asm", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
			ImGuiListClipper clip;
			clip.Begin(static_cast<int>(d.disasm.size()));
			while (clip.Step()) {
				for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
					const auto& l = d.disasm[static_cast<std::size_t>(i)];
					const auto  t = l.find_first_not_of(' ');
					if (t != std::string::npos && l[t] == ';') {
						ImGui::PushStyleColor(ImGuiCol_Text, kDim);
						ImGui::TextUnformatted(l.c_str());
						ImGui::PopStyleColor();
					} else {
						ImGui::TextUnformatted(l.c_str());
					}
				}
			}
			ImGui::EndChild();
			if (monoFont) {
				ImGui::PopFont();
			}
		}
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

bool App::saveDoc(ScriptDoc& d, bool a_saveAs)
{
	std::filesystem::path target = d.psc;
	if (a_saveAs || target.empty()) {
		// a decompiled script: next to the mod's other sources (<mod>\Source\Scripts)
		std::filesystem::path suggested;
		if (!d.psc.empty()) {
			suggested = d.psc;
		} else if (d.pex && d.pex->mod != "Game Data" && !_sources.folderPath(d.pex->mod).empty()) {
			suggested = _sources.folderPath(d.pex->mod) / "Source" / "Scripts" / (d.name + ".psc");
		} else if (!_sources.overwriteDir.empty()) {
			suggested = _sources.overwriteDir / "Source" / "Scripts" / (d.name + ".psc");
		} else if (!d.pexFile.empty()) {
			suggested = d.pexFile.parent_path() / (d.name + ".psc");
		} else {
			suggested = platform::SettingsFile().parent_path() / (d.name + ".psc");
		}
		if (platform::HasNativeDialogs()) {
			auto p = platform::SaveFileDialog(suggested, "psc", "Save Papyrus source");
			if (!p) {
				return false;
			}
			target = *p;
		} else {
			target = suggested;
		}
	}
	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	// keep the original the first time an existing source is overwritten
	auto bak = target;
	bak += ".bak";
	if (std::filesystem::exists(target, ec) && !std::filesystem::exists(bak, ec)) {
		std::filesystem::copy_file(target, bak, ec);
	}
	std::string text = d.editor.GetText();
	if (!text.empty() && text.back() != '\n') {
		text += "\n";
	}
	if (d.crlf) {
		std::string t;
		t.reserve(text.size() + text.size() / 30);
		for (char c : text) {
			if (c == '\n') {
				t += "\r\n";
			} else {
				t += c;
			}
		}
		text.swap(t);
	}
	std::ofstream out(target, std::ios::binary | std::ios::trunc);
	out << text;
	if (!out) {
		_error = "Could not write " + platform::ToUtf8(target);
		return false;
	}
	d.psc = target;
	d.savedText = d.editor.GetText();
	d.textModified = false;
	d.decompiled = false;  // it is a source file now
	_status = "Saved " + platform::ToUtf8(target);
	return true;
}

// ==========================================================================================
// compiling
// ==========================================================================================

std::filesystem::path App::gameFolder() const
{
	if (_mo2 && !_mo2->gamePath.empty()) {
		return _mo2->gamePath;
	}
	if (!_gamePath.empty()) {
		return platform::FromUtf8(_gamePath);
	}
	if (!_sources.gameData.empty()) {
		return _sources.gameData.parent_path();
	}
	if (_sources.gameDataScanned && !_sources.root.empty()) {
		return _sources.root.parent_path();
	}
	return {};
}

std::filesystem::path App::compilerPath() const
{
	if (!_compilerSetting.empty()) {
		return platform::FromUtf8(_compilerSetting);
	}
	if (auto p = papc::FindCompiler(gameFolder())) {
		return *p;
	}
	return {};
}

std::filesystem::path App::compileOutputFolder(const ScriptDoc& d) const
{
	const auto overwrite = !_sources.overwriteDir.empty() ? _sources.overwriteDir / "Scripts"
						 : !_sources.gameData.empty()     ? _sources.gameData / "Scripts"
														  : std::filesystem::path();
	if (_compileOutMode == 2 && !_compileOutDir.empty()) {
		return platform::FromUtf8(_compileOutDir);
	}
	if (_compileOutMode == 1 && !overwrite.empty()) {
		return overwrite;
	}
	// the mod the script belongs to: <mod>\Source\Scripts\x.psc (or <mod>\Scripts\Source) -> <mod>\Scripts
	if (!d.psc.empty() && d.sourceMod != "Game Data") {
		const auto dir = d.psc.parent_path();
		const auto a = Lower(platform::ToUtf8(dir.filename())), b = Lower(platform::ToUtf8(dir.parent_path().filename()));
		if ((a == "scripts" && b == "source") || (a == "source" && b == "scripts")) {
			return dir.parent_path().parent_path() / "Scripts";
		}
		return dir;
	}
	if (d.pex && d.pex->mod != "Game Data") {
		return d.pex->bsaEntry.empty() ? d.pex->file.parent_path() : _sources.folderPath(d.pex->mod) / "Scripts";
	}
	if (!d.pexFile.empty()) {
		return d.pexFile.parent_path();
	}
	return overwrite.empty() ? std::filesystem::temp_directory_path() / "ReSaverPP" / "compiled" : overwrite;
}

void App::startCompile(ScriptDoc& d)
{
	if (_compileJob) {
		return;
	}
	d.compiled = true;
	d.compileOk = false;
	d.messages.clear();
	d.output.clear();
	d.commandLine.clear();
	const auto compiler = compilerPath();
	std::error_code ec;
	if (compiler.empty() || !std::filesystem::exists(compiler, ec)) {
		d.summary = "PapyrusCompiler.exe was not found. It comes with the Creation Kit (in the game folder, under \"Papyrus Compiler\"). "
					"Set its location in Compile settings.";
		_showCompileSettings = true;
		return;
	}
	// the script name the compiler expects is the ScriptName line
	if (d.analysisDirty) {
		d.analysis = psc::Analyze(d.editor.GetTextLines());
		d.analysisDirty = false;
	}
	const std::string script = d.analysis.scriptName.empty() ? d.name : d.analysis.scriptName;

	// the text as shown (saved or not) goes to a work folder, which is searched first
	const auto work = std::filesystem::temp_directory_path() / "ReSaverPP" / "compile";
	std::filesystem::remove_all(work, ec);
	std::filesystem::create_directories(work, ec);
	const auto tmp = work / (script + ".psc");
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		for (const auto& l : d.editor.GetTextLines()) {
			out << l << "\r\n";
		}
		if (!out) {
			d.summary = "Could not write " + platform::ToUtf8(tmp);
			return;
		}
	}

	papc::Request req;
	req.compiler = compiler;
	req.source = tmp;
	req.flags = _compileFlags.empty() ? "TESV_Papyrus_Flags.flg" : _compileFlags;
	req.optimize = _compileOptimize;
	req.anonymize = _compileAnonymize;
	req.output = compileOutputFolder(d);
	std::vector<std::string> seen;
	auto                     add = [&](const std::filesystem::path& p) {
        if (p.empty()) {
            return;
        }
        const auto k = Lower(platform::ToUtf8(p));
        if (std::find(seen.begin(), seen.end(), k) == seen.end() && std::filesystem::is_directory(p, ec)) {
            seen.push_back(k);
            req.imports.push_back(p);
        }
	};
	add(work);
	std::size_t start = 0;
	for (std::size_t i = 0; i <= _compileImports.size(); ++i) {
		if (i == _compileImports.size() || _compileImports[i] == '\n' || _compileImports[i] == ';') {
			auto part = _compileImports.substr(start, i - start);
			while (!part.empty() && (part.back() == '\r' || part.back() == ' ')) {
				part.pop_back();
			}
			add(platform::FromUtf8(part));
			start = i + 1;
		}
	}
	if (!d.psc.empty()) {
		add(d.psc.parent_path());
	}
	for (const auto& [dir, enabled] : _sources.sourceDirs) {
		if (enabled) {
			add(dir);
		}
	}
	const auto data = gameFolder() / "Data";
	add(data / "Source" / "Scripts");
	add(data / "Scripts" / "Source");
	for (const auto& [dir, enabled] : _sources.sourceDirs) {
		if (!enabled) {
			add(dir);
		}
	}
	bool flagsFound = false;
	for (const auto& dir : req.imports) {
		flagsFound = flagsFound || std::filesystem::exists(dir / req.flags, ec);
	}
	if (!flagsFound) {
		d.summary = req.flags + " was not found in any import folder. It is in the game's Data\\Source\\Scripts folder once the Creation Kit's "
							   "Scripts.zip is unpacked there (that also gives the compiler the vanilla sources it needs).";
		return;
	}
	if (_compileBackup) {
		const auto pex = req.output / (script + ".pex");
		auto       bak = pex;
		bak += ".bak";
		if (std::filesystem::exists(pex, ec) && !std::filesystem::exists(bak, ec)) {
			std::filesystem::copy_file(pex, bak, ec);
		}
	}

	auto job = std::make_shared<CompileJob>();
	for (const auto& x : _docs) {
		if (x.get() == &d) {
			job->doc = x;
		}
	}
	job->script = script;
	job->output = req.output;
	CompileJob* raw = job.get();
	job->thread = std::thread([raw, req] {
		raw->result = papc::Compile(req);
		raw->done = true;
	});
	_compileJob = job;
	d.compiling = true;
	d.summary = "Compiling " + script + "...";
	_status = d.summary;
}

void App::pollCompile()
{
	if (!_compileJob || !_compileJob->done) {
		return;
	}
	auto job = _compileJob;
	_compileJob.reset();
	job->thread.join();
	auto doc = job->doc.lock();
	if (!doc) {
		return;
	}
	auto& d = *doc;
	auto& r = job->result;
	d.compiling = false;
	d.compileOk = r.ok;
	d.messages = r.messages;
	d.output = r.output;
	d.commandLine = r.commandLine;
	if (!r.ran) {
		d.summary = "The compiler could not be run: " + r.error;
	} else if (r.ok) {
		d.summary = "Compiled " + job->script + ".pex into " + platform::ToUtf8(job->output) + ". The game uses it the next time it starts.";
		if (r.importsDropped) {
			d.summary += Fmt(" (%zu import folders were left out: the command line was too long)", r.importsDropped);
		}
		if (!r.error.empty()) {
			d.summary += "  " + r.error;
		}
	} else if (!r.messages.empty()) {
		d.summary = Fmt("%zu error(s): click one to go to it.", r.messages.size());
	} else {
		d.summary = "The compiler did not write " + job->script + ".pex (see the compiler output below).";
	}
	_status = d.summary;
}

void App::stopScriptJobs()
{
	if (_compileJob && _compileJob->thread.joinable()) {
		_compileJob->thread.join();
	}
	_compileJob.reset();
}

void App::drawCompileSettings()
{
	ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Compile settings", &_showCompileSettings, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	bool changed = false;
	auto pathInput = [&](const char* label, std::string& value, const char* hint) {
		char buf[1024];
		std::snprintf(buf, sizeof(buf), "%s", value.c_str());
		ImGui::SetNextItemWidth(560);
		if (ImGui::InputTextWithHint(label, hint, buf, sizeof(buf))) {
			value = buf;
			changed = true;
		}
	};
	ImGui::SeparatorText("Papyrus compiler");
	const auto detected = papc::FindCompiler(gameFolder());
	pathInput("##compiler", _compilerSetting, detected ? platform::ToUtf8(*detected).c_str() : "path to PapyrusCompiler.exe");
	ImGui::SameLine();
	if (ImGui::Button("Browse...##c") && platform::HasNativeDialogs()) {
		if (auto p = platform::OpenFileDialog(gameFolder(), "exe", "PapyrusCompiler.exe")) {
			_compilerSetting = platform::ToUtf8(*p);
			changed = true;
		}
	}
	if (_compilerSetting.empty()) {
		if (detected) {
			ImGui::TextDisabled("Using the one found in the game folder.");
		} else {
			TextColored(kWarn, "Not found in the game folder. Install the Creation Kit (Steam), or browse to PapyrusCompiler.exe.");
		}
	}
	char flags[128];
	std::snprintf(flags, sizeof(flags), "%s", _compileFlags.c_str());
	ImGui::SetNextItemWidth(260);
	if (ImGui::InputText("Flags file", flags, sizeof(flags))) {
		_compileFlags = flags;
		changed = true;
	}
	changed |= ImGui::Checkbox("Optimize (-op)", &_compileOptimize);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Anonymize the .pex (blank the source path, user and machine names it stores)", &_compileAnonymize);

	ImGui::SeparatorText("Where the compiled .pex goes");
	changed |= ImGui::RadioButton("The script's own mod (<mod>\\Scripts)", &_compileOutMode, 0);
	changed |= ImGui::RadioButton("Mod Organizer's Overwrite folder (Overwrite\\Scripts)", &_compileOutMode, 1);
	changed |= ImGui::RadioButton("This folder:", &_compileOutMode, 2);
	ImGui::SameLine();
	pathInput("##outdir", _compileOutDir, "folder");
	ImGui::SameLine();
	if (ImGui::Button("Browse...##o") && platform::HasNativeDialogs()) {
		if (auto p = platform::PickFolderDialog("Folder for compiled scripts")) {
			_compileOutDir = platform::ToUtf8(*p);
			_compileOutMode = 2;
			changed = true;
		}
	}
	changed |= ImGui::Checkbox("Keep a .pex.bak of a compiled script before replacing it (once)", &_compileBackup);

	ImGui::SeparatorText("Extra import folders (searched first; one per line)");
	char imports[4096];
	std::snprintf(imports, sizeof(imports), "%s", _compileImports.c_str());
	if (ImGui::InputTextMultiline("##imports", imports, sizeof(imports), ImVec2(700, 70))) {
		_compileImports = imports;
		changed = true;
	}
	std::size_t enabled = 0;
	for (const auto& [dir, on] : _sources.sourceDirs) {
		enabled += on ? 1 : 0;
	}
	ImGui::TextDisabled("Also searched: the script's folder, the source folders of your %zu active mods (in load priority), the game's\n"
						"Data\\Source\\Scripts, then inactive mods.",
		enabled);
	if (changed) {
		saveSettings();
	}
	ImGui::End();
}

void App::drawDecompilePrompt()
{
	if (_decompileAsk) {
		ImGui::OpenPopup("Decompile script?");
		_decompileAsk = false;
	}
	ImGui::SetNextWindowSize(ImVec2(620, 0));
	if (!ImGui::BeginPopupModal("Decompile script?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}
	ImGui::TextWrapped("There is no source (.psc) for %s in your mods or the game's Data folder.", _decompileScript.c_str());
	ImGui::TextWrapped("The compiled script is in %s.", _decompileWhere.c_str());
	ImGui::Spacing();
	ImGui::TextWrapped("ReSaver++ can decompile it with its built-in decompiler. The code does what the original does, but comments and the "
					   "original layout are not stored in a .pex.");
	ImGui::Spacing();
	if (ImGui::Checkbox("Always decompile without asking", &_decompileAlways)) {
		saveSettings();
	}
	ImGui::Spacing();
	if (ImGui::Button("Decompile", ImVec2(140, 0))) {
		ImGui::CloseCurrentPopup();
		openDoc(_decompileScript, {}, true, _decompileFn, _decompileState, _decompileInstr);
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void App::viewRowScript(std::uint64_t a_key)
{
	if (!_save) {
		return;
	}
	const auto& p = *_save->papyrus();
	switch (_cat) {
	case Cat::kScripts:
		if (const auto* s = p.findScript(static_cast<TStr>(a_key))) {
			viewScript(p.str(s->name));
		}
		break;
	case Cat::kInstances:
	case Cat::kUnbinds:
		if (const auto* i = p.findInstance(a_key)) {
			viewScript(p.str(i->script));
		}
		break;
	case Cat::kThreads:
		if (const auto* a = p.findActive(static_cast<ThreadID>(a_key)); a && a->data && !a->data->frames.empty()) {
			// the innermost call: where the thread is stopped
			const auto& f = a->data->frames.back();
			viewScript(p.str(f.baseName), p.str(f.event), f.status ? p.str(*f.status) : std::string(), static_cast<int>(f.ptr));
		}
		break;
	case Cat::kMessages:
		if (a_key < p.functionMessages.size() && p.functionMessages[a_key].data) {
			const auto& m = *p.functionMessages[a_key].data;
			viewScript(p.str(m.scriptName), p.str(m.event));
		}
		break;
	case Cat::kSuspended:
		{
			const auto  table = static_cast<int>(a_key >> 32);
			const auto  id = static_cast<ThreadID>(a_key & 0xFFFFFFFF);
			const auto& list = table == 1 ? p.suspended1 : p.suspended2;
			for (const auto& s : list) {
				if (s.id == id && s.data) {
					viewScript(p.str(s.data->scriptName), p.str(s.data->event));
				}
			}
			break;
		}
	default:
		break;
	}
}

// ==========================================================================================
// finding Mod Organizer and the game by themselves
// ==========================================================================================

void App::autoDetectPaths(bool a_force)
{
	std::error_code ec;
	if (!a_force && !_sourcesPath.empty()) {
		// already set: just read its Mod Organizer settings (cheap; the scan happens when needed)
		if (!_mo2) {
			_mo2 = FindMo2Instance(platform::FromUtf8(_sourcesPath));
		}
		if (!_mo2 && !_vortex) {
			_vortex = FindVortexDeployment(platform::FromUtf8(_sourcesPath));
			if (_vortex && _gamePath.empty()) {
				_gamePath = platform::ToUtf8(_vortex->dataPath.parent_path());
			}
			// an older version saved the game's Data folder: Vortex users get its staging folder from now on
			if (_vortex && platform::ToUtf8(_vortex->stagingPath) != _sourcesPath) {
				_detectNote = "Found Vortex: the mods folder is now its staging folder (was " + _sourcesPath + ")";
				_sourcesPath = platform::ToUtf8(_vortex->stagingPath);
				saveSettings();
			}
		}
		if (_gamePath.empty()) {
			if (_mo2 && !_mo2->gamePath.empty()) {
				_gamePath = platform::ToUtf8(_mo2->gamePath);
			} else if (const auto games = platform::SkyrimInstallFolders(); !games.empty()) {
				_gamePath = platform::ToUtf8(games.front());
			}
		}
		return;
	}
	// Mod Organizer instances for Skyrim Special Edition (global, or portable next to ReSaver++)
	std::vector<Mo2Instance> skyrim;
	for (auto& m : FindMo2Instances({ platform::ExeFolder() })) {
		const auto g = Lower(m.gameName);
		const bool se = g.empty() ? Lower(platform::ToUtf8(m.gamePath)).find("skyrim special edition") != std::string::npos
								  : (g.find("skyrim special edition") != std::string::npos || g == "skyrimse" || g == "skyrim se");
		if (se && std::filesystem::is_directory(m.modsDir, ec)) {
			skyrim.push_back(std::move(m));
		}
	}
	const auto games = platform::SkyrimInstallFolders();
	if (!skyrim.empty()) {
		// several instances: the one whose mod list was changed last
		auto newest = [&](const Mo2Instance& m) {
			std::filesystem::file_time_type t{};
			const auto list = m.profilesDir / platform::FromUtf8(m.selectedProfile) / "modlist.txt";
			if (std::filesystem::exists(list, ec)) {
				t = std::filesystem::last_write_time(list, ec);
			}
			return t;
		};
		std::sort(skyrim.begin(), skyrim.end(), [&](const Mo2Instance& a, const Mo2Instance& b) { return newest(a) > newest(b); });
		const auto& m = skyrim.front();
		_sourcesPath = platform::ToUtf8(m.modsDir);
		_profile = m.selectedProfile;
		_mo2 = m;
		_gamePath = platform::ToUtf8(!m.gamePath.empty() ? m.gamePath : games.empty() ? std::filesystem::path() : games.front());
		_detectNote = "Found Mod Organizer (" + platform::ToUtf8(m.ini.parent_path()) + "), profile \"" + m.selectedProfile + "\"" +
					  (skyrim.size() > 1 ? Fmt(" (%zu Mod Organizer setups for Skyrim SE found: the most recently used one was picked)", skyrim.size()) : "");
	} else if (const auto vx = [&]() -> std::optional<VortexDeployment> {
				   // Vortex: its deployment manifest in a game's Data folder, or in its default staging folder
				   for (const auto& g : games) {
					   if (auto v = FindVortexDeployment(g)) {
						   return v;
					   }
				   }
				   if (const char* ad = std::getenv("APPDATA")) {
					   return FindVortexDeployment(platform::FromUtf8(ad) / "Vortex" / "skyrimse" / "mods");
				   }
				   return std::nullopt;
			   }()) {
		_vortex = vx;
		_gamePath = platform::ToUtf8(vx->dataPath.parent_path());
		_sourcesPath = platform::ToUtf8(vx->stagingPath);
		_detectNote = "Found Vortex: staging folder " + _sourcesPath + Fmt(", %zu mods deployed", vx->mods.size());
	} else if (!games.empty()) {
		_gamePath = platform::ToUtf8(games.front());
		_sourcesPath = platform::ToUtf8(games.front() / "Data");
		_detectNote = "No Mod Organizer or Vortex found: using the game's Data folder";
	} else {
		_detectNote = "Neither Mod Organizer nor the game was found: set the folders below.";
		return;
	}
	_sources = ScriptSources{};
	_modsDirty = true;
	saveSettings();
	_status = _detectNote + ".";
}

std::filesystem::path App::savesFolder() const
{
	// the folder with the newest save: Documents\My Games\Skyrim Special Edition\Saves, or the MO2 profile's own saves
	std::filesystem::path           best = platform::DefaultSavesFolder();
	std::filesystem::file_time_type bestTime{};
	std::error_code                 ec;
	for (const auto& dir : watchFolders()) {
		for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
			if (e.is_regular_file(ec) && Lower(platform::ToUtf8(e.path().extension())) == ".ess") {
				const auto t = e.last_write_time(ec);
				if (t > bestTime) {
					bestTime = t;
					best = dir;
				}
			}
		}
	}
	return best;
}
