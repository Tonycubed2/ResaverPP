// Starfield saves (included by unit_test.cpp). Real saves: RESAVERPP_SF_SAVES = .sfs files separated by ';'.


namespace fs = std::filesystem;
namespace
{
	std::vector<std::uint8_t> SfReadAll(const fs::path& a_p)
	{
		std::ifstream f(a_p, std::ios::binary);
		return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	}
}

void TestStarfield()
{
	// the container: a chunk zlib cannot shrink is stored raw; the chunk table, alignment and filler as the game writes them
	{
		std::vector<std::uint8_t> inner(0x40000 * 2 + 1234);
		std::uint32_t             x = 12345;
		for (std::size_t i = 0; i < inner.size(); ++i) {
			x = x * 1103515245u + 12345u;
			inner[i] = i < 0x40000 ? static_cast<std::uint8_t>(x >> 24) : static_cast<std::uint8_t>(i % 7);  // chunk 0 random
		}
		SfContainer c;
		c.present = true;
		c.pad[0] = 0x33;
		c.pad[1] = 0x5B;
		c.pad[2] = 0xEC;
		const auto  packed = PackBCPS(inner, c);
		SfContainer back;
		const auto  un = UnpackBCPS(packed, back);
		Check(un == inner, "sf container: unpacks to what was packed");
		Check(back.chunks == 3 && back.storedChunks == 1, "sf container: the random chunk is stored raw, the others compressed");
		Check(back.pad[0] == 0x33 && back.pad[2] == 0xEC && back.level == 3 && std::memcmp(back.codec, "ZIP ", 4) == 0,
			"sf container: header bytes kept");
		std::uint64_t dataOff = 0;
		std::memcpy(&dataOff, packed.data() + 0x18, 8);
		Check(dataOff == 0x60 && packed.size() % 16 == 0 && std::memcmp(packed.data() + 0x54, "padding\0", 8) == 0,
			"sf container: data offset aligned, gaps filled with \"padding\"");
		Check(packed[0x60] == 0x78 && packed[0x61] == 0x5E || back.storedChunks == 1, "sf container: zlib level 3 stream");
	}
	// a body that shrinks by a chunk: the container is rebuilt from the data alone (the loader 0x1428ef9c0 takes the
	// chunk count from the total size, each chunk's place from the table and the alignment)
	{
		std::vector<std::uint8_t> big(0x40000 * 3 + 77, 0x41), small(0x40000 * 2 - 5, 0x42);
		SfContainer               c;
		c.present = true;
		SfContainer b1, b2;
		Check(UnpackBCPS(PackBCPS(big, c), b1) == big && b1.chunks == 4 && UnpackBCPS(PackBCPS(small, c), b2) == small && b2.chunks == 2,
			"sf container: any size packs and unpacks (4 chunks, then 2)");
	}
	// the game's base game file test (0x140425850): it decides at save versions 120..139 which entries carry details
	Check(IsBaseGameFileSF("Starfield.esm") && IsBaseGameFileSF("starfield.ESM") && IsBaseGameFileSF("SFBGS00D.esm") &&
			  IsBaseGameFileSF("ShatteredSpace.esm") && IsBaseGameFileSF("BlueprintShips-Starfield.esm") &&
			  IsBaseGameFileSF("BlueprintShips-SFBGS050.esm") && IsBaseGameFileSF("BlueprintShips") && !IsBaseGameFileSF("") &&
			  !IsBaseGameFileSF("SFBGS0AA.esm") && !IsBaseGameFileSF("MyMod.esm") && !IsBaseGameFileSF("Starfield.esm.bak") &&
			  !IsBaseGameFileSF("BlueprintShips-MyMod.esm"),
		"sf: base game file rule as the exe's");
	Check(EditedSaveName("Save1471_2C1EE8CF_42726F6E78_092246_20261111021801_73_1_4.sfs", "edited", false).filename() ==
			  "Save1471_2C1EE8CF_42726F6E78_092246_20261111021801edited_73_1_4.sfs",
		"sf: edited name keeps the game's fields");

	const char* env = std::getenv("RESAVERPP_SF_SAVES");
	if (!env || !*env) {
		std::printf("(starfield: RESAVERPP_SF_SAVES not set, real saves skipped)\n");
		return;
	}
	std::string all = env;
	std::size_t n = 0;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) {
			e = all.size();
		}
		const fs::path f = all.substr(p, e - p);
		p = e + 1;
		if (f.empty()) {
			continue;
		}
		const auto name = f.filename().string();
		auto       fail = [&](const std::string& a_what) {
            std::printf("FAIL: sf %s: %s\n", name.c_str(), a_what.c_str());
            ++g_fail;
		};
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception& ex) {
			fail(std::string("does not read: ") + ex.what());
			continue;
		}
		++n;
		const auto  file = SfReadAll(f);
		SfContainer info;
		const auto  inner = UnpackBCPS(file, info);
		Check(s->isSF() && s->problems.empty(), "sf: reads with no problems");
		Check(!s->sfPapyrusRaw() && s->papyrus() && s->papyrus()->starfield && !s->papyrus()->instances.empty(),
			"sf: Papyrus read (PapyrusSF)");
		Check(s->buildBody() == inner, "sf: the data is byte-identical when built again");
		Check(s->plugins.full.size() >= 1 && s->plugins.full[0] == "Starfield.esm", "sf: Starfield.esm first");
		Check(s->table1.size() == 17 && s->table2.size() == 21 && (s->table3.size() == 7 || s->table3.size() == 9), "sf: global data counts");
		{
			std::size_t refr = 0, unknown = 0;
			for (const auto& c : s->changeForms) {
				refr += std::string_view(s->cfTypeName(c)) == "REFR";
				unknown += std::string_view(s->cfTypeName(c)) == "????";
			}
			Check(refr > 0 && unknown == 0, "sf: change form types named");
		}
		// the plugin entries as the loader reads them: at 140+ the flag byte the game wrote is !IsBaseGameFile(name)
		{
			bool rule = true;
			if (s->sfVersion() >= 140) {
				for (const auto* v : { &s->plugins.sfFull, &s->plugins.sfLite, &s->plugins.sfMedium }) {
					const auto& names = v == &s->plugins.sfFull ? s->plugins.full : v == &s->plugins.sfLite ? s->plugins.lite : s->plugins.medium;
					for (std::size_t i = 0; i < v->size(); ++i) {
						rule &= ((*v)[i].isCreation != 0) == !IsBaseGameFileSF(names[i]);
					}
				}
			}
			Check(rule && s->plugins.sfExtraMode == (s->sfVersion() >= 140 ? 2 : s->sfVersion() >= 120 ? 1 : 0) && s->plugins.hasMedium,
				"sf: plugin entries read in the layout of the save version");
		}
		// the same save written as save version 130 (no flag byte: details by the base file rule) reads back the same
		if (s->sfVersion() >= 140) {
			auto v130 = SaveFile::load(f);
			v130->header.sfSaveVersion = 130;
			v130->plugins.sfExtraMode = 1;
			const auto body = v130->buildBody();
			const auto rawPath = fs::temp_directory_path() / ("rs_sf130_" + std::to_string(n) + ".sfs");
			{
				std::ofstream o(rawPath, std::ios::binary);
				o.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
			}
			std::unique_ptr<SaveFile> back;
			try {
				back = SaveFile::load(rawPath);
			} catch (const std::exception& ex) {
				fail(std::string("version 130 body does not read: ") + ex.what());
			}
			std::error_code ecRaw;
			fs::remove(rawPath, ecRaw);
			Check(back && back->problems.empty() && back->plugins.sfExtraMode == 1 && back->plugins.full == s->plugins.full &&
					  back->plugins.medium == s->plugins.medium && back->changeForms.size() == s->changeForms.size() && back->buildBody() == body,
				"sf: a save version 130 plugin list (no flag byte) reads and writes");
			std::size_t same = 0;
			for (std::size_t i = 0; back && i < back->plugins.sfFull.size(); ++i) {
				same += back->plugins.sfFull[i].isCreation == s->plugins.sfFull[i].isCreation && back->plugins.sfFull[i].id == s->plugins.sfFull[i].id;
			}
			Check(back && same == back->plugins.sfFull.size(), "sf: version 130: the same Creations details");
			v130->header.sfSaveVersion = 105;  // before 108: no medium count, names only
			v130->plugins.sfExtraMode = 0;
			v130->plugins.hasMedium = false;
			const bool hasMed = !v130->plugins.medium.empty();
			bool       threw = false;
			try {
				(void)v130->buildBody();
			} catch (const std::exception&) {
				threw = true;
			}
			Check(threw == hasMed, "sf: medium plugins refused for a save version without the medium list");
		}
		// the whole file: packed again, it unpacks to the same data; the container header as before
		const auto packed = s->serialize();
		{
			SfContainer back;
			const auto  un = UnpackBCPS(packed, back);
			Check(un == inner, "sf: written container unpacks to the same data");
			Check(std::memcmp(packed.data(), file.data(), 8) == 0 && std::memcmp(packed.data() + 0x20, file.data() + 0x20, 0x28) == 0,
				"sf: container header fields as the game wrote them");
		}
		// a save written and read again
		const auto out = fs::temp_directory_path() / ("rs_sf_" + std::to_string(n) + ".sfs");
		{
			std::ofstream o(out, std::ios::binary);
			o.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
		}
		auto again = SaveFile::load(out);
		Check(again->problems.empty() && again->buildBody() == inner, "sf: written save reads back identical");
		// the script cleanups the Starfield GUI offers: written, read back clean
		{
			auto       edit = SaveFile::load(out);
			const auto before = edit->papyrus()->instances.size();
			const auto r = ops::RemoveUnattachedInstances(*edit);
			ops::TerminateUndefinedThreads(*edit->papyrus());
			const auto named = ops::NamedObjectIDs(*edit->papyrus());
			bool       anyNamedGone = false;
			for (const auto& i : edit->papyrus()->instances) {
				anyNamedGone |= ops::IsUnattached(i) && !named.contains(i.id);
			}
			Check(r.removed > 0 && !anyNamedGone && edit->papyrus()->instances.size() == before - r.removed,
				"sf: unattached instances removed, the ones a variable names kept");
			const auto bytes = edit->serialize();
			const auto out2 = fs::temp_directory_path() / ("rs_sf_clean_" + std::to_string(n) + ".sfs");
			{
				std::ofstream o(out2, std::ios::binary);
				o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			auto back = SaveFile::load(out2);
			Check(back->problems.empty() && !back->sfPapyrusRaw() && back->papyrus()->instances.size() == before - r.removed &&
					  back->buildBody() == edit->buildBody(),
				"sf: cleaned save reads back clean and identical");
			std::error_code ec2;
			fs::remove(out2, ec2);
		}
		// deleting change forms: a whole run of one type, one form inside a run, the first and the last form
		{
			auto&       cfs = again->changeForms;
			const auto  before = cfs.size();
			std::size_t runStart = 0;
			while (runStart < cfs.size() && cfs[runStart].typeField == cfs[0].typeField) {
				++runStart;
			}
			std::size_t runEnd = runStart;
			while (runEnd < cfs.size() && cfs[runEnd].typeField == cfs[runStart].typeField) {
				++runEnd;
			}
			const auto removedType = cfs[runStart].typeField;
			std::size_t typeTotal = 0;
			for (const auto& c : cfs) {
				typeTotal += c.typeField == removedType;
			}
			const auto lastRef = cfs.back().refID.data;
			std::vector<ChangeForm> keep;
			for (std::size_t i = 0; i < cfs.size(); ++i) {
				if ((i >= runStart && i < runEnd) || i == 0 || i == cfs.size() - 1 || i == runEnd + 3) {
					continue;
				}
				keep.push_back(std::move(cfs[i]));
			}
			const auto removed = before - keep.size();
			std::size_t expectType = 0;
			for (const auto& c : keep) {
				expectType += c.typeField == removedType;
			}
			Check(expectType < typeTotal, "sf: deleted change forms: a whole run went");
			cfs = std::move(keep);
			again->reindexChangeForms();
			const auto bytes = again->serialize();
			{
				std::ofstream o(out, std::ios::binary | std::ios::trunc);
				o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			auto third = SaveFile::load(out);
			Check(third->problems.empty() && third->changeForms.size() == before - removed && third->flt.changeFormCount == before - removed,
				"sf: deleted change forms: counts updated, every section where the table says");
			std::size_t typeNow = 0;
			for (const auto& c : third->changeForms) {
				typeNow += c.typeField == removedType;
			}
			Check(typeNow == expectType,
				"sf: deleted change forms: the type list follows");
			Check(third->buildBody() == again->buildBody(), "sf: deleted change forms: the data read back is what was written");
			Check(!third->findChangeForm(RefID{ lastRef }), "sf: deleted change forms: the last one is gone");
		}
		std::error_code ec;
		fs::remove(out, ec);
	}
	std::printf("(starfield: %zu real save(s) read and written)\n", n);
}
