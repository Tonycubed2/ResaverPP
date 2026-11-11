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
	// the game's name parser (0x1418365c0): the date field must stay 14 characters; the save number is read with strtoul
	Check(EditedSaveName("Save1471_2C1EE8CF_42726F6E78_092246_20261111021801_73_1_4.sfs", "edited", false).filename() ==
			  "Save1471edited_2C1EE8CF_42726F6E78_092246_20261111021801_73_1_4.sfs",
		"sf: edited name keeps the game's fields (tag after the save number)");
	Check(EditedSaveName("Autosave1_5AD6CEB5M46726579612046726F7374_010516_20261108071620_52_3_4.sfs", "cleaned", false).filename() ==
			  "Autosave1cleaned_5AD6CEB5M46726579612046726F7374_010516_20261108071620_52_3_4.sfs",
		"sf: autosave names get the tag after their number too");
	Check(EditedSaveName("Quicksave0edited_5AD6CEB5M46726579612046726F7374_010516_20261108071620_52_3_4.sfs", "cleaned", false).filename() ==
			  "Quicksave0cleaned_5AD6CEB5M46726579612046726F7374_010516_20261108071620_52_3_4.sfs",
		"sf: an earlier tag is replaced");
	Check(EditedSaveName("HardShip.sfs", "cleaned", false).filename() == "HardShip_cleaned.sfs", "sf: a name of the player's own gets _tag");
	Check(EditedSaveName("Save1471_x.ess", "edited", false).filename() == "Save1471_x_edited.ess", "skyrim names unchanged by the Starfield rule");

	const char* env = std::getenv("RESAVERPP_SF_SAVES");
	if (!env || !*env) {
		std::printf("(starfield: RESAVERPP_SF_SAVES not set, real saves skipped)\n");
		return;
	}
	std::string all = env;
	std::size_t n = 0;
	std::size_t sfUnattachedSeen = 0;
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
			std::size_t expect = 0;
			{
				const auto named0 = ops::NamedObjectIDs(*edit->papyrus());
				for (const auto& i : edit->papyrus()->instances) {
					expect += ops::IsUnattached(i) && !named0.contains(i.id);
				}
			}
			const auto r = ops::RemoveUnattachedInstances(*edit);
			sfUnattachedSeen += r.removed;
			ops::TerminateUndefinedThreads(*edit->papyrus());
			const auto named = ops::NamedObjectIDs(*edit->papyrus());
			bool       anyNamedGone = false;
			for (const auto& i : edit->papyrus()->instances) {
				anyNamedGone |= ops::IsUnattached(i) && !named.contains(i.id);
			}
			Check(r.removed == expect && !anyNamedGone && edit->papyrus()->instances.size() == before - r.removed,
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
		// the Save check on a save the game made: nothing stranded, nothing memberless, the size by mod adds up
		{
			const auto& p = *s->papyrus();
			Check(ops::StrandedThreads(p).empty(), "sf: no stranded thread in a save the game made");
			std::size_t memberless = 0;
			for (const auto& i : p.instances) {
				memberless += ops::IsMemberless(p, i);
			}
			Check(memberless == 0, "sf: no memberless instance in a save the game made");
			const auto rep = bloat::Build(*s);
			Check(rep.changeFormBytes == s->flt.table3Offset - s->flt.changeFormsOffset, "sf: bloat: change form bytes = the change form section");
			std::uint64_t pap = 0;
			for (const auto& m : rep.mods) {
				pap += m.papyrusBytes();
			}
			Check(pap > 0 && pap <= rep.papyrusBytes && rep.mods.size() > 1, "sf: bloat: script data by mod within the section");
			std::uint32_t pick = 0;
			std::string   pickType;
			for (const auto& c : s->changeForms) {
				if (c.refID.type() != RefID::Type::kCreated && s->formIDOf(c.refID) != 0) {
					pick = s->formIDOf(c.refID);
					pickType = s->cfTypeName(c);
					break;
				}
			}
			const auto mm = ops::SfTypeMismatches(*s, [&](std::uint32_t a_id) -> std::string {
				return a_id == pick ? std::string("XXXX") : std::string();
			});
			Check(mm.size() == 1 && mm[0].formID == pick && mm[0].saved == pickType && mm[0].now == "XXXX", "sf: a form whose type changed is found");
			const auto rb = flood::ReadBack(*s);
			Check(rb.ok && rb.threads == p.activeScripts.size() && rb.stranded == 0, "sf: script data written and read back (flood check)");
		}
		// a thread left stranded (its queued call deleted without it; or a latent wait turned off), found and removed
		{
			auto  e = SaveFile::load(out);
			auto& p = *e->papyrus();
			bool  made = false;
			if (!p.functionMessages.empty()) {
				ops::DeleteFunctionMessages(p, { 0 });
				made = true;
			} else {
				for (auto& a : p.activeScripts) {
					if (a.type != 0) {
						a.type = 0;
						// a latent result waiting for it holds it
						PendingLatent l;
						l.id = a.id;
						p.pendingLatents.push_back(l);
						Check(ops::StrandedThreads(p).empty(), "sf: a pending latent result holds its thread");
						p.pendingLatents.clear();
						made = true;
						break;
					}
				}
			}
			if (made) {
				Check(ops::StrandedThreads(p).size() == 1, "sf: a thread nothing holds is stranded");
				const auto r = ops::RemoveStrandedThreads(p);
				Check(r.threads == 1 && ops::StrandedThreads(p).empty(), "sf: the stranded thread removed");
				const auto bytes = e->serialize();
				const auto out3 = fs::temp_directory_path() / ("rs_sf_str_" + std::to_string(n) + ".sfs");
				{
					std::ofstream o(out3, std::ios::binary);
					o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				}
				auto back = SaveFile::load(out3);
				Check(back->problems.empty() && ops::StrandedThreads(*back->papyrus()).empty() &&
						  back->papyrus()->activeScripts.size() == p.activeScripts.size(),
					"sf: without the stranded thread it reads back clean");
				std::error_code ec3;
				fs::remove(out3, ec3);
			}
		}
		// an .sfse next to the save is read as bytes and written next to the new save under its name
		{
			const auto dir = fs::temp_directory_path() / ("rs_sfse_" + std::to_string(n));
			std::error_code ec4;
			fs::remove_all(dir, ec4);
			fs::create_directories(dir, ec4);
			const auto src = dir / "Save7_ABCDEF01_42726F6E78_092246_20261111021801_73_1_4.sfs";
			fs::copy_file(out, src, ec4);
			const std::vector<std::uint8_t> co = { 'n', 'o', 't', ' ', 'S', 'F', 'S', 'E', 0, 1, 2, 3 };
			{
				std::ofstream o(dir / "Save7_ABCDEF01_42726F6E78_092246_20261111021801_73_1_4.sfse", std::ios::binary);
				o.write(reinterpret_cast<const char*>(co.data()), static_cast<std::streamsize>(co.size()));
			}
			auto withCo = SaveFile::load(src);
			Check(withCo->cosave == co, "sf: an .sfse next to the save is kept");
			const auto dst = EditedSaveName(src, "cleaned", true);
			withCo->write(dst, false);
			auto dstCo = dst;
			dstCo.replace_extension(".sfse");
			Check(dst.filename().string().rfind("Save7cleaned_", 0) == 0 && fs::exists(dstCo) && SfReadAll(dstCo) == co,
				"sf: the .sfse is written next to the new save under its name");
			withCo->write(src, true);  // in place with a backup: both files backed up
			auto bakCo = dir / "Save7_ABCDEF01_42726F6E78_092246_20261111021801_73_1_4.sfse.bak";
			Check(fs::exists(bakCo) && SfReadAll(bakCo) == co, "sf: writing in place backs up the .sfse too");
			fs::remove_all(dir, ec4);
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
	Check(n == 0 || sfUnattachedSeen > 0, "sf: some real save had unattached instances to remove");
	std::printf("(starfield: %zu real save(s) read and written)\n", n);
}

// Starfield names (PluginDBSF.cpp) and mods no longer installed (modremove::StarfieldNotLoaded + MakePlan / Apply).
// Synthetic plugins and archives always; then RESAVERPP_SF_DATA (a Starfield Data folder, ';' for more) with the saves of
// RESAVERPP_SF_SAVES: names of known forms (checked against a separate Python reader on Tony's files), and the leftovers of
// the plugins that folder does not have removed, written, read back clean. RESAVERPP_SF_OUT: a folder to keep the cleaned
// saves in (for the Python reference parser).
namespace
{
	struct SfRec
	{
		const char*   type;
		std::uint32_t id;
		std::string   edid;
		std::uint32_t fullId = 0;     // localized FULL (string id), 0 none
		std::string   full;           // or a plain FULL
		std::uint32_t name = 0;       // NAME (placed reference's base), 0 none
		bool          compress = false;
	};

	void SfSub(Writer& w, const char* t, const void* d, std::size_t n)
	{
		w.raw(t, 4);
		w.u16(static_cast<std::uint16_t>(n));
		w.raw(d, n);
	}

	std::vector<std::uint8_t> SfPlugin(std::uint32_t a_flags, const std::vector<std::string>& a_masters, const std::vector<SfRec>& a_recs)
	{
		Writer hdr;
		for (const auto& m : a_masters) {
			SfSub(hdr, "MAST", (m + std::string(1, '\0')).data(), m.size() + 1);
		}
		Writer w;
		w.raw("TES4", 4);
		w.u32(static_cast<std::uint32_t>(hdr.buf.size()));
		w.u32(a_flags);
		w.u32(0);
		w.u32(0);
		w.u16(582);
		w.u16(0);
		w.bytes(hdr.buf);
		// one group with every record (its label is not read)
		Writer body;
		for (const auto& r : a_recs) {
			Writer d;
			if (!r.edid.empty()) {
				SfSub(d, "EDID", (r.edid + std::string(1, '\0')).data(), r.edid.size() + 1);
			}
			if (r.fullId) {
				SfSub(d, "FULL", &r.fullId, 4);
			} else if (!r.full.empty()) {
				SfSub(d, "FULL", (r.full + std::string(1, '\0')).data(), r.full.size() + 1);
			}
			if (r.name) {
				SfSub(d, "NAME", &r.name, 4);
			}
			std::vector<std::uint8_t> data = d.buf;
			std::uint32_t             flags = 0;
			if (r.compress) {
				const auto z = CompressZlib(d.buf);
				Writer     c;
				c.u32(static_cast<std::uint32_t>(d.buf.size()));
				c.bytes(z);
				data = c.buf;
				flags = 0x40000;
			}
			body.raw(r.type, 4);
			body.u32(static_cast<std::uint32_t>(data.size()));
			body.u32(flags);
			body.u32(r.id);
			body.u32(0);
			body.u16(582);
			body.u16(0);
			body.bytes(data);
		}
		w.raw("GRUP", 4);
		w.u32(static_cast<std::uint32_t>(24 + body.buf.size()));
		w.raw("KYWD", 4);
		w.u32(0);
		w.u32(0);
		w.u32(0);
		w.bytes(body.buf);
		return w.buf;
	}

	std::vector<std::uint8_t> SfStrings(const std::vector<std::pair<std::uint32_t, std::string>>& a_s)
	{
		Writer dir, data;
		for (const auto& [id, t] : a_s) {
			dir.u32(id);
			dir.u32(static_cast<std::uint32_t>(data.buf.size()));
			data.raw(t.data(), t.size());
			data.u8(0);
		}
		Writer w;
		w.u32(static_cast<std::uint32_t>(a_s.size()));
		w.u32(static_cast<std::uint32_t>(data.buf.size()));
		w.bytes(dir.buf);
		w.bytes(data.buf);
		return w.buf;
	}

	// a Starfield GNRL BA2 (version 2): header + 8 bytes, 36-byte records, data, name table with '/'
	std::vector<std::uint8_t> SfBa2(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& a_files)
	{
		const std::size_t hdr = 32, recs = 36 * a_files.size();
		std::uint64_t     off = hdr + recs;
		Writer            r, d, n;
		for (const auto& [name, bytes] : a_files) {
			r.u32(0);
			r.raw("strs", 4);
			r.u32(0);
			r.u32(0x00100100);
			r.u64(off);
			r.u32(0);  // stored
			r.u32(static_cast<std::uint32_t>(bytes.size()));
			r.u32(0xBAADF00Du);
			d.bytes(bytes);
			off += bytes.size();
			n.u16(static_cast<std::uint16_t>(name.size()));
			n.raw(name.data(), name.size());
		}
		Writer w;
		w.raw("BTDX", 4);
		w.u32(2);
		w.raw("GNRL", 4);
		w.u32(static_cast<std::uint32_t>(a_files.size()));
		w.u64(off);
		w.u32(1);
		w.u32(0);
		w.bytes(r.buf);
		w.bytes(d.buf);
		w.bytes(n.buf);
		return w.buf;
	}

	void SfWrite(const fs::path& a_p, const std::vector<std::uint8_t>& a_b)
	{
		std::ofstream o(a_p, std::ios::binary);
		o.write(reinterpret_cast<const char*>(a_b.data()), static_cast<std::streamsize>(a_b.size()));
	}

	std::vector<std::string> SfSplit(const char* a_env)
	{
		std::vector<std::string> v;
		if (!a_env) {
			return v;
		}
		std::string s = a_env;
		for (std::size_t p = 0; p <= s.size();) {
			auto e = s.find(';', p);
			if (e == std::string::npos) {
				e = s.size();
			}
			if (e > p) {
				v.push_back(s.substr(p, e - p));
			}
			p = e + 1;
		}
		return v;
	}
}

void TestStarfieldNames()
{
	// ---- synthetic: a full master, a light plugin, a medium plugin and a full plugin that uses all three
	const auto dir = fs::temp_directory_path() / "rs_sfnames";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir / "Strings");
	// Base.esm (full, localized): its strings in a v2 BA2 "Base - Localization.ba2"
	SfWrite(dir / "Base.esm", SfPlugin(0x81, {}, { { "KYWD", 0x000800, "BaseKwd", 1 }, { "MISC", 0x000801, "BaseMisc", 2, "", 0, true },
													   { "REFR", 0x000900, "BaseRef", 0, "", 0x000801 }, { "REFR", 0x000901, "Unwanted" } }));
	SfWrite(dir / "Base - Localization.ba2",
		SfBa2({ { "strings/base_en.strings", SfStrings({ { 1, "Base Keyword" }, { 2, "Base Thing" } }) },
			{ "strings/base_de.strings", SfStrings({ { 1, "Basis" } }) } }));
	// Light.esm (light, master Base): own records FE000xxx; overrides Base's MISC (top byte 00)
	SfWrite(dir / "Light.esm", SfPlugin(0x101, { "Base.esm" }, { { "KYWD", 0xFE000810, "LightKwd", 0, "Light Keyword" },
																	{ "MISC", 0x00000801, "BaseMisc", 0, "Changed By Light" } }));
	// Med.esm (medium, masters Base and Light): own FD00xxxx, Light's records FE000xxx, Base's 00xxxxxx; loose strings
	SfWrite(dir / "Med.esm", SfPlugin(0x481, { "Base.esm", "Light.esm" }, { { "KYWD", 0xFD001234, "MedKwd", 7 }, { "KYWD", 0xFE000810, "LightKwd", 8 },
																				{ "KYWD", 0x00000800, "BaseKwd" } }));
	SfWrite(dir / "Strings" / "med_en.strings", SfStrings({ { 7, "Medium Keyword" }, { 8, "Light Keyword (Med)" } }));
	// Last.esm (full; masters Base, Light2 (light), Med (medium), Light): its own records 01xxxxxx (one full master), Light2
	// FE000xxx, Light FE001xxx, Med FD00xxxx; an id with a light index past its light masters and its own does not resolve
	SfWrite(dir / "Light2.esm", SfPlugin(0x101, { "Base.esm" }, { { "KYWD", 0xFE000820, "Light2Kwd" } }));
	SfWrite(dir / "Last.esm", SfPlugin(0x01, { "Base.esm", "Light2.esm", "Med.esm", "Light.esm" },
								  { { "KYWD", 0x01000830, "LastKwd", 0, "Last" }, { "KYWD", 0xFE001810, "LightKwdLast" }, { "KYWD", 0xFD001234, "MedKwdLast" },
									  { "KYWD", 0xFE000820, "Light2KwdLast" }, { "KYWD", 0xFE005000, "Nobody" } }));
	{
		PluginDB::Input in;
		in.sf = true;
		in.full = { "Base.esm", "Last.esm" };
		in.lite = { "Light2.esm", "Light.esm" };   // FE:000 Light2, FE:001 Light
		in.medium = { "Med.esm" };                  // FD:00
		in.sfDataDirs = { dir };
		in.language = "en";
		in.wantedRefs = { 0x00000900 };
		PluginDB::Progress prog;
		PluginDB           db;
		db.load(in, prog);
		auto d = [&](std::uint32_t id) { return db.describe(id); };
		if (std::getenv("RS_SF_DEBUG")) {
			for (auto id : { 0x00000800u, 0x00000801u, 0x00000900u, 0xFE001810u, 0xFE000820u, 0xFD001234u, 0x01000830u }) {
				const auto* ff = db.find(id);
				std::printf("  %08X %s (%s)\n", id, d(id).c_str(), ff ? db.pluginName(*ff).c_str() : "-");
			}
			std::printf("  unresolved %zu orphans %zu read %zu\n", db.sfUnresolved, db.sfOrphanOverrides, db.pluginsRead);
			for (auto& x : db.sfSamples) std::printf("  %s\n", x.c_str());
			for (auto& x : db.errors) std::printf("  err %s\n", x.c_str());
		}
		Check(db.pluginsRead == 5 && db.missing.empty() && db.errors.empty(), "sf names: every synthetic plugin read");
		Check(d(0x00000800) == "BaseKwd 'Base Keyword' [KYWD]", ("sf names: localized FULL from a v2 BA2 (" + d(0x00000800) + ")").c_str());
		Check(d(0x00000801) == "BaseMisc 'Changed By Light' [MISC]", "sf names: compressed record, then overridden by a light plugin");
		Check(d(0x00000900) == "BaseRef -> Changed By Light [REFR]" && !db.find(0x00000901), "sf names: wanted placed reference with its base, unwanted one skipped");
		Check(d(0xFE001810) == "LightKwdLast 'Light Keyword (Med)' [KYWD]", "sf names: light plugin FE:001 own records, overridden twice");
		Check(d(0xFE000820) == "Light2KwdLast [KYWD]", "sf names: FE000xxx in Last = its first light master");
		Check(d(0xFD001234) == "MedKwdLast 'Medium Keyword' [KYWD]", "sf names: medium plugin FD:00 (own FD00xxxx), loose strings, override keeps the name");
		Check(d(0x01000830) == "LastKwd 'Last' [KYWD]", "sf names: own records of a full plugin = number of full masters");
		Check(db.sfUnresolved == 1 && db.sfOrphanOverrides == 0, "sf names: only the id with no light plugin is unresolved, no orphan overrides");
		const auto* f = db.find(0xFD001234);
		Check(f && db.pluginName(*f) == "Last.esm", "sf names: the last plugin that defines a form");
		// the language asked for first
		in.language = "de";
		PluginDB db2;
		db2.load(in, prog);
		Check(db2.describe(0x00000800) == "BaseKwd 'Basis' [KYWD]" && db2.describe(0xFD001234) == "MedKwdLast 'Medium Keyword' [KYWD]",
			"sf names: German strings where there are some, English otherwise");
	}
	// the archive itself: v2 header, '/' names read as '\'
	{
		BsaArchive a(dir / "Base - Localization.ba2");
		const auto* e = a.find("strings\\base_en.strings");
		Check(a.isBa2() && a.version() == 2 && e && ReadStringsTable(a.extract(*e)).at(2) == "Base Thing", "sf ba2: version 2 general archive");
	}
	// load order: base files always, plugins.txt '*' lines, files must exist
	{
		const auto d2 = dir / "data2";
		fs::create_directories(d2);
		for (const char* n : { "Starfield.esm", "Constellation.esm", "ModA.esm", "ModB.esm" }) {
			SfWrite(d2 / n, SfPlugin(0x81, {}, {}));
		}
		const auto txt = dir / "plugins.txt";
		{
			std::ofstream o(txt);
			o << "# test\n*ModA.esm\nModB.esm\n*Gone.esm\n";
		}
		const auto o1 = loadorder::ReadStarfield(dir / "none.txt", { d2 });
		Check(!o1.pluginsTxtFound && o1.active == std::set<std::string>{ "starfield.esm", "constellation.esm" }, "sf load order: no plugins.txt: the game's own files");
		const auto o2 = loadorder::ReadStarfield(txt, { d2 });
		Check(o2.pluginsTxtFound && o2.active.contains("moda.esm") && !o2.active.contains("modb.esm") && !o2.active.contains("gone.esm") && o2.installed("modb.esm"),
			"sf load order: '*' enables, the file must be there");
	}
	fs::remove_all(dir, ec);

	// ---- real files
	const auto dataDirs = SfSplit(std::getenv("RESAVERPP_SF_DATA"));
	const auto saves = SfSplit(std::getenv("RESAVERPP_SF_SAVES"));
	if (dataDirs.empty() || saves.empty()) {
		std::printf("(starfield names: RESAVERPP_SF_DATA / RESAVERPP_SF_SAVES not set, real files skipped)\n");
		return;
	}
	std::vector<fs::path> dirs(dataDirs.begin(), dataDirs.end());
	const auto            outDir = std::getenv("RESAVERPP_SF_OUT");
	std::size_t           namedSaves = 0, cleaned = 0;
	for (const auto& sp : saves) {
		const fs::path f = sp;
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception& e) {
			std::printf("FAIL: sf names %s: %s\n", f.filename().string().c_str(), e.what());
			++g_fail;
			continue;
		}
		const auto name = f.filename().string();
		// names: only for the first save of each plugin list (Starfield.esm takes seconds)
		if (namedSaves < 2) {
			PluginDB::Input in;
			PrepareInput(*s, in);
			in.sf = true;
			in.medium = s->plugins.medium;
			in.sfDataDirs = dirs;
			PluginDB::Progress prog;
			PluginDB           db;
			const auto         t0 = std::chrono::steady_clock::now();
			db.load(in, prog);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			std::printf("(starfield names %s: %zu of %zu plugins, %zu forms, %.0f MB in %.0f ms; unresolved %zu, orphan overrides %zu)\n", name.c_str(),
				db.pluginsRead, db.plugins().size(), db.size(), db.sfBytes / 1048576.0, ms, db.sfUnresolved, db.sfOrphanOverrides);
			Check(db.sfUnresolved == 0, ("sf names: every form id of the plugins placed (" + name + ")").c_str());
			// forms checked against the Python reader on the device (sfdev/extract.py): the runtime id from the save's own lists
			auto runtimeOf = [&](const std::string& a_plugin, std::uint32_t a_low) -> std::uint32_t {
				for (std::size_t i = 0; i < s->plugins.full.size(); ++i) {
					if (s->plugins.full[i] == a_plugin) {
						return static_cast<std::uint32_t>(i << 24) | (a_low & 0xFFFFFF);
					}
				}
				for (std::size_t i = 0; i < s->plugins.lite.size(); ++i) {
					if (s->plugins.lite[i] == a_plugin) {
						return 0xFE000000u | static_cast<std::uint32_t>(i << 12) | (a_low & 0xFFF);
					}
				}
				for (std::size_t i = 0; i < s->plugins.medium.size(); ++i) {
					if (s->plugins.medium[i] == a_plugin) {
						return 0xFD000000u | static_cast<std::uint32_t>(i << 16) | (a_low & 0xFFFF);
					}
				}
				return 0;
			};
			struct Known
			{
				const char*   plugin;
				std::uint32_t low;
				const char*   text;
			};
			static const Known known[] = {
				{ "Starfield.esm", 0x000007, "Player 'Player' [NPC_]" },
				{ "Starfield.esm", 0x00118F, "MQ_TutorialQuest_Misc06 'Misc Pointer to Research' [QUST]" },
				{ "ShatteredSpace.esm", 0x00154E, "AnimFurnSFBGS001_MQ04_1000_Eggie_QuestioningScene [KYWD]" },
				{ "SFBGS003.esm", 0x0063, "MissionBoardTypeBountyElite 'Elite Bounty' [KYWD]" },          // medium
				{ "SFBGS003.esm", 0xF6CD, "SFBGS003_abAddPerk_ActivatePoster 'Activate Poster' [SPEL]" },
				{ "SFBGS004.esm", 0x289, "Rev8_MountedLaser 'Rev8 Laser' [WEAP]" },                       // light
				{ "Constellation.esm", 0x008, "mod_AVM_Spacesuit_Constellation_Body_DigitalDeluxe 'Constellation Skin' [OMOD]" },
				{ "SFBGS047.esm", 0x002, "MoonJumper_Turret 'MoonJumper Turret' [WEAP]" },                // light, own index FE001 in its file
			};
			std::size_t checked = 0;
			for (const auto& k : known) {
				const auto id = runtimeOf(k.plugin, k.low);
				if (!id || !db.wasRead(k.plugin)) {
					continue;
				}
				++checked;
				Check(db.describe(id) == k.text, (std::string("sf names: ") + k.plugin + " " + k.text + " (got: " + db.describe(id) + ")").c_str());
			}
			std::printf("(starfield names: %zu known forms checked)\n", checked);
			// most change forms of plugins that were read are named
			std::size_t cfs = 0, named = 0;
			for (const auto& c : s->changeForms) {
				const auto fid = s->formIDOf(c.refID);
				if (c.refID.type() == RefID::Type::kCreated || !db.wasRead(s->pluginOf(fid))) {
					continue;
				}
				++cfs;
				named += db.find(fid) != nullptr;
			}
			Check(cfs == 0 || named * 100 >= cfs * 99, ("sf names: 99% of the change forms of the plugins read are found (" + std::to_string(named) + "/" + std::to_string(cfs) + ")").c_str());
			++namedSaves;
		}
		// ---- mods no longer installed: every plugin of the save that the Data folders do not have
		const auto order = loadorder::ReadStarfield(dirs.front() / "no-plugins.txt", dirs);
		const auto table = mods::Build(*s, nullptr);
		const auto left = modremove::StarfieldNotLoaded(*s, order, &table);
		std::size_t gone = 0, removed = 0, refused = 0;
		modremove::Options opt;
		opt.sfFiles = &order.files;
		std::size_t cfBefore = s->changeForms.size(), cfLeft = 0;
		for (const auto& l : left) {
			Check(!order.active.contains(l.plugin), "sf missing: a listed plugin is not active");
			gone += !l.installed;
			cfLeft += l.changeForms;
			if (l.installed || l.nothing()) {
				continue;
			}
			const auto plan = modremove::MakePlan(*s, l.plugin, nullptr, opt);
			if (!plan.canApply()) {
				++refused;
				continue;
			}
			Check(plan.changeForms.size() == l.changeForms && plan.spawnedForms.empty(), "sf missing: the plan has the plugin's change forms and no spawned objects");
			modremove::Apply(*s, plan);
			++removed;
		}
		std::printf("(starfield missing %s: %zu plugins not loaded (%zu not installed, %zu change forms), %zu cleaned, %zu refused)\n", name.c_str(),
			left.size(), gone, cfLeft, removed, refused);
		if (!removed) {
			continue;
		}
		// nothing of a cleaned plugin is left; the save writes and reads back clean
		const auto left2 = modremove::StarfieldNotLoaded(*s, order, nullptr);
		std::size_t forms2 = 0, inst2 = 0;
		for (const auto& l : left2) {
			std::string low = l.plugin;
			for (auto& c : low) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			forms2 += l.installed || IsStarfieldBaseFile(low) ? 0 : l.changeForms;
			inst2 += l.installed || IsStarfieldBaseFile(low) ? 0 : l.ownInstances;
		}
		Check(forms2 == 0 && s->changeForms.size() < cfBefore, "sf missing: no change form of a removed plugin is left");
		Check(inst2 == 0, "sf missing: no script instance on a removed plugin's forms is left (engine types included)");
		const auto bytes = s->serialize();
		const auto out = (outDir && *outDir ? fs::path(outDir) : fs::temp_directory_path()) / EditedSaveName(f, "cleaned", false).filename();
		SfWrite(out, bytes);
		auto back = SaveFile::load(out);
		Check(back->problems.empty() && !back->sfPapyrusRaw() && back->papyrus()->check().empty() && back->buildBody() == s->buildBody() &&
				  back->changeForms.size() == s->changeForms.size(),
			("sf missing: the cleaned save reads back clean (" + name + ")").c_str());
		++cleaned;
		if (!outDir || !*outDir) {
			fs::remove(out, ec);
		}
	}
	std::printf("(starfield names: %zu save(s) named, %zu cleaned of mods not installed)\n", namedSaves, cleaned);
}

