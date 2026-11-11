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
		{
			// every quest's state and stages decode within its data (DecodeQuestSF); without the later blocks the data ends there
			std::size_t qn = 0, qbad = 0;
			for (const auto& c : s->changeForms) {
				if (c.typeCode() != 8) {
					continue;
				}
				++qn;
				const auto q = DecodeQuestSF(c);
				qbad += !q.ok;
			}
			if (qbad) {
				fail(std::to_string(qbad) + " of " + std::to_string(qn) + " quests do not decode");
			}
		}
		{
			// every reference's data (version 153) reads to its last byte with its reference list count; actors to the end
			// of their reference part, the player's inventory included
			std::size_t rn = 0, rbad = 0, an = 0, aok = 0, sbad = 0;
			bool        player = false;
			std::string firstBad;
			for (const auto& c : s->changeForms) {
				if (c.typeCode() > 1 || c.version != 153) {
					continue;
				}
				const auto fid = s->formIDOf(c.refID);
				const auto r = DecodeReferenceSF(fid, c);
				if (fid == 0x14 && !r.ok && std::getenv("RS_SF_DEBUG")) {
					std::printf("  player: %s\n", r.error.c_str());
				}
				if (c.typeCode() == 1) {
					++an;
					aok += r.ok;
					if (fid == 0x14) {
						player = r.ok && r.hasInventory && !r.items.empty();
					}
					continue;
				}
				// the strings: a REFR's data is the reference part only, so its string count is the whole list
				if (r.ok && c.typeCode() == 0 && r.referenceStrings != c.sfStrings.size()) {
					++sbad;
					if (std::getenv("RS_SF_DEBUG") && sbad < 5) {
						std::printf("  strings %08X: counted %zu, list %zu\n", fid, r.referenceStrings, c.sfStrings.size());
					}
				}
				++rn;
				// (3 created references in one save of the sample end 83 bytes early with change flag 0x2 alone: not read yet)
				if (!r.ok && !(c.changeFlags == 0x2 && (fid >> 24) == 0xFF)) {
					if (!rbad++) {
						char b[64];
						std::snprintf(b, sizeof(b), "%08X ", fid);
						firstBad = b + r.error;
					}
				}
			}
			if (sbad) {
				fail(std::to_string(sbad) + " references whose strings do not add up");
			}
			if (rbad) {
				fail(std::to_string(rbad) + " of " + std::to_string(rn) + " references do not decode (first: " + firstBad + ")");
			}
			if (an && !player) {
				fail("the player's inventory does not decode");
			}
			// a count changed, written and read back: that count only, everything else as it was
			if (player && n == 0) {
				for (auto& c : s->changeForms) {
					if (c.typeCode() != 1 || s->formIDOf(c.refID) != 0x14) {
						continue;
					}
					const auto before = DecodeReferenceSF(0x14, c);
					const auto st = before.items.back().stacks.back();
					const auto data0 = c.data;
					Check(SetInventoryCountSF(c, st, st.count + 7), "sf inventory: player count set");
					const auto out = fs::temp_directory_path() / ("rs_sfinv_" + name);
					try {
						s->write(out, false);
						auto back = SaveFile::load(out);
						bool same = false, others = true;
						for (std::size_t i = 0; i < back->changeForms.size() && i < s->changeForms.size(); ++i) {
							const auto& b = back->changeForms[i];
							if (b.typeCode() == 1 && back->formIDOf(b.refID) == 0x14) {
								const auto after = DecodeReferenceSF(0x14, b);
								same = after.ok && after.items.size() == before.items.size() &&
									   after.items.back().stacks.back().count == st.count + 7 && b.data.size() == data0.size();
							} else {
								others = others && b.data == s->changeForms[i].data && b.sfRefs.size() == s->changeForms[i].sfRefs.size();
							}
						}
						Check(same && others, "sf inventory: the changed count reads back, nothing else changed");
					} catch (const std::exception& ex) {
						fail(std::string("inventory edit does not write: ") + ex.what());
					}
					std::error_code ec;
					fs::remove(out, ec);
					c.data = data0;
					break;
				}
				// an item added to the player's inventory (one taken from another inventory of the save), written and read back
				for (auto& c : s->changeForms) {
					if (c.typeCode() != 1 || s->formIDOf(c.refID) != 0x14) {
						continue;
					}
					const auto before = DecodeReferenceSF(0x14, c);
					std::set<std::uint32_t> have;
					for (const auto& it : before.items) {
						have.insert(s->formIDOf(c.sfRefs[it.refIndex]));
					}
					std::uint32_t pick = 0;
					for (const auto& o : s->changeForms) {
						if (pick || o.typeCode() != 0 || o.version != 153 || !(o.changeFlags & 0x20)) {
							continue;
						}
						const auto d = DecodeReferenceSF(s->formIDOf(o.refID), o);
						for (const auto& it : d.items) {
							const auto f = s->formIDOf(o.sfRefs[it.refIndex]);
							if (!have.count(f) && (f >> 24) != 0xFF) {
								pick = f;
								break;
							}
						}
					}
					if (!pick) {
						break;
					}
					const auto data0 = c.data;
					const auto refs0 = c.sfRefs;
					const auto ids0 = s->formIDs;
					std::string why;
					if (!AddInventoryItemSF(*s, c, pick, 5, &why)) {
						fail("item not added: " + why);
					}
					Check(!AddInventoryItemSF(*s, c, pick, 5, &why), "sf inventory: the same item is not added twice");
					const auto out = fs::temp_directory_path() / ("rs_sfadd_" + name);
					try {
						s->write(out, false);
						auto back = SaveFile::load(out);
						bool ok = false;
						for (const auto& b : back->changeForms) {
							if (b.typeCode() == 1 && back->formIDOf(b.refID) == 0x14) {
								const auto a = DecodeReferenceSF(0x14, b);
								ok = a.ok && a.items.size() == before.items.size() + 1 && back->formIDOf(b.sfRefs[a.items.back().refIndex]) == pick &&
									 a.items.back().total() == 5 && b.sfRefs.size() == refs0.size() + 1;
								for (std::size_t i = 0; ok && i < before.items.size(); ++i) {
									ok = a.items[i].total() == before.items[i].total() &&
										 back->formIDOf(b.sfRefs[a.items[i].refIndex]) == s->formIDOf(refs0[before.items[i].refIndex]);
								}
							}
						}
						Check(ok, "sf inventory: the added item reads back, the others as they were");
					} catch (const std::exception& ex) {
						fail(std::string("add item does not write: ") + ex.what());
					}
					std::error_code ec;
					fs::remove(out, ec);
					c.data = data0;
					c.sfRefs = refs0;
					s->formIDs = ids0;
					break;
				}
				// the player's XP found among the actor values; a level change written and read back (needs the list of AVIF
				// form IDs of the game's plugins: RESAVERPP_SF_AVIF, one hex ID a line; mods' forms are taken as actor values)
				if (const char* avf = std::getenv("RESAVERPP_SF_AVIF"); avf && *avf) {
					std::set<std::uint32_t> av;
					std::ifstream in(avf);
					for (std::string l; std::getline(in, l);) {
						if (!l.empty()) {
							av.insert(static_cast<std::uint32_t>(std::stoul(l, nullptr, 16)));
						}
					}
					auto isAV = [&](std::uint32_t f) { return av.count(f) || (f > 0 && f < 0x800) || ((f >> 24) != 0 && (f >> 24) != 0xFF); };
					const ChangeForm* pc = nullptr;
					for (const auto& c : s->changeForms) {
						if (c.typeCode() == 1 && s->formIDOf(c.refID) == 0x14) {
							pc = &c;
						}
					}
					if (pc) {
						const auto xp = FindPlayerXPSF(*s, *pc, isAV);
						if (!xp.ok || std::fabs(xp.experience - XPForLevelSF(s->header.playerLevel) - s->header.currentXP) > 1.0f) {
							fail("player XP: " + (xp.ok ? std::string("does not match the header") : xp.error));
						} else {
							const auto data0 = pc->data;
							const auto hdr0 = s->header;
							std::vector<std::uint8_t> npc0;
							for (const auto& c : s->changeForms) {
								if (c.typeCode() == 9 && s->formIDOf(c.refID) == 0x7) {
									npc0 = c.data;
								}
							}
							std::string why;
							const auto lv = static_cast<std::uint16_t>(s->header.playerLevel + 1);
							if (!SetPlayerLevelSF(*s, lv, 100.0f, isAV, &why)) {
								fail("level not set: " + why);
							} else {
								const auto out = fs::temp_directory_path() / ("rs_sflvl_" + name);
								try {
									s->write(out, false);
									auto back = SaveFile::load(out);
									const ChangeForm* bp = nullptr;
									for (const auto& c : back->changeForms) {
										if (c.typeCode() == 1 && back->formIDOf(c.refID) == 0x14) {
											bp = &c;
										}
									}
									const auto x2 = bp ? FindPlayerXPSF(*back, *bp, isAV) : SfPlayerXP{};
									Check(back->header.playerLevel == lv && x2.ok && x2.experience == XPForLevelSF(lv) + 100.0f &&
											  back->header.currentXP == 100.0f,
										"sf xp: a new level and XP read back");
								} catch (const std::exception& ex) {
									fail(std::string("level change does not write: ") + ex.what());
								}
								std::error_code ec;
								fs::remove(out, ec);
							}
							for (auto& c : s->changeForms) {
								if (c.typeCode() == 1 && s->formIDOf(c.refID) == 0x14) {
									c.data = data0;
								}
								if (c.typeCode() == 9 && s->formIDOf(c.refID) == 0x7) {
									c.data = npc0;
								}
							}
							s->header = hdr0;
						}
					}
				}
				// the player renamed, written and read back: the record's first string and the header carry the new name
				for (auto& c : s->changeForms) {
					if (c.typeCode() != 9 || s->formIDOf(c.refID) != 0x7) {
						continue;
					}
					const auto strings0 = s->sfStrings;
					const auto list0 = c.sfStrings;
					const auto name0 = s->header.playerName;
					Check(RenameNpcSF(*s, c, "Test Name"), "sf character: renamed");
					const auto out = fs::temp_directory_path() / ("rs_sfname_" + name);
					try {
						s->write(out, false);
						auto back = SaveFile::load(out);
						bool ok = back->header.playerName == "Test Name";
						for (const auto& b : back->changeForms) {
							if (b.typeCode() == 9 && back->formIDOf(b.refID) == 0x7) {
								ok = ok && !b.sfStrings.empty() && back->sfStrings[b.sfStrings[0]] == "Test Name" && DecodeNpcSF(b).ok;
							}
						}
						Check(ok, "sf character: the new name reads back");
					} catch (const std::exception& ex) {
						fail(std::string("rename does not write: ") + ex.what());
					}
					std::error_code ec;
					fs::remove(out, ec);
					s->sfStrings = strings0;
					c.sfStrings = list0;
					s->header.playerName = name0;
					break;
				}
				// an item taken out of a container (REFR) and out of the player (ACHR): written, read back, gone, the rest kept
				for (const std::uint8_t code : { std::uint8_t(0), std::uint8_t(1) }) {
					for (auto& c : s->changeForms) {
						if (c.typeCode() != code || c.version != 153 || (code == 1 && s->formIDOf(c.refID) != 0x14)) {
							continue;
						}
						const auto fid = s->formIDOf(c.refID);
						const auto r = DecodeReferenceSF(fid, c);
						if (!r.ok || r.items.size() < 2) {
							continue;
						}
						// an item with text when the record is a REFR (its strings must move too), else any
						const SfInventoryItem* pick = nullptr;
						for (const auto& it : r.items) {
							if (code == 0 ? it.stringEnd > it.stringBegin : it.stringEnd == it.stringBegin) {
								pick = &it;
								break;
							}
						}
						if (!pick) {
							continue;
						}
						const auto item = s->formIDOf(c.sfRefs[pick->refIndex]);
						const auto before = c;
						std::size_t n = 0;
						const auto  why = RemoveInventoryItemsSF(*s, c, { item }, &n);
						Check(why.empty() && n == 1, code ? "sf leftovers: an item taken out of the player" : "sf leftovers: a named item taken out of a container");
						const auto out = fs::temp_directory_path() / ("rs_sfleft_" + name);
						try {
							s->write(out, false);
							auto back = SaveFile::load(out);
							bool ok = false;
							for (const auto& b : back->changeForms) {
								if (b.refID == c.refID) {
									const auto rb = DecodeReferenceSF(fid, b);
									ok = rb.ok && rb.items.size() == r.items.size() - 1 && (code == 1 || rb.referenceStrings == b.sfStrings.size());
									for (const auto& it : rb.items) {
										ok = ok && back->formIDOf(b.sfRefs[it.refIndex]) != item;
									}
								}
							}
							Check(ok, "sf leftovers: the record reads back without the item");
						} catch (const std::exception& ex) {
							fail(std::string("item removal does not write: ") + ex.what());
						}
						std::error_code ec;
						fs::remove(out, ec);
						c = before;
						break;
					}
				}
				// a face number of the player changed, written and read back
				for (auto& c : s->changeForms) {
					if (c.typeCode() != 9 || s->formIDOf(c.refID) != 0x7) {
						continue;
					}
					const auto n = DecodeNpcSF(c);
					if (!n.ok || n.face.empty()) {
						break;
					}
					const auto data0 = c.data;
					const auto off = n.face.back().offset;
					Check(SetFaceValueSF(c, off, 0.625f), "sf face: value set");
					const auto out = fs::temp_directory_path() / ("rs_sfface_" + name);
					try {
						s->write(out, false);
						auto back = SaveFile::load(out);
						bool ok = false;
						for (const auto& b : back->changeForms) {
							if (b.typeCode() == 9 && back->formIDOf(b.refID) == 0x7) {
								const auto nb = DecodeNpcSF(b);
								float x = 0;
								if (nb.ok && nb.face.size() == n.face.size()) {
									std::memcpy(&x, b.data.data() + nb.face.back().offset, 4);
								}
								ok = x == 0.625f;
							}
						}
						Check(ok, "sf face: the new value reads back");
					} catch (const std::exception& ex) {
						fail(std::string("face edit does not write: ") + ex.what());
					}
					std::error_code ec;
					fs::remove(out, ec);
					c.data = data0;
					break;
				}
			}
			std::size_t nn = 0, nok = 0;
			std::string firstNpc;
			for (const auto& c : s->changeForms) {
				if (c.typeCode() == 9 && c.version == 153) {
					++nn;
					const auto d = DecodeNpcSF(c);
					nok += d.ok;
					if (!d.ok && firstNpc.empty()) {
						char b[64];
						std::snprintf(b, sizeof(b), "%08X ", s->formIDOf(c.refID));
						firstNpc = b + d.error;
					}
				}
			}
			if (nok != nn) {
				fail(std::to_string(nn - nok) + " of " + std::to_string(nn) + " NPC_ records do not decode (first: " + firstNpc + ")");
			}
			// (16 actors of the 90,166 in the sample hold extra 0x66, a running AI package's data, not read yet)
			if (an && aok * 1000 < an * 999) {
				fail(std::to_string(an - aok) + " of " + std::to_string(an) + " actors do not decode");
			}
			std::printf("  sf %s: %zu references, %zu of %zu actors, %zu NPC_ records decode\n", name.c_str(), rn - rbad, aok, an, nok);
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

void TestStarfieldQuest()
{
	ChangeForm c;
	c.typeField = 8;
	c.changeFlags = 0x80000002u;
	// quest flags 0x3 (enabled, completed), vsval 2 stages: 10 done, 20 not done
	c.data = { 0x03, 0, 0, 0, 2 << 2, 10, 0, 1, 20, 0, 0 };
	auto q = DecodeQuestSF(c);
	Check(q.ok && q.completed() && q.enabled() && q.flagsOffset == 0 && q.stages.size() == 2 && q.stages[0].index == 10 && q.stages[0].done &&
			  q.stages[1].index == 20 && !q.stages[1].done && q.stages[1].offset == 10,
		"sf quest: flags and stages");
	c.changeFlags = 0x80000007u;  // + form flags (6 bytes) and +0x110 (4 bytes)
	c.data.insert(c.data.begin(), 6, 0);
	c.data.insert(c.data.begin() + 10, 4, 0);
	q = DecodeQuestSF(c);
	Check(q.ok && q.flagsOffset == 6 && q.stages.size() == 2 && q.stages[1].offset == 20, "sf quest: after the form flags and +0x110");
	c.data.resize(18);
	Check(!DecodeQuestSF(c).ok, "sf quest: data too short");
}

void TestStarfieldXP()
{
	// the game's XP for a level (0x140d83030), as 21 saves gave it: Experience minus the header's XP
	const std::pair<unsigned, float> known[] = { { 1, 0.0f }, { 2, 200.0f }, { 17, 12545.0f }, { 20, 17225.0f }, { 26, 28895.0f }, { 27, 31150.0f },
		{ 34, 49460.0f }, { 39, 65335.0f }, { 43, 79760.0f }, { 45, 87570.0f }, { 47, 95780.0f }, { 48, 100035.0f }, { 50, 108850.0f },
		{ 52, 118085.0f }, { 73, 241480.0f }, { 74, 248625.0f } };
	bool ok = true;
	for (const auto& [l, v] : known) {
		ok = ok && XPForLevelSF(l) == v;
	}
	Check(ok, "sf xp: the XP for each level matches the game's");
}

void TestStarfieldInventory()
{
	// a plugin's container reference (type 4 initial data: change flag 0x2) with flag 0x20 (inventory): position struct
	// (flags 0x10: no rotation, no trailing byte), then the form's own ID, then 2 items: credits (no extra data) in two
	// stacks, and one item with a stack carrying extra 0x46 (1 byte); then the component byte 0
	ChangeForm c;
	c.typeField = 0;
	c.version = 153;
	c.changeFlags = 0x22;
	c.data = { 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  // position: flags, 3 x f32 (cell is a form)
	const std::vector<std::uint8_t> inv = { 2 << 2,  // 2 items
		2 << 2, 0, 5, 0, 0, 0, 0, 7, 0, 0, 0, 0x40, 0, 0, 0, 0xFE,  // credits: 2 stacks (no extras): 5, 7; u32 0x40, u8 0xFE
		1 << 2, 1 << 2, 0x46, 9, 3, 0, 0, 0, 0x40, 0, 0, 0, 0xFE };  // 1 stack: 1 extra (0x46, 1 byte), count 3
	c.data.insert(c.data.end(), inv.begin(), inv.end());
	c.data.push_back(0);  // no component data
	c.sfRefs.resize(4);   // cell, the form itself, credits, the other item
	auto r = DecodeReferenceSF(0x00012345, c);
	Check(r.ok && r.hasInventory && r.items.size() == 2, "sf inventory: two items");
	if (r.items.size() == 2) {
		Check(r.items[0].refIndex == 2 && r.items[0].stacks.size() == 2 && r.items[0].total() == 12 && r.items[1].refIndex == 3 &&
				  r.items[1].stacks.size() == 1 && r.items[1].stacks[0].count == 3 && r.inventoryRefBegin == 2 && r.inventoryRefEnd == 4,
			"sf inventory: items, stacks, counts and their forms");
		Check(SetInventoryCountSF(c, r.items[1].stacks[0], 250) && DecodeReferenceSF(0x00012345, c).items[1].stacks[0].count == 250,
			"sf inventory: a count changed in place");
	}
	c.sfRefs.resize(3);
	Check(!DecodeReferenceSF(0x00012345, c).ok, "sf inventory: a reference list that does not match is refused");
	c.sfRefs.resize(4);
	c.data.push_back(0);
	Check(!DecodeReferenceSF(0x00012345, c).ok, "sf inventory: a byte too many is refused");
	c.data.pop_back();
	c.data[13 + 1] = 0xFF;  // a stack count larger than the data
	Check(!DecodeReferenceSF(0x00012345, c).ok, "sf inventory: an impossible count is refused, not looped over");
}

