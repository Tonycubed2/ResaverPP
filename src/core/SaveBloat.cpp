// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "SaveBloat.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4.h"
#include "LoadOrder.h"
#include "Ops.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace ess::bloat
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		bool Bit(std::uint32_t f, int n) { return (f >> n) & 1; }

		const char* const kCreatedName = "(created objects, base not known)";
		const char* const kNothingName = "(attached to nothing / no object)";

		// the game's own plugins: big numbers there are normal in a long game
		bool IsGamePlugin(const std::string& a_name)
		{
			const auto l = Lower(a_name);
			if (IsBaseGameFileSF(a_name)) {  // Starfield's own files (the exe's list)
				return true;
			}
			for (const auto& m : loadorder::Fallout4Masters()) {
				if (l == m) {
					return true;
				}
			}
			for (const char* m : { "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm", "skyrimvr.esm",
					 "_resourcepack.esl", "fallout4_vr.esm" }) {
				if (l == m) {
					return true;
				}
			}
			return l.rfind("cc", 0) == 0 && (l.ends_with(".esl") || l.ends_with(".esm"));  // Creation Club
		}

		std::uint64_t HeaderBytes(const ChangeForm& c)
		{
			const int w = (c.typeField >> 6) == 0 ? 1 : (c.typeField >> 6) == 1 ? 2 : 4;
			return 3 + 4 + 1 + 1 + 2u * w;
		}
		std::uint64_t ChangeFormBytes(const ChangeForm& c) { return HeaderBytes(c) + (c.length2 ? c.length2 : c.length1); }
		// Starfield (FORMAT.md 2.5): u8 flags, [u8 version], 3-byte refID, [u32 change flags], vsval counts/length, refs, strings, data
		std::uint64_t VsvalBytes(std::size_t n) { return n < 0x40 ? 1 : n < 0x4000 ? 2 : 4; }
		std::uint64_t ChangeFormBytesSF(const ChangeForm& c)
		{
			std::uint64_t n = 1 + 3;
			if (c.sfFlags & 0x01) {
				n += 1;
			}
			if (c.sfFlags & 0x02) {
				n += 4;
			}
			if (c.sfFlags & 0x08) {
				n += VsvalBytes(c.sfRefs.size());
			}
			if (c.sfFlags & 0x10) {
				n += VsvalBytes(c.sfStrings.size());
			}
			if (c.sfFlags & 0x20) {
				n += VsvalBytes(c.data.size());
			}
			return n + 3 * c.sfRefs.size() + 2 * c.sfStrings.size() + c.data.size();
		}
	}

	std::uint64_t ModBloat::changeFormBytes() const
	{
		std::uint64_t n = 0;
		for (const auto& [k, v] : changeForms) {
			n += v.bytes;
		}
		return n;
	}
	std::uint64_t ModBloat::createdBytes() const
	{
		std::uint64_t n = 0;
		for (const auto& [k, v] : created) {
			n += v.bytes;
		}
		return n;
	}
	std::size_t ModBloat::changeFormCount() const
	{
		std::size_t n = 0;
		for (const auto& [k, v] : changeForms) {
			n += v.count;
		}
		return n;
	}
	std::size_t ModBloat::createdCount() const
	{
		std::size_t n = 0;
		for (const auto& [k, v] : created) {
			n += v.count;
		}
		return n;
	}

	const ModBloat* Report::find(const std::string& a_name) const
	{
		const auto l = Lower(a_name);
		for (const auto& m : mods) {
			if (Lower(m.name) == l) {
				return &m;
			}
		}
		return nullptr;
	}

	std::string Num(std::uint64_t a_n)
	{
		auto s = std::to_string(a_n);
		for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
			s.insert(static_cast<std::size_t>(i), ",");
		}
		return s;
	}

	std::string Bytes(std::uint64_t a_n)
	{
		char b[64];
		if (a_n >= 1024ull * 1024) {
			std::snprintf(b, sizeof(b), "%.1f MB", a_n / (1024.0 * 1024.0));
		} else if (a_n >= 1024) {
			std::snprintf(b, sizeof(b), "%.1f KB", a_n / 1024.0);
		} else {
			std::snprintf(b, sizeof(b), "%llu bytes", static_cast<unsigned long long>(a_n));
		}
		return b;
	}

	Report Build(const SaveFile& a_save, const Options& a_opt)
	{
		Report rep;
		rep.fo4 = a_save.isFO4();
		const bool fo4 = rep.fo4;
		const bool sf = a_save.isSF();

		std::vector<ModBloat>                        rows;
		std::unordered_map<std::string, std::size_t> rowOf;  // lowercase name -> row
		auto row = [&](const std::string& a_name, RowKind a_kind, int a_index = -1) -> std::size_t {
			const auto key = Lower(a_name);
			if (auto it = rowOf.find(key); it != rowOf.end()) {
				return it->second;
			}
			ModBloat m;
			m.name = a_name;
			m.kind = a_kind;
			m.index = a_index;
			rows.push_back(std::move(m));
			rowOf.emplace(key, rows.size() - 1);
			return rows.size() - 1;
		};
		for (std::size_t i = 0; i < a_save.plugins.full.size(); ++i) {
			row(a_save.plugins.full[i], RowKind::kPlugin, static_cast<int>(i));
		}
		for (std::size_t i = 0; i < a_save.plugins.lite.size(); ++i) {
			row(a_save.plugins.lite[i], RowKind::kLight, static_cast<int>(i));
		}
		for (std::size_t i = 0; i < a_save.plugins.medium.size(); ++i) {
			row(a_save.plugins.medium[i], RowKind::kMedium, static_cast<int>(i));
		}
		const auto createdRow = row(kCreatedName, RowKind::kCreated);
		const auto nothingRow = row(kNothingName, RowKind::kNothing);

		auto rowOfForm = [&](std::uint32_t a_formID) -> std::size_t {
			if (a_formID == 0) {
				return nothingRow;
			}
			const auto p = a_save.pluginOf(a_formID);
			if (p == "Created") {
				return createdRow;
			}
			if (auto it = rowOf.find(Lower(p)); it != rowOf.end()) {
				return it->second;
			}
			return row(p == "?" ? std::string("(form of an unknown plugin)") : p, RowKind::kOther);
		};

		// ---- pass 1: created forms' base objects (references: their initial data; Skyrim's leveled actors: their template)
		std::unordered_map<std::uint32_t, std::uint32_t> createdBase;  // created form id -> base form id (may be created too)
		struct Decoded
		{
			bool                                                 ok = false;
			std::vector<std::tuple<std::uint32_t, std::uint64_t, std::int64_t>> inventory;  // item, bytes, count (0: not read)
		};
		std::vector<Decoded> decoded(a_save.changeForms.size());
		if (a_opt.total) {
			*a_opt.total = a_save.changeForms.size();
		}
		for (std::size_t i = 0; i < a_save.changeForms.size(); ++i) {
			if (a_opt.cancel && *a_opt.cancel) {
				return rep;
			}
			if (a_opt.done) {
				*a_opt.done = i;
			}
			const auto& c = a_save.changeForms[i];
			const auto  type = static_cast<ChangeFormType>(c.typeCode());
			if (sf) {
				// Starfield: the reference part (DecodeReferenceSF): a created reference's base, and its inventory
				if (c.typeCode() > 1 || c.version != 153) {
					continue;
				}
				const bool createdSf = c.refID.type() == RefID::Type::kCreated;
				if (!createdSf && !(c.changeFlags & 0x8000020)) {
					continue;
				}
				const auto fid = a_save.formIDOf(c.refID);
				const auto r = DecodeReferenceSF(fid, c);
				if (createdSf) {
					for (const auto& b : BaseObjectsSF(fid, c)) {
						if (const auto bf = a_save.formIDOf(b)) {
							createdBase[fid] = bf;
							break;
						}
					}
				}
				if (!r.ok) {
					++rep.decodeFailures;
					continue;
				}
				auto& d = decoded[i];
				d.ok = true;
				for (const auto& it : r.items) {
					const auto item = it.refIndex < c.sfRefs.size() ? a_save.formIDOf(c.sfRefs[it.refIndex]) : 0;
					d.inventory.push_back({ item, it.end - it.begin, static_cast<std::int64_t>(it.total()) });
				}
				continue;
			}
			if (type != ChangeFormType::REFR && type != ChangeFormType::ACHR) {
				continue;
			}
			const bool created = c.refID.type() == RefID::Type::kCreated;
			const bool inv = Bit(c.changeFlags, 5) || Bit(c.changeFlags, 27);
			if (!created && !inv) {
				continue;
			}
			auto& d = decoded[i];
			const auto fid = a_save.formIDOf(c.refID);
			try {
				if (fo4) {
					const auto r = fo4cf::DecodeRefr(a_save, c);
					if (!r.decoded) {
						++rep.decodeFailures;
						continue;
					}
					d.ok = true;
					if (created && !r.base.isZero()) {
						createdBase[fid] = a_save.formIDOf(r.base);
					}
					for (const auto& it : r.inventory) {
						d.inventory.push_back({ it.formID, it.range.size(), it.count });
					}
				} else {
					const auto r = cf::Decode(a_save, c, {}, true);
					if (!r.error.empty() && r.inventory.empty() && !r.baseObject) {
						++rep.decodeFailures;
						continue;
					}
					d.ok = true;
					if (created) {
						// a leveled actor's base is itself rebuilt at every load: its template says which mod it is
						const auto b = r.leveledTemplate ? r.leveledTemplate : r.baseObject;
						if (b) {
							createdBase[fid] = b;
						}
					}
					for (const auto& it : r.inventory) {
						d.inventory.push_back({ it.item, it.endAt > it.entryAt ? it.endAt - it.entryAt : 0, 0 });
					}
				}
			} catch (const std::exception&) {
				++rep.decodeFailures;
			}
		}
		// the plugin a created form comes from: follow its base (a created reference of a created base...)
		auto createdRowOf = [&](std::uint32_t a_formID) -> std::size_t {
			std::uint32_t f = a_formID;
			for (int hop = 0; hop < 8 && (f >> 24) == 0xFF; ++hop) {
				auto it = createdBase.find(f);
				if (it == createdBase.end()) {
					return createdRow;
				}
				f = it->second;
			}
			return (f >> 24) == 0xFF ? createdRow : rowOfForm(f);
		};
		auto rowOfAny = [&](std::uint32_t a_formID) { return (a_formID >> 24) == 0xFF ? createdRowOf(a_formID) : rowOfForm(a_formID); };

		// ---- change forms
		for (std::size_t i = 0; i < a_save.changeForms.size(); ++i) {
			const auto& c = a_save.changeForms[i];
			const auto  bytes = sf ? ChangeFormBytesSF(c) : ChangeFormBytes(c);
			rep.changeFormBytes += bytes;
			const std::string typeName = a_save.cfTypeName(c);
			const auto        fid = a_save.formIDOf(c.refID);
			const bool        created = c.refID.type() == RefID::Type::kCreated;
			std::uint64_t     invBytes = 0;
			for (const auto& [item, b, cnt] : decoded[i].inventory) {
				auto& r = rows[rowOfAny(item)];
				r.inventory.add(b);
				r.itemCount += cnt;
				invBytes += b;
			}
			const auto own = bytes > invBytes ? bytes - invBytes : 0;
			if (created) {
				const auto r = createdRowOf(fid);
				if (r == createdRow) {
					++rep.createdUnknown;
				}
				rows[r].created[typeName].add(own);
			} else {
				rows[rowOfForm(fid)].changeForms[typeName].add(own);
			}
		}

		// ---- Papyrus
		if (const auto* p = a_save.papyrus()) {
			Writer w;
			w.setLE(a_save.isLE());
			w.str16 = a_save.str16();
			w.fo4 = fo4;
			auto sizeOf = [&](auto&& a_write) -> std::uint64_t {
				w.buf.clear();
				a_write();
				return w.buf.size();
			};
			{
				Writer all;
				all.setLE(a_save.isLE());
				all.str16 = a_save.str16();
				all.fo4 = fo4;
				p->write(all);
				rep.papyrusBytes = all.buf.size();
			}

			// the mod that ships a script (scanned sources), as a row: its plugin if the save has it, else "[folder]"
			const bool hasSources = a_opt.sources && !a_opt.sources->empty();
			std::unordered_map<std::string, std::size_t> folderRow;
			if (hasSources) {
				for (const auto& [plugin, folder] : a_opt.sources->pluginFolder) {
					if (folder == "Game Data" || folder == "Data" || folder == "Overwrite") {
						continue;
					}
					if (auto it = rowOf.find(Lower(plugin)); it != rowOf.end()) {
						folderRow.emplace(folder, it->second);
					}
				}
			}
			auto providerRow = [&](const std::string& a_script) -> std::optional<std::size_t> {
				if (!hasSources) {
					return std::nullopt;
				}
				const auto* v = a_opt.sources->providers(a_script);
				if (!v || v->empty()) {
					return std::nullopt;
				}
				const auto& folder = a_opt.sources->ranked ? v->front().mod : v->back().mod;
				if (folder.empty() || folder == "Game Data" || folder == "Data") {
					return std::nullopt;
				}
				if (auto it = folderRow.find(folder); it != folderRow.end()) {
					return it->second;
				}
				const auto r = row("[" + folder + "]", RowKind::kFolder);
				folderRow.emplace(folder, r);
				return r;
			};

			std::unordered_map<EID, std::size_t> instRow;
			instRow.reserve(p->instances.size());
			for (const auto& inst : p->instances) {
				const auto& script = p->str(inst.script);
				std::size_t r;
				if (inst.refID.isZero() || (sf && inst.unknown2bits > 2)) {  // (Starfield handle kinds 3+: not a form id)
					r = providerRow(script).value_or(nothingRow);
				} else {
					const auto fid = a_save.formIDOf(inst.refID);
					if ((fid >> 24) == 0xFF || inst.refID.type() == RefID::Type::kCreated) {
						r = createdRowOf(fid);
						if (r == createdRow) {
							r = providerRow(script).value_or(createdRow);
						}
					} else {
						r = rowOfForm(fid);
					}
				}
				instRow[inst.id] = r;
				const auto b = sf ? sizeOf([&] {
					// Starfield: the object table entry (u64 id, u32 script, handle, u8) + its object data
					w.u64(inst.id);
					w.u32(inst.script);
					w.u16(inst.unknown2bits);
					w.u16(inst.unknown);
					w.bytes(std::vector<std::uint8_t>(inst.unknown2bits > 2 ? 4 : 3, 0));
					w.u8(inst.unknownByte);
					if (inst.data) {
						WriteObjectDataSF(w, inst.id, *inst.data);
					}
				}) : sizeOf([&] {
					w.eid(inst.id);
					w.tstr(inst.script);
					w.u16(inst.unknown2bits);
					w.u16(inst.unknown);
					inst.refID.write(w);
					w.u8(inst.unknownByte);
					if (inst.unknownFO4Byte) {
						w.u8(*inst.unknownFO4Byte);
					}
					if (inst.data) {
						w.eid(inst.id);
						w.u8(inst.data->flag);
						w.tstr(inst.data->state);
						w.u32(inst.data->unknown1);
						if (inst.data->flag & 4) {
							w.u32(inst.data->unknown2);
						}
						w.u32(static_cast<std::uint32_t>(inst.data->vars.size()));
						for (const auto& v : inst.data->vars) {
							v.write(w);
						}
					}
				});
				rows[r].instances.add(b);
				rows[r].scripts[script].add(b);
			}

			// arrays and structs belong to whoever holds them: walk the values from instances, threads, structs, arrays
			std::unordered_map<EID, std::size_t> arrRow, structRow;
			std::unordered_map<EID, const StructInstance*> structById;
			for (const auto& s : p->structInstances) {
				structById[s.id] = &s;
			}
			std::deque<std::pair<const std::vector<Variable>*, std::size_t>> work;
			auto visit = [&](const Variable& v, std::size_t r, auto&& self) -> void {
				if (v.type == VType::kVariant) {
					if (v.inner) {
						self(*v.inner, r, self);
					}
					return;
				}
				if (v.value == 0) {
					return;
				}
				if (IsArrayType(v.type)) {
					if (arrRow.emplace(v.value, r).second) {
						if (const auto* a = p->findArray(v.value)) {
							work.push_back({ &a->vars, r });
						}
					}
				} else if (v.type == VType::kStruct) {
					if (structRow.emplace(v.value, r).second) {
						if (auto it = structById.find(v.value); it != structById.end()) {
							work.push_back({ &it->second->vars, r });
						}
					}
				}
			};
			auto drain = [&] {
				while (!work.empty()) {
					auto [vars, r] = work.front();
					work.pop_front();
					for (const auto& v : *vars) {
						visit(v, r, visit);
					}
				}
			};
			for (const auto& inst : p->instances) {
				if (inst.data) {
					work.push_back({ &inst.data->vars, instRow[inst.id] });
				}
			}
			drain();

			// threads: by the object running the first frame, else the mod shipping its script
			for (const auto& a : p->activeScripts) {
				std::size_t r = nothingRow;
				std::string script;
				if (a.data && !a.data->frames.empty()) {
					const auto& f0 = a.data->frames.front();
					script = p->str(f0.scriptName);
					bool found = false;
					if (f0.owner.type == VType::kRef && f0.owner.value != 0) {
						if (auto it = instRow.find(f0.owner.value); it != instRow.end()) {
							r = it->second;
							found = true;
						}
					}
					if (!found) {
						r = providerRow(script).value_or(nothingRow);
					}
				}
				const auto b = sizeOf([&] {
					w.u32(a.id);
					w.u8(a.type);
					if (a.data) {
						if (sf) {
							WriteThreadDataSF(w, a.id, *a.data);
						} else {
							WriteThreadData(w, a.id, *a.data);
						}
					}
				});
				rows[r].threads.add(b);
				rows[r].threadScripts[script.empty() ? std::string("?") : script].add(b);
				if (a.data) {
					for (const auto& f : a.data->frames) {
						work.push_back({ &f.vars, r });
					}
				}
			}
			drain();

			for (const auto& a : p->arrays) {
				auto       it = arrRow.find(a.id);
				const auto r = it == arrRow.end() ? nothingRow : it->second;
				const auto b = sf ? sizeOf([&] {
					w.u64(a.id);
					w.u8(static_cast<std::uint8_t>(a.type));
					if (IsRefType(a.type)) {
						w.u32(a.refType);
					}
					w.u32(a.length);
					if (a.hasData) {
						w.u64(a.id);
						for (const auto& v : a.vars) {
							WriteVariableSF(w, v);
						}
					}
				}) : sizeOf([&] {
					w.eid(a.id);
					w.u8(static_cast<std::uint8_t>(a.type));
					if (IsRefType(a.type)) {
						w.tstr(a.refType);
					}
					w.u32(a.length);
					if (a.hasData) {
						w.eid(a.id);
						for (const auto& v : a.vars) {
							v.write(w);
						}
					}
				});
				rows[r].arrays.add(b);
			}
			for (const auto& s : p->structInstances) {
				auto       it = structRow.find(s.id);
				const auto r = it == structRow.end() ? nothingRow : it->second;
				const auto b = sf ? sizeOf([&] {
					w.u64(s.id);
					w.u32(s.def);
					if (s.hasData) {
						w.u64(s.id);
						w.u8(s.flag);
						w.u32(static_cast<std::uint32_t>(s.vars.size()));
						for (const auto& v : s.vars) {
							WriteVariableSF(w, v);
						}
					}
				}) : sizeOf([&] {
					w.eid(s.id);
					w.tstr(s.def);
					if (s.hasData) {
						w.eid(s.id);
						w.u8(s.flag);
						w.u32(static_cast<std::uint32_t>(s.vars.size()));
						for (const auto& v : s.vars) {
							v.write(w);
						}
					}
				});
				rows[r].structs.add(b);
			}
			std::uint64_t pap = 0;
			for (const auto& m : rows) {
				pap += m.papyrusBytes();
			}
			rep.papyrusOther = rep.papyrusBytes > pap ? rep.papyrusBytes - pap : 0;
		}
		if (a_opt.done) {
			*a_opt.done = a_save.changeForms.size();
		}

		// ---- totals, order, causes
		try {
			rep.bodyBytes = a_save.buildBody().size();
		} catch (const std::exception&) {
			rep.bodyBytes = 0;
		}
		for (auto& m : rows) {
			rep.attributed += m.total();
		}
		rows.erase(std::remove_if(rows.begin(), rows.end(), [](const ModBloat& m) { return m.total() == 0; }), rows.end());
		std::stable_sort(rows.begin(), rows.end(), [](const ModBloat& a, const ModBloat& b) { return a.total() > b.total(); });
		rep.mods = std::move(rows);

		std::vector<Cause> all;
		for (const auto& m : rep.mods) {
			const bool game = IsGamePlugin(m.name);
			const bool unknownCreated = m.kind == RowKind::kCreated;
			const bool nothing = m.kind == RowKind::kNothing;
			for (const auto& [script, c] : m.scripts) {
				Cause x{ m.name, "instances", script, c.count, c.bytes };
				x.engine = ops::IsEngineType(script);
				if (nothing) {
					x.text = Num(c.count) + " script instances of " + script + " attached to nothing (" + Bytes(c.bytes) + ")";
					x.advice = "Their object is gone; they do nothing but take space. Remove unattached instances clears them.";
					x.tool = Tool::kUnattached;
				} else if (unknownCreated) {
					x.text = Num(c.count) + " script instances of " + script + " on created objects whose mod is not known (" + Bytes(c.bytes) + ")";
					x.advice = sf ? "Objects made while playing. A script that should not be there at all can be purged (Starfield keeps many such "
					                "objects outside the change forms, so they are not counted as gone)."
					              : "Objects spawned while playing. If their objects are gone, Missing spawned objects removes them; a script that "
					                "should not be there at all can be purged.";
					x.tool = sf ? Tool::kModsView : Tool::kOrphans;
				} else {
					x.text = Num(c.count) + " script instances of " + script + " on " + m.name + "'s objects (" + Bytes(c.bytes) + ")";
					x.advice = game ? "Scripts on the game's own objects: normal in a long game unless one script has far more than the rest."
					                : "Many instances of one script usually means the mod attaches it to every object it touches or spawns. "
					                  "If the mod is gone, its scripts can be removed; if not, see the mod's page for a cleanup option.";
					x.tool = Tool::kModsView;
				}
				all.push_back(std::move(x));
			}
			for (const auto& [script, c] : m.threadScripts) {
				Cause x{ m.name, "threads", script, c.count, c.bytes };
				x.text = Num(c.count) + " running script thread(s) of " + script + (nothing ? "" : " (" + m.name + ")") + " (" + Bytes(c.bytes) + ")";
				x.advice = "Threads are scripts that were running when the game saved. Many of one script means it is stuck or called over and "
				           "over; they load with the save and keep the script engine busy.";
				x.tool = Tool::kModsView;
				all.push_back(std::move(x));
			}
			if (m.arrays.count) {
				Cause x{ m.name, "arrays", "", m.arrays.count, m.arrays.bytes };
				x.text = Num(m.arrays.count) + " script arrays held by " + m.name + " (" + Bytes(m.arrays.bytes) + ")";
				x.advice = "Arrays a mod's scripts keep (lists of objects, values...). Analyze arrays shows the biggest and the ones nothing points at.";
				x.tool = Tool::kArrays;
				all.push_back(std::move(x));
			}
			if (m.structs.count) {
				Cause x{ m.name, "structs", "", m.structs.count, m.structs.bytes };
				x.text = Num(m.structs.count) + " script structs held by " + m.name + " (" + Bytes(m.structs.bytes) + ")";
				x.advice = "Struct values a mod's scripts keep (usually inside arrays).";
				x.tool = Tool::kModsView;
				all.push_back(std::move(x));
			}
			for (const auto& [type, c] : m.changeForms) {
				Cause x{ m.name, "changeforms", type, c.count, c.bytes };
				x.text = Num(c.count) + " changed " + type + " forms of " + m.name + " (" + Bytes(c.bytes) + ")";
				x.advice = game ? "The game's own objects changed while playing (moved, picked up, killed...): this grows with every place visited "
				                  "and is normal."
				                : "Forms of this mod that the game had to remember. Normal for a mod that adds places or NPCs; if the mod was removed "
				                  "they stay behind (Mods no longer installed).";
				x.tool = Tool::kNone;
				all.push_back(std::move(x));
			}
			for (const auto& [type, c] : m.created) {
				Cause x{ m.name, "created", type, c.count, c.bytes };
				x.text = unknownCreated ? Num(c.count) + " created " + type + " forms whose base could not be read (" + Bytes(c.bytes) + ")"
				                        : Num(c.count) + " created objects (" + type + ") with a base from " + m.name + " (" + Bytes(c.bytes) + ")";
				x.advice = type == "ACHR" ? "Spawned or leveled NPCs and creatures. Thousands of them usually means a mod keeps spawning actors that are "
				                            "never cleaned up."
				                          : "Objects made while playing (dropped items, spawned containers, placed objects). A mod that keeps "
				                            "creating them without deleting them makes saves grow.";
				x.tool = type == "ACHR" ? Tool::kFindNpcs : Tool::kCompare;
				all.push_back(std::move(x));
			}
			if (m.inventory.count) {
				Cause x{ m.name, "inventory", "", m.inventory.count, m.inventory.bytes };
				x.text = Num(m.inventory.count) + " inventory entries of " + m.name + "'s items (" +
				         (m.itemCount > 0 ? Num(static_cast<std::uint64_t>(m.itemCount)) + " items, " : std::string()) + Bytes(m.inventory.bytes) + ")";
				x.advice = "Items of this mod in the player's, NPCs' and containers' inventories. Leftover items removes those of mods that are gone.";
				x.tool = Tool::kLeftovers;
				all.push_back(std::move(x));
			}
		}
		for (auto& c : all) {
			c.game = IsGamePlugin(c.mod);
		}
		std::stable_sort(all.begin(), all.end(), [](const Cause& a, const Cause& b) { return a.bytes > b.bytes; });
		// the biggest of the mods' and of the game's own, a_opt.maxCauses each (the game's would crowd out the mods' otherwise)
		std::size_t mods = 0, game = 0;
		for (auto& c : all) {
			auto& n = c.game ? game : mods;
			if (n < a_opt.maxCauses) {
				++n;
				rep.causes.push_back(std::move(c));
			}
		}
		return rep;
	}

	std::vector<Growth> Compare(const Report& a_older, const Report& a_newer)
	{
		std::map<std::string, Growth> by;
		auto take = [&](const Report& r, bool newer) {
			for (const auto& m : r.mods) {
				auto& g = by[Lower(m.name)];
				if (g.mod.empty() || newer) {
					g.mod = m.name;
				}
				const auto b = static_cast<std::int64_t>(m.total());
				const auto in = static_cast<std::int64_t>(m.instances.count);
				const auto cr = static_cast<std::int64_t>(m.createdCount());
				const auto cf = static_cast<std::int64_t>(m.changeFormCount());
				const auto th = static_cast<std::int64_t>(m.threads.count);
				(newer ? g.newBytes : g.oldBytes) = b;
				(newer ? g.newInstances : g.oldInstances) = in;
				(newer ? g.newCreated : g.oldCreated) = cr;
				(newer ? g.newChangeForms : g.oldChangeForms) = cf;
				(newer ? g.newThreads : g.oldThreads) = th;
			}
		};
		take(a_older, false);
		take(a_newer, true);
		std::vector<Growth> out;
		for (auto& [k, g] : by) {
			if (g.delta() != 0) {
				out.push_back(g);
			}
		}
		std::stable_sort(out.begin(), out.end(), [](const Growth& a, const Growth& b) { return a.delta() > b.delta(); });
		return out;
	}

	std::vector<std::string> Describe(const Report& a_report, std::size_t a_rows)
	{
		std::vector<std::string> out;
		out.push_back("Save body " + Bytes(a_report.bodyBytes) + ": Papyrus " + Bytes(a_report.papyrusBytes) + ", change forms " +
					  Bytes(a_report.changeFormBytes) + " (sizes uncompressed). Shared Papyrus data (strings, script definitions, queued calls): " +
					  Bytes(a_report.papyrusOther) + ".");
		if (a_report.decodeFailures) {
			out.push_back(Num(a_report.decodeFailures) + " object(s) could not be read; their inventory and base are counted with the object.");
		}
		out.push_back("");
		out.push_back("By mod (total = Papyrus + change forms + created objects + inventory entries):");
		std::size_t n = 0;
		for (const auto& m : a_report.mods) {
			if (n++ >= a_rows) {
				break;
			}
			char line[512];
			std::snprintf(line, sizeof(line), "  %-44s %10s  instances %s (%s), threads %s, arrays %s, change forms %s (%s), created %s (%s), inventory %s",
				m.name.c_str(), Bytes(m.total()).c_str(), Num(m.instances.count).c_str(), Bytes(m.instances.bytes).c_str(), Num(m.threads.count).c_str(),
				Num(m.arrays.count).c_str(), Num(m.changeFormCount()).c_str(), Bytes(m.changeFormBytes()).c_str(), Num(m.createdCount()).c_str(),
				Bytes(m.createdBytes()).c_str(), Num(m.inventory.count).c_str());
			out.push_back(line);
		}
		out.push_back("");
		out.push_back("Biggest causes (mods):");
		for (const auto& c : a_report.causes) {
			if (!c.game) {
				out.push_back("  - " + c.text);
			}
		}
		out.push_back("Biggest causes (the game's own plugins: normal in a long game):");
		for (const auto& c : a_report.causes) {
			if (c.game) {
				out.push_back("  - " + c.text);
			}
		}
		return out;
	}

	std::vector<std::string> DescribeGrowth(const std::vector<Growth>& a_growth, std::size_t a_rows)
	{
		std::vector<std::string> out;
		std::size_t              n = 0;
		for (const auto& g : a_growth) {
			if (n++ >= a_rows) {
				break;
			}
			const auto d = g.delta();
			out.push_back("  " + g.mod + ": " + (d >= 0 ? "+" : "-") + Bytes(static_cast<std::uint64_t>(d >= 0 ? d : -d)) + " (" + Bytes(static_cast<std::uint64_t>(g.oldBytes)) +
						  " -> " + Bytes(static_cast<std::uint64_t>(g.newBytes)) + "), instances " + std::to_string(g.oldInstances) + " -> " +
						  std::to_string(g.newInstances) + ", created " + std::to_string(g.oldCreated) + " -> " + std::to_string(g.newCreated) +
						  ", threads " + std::to_string(g.oldThreads) + " -> " + std::to_string(g.newThreads));
		}
		return out;
	}
}
