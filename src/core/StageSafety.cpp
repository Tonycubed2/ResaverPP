// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "StageSafety.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ess::stagesafety
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

		std::string Join(const std::vector<std::string>& a_v, const char* a_sep)
		{
			std::string out;
			for (const auto& s : a_v) {
				out += (out.empty() ? "" : a_sep) + s;
			}
			return out;
		}

		// script classes of the game (and SKSE) whose functions are the game's own: not walked into. Every other class whose
		// .pex is found (the quest's own scripts, a mod's library scripts) is walked into.
		const std::unordered_set<std::string>& BaseClasses()
		{
			static const std::unordered_set<std::string> s{ "action", "activator", "activemagiceffect", "actor", "actorbase", "actorvalueinfo", "alias",
				"ammo", "apparatus", "armor", "armoraddon", "art", "associationtype", "book", "camera", "cell", "class", "colorform", "combatstyle",
				"constructibleobject", "debug", "defaultobjectmanager", "door", "effectshader", "enchantment", "encounterzone", "equipslot", "explosion",
				"faction", "flora", "form", "formlist", "formtype", "furniture", "game", "globalvariable", "hazard", "headpart", "idle",
				"imagespacemodifier", "impactdataset", "ingredient", "input", "key", "keyword", "leveledactor", "leveleditem", "leveledspell", "light",
				"location", "locationalias", "locationreftype", "magiceffect", "math", "message", "miscobject", "modevent", "musictype", "netimmerse",
				"objectreference", "outfit", "package", "perk", "potion", "projectile", "quest", "race", "referencealias", "scene", "scroll",
				"shaderparticlegeometry", "shout", "skse", "soulgem", "sound", "soundcategory", "sounddescriptor", "spell", "static",
				"stringutil", "textureset", "topic", "topicinfo", "tree", "ui", "uicallback", "utility", "visualeffect", "voicetype", "weapon", "weather",
				"wordofpower", "worldspace", "wornobject", "spawnertask" };
			return s;
		}

		struct V
		{
			enum class K
			{
				kNone,
				kPlayer,
				kSelf,  // self of the script being walked
				kForm,  // a form from the quest's VMAD (property value)
			} k = K::kNone;
			std::uint32_t form = 0;
			std::uint32_t quest = 0;  // kSelf: the quest that is self (0: self is not a quest)
			int           lit = -1;   // a bool / int literal copied into it: 0 or 1 (-1: not known)
			bool          hasNum = false;  // an int literal copied into it (num)
			int           num = 0;
			std::string   text;
		};

		const pex::Function* FindIn(const pex::Object& a_o, const pex::File& a_f, const std::string& a_name)
		{
			const auto want = Lower(a_name);
			// the empty state first (where fragments and most functions live), then the others
			for (int pass = 0; pass < 2; ++pass) {
				for (const auto& st : a_o.states) {
					if ((a_f.str(st.name).empty()) != (pass == 0)) {
						continue;
					}
					for (const auto& nf : st.functions) {
						if (Lower(a_f.str(nf.name)) == want) {
							return &nf.fn;
						}
					}
				}
			}
			return nullptr;
		}

		class Walker
		{
		public:
			Walker(const Env& a_env, Report& a_rep) :
				_env(a_env), _rep(a_rep) {}

			void startup(std::uint32_t a_quest, const std::vector<std::string>& a_path, int a_depth)
			{
				if (const auto* d = _env.quest(a_quest)) {
					for (const auto& st : d->stages) {
						if (st.flags & 0x02) {
							stage(a_quest, st.index, a_path, a_depth, " (start-up stage)");
						}
					}
				}
			}

			void shutdown(std::uint32_t a_quest, const std::vector<std::string>& a_path, int a_depth)
			{
				if (const auto* d = _env.quest(a_quest)) {
					for (const auto& st : d->stages) {
						if (st.flags & 0x04) {
							stage(a_quest, st.index, a_path, a_depth, " (shut-down stage)");
						}
					}
				}
			}

			void stage(std::uint32_t a_quest, int a_stage, std::vector<std::string> a_path, int a_depth, const char* a_note = "")
			{
				if (!_stages.insert({ a_quest, a_stage }).second) {
					return;
				}
				const auto* def = _env.quest(a_quest);
				if (!def) {
					notChecked(a_depth == 0 ? "the quest is not in the plugins" : questName(a_quest) + " is not in the plugins");
					return;
				}
				a_path.push_back((def->edid.empty() ? questName(a_quest) : def->edid) + " stage " + std::to_string(a_stage) + a_note);
				for (const auto& fr : def->fragments) {
					if (fr.stage != a_stage) {
						continue;
					}
					const auto  scriptName = !fr.script.empty() ? fr.script : def->fragmentScript;
					const pex::File* file = nullptr;
					try {
						file = _env.script(scriptName);
					} catch (const std::exception& e) {
						notChecked("its stage script " + scriptName + " could not be read (" + e.what() + ")");
						continue;
					}
					if (!file) {
						notChecked("its stage script " + scriptName + ".pex was not found");
						continue;
					}
					if (file->objects.empty()) {
						notChecked(scriptName + " has no script object");
						continue;
					}
					const auto& obj = file->objects.front();
					const auto* fn = FindIn(obj, *file, fr.function);
					if (!fn) {
						// the record names a fragment the script does not have (vanilla DA11IntroScene stage 100): the game cannot run
						// it either (a Papyrus error, nothing else)
						note(_rep.notRead, scriptName + "." + fr.function + " (named by the quest, not in its script: nothing runs)");
						continue;
					}
					V self;
					self.k = V::K::kSelf;
					self.quest = a_quest;
					self.text = "";
					walk(*file, obj, *fn, scriptName, fr.function, self, &def->properties, a_path, 0, a_depth);
				}
			}

		private:
			static constexpr int         kMaxQuestDepth = 3;  // quests started by quests started by ... the transplanted one
			static constexpr int         kMaxCallDepth = 4;
			static constexpr std::size_t kBudget = 200000;  // instructions in all

			const Env&                                 _env;
			Report&                                    _rep;
			std::set<std::pair<std::uint32_t, int>>    _stages;
			std::set<std::string>                      _functions;
			std::map<std::string, bool>                _isA;
			std::size_t                                _steps = 0;

			void notChecked(const std::string& a_why)
			{
				if (_rep.notChecked.empty()) {
					_rep.notChecked = a_why;
				}
			}

			std::string questName(std::uint32_t a_quest) const
			{
				if (const auto* d = _env.quest(a_quest); d && !d->edid.empty()) {
					return d->edid;
				}
				char buf[16];
				std::snprintf(buf, sizeof(buf), "%08X", a_quest);
				return buf;
			}

			const pex::File* load(const std::string& a_script)
			{
				try {
					return _env.script(a_script);
				} catch (...) {
					return nullptr;
				}
			}

			// a_type is a_base or extends it (through the scripts' parents)
			bool isA(const std::string& a_type, const char* a_base)
			{
				auto t = Lower(a_type);
				const std::string base = a_base;
				const auto        key = t + "|" + base;
				if (auto it = _isA.find(key); it != _isA.end()) {
					return it->second;
				}
				bool yes = false;
				for (int i = 0; i < 12 && !t.empty(); ++i) {
					if (t == base) {
						yes = true;
						break;
					}
					if (BaseClasses().contains(t)) {
						break;  // another game class: not a_base
					}
					const auto* f = load(t);
					if (!f || f->objects.empty()) {
						break;
					}
					t = Lower(f->str(f->objects.front().parent));
				}
				_isA[key] = yes;
				return yes;
			}

			void find(Kind a_kind, const std::string& a_call, const std::vector<std::string>& a_path, int a_questDepth)
			{
				const auto where = Join(a_path, " > ");
				for (const auto& f : _rep.findings) {
					if (f.kind == a_kind && f.call == a_call && f.where == where) {
						return;
					}
				}
				// a scene / fight / new actors of a quest it starts: that quest starting normally (see Finding::followUp)
				const bool world = a_kind == Kind::kScene || a_kind == Kind::kCombat || a_kind == Kind::kSpawn;
				_rep.findings.push_back({ a_kind, a_call, where, world && a_questDepth > 0 });
			}

			void note(std::vector<std::string>& a_list, const std::string& a_text)
			{
				if (std::find(a_list.begin(), a_list.end(), a_text) == a_list.end()) {
					a_list.push_back(a_text);
				}
			}

			// the quest a value is (0: not a known quest)
			std::uint32_t questOf(const V& a_v) const
			{
				if (a_v.k == V::K::kSelf) {
					return a_v.quest;
				}
				if (a_v.k == V::K::kForm && _env.quest(a_v.form)) {
					return a_v.form;
				}
				return 0;
			}

			void walk(const pex::File& a_file, const pex::Object& a_obj, const pex::Function& a_fn, const std::string& a_script, const std::string& a_name,
				const V& a_self, const std::vector<quests::Property>* a_props, const std::vector<std::string>& a_path, int a_callDepth, int a_questDepth)
			{
				{
					const auto key = Lower(a_script) + "|" + Lower(a_name) + "|" + std::to_string(static_cast<int>(a_self.k)) + "|" +
					                 std::to_string(a_self.form) + "|" + std::to_string(a_self.quest);
					if (!_functions.insert(key).second) {
						return;
					}
				}
				++_rep.functions;
				const auto& F = a_file;
				// declared types (lower case) and what each variable holds
				std::unordered_map<std::string, std::string> types;
				std::unordered_map<std::string, V>           vals;
				for (const auto& [n, t] : a_fn.params) {
					types[Lower(F.str(n))] = Lower(F.str(t));
				}
				for (const auto& [n, t] : a_fn.locals) {
					types[Lower(F.str(n))] = Lower(F.str(t));
				}
				for (const auto& var : a_obj.variables) {
					types[Lower(F.str(var.name))] = Lower(F.str(var.type));
				}
				types["self"] = Lower(F.str(a_obj.name));
				auto clean = [](std::string s) {
					if (s.rfind("::", 0) == 0) {
						s = s.substr(2);
					}
					if (s.size() > 4 && Lower(s.substr(s.size() - 4)) == "_var") {
						s.resize(s.size() - 4);
					}
					return s;
				};
				// properties: their auto variables hold the value the quest's VMAD gives them
				auto propValue = [&](const std::string& a_prop) {
					V v;
					v.text = a_prop;
					if (a_props) {
						for (const auto& p : *a_props) {
							if (Lower(p.name) == Lower(a_prop) && p.type == 1 && p.alias < 0 && p.form) {
								v.k = p.form == 0x14 ? V::K::kPlayer : V::K::kForm;
								v.form = p.form;
							}
						}
					}
					return v;
				};
				std::unordered_map<std::string, std::string> autoOf;  // auto variable -> property
				for (const auto& p : a_obj.properties) {
					if (p.flags & 4) {
						autoOf[Lower(F.str(p.autoVar))] = F.str(p.name);
					}
				}
				for (const auto& var : a_obj.variables) {
					const auto n = Lower(F.str(var.name));
					if (auto it = autoOf.find(n); it != autoOf.end()) {
						vals[n] = propValue(it->second);
					}
				}
				auto val = [&](const pex::Value& a_v) -> V {
					if (a_v.type != 1) {
						V v;
						switch (a_v.type) {
						case 2: v.text = "\"" + F.str(a_v.s) + "\""; break;
						case 3:
							v.text = std::to_string(a_v.i);
							v.lit = a_v.i != 0 ? 1 : 0;
							v.hasNum = true;
							v.num = a_v.i;
							break;
						case 4:
							{
								char buf[32];
								std::snprintf(buf, sizeof(buf), "%g", a_v.f);
								v.text = buf;
								v.lit = a_v.f != 0 ? 1 : 0;
							}
							break;
						case 5:
							v.text = a_v.b ? "True" : "False";
							v.lit = a_v.b ? 1 : 0;
							break;
						default: v.text = "None"; break;
						}
						return v;
					}
					const auto n = Lower(F.str(a_v.s));
					if (n == "self") {
						return a_self;
					}
					if (auto it = vals.find(n); it != vals.end()) {
						return it->second;
					}
					V v;
					v.text = clean(F.str(a_v.s));
					return v;
				};
				auto typeOf = [&](const pex::Value& a_v) -> std::string {
					if (a_v.type != 1) {
						return {};
					}
					auto it = types.find(Lower(F.str(a_v.s)));
					return it == types.end() ? std::string() : it->second;
				};
				auto setDest = [&](const pex::Value& a_dest, V a_v) {
					if (a_dest.type == 1) {
						const auto n = Lower(F.str(a_dest.s));
						if (n != "::nonevar") {
							vals[n] = std::move(a_v);
						}
					}
				};
				auto argsText = [&](const pex::Instruction& in, std::size_t a_first, std::size_t a_max = 4) {
					std::vector<std::string> a;
					for (std::size_t i = a_first; i < in.args.size() && a.size() < a_max; ++i) {
						a.push_back(val(in.args[i]).text);
					}
					return Join(a, ", ");
				};
				auto onOrUnknown = [&](const pex::Instruction& in, std::size_t a_first) {  // first argument true (or not a known literal)
					return a_first >= in.args.size() || val(in.args[a_first]).lit != 0;
				};
				auto anyOnOrUnknown = [&](const pex::Instruction& in, std::size_t a_first) {
					for (std::size_t i = a_first; i < in.args.size(); ++i) {
						if (val(in.args[i]).lit != 0) {
							return true;
						}
					}
					return false;
				};
				auto dot = [](const std::string& a_recv, const std::string& a_call) { return a_recv.empty() ? a_call : a_recv + "." + a_call; };

				// a call into a function of a script that is not one of the game's own classes
				auto follow = [&](const std::string& a_type, const std::string& a_fname, const V& a_newSelf) {
					if (a_callDepth >= kMaxCallDepth) {
						return;
					}
					auto t = Lower(a_type);
					for (int i = 0; i < 12 && !t.empty(); ++i) {
						if (BaseClasses().contains(t)) {
							return;
						}
						const pex::File* f = nullptr;
						try {
							f = _env.script(t);
						} catch (const std::exception&) {
							f = nullptr;
						}
						if (!f || f->objects.empty()) {
							note(_rep.notRead, t);
							return;
						}
						const auto& o = f->objects.front();
						if (const auto* fn = FindIn(o, *f, a_fname)) {
							if (fn->flags & 2) {
								return;  // native
							}
							// the quest's fragment script calling its own functions keeps its properties
							const bool same = Lower(t) == Lower(a_script);
							auto       path = a_path;
							if (!same) {
								path.push_back(f->str(o.name) + "." + a_fname + "()");
							}
							walk(*f, o, *fn, f->str(o.name), a_fname, a_newSelf, same ? a_props : nullptr, path, a_callDepth + 1, a_questDepth);
							return;
						}
						t = Lower(f->str(o.parent));
					}
				};

				for (const auto& in : a_fn.code) {
					if (++_steps > kBudget) {
						notChecked("too much script to read");
						return;
					}
					switch (in.op) {
					case 13:  // assign
					case 14:  // cast
						if (in.args.size() >= 2) {
							setDest(in.args[0], val(in.args[1]));
						}
						break;
					case 28:  // propget name obj dest
						if (in.args.size() >= 3 && in.args[0].type == 1) {
							const auto prop = F.str(in.args[0].s);
							const auto obj = val(in.args[1]);
							V          v;
							if (in.args[1].type == 1 && Lower(F.str(in.args[1].s)) == "self") {
								v = propValue(prop);
							} else {
								v.text = dot(obj.text, prop);
							}
							setDest(in.args[2], v);
						}
						break;
					case 25:  // callstatic class name dest count args...
						if (in.args.size() >= 4 && in.args[0].type == 1 && in.args[1].type == 1) {
							const auto cls = Lower(F.str(in.args[0].s));
							const auto fname = F.str(in.args[1].s);
							const auto m = Lower(fname);
							const auto call = F.str(in.args[0].s) + "." + fname + "(" + argsText(in, 4) + ")";
							V          dest;
							dest.text = call;
							if (cls == "game") {
								if (m == "getplayer") {
									dest.k = V::K::kPlayer;
									dest.text = "Game.GetPlayer()";
								} else if (m == "disableplayercontrols") {
									find(Kind::kControls, call, a_path, a_questDepth);
								} else if (m == "setplayeraidriven" && onOrUnknown(in, 4)) {
									find(Kind::kControls, call, a_path, a_questDepth);
								} else if (m == "fadeoutgame" && onOrUnknown(in, 4)) {
									find(Kind::kFade, call, a_path, a_questDepth);
								} else if (m == "enablefasttravel" && in.args.size() > 4 && val(in.args[4]).lit != 1) {
									find(Kind::kFastTravel, call, a_path, a_questDepth);
								} else if (m == "setinchargen" && anyOnOrUnknown(in, 4)) {
									find(Kind::kSaving, call, a_path, a_questDepth);
								} else if (m == "showracemenu" || m == "showlimitedracemenu") {
									find(Kind::kMenu, call, a_path, a_questDepth);
								} else if (m == "playbink") {
									find(Kind::kVideo, call, a_path, a_questDepth);
								} else if (m == "addperkpoints" || m == "advanceskill" || m == "incrementskill" || m == "incrementskillby" || m == "teachword" ||
										   m == "unlockword") {
									note(_rep.rewards, fname + "(" + argsText(in, 4, 2) + ")");
								}
							} else {
								follow(cls, fname, V{});
							}
							setDest(in.args[2], dest);
						}
						break;
					case 24:  // callparent name dest count args...
						if (in.args.size() >= 3 && in.args[0].type == 1) {
							follow(F.str(a_obj.parent), F.str(in.args[0].s), a_self);
							V dest;
							dest.text = "Parent." + F.str(in.args[0].s) + "()";
							setDest(in.args[1], dest);
						}
						break;
					case 23:  // callmethod name receiver dest count args...
						if (in.args.size() >= 4 && in.args[0].type == 1) {
							const auto fname = F.str(in.args[0].s);
							const auto m = Lower(fname);
							const auto recv = val(in.args[1]);
							const auto type = typeOf(in.args[1]);
							const auto call = dot(recv.text, fname) + "(" + argsText(in, 4) + ")";
							V          dest;
							dest.text = dot(recv.text, fname) + "()";
							bool handled = false;
							if (recv.k == V::K::kPlayer) {
								if (m == "moveto" || m == "movetonode" || m == "movetointeractionlocation" || m == "movetomyeditorlocation" ||
									m == "movetopackagelocation" || m == "setposition" || m == "translateto" || m == "translatetoref" || m == "splinetranslateto" ||
									m == "splinetranslatetoref" || m == "splinetranslatetorefnode") {
									find(Kind::kMovesPlayer, call, a_path, a_questDepth);
									handled = true;
								} else if (m == "kill" || m == "killessential" || m == "killsilent" ||
										   ((m == "setrestrained" || m == "setdontmove" || m == "setunconscious") && onOrUnknown(in, 4))) {
									find(Kind::kHoldsPlayer, call, a_path, a_questDepth);
									handled = true;
								} else if (m == "additem" || m == "addperk" || m == "addspell" || m == "addshout" || m == "modactorvalue" || m == "modav" ||
										   m == "setactorvalue" || m == "setav" || m == "addtofaction") {
									note(_rep.rewards, fname + "(" + argsText(in, 4, 2) + ")");
									handled = true;
								}
							}
							if (!handled && (m == "start" || m == "forcestart") && isA(type, "scene")) {
								find(Kind::kScene, call, a_path, a_questDepth);
								handled = true;
							}
							if (!handled && m == "startcombat") {
								find(Kind::kCombat, call, a_path, a_questDepth);
								handled = true;
							}
							if (!handled && (m == "placeactoratme" || (m == "placeatme" && in.args.size() > 4 &&
																		   (isA(typeOf(in.args[4]), "actorbase") || isA(typeOf(in.args[4]), "leveledactor"))))) {
								find(Kind::kSpawn, call, a_path, a_questDepth);
								handled = true;
							}
							// quests: its own stages, other quests started or moved on
							const auto qid = questOf(recv);
							const bool quest = qid != 0 || isA(type, "quest");
							if (!handled && quest &&
								(m == "setstage" || m == "setcurrentstageid" || m == "start" || m == "stop" || m == "reset" || m == "completequest")) {
								handled = true;
								int n = -1;  // the stage (a literal, or a variable a literal was copied into)
								if (in.args.size() > 4) {
									if (const auto a = val(in.args[4]); a.hasNum) {
										n = a.num;
									}
								}
								const bool setStage = m == "setstage" || m == "setcurrentstageid";
								const bool self = recv.k == V::K::kSelf && qid != 0;
								if (self) {
									if (setStage && n >= 0) {
										auto path = a_path;
										path.push_back(fname + "(" + std::to_string(n) + ")");
										stage(qid, n, path, a_questDepth);
									} else if (m == "stop") {
										shutdown(qid, a_path, a_questDepth);
									}
								} else if (qid != 0) {
									const auto name = questName(qid);
									if (setStage || m == "start") {
										note(_rep.followUps, name + "." + fname + "(" + (setStage ? (n >= 0 ? std::to_string(n) : std::string("?")) : std::string()) + ")");
										if (a_questDepth + 1 > kMaxQuestDepth) {
											break;
										}
										auto path = a_path;
										path.push_back(name + "." + fname + "(" + (setStage && n >= 0 ? std::to_string(n) : std::string()) + ")");
										startup(qid, path, a_questDepth + 1);
										if (setStage && n >= 0) {
											stage(qid, n, path, a_questDepth + 1);
										}
									} else if (m == "stop" && a_questDepth + 1 <= kMaxQuestDepth) {
										auto path = a_path;
										path.push_back(name + ".Stop()");
										shutdown(qid, path, a_questDepth + 1);
									}
								} else if (setStage || m == "start") {
									note(_rep.followUps, call + " (quest not identified)");
								}
							}
							if (!handled) {
								// a function of a script of its own (the quest's script, a mod's library)
								if (!type.empty()) {
									follow(type, fname, recv);
								}
							}
							setDest(in.args[2], dest);
						}
						break;
					default: break;
					}
				}
			}
		};
	}

	const char* KindText(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kMovesPlayer: return "moves you";
		case Kind::kScene: return "plays a scene";
		case Kind::kControls: return "switches your controls off";
		case Kind::kFade: return "fades the screen out";
		case Kind::kCombat: return "starts a fight";
		case Kind::kSpawn: return "places new people or creatures";
		case Kind::kFastTravel: return "switches fast travel off";
		case Kind::kSaving: return "switches saving off";
		case Kind::kMenu: return "opens the character menu";
		case Kind::kVideo: return "plays a video";
		case Kind::kHoldsPlayer: return "holds you in place or kills you";
		default: return "?";
		}
	}

	namespace
	{
		std::string List(const std::vector<Finding>& a_findings, bool a_blocking, std::size_t a_max)
		{
			std::string out;
			std::size_t n = 0, all = 0;
			for (const auto& f : a_findings) {
				all += f.blocking() == a_blocking ? 1 : 0;
			}
			for (const auto& f : a_findings) {
				if (f.blocking() != a_blocking) {
					continue;
				}
				if (n++ == a_max) {
					out += "; and " + std::to_string(all - a_max) + " more";
					break;
				}
				out += (out.empty() ? "" : "; ") + std::string(KindText(f.kind)) + ": " + f.call + " (" + f.where + ")";
			}
			return out;
		}
	}

	std::string Report::what(std::size_t a_max) const { return List(findings, true, a_max); }
	std::string Report::notes(std::size_t a_max) const { return List(findings, false, a_max); }

	std::string Report::firstKind() const
	{
		for (const auto& f : findings) {
			if (f.blocking()) {
				return KindText(f.kind);
			}
		}
		return {};
	}

	Report Check(std::uint32_t a_quest, int a_stage, bool a_starts, const Env& a_env)
	{
		Report r;
		if (!a_env.quest || !a_env.script) {
			r.notChecked = "the plugins or scripts are not read";
			return r;
		}
		if (!a_env.quest(a_quest)) {
			r.notChecked = "the quest is not in the plugins read";
			return r;
		}
		Walker w(a_env, r);
		if (a_starts) {
			w.startup(a_quest, {}, 0);
		}
		if (a_stage >= 0) {
			w.stage(a_quest, a_stage, {}, 0);
		}
		const bool blocking = std::any_of(r.findings.begin(), r.findings.end(), [](const Finding& f) { return f.blocking(); });
		r.state = blocking ? Report::State::kUnsafe : !r.notChecked.empty() ? Report::State::kNotChecked : Report::State::kSafe;
		return r;
	}
}