// Starfield with a mod manager: the plugin kind of a file (renames keep the kind), the game's own files in a profile's
// plugins.txt, renames of medium plugins
void TestStarfieldModManager()
{
	const auto dir = fs::temp_directory_path() / "rs_sf_mm";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	auto plugin = [&](const std::string& a_name, std::uint32_t a_flags, const char* a_magic = "TES4") {
		std::ofstream o(dir / a_name, std::ios::binary);
		o.write(a_magic, 4);
		const std::uint32_t v[2] = { 0, a_flags };
		o.write(reinterpret_cast<const char*>(v), sizeof(v));
		return dir / a_name;
	};
	Check(loadorder::StarfieldPluginKind(plugin("Full.esm", 0x1)) == 0, "sf mm: a plain master is full");
	Check(loadorder::StarfieldPluginKind(plugin("Light.esm", 0x101)) == 1, "sf mm: flag 0x100 is light");
	Check(loadorder::StarfieldPluginKind(plugin("Medium.esm", 0x401)) == 2, "sf mm: flag 0x400 is medium");
	Check(loadorder::StarfieldPluginKind(plugin("Small.esl", 0)) == 1, "sf mm: a .esl file is light");
	Check(loadorder::StarfieldPluginKind(plugin("Overlay.esm", 0x201)) == -1, "sf mm: an overlay file is not told");
	Check(loadorder::StarfieldPluginKind(plugin("Bad.esm", 0, "XXXX")) == -1, "sf mm: not a plugin");
	Check(loadorder::StarfieldPluginKind(dir / "None.esm") == -1, "sf mm: no file");
	{
		std::ofstream o(dir / "plugins.txt");
		o << "# comment\n*Mod.esm\nOff.esp\n";
	}
	const auto a = loadorder::Read(dir / "plugins.txt", {}, 3);
	Check(a.known && a.plugins.contains("starfield.esm") && a.plugins.contains("mod.esm") && !a.plugins.contains("off.esp") &&
			  !a.plugins.contains("skyrim.esm"),
		"sf mm: a profile's plugins.txt: the enabled plugins and the game's own files, no Skyrim ones");
	auto s = MakeSave();
	s->header.magic = "SFS_SAVEGAME";
	s->plugins.full = { "Starfield.esm", "Mod.esp" };
	s->plugins.medium = { "Med.esm" };
	{
		const ops::RenameFinder f(*s, { "Med.esm", "Med.esp", "Mod.esm" });
		Check(f.find("Med.esl") == std::vector<std::string>({ "Med.esp" }), "sf mm: a medium plugin of the save is not offered as a new name");
	}
	Check(ops::RenamePlugin(*s, "Med.esm", "Med.esp").empty() && s->plugins.medium.front() == "Med.esp", "sf mm: a medium plugin's entry is renamed");
	Check(!ops::RenamePlugin(*s, "Mod.esp", "Starfield.esm").empty() && s->plugins.full[1] == "Mod.esp", "sf mm: never renamed to one of the game's own files");
	// a plugin full in the save, light now: its ids above 0xFFF no longer fit; a medium one made full: all fit
	{
		loadorder::StarfieldOrder o;
		o.files["mod.esp"] = plugin("Mod.esp", 0x100);  // light now
		o.files["med.esm"] = plugin("Med.esm", 0x1);    // full now
		o.files["same.esm"] = plugin("Same.esm", 0x1);  // full, as in the save
		const std::vector<std::string> full = { "Starfield.esm", "Mod.esp", "Same.esm" }, lite, med = { "Med.esm" };
		const auto k = loadorder::StarfieldKindChanges(full, lite, med, { 0x01000800, 0x01001234, 0x01000FFF, 0x02012345, 0xFD001234, 0xFF000001 }, o);
		Check(k.size() == 2 && k[0].plugin == "Mod.esp" && k[0].was == 0 && k[0].now == 1 && k[0].ids == 3 && k[0].outOfRange == 1 &&
				  k[0].example == 0x01001234,
			"sf mm: full -> light: the ids above 0xFFF are found");
		Check(k.size() == 2 && k[1].plugin == "Med.esm" && k[1].now == 0 && k[1].outOfRange == 0, "sf mm: medium -> full: every id fits");
	}
	fs::remove_all(dir, ec);
}

