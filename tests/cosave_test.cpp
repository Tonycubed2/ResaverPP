// ReSaver++ co-save tests (core/Cosave.h): synthetic SKSE (LE and SE) and F4SE co-saves, PapyrusUtil records,
// cleaning, the DLL id search; with RESAVERPP_COSAVES="dir;dir" every .skse / .f4se file there must read back
// byte for byte and clean cleanly.
#include "core/ByteIO.h"
#include "core/Cosave.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ess;
using namespace ess::cosave;

namespace
{
	int g_fail = 0, g_pass = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (a_ok) {
			++g_pass;
		} else {
			++g_fail;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	constexpr std::uint32_t MC(const char* s)
	{
		std::uint32_t v = 0;
		for (; *s; ++s) {
			v = (v << 8) | static_cast<std::uint8_t>(*s);
		}
		return v;
	}

	std::vector<std::uint8_t> Bytes(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

	std::vector<std::uint8_t> Plgn(const std::vector<std::tuple<std::uint8_t, std::uint16_t, std::string>>& a_list)
	{
		Writer w;
		w.u16(static_cast<std::uint16_t>(a_list.size()));
		for (const auto& [i, l, n] : a_list) {
			w.u8(i);
			if (i == 0xFE) {
				w.u16(l);
			}
			w.u16(static_cast<std::uint16_t>(n.size()));
			w.raw(n.data(), n.size());
		}
		return w.buf;
	}

	// a co-save written the way Serialization.cpp HandleSaveGlobalData writes it
	std::vector<std::uint8_t> Make(std::uint32_t a_sig, std::uint32_t a_xse, std::uint32_t a_rt,
		const std::vector<std::pair<std::uint32_t, std::vector<Chunk>>>& a_plugins)
	{
		Writer w;
		w.u32(a_sig);
		w.u32(1);
		w.u32(a_xse);
		w.u32(a_rt);
		w.u32(static_cast<std::uint32_t>(a_plugins.size()));
		for (const auto& [uid, chunks] : a_plugins) {
			std::size_t len = 0;
			for (const auto& c : chunks) {
				len += 12 + c.data.size();
			}
			w.u32(uid);
			w.u32(static_cast<std::uint32_t>(chunks.size()));
			w.u32(static_cast<std::uint32_t>(len));
			for (const auto& c : chunks) {
				w.u32(c.type);
				w.u32(c.version);
				w.u32(static_cast<std::uint32_t>(c.data.size()));
				w.bytes(c.data);
			}
		}
		return w.buf;
	}

	constexpr std::uint32_t kSigSKSE = 0x45534B53, kSigF4SE = 0x45533446;

	// PapyrusUtil text as Data.cpp SaveStream writes it ("<count>" " <obj> <n>" " <key> <value>")
	const char* kStrv = "6 0 2 global\x07key hello other \x1b 266287972421 1 name Lydia 266304751616 2 a x b y 270549391360 1 n light 270566164480 1 made "
						"\x1b";  // 6 = 5 objects + one empty object PapyrusUtil counted but did not write
	const char* kIntl = "2 0 1 list 3 1 2 3 266304751617 1 gonelist 2 7 8";

	std::vector<std::uint8_t> SampleSE()
	{
		const auto plgn = Plgn({ { 0, 0, "Skyrim.esm" }, { 1, 0, "Gone.esp" }, { 0xFE, 1, "Light.esl" } });
		return Make(kSigSKSE, 0x02020060, 0x01064920,
			{
				{ 0, { { MC("PLGN"), 0, plgn }, { MC("MENR"), 1, {} }, { MC("REGE"), 1, {} } } },
				{ MC("SKEE"), { { MC("STTB"), 3, Bytes("abc") }, { MC("MRST"), 2, Bytes("1234") } } },
				{ papu::kUid, { { MC("PLGN"), 0, plgn }, { MC("STRV"), 1, Bytes(kStrv) }, { MC("INTL"), 1, Bytes(kIntl) }, { MC("PKGO"), 1, Bytes("0") } } },
				{ MC("JSTR"), { { MC("JSTR"), 6, Bytes("{\"commonVersion\": 6}rest") } } },
			});
	}

	void TestIds()
	{
		Check(IdText(MC("SKEE")) == "'SKEE'", "IdText SKEE");
		Check(IdText(MC("ECF")) == "'ECF'", "IdText 3-char ECF");
		Check(IdText(0x54534F00) == "54534F00 (byteswap 'OST')", "IdText byteswapped OST");
		Check(IdText(0x424510A2).rfind("424510A2", 0) == 0, "IdText hex");
		Check(papu::kUid == 0x424510A2, "PapyrusUtil uid = 884715692 + 227106806");
		Check(Known(MC("SKEE"), false) && !Known(MC("SKEE"), true), "SKEE is Skyrim only");
		Check(Known(MC("F4EE"), true) && !Known(MC("F4EE"), false), "F4EE is Fallout 4 only");
		Check(Known(0, true) && Known(0, false), "id 0 both games");
		Check(PackedVersion(0x01064920) == "1.6.1170", "packed runtime 1.6.1170");
		Check(PackedVersion(0x09200000) == "9.32.0", "packed LE runtime 9.32.0");
		Check(PackedVersion(0x02020060) == "2.2.6", "packed SKSE 2.2.6");
		std::set<std::uint32_t> uids;
		for (const auto& k : KnownTable()) {
			Check(uids.insert(k.uid * 4 + static_cast<std::uint32_t>(k.game)).second, k.name);  // no id twice for a game
		}
	}

	void TestSE()
	{
		const auto bytes = SampleSE();
		auto       f = Parse(bytes);
		Check(!f.fo4 && f.plugins.size() == 4 && f.notes.empty(), "SE: 4 plugins, no notes");
		Check(Write(f) == bytes, "SE: byte-identical");
		Check(f.plugins[1].uid == MC("SKEE") && f.plugins[1].chunks.size() == 2 && f.plugins[1].fileSize() == 12 + 12 + 3 + 12 + 4, "SE: SKEE block");
		std::vector<PlgnEntry> pl;
		Check(ReadPlgn(f.plugins[0].chunks[0], pl) && pl.size() == 3 && pl[2].index == 0xFE && pl[2].light == 1 && pl[2].name == "Light.esl", "PLGN read");
		Check(ChunkLabel(0, MC("MCBR"), false).find("mod event") != std::string::npos, "label MCBR");
		Check(jc::Header(f) == "{\"commonVersion\": 6}", "JContainers header");

		// PapyrusUtil
		const auto rec = papu::ParseRecord(f.plugins[2].chunks[1]);
		Check(rec.ok && rec.declared == 6 && rec.objects.size() == 5, "STRV: 6 declared, 5 written");
		Check(rec.objects.size() == 5 && rec.objects[0].key == 0 && rec.objects[0].keys == 2, "STRV: global object with 2 keys");
		Check(papu::Owner(266287972421ull, pl) == "Skyrim.esm", "owner Skyrim.esm");
		Check(papu::Owner(266304751616ull, pl) == "Gone.esp", "owner Gone.esp");
		Check(papu::Owner(270549391360ull, pl) == "Light.esl", "owner Light.esl (FE001)");
		Check(papu::Owner(270566164480ull, pl).empty(), "owner FF = made in game");
		bool sawKey = false;
		for (const auto& k : rec.keys) {
			sawKey = sawKey || k.name == "global key";
		}
		Check(sawKey, "STRV: key with a space decoded");
		const auto il = papu::ParseRecord(f.plugins[2].chunks[2]);
		Check(il.ok && il.list && il.objects.size() == 2 && il.objects[0].values == 3, "INTL: list values");
		const auto b = papu::Analyze(f);
		Check(b.present && b.records.size() == 2 && b.plgn.size() == 3, "PapyrusUtil breakdown");

		// view without DLLs
		const auto v = MakeView(f, {}, false);
		Check(v.rows.size() == 4 && v.rows[0].status == Status::kExtender && v.rows[1].status == Status::kUnknown, "view without DLL search");
		Check(!Report(f, v, 5, true).empty(), "report");

		// clean: SKEE + Gone.esp + Light.esl objects
		CleanOptions opt;
		opt.removeUids = { MC("SKEE"), 0 };  // 0 is never removed
		opt.gonePlugins = { "gone.esp", "light.esl" };
		CleanResult r;
		const auto  out = CleanBytes(bytes, opt, &r);
		const auto  g = Parse(out);
		Check(r.plugins == 1 && g.plugins.size() == 3 && g.numPlugins == 3 && !g.find(MC("SKEE")) && g.find(0), "clean: SKEE removed, id 0 kept");
		Check(r.papuObjects == 3, "clean: 3 PapyrusUtil objects removed (2 STRV, 1 INTL)");
		const auto* pu = g.find(papu::kUid);
		Check(pu && pu->chunks.size() == 4, "clean: PapyrusUtil records kept");
		if (pu && pu->chunks.size() == 4) {
			const std::string strv(pu->chunks[1].data.begin(), pu->chunks[1].data.end());
			Check(strv == "4 0 2 global\x07key hello other \x1b 266287972421 1 name Lydia 270566164480 1 made \x1b", "clean: STRV text exact");
			const std::string intl(pu->chunks[2].data.begin(), pu->chunks[2].data.end());
			Check(intl == "1 0 1 list 3 1 2 3", "clean: INTL text exact");
			Check(pu->chunks[3].data == Bytes("0"), "clean: PKGO untouched");
		}
		Check(g.plugins.back().uid == MC("JSTR") && g.plugins.back().chunks[0].data == f.plugins[3].chunks[0].data, "clean: other plugins untouched");

		// every object of a record on gone plugins: the record goes
		CleanOptions all;
		all.gonePlugins = { "gone.esp" };
		auto f2 = Parse(bytes);
		Clean(f2, all);
		const auto* pu2 = f2.find(papu::kUid);
		const auto  il2 = papu::ParseRecord(pu2->chunks[2]);
		Check(il2.ok && il2.objects.size() == 1, "clean: INTL keeps the global object");

		// nothing to do: the same bytes
		CleanOptions none;
		none.removeUids = { MC("ZZZZ") };
		Check(CleanBytes(bytes, none) == bytes, "clean with nothing matching = same bytes");
	}

	void TestLE()
	{
		// SKSE 1.7.3 for TESV 1.9.32.0: MAKE_SKYRIM_VERSION(1,7,3) and (9,32,0); the same file format
		const auto bytes = Make(kSigSKSE, 0x01070030, 0x09200000,
			{ { 0, { { MC("MODS"), 0, Bytes("\x01\x0a\x00Skyrim.esm") } } }, { MC("NIOV"), { { MC("AAAA"), 1, Bytes("x") } } } });
		auto f = Parse(bytes);
		Check(!f.fo4 && f.plugins.size() == 2 && Write(f) == bytes, "LE: round trip");
		Check(PackedVersion(f.xseVersion) == "1.7.3", "LE: SKSE 1.7.3");
		Check(Known(MC("NIOV"), false) != nullptr, "LE: NiOverride known");
	}

	void TestF4()
	{
		const auto plgn = Plgn({ { 0, 0, "Fallout4.esm" } });
		const auto bytes = Make(kSigF4SE, 0x00070090, 0x010B0F00,
			{ { 0, { { MC("PLGN"), 0, plgn }, { MC("KEYR"), 1, {} } } }, { MC("F4EE"), { { MC("OVRD"), 1, Bytes("12345678") } } },
				{ MC("MCM"), { { MC("MCMD"), 1, Bytes("z") } } } });
		auto f = Parse(bytes);
		Check(f.fo4 && f.xseName() == "F4SE" && f.plugins.size() == 3 && Write(f) == bytes, "F4SE: round trip");
		const auto v = MakeView(f, {}, false);
		Check(v.rows[1].name.find("LooksMenu") != std::string::npos && v.rows[2].name.find("Mod Configuration Menu") != std::string::npos, "F4SE: names");
		Check(!papu::Analyze(f).present, "F4SE: no PapyrusUtil breakdown");
		CleanOptions opt;
		opt.removeUids = { MC("F4EE") };
		const auto out = CleanBytes(bytes, opt);
		Check(Parse(out).plugins.size() == 2, "F4SE: clean");
	}

	void TestIrregular()
	{
		auto bytes = SampleSE();
		// header count off, a block whose chunk count is wrong, 5 trailing bytes
		std::uint32_t n = 9;
		std::memcpy(bytes.data() + 16, &n, 4);
		std::uint32_t chunks = 7;
		std::memcpy(bytes.data() + 20 + 4, &chunks, 4);  // the first plugin's numChunks
		bytes.insert(bytes.end(), { 1, 2, 3, 4, 5 });
		const auto f = Parse(bytes);
		Check(f.notes.size() == 3, "irregular: three notes");
		Check(!f.plugins[0].regular && f.trailing.size() == 5, "irregular: block kept raw, trailing kept");
		Check(Write(f) == bytes, "irregular: byte-identical");
		bool threw = false;
		try {
			Parse(Bytes("ESS_SAVEGAME and more bytes"));
		} catch (const std::exception&) {
			threw = true;
		}
		Check(threw, "not a co-save: throws");
		Check(LooksLikeCosave(SampleSE()) && !LooksLikeCosave(Bytes("abc")), "LooksLikeCosave");
	}

	void TestDlls()
	{
		namespace fs = std::filesystem;
		const auto dir = fs::temp_directory_path() / "rs_cosave_test";
		std::error_code ec;
		fs::remove_all(dir, ec);
		auto put = [&](const fs::path& p, const std::vector<std::uint8_t>& d) {
			fs::create_directories(p.parent_path());
			std::ofstream o(p, std::ios::binary);
			o.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
		};
		std::vector<std::uint8_t> skee(200, 0x90), papu(200, 0x90), le(200, 0x90);
		const std::uint32_t       s = MC("SKEE"), p = papu::kUid;
		skee[50] = 0xBA;  // mov edx, 'SKEE'
		std::memcpy(&skee[51], &s, 4);
		papu[60] = 0x41;  // mov r8d, 0x424510A2
		papu[61] = 0xB8;
		std::memcpy(&papu[62], &p, 4);
		le[70] = 0x68;  // push 'SKEE'
		std::memcpy(&le[71], &s, 4);
		put(dir / "mods" / "RaceMenu" / "SKSE" / "Plugins" / "skee64.dll", skee);
		put(dir / "mods" / "PapyrusUtil" / "SKSE" / "Plugins" / "PapyrusUtil.dll", papu);
		put(dir / "mods" / "Other" / "SKSE" / "Plugins" / "other.dll", std::vector<std::uint8_t>(200, 0));
		put(dir / "le" / "SKSE" / "Plugins" / "nio.dll", le);
		put(dir / "le" / "Skyrim.esm", { 0 });

		const auto dlls = ListPluginDlls(nullptr, dir / "mods", false);
		Check(dlls.size() == 3, "ListPluginDlls: 3 DLLs in a mods folder");
		const auto f = Parse(SampleSE());
		const auto v = MakeView(f, dlls, false);
		Check(v.searched && v.dllsSearched == 3, "view: DLLs searched");
		Check(v.rows[1].status == Status::kInstalled && v.rows[1].dlls.size() == 1 && v.rows[1].dlls[0].rfind("RaceMenu:", 0) == 0, "view: SKEE installed (mov edx)");
		Check(v.rows[2].status == Status::kInstalled, "view: PapyrusUtil installed (mov r8d)");
		Check(v.rows[3].status == Status::kNotInstalled && v.notInstalledCount == 1, "view: JContainers not installed");
		auto dis = dlls;
		for (auto& d : dis) {
			d.enabled = d.mod != "RaceMenu";
		}
		Check(MakeView(f, dis, false).rows[1].status == Status::kDisabled, "view: disabled mod");
		// x86 (LE): push imm32 counts, an x64 search does not take it
		const auto le32 = ListPluginDlls(nullptr, dir / "le", false);
		Check(le32.size() == 1 && le32[0].mod.empty(), "ListPluginDlls: a Data folder");
		Check(FindIds(le32, { s }, true).count(s) == 1 && FindIds(le32, { s }, false).count(s) == 0, "FindIds: push imm32 only for x86");
		fs::remove_all(dir, ec);
	}

	// JContainers: an empty object tree as JContainers 4.2.x writes it (Tony's co-save "68.skse"), cut short, and spoiled
	void TestJContainers()
	{
		const char* hex = "32307b22636f6d6d6f6e56657273696f6e223a20367d160000000000000073657269616c697a6174696f6e3a3a617263686976651000040404080100000000000000000002000000000000000000010000000000000000000000000000000000020000000000000000000000010000000000000000010000000000000000000000000000000001000000feffff7f00000000000000000002000000ea32000000000000000000000000000000000000000000000000000000000300000000000000";
		std::vector<std::uint8_t> d;
		for (std::size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
			d.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
		}
		Check(d.size() == 193, "jc: test data");
		const auto b = jc::AnalyzeBytes(d);
		Check(b.ok && b.header == "{\"commonVersion\": 6}" && b.library == 16 && b.objects.empty() && b.watchedForms == 0 && b.queue == 0 &&
				  b.domains.empty(), ("jc: an empty tree is read to its end " + b.error).c_str());
		auto cut = d;
		cut.pop_back();
		const auto c = jc::AnalyzeBytes(cut);
		Check(!c.ok && !c.error.empty(), "jc: a cut archive is refused");
		auto bad = d;
		bad[0x44] = 9;  // tes_context's version (u32 at 0x44, after its tracking byte): 2 -> 9
		const auto x = jc::AnalyzeBytes(bad);
		Check(!x.ok && x.error.find("version") != std::string::npos, ("jc: an unknown class version is refused: " + x.error).c_str());
		auto old = d;
		old[20] = '5';
		Check(!jc::AnalyzeBytes(old).ok, "jc: another commonVersion is refused");
	}

	void TestReal()
	{
		const char* env = std::getenv("RESAVERPP_COSAVES");
		if (!env) {
			std::printf("(cosave: RESAVERPP_COSAVES not set, real co-saves skipped)\n");
			return;
		}
		std::string list = env;
		std::size_t n = 0, jstr = 0;
		for (std::size_t a = 0; a <= list.size();) {
			auto b = list.find(';', a);
			if (b == std::string::npos) {
				b = list.size();
			}
			const std::filesystem::path dir = list.substr(a, b - a);
			a = b + 1;
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				const auto x = e.path().extension().string();
				if (x != ".skse" && x != ".f4se") {
					continue;
				}
				std::ifstream             in(e.path(), std::ios::binary);
				std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				try {
					const auto f = Parse(d);
					Check(Write(f) == d, ("real: byte-identical " + e.path().filename().string()).c_str());
					// remove the biggest plugin block (not id 0) and every PapyrusUtil object of its list's second plugin
					CleanOptions opt;
					std::size_t  big = 0;
					for (const auto& p : f.plugins) {
						if (p.uid && p.fileSize() > big) {
							big = p.fileSize();
							opt.removeUids = { p.uid };
						}
					}
					const auto b2 = papu::Analyze(f);
					if (b2.plgn.size() > 1) {
						opt.gonePlugins = { b2.plgn[1].name };
					}
					CleanResult r;
					const auto  out = CleanBytes(d, opt, &r);
					Check(r.plugins == 1 && out.size() < d.size(), ("real: clean " + e.path().filename().string()).c_str());
					if (f.find(jc::kUid)) {
						const auto j = jc::Analyze(f);
						Check(j.ok, ("real: JContainers read to its end " + e.path().filename().string() + " " + j.error).c_str());
						++jstr;
					}
					++n;
				} catch (const std::exception& ex) {
					Check(false, (e.path().filename().string() + ": " + ex.what()).c_str());
				}
			}
		}
		std::printf("(cosave: %zu real co-saves, %zu with JContainers data)\n", n, jstr);
	}
}

int main()
{
	TestIds();
	TestSE();
	TestLE();
	TestF4();
	TestIrregular();
	TestDlls();
	TestJContainers();
	TestReal();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
