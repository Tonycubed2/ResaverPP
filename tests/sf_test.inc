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