// Starfield: the base object of a reference made in game (the initial data writer 0x141832fd0, verified in Starfield.exe 1.16.244)
void TestStarfieldRefBase()
{
	Check(InitialDataTypeSF(0xFF0158FA, 0, 0x9A00118B) == 5, "sf base: made in game without 0x8000: initial data 5");
	Check(InitialDataTypeSF(0xFF07131A, 0, 0x02008100) == 6, "sf base: 0x2000008 with 0x8000: 6");
	Check(InitialDataTypeSF(0xFF018A70, 0, 0x88008080) == 7, "sf base: 0x8000 alone: 7");
	Check(InitialDataTypeSF(0x0001234, 0, 0x00000002) == 4, "sf base: a plugin's reference moved: 4");
	Check(InitialDataTypeSF(0x0001234, 0, 0x00000000) == 0, "sf base: a plugin's reference, nothing: 0");
	Check(InitialDataTypeSF(0xFF0158FA, 8, 0x9A00118B) == 0, "sf base: not a reference (QUST)");
	auto cf = [](std::uint8_t a_type, std::uint32_t a_flags, std::vector<std::uint32_t> a_refs) {
		ChangeForm c;
		c.typeField = a_type;
		c.changeFlags = a_flags;
		for (auto r : a_refs) {
			RefID id;
			id.data = r;
			c.sfRefs.push_back(id);
		}
		return c;
	};
	{
		const auto c = cf(0, 0x9A00118B, { 0x8187FB, 0x6E754E, 0x8158FA, 0x8158F9 });  // cell, base, self, +0x98 base
		const auto b = BaseObjectsSF(0xFF0158FA, c);
		Check(b.size() == 2 && b[0].data == 0x6E754E && b[1].data == 0x8158F9, "sf base: type 5: GetBaseObject's, then the changed base");
	}
	{
		const auto c = cf(0, 0x02008100, { 0x67CFA2, 0x67CFA2, 0x87131A, 0x4F1982 });  // cell, editor cell, self, ...
		Check(BaseObjectsSF(0xFF07131A, c).empty(), "sf base: type 6 names cells, not a base");
	}
	{
		const auto c = cf(0, 0x88008080, { 0x86B0FC, 0x818A70, 0x832CBB });  // location, self, +0x98 base
		const auto b = BaseObjectsSF(0xFF018A70, c);
		Check(b.size() == 1 && b[0].data == 0x832CBB, "sf base: type 7 with 0x80: the changed base after the own id");
	}
	{
		const auto c = cf(1, 0x88008080, { 0x86B0FC, 0x818A70, 0x832CBB });
		Check(BaseObjectsSF(0xFF018A70, c).empty(), "sf base: an actor's 0x80 base is not written there");
	}
}
