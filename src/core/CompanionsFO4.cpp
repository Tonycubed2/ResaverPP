// ReSaver++ - Fallout 4 companions (see CompanionsFO4.h for where each part comes from and how it was checked).
#include "CompanionsFO4.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4.h"
#include "Ops.h"
#include "Papyrus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace ess::fo4comp
{
	namespace
	{
		std::string Fmt(const char* f, auto... args)
		{
			char buf[2048];
			std::snprintf(buf, sizeof(buf), f, args...);
			return buf;
		}
		std::string Hex(std::uint32_t a_id) { return Fmt("%08X", a_id); }

		// a script instance's variables by name (parents' members first, as the save stores them)
		struct Vars
		{
			const Papyrus*              p = nullptr;
			const ScriptInstance*       inst = nullptr;
			std::vector<std::string>    names;

			const Variable* get(const char* a_name) const
			{
				if (!inst || !inst->data) {
					return nullptr;
				}
				for (std::size_t i = 0; i < names.size() && i < inst->data->vars.size(); ++i) {
					if (EqualsNoCase(names[i], a_name)) {
						return &inst->data->vars[i];
					}
				}
				return nullptr;
			}
		};
		Vars VarsOf(const Papyrus& p, const ScriptInstance& i)
		{
			Vars v;
			v.p = &p;
			v.inst = &i;
			v.names = ops::ExtendedMembers(p, i.script);
			return v;
		}

		// the script and its parents, lower case names
		bool Extends(const Papyrus& p, TStr a_script, const char* a_base)
		{
			for (int guard = 0; guard < 64; ++guard) {
				if (EqualsNoCase(p.str(a_script), a_base)) {
					return true;
				}
				const auto* s = p.findScript(a_script);
				if (!s || s->parent == a_script || p.str(s->parent).empty()) {
					return false;
				}
				a_script = s->parent;
			}
			return false;
		}

		// what an object variable points at: the instance it holds (an engine object: a form's or an alias's link
		// to Papyrus)
		const ScriptInstance* Target(const Papyrus& p, const Variable* a_v)
		{
			if (!a_v || a_v->type != VType::kRef || a_v->value == 0) {
				return nullptr;
			}
			return p.findInstance(a_v->value);
		}
		std::uint32_t FormOf(const SaveFile& s, const Papyrus& p, const Variable* a_v)
		{
			const auto* t = Target(p, a_v);
			return t ? s.formIDOf(t->refID) : 0;
		}
		bool BoolOf(const Variable* a_v, bool& a_out)
		{
			if (!a_v || (a_v->type != VType::kBool && a_v->type != VType::kInt)) {
				return false;
			}
			a_out = a_v->value != 0;
			return true;
		}
		bool IntOf(const Variable* a_v, float& a_out)
		{
			if (!a_v) {
				return false;
			}
			if (a_v->type == VType::kInt) {
				a_out = static_cast<float>(static_cast<std::int32_t>(a_v->value));
				return true;
			}
			if (a_v->type == VType::kFloat) {
				a_out = a_v->asFloat();
				return true;
			}
			return false;
		}

		std::uint32_t Vsval(const std::uint8_t* d, std::size_t n, std::size_t& p)
		{
			if (p >= n) {
				throw std::runtime_error("end of data");
			}
			const auto sz = d[p] & 3;
			std::uint32_t v = 0;
			const std::size_t len = sz == 0 ? 1 : sz == 1 ? 2 : 4;
			if (sz == 3 || p + len > n) {
				throw std::runtime_error("bad count");
			}
			for (std::size_t k = 0; k < len; ++k) {
				v |= static_cast<std::uint32_t>(d[p + k]) << (8 * k);
			}
			p += len;
			return v >> 2;
		}
		RefID Ref3(const std::uint8_t* d, std::size_t n, std::size_t& p)
		{
			if (p + 3 > n) {
				throw std::runtime_error("end of data");
			}
			RefID r;
			r.data = (static_cast<std::uint32_t>(d[p]) << 16) | (d[p + 1] << 8) | d[p + 2];
			p += 3;
			return r;
		}

		// extra 0x88 / 0x5b of a reference (range: the type byte and its data)
		void ReadAliasExtra(const SaveFile& s, const std::vector<std::uint8_t>& b, const fo4cf::Range& r, Companion& c)
		{
			std::size_t p = r.begin + 1;
			const auto  n = Vsval(b.data(), r.end, p);
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto q = s.formIDOf(Ref3(b.data(), r.end, p));
				if (p + 4 > r.end) {
					throw std::runtime_error("end of data");
				}
				std::uint32_t a;
				std::memcpy(&a, b.data() + p, 4);
				p += 4;
				c.aliases.push_back({ q, a });
			}
		}
		void ReadFactionExtra(const SaveFile& s, const std::vector<std::uint8_t>& b, const fo4cf::Range& r, Companion& c)
		{
			std::size_t p = r.begin + 1;
			const auto  n = Vsval(b.data(), r.end, p);
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto f = s.formIDOf(Ref3(b.data(), r.end, p));
				if (p >= r.end) {
					throw std::runtime_error("end of data");
				}
				c.factions.push_back({ f, static_cast<std::int8_t>(b[p++]) });
			}
		}

		// the quest's alias fills: QUEST_RUN_DATA / ITEMS1 entries {alias id, created flag, ref} (flag 0 only)
		void ReadFills(const cf::Node& a_root, std::vector<std::pair<std::int32_t, std::uint32_t>>& a_out)
		{
			for (const auto& k : a_root.kids) {
				if (k.label != "QUEST_RUN_DATA") {
					continue;
				}
				for (const auto& g : k.kids) {
					if (g.label.rfind("ITEMS1", 0) != 0) {
						continue;
					}
					for (const auto& e : g.kids) {
						std::int32_t  alias = -1;
						bool          created = true;
						std::uint32_t ref = 0;
						for (const auto& f : e.kids) {
							if (f.label == "UNK") {
								alias = static_cast<std::int32_t>(std::strtol(f.value.c_str(), nullptr, 0));
							} else if (f.label == "FLAG") {
								created = std::strtoul(f.value.c_str(), nullptr, 0) != 0;
							} else if (f.label == "REF0") {
								ref = f.formID;
							}
						}
						if (!created && alias >= 0) {
							a_out.push_back({ alias, ref });
						}
					}
				}
			}
		}

		std::string FloatText(float v)
		{
			char b[32];
			std::snprintf(b, sizeof(b), "%g", v);
			return b;
		}
	}

	const Companion* Result::find(std::uint32_t a_ref) const
	{
		for (const auto& c : companions) {
			if (c.ref == a_ref) {
				return &c;
			}
		}
		return nullptr;
	}

	std::string SetAffinityCommand(const Companion& a_c, float a_value)
	{
		return Fmt("%08X.cf \"%s.SetAffinity\" %s", a_c.ref, a_c.script.c_str(), FloatText(a_value).c_str());
	}

	std::vector<std::string> MakeCompanionCommands(const Companion& a_c, const std::string& a_quest)
	{
		std::vector<std::string> out;
		if (!a_c.dogmeat && a_c.hasLeftPermanently) {
			out.push_back(Fmt("%08X.cf \"%s.UnsetHasLeftPlayerPermanently\"", a_c.ref, a_c.script.c_str()));
		}
		out.push_back(Fmt("cqf %s \"FollowersScript.AllowCompanion\" %08X 1 1", a_quest.c_str(), a_c.ref));
		return out;
	}

	std::string FollowCommand(const Companion& a_c, const std::string& a_quest)
	{
		return Fmt("cqf %s \"FollowersScript.FollowerFollow\" %08X", a_quest.c_str(), a_c.ref);
	}

	std::vector<std::string> CompanionRecord(const Companion& a_c)
	{
		if (a_c.dogmeat) {
			return { "companion", Hex(a_c.ref), "0" };
		}
		return { "companion", Hex(a_c.ref), a_c.hasLeftPermanently ? "1" : "0", a_c.script };
	}
	std::vector<std::string> FollowRecord(const Companion& a_c) { return { "companionfollow", Hex(a_c.ref) }; }
	std::vector<std::string> AffinityRecord(const Companion& a_c, int a_value) { return { "affinity", Hex(a_c.ref), std::to_string(a_value), a_c.script }; }

	Result Read(const SaveFile& a_save)
	{
		Result out;
		if (!a_save.isFO4()) {
			return out;
		}
		const auto* p = a_save.papyrus();
		if (!p) {
			out.notes.push_back("the save has no Papyrus section");
			return out;
		}
		auto& sys = out.system;

		// the scripts' own instances
		const ScriptInstance* followers = nullptr;
		const ScriptInstance* common = nullptr;
		for (const auto& i : p->instances) {
			const auto& n = p->str(i.script);
			if (!followers && EqualsNoCase(n, "FollowersScript") && i.data) {
				followers = &i;
			} else if (!common && EqualsNoCase(n, "CommonPropertiesScript") && i.data) {
				common = &i;
			}
		}
		std::int32_t aliasCommand = -1;
		(void)aliasCommand;
		if (followers) {
			sys.found = true;
			sys.quest = a_save.formIDOf(followers->refID);
			sys.questScript = p->str(followers->script);
			const auto v = VarsOf(*p, *followers);
			auto       aliasOf = [&](const char* a_name) -> std::int32_t {
                const auto* t = Target(*p, v.get(a_name));
                if (!t || a_save.formIDOf(t->refID) != sys.quest || t->unknown == 0xFFFF) {
                    return -1;
                }
                return t->unknown;
			};
			sys.aliasCompanion = aliasOf("::Companion_var");
			sys.aliasDogmeat = aliasOf("::DogmeatCompanion_var");
			sys.aliasActive = aliasOf("::ActiveCompanions_var");
			sys.factionCurrent = FormOf(a_save, *p, v.get("::CurrentCompanionFaction_var"));
			sys.factionHasBeen = FormOf(a_save, *p, v.get("::HasBeenCompanionFaction_var"));
			sys.factionDisallowed = FormOf(a_save, *p, v.get("::DisallowedCompanionFaction_var"));
			sys.avAffinity = FormOf(a_save, *p, v.get("::CA_Affinity_var"));
			sys.avThreshold = FormOf(a_save, *p, v.get("::CA_CurrentThreshold_var"));
			sys.avHighest = FormOf(a_save, *p, v.get("::CA_HighestReached_var"));
			sys.avLowest = FormOf(a_save, *p, v.get("::CA_LowestReached_var"));
			sys.avIsRomantic = FormOf(a_save, *p, v.get("::CA_IsRomantic_var"));
			sys.avRomanceableNow = FormOf(a_save, *p, v.get("::CA_IsRomanceableNow_var"));
		} else {
			out.notes.push_back("no FollowersScript instance in the save (the follower quest has not started, or a mod replaced it)");
		}
		if (common) {
			const auto v = VarsOf(*p, *common);
			sys.avFollowerState = FormOf(a_save, *p, v.get("::FollowerState_var"));
		}

		// the quest's alias fills
		if (sys.found) {
			for (const auto& c : a_save.changeForms) {
				if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::QUST || a_save.formIDOf(c.refID) != sys.quest) {
					continue;
				}
				const auto d = cf::Decode(a_save, c);
				sys.questDecoded = d.complete;
				if (!d.complete) {
					out.notes.push_back("the follower quest's saved data could not be read: " + d.error);
					break;
				}
				for (const auto& k : d.root.kids) {
					if (k.label == "QUEST_FLAGS") {
						sys.questRunning = (std::strtoul(k.value.c_str(), nullptr, 0) & 1) != 0;
					}
				}
				std::vector<std::pair<std::int32_t, std::uint32_t>> fills;
				ReadFills(d.root, fills);
				for (const auto& [a, r] : fills) {
					if (a == sys.aliasCompanion && sys.aliasCompanion >= 0) {
						sys.companionFill = r;
					} else if (a == sys.aliasDogmeat && sys.aliasDogmeat >= 0) {
						sys.dogmeatFill = r;
					} else if (a == sys.aliasActive && sys.aliasActive >= 0) {
						sys.active.push_back(r);
					}
				}
				break;
			}
			if (!sys.questDecoded && out.notes.empty()) {
				out.notes.push_back("the follower quest has no saved data");
			}
		}

		// the companions
		std::unordered_map<std::uint32_t, const ChangeForm*> achr;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR) {
				achr.emplace(a_save.formIDOf(c.refID), &c);
			}
		}
		for (const auto& i : p->instances) {
			const bool dog = EqualsNoCase(p->str(i.script), "DogmeatActorScript");
			if (!dog && !Extends(*p, i.script, "CompanionActorScript")) {
				continue;
			}
			if (i.unknown != 0xFFFF) {
				continue;  // (an alias's script, not an actor's)
			}
			Companion c;
			c.ref = a_save.formIDOf(i.refID);
			if (c.ref == 0 || out.find(c.ref)) {
				continue;
			}
			c.script = p->str(i.script);
			c.dogmeat = dog;
			c.instance = i.id;
			if (!dog && i.data) {
				const auto v = VarsOf(*p, i);
				bool       b = false;
				c.scriptRead = true;
				if (BoolOf(v.get("HasLeftPlayerPermanently"), b)) {
					c.hasLeftPermanently = b;
				}
				if (BoolOf(v.get("InfatuationPerkUnlocked"), b)) {
					c.perkUnlocked = b;
				}
				const auto* last = v.get("LastThresholdCrossed");
				const auto* inf = v.get("::InfatuationThreshold_var");
				c.infatuated = last && inf && last->type == VType::kRef && inf->type == VType::kRef && last->value != 0 && last->value == inf->value;
				float mn = 0, mx = 0;
				if (IntOf(v.get("::MinAffinity_var"), mn) && IntOf(v.get("::MaxAffinity_var"), mx) && mn < mx) {
					c.hasRange = true;
					c.minAffinity = mn;
					c.maxAffinity = mx;
				}
			}
			if (auto it = achr.find(c.ref); it != achr.end()) {
				c.hasChangeForm = true;
				try {
					const auto body = cf::Body(*it->second);
					const auto r = fo4cf::DecodeRefr(a_save, *it->second, body);
					c.refRead = r.decoded;
					if (r.decoded) {
						c.cell = r.hasInitial ? a_save.formIDOf(r.cell) : 0;
						std::copy(std::begin(r.pos), std::end(r.pos), c.pos);
						for (const auto& e : r.extras) {
							try {
								if (e.type == 0x88) {
									ReadAliasExtra(a_save, body, e.range, c);
								} else if (e.type == 0x5b) {
									ReadFactionExtra(a_save, body, e.range, c);
								}
							} catch (const std::exception&) {
								out.notes.push_back(Fmt("%08X: extra data 0x%02X could not be read", c.ref, e.type));
							}
						}
					}
				} catch (const std::exception& e) {
					out.notes.push_back(Fmt("%08X: %s", c.ref, e.what()));
				}
				const auto a = fo4cf::DecodeActor(a_save, *it->second);
				c.actorRead = a.decoded;
				for (const auto& av : a.values) {
					if (av.formID == 0) {
						continue;
					}
					if (av.formID == sys.avAffinity) {
						c.hasAffinity = true;
						c.affinity = av.base;
					} else if (av.formID == sys.avThreshold) {
						c.hasThreshold = true;
						c.threshold = av.base;
					} else if (av.formID == sys.avFollowerState) {
						c.hasFollowerState = true;
						c.followerState = av.base;
					} else if (av.formID == sys.avIsRomantic) {
						c.hasRomantic = true;
						c.romantic = av.base;
					} else if (av.formID == sys.avRomanceableNow) {
						c.hasRomanceable = true;
						c.romanceable = av.base;
					}
				}
			}
			for (const auto& f : c.factions) {
				if (f.rank >= 0 && f.faction == sys.factionDisallowed && f.faction) {
					c.disallowed = true;
				}
				if (f.rank >= 0 && f.faction == sys.factionHasBeen && f.faction) {
					c.hasBeen = true;
				}
			}
			const auto slot = dog ? sys.aliasDogmeat : sys.aliasCompanion;
			const auto fill = dog ? sys.dogmeatFill : sys.companionFill;
			c.inCompanionAlias = fill != 0 && fill == c.ref;
			c.inActive = std::find(sys.active.begin(), sys.active.end(), c.ref) != sys.active.end();
			for (const auto& [q, a] : c.aliases) {
				if (q == sys.quest && slot >= 0 && static_cast<std::int32_t>(a) == slot) {
					c.aliasExtraSaysCompanion = true;
				}
			}
			out.companions.push_back(std::move(c));
		}

		// ---- what does not add up --------------------------------------------------------------------------
		using L = Finding::Level;
		const std::string quest = sys.quest ? Hex(sys.quest) : std::string(kFollowersQuest);
		auto              add = [&](L a_level, std::uint32_t a_ref, std::string a_title, std::string a_what, std::vector<std::string> a_cmds = {},
                       std::string a_why = {}, std::vector<std::string> a_record = {}) {
            Finding f;
            f.level = a_level;
            f.ref = a_ref;
            f.title = std::move(a_title);
            f.what = std::move(a_what);
            f.console = std::move(a_cmds);
            f.consoleWhy = std::move(a_why);
            f.record = std::move(a_record);
            out.findings.push_back(std::move(f));
		};
		const char* makeWhy = "Runs the game's own scripts in the running game (open the console with ~, type each line, Enter): "
		                      "FollowersScript.AllowCompanion takes it out of DisallowedCompanionFaction and makes it your companion "
		                      "(SetCompanion: the Companion alias, follow, teammate, events) - your current companion is dismissed first.";
		if (sys.found && sys.questDecoded) {
			// the quest's companion slots hold someone the scripts do not know as a companion
			for (int dog = 0; dog < 2; ++dog) {
				const auto fill = dog ? sys.dogmeatFill : sys.companionFill;
				if (fill && !out.find(fill)) {
					add(L::kProblem, fill,
						Fmt("The %s slot holds %08X, which has no companion script", dog ? "Dogmeat companion" : "companion", fill),
						Fmt("The follower quest's %s alias is filled with %08X, but no actor in the save has %s for it. The follower "
							"scripts call that script on whoever is in the slot, so commands, affinity and dismissing do not work. A mod "
							"that forced its NPC into the alias, or a removed companion mod, leaves this.",
							dog ? "DogmeatCompanion" : "Companion", fill, dog ? "DogmeatActorScript" : "CompanionActorScript"),
						{ Fmt("cqf %s \"FollowersScript.%s\"", quest.c_str(), dog ? "ClearDogmeatCompanion" : "ClearCompanion") },
						"FollowersScript.ClearCompanion / ClearDogmeatCompanion empty the slot the game's way (teammate off, events, "
						"the PlayerHasActiveCompanion global), in the running game.");
				}
			}
		}
		for (const auto& c : out.companions) {
			if (sys.found && sys.questDecoded && c.refRead) {
				if (c.aliasExtraSaysCompanion && !c.inCompanionAlias) {
					const auto fill = c.dogmeat ? sys.dogmeatFill : sys.companionFill;
					add(L::kProblem, c.ref, Fmt("%08X thinks it is your companion, but the follower quest has %s", c.ref,
												fill ? Hex(fill).c_str() : "nobody"),
						Fmt("Its own saved data lists the follower quest's %s alias, while the quest's saved data has %s in that "
							"alias. The two halves of the companion link disagree (a mod or a console command that moved it in or out "
							"halfway). Put it back consistently in the game:",
							c.dogmeat ? "DogmeatCompanion" : "Companion", fill ? ("reference " + Hex(fill)).c_str() : "no one"),
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				} else if (c.inCompanionAlias && !c.aliasExtraSaysCompanion) {
					add(L::kProblem, c.ref, Fmt("Companion %08X is in the follower quest's slot, but its own data does not say so", c.ref),
						Fmt("The follower quest has %08X in its %s alias, but the actor's own alias list does not name it, so the "
							"alias's packages and faction may not apply to it (it does not follow). Make it your companion again in "
							"the game:",
							c.ref, c.dogmeat ? "DogmeatCompanion" : "Companion"),
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				}
			}
			if (c.current()) {
				if (c.hasLeftPermanently) {
					add(L::kCaution, c.ref, Fmt("Your companion %08X has left you for good, as far as its script knows", c.ref),
						"It is in the companion slot, but its CompanionActorScript has HasLeftPlayerPermanently set: affinity no "
						"longer changes and its scenes treat it as gone. Clear the flag and make it your companion again:",
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				}
				if (c.disallowed) {
					add(L::kCaution, c.ref, Fmt("Your companion %08X is in DisallowedCompanionFaction", c.ref),
						"It is in the companion slot, but it is also in the faction the scripts use for \"cannot be a companion now\" "
						"(a story moment, or a mod). Dialogue to recruit or dismiss it may be missing.",
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				}
				if (c.hasFollowerState && c.followerState != kFollow) {
					const bool home = c.followerState == kGoHome;
					add(L::kInfo, c.ref, Fmt("Your companion %08X is %s, not following", c.ref, home ? "sent home" : "told to wait"),
						Fmt("Its FollowerState is %g (%s). That is a normal command, not damage: talk to it and say Follow, or in the "
							"running game:",
							c.followerState, home ? "4 = go home" : c.followerState == kWait ? "2 = wait" : "not follow 1, wait 2 or go home 4"),
						{ FollowCommand(c, quest) },
						"FollowersScript.FollowerFollow sets FollowerState to iFollower_Com_Follow (1), as the Follow command does.", FollowRecord(c));
				}
				if (sys.aliasActive >= 0 && sys.questDecoded && !c.dogmeat && !c.inActive) {
					add(L::kCaution, c.ref, Fmt("Your companion %08X is missing from ActiveCompanions", c.ref),
						"FollowersScript.SetCompanion adds every new companion to the ActiveCompanions collection; this one is in "
						"the companion slot but not in it, so it was put there some other way. Scripts that go through "
						"ActiveCompanions (teleporting, sleeping near you) skip it.",
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				}
			} else {
				if (c.hasLeftPermanently) {
					add(L::kInfo, c.ref, Fmt("%08X has left you for good", c.ref),
						"Its CompanionActorScript has HasLeftPlayerPermanently set (it left because its affinity fell, or a quest "
						"ended it). To have it back anyway:",
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				} else if (c.disallowed) {
					add(L::kInfo, c.ref, Fmt("%08X cannot be your companion right now", c.ref),
						"It is in DisallowedCompanionFaction: the story (or a mod) blocks it for now. Usually a quest takes it out "
						"again; only force it if that quest is done or broken:",
						MakeCompanionCommands(c, quest), makeWhy, CompanionRecord(c));
				}
			}
			if (c.hasAffinity && c.hasRange && (c.affinity > c.maxAffinity || c.affinity < c.minAffinity)) {
				add(L::kCaution, c.ref, Fmt("%08X's affinity %g is outside its script's range %g..%g", c.ref, c.affinity, c.minAffinity, c.maxAffinity),
					"The companion scripts keep affinity between MinAffinity and MaxAffinity; a value outside was set by a mod or "
					"the console. ReSaver++ can put it back at the nearest limit in the save, or in the game:",
					{ SetAffinityCommand(c, std::clamp(c.affinity, c.minAffinity, c.maxAffinity)) },
					"CompanionActorScript.SetAffinity changes affinity the scripts' way (ModAffinity), in the running game.",
					AffinityRecord(c, static_cast<int>(std::clamp(c.affinity, c.minAffinity, c.maxAffinity))));
			}
		}
		// two actors in the same slot
		for (int dog = 0; dog < 2; ++dog) {
			std::vector<std::uint32_t> claim;
			for (const auto& c : out.companions) {
				if (c.dogmeat == (dog == 1) && c.aliasExtraSaysCompanion) {
					claim.push_back(c.ref);
				}
			}
			if (claim.size() > 1) {
				std::string list;
				for (auto r : claim) {
					list += (list.empty() ? "" : ", ") + Hex(r);
				}
				add(L::kProblem, 0, Fmt("%zu actors are listed in the %s slot", claim.size(), dog ? "Dogmeat companion" : "companion"),
					"Their own saved data names the same follower alias (" + list +
						"); the alias holds only one. See the separate findings for each.");
			}
		}
		return out;
	}

	std::string SetAffinity(SaveFile& a_save, std::uint32_t a_ref, float a_value)
	{
		const auto r = Read(a_save);
		const auto* c = r.find(a_ref);
		if (!c) {
			return Fmt("%08X is not a companion in this save", a_ref);
		}
		if (c->dogmeat) {
			return "Dogmeat-type companions have no affinity";
		}
		if (!r.system.avAffinity) {
			return "the follower scripts' CA_Affinity actor value was not found in the save";
		}
		if (!c->hasChangeForm || !c->actorRead) {
			return "the companion's actor values cannot be read";
		}
		if (!c->hasAffinity) {
			return "the save has no affinity value for this companion yet (the plugin's value applies until its scripts first set "
			       "one): set it in the game with " +
			       SetAffinityCommand(*c, a_value);
		}
		if (c->hasRange && (a_value < c->minAffinity || a_value > c->maxAffinity)) {
			return Fmt("the companion's script keeps affinity between %g and %g", c->minAffinity, c->maxAffinity);
		}
		ChangeForm* cf = nullptr;
		for (auto& x : a_save.changeForms) {
			if (static_cast<ChangeFormType>(x.typeCode()) == ChangeFormType::ACHR && a_save.formIDOf(x.refID) == a_ref) {
				cf = &x;
				break;
			}
		}
		if (!cf) {
			return "the companion's change form was not found";
		}
		ChangeForm work = *cf;
		if (auto e = fo4cf::SetActorValueBase(a_save, work, r.system.avAffinity, a_value); !e.empty()) {
			return e;
		}
		// CompanionActorScript.ModAffinity: CA_HighestReached / CA_LowestReached follow the new value past them
		const auto a = fo4cf::DecodeActor(a_save, work);
		for (const auto& v : a.values) {
			if (v.formID && v.formID == r.system.avHighest && v.base < a_value) {
				if (auto e = fo4cf::SetActorValueBase(a_save, work, v.formID, a_value); !e.empty()) {
					return e;
				}
			} else if (v.formID && v.formID == r.system.avLowest && v.base > a_value) {
				if (auto e = fo4cf::SetActorValueBase(a_save, work, v.formID, a_value); !e.empty()) {
					return e;
				}
			}
		}
		const auto again = fo4cf::DecodeActor(a_save, work);
		if (!again.decoded || again.complete != a.complete) {
			return "the edited actor does not read back";
		}
		*cf = std::move(work);
		return {};
	}
}
