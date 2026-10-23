// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "ModRemove.h"

#include "ByteIO.h"
#include "Ops.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace ess::modremove
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

		std::string U8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return std::string(s.begin(), s.end());
		}

		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[1024];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		// the EID a variable points at (ref / array / struct), 0 if none
		EID Target(const Variable& v)
		{
			const Variable* x = &v;
			if (x->type == VType::kVariant && x->inner) {
				x = x->inner.get();
			}
			if (x->type == VType::kRef || x->type == VType::kStruct || IsArrayType(x->type)) {
				return x->value;
			}
			return 0;
		}

		bool IsBaseGamePlugin(const std::string& lower)
		{
			static const char* v[] = { "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm", "skyrimvr.esm" };
			for (const char* n : v) {
				if (lower == n) {
					return true;
				}
			}
			return false;
		}

		// Official Creation Club content: "cc" + a 3-4 letter creator code + "sse" + 3 digits, e.g.
		// ccbgssse001-fish.esm, ccqdrsse001-survivalmode.esl. Not just any name starting with "cc".
		bool IsCreationClub(const std::string& lower)
		{
			if (lower.size() < 11 || lower.rfind("cc", 0) != 0) {
				return false;
			}
			for (std::size_t letters : { std::size_t(3), std::size_t(4) }) {
				const std::size_t s = 2 + letters;
				if (lower.size() < s + 6) {
					continue;
				}
				bool ok = true;
				for (std::size_t i = 2; i < s; ++i) {
					ok = ok && std::isalpha(static_cast<unsigned char>(lower[i]));
				}
				ok = ok && lower.compare(s, 3, "sse") == 0;
				for (std::size_t i = s + 3; i < s + 6; ++i) {
					ok = ok && std::isdigit(static_cast<unsigned char>(lower[i]));
				}
				if (ok) {
					return true;
				}
			}
			return false;
		}

		bool IsVanillaPlugin(const std::string& lower) { return IsBaseGamePlugin(lower) || IsCreationClub(lower); }
	}

	bool IsCreationClubPlugin(const std::string& a_lower) { return IsCreationClub(a_lower); }

	std::vector<std::string> ReadMasters(const std::filesystem::path& a_plugin)
	{
		std::ifstream f(a_plugin, std::ios::binary);
		if (!f) {
			throw std::runtime_error("cannot open " + U8(a_plugin.filename()));
		}
		std::uint8_t hdr[24];
		if (!f.read(reinterpret_cast<char*>(hdr), 24) || std::memcmp(hdr, "TES4", 4) != 0) {
			throw std::runtime_error(U8(a_plugin.filename()) + " is not a plugin");
		}
		std::uint32_t size;
		std::memcpy(&size, hdr + 4, 4);
		if (size > (16u << 20)) {
			throw std::runtime_error(U8(a_plugin.filename()) + ": header too large");
		}
		std::vector<std::uint8_t> data(size);
		f.read(reinterpret_cast<char*>(data.data()), size);
		data.resize(static_cast<std::size_t>(f.gcount()));
		std::vector<std::string> out;
		Reader                   r(data.data(), data.size());
		std::uint32_t            bigSize = 0;
		while (r.remaining() >= 6) {
			auto        t = r.bytes(4);
			std::string type(t.begin(), t.end());
			std::uint32_t len = r.u16();
			if (bigSize) {
				len = bigSize;
				bigSize = 0;
			}
			if (r.remaining() < len) {
				break;
			}
			auto body = r.bytes(len);
			if (type == "XXXX" && len == 4) {
				std::memcpy(&bigSize, body.data(), 4);
			} else if (type == "MAST") {
				std::string m(body.begin(), body.end());
				while (!m.empty() && m.back() == '\0') {
					m.pop_back();
				}
				out.push_back(Lower(m));
			}
		}
		return out;
	}

	Plan MakePlan(SaveFile& a_save, const std::string& a_mod, const ScriptSources* a_src, const Options& a_opt)
	{
		Plan plan;
		plan.name = a_mod;
		auto* p = a_save.papyrus();
		if (!p) {
			plan.blockers.push_back("The save has no Papyrus section.");
			return plan;
		}
		const bool haveSources = a_src && !a_src->empty();
		const auto lowerName = Lower(a_mod);

		// ---- what is being removed: a plugin, or a script-only mod folder ("[folder]") --------
		for (const auto* list : { &a_save.plugins.full, &a_save.plugins.lite }) {
			for (const auto& pl : *list) {
				if (Lower(pl) == lowerName) {
					plan.isPlugin = true;
				}
			}
		}
		if (plan.isPlugin) {
			if (IsVanillaPlugin(lowerName)) {
				plan.blockers.push_back(a_mod + " is part of the base game or Creation Club content and cannot be removed.");
				return plan;
			}
			if (haveSources) {
				if (auto it = a_src->pluginFolder.find(lowerName); it != a_src->pluginFolder.end()) {
					plan.folder = it->second;
				}
			}
		} else if (a_mod.size() > 2 && a_mod.front() == '[' && a_mod.back() == ']') {
			plan.folder = a_mod.substr(1, a_mod.size() - 2);
		} else {
			plan.folder = a_mod;
		}
		if (!plan.isPlugin && (!haveSources || plan.folder.empty())) {
			plan.blockers.push_back(a_mod + " is not a plugin in this save, and without a scanned mods folder its scripts cannot be identified.");
			return plan;
		}
		if (plan.folder == "Game Data" || plan.folder == "Overwrite") {
			plan.blockers.push_back("\"" + plan.folder + "\" is not a mod and cannot be removed.");
			return plan;
		}

		// ---- scripts that only this mod provides ------------------------------------------------
		std::unordered_set<std::string> ownLower;
		if (haveSources && plan.folder.empty() && plan.isPlugin) {
			plan.notes.push_back(a_mod + " is not installed in the scanned mods folder any more (already uninstalled). Its scripts on other "
										 "objects cannot be identified, so only what sits on its own objects is removed.");
			if (a_src->ranked) {
				plan.enabledInProfile = 0;
			}
		}
		if (haveSources && !plan.folder.empty()) {
			if (!a_src->gameDataScanned) {
				plan.notes.push_back(
					"The game's Data folder was not scanned, so vanilla scripts this mod overrides cannot be told apart; "
					"only scripts on its own objects are removed.");
			} else if (a_opt.otherObjects) {
				std::unordered_set<std::string> seen;
				for (const auto& s : p->scripts) {
					const auto& name = p->str(s.name);
					const auto  key = Lower(name);
					if (!seen.insert(key).second || ops::IsEngineType(name) || ops::IsNativeScriptName(name)) {
						continue;
					}
					const auto* prov = a_src->providers(name);
					if (!prov) {
						continue;
					}
					bool mine = false, others = false;
					for (const auto& pr : *prov) {
						if (pr.mod == plan.folder) {
							mine = true;
						} else if (pr.enabled) {
							others = true;
						}
					}
					if (mine && !others) {
						ownLower.insert(key);
						plan.ownScripts.insert(name);
					}
				}
			}
		} else if (!haveSources) {
			plan.notes.push_back("Mods folder not scanned: only scripts on this plugin's own objects are removed, masters and DLLs are not checked.");
		}
		auto isOwn = [&](TStr t) { return !ownLower.empty() && ownLower.contains(Lower(p->str(t))); };

		// ---- instances ------------------------------------------------------------------------
		auto onOwnForm = [&](const RefID& id) {
			if (!plan.isPlugin || id.isZero() || id.type() == RefID::Type::kCreated) {
				return false;
			}
			return Lower(a_save.pluginOf(a_save.formIDOf(id))) == lowerName;
		};
		for (const auto& i : p->instances) {
			if (onOwnForm(i.refID)) {
				plan.instances.insert(i.id);
				++plan.instancesOnOwnForms;
			} else if (isOwn(i.script)) {
				plan.instances.insert(i.id);
				++plan.instancesElsewhere;
			}
		}
		auto removedInst = [&](const Variable& v) {
			const auto t = Target(v);
			return t && plan.instances.contains(t);
		};

		// ---- threads ---------------------------------------------------------------------------
		for (const auto& a : p->activeScripts) {
			if (!a.data) {
				continue;
			}
			bool hit = false;
			for (const auto& f : a.data->frames) {
				if (isOwn(f.scriptName) || removedInst(f.owner)) {
					hit = true;
					break;
				}
			}
			if (!hit) {
				continue;
			}
			plan.threads.insert(a.id);
			const auto& f0 = a.data->frames.front();
			if (!isOwn(f0.scriptName) && !removedInst(f0.owner)) {
				++plan.threadsOfOtherMods;  // started elsewhere, currently inside this mod
			}
		}
		// suspended stacks and queued calls: of those threads, of its scripts, or called on its
		// instances. A thread whose pending call is removed is removed with it (it would wait forever).
		auto addThread = [&](ThreadID id) {
			if (!plan.threads.contains(id) && p->findActive(id)) {
				plan.threads.insert(id);
				++plan.threadsOfOtherMods;
				return true;
			}
			return false;
		};
		for (bool changed = true; changed;) {
			changed = false;
			for (const auto* list : { &p->suspended1, &p->suspended2 }) {
				for (const auto& st : *list) {
					if (plan.stacks.contains(st.id)) {
						continue;
					}
					if (plan.threads.contains(st.id) || (st.data && (isOwn(st.data->scriptName) || removedInst(st.data->unknownVar)))) {
						plan.stacks.insert(st.id);
						changed |= addThread(st.id);
					}
				}
			}
			for (std::size_t i = 0; i < p->functionMessages.size(); ++i) {
				if (plan.messages.contains(i)) {
					continue;
				}
				const auto& m = p->functionMessages[i];
				if ((m.id && (plan.threads.contains(*m.id) || plan.stacks.contains(*m.id))) ||
					(m.data && (isOwn(m.data->scriptName) || removedInst(m.data->unknownVar)))) {
					plan.messages.insert(i);
					if (m.id) {
						changed |= addThread(*m.id);
					}
				}
			}
		}
		for (const auto& u : p->unbinds) {
			if (plan.instances.contains(u.id)) {
				plan.unbinds.insert(u.id);
			}
		}

		// ---- arrays only the removed elements use; links that other scripts lose -------------
		std::unordered_set<EID> candidates;
		auto                    collect = [&](const std::vector<Variable>& vars) {
            for (const auto& v : vars) {
                const auto t = Target(v);
                if (t && IsArrayType(v.type == VType::kVariant && v.inner ? v.inner->type : v.type)) {
                    candidates.insert(t);
                }
            }
		};
		for (const auto& i : p->instances) {
			if (plan.instances.contains(i.id) && i.data) {
				collect(i.data->vars);
			}
		}
		for (const auto& a : p->activeScripts) {
			if (plan.threads.contains(a.id) && a.data) {
				for (const auto& f : a.data->frames) {
					collect(f.vars);
				}
			}
		}
		std::unordered_set<EID> keepArrays;
		auto                    scanRemaining = [&](const std::vector<Variable>& vars, const std::string& owner) {
            for (const auto& v : vars) {
                const auto t = Target(v);
                if (!t) {
                    continue;
                }
                if (candidates.contains(t)) {
                    keepArrays.insert(t);
                }
                if (plan.instances.contains(t)) {
                    ++plan.linksFromOthers[owner];
                }
            }
		};
		for (const auto& i : p->instances) {
			if (!plan.instances.contains(i.id) && i.data) {
				scanRemaining(i.data->vars, p->str(i.script));
			}
		}
		for (const auto& r : p->references) {
			if (r.data) {
				scanRemaining(r.data->vars, p->str(r.script));
			}
		}
		// every element that stays and can hold a reference: running threads (owner and variables),
		// suspended calls and queued calls (their receiver and arguments)
		std::vector<Variable> one(1);
		for (const auto& a : p->activeScripts) {
			if (!plan.threads.contains(a.id) && a.data) {
				for (const auto& f : a.data->frames) {
					scanRemaining(f.vars, p->str(f.scriptName) + " (running)");
					one[0] = f.owner;
					scanRemaining(one, p->str(f.scriptName) + " (running)");
				}
			}
		}
		for (const auto* list : { &p->suspended1, &p->suspended2 }) {
			for (const auto& st : *list) {
				if (!plan.stacks.contains(st.id) && !plan.threads.contains(st.id) && st.data) {
					scanRemaining(st.data->vars, p->str(st.data->scriptName) + " (waiting call)");
					one[0] = st.data->unknownVar;
					scanRemaining(one, p->str(st.data->scriptName) + " (waiting call)");
				}
			}
		}
		for (std::size_t i = 0; i < p->functionMessages.size(); ++i) {
			const auto& m = p->functionMessages[i];
			if (!plan.messages.contains(i) && m.data) {
				scanRemaining(m.data->vars, p->str(m.data->scriptName) + " (queued call)");
				one[0] = m.data->unknownVar;
				scanRemaining(one, p->str(m.data->scriptName) + " (queued call)");
			}
		}
		// an element that stays but whose data could not be read might hold any of them: keep them all
		bool uncertain = false;
		for (const auto& a : p->activeScripts) {
			uncertain |= !plan.threads.contains(a.id) && !a.data;
		}
		for (const auto* list : { &p->suspended1, &p->suspended2 }) {
			for (const auto& st : *list) {
				uncertain |= !plan.stacks.contains(st.id) && !plan.threads.contains(st.id) && !st.data;
			}
		}
		if (uncertain && !candidates.empty()) {
			plan.notes.push_back("Some saved script data that stays could not be read, so no arrays are removed (they might still be in use).");
		}
		// arrays that stay: if they hold one of our arrays (not possible in Papyrus) or instances
		std::unordered_set<EID> removedArrays;
		for (auto id : candidates) {
			if (!uncertain && !keepArrays.contains(id)) {
				removedArrays.insert(id);
			}
		}
		for (const auto& a : p->arrays) {
			if (!removedArrays.contains(a.id)) {
				scanRemaining(a.vars, std::string("array of ") + VTypeName(a.type));
			}
		}
		for (auto id : removedArrays) {
			if (p->findArray(id)) {
				plan.arrays.insert(id);
			}
		}

		// ---- change forms ----------------------------------------------------------------------
		if (plan.isPlugin) {
			for (const auto& cf : a_save.changeForms) {
				if (cf.refID.type() == RefID::Type::kCreated || cf.refID.isZero()) {
					continue;
				}
				if (Lower(a_save.pluginOf(a_save.formIDOf(cf.refID))) == lowerName) {
					plan.changeForms.insert(cf.refID.data);
					++plan.changeFormsByType[ChangeFormTypeName(cf.typeCode())];
				}
			}
		}
		if (a_opt.spawnedObjects) {
			// a spawned object goes only when every script on it is being removed
			std::unordered_map<std::uint32_t, bool> allRemoved;
			for (const auto& i : p->instances) {
				if (i.refID.type() != RefID::Type::kCreated) {
					continue;
				}
				const bool gone = plan.instances.contains(i.id);
				auto [it, fresh] = allRemoved.emplace(i.refID.data, gone);
				if (!fresh) {
					it->second = it->second && gone;
				}
			}
			for (const auto& [ref, gone] : allRemoved) {
				if (gone) {
					plan.spawnedForms.insert(ref);
				}
			}
		}

		// ---- safety checks ---------------------------------------------------------------------
		if (plan.isPlugin) {
			if (haveSources) {
				for (const auto* list : { &a_save.plugins.full, &a_save.plugins.lite }) {
					for (const auto& pl : *list) {
						const auto low = Lower(pl);
						// every other plugin's masters are read (only the base game's own masters, which
						// never depend on mods, are skipped)
						if (low == lowerName || IsBaseGamePlugin(low)) {
							continue;
						}
						const auto path = a_src->pluginPath(pl);
						if (!path) {
							++plan.uncheckedPlugins;
							continue;
						}
						try {
							const auto masters = ReadMasters(*path);
							if (std::find(masters.begin(), masters.end(), lowerName) != masters.end()) {
								plan.dependents.push_back(pl);
							}
						} catch (const std::exception&) {
							++plan.uncheckedPlugins;
						}
					}
				}
				if (!plan.dependents.empty()) {
					std::string list;
					for (const auto& d : plan.dependents) {
						list += "\n    " + d;
					}
					plan.blockers.push_back(Fmt("%zu plugin(s) in this save need %s as a master; the game crashes at load without it:", plan.dependents.size(),
												a_mod.c_str()) +
											list);
				}
				if (plan.uncheckedPlugins) {
					plan.notes.push_back(Fmt("%zu plugin(s) in the save were not found in the scanned folders, so their masters were not checked.",
						plan.uncheckedPlugins));
				}
			} else {
				plan.warnings.push_back("Masters were not checked (scan your mods folder first). If another plugin needs " + a_mod +
										" as a master, the game crashes at load.");
			}
		}
		if (haveSources && !plan.folder.empty()) {
			std::error_code ec;
			const auto      dir = a_src->folderPath(plan.folder);
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (!e.is_directory(ec) || Lower(U8(e.path().filename())) != "skse") {
					continue;
				}
				for (const auto& s : std::filesystem::directory_iterator(e.path(), ec)) {
					if (!s.is_directory(ec) || Lower(U8(s.path().filename())) != "plugins") {
						continue;
					}
					for (const auto& d : std::filesystem::directory_iterator(s.path(), ec)) {
						if (d.is_regular_file(ec) && Lower(U8(d.path().extension())) == ".dll") {
							plan.dlls.push_back(U8(d.path().filename()));
						}
					}
				}
			}
			if (a_src->ranked) {
				plan.enabledInProfile = a_src->enabledFolders.contains(plan.folder) ? 1 : 0;
			}
		}
		if (plan.enabledInProfile == 1) {
			plan.warnings.push_back("\"" + plan.folder +
									"\" is still enabled in your MO2 profile. Disable it before loading the new save, or the mod starts again from scratch.");
		}
		if (!plan.dlls.empty()) {
			std::string list;
			for (const auto& d : plan.dlls) {
				list += (list.empty() ? "" : ", ") + d;
			}
			plan.warnings.push_back("The mod has an SKSE plugin (" + list +
									"). Whatever it stored in the .skse co-save cannot be removed by a save editor; disable the whole mod so the DLL is not loaded.");
		}
		if (!plan.threads.empty()) {
			plan.warnings.push_back(Fmt("%zu of its scripts are running right now and will be cut off mid-way.", plan.threads.size()));
		}
		if (plan.threadsOfOtherMods) {
			plan.warnings.push_back(Fmt("%zu thread(s) started by other mods are inside this mod right now; they are removed too, so those "
										"other mods' events end early.",
				plan.threadsOfOtherMods));
		}
		if (!plan.linksFromOthers.empty()) {
			std::vector<std::pair<std::string, std::size_t>> v(plan.linksFromOthers.begin(), plan.linksFromOthers.end());
			std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
			std::string list;
			for (std::size_t i = 0; i < v.size() && i < 8; ++i) {
				list += Fmt("\n    %s (%zu)", v[i].first.c_str(), v[i].second);
			}
			if (v.size() > 8) {
				list += Fmt("\n    ... and %zu more", v.size() - 8);
			}
			plan.warnings.push_back(Fmt("%zu variable(s) in other scripts point at its scripts and will read None:", [&] {
				std::size_t n = 0;
				for (auto& x : v) {
					n += x.second;
				}
				return n;
			}()) + list);
		}
		if (plan.instances.empty() && plan.threads.empty() && plan.changeForms.empty()) {
			plan.blockers.push_back("Nothing of " + a_mod + " was found in this save.");
		}
		plan.notes.push_back("The plugin list in the save is left as it is; the game drops a missing plugin's forms by itself when loading.");
		plan.notes.push_back("Items, spells, perks or effects it gave the player are dropped by the game when the plugin is gone; nothing can restore what they changed.");
		return plan;
	}

	std::string Plan::summary() const
	{
		std::string s;
		s += Fmt("Remove %s%s\n\n", name.c_str(), folder.empty() || folder == name ? "" : (" (mod folder: " + folder + ")").c_str());
		s += "Will delete:\n";
		s += Fmt("  - %zu script instance(s): %zu on its own objects, %zu of its scripts on other objects\n", instances.size(), instancesOnOwnForms,
			instancesElsewhere);
		s += Fmt("  - %zu running thread(s), %zu suspended stack(s), %zu queued call(s)\n", threads.size(), stacks.size(), messages.size());
		if (!arrays.empty() || !unbinds.empty()) {
			s += Fmt("  - %zu array(s) only it used, %zu queued unbind(s)\n", arrays.size(), unbinds.size());
		}
		if (!changeForms.empty()) {
			s += Fmt("  - saved state of %zu of its forms:", changeForms.size());
			for (const auto& [t, n] : changeFormsByType) {
				s += Fmt(" %s %zu", t.c_str(), n);
			}
			s += "\n";
		}
		if (!spawnedForms.empty()) {
			s += Fmt("  - %zu spawned object(s) that only carried its scripts\n", spawnedForms.size());
		}
		if (!ownScripts.empty()) {
			s += Fmt("  (%zu script(s) only this mod provides are removed wherever they are attached)\n", ownScripts.size());
		}
		if (!blockers.empty()) {
			s += "\nCANNOT REMOVE:\n";
			for (const auto& b : blockers) {
				s += "  * " + b + "\n";
			}
		}
		if (!warnings.empty()) {
			s += "\nRead before continuing:\n";
			for (const auto& w : warnings) {
				s += "  * " + w + "\n";
			}
		}
		if (!notes.empty()) {
			s += "\nNotes:\n";
			for (const auto& n : notes) {
				s += "  - " + n + "\n";
			}
		}
		return s;
	}

	Result Apply(SaveFile& a_save, const Plan& a_plan)
	{
		Result r;
		auto*  p = a_save.papyrus();
		if (!p || !a_plan.canApply()) {
			return r;
		}
		// message indices first: every later step leaves functionMessages alone
		r.messages = ops::DeleteFunctionMessages(*p, a_plan.messages);
		std::set<ThreadID> stacks = a_plan.stacks;
		stacks.insert(a_plan.threads.begin(), a_plan.threads.end());
		r.stacks = ops::DeleteSuspendedStacks(*p, stacks);
		r.threads = ops::DeleteThreads(*p, a_plan.threads);
		r.instances = ops::DeleteInstances(*p, a_plan.instances);
		r.arrays = ops::DeleteArrays(*p, a_plan.arrays);
		r.unbinds = ops::DeleteUnbinds(*p, a_plan.unbinds);
		std::set<std::uint32_t> forms = a_plan.changeForms;
		forms.insert(a_plan.spawnedForms.begin(), a_plan.spawnedForms.end());
		r.changeForms = ops::DeleteChangeForms(a_save, forms);
		return r;
	}

	std::string Result::text() const
	{
		return Fmt("%zu instances, %zu threads, %zu suspended stacks, %zu queued calls, %zu arrays, %zu unbinds, %zu change forms", instances, threads,
			stacks, messages, arrays, unbinds, changeForms);
	}
}
