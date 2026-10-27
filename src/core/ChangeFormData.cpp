// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "ChangeFormData.h"
#include <set>

#include "ByteIO.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ess::cf
{
	namespace
	{
		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		bool Bit(std::uint32_t flags, int n) { return (flags >> n) & 1; }

		struct Stop : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		// ---- tree-building reader ----------------------------------------------------------
		class P
		{
		public:
			P(const SaveFile& s, const std::vector<std::uint8_t>& body, const TypeLookup& types, Decoded& out) :
				_save(s), r(body.data(), body.size()), _types(types), _out(out)
			{
				_stack.push_back(&out.root);
			}

			Reader r;
			std::uint8_t                 version = 0;  // the change form's version (some fields depend on it)
			std::set<std::uint64_t>      pathIDs;      // pathing stream objects already read in this change form

			// one change flag's data: where it starts and ends in the body
			template <class F>
			void section(int a_bit, F f)
			{
				const auto start = r.pos();
				f();
				_out.sections[a_bit] = { start, r.pos() };
			}

			Node& add(const std::string& label, std::string value = {})
			{
				auto& k = top().kids;
				k.push_back(Node{ label, std::move(value) });
				return k.back();
			}
			template <class F>
			void group(const std::string& label, F f)
			{
				auto& n = add(label);
				_stack.push_back(&n);
				try {
					f();
				} catch (...) {
					_stack.pop_back();
					throw;
				}
				_stack.pop_back();
			}

			std::uint8_t u8(const std::string& l)
			{
				const auto v = r.u8();
				add(l, std::to_string(v));
				return v;
			}
			std::int16_t i16(const std::string& l)
			{
				const auto v = r.i16();
				add(l, std::to_string(v));
				return v;
			}
			std::uint16_t flags16(const std::string& l)
			{
				const auto v = r.u16();
				add(l, Fmt("0x%04X", v));
				return v;
			}
			std::uint8_t flags8(const std::string& l)
			{
				const auto v = r.u8();
				add(l, Fmt("0x%02X", v));
				return v;
			}
			std::uint32_t flags32(const std::string& l)
			{
				const auto v = r.u32();
				add(l, Fmt("0x%08X", v));
				return v;
			}
			std::int32_t i32(const std::string& l)
			{
				const auto v = r.i32();
				add(l, std::to_string(v));
				return v;
			}
			float f32(const std::string& l)
			{
				const auto v = r.f32();
				add(l, Fmt("%g", v));
				return v;
			}
			void floats(const std::string& l, std::size_t n)
			{
				std::string s;
				for (std::size_t i = 0; i < n; ++i) {
					s += (i ? ", " : "") + Fmt("%g", r.f32());
				}
				add(l, s);
			}
			void ints(const std::string& l, std::size_t n)
			{
				std::string s;
				for (std::size_t i = 0; i < n; ++i) {
					if (i < 16) {
						s += (i ? ", " : "") + std::to_string(r.i32());
					} else {
						r.i32();
					}
				}
				if (n > 16) {
					s += Fmt(", ... (%zu)", n);
				}
				add(l, s);
			}
			std::size_t bytes(const std::string& l, std::size_t n)
			{
				r.need(n);
				const auto  at = r.pos();
				std::string s;
				for (std::size_t i = 0; i < n && i < 24; ++i) {
					s += Fmt("%02X ", r.here()[i]);
				}
				if (n > 24) {
					s += Fmt("... (%zu bytes)", n);
				}
				r.seek(r.pos() + n);
				add(l, s);
				return at;
			}
			std::uint32_t vs(const std::string& l)
			{
				const auto    b0 = r.u8();
				std::uint32_t v = b0;
				switch (b0 & 3) {
				case 0:
					break;
				case 1:
					v |= std::uint32_t(r.u8()) << 8;
					break;
				default:
					v |= std::uint32_t(r.u8()) << 8;
					v |= std::uint32_t(r.u8()) << 16;
					break;
				}
				v >>= 2;
				if (!l.empty()) {
					add(l, std::to_string(v));
				}
				return v;
			}
			// returns {offset, size}
			std::pair<std::size_t, std::size_t> bytesVS(const std::string& l)
			{
				const auto n = vs(l + "_COUNT");
				const auto at = bytes(l, n);
				return { at, n };
			}
			void intsVS(const std::string& l) { ints(l, vs(l + "_COUNT")); }
			void floatsVS(const std::string& l)
			{
				const auto n = vs(l + "_COUNT");
				floats(l, n);
			}
			void longsVS(const std::string& l)
			{
				const auto n = vs(l + "_COUNT");
				r.need(std::size_t(n) * 8);
				r.seek(r.pos() + std::size_t(n) * 8);
				add(l, Fmt("%u values", n));
			}
			std::uint32_t ref(const std::string& l)
			{
				const auto id = RefID::read(r);
				auto&      n = add(l);
				n.isRef = true;
				n.refRaw = id.data;
				n.formID = id.isZero() ? 0 : _save.formIDOf(id);
				lastFormID = n.formID;
				return id.data;
			}
			std::uint32_t refResolved(const std::string& l)
			{
				ref(l);
				return top().kids.back().formID;
			}
			std::string wstring(const std::string& l)
			{
				const auto n = r.u16();
				r.need(n);
				std::string s(reinterpret_cast<const char*>(r.here()), n);
				r.seek(r.pos() + n);
				add(l, "\"" + s + "\"");
				return s;
			}
			template <class F>
			void vsArray(const std::string& l, F f)
			{
				const auto n = vs("");
				if (n > 100000) {
					throw Stop(Fmt("%s: implausible count %u", l.c_str(), n));
				}
				group(Fmt("%s (%u)", l.c_str(), n), [&] {
					for (std::uint32_t i = 0; i < n; ++i) {
						group(Fmt("[%u]", i), [&] { f(); });
					}
				});
			}
			template <class F>
			void countedArray(const std::string& l, std::uint32_t n, F f)
			{
				if (n > 100000) {
					throw Stop(Fmt("%s: implausible count %u", l.c_str(), n));
				}
				group(Fmt("%s (%u)", l.c_str(), n), [&] {
					for (std::uint32_t i = 0; i < n; ++i) {
						group(Fmt("[%u]", i), [&] { f(); });
					}
				});
			}

			void changeFormFlags()
			{
				group("CHANGE_FORM_FLAGS", [&] {
					flags32("FLAGS");
					i16("UNKNOWN");
				});
			}

			InvCharge*    inv = nullptr;  // inventory entry being read (captures Enchantment / Charge)
			std::uint32_t lastFormID = 0;  // of the last ref() read
			int        extraDepth = 0;  // nesting of extra data groups below the inventory entry

			std::size_t mark() { return top().kids.size(); }
			void        rollback(std::size_t a_mark) { top().kids.resize(a_mark); }

			std::string typeOf(std::uint32_t formID) const { return _types && formID ? _types(formID) : std::string(); }
			Decoded&    out() { return _out; }

		private:
			Node& top() { return *_stack.back(); }

			const SaveFile&    _save;
			const TypeLookup&  _types;
			Decoded&           _out;
			std::vector<Node*> _stack;
		};

		// ---- shared pieces --------------------------------------------------------------------

		void ExtraData(P& p);
		void NPC(P& p, std::uint32_t flags, bool inlined);
		void CombatController(P& p);
		void CombatObjectData(P& p, std::uint32_t a_cls);

		void NonActorMagicCaster(P& p)
		{
			p.i32("unk1");
			p.ref("ref1");
			p.i32("unk2");
			p.i32("unk3");
			p.ref("ref2");
			p.f32("unk4");
			p.ref("ref3");
			p.ref("ref4");
		}

		// PackageLocation / PackageTarget of a package created in game
		void PackageLocation(P& p)
		{
			p.group("LOCATION", [&] {
				const auto type = p.u8("TYPE");
				p.i32("RADIUS");
				switch (type) {
				case 0:
				case 1:
				case 2:
				case 3:
				case 4:
					p.ref("REF");
					break;
				case 5:
				case 8:
				case 9:
				case 10:
					p.i32("VALUE");
					break;
				case 11:
					p.u8("VALUE");
					break;
				default:
					break;
				}
			});
		}

		void PackageTarget(P& p)
		{
			p.group("TARGET", [&] {
				const auto type = p.u8("TYPE");
				p.i32("COUNT");
				switch (type) {
				case 0:
				case 1:
				case 3:
					p.ref("REF");
					break;
				case 2:
				case 4:
				case 5:
					p.i32("VALUE");
					break;
				default:
					break;
				}
			});
		}

		// a package created in game (FF form): TESPackage::LoadGame of its class
		void CreatedPackage(P& p, std::int8_t a_type)
		{
			p.group(Fmt("CREATED PACKAGE (type %d)", a_type), [&] {
				switch (a_type) {
				case 0x0f:
				case 0x15:
				case 0x17:
				case 0x18:
				case 0x1c:
					throw Stop(Fmt("created package (type %d) is not decoded yet", a_type));
				default:
					break;
				}
				p.bytes("PACKAGE DATA", 12);
				const auto mask = p.u8("PARTS");
				if (mask & 1) {
					PackageLocation(p);
				}
				if (mask & 2) {
					PackageTarget(p);
				}
				if (mask & 4) {
					p.group("TYPE DATA", [&] {
						auto optLocation = [&] {
							if (p.u8("HAS LOCATION")) {
								PackageLocation(p);
							}
						};
						switch (a_type) {
						case 1:   // Follow
						case 2:   // Escort
							p.i32("UNK10");
							optLocation();
							break;
						case 3:   // Eat
						case 8:   // UseItemAt
						case 9:   // Ambush
							optLocation();
							break;
						case 13:  // Patrol
							p.u8("UNK8");
							p.u8("UNK9");
							break;
						case 15:  // Dialogue
						case 16:  // UseWeapon
							throw Stop(Fmt("created package data (type %d) is not decoded yet", a_type));
						default:
							break;
						}
					});
				}
				p.i32("UNKD8");
				if (a_type == 0x16) {  // FleePackage
					p.u8("UNK110");
					p.u8("UNK111");
					p.floats("POS", 3);
					p.f32("UNK104");
					p.u8("UNK112");
					p.u8("UNK113");
					p.u8("UNK114");
					p.ref("REF1");
					p.ref("REF2");
					const auto n = p.vs("");
					p.countedArray("REFS", n, [&] { p.ref("REF"); });
				}
			});
		}

		void PackageDataExtra(P& p, std::int8_t a_type);

		void ExtraDataData(P& p)
		{
			const auto type = p.r.u8();
			auto       one = [&](const char* name, auto body) { p.group(name, body); };
			switch (type) {
			case 0: one("NULL", [] {}); break;
			case 4: one("ExtraExtraData1", [&] { p.group("SUB_DATA", [&] { ExtraDataData(p); }); }); break;
			case 8:
				one("ExtraExtraData2", [&] {
					p.group("SUB_DATA1", [&] { ExtraDataData(p); });
					p.group("SUB_DATA2", [&] { ExtraDataData(p); });
				});
				break;
			case 12:
				one("ExtraExtraData3", [&] {
					p.group("SUB_DATA1", [&] { ExtraDataData(p); });
					p.group("SUB_DATA2", [&] { ExtraDataData(p); });
					p.group("SUB_DATA3", [&] { ExtraDataData(p); });
				});
				break;
			case 22: one("Worn", [] {}); break;
			case 23: one("WornLeft", [] {}); break;
			case 24:
				one("PackageStartLocation", [&] {
					p.ref("UNK");
					p.floats("POS", 3);
					p.f32("UNK1");
				});
				break;
			case 25:
				one("Package", [&] {
					p.ref("UNK1");
					p.ref("UNK2");
					p.i32("UNK3");
					p.bytes("UNK4", 3);
				});
				break;
			case 26:
				one("TrespassPackage", [&] {
					if (p.ref("PACK") == 0) {
						throw Stop("TrespassPackage incomplete");
					}
				});
				break;
			case 27: one("RunOncePacks", [&] { p.intsVS("PACKS"); }); break;
			case 28: one("ReferenceHandle", [&] { p.ref("ID"); }); break;
			case 29: one("Unknown29", [] {}); break;
			case 30: one("LevCreaModifier", [&] { p.i32("MOD"); }); break;
			case 31: one("Ghost", [&] { p.u8("UNK"); }); break;
			case 32: one("UNKNOWN32", [] {}); break;
			case 33: one("Ownership", [&] { p.ref("OWNER"); }); break;
			case 34: one("Global", [&] { p.ref("UNK"); }); break;
			case 35: one("Rank", [&] { p.ref("RANKID"); }); break;
			case 36:
				one("Count", [&] {
					const auto v = p.i16("COUNT");
					if (p.inv) {
						p.inv->count = v;  // this stack's count
					}
				});
				break;
			case 37: one("Health", [&] { p.f32("HEALTH"); }); break;
			case 39: one("TimeLeft", [&] { p.i32("TIME"); }); break;
			case 40:
				one("Charge", [&] {
					const auto at = p.r.pos();
					const auto v = p.f32("CHARGE");
					if (p.inv) {
						p.inv->hasCharge = true;
						p.inv->charge = v;
						p.inv->chargeOffset = at;
					}
				});
				break;
			case 42:
				one("Lock", [&] {
					p.bytes("UNKS", 2);
					p.ref("KEY");
					p.ints("UNKS2", 2);
				});
				break;
			case 43:
				one("Teleport", [&] {
					p.floats("POS", 3);
					p.floats("ROT", 3);
					p.u8("UNK");
					p.ref("REF");
				});
				break;
			case 44: one("MapMarker", [&] { p.u8("UNK"); }); break;
			case 45:
				one("LeveledCreature", [&] {
					p.ref("UNK1");
					p.ref("UNK2");
					const auto flags = p.flags32("NPCChangeFlags");
					p.group("NPC", [&] { NPC(p, flags, true); });
				});
				break;
			case 46:
				one("LeveledItem", [&] {
					p.i32("UNK");
					p.u8("UNK2");
				});
				break;
			case 47: one("Scale", [&] { p.f32("scale"); }); break;
			case 49: one("NonActorMagicCaster", [&] { p.group("CASTER", [&] { NonActorMagicCaster(p); }); }); break;
			case 50:
				one("NonActorMagicTarget", [&] {
					p.ref("ref");
					p.vsArray("targets", [&] {
						p.ref("REF");
						p.u8("unk1");
						p.vs("unk2");
						p.bytesVS("data");
					});
				});
				break;
			case 52: one("PlayerCrimeList", [&] { p.longsVS("list"); }); break;
			case 53: one("Unknown53", [] {}); break;
			case 56: one("ItemDropper", [&] { p.ref("unk"); }); break;
			case 61: one("CannotWear", [] {}); break;
			case 62:
				one("ExtraPoison", [&] {
					p.ref("ref");
					p.i32("unk");
				});
				break;
			case 68: one("FriendHits", [&] { p.floatsVS("unk"); }); break;
			case 69: one("HeadingTarget", [&] { p.ref("targetID"); }); break;
			case 72: one("StartingWorldOrCell", [&] { p.ref("worldOrCellID"); }); break;
			case 73: one("HotKey", [&] { p.u8("unk"); }); break;
			case 76:
				one("InfoGeneralTopic", [&] {
					p.group("TOPIC", [&] {
						p.wstring("NAME");
						p.bytes("UNK", 5);
						for (int i = 0; i < 4; ++i) {
							p.ref(Fmt("REF%d", i));
						}
					});
				});
				break;
			case 77: one("HasNoRumors", [&] { p.u8("FLAG"); }); break;
			case 79: one("TerminalState", [&] { p.bytes("STATE", 2); }); break;
			case 83: one("Unknown83", [&] { p.i32("unk"); }); break;
			case 84: one("CanTalkToPlayer", [&] { p.u8("FLAG"); }); break;
			case 85: one("ObjectHealth", [&] { p.f32("HEALTH"); }); break;
			case 88:
				one("ModelSwap", [&] {
					p.ref("REF");
					p.i32("UNK");
				});
				break;
			case 89: one("Radius", [&] { p.f32("RADIUS"); }); break;
			case 91:
				one("FactionChanges", [&] {
					p.vsArray("FactionChanges", [&] {
						p.ref("FACTION");
						p.u8("RANK");
					});
					p.ref("FACTION2");
					p.u8("RANK2");
				});
				break;
			case 92:
				one("DismemberedLimbs", [&] {
					p.i16("UNK1");
					p.i32("UNK2");
					p.i32("UNK3");
					p.u8("UNK4");
					p.ref("REF");
					p.vsArray("LIMBS", [&] {
						p.bytes("UNK", 4);
						p.vsArray("REFS", [&] { p.ref("REF"); });
					});
				});
				break;
			case 93: one("ActorCause", [&] { p.i32("ID"); }); break;
			case 101: one("CombatStyle", [&] { p.ref("REF"); }); break;
			case 102:
				one("MYSTERIOUS", [&] {
					p.ref("REF1");
					p.ref("REF2");
				});
				break;
			case 104: one("OpenCloseActivateRef", [&] { p.ref("REF"); }); break;
			case 108:
				one("PackageData", [&] {
					const auto type = static_cast<std::int8_t>(p.u8("TYPE"));
					if (type != -1) {
						PackageDataExtra(p, type);
					}
				});
				break;
			case 106:
				one("Ammo", [&] {
					p.ref("REF");
					p.i32("COUNT");
				});
				break;
			case 111:
				one("SayTopicInfoOnceADay", [&] {
					p.vsArray("INFOS", [&] {
						p.ref("INFO");
						p.i32("UNK1");
						p.i32("UNK2");
					});
				});
				break;
			case 112: one("EncounterZone", [&] { p.ref("REF"); }); break;
			case 113:
				one("SayToTopicInfo", [&] {
					p.ref("REF1");
					p.u8("UNK1");
					p.i32("UNK2");
					p.ref("REF2");
					p.group("DATA2", [&] {
						p.wstring("TEXT1");
						p.wstring("TEXT2");
						p.i32("UNK1");
						p.i32("UNK2");
						p.u8("UNK3");
						p.ref("REF1");
						p.ref("REF2");
						p.ref("REF3");
						p.u8("UNK4");
					});
				});
				break;
			case 120:
				one("GuardedRefData", [&] {
					p.vsArray("DATA", [&] {
						p.ref("INFO");
						p.i32("UNK1");
						p.u8("UNK2");
					});
				});
				break;
			case 133: one("AshPileRef", [&] { p.ref("REF"); }); break;
			case 135:
				// ReSaver's "TEST" (read as four RefIDs = 12 bytes). It is the game's FollowerSwimBreadcrumbs
				// and takes 20 bytes: the leader's position, its navmesh and a state; read as 12 bytes, all
				// that followed (an NPC's whole inventory) was read 8 bytes off.
				one("FollowerSwimBreadcrumbs", [&] {
					p.floats("LEADER POS", 3);
					p.flags32("NAVMESH");
					p.flags32("STATE");
				});
				break;
			case 136:
				one("AliasInstanceArray", [&] {
					p.vsArray("ALIASES", [&] {
						p.ref("QUEST");
						p.i32("ALIAS");
					});
				});
				break;
			case 140: one("PromotedRef", [&] { p.vsArray("REFS", [&] { p.ref("REF"); }); }); break;
			case 142: one("OutfitItem", [&] { p.ref("REF"); }); break;
			case 146: one("SceneData", [&] { p.ref("REF"); }); break;
			case 149:
				one("FromAlias", [&] {
					p.ref("REF");
					p.i32("VAL");
				});
				break;
			case 150: one("ShouldWear", [&] { p.u8("UNK"); }); break;
			case 152:
				one("AttachedArrows3D", [&] {
					p.vsArray("ARROWS", [&] {
						if (p.ref("REF") != 0) {
							if (p.i16("unknU16") != -1) {
								p.i32("UNK2");
								p.floats("LOCATION", 8);
							}
						}
					});
					p.i16("UNK1");
					p.i16("UNK2");
				});
				break;
			case 153:
				one("TextDisplayData", [&] {
					p.group("TEXTDISPLAY", [&] {
						const auto r1 = p.ref("REF1");
						const auto r2 = p.ref("REF2");
						const auto unk = p.i32("UNK");
						if (r1 == 0 && r2 == 0 && unk == -2) {
							p.wstring("TEXT");
						}
					});
				});
				break;
			case 155:
				one("Enchantment", [&] {
					const auto raw = p.ref("ENCH");
					const auto at = p.r.pos();
					const auto v = p.i16("CHARGE");
					if (p.inv) {
						p.inv->hasEnchant = true;
						p.inv->enchRaw = raw;
						p.inv->ench = raw ? p.lastFormID : 0;
						p.inv->maxCharge = static_cast<std::uint16_t>(v);
						p.inv->maxChargeOffset = at;
					}
				});
				break;
			case 156: one("Soul", [&] { p.u8("SIZE"); }); break;
			case 157: one("ForcedTarget", [&] { p.ref("TARGET"); }); break;
			case 159:
				one("UniqueId", [&] {
					p.i32("ID1");
					p.i16("ID2");
				});
				break;
			case 160: one("Flags", [&] { p.flags32("FLAGS"); }); break;
			case 161:
				one("RefrPath", [&] {
					p.floats("UNK1", 18);
					p.ints("UNK2", 4);
				});
				break;
			case 164: one("ForcedLandingMarker", [&] { p.ref("SITE"); }); break;
			case 169:
				one("Interaction", [&] {
					p.i32("UNK1");
					p.ref("Target1");
					p.ref("Target2");
					p.u8("UNK2");
				});
				break;
			case 174:
				one("GroupConstraint", [&] {
					p.group("CONSTRAINT", [&] {
						p.i32("UNK1");
						p.ref("REF");
						p.wstring("NAME1");
						p.wstring("NAME2");
						p.floats("POS", 3);
						p.floats("ROT", 3);
						p.i32("UNK2");
						p.f32("UNK3");
					});
				});
				break;
			case 175:
				one("ScriptedAnimDependence", [&] {
					const auto n = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray("CONSTRAINT", n, [&] {
						p.ref("REF");
						p.i32("UNK1");
					});
				});
				break;
			case 176:
				one("CachedScale", [&] {
					p.f32("SCALE1");
					p.f32("SCALE2");
				});
				break;
			default:
				throw Stop(Fmt("unknown extra data type %u", type));
			}
		}

		void ExtraData(P& p)
		{
			const auto n = p.vs("");
			if (n > 1024) {
				throw Stop(Fmt("excessive extra data count %u", n));
			}
			p.group(Fmt("EXTRADATA (%u)", n), [&] {
				for (std::uint32_t i = 0; i < n; ++i) {
					ExtraDataData(p);
				}
			});
		}

		void Inventory(P& p)
		{
			p.vsArray("INVENTORY", [&] {
				const auto item = p.refResolved("ITEM");
				const auto count = p.i32("COUNT");
				// the entry's extra data is a list of extra data lists, one per distinct stack (each its
				// own enchantment, charge, health...)
				const auto lists = p.vs("");
				if (lists > 1024) {
					throw Stop(Fmt("excessive extra data list count %u", lists));
				}
				p.group(Fmt("EXTRADATA (%u)", lists), [&] {
					for (std::uint32_t i = 0; i < lists; ++i) {
						const auto k = p.vs("");
						if (k > 1024) {
							throw Stop(Fmt("excessive extra data count %u", k));
						}
						InvCharge e;
						e.item = item;
						e.count = 1;  // a stack without a Count extra holds one
						p.inv = &e;
						try {
							p.group(Fmt("ExtraExtraData%u", k), [&] {
								for (std::uint32_t j = 0; j < k; ++j) {
									p.group(k == 1 ? std::string("SUB_DATA") : Fmt("SUB_DATA%u", j + 1), [&] { ExtraDataData(p); });
								}
							});
						} catch (...) {
							p.inv = nullptr;
							throw;
						}
						p.inv = nullptr;
						if (e.hasEnchant || e.hasCharge) {
							p.out().charged.push_back(e);
						}
					}
				});
				(void)count;
			});
		}

		int InitialType(const RefID& id, std::uint32_t flags)
		{
			if (id.type() == RefID::Type::kCreated) {
				return 5;
			}
			if (Bit(flags, 25) || Bit(flags, 3)) {
				return 6;
			}
			if (Bit(flags, 2) || Bit(flags, 1)) {
				return 4;
			}
			return 0;
		}

		void Initial(P& p, int t)
		{
			p.group("INITIAL", [&] {
				switch (t) {
				case 4:
					p.ref("CELL");
					p.floats("POS", 3);
					p.floats("ROT", 3);
					break;
				case 5:
					p.ref("CELL");
					p.floats("POS", 3);
					p.floats("ROT", 3);
					p.u8("UNK");
					p.out().baseObject = p.refResolved("BASE_OBJECT");
					break;
				case 6:
					p.ref("CELL");
					p.floats("POS", 3);
					p.floats("ROT", 3);
					p.ref("STARTING CELL");
					p.i16("UNK1");
					p.i16("UNK2");
					break;
				default:
					break;
				}
			});
		}

		void Havok(P& p, std::uint32_t flags)
		{
			if (Bit(flags, 2)) {
				const auto [at, n] = p.bytesVS("HAVOK");
				p.out().hasHavok = true;
				p.out().havokOffset = at;
				p.out().havokSize = n;
			}
		}

		void Refr(P& p, const RefID& id, std::uint32_t flags)
		{
			const int t = InitialType(id, flags);
			Initial(p, t);
			Havok(p, flags);
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 7)) {
				p.out().baseObject = p.refResolved("BASE_OBJECT");
			}
			if (Bit(flags, 4)) {
				p.f32("SCALE");
			}
			if (Bit(flags, 6) || Bit(flags, 12) || Bit(flags, 29) || Bit(flags, 31) || Bit(flags, 11) || Bit(flags, 17) || Bit(flags, 25) ||
				Bit(flags, 26) || Bit(flags, 10)) {
				ExtraData(p);
			}
			if (Bit(flags, 5) || Bit(flags, 27)) {
				Inventory(p);
			}
			// ReSaver reads a separate promotion list here; SE saves keep it in the PromotedRef extra
			// data instead (checked against real saves), so there is no list to read.
			if (Bit(flags, 28)) {
				p.bytesVS("ANIMATIONS");
			}
			if (t == 5 && p.r.remaining() && p.typeOf(p.out().baseObject) == "EXPL") {
				p.group("EXPLOSION", [&] {
					p.floats("UNKS", 4);
					p.ints("UNK1", 2);
					p.floats("UNK2", 2);
					p.floats("UNK3", 3);
					p.floats("UNK4", 3);
					p.f32("UNK5");
					for (int i = 0; i < 4; ++i) {
						p.ref(Fmt("REF%d", i));
					}
					p.f32("UNK6");
					p.i32("UNK7");
					p.u8("FLAG");
					p.group("CASTER", [&] { NonActorMagicCaster(p); });
				});
			}
		}

		// ---- the actor's own data, after its reference data (Actor::LoadGame in the game) ----
		// Mapped from SkyrimSE.exe 1.6 (Actor::SaveGame / LoadGame and what they call); names from
		// CommonLibSSE where it has them. Nothing here depends on change flags: every ACHR has it.

		// MagicCaster + ActorMagicCaster state (one of the four casters: left, right, other, instant)
		void ActorCaster(P& p)
		{
			p.i32("STATE");
			p.ref("CURRENT SPELL");
			p.f32("CAST TIME");
			p.i32("UNK1");
			p.ref("DESIRED TARGET");
			p.i32("UNK2");
			p.i32("UNK3");
			p.ref("UNK REF");
			p.flags32("FLAGS");
			const auto n = p.vs("DATA_SIZE");
			p.bytes("DATA", n);
		}

		void ActorValueModifiers(P& p, const char* a_name)
		{
			p.group(a_name, [&] {
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				std::string s;
				for (std::uint32_t i = 0; i < n && i < 3; ++i) {
					s += (i ? ", " : "") + Fmt("%g", p.r.f32());
				}
				p.add("PERMANENT, TEMPORARY, DAMAGE", s);
			});
		}

		// ---- packages: instance data and the procedure tree's run state ----
		// Each procedure's run state is stored by the procedure's name ("Sequence", "Sandbox"...).
		void ProcedureState(P& p);

		void ProcedureChild(P& p)
		{
			ProcedureState(p);
		}
		void ProcedureNamed(P& p, const std::string& a_name);

		// a nested state of a fixed kind, present when its flag byte is set
		void OptionalNamed(P& p, const char* a_name)
		{
			if (p.r.u8()) {
				p.group(a_name, [&] { ProcedureNamed(p, a_name); });
			}
		}

		// ---- pathing stream objects. Polymorphic objects are written as a class ID: Bethesda's CRC32 of
		// the class name (e.g. 330EB0E3 = "PathingRequest") ----
		enum : std::uint32_t
		{
			kBSPathingRequest = 0x0534ED31,
			kPathingRequest = 0x330EB0E3,
			kPathingRequestClosePoint = 0xEB5ED874,
			kPathingRequestFlee = 0xFA2763CE,
			kPathingRequestFly = 0x3C5FF134,
			kPathingRequestStopMoving = 0x8B2152AF,
			kPathingRequestClosestGoal = 0xF31543AB,
			kPathingRequestCover = 0x0618E573,
			kPathingRequestFlyAction = 0xA5021751,
			kPathingRequestFlyHover = 0x8353103B,
			kPathingRequestFlyLand = 0xF075EEF7,
			kPathingRequestFlyOrbit = 0xCDF9A2FC,
			kPathingRequestFlyTakeOff = 0x98C4C679,
			kPathingRequestHide = 0x0528E757,
			kPathingRequestLOS = 0x54DACA55,
			kPathingRequestOptimalLocation = 0xCA622528,
			kPathingRequestRotate = 0xC702BB5B,
		};

		// a polymorphic member: class ID, then that class's data (family = which loader it is)
		void PolyMember(P& p, const char* a_label, const char* a_family)
		{
			p.group(a_label, [&] {
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				if (cls == 0) {
					return;
				}
				switch (cls) {
				case 0xA5E9A03C:  // PathingCell: the cell, or (no cell) the worldspace
					if (!p.ref("CELL")) {
						p.ref("WORLDSPACE");
					} else {
						p.i32("UNK");
					}
					return;
				case 0x5826A5DD:  // PathingLockData
					if (p.u8("LOCKED")) {
						p.ref("REF");
						if (p.version > 0x15) {
							p.u8("UNK");
						}
					}
					return;
				case 0x0C28D1C5:  // MovementParameters
					p.floats("VALUES", 5);
					return;
				default:
					throw Stop(Fmt("%s class %08X is not decoded yet", a_family, cls));
				}
			});
		}

		// BSPathingLocation: a point on the navmesh
		void PathingLocation(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				p.floats("POS", 3);
				PolyMember(p, "SPACE", "pathing space");
				p.ref("CELL/WORLD");
				p.i16("TRIANGLE");
				p.u8("FLAG1");
				p.u8("FLAG2");
			});
		}

		void BSPathingRequest(P& p)
		{
			PathingLocation(p, "START");
			p.f32("START UNK");
			p.group("GOAL", [&] {
				PathingLocation(p, "LOCATION");
				p.floats("VALUES", 7);
			});
			const auto n = static_cast<std::uint32_t>(p.r.i32());
			p.countedArray("SEGMENTS", n, [&] { p.ints("VALUES", 9); });
			p.ints("UNK4", 4);
			p.group("PARAMS", [&] {
				p.i32("UNK1");
				p.i32("UNK2");
				p.i16("UNK3");
				p.i16("UNK4");
			});
			p.group("AVOID", [&] {
				p.i32("UNK1");
				p.i32("UNK2");
				p.i32("UNK3");
				PolyMember(p, "OBJECT", "pathing avoid");
				p.ref("REF");
			});
			PolyMember(p, "BUILDER", "path builder");
			const auto m = static_cast<std::uint32_t>(p.r.i32());
			p.countedArray("REFS", m, [&] { p.ref("REF"); });
			p.i32("UNK_F0");
		}

		void OptionalRef(P& p, const char* a_label)
		{
			if (p.r.u8()) {
				p.ref(a_label);
			}
		}

		// PathingRequestFly: the shared part of every flying request
		void FlyRequest(P& p)
		{
			BSPathingRequest(p);
			OptionalRef(p, "TARGET");
			OptionalRef(p, "NAVMESH REF");
			p.f32("UNK110");
			if (p.version > 0x3b) {
				p.u8("UNK114");
				p.f32("UNK118");
				p.f32("UNK11C");
			}
			if (p.version > 0x44) {
				p.u8("UNK120");
			}
		}

		void PathingObjectData(P& p, std::uint32_t a_cls)
		{
			switch (a_cls) {
			case kBSPathingRequest:
			case kPathingRequest:
			case kPathingRequestStopMoving:
			case kPathingRequestRotate:
			case kPathingRequestCover:
				BSPathingRequest(p);
				break;
			case kPathingRequestFlee:
				BSPathingRequest(p);
				p.f32("FLEE DISTANCE");
				break;
			case kPathingRequestFly:
			case kPathingRequestFlyTakeOff:
				FlyRequest(p);
				break;
			case kPathingRequestFlyLand:
				FlyRequest(p);
				p.floats("LAND POS", 3);
				p.i32("UNK13");
				p.f32("UNK14");
				p.u8("UNK15");
				if (p.version > 1) {
					p.ref("LAND REF");
				}
				break;
			case kPathingRequestFlyAction:
				FlyRequest(p);
				p.i32("ACTION");
				OptionalRef(p, "TARGET");
				p.floats("POS", 3);
				OptionalRef(p, "REF2");
				OptionalRef(p, "REF3");
				OptionalRef(p, "REF4");
				p.u8("UNK158");
				p.i32("UNK15C");
				break;
			case kPathingRequestFlyHover:
			{
				FlyRequest(p);
				p.floats("HOVER", 3);
				const auto n = p.r.u32();
				p.countedArray("GOALS", n, [&] {
					PathingLocation(p, "LOCATION");
					p.floats("VALUES", 7);
				});
				break;
			}
			case kPathingRequestFlyOrbit:
				FlyRequest(p);
				p.f32("RADIUS");
				p.i32("UNK12C");
				if (p.version > 0x42) {
					p.floats("UNK130", 3);
				}
				break;
			case kPathingRequestHide:
				BSPathingRequest(p);
				PathingLocation(p, "HIDE FROM");
				p.f32("UNK130");
				break;
			case kPathingRequestLOS:
			{
				BSPathingRequest(p);
				const auto n = p.r.u32();
				p.countedArray("POINTS", n, [&] { p.floats("POS", 3); });
				p.floats("UNK120", 5);
				break;
			}
			case kPathingRequestOptimalLocation:
				BSPathingRequest(p);
				p.floats("UNK100", 4);
				break;
			case kPathingRequestClosestGoal:
			{
				BSPathingRequest(p);
				const auto n = p.r.u32();
				p.countedArray("GOALS", n, [&] { PathingLocation(p, "LOCATION"); });
				p.i32("UNK118");
				break;
			}
			case kPathingRequestClosePoint:
				BSPathingRequest(p);
				p.f32("UNK130");
				p.f32("UNK134");
				p.u8("UNK138");
				PathingLocation(p, "CLOSE POINT");
				p.u8("UNK139");
				p.u8("UNK13A");
				break;
			default:
				throw Stop(Fmt("pathing object class %08X is not decoded yet", a_cls));
			}
		}

		// An object written through the game's pathing stream (BSPathingStream): an ID, and the first
		// time an ID appears in a change form, the object itself (class ID + its data)
		void PathingObject(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const std::uint64_t id = p.version >= 0x4b ? p.r.u64() : p.r.u32();
				p.add("ID", Fmt("%llX", static_cast<unsigned long long>(id)));
				if (id == 0 || !p.pathIDs.insert(id).second) {
					return;  // none, or written earlier in this change form
				}
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				PathingObjectData(p, cls);
			});
		}

		// BGSAIWorldLocation*: a place a procedure goes to or watches, stored by kind
		void AIWorldLocation(P& p, const char* a_label)
		{
			const auto kind = p.r.wstring();
			p.group(std::string(a_label) + " (" + kind + ")", [&] {
				if (kind == "LocRef") {
					p.ref("REF");
					p.f32("RADIUS");
					p.f32("UNK1");
					p.f32("UNK2");
					p.u8("FLAG");
				} else if (kind == "LocPt") {
					p.floats("POS", 3);
					p.ref("WORLDSPACE");
					p.ref("CELL");
					p.f32("RADIUS");
					p.f32("UNK");
					p.u8("FLAG1");
					p.u8("FLAG2");
				} else if (kind == "LocCell") {
					p.ref("CELL");
					p.floats("UNK", 3);
				} else {
					throw Stop("AI world location '" + kind + "' is not decoded yet");
				}
			});
		}

		void OptionalLocation(P& p, const char* a_label)
		{
			if (p.r.u8()) {
				AIWorldLocation(p, a_label);
			}
		}

		void ProcedureState(P& p)
		{
			const auto name = p.r.wstring();
			if (name.empty()) {
				p.add("PROCEDURE", "(none)");
				return;
			}
			p.group(name, [&] { ProcedureNamed(p, name); });
		}

		void ProcedureNamed(P& p, const std::string& name)
		{
			{
				if (name == "Sequence" || name == "Stacked") {
					ProcedureChild(p);
					p.i32("CURRENT");
				} else if (name == "Random") {
					ProcedureChild(p);
					p.i32("CURRENT");
					const auto n = p.vs("");
					p.ints("WEIGHTS", n);
				} else if (name == "Simultaneous") {
					const auto n = p.vs("");
					p.countedArray("CHILDREN", n, [&] {
						p.i32("INDEX");
						ProcedureChild(p);
					});
				} else if (name == "Sandbox") {
					p.i32("UNK08");
					p.i32("UNK3C");
					p.i32("UNK68");
					p.i32("UNK70");
					p.u8("UNKAB");
					p.f32("TIME1");
					p.i32("UNK68B");
					p.f32("TIME2");
					p.bytes("FLAGS", 4);
					p.i32("UNK7C");
					p.u8("UNK84");
					p.i32("UNK8C");
					p.i32("UNK90");
					p.i32("UNK94");
					p.i32("UNK98");
					p.i32("UNKA0");
					p.bytes("UNKA4", 7);
					p.ref("REF1");
					p.ref("REF2");
					p.ref("REF3");
					const auto n = p.vs("");
					p.countedArray("ENTRIES", n, [&] {
						p.i32("UNK1");
						p.i32("UNK2");
						p.i32("UNK3");
						p.ref("REF");
					});
					OptionalLocation(p, "LOCATION");
					ProcedureChild(p);
					p.i32("UNK38");
					PathingObject(p, "PATH");
					p.i32("UNK9C");
					if (p.version > 0x30) {
						p.u8("UNKB1");
					}
				} else if (name == "Follow") {
					p.ref("TARGET");
					OptionalLocation(p, "LOCATION1");
					OptionalLocation(p, "LOCATION2");
					OptionalLocation(p, "LOCATION3");
					p.i32("UNK30");
					p.f32("MIN DISTANCE");
					p.f32("MAX DISTANCE");
					PathingObject(p, "PATH");
					p.bytes("FLAGS", 5);
				} else if (name == "UseWeapon") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
					OptionalNamed(p, "Travel");
					p.ref("WEAPON");
					p.i32("UNK28");
					p.ref("TARGET");
					p.i16("UNK34");
					p.i16("UNK36");
					p.i16("UNK38");
					p.i32("UNK3C");
				} else if (name == "UseMagic") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
					OptionalNamed(p, "Travel");
					p.ref("SPELL");
					p.i32("UNK40");
					p.i32("UNK18");
					if (p.version > 10) {
						p.ref("TARGET");
					}
					p.ref("TARGET2");
					p.i16("UNK50");
					p.i32("UNK54");
					if (p.version >= 0x1e) {
						p.i32("UNK10");
						p.i32("UNK14");
					}
					if (p.version > 0x2b) {
						p.i32("UNK4C");
					}
				} else if (name == "Orbit") {
					p.u8("UNK18");
					OptionalLocation(p, "LOCATION");
					p.u8("UNK19");
				} else if (name == "Acquire") {
					p.i32("UNK08");
					p.ref("TARGET");
					p.i32("UNK10");
					p.ref("ITEM");
					OptionalNamed(p, "Activate");
				} else if (name == "Find") {
					OptionalLocation(p, "LOCATION");
					p.i32("UNK");
				} else if (name == "Guard" || name == "HoldPosition") {
					OptionalLocation(p, "LOCATION");
				} else if (name == "KeepAnEyeOn") {
					OptionalLocation(p, "LOCATION");
					OptionalLocation(p, "TARGET LOCATION");
				} else if (name == "LockUnlock") {
					OptionalLocation(p, "LOCATION");
					p.u8("UNK1");
					p.u8("UNK2");
				} else if (name == "Wait") {
					p.f32("TIME");
				} else if (name == "Say") {
					p.u8("DONE");
				} else if (name == "Done") {
				} else if (name == "Eat") {
					p.ref("FOOD");
					p.i32("UNK");
					p.u8("UNK1");
					p.u8("UNK2");
				} else if (name == "Wander") {
					OptionalLocation(p, "LOCATION");
					p.i32("UNK10");
					p.u8("UNK20");
					p.i32("UNK14");
					PathingObject(p, "PATH");
				} else if (name == "Activate") {
					p.u8("TARGET INDEX");
					p.ref("TARGET");
					OptionalLocation(p, "LOCATION");
					p.i32("UNK28");
					p.i32("UNK2C");
					p.u8("UNK30");
					p.u8("UNK31");
					PathingObject(p, "PATH");
				} else if (name == "SitSleep") {
					p.u8("TARGET INDEX");
					p.u8("UNK20");
					OptionalNamed(p, "Activate");
				} else if (name == "UseIdleMarker") {
					p.u8("UNK08");
					p.ref("MARKER");
					p.ref("IDLE");
					p.i32("UNK18");
					PathingObject(p, "PATH");
					p.u8("UNK28");
				} else if (name == "Patrol") {
					p.i32("UNK08");
					p.group("PATROL DATA", [&] {
						p.i32("UNK08");
						p.i32("UNK0C");
						p.i32("UNK38");
						p.u8("UNK35");
						p.u8("UNK34");
						p.i32("UNK30");
						const auto n = p.vs("");
						p.countedArray("REFS", n, [&] { p.ref("REF"); });
					});
					OptionalNamed(p, "UseIdleMarker");
					OptionalNamed(p, "SitSleep");
					PathingObject(p, "PATH");
					p.u8("UNK50");
					p.u8("UNK51");
					p.u8("UNK52");
				} else if (name == "Travel") {
					p.u8("UNK20");
					p.u8("UNK21");
					p.u8("UNK23");
					OptionalLocation(p, "LOCATION");
					p.u8("TARGET INDEX");
					PathingObject(p, "PATH");
				} else {
					throw Stop("package procedure '" + name + "' is not decoded yet");
				}
			}
		}

		void PackageLocation(P& p);
		void PackageTarget(P& p);

		// one entry of a package's instance data (BGSPackageData*), by type name
		void PackageValue(P& p)
		{
			p.u8("INDEX");
			const auto type = p.r.wstring();
			p.group(type, [&] {
				if (type == "Bool") {
					p.u8("VALUE");
				} else if (type == "Int") {
					p.i32("VALUE");
				} else if (type == "Float") {
					p.f32("VALUE");
				} else if (type == "SingleRef" || type == "Ref" || type == "TargetSelector") {
					PackageTarget(p);
				} else if (type == "Location") {
					PackageLocation(p);
				} else if (type == "ObjectList") {
					const auto n = p.vs("");
					p.countedArray("ITEMS", n, [&] {
						p.ref("REF");
						p.ref("FORM");
						p.floats("VALUES", 4);
					});
				} else {
					throw Stop("package data value '" + type + "' is not decoded yet");
				}
			});
		}

		void PackageData(P& p, std::int8_t a_type)
		{
			p.group(Fmt("PACKAGE DATA (%d)", a_type), [&] {
				switch (a_type) {
				case 18:  // CustomActorPackageData: instance values, then the procedure tree's run state
					{
						const auto n = p.vs("");
						p.countedArray("VALUES", n, [&] { PackageValue(p); });
						ProcedureState(p);
					}
					break;
				case 0:
				case 1:
				case 3:
				case 4:
				case 5:
				case 6:
				case 7:
				case 8:
				case 9:
				case 10:
				case 11:
				case 15:
				case 17:
					break;  // no data object for these package types
				default:
					throw Stop(Fmt("package data (type %d) is not decoded yet", a_type));
				}
			});
		}

		void PackageDataExtra(P& p, std::int8_t a_type)
		{
			PackageData(p, a_type);
		}

		// ActorPackage: the package an actor is running (AIProcess, and again in the middle-high process)
		void ActorPackage(P& p, const char* a_name)
		{
			p.group(a_name, [&] {
				const auto package = p.ref("PACKAGE");
				if (!package) {
					return;  // no package: nothing else is saved for it
				}
				if ((package >> 22) == 2) {  // a package created in game (FF): its type, or -1 for none
					const auto created = static_cast<std::int8_t>(p.u8("CREATED TYPE"));
					if (created != -1) {
						CreatedPackage(p, created);
					}
				}
				// the data object's kind comes from the package's own type: all Skyrim packages are
				// template / procedure-tree packages (type 18); the byte is -1 when it has none
				const auto data = static_cast<std::int8_t>(p.u8("DATA STATE"));
				if (data != -1) {
					PackageData(p, 18);
				}
				p.f32("UNK1");
				p.f32("UNK2");
				p.f32("UNK3");
				p.i16("UNK4");
				p.u8("UNK5");
				p.u8("FLAGS");
				p.ref("TARGET");
			});
		}

		void ValueList(P& p, const char* a_name)
		{
			const auto n = static_cast<std::uint32_t>(p.r.i32());
			p.countedArray(a_name, n, [&] {
				p.i32("INDEX");
				p.i32("VALUE");
				p.u8("FLAG");
			});
		}

		// active magic effects: each carries its own size, so its data is kept as bytes
		void ActiveEffects(P& p)
		{
			const auto n = p.vs("");
			p.countedArray("ACTIVE EFFECTS", n, [&] {
				p.ref("MAGIC ITEM");
				p.u8("EFFECT INDEX");
				p.vs("TYPE");
				const auto size = p.vs("SIZE");
				p.bytes("DATA", size);
			});
		}

		void MiddleHighProcess(P& p)
		{
			p.f32("UNK2C8");
			p.f32("UNK2CC");
			p.f32("UNK300");
			p.u8("UNK322");
			p.floats("UNKE0", 3);
			p.f32("UNK29C");
			p.f32("UNK2E8");
			p.u8("UNK315");
			p.u8("UNK31B");
			p.u8("UNK313");
			p.f32("UNK290");
			p.u8("UNK31C");
			p.f32("UNK2A0");
			p.f32("UNK2A4");
			p.u8("UNK310");
			p.u8("UNK321");
			p.i16("UNK308");
			p.f32("UNK2E4");
			p.u8("UNK312");
			p.f32("UNK2E0");
			p.f32("UNK298");
			p.u8("UNK31E");
			p.u8("UNK31F");
			p.u8("UNK314");
			p.ref("UNK280");
			p.ref("UNK210");
			p.ref("UNK218");
			p.ref("UNK208");
			p.ref("UNK228");
			p.floats("UNKD4", 3);
			p.f32("UNK294");
			p.u8("UNK320");
			p.f32("UNK2C0");
			p.i32("UNK");
			const auto n = p.vs("");
			p.countedArray("REFS", n, [&] { p.ref("REF"); });
			ActorPackage(p, "PACKAGE 2");
			ActiveEffects(p);
			p.ref("UNK240");
			const auto m = p.vs("");
			p.countedArray("ITEMS", m, [&] {
				p.ref("ITEM");
				p.i32("UNK");
				if (p.version > 0x2f) {
					p.u8("UNK40");
					p.group("DATA", [&] {
						p.i32("UNK08");
						p.ref("REF1");
						p.ref("REF2");
						p.bytes("FLAGS", 5);
					});
				}
				p.ref("REF");
				p.i32("UNK18");
				p.bytes("FLAGS", 6);
			});
			p.f32("UNK2B4");
			p.f32("UNK2B0");
			p.f32("UNK2B8");
			p.ref("UNKEC");
			p.f32("UNKF0");
			p.f32("UNK2BC");
			p.u8("UNK325");
			p.f32("UNK2F8");
			p.bytes("FLAGS", 5);
			if (p.version > 7) {
				p.u8("UNK32D");
			}
			if (p.version > 0xc) {
				p.u8("UNK32E");
			}
		}

		// animation files loaded for an actor (resource IDs: folder hash, file hash, "hkx")
		void AnimationFiles(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				if (!p.u8("HAS ANIMATIONS")) {
					return;
				}
				p.u8("UNK");
				if (p.version > 0x34) {
					p.u8("UNK2");
				}
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("FILES", n, [&] {
					const auto dir = p.r.u32();
					const auto file = p.r.u32();
					const auto ext = p.r.u32();
					p.add("FILE", Fmt("%08X %08X %.4s", dir, file, reinterpret_cast<const char*>(&ext)));
				});
			});
		}

		void HighProcess(P& p)
		{
			p.u8("UNK454");
			p.u8("UNK45A");
			p.u8("UNK45B");
			p.i16("UNK2DA");
			p.floats("UNK1", 9);
			p.i16("UNK448");
			p.i16("UNK44A");
			p.i16("UNK44C");
			p.u8("UNK457");
			p.floats("UNK2E8", 3);
			p.floats("UNK2DC", 3);
			p.floats("UNK2", 6);
			p.u8("UNK45C");
			p.f32("UNK390");
			p.u8("UNK45D");
			p.f32("UNK394");
			p.u8("UNK46A");
			p.f32("UNK3A0");
			p.f32("UNK3A4");
			p.u8("UNK451");
			p.f32("UNK3C0");
			p.floats("UNK2C0", 3);
			p.u8("UNK44F");
			p.u8("UNK44E");
			p.f32("UNK2CC");
			p.f32("UNK328");
			p.u8("UNK44E_2");
			p.u8("UNK44E_3");
			p.i32("UNK3D0");
			p.f32("UNK200");
			p.u8("UNK450");
			p.f32("UNK2A0");
			p.f32("UNK2B0");
			p.f32("UNK160");
			p.f32("UNK1E0");
			p.u8("UNK460");
			if (p.version > 0x33) {
				p.u8("UNK461");
			}
			p.u8("UNK462");
			p.f32("UNK210");
			p.u8("UNK466");
			p.u8("UNK465");
			p.f32("UNK29C");
			p.u8("UNK");
			p.u8("UNK2");
			p.ref("UNK2F4");
			p.ref("UNK");
			p.ref("UNK138");
			p.ref("UNK178");
			p.ref("UNK36C");
			p.ref("UNK348");
			p.ref("UNK350");
			p.ref("UNK17C");
			p.countedArray("SLOTS", 6, [&] {
				p.ref("REF");
				p.u8("FLAG");
			});
			if (p.u8("HAS LIST")) {
				p.group("LIST", [&] {
					const auto n = p.vs("");
					p.countedArray("ENTRIES", n, [&] {
						p.add("NAME1", p.r.wstring());
						p.add("NAME2", p.r.wstring());
						p.i32("UNK10");
						p.i32("UNK14");
						p.u8("UNK38");
						p.ref("REF1");
						p.ref("REF2");
						p.ref("REF3");
						p.u8("UNK39");
					});
					p.i16("CURRENT");
				});
			}
			{
				const auto n = p.vs("");
				p.countedArray("AVOID", n, [&] {
					p.ints("VALUES", 9);
					p.f32("UNK24");
					p.ref("REF");
				});
			}
			if (p.u8("HAS UNK3D8")) {
				p.i32("UNK0");
				p.floats("UNK4", 3);
				p.f32("UNK10");
				p.ref("REF");
			}
			{
				const auto n = p.vs("CRIME_SIZE");
				p.bytes("CRIME", n);
			}
			p.u8("UNK45F");
			p.u8("UNK");
			{
				const auto n = p.vs("");
				p.countedArray("PAIRS", n, [&] {
					p.i32("UNK1");
					p.i32("UNK2");
					p.ref("REF1");
					p.ref("REF2");
				});
			}
			{
				const auto n = p.vs("");
				p.countedArray("KNOWLEDGE", n, [&] {
					p.ref("REF");
					p.ref("REF1");
					p.ref("REF2");
					p.i32("UNK40");
					p.f32("UNK44");
					if (p.u8("HAS DATA")) {
						p.group("DATA", [&] {
							p.i32("UNK10");
							p.bytes("FLAGS", 3);
							p.floats("POS1", 3);
							p.floats("POS2", 3);
							p.floats("POS3", 3);
							p.f32("TIME1");
							p.f32("TIME2");
							p.f32("TIME3");
						});
					}
				});
			}
			p.i32("UNK0");
			p.ref("UNK8");
			p.floats("UNK10", 6);
			p.u8("UNK467");
			p.f32("UNK318");
			p.f32("UNK1FC");
			p.f32("UNK1F4");
			p.f32("UNK1F8");
			p.ref("UNK358");
			p.ref("UNKD8");
			p.add("UNK258", p.r.wstring());
			if (p.u8("HAS EQUIP DATA")) {
				p.group("EQUIP DATA", [&] {
					p.u8("UNK1");
					p.u8("UNK455");
					if (p.u8("HAS IDLE")) {
						p.add("IDLE", p.r.wstring());
						p.i32("UNK");
					}
					if (p.version > 0x31 && p.u8("HAS IDLE2")) {
						p.add("IDLE2", p.r.wstring());
					}
					// two animation slots (3 and 4): the animation files they hold (resource IDs: folder
					// hash, file hash, "hkx")
					for (int slot = 3; slot < 5; ++slot) {
						AnimationFiles(p, Fmt("ANIMATION SLOT %d", slot).c_str());
					}
					p.i16("UNK3F8");
					p.i16("UNK3FA");
					if (p.version >= 0x32) {
						p.i32("UNK200");
						p.u8("UNK32F");
					}
				});
			}
			p.bytes("FLAGS", 5);
			p.f32("UNK32C");
			p.u8("UNK470");
			p.u8("UNK459");
			p.f32("UNK2B4");
			p.ref("UNKE0");
			p.ref("UNKE8");
			p.floats("UNK3E8", 2);
			p.u8("UNK471");
			if (p.version > 0xc) {
				p.u8("UNK330");
			}
			if (p.version > 0xf) {
				p.ref("UNK270");
				p.u8("UNK463");
				p.u8("UNK464");
			}
			if (p.version > 0x16) {
				p.f32("UNK3A8");
				p.f32("UNK3B0");
			}
			if (p.version > 0x24) {
				p.u8("UNK472");
			}
			if (p.version > 0x26) {
				p.u8("UNK469");
			}
		}

		// AIProcess: present when the actor has an AI process (process level 0-3). Level 3 (low) saves
		// only this common part; 2 adds cached values, 1 the middle-high process, 0 the high process.
		void AIProcess(P& p, std::int32_t a_level)
		{
			p.group("AI PROCESS", [&] {
				p.f32("UNK48");
				p.f32("UNK4C");
				if (p.u8("HAS UNK DATA")) {
					p.group("UNK DATA", [&] {
						p.floats("VALUES", 12);
						ValueList(p, "LIST1");
						ValueList(p, "LIST2");
					});
				}
				ActorPackage(p, "CURRENT PACKAGE");
				p.u8("UNK136");
				p.f32("HOURS TO SLEEP?");
				p.ref("UNK100");
				p.i32("UNK58");
				p.i32("UNK94");
				p.i32("UNK98");
				p.u8("UNK138");
				p.i16("UNK134");
				p.floats("TIMESTAMP", 2);
				p.ref("UNK120");
				const auto n = p.vs("");
				p.countedArray("REFS", n, [&] { p.ref("REF"); });
				const auto m = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("PAIRS", m, [&] {
					p.ref("REF1");
					p.ref("REF2");
				});
				p.ref("UNKF0");
				p.ref("UNKF8");
				p.u8("UNK139");
				p.ref("UNK128");
				if (a_level == 3) {
					return;
				}
				p.f32("CACHED VALUES");  // level 2 (middle-low) ends here
				if (a_level == 2) {
					return;
				}
				p.group("MIDDLE HIGH", [&] { MiddleHighProcess(p); });
				if (a_level == 0) {
					p.group("HIGH", [&] { HighProcess(p); });
				}
				if (p.version > 0x3d) {
					p.u8("UNK332");
				}
			});
		}

		// ---- MovementControllerNPC (through the pathing stream) ----
		static const char* kArbiters[] = { "MovementHandlerArbiter", "MovementPathManagerArbiter", "MovementPlannerArbiter",
			"MovementPostUpdateArbiter", "MovementTweenerArbiter" };

		const char* AgentName(std::uint8_t a_id)
		{
			switch (a_id) {
			case 0x00: return "MovementStateFollowPath";
			case 0x01: return "MovementStateTweener";
			case 0x02: return "MovementHandlerAgentAngleController";
			case 0x03: return "MovementHandlerAgentAngleGain";
			case 0x04: return "MovementHandlerAgentAnglePassThrough";
			case 0x05: return "MovementHandlerAgentDirectionPassThrough";
			case 0x06: return "MovementHandlerAgentLargeDeltaIdle";
			case 0x07: return "MovementHandlerAgentSpeedPassThrough";
			case 0x08: return "MovementHandlerAgentStrafing";
			case 0x09: return "MovementHandlerAgentTranslationController";
			case 0x0A: return "MovementAgentActorState";
			case 0x0B: return "MovementAgentActorAvoider";
			case 0x0C: return "MovementAgentPathFollowerFlight";
			case 0x0D: return "MovementAgentPathFollowerStandard";
			case 0x0E: return "MovementAgentPathFollowerVirtual";
			case 0x0F: return "MovementPlannerAgentNavmeshBounds";
			case 0x10: return "MovementPathManagerAgentAvoidBox";
			case 0x11: return "MovementPathManagerAgentLoadedAreaMonitor";
			case 0x12: return "MovementPathManagerAgentStaticAvoider";
			case 0x13: return "MovementTweenerAgentFixedDelta";
			case 0x80: return "MovementHandlerAgentDirectControl";
			case 0x81: return "MovementHandlerAgentPlayerControls";
			case 0x82: return "MovementHandlerAgentPlayerControlsActionTrigger";
			case 0x83: return "MovementHandlerAgentSprintActionTrigger";
			case 0x84: return "MovementHandlerAgentStairsHelper";
			case 0x85: return "MovementPlannerAgentDirectControl";
			case 0x86: return "MovementPlannerAgentHorseControls";
			case 0x87: return "MovementPlannerAgentKeepOffset";
			case 0x88: return "MovementPlannerAgentWarp";
			case 0x89: return "MovementTweenerAgentAnimationDriven";
			case 0x8A: return "MovementTweenerAgentNodeFollower";
			default: return nullptr;
			}
		}

		void Stream18(P& p, const char* a_label);
		// PathingDoor, BSPathingDoor
		bool PathingDoorClass(std::uint32_t a_cls) { return a_cls == 0xE48B73F3 || a_cls == 0x72BFDFEF; }

		// a path (the 0x70 kind of shared pathing object): ID; the first time, class ID + data
		void PathObject(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const std::uint64_t id = p.version >= 0x4b ? p.r.u64() : p.r.u32();
				p.add("ID", Fmt("%llX", static_cast<unsigned long long>(id)));
				if (id == 0 || !p.pathIDs.insert(id).second) {
					return;
				}
				p.add("CLASS", Fmt("%08X", p.r.u32()));
				const auto n1 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("NODES", n1, [&] {
					PathingLocation(p, "LOCATION");
					Stream18(p, "EDGE");
				});
				p.i32("UNK88");
				p.i32("UNK8C");
				const auto n2 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("POINTS", n2, [&] {
					PathingLocation(p, "LOCATION");
					p.floats("VALUES", 3);
					Stream18(p, "EDGE");
				});
				p.u8("UNKA8");
				p.u8("UNKA9");
				const auto n3 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("POINTS2", n3, [&] {
					PathingLocation(p, "LOCATION");
					p.floats("VALUES", 3);
					Stream18(p, "EDGE");
				});
			});
		}

		void MovementArbiter(P& p, const std::string& a_name)
		{
			auto tweenerEntry = [&] { p.floats("VALUES", 13); };
			if (a_name == "MovementHandlerArbiter") {
			} else if (a_name == "MovementPathManagerArbiter") {
				p.i32("STATE");
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
			} else if (a_name == "MovementPlannerArbiter") {
				p.u8("UNK24");
			} else if (a_name == "MovementPostUpdateArbiter") {
				p.floats("VALUES", 14);
			} else if (a_name == "MovementTweenerArbiter") {
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ENTRIES", n, [&] { tweenerEntry(); });
				p.group("UNKD4", [&] {
					p.i32("UNK");
					p.floats("VALUES", 33);
				});
				p.floats("A", 13);
				p.floats("B", 13);
				p.floats("C", 13);
				p.floats("VALUES", 4);
			} else {
				throw Stop("movement arbiter '" + a_name + "' is not decoded yet");
			}
		}

		void MovementAgent(P& p, const std::string& a_name)
		{
			auto pointList = [&](const char* a_l) {
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray(a_l, n, [&] { p.floats("POINT", 3); });
				p.i32("UNK");
			};
			static const std::set<std::string> kEmpty = { "MovementHandlerAgentDirectControl", "MovementHandlerAgentPlayerControlsActionTrigger",
				"MovementHandlerAgentStairsHelper", "MovementPlannerAgentDirectControl", "MovementPlannerAgentHorseControls",
				"MovementHandlerAgentAngleGain", "MovementHandlerAgentSpeedPassThrough", "MovementHandlerAgentAnglePassThrough",
				"MovementHandlerAgentDirectionPassThrough", "MovementHandlerAgentStrafing", "MovementStateTweener", "MovementStateFollowPath",
				"MovementHandlerAgentAngleController", "MovementTweenerAgentNodeFollower" };
			if (kEmpty.count(a_name)) {
				return;
			}
			if (a_name == "MovementHandlerAgentPlayerControls") {
				p.i32("UNK38");
				p.f32("UNK48");
				p.u8("UNK4C");
			} else if (a_name == "MovementHandlerAgentSprintActionTrigger") {
				if (p.version >= 0x1d) {
					p.i32("UNK38");
				}
				p.u8("UNK3C");
				p.u8("UNK3D");
			} else if (a_name == "MovementPlannerAgentKeepOffset") {
				p.ref("TARGET");
				p.floats("OFFSET", 8);
			} else if (a_name == "MovementPlannerAgentWarp") {
				PathingLocation(p, "FROM");
				PathingLocation(p, "TO");
				p.f32("UNK88");
				p.ref("REF");
				p.i32("STATE");
				p.i32("UNKB4");
			} else if (a_name == "MovementTweenerAgentAnimationDriven") {
				p.i32("STATE");
			} else if (a_name == "MovementAgentActorState") {
				p.group("STATE", [&] {
					p.i32("UNK0");
					p.floats("VALUES", 5);
				});
				p.f32("UNK1C");
				if (p.version > 0x1a) {
					p.f32("UNK20");
				}
				p.i32("UNK58");
				p.i32("UNK70");
			} else if (a_name == "MovementAgentActorAvoider") {
				PathingObject(p, "REQUEST");
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ACTORS", n, [&] {
					p.ref("ACTOR");
					p.floats("VALUES", 12);
					p.i32("UNK24");
					p.i32("UNK28");
					p.u8("UNK3C");
				});
				p.i32("UNK60");
				p.i32("UNK64");
				p.i32("UNK68");
			} else if (a_name == "MovementHandlerAgentTranslationController") {
				p.u8("UNK28");
				p.i32("UNK38");
			} else if (a_name == "MovementTweenerAgentFixedDelta") {
				p.floats("VALUES", 31);
			} else if (a_name == "MovementPathManagerAgentLoadedAreaMonitor") {
				PathObject(p, "PATH");
			} else if (a_name == "MovementHandlerAgentLargeDeltaIdle") {
				p.i32("UNK28");
			} else if (a_name == "MovementPathManagerAgentAvoidBox") {
				p.i32("STATE");
				p.f32("UNK34");
				PathingLocation(p, "LOCATION");
				p.floats("VALUES", 3);
				p.u8("UNKA8");
				const auto door = p.r.u32();
				if (door != 0) {
					if (!PathingDoorClass(door)) {
						throw Stop(Fmt("pathing door class %08X is not decoded yet", door));
					}
					p.ref("DOOR");
				}
				PolyMember(p, "LOCK", "pathing avoid");
			} else if (a_name == "MovementPathManagerAgentStaticAvoider") {
				if (p.version >= 0x1a) {
					p.f32("UNK18");
				}
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("BOXES", n, [&] { p.floats("VALUES", 13); });
				p.floats("UNK1C", 5);
				const auto m = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("SEGMENTS", m, [&] { p.ints("VALUES", 9); });
				PathingObject(p, "REQUEST");
				p.f32("UNKB0");
				p.f32("UNKB4");
				p.i32("UNKB8");
			} else if (a_name == "MovementAgentPathFollowerStandard") {
				p.i32("STATE");
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
				p.floats("VALUES", 7);
				p.add("PATHING STATE ID", Fmt("%llX", static_cast<unsigned long long>(p.version >= 0x4b ? p.r.u64() : p.r.u32())));
				p.floats("VALUES2", 3);
				p.i32("UNK15C");
				p.i32("UNK160");
				p.f32("PARAMETER REACHED");
				p.f32("GOAL REACHED");
				p.group("PLAY IDLE", [&] {
					if (p.u8("HAS IDLE")) {
						p.add("IDLE", p.r.wstring());
					}
					p.i32("UNK10");
				});
				p.group("FOLLOW PATH TO PARAMETER", [&] {
					p.floats("VALUES", 3);
					p.i32("INDEX");
				});
				p.group("TURN TO ANGLE", [&] {
					p.i32("UNK08");
					p.floats("VALUES", 5);
				});
				p.group("KEEP LAST DIRECTION", [&] {
					p.floats("VALUES", 4);
					p.i32("INDEX");
				});
			} else if (a_name == "MovementPlannerAgentNavmeshBounds") {
				p.i32("STATE");
				p.f32("UNK44");
				p.f32("UNK48");
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("BOUNDS", n, [&] {
					p.floats("BOX", 6);
					const auto m = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray("POINTS", m, [&] { p.floats("POINT", 3); });
					p.i32("UNKA0");
				});
				PathObject(p, "PATH");
			} else if (a_name == "MovementAgentPathFollowerFlight") {
				p.i32("STATE");
				p.floats("VALUES", 5);
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
				p.f32("UNK3C");
				p.i32("UNK58");
				p.i32("UNK5C");
				pointList("POINTS1");
				pointList("POINTS2");
			} else if (a_name == "MovementAgentPathFollowerVirtual") {
				p.i32("STATE");
				PathingObject(p, "REQUEST");
				PathObject(p, "PATH");
			} else {
				throw Stop("movement agent '" + a_name + "' is not decoded yet");
			}
		}

		// members of the movement/path family written as class ID + data (class IDs: CRC32 of names)
		void Stream18(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				if (cls == 0) {
					return;
				}
				auto movementType = [&](const char* a_l) {
					p.group(a_l, [&] {
						p.add("NAME", p.r.wstring());
						p.floats("SPEEDS", 10);
						p.floats("ROTATION", 4);
					});
				};
				switch (cls) {
				case 0x5CC2A237:  // MovementMessageActivateDoor
				case 0x7663F86A:  // MovementMessageApproachingDoor
				case 0x2D8AD535:  // MovementMessageDoor
					p.add("MESSAGE", "Door");
					if (p.u8("HAS DOOR")) {
						const auto door = p.r.u32();
						if (door == 0x00000000) {
						} else if (PathingDoorClass(door)) {
							p.ref("DOOR");
						} else {
							throw Stop(Fmt("pathing door class %08X is not decoded yet", door));
						}
					}
					break;
				case 0xCDED4F63:  // MovementMessageNewPath
				case 0x0F13E00F:  // MovementMessagePathCleared
				case 0xD7578F99:  // MovementMessagePathComplete
				case 0xABBC80DC:  // MovementMessagePathEvent
				case 0x8BCEF6C4:  // MovementMessagePathFailed
					p.add("MESSAGE", "Path");
					PathingObject(p, "REQUEST");
					break;
				case 0xC8B4153E:  // MovementMessageBlocked
				case 0x77A37BFA:  // MovementMessageActorCollision
					break;
				case 0x3BED430B:  // MovementMessageFreezeDirection
					p.i32("FREEZE");
					break;
				case 0x00DC870E:  // MovementMessageJump
					p.f32("HEIGHT");
					break;
				case 0x88058586:  // MovementMessageMovementTypeChange
					movementType("FROM");
					movementType("TO");
					break;
				case 0x616653D5:  // MovementMessageSetStaticPath
					p.u8("STATIC");
					break;
				case 0x67DA9023:  // MovementMessageWarpToLocation
					PathingLocation(p, "LOCATION");
					p.floats("ANGLES", 3);
					break;
				default:
					throw Stop(Fmt("movement object class %08X is not decoded yet", cls));
				}
			});
		}

		void MovementController(P& p)
		{
			p.group("MOVEMENT CONTROLLER", [&] {
				const auto n1 = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("MESSAGES", n1, [&] { Stream18(p, "MESSAGE"); });
				if (p.version > 0x38) {
					const auto n2 = static_cast<std::uint32_t>(p.r.i32());
					p.countedArray("LOCATIONS", n2, [&] {
						PathingLocation(p, "LOCATION");
						p.floats("VALUES", 3);
						p.i32("UNK");
					});
				}
				p.i32("STATE");
				p.bytes("FLAGS", 7);
				const auto na = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ARBITERS", na, [&] {
					std::string name;
					if (p.version < 0x36) {
						name = p.r.wstring();
					} else {
						const auto id = p.r.u8();
						name = id < 5 ? kArbiters[id] : Fmt("arbiter %u", id);
					}
					p.group(name, [&] { MovementArbiter(p, name); });
				});
				const auto ng = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("AGENTS", ng, [&] {
					std::string name;
					if (p.version < 0x36) {
						name = p.r.wstring();
					} else {
						const auto id = p.r.u8();
						const auto* nm = AgentName(id);
						name = nm ? nm : Fmt("agent %u", id);
					}
					p.group(name, [&] { MovementAgent(p, name); });
				});
				if (p.u8("HAS IDLE DATA")) {
					AnimationFiles(p, "ANIMATIONS");
					if (p.u8("HAS IDLE")) {
						p.group("PLAY IDLE", [&] {
							p.add("IDLE", p.r.wstring());
							p.i32("UNK");
							p.floats("VALUES", 33);
							p.add("PATHING STATE ID", Fmt("%llX", static_cast<unsigned long long>(p.version >= 0x4b ? p.r.u64() : p.r.u32())));
						});
					}
				}
			});
		}

		void ActorData(P& p, std::uint8_t a_version, std::int32_t a_processLevel)
		{
			if (a_processLevel != -1) {
				AIProcess(p, a_processLevel);
			}
			p.group("ACTOR", [&] {
				p.u8("UNK_278");
				p.f32("UPDATE TARGET TIMER");
				p.f32("VOICE TIMER");
				p.f32("LAST UPDATE (AGE)");
				p.f32("CHECK DEAD BODY TIMER");
				p.i32("ACTION VALUE");
				p.f32("TIMER ON ACTION");
				p.i32("UNK_11C");
				p.i32("EMOTION TYPE");
				p.i32("EMOTION VALUE");
				p.i32("CRITICAL STAGE");
				p.i32("THIEF CRIME STAMP");
				p.i32("LAST SEEN TIME");
				p.f32("BASE HEIGHT");
				p.ref("KILLER");
				p.group("ACTOR STATE", [&] { p.bytes("BITS", 18); });
				p.ref("UNK HANDLE");
				const auto mask = p.u8("MAGIC CASTERS");
				static const char* kSlots[4] = { "LEFT HAND CASTER", "RIGHT HAND CASTER", "OTHER CASTER", "INSTANT CASTER" };
				for (int i = 0; i < 4; ++i) {
					if (mask & (1u << i)) {
						p.group(kSlots[i], [&] { ActorCaster(p); });
					}
				}
				p.ref("SELECTED SPELL LEFT");
				p.ref("SELECTED SPELL RIGHT");
				p.ref("SELECTED SPELL OTHER");
				p.ref("SELECTED SPELL INSTANT");
				p.ref("SELECTED POWER");
				ActorValueModifiers(p, "HEALTH MODIFIERS");
				ActorValueModifiers(p, "MAGICKA MODIFIERS");
				ActorValueModifiers(p, "STAMINA MODIFIERS");
				ActorValueModifiers(p, "VOICE MODIFIERS");
				p.group("ACTOR VALUES", [&] {
					p.vsArray("BASE", [&] {
						p.i32("AV");
						p.f32("VALUE");
					});
					p.vsArray("MODIFIERS", [&] {
						p.i32("AV");
						p.floats("PERMANENT, TEMPORARY, DAMAGE", 3);
					});
				});
				const auto spells = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("ADDED SPELLS", spells, [&] { p.ref("SPELL"); });
				if (p.u8("HAS MOVEMENT CONTROLLER")) {
					MovementController(p);
				}
				p.flags32("BOOL FLAGS");
				if (p.u8("HAS COMBAT CONTROLLER")) {
					CombatController(p);
				}
				const auto n = static_cast<std::uint32_t>(p.r.i32());
				p.countedArray("UNK LIST", n, [&] {
					p.ref("REF");
					p.i32("VALUE");
				});
				p.i32("INTIMIDATE / BRIBE DAY");
				if (a_version > 0x27) {
					p.f32("UNDERWATER TIMER");
				}
				if (a_version > 0x2a) {
					p.ref("EXCLUSIVE BRANCH");
				}
				if (a_version > 0x2c) {
					p.u8("UNK_27B");
				}
			});
		}

		// CombatInventoryItemMagicT<item, caster>: the game files each pairing under the XOR of the
		// three name CRCs; every pairing loads like CombatInventoryItemMagic
		constexpr std::uint32_t kMagicItemPairings[] = {
			0x03ADDFBA, 0x091A0CDA, 0x09B2C33F, 0x0A158F4F, 0x0A90173E, 0x0BC670CF,
			0x12C735BF, 0x1448419A, 0x15D7FD71, 0x1A294A22, 0x1B0C8C0D, 0x25B25C08,
			0x2A4CEB5B, 0x2B692D74, 0x2CC9133E, 0x37F333C8, 0x397CB368, 0x397FADA3,
			0x3A702E36, 0x3AF5B647, 0x3D66C842, 0x40AE6A73, 0x42721666, 0x42DAD983,
			0x4AE38186, 0x506496B1, 0x5141509E, 0x59AF2F03, 0x5D061FA6, 0x63CD085E,
			0x67D77374, 0x6958F3D4, 0x69CA8E7A, 0x6EDA46B4, 0x70CBCB0A, 0x7118348A,
			0x719DACFB, 0x7262D322, 0x72BF78FA, 0x81AA2A4D, 0x89C18538, 0x8B689D10,
			0x8BB536C8, 0x8CC8A35A, 0x8DED6575, 0x90C0C048, 0x964FB46D, 0x9EDD3D46,
			0x9EDE238D, 0xAD319910, 0xADB40161, 0xAEB89C3F, 0xB276735F, 0xB5F4C63F,
			0xBB0D3C69, 0xBF613DB5, 0xC075E391, 0xC46C8670, 0xCAC230F1, 0xCE6B0054,
			0xCEF97DFA, 0xD2636346, 0xD346A569, 0xD51EF6D4, 0xD76A4524, 0xDD27AED1,
			0xE5D08683, 0xECDDB343, 0xED420FA8, 0xF06526D5, 0xF31FC17D, 0xF39A590C,
			0xF4092709, 0xFAA79188, 0xFE9CDC83,
		};

		// A combat object (CombatObject subclass), written by reference: a u32 ID where 0 is none, an
		// even ID points at an object written earlier and an odd ID is followed by the class and data
		void CombatObject(P& p, const char* a_label)
		{
			p.group(a_label, [&] {
				const auto id = p.r.u32();
				p.add("ID", Fmt("%X", id));
				if ((id & 1) == 0) {
					return;
				}
				const auto cls = p.r.u32();
				p.add("CLASS", Fmt("%08X", cls));
				CombatObjectData(p, cls);
			});
		}

		void CombatObjectData(P& p, std::uint32_t a_cls)
		{
			const auto aimBase = [&] {
				p.floats("POS", 3);
				p.ref("REF");
				p.i32("UNK34");
				p.i32("UNK38");
				p.i32("UNK3C");
				p.f32("TIMER");
				p.i32("UNK44");
			};
			const auto areaBase = [&] {
				p.ref("REF");
				p.i32("UNK28");
				p.u8("UNK2C");
				p.u8("UNK2D");
			};
			const auto itemBase = [&] {
				p.ref("ITEM");
				p.i32("UNK18");
				p.ref("SOURCE");
				p.i32("UNK28");
			};
			const auto areaLocation = [&] {
				p.bytes("UNK20", 8);
				p.floats("POS", 3);
				p.ref("CELL");
			};
			switch (a_cls) {
			case 0xC8F87A78:  // CombatObject
			case 0x860CC21F:  // CombatAimControllerBase
			case 0xE588E90D:  // CombatPathSpeedController
				break;
			case 0x86D330A0:  // CombatAimController
			case 0xBE236AC9:  // CombatDisableAimController
				aimBase();
				break;
			case 0x64927B7C:  // CombatMeleeAimController
				aimBase();
				p.i32("UNK48");
				p.u8("UNK4C");
				p.i32("UNK50");
				break;
			case 0x5AD9B53F:  // CombatTrackTargetAimController
				aimBase();
				p.i32("UNK48");
				p.u8("UNK4C");
				break;
			case 0xF06E7A91:  // CombatProjectileAimController
				aimBase();
				p.ref("PROJECTILE");
				p.ints("UNK50", 3);
				p.floats("VECTORS", 15);
				p.ints("UNK98", 3);
				p.f32("UNKB0");
				p.floats("UNKB4", 3);
				p.f32("UNKC0");
				p.wstring("NODE");
				break;
			case 0x3888CDCA:  // CombatArea
				areaBase();
				p.f32("TIMER");
				p.i32("UNK24");
				break;
			case 0xA9BDAAFE:  // CombatAreaStandard
			case 0x2E71F18B:  // CombatAreaRadius
				areaBase();
				areaLocation();
				p.i32("UNK48");
				break;
			case 0x55FAB455:  // CombatAreaHoldPosition
			case 0x55C1257B:  // CombatAreaReference
				areaBase();
				areaLocation();
				p.i32("UNK48");
				p.ref("REF2");
				p.f32("TIMER");
				p.i32("UNK58");
				break;
			case 0xD6E95B87:  // CombatTargetSelector
			case 0xF7B17BBC:  // CombatTargetSelectorFixed
				p.ref("TARGET");
				p.i32("UNK1C");
				p.i32("UNK20");
				break;
			case 0xC3719B85:  // CombatTargetSelectorStandard
				p.ref("TARGET");
				p.i32("UNK1C");
				p.i32("UNK20");
				p.f32("TIMER");
				p.i32("UNK2C");
				break;
			case 0x1F7409DD:  // CombatInventoryItem
				itemBase();
				break;
			case 0x4C526266:  // CombatInventoryItemComposite
				itemBase();
				p.ref("REF");
				break;
			case 0x9C2A0B47:  // CombatInventoryItemMagic
			case 0xAC4FAA3E:  // CombatInventoryItemPotion
			case 0x0BEE2410:  // CombatInventoryItemScroll
			case 0x5545E40C:  // CombatInventoryItemShout
			case 0xD74211FB:  // CombatInventoryItemStaff
				itemBase();
				p.ints("UNK30", 5);
				p.i32("EFFECT INDEX");
				break;
			case 0x5ACB99C8:  // CombatInventoryItemMelee
				itemBase();
				p.i32("UNK30");
				break;
			case 0x8121581D:  // CombatInventoryItemOneHandedBlock
			case 0xF86AC87B:  // CombatInventoryItemShield
			case 0x66D5D91E:  // CombatInventoryItemTorch
				itemBase();
				if (p.version > 0x20) {
					p.i32("UNK30");
				}
				break;
			case 0x71B72E31:  // CombatInventoryItemRanged
				itemBase();
				p.ref("AMMO");
				p.i32("UNK38");
				p.i32("UNK3C");
				break;
			case 0x51B14DE6:  // CombatMagicCaster
			case 0x11126C48:  // CombatMagicCasterArmor
			case 0x3DBA3C9A:  // CombatMagicCasterBoundItem
			case 0x22784EA4:  // CombatMagicCasterCloak
			case 0xB6B0FCAD:  // CombatMagicCasterDisarm
			case 0xA3055CFB:  // CombatMagicCasterInvisibility
			case 0xC52FCE43:  // CombatMagicCasterLight
			case 0xEC01479B:  // CombatMagicCasterParalyze
			case 0x02212AB0:  // CombatMagicCasterScript
			case 0xE694BC11:  // CombatMagicCasterStagger
			case 0xD2CA5063:  // CombatMagicCasterSummon
			case 0x5AD2B911:  // CombatMagicCasterTargetEffect
				CombatObject(p, "ITEM");
				break;
			case 0x41A74F91:  // CombatMagicCasterOffensive
				CombatObject(p, "ITEM");
				p.bytes("UNK20", 8);
				p.f32("TIMER");
				p.i32("UNK2C");
				break;
			case 0x0304EC9F:  // CombatMagicCasterReanimate
				CombatObject(p, "ITEM");
				CombatObject(p, "OBJECT");
				p.ref("TARGET");
				break;
			case 0x58A60AE1:  // CombatMagicCasterRestore
				CombatObject(p, "ITEM");
				p.f32("UNK20");
				p.i32("UNK24");
				p.i32("UNK28");
				break;
			case 0x22FDD6D5:  // CombatMagicCasterWard
				CombatObject(p, "ITEM");
				p.f32("TIMER");
				p.i32("UNK24");
				break;
			default:
				if (std::find(std::begin(kMagicItemPairings), std::end(kMagicItemPairings), a_cls) !=
					std::end(kMagicItemPairings)) {
					itemBase();
					p.ints("UNK30", 5);
					p.i32("EFFECT INDEX");
					break;
				}
				throw Stop(Fmt("combat object class %08X is not decoded yet", a_cls));
			}
		}

		void CombatObjects(P& p, const char* a_label)
		{
			const auto n = p.vs("COUNT");
			p.countedArray(a_label, n, [&] { CombatObject(p, "OBJECT"); });
		}

		void CombatController(P& p)
		{
			p.group("COMBAT CONTROLLER", [&] {
				p.ref("ATTACKER");
				p.ref("TARGET");
				p.i32("COMBAT GROUP");
				p.bytes("FLAGS", 6);
				p.i32("UNK4C");
				p.bytes("UNK44", 8);
				CombatObjects(p, "AIM CONTROLLERS");
				p.i32("CURRENT AIM CONTROLLER");
				p.i32("PREVIOUS AIM CONTROLLER");
				CombatObjects(p, "AREAS");
				p.i32("CURRENT AREA");
				CombatObjects(p, "TARGET SELECTORS");
				CombatObject(p, "CURRENT TARGET SELECTOR");
				CombatObject(p, "PREVIOUS TARGET SELECTOR");
				p.group("STATE", [&] {
					p.bytes("FLAGS", 6);
					p.i32("UNK0C");
					p.bytes("UNK10", 12);
					p.i32("UNK4C");
					p.u8("UNK6C");
					p.u8("UNK6D");
					p.ints("UNK70", 3);
					p.i32("UNK00");
					p.u8("UNK04");
					p.floats("UNK1C", 4);
					p.bytes("TIMERS", 32);
					const auto n = p.vs("COUNT");
					p.countedArray("POSITIONS", n, [&] {
						p.floats("POS", 3);
						p.ref("CELL");
						p.f32("TIME");
					});
					const auto m = p.vs("COUNT");
					p.countedArray("UNK50", m, [&] { p.ref("REF"); });
					p.u8("UNKA0");
					p.bytes("UNKAC", 8);
					p.bytes("UNKA4", 8);
					p.bytes("UNKB4", 8);
					p.f32("UNK68");
				});
				p.group("INVENTORY", [&] {
					p.ints("UNK19C", 10);
					p.u8("UNK1C4");
					p.i32("UNK190");
					p.bytes("UNK194", 8);
					for (int i = 0; i < 7; ++i) {
						CombatObjects(p, "ITEMS");
					}
					for (int i = 0; i < 2; ++i) {
						p.group("ITEM LIST", [&] {
							CombatObjects(p, "ITEMS");
							p.ints("UNK", 4);
							p.i32("UNK28");
						});
					}
					const auto n = p.vs("COUNT");
					p.countedArray("RATED ITEMS", n, [&] {
						CombatObject(p, "ITEM");
						p.f32("RATING");
					});
					const auto a = p.vs("COUNT");
					p.countedArray("REFS1", a, [&] { p.ref("REF"); });
					const auto b = p.vs("COUNT");
					p.countedArray("REFS2", b, [&] { p.ref("REF"); });
					const auto c = p.vs("COUNT");
					p.countedArray("COUNTS", c, [&] {
						p.ref("REF");
						p.i32("COUNT");
					});
				});
				p.group("BLACKBOARD", [&] {
					const auto n = p.vs("COUNT");
					p.countedArray("VALUES", n, [&] {
						const auto name = p.r.wstring();
						p.group(name, [&] {
							// the game's six blackboard keys and their value types
							if (name == "TargetUnreachableDistance") {
								p.f32("VALUE");
							} else if (name == "AcquireWeaponTimer") {
								p.f32("TIMER");
								p.f32("INTERVAL");
							} else if (name == "CombatDisarmedTimeStamp" || name == "CoverSearchFailedTimeStamp" ||
									   name == "FindAttackLocationFailedTimeStamp") {
								p.f32("TIME STAMP");
							} else if (name == "LastFleeDoor") {
								p.ref("DOOR");
								p.f32("TIME STAMP");
							} else {
								throw Stop("combat blackboard key '" + name + "' is not decoded yet");
							}
						});
					});
					p.i32("UNK40");
				});
				p.group("BEHAVIOR CONTROLLER", [&] {
					p.i32("UNK70");
					if (p.u8("HAS TREE")) {
						// the running behavior tree is written with its size up front
						const auto size = p.vs("TREE SIZE");
						p.bytes("TREE STATE", size);
					}
					const auto n = p.vs("COUNT");
					p.countedArray("THREAD IDS", n, [&] { p.i32("ID"); });
					const auto m = p.vs("COUNT");
					p.countedArray("NAMED", m, [&] {
						p.wstring("NAME");
						p.i32("THREAD");
						const auto a = p.vs("COUNT");
						p.countedArray("THREADS", a, [&] { p.i32("ID"); });
						const auto b = p.vs("COUNT");
						p.countedArray("PAIRS", b, [&] {
							p.i32("UNK");
							p.i32("THREAD");
						});
					});
				});
			});
		}

		void Achr(P& p, const RefID& id, std::uint32_t flags, std::uint8_t a_version)
		{
			const int t = InitialType(id, flags);
			Initial(p, t);
			Havok(p, flags);
			const auto level = p.i32("PROCESS LEVEL");
			p.flags32("BOOL BITS");
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 7)) {
				p.out().baseObject = p.refResolved("BASE_OBJECT");
			}
			if (Bit(flags, 4)) {
				p.f32("SCALE");
			}
			for (int b : { 6, 25, 9, 11, 17, 26, 29, 30, 31, 18 }) {
				if (Bit(flags, b)) {
					ExtraData(p);
					break;
				}
			}
			if (Bit(flags, 5) || Bit(flags, 27)) {
				Inventory(p);
			}
			if (Bit(flags, 28)) {
				p.bytesVS("ANIMATIONS");
			}
			ActorData(p, a_version, level);
		}

		void NPC(P& p, std::uint32_t flags, bool)
		{
			if (Bit(flags, 0)) {
				p.section(0, [&] { p.changeFormFlags(); });
			}
			if (Bit(flags, 1)) {
				p.section(1, [&] { p.bytes("ACBS", 24); });
			}
			if (Bit(flags, 6)) {
				p.section(6, [&] {
					p.vsArray("FACTIONRANKS", [&] {
						p.ref("FACTION");
						p.u8("RANK");
					});
				});
			}
			if (Bit(flags, 4)) {
				p.section(4, [&] {
					p.vsArray("SPELLS", [&] { p.ref("SPELL"); });
					p.vsArray("SPELLS_LEVELLED", [&] { p.ref("SPELL"); });
					p.vsArray("SHOUTS", [&] { p.ref("SHOUT"); });
				});
			}
			if (Bit(flags, 3)) {
				p.section(3, [&] { p.bytes("AIDT", 20); });
			}
			if (Bit(flags, 5)) {
				p.section(5, [&] { p.wstring("FULLNAME"); });
			}
			if (Bit(flags, 9)) {
				p.section(9, [&] { p.bytes("DNAM", 52); });
			}
			if (Bit(flags, 10)) {
				p.section(10, [&] { p.ref("CLASS"); });
			}
			if (Bit(flags, 25)) {
				p.section(25, [&] {
					p.ref("RACE");
					p.ref("OLDRACE");
				});
			}
			if (Bit(flags, 11)) {
				p.section(11, [&] {
					p.group("FACEDATA", [&] {
						if (p.u8("FACEPRESENT") != 0) {
							p.ref("HAIRCOLOR");
							p.i32("SKINTONE");
							p.ref("SKIN");
							p.vsArray("HEADPARTS", [&] { p.ref("PART"); });
							if (p.u8("FACEDATAPRESENT") != 0) {
								const auto m = p.i32("MORPHS_COUNT");
								p.floats("MORPHS", static_cast<std::size_t>(m));
								const auto pr = p.i32("PRESETS_COUNT");
								p.ints("PRESETS", static_cast<std::size_t>(pr));
							}
						}
					});
				});
			}
			if (Bit(flags, 24)) {
				p.section(24, [&] { p.u8("GENDER"); });
			}
			if (Bit(flags, 12)) {
				p.section(12, [&] { p.ref("DEFAULT_OUTFIT"); });
			}
			if (Bit(flags, 13)) {
				p.section(13, [&] { p.ref("SLEEP_OUTFIT"); });
			}
		}

		void Qust(P& p, std::uint32_t flags)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 1)) {
				p.flags16("QUEST_FLAGS");
			}
			if (Bit(flags, 2)) {
				p.f32("SCRIPT_DELAY");
			}
			if (Bit(flags, 31)) {
				p.vsArray("QUEST_STAGES", [&] {
					p.i16("STAGE");
					p.flags8("STATUS");
				});
			}
			if (Bit(flags, 29)) {
				p.vsArray("QUEST_OBJECTIVES", [&] {
					p.i32("UNK1");
					p.i32("UNK2");
				});
			}
			if (Bit(flags, 28)) {
				p.group("QUEST_RUN_DATA", [&] {
					p.u8("UNK");
					const auto c1 = static_cast<std::uint32_t>(p.i32("COUNT1"));
					p.countedArray("ITEMS1", c1, [&] {
						p.i32("UNK");
						const auto f = p.flags8("FLAG");
						for (int i = 0; i < (f == 0 ? 1 : 5); ++i) {
							p.ref(Fmt("REF%d", i));
						}
					});
					const auto c2 = static_cast<std::uint32_t>(p.i32("COUNT2"));
					p.countedArray("ITEMS2", c2, [&] {
						p.i32("UNK");
						p.ref("REF");
					});
					if (p.flags8("FLAG") != 0) {
						p.group("ITEM3", [&] {
							p.i32("UNK1");
							p.f32("UNK2");
							const auto n = static_cast<std::uint32_t>(p.i32("COUNT"));
							p.countedArray("ITEMS", n, [&] {
								const auto t = p.i32("TYPE");
								if (t == 1 || t == 2 || t == 4) {
									p.ref("UNK_REF");
								} else if (t == 3) {
									p.i32("UNK_INT");
								} else {
									throw Stop(Fmt("unexpected quest run data type %d", t));
								}
							});
						});
					}
				});
			}
			if (Bit(flags, 26)) {
				p.u8("ALREADY_RUN");
			}
		}

		void Leveled(P& p, std::uint32_t flags)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 31)) {
				const auto n = p.r.u8();
				p.countedArray("ENTRIES", n, [&] {
					p.ref("REF");
					p.u8("LEVEL");
					p.i16("COUNT");
					p.u8("CHANCE");
				});
			}
		}

		void Rela(P& p, const RefID& id, std::uint32_t flags)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (id.type() == RefID::Type::kCreated) {
				p.ref("PERSON1");
				p.ref("PERSON2");
				p.ref("ASSOCIATION");
			}
			if (Bit(flags, 1)) {
				p.i32("RANK");
			}
		}

		// FLST: ReSaver reads the CHANGE_FORM_FLAGS block when bit 1 is set; the game's own constant
		// is bit 0. Both are tried, and only an interpretation that uses every byte is accepted.
		bool Flst(P& p, std::uint32_t flags, int formFlagsBit)
		{
			const auto start = p.r.pos();
			const auto mark = p.mark();
			try {
				if (Bit(flags, formFlagsBit)) {
					p.changeFormFlags();
				}
				p.out().listRaw.clear();
				if (Bit(flags, 31)) {
					const auto n = p.r.u32();
					if (n > 0x3FFF) {
						throw Stop("implausible form list size");
					}
					p.countedArray("FORMS", n, [&] { p.out().listRaw.push_back(p.ref("FORM")); });
				}
				if (p.r.remaining() == 0) {
					return true;
				}
			} catch (const std::exception&) {
			}
			p.r.seek(start);
			p.rollback(mark);
			return false;
		}

		const char* kRefrFlags[32] = { "CHANGE_FORM_FLAGS", "REFR_MOVE", "REFR_HAVOK_MOVE", "REFR_CELL_CHANGED", "REFR_SCALE", "REFR_INVENTORY",
			"REFR_EXTRA_OWNERSHIP", "REFR_BASEOBJECT", nullptr, nullptr, "OBJECT_EXTRA_ITEM_DATA", "OBJECT_EXTRA_AMMO", "OBJECT_EXTRA_LOCK", nullptr,
			nullptr, nullptr, nullptr, "DOOR_EXTRA_TELEPORT", nullptr, nullptr, nullptr, "OBJECT_EMPTY", "OBJECT_OPEN_DEFAULT_STATE", "OBJECT_OPEN_STATE",
			nullptr, "REFR_PROMOTED", "REFR_EXTRA_ACTIVATING_CHILDREN", "REFR_LEVELED_INVENTORY", "REFR_ANIMATION", "REFR_EXTRA_ENCOUNTER_ZONE",
			"REFR_EXTRA_CREATED_ONLY", "REFR_EXTRA_GAME_ONLY" };
		const char* kAchrFlags[32] = { "CHANGE_FORM_FLAGS", "REFR_MOVE", "REFR_HAVOK_MOVE", "REFR_CELL_CHANGED", "REFR_SCALE", "REFR_INVENTORY",
			"REFR_EXTRA_OWNERSHIP", "REFR_BASEOBJECT", nullptr, "ACTOR_EXTRA_PACKAGE_DATA", "LIFESTATE", "ACTOR_EXTRA_MERCHANT_CONTAINER", nullptr,
			nullptr, nullptr, nullptr, nullptr, "ACTOR_EXTRA_DISMEMBERED_LIMBS", "ACTOR_LEVELED_ACTOR", "ACTOR_DISPOSITION_MODIFIERS",
			"ACTOR_TEMP_MODIFIERS", "ACTOR_DAMAGE_MODIFIERS", "ACTOR_OVERRIDE_MODIFIERS", "ACTOR_PERMANENT_MODIFIERS", nullptr, "REFR_PROMOTED",
			"REFR_EXTRA_ACTIVATING_CHILDREN", "REFR_LEVELED_INVENTORY", "REFR_ANIMATION", "REFR_EXTRA_ENCOUNTER_ZONE", "REFR_EXTRA_CREATED_ONLY",
			"REFR_EXTRA_GAME_ONLY" };
		const char* kNpcFlags[32] = { "CHANGE_FORM_FLAGS", "ACTOR_BASE_DATA", nullptr, "ACTOR_BASE_AIDATA", "ACTOR_BASE_SPELLLIST",
			"ACTOR_BASE_FULLNAME", "ACTOR_BASE_FACTIONS", nullptr, nullptr, "NPC_SKILLS", "NPC_CLASS", "NPC_FACE", "DEFAULT_OUTFIT", "SLEEP_OUTFIT",
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "NPC_GENDER", "NPC_RACE" };
		const char* kQustFlags[32] = { "CHANGE_FORM_FLAGS", "QUEST_FLAGS", "QUEST_SCRIPT_DELAY", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, "QUEST_ALREADY_RUN", "QUEST_INSTANCES", "QUEST_RUNDATA", "QUEST_OBJECTIVES", "QUEST_SCRIPT", "QUEST_STAGES" };
	}

	std::vector<std::uint8_t> Body(const ChangeForm& a_cf)
	{
		if (a_cf.length2 > 0) {
			return DecompressZlib(a_cf.data.data(), a_cf.data.size(), a_cf.length2);
		}
		return a_cf.data;
	}

	void SetBody(ChangeForm& a_cf, const std::vector<std::uint8_t>& a_body)
	{
		if (a_cf.length2 > 0) {
			a_cf.data = CompressZlib(a_body);
			a_cf.length2 = static_cast<std::uint32_t>(a_body.size());
		} else {
			a_cf.data = a_body;
		}
		a_cf.length1 = static_cast<std::uint32_t>(a_cf.data.size());
		// widen the length fields if the new sizes need it (never narrows: keeps the original layout)
		const auto biggest = std::max<std::uint32_t>(a_cf.length1, a_cf.length2);
		const int  need = biggest > 0xFFFF ? 2 : biggest > 0xFF ? 1 : 0;
		if (need > (a_cf.typeField >> 6)) {
			a_cf.typeField = static_cast<std::uint8_t>((a_cf.typeField & 0x3F) | (need << 6));
		}
	}

	Decoded Decode(const SaveFile& a_save, const ChangeForm& a_cf, const TypeLookup& a_types)
	{
		Decoded out;
		out.root.label = ChangeFormTypeName(a_cf.typeCode());
		std::vector<std::uint8_t> body;
		try {
			body = Body(a_cf);
		} catch (const std::exception& e) {
			out.error = std::string("could not decompress: ") + e.what();
			return out;
		}
		out.bodySize = body.size();
		P          p(a_save, body, a_types, out);
		p.version = a_cf.version;
		const auto flags = a_cf.changeFlags;
		const auto t = static_cast<ChangeFormType>(a_cf.typeCode());
		out.supported = true;
		try {
			switch (t) {
			case ChangeFormType::REFR:
				Refr(p, a_cf.refID, flags);
				break;
			case ChangeFormType::ACHR:
				Achr(p, a_cf.refID, flags, a_cf.version);
				break;
			case ChangeFormType::NPC_:
				NPC(p, flags, false);
				break;
			case ChangeFormType::QUST:
				Qust(p, flags);
				break;
			case ChangeFormType::LVLN:
			case ChangeFormType::LVLI:
				Leveled(p, flags);
				break;
			case ChangeFormType::RELA:
				Rela(p, a_cf.refID, flags);
				break;
			case ChangeFormType::FLST:
				out.isFormList = true;
				if (!Flst(p, flags, 0) && !Flst(p, flags, 1)) {
					out.root.kids.clear();
					throw Stop("form list layout not recognised");
				}
				break;
			default:
				out.supported = false;
				break;
			}
		} catch (const std::exception& e) {
			out.error = e.what();
		}
		out.consumed = p.r.pos();
		if (out.supported && out.error.empty() && p.r.remaining()) {
			out.error = Fmt("%zu byte(s) at the end were not decoded", p.r.remaining());
		}
		if (p.r.remaining()) {
			const auto rest = p.r.remaining();
			p.bytes(out.supported ? "NOT DECODED" : "DATA", rest);
		}
		out.complete = out.supported && out.error.empty();

		// filter facts
		for (const auto& k : out.root.kids) {
			if (k.label == "CHANGE_FORM_FLAGS") {
				for (const auto& f : k.kids) {
					if (f.label == "FLAGS") {
						out.hasFormFlags = true;
						out.formFlags = static_cast<std::uint32_t>(std::strtoul(f.value.c_str(), nullptr, 16));
					}
				}
			}
		}
		if (t == ChangeFormType::REFR || t == ChangeFormType::ACHR) {
			if (Bit(flags, 1) || Bit(flags, 3)) {
				for (const auto& k : out.root.kids) {
					if (k.label == "INITIAL") {
						for (const auto& f : k.kids) {
							if (f.label == "CELL" && f.isRef && f.refRaw &&
								(f.formID == 0xFFFFFFFFu || RefID{ f.refRaw }.type() == RefID::Type::kInvalid)) {
								out.inCellMinus1 = true;
							}
						}
					}
				}
			}
			if ((flags & 0xFF) == 0 && out.complete) {
				const auto& kids = out.root.kids;
				auto        emptyInitial = [](const Node& n) { return n.label == "INITIAL" && n.kids.empty(); };
				if (kids.empty() || (kids.size() == 1 && emptyInitial(kids[0])) ||
					(kids.size() == 2 && emptyInitial(kids[0]) && kids[1].label == "EXTRADATA (0)")) {
					out.isEmptyRef = true;
				}
			}
		}
		return out;
	}

	namespace
	{
		std::string LowerS(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
		std::string BaseLabel(const std::string& l)
		{
			auto s = l;
			if (auto p = s.find(" ("); p != std::string::npos) {
				s = s.substr(0, p);
			}
			return LowerS(s);
		}
		bool ValueEquals(const Node& n, const std::string& want)
		{
			const auto w = LowerS(want);
			if (n.isRef) {
				const auto fid = LowerS(Fmt("%08X", n.formID));
				const auto raw = LowerS(Fmt("%06X", n.refRaw));
				return fid.rfind(w, 0) == 0 || raw == w || (w == "none" && n.refRaw == 0);
			}
			if (LowerS(n.value) == w) {
				return true;
			}
			char*       end = nullptr;
			const double a = std::strtod(n.value.c_str(), &end);
			if (end && *end == '\0' && !n.value.empty()) {
				char*       e2 = nullptr;
				const double b = std::strtod(want.c_str(), &e2);
				return e2 && *e2 == '\0' && a == b;
			}
			return false;
		}
		// path[i..] matches somewhere below node (array elements "[n]" are transparent)
		bool PathMatch(const Node& node, const std::vector<std::string>& path, std::size_t i)
		{
			if (i >= path.size()) {
				return true;
			}
			const bool last = i + 1 == path.size();
			for (const auto& k : node.kids) {
				if (!k.label.empty() && k.label[0] == '[') {
					if (PathMatch(k, path, i)) {
						return true;
					}
					continue;
				}
				if (BaseLabel(k.label) == LowerS(path[i])) {
					if (last) {
						return true;  // the field exists
					}
					if (i + 2 == path.size() && ValueEquals(k, path[i + 1])) {
						return true;  // field = value
					}
					if (PathMatch(k, path, i + 1)) {
						return true;
					}
				}
			}
			return false;
		}
		std::vector<std::string> Split(const std::string& s, char c)
		{
			std::vector<std::string> out;
			std::string              cur;
			for (char ch : s) {
				if (ch == c) {
					out.push_back(cur);
					cur.clear();
				} else {
					cur.push_back(ch);
				}
			}
			out.push_back(cur);
			return out;
		}
		std::string Trim(const std::string& s)
		{
			const auto a = s.find_first_not_of(" \t");
			if (a == std::string::npos) {
				return {};
			}
			return s.substr(a, s.find_last_not_of(" \t") - a + 1);
		}
	}

	bool ContentMatches(const Node& a_root, const std::string& a_expression)
	{
		if (Trim(a_expression).empty()) {
			return true;
		}
		for (const auto& term : Split(a_expression, ';')) {
			if (Trim(term).empty()) {
				continue;
			}
			bool any = false;
			for (auto clause : Split(term, ',')) {
				clause = Trim(clause);
				bool neg = false;
				if (!clause.empty() && clause[0] == '!') {
					neg = true;
					clause = Trim(clause.substr(1));
				}
				std::vector<std::string> path;
				for (const auto& seg : Split(clause, '/')) {
					if (!Trim(seg).empty()) {
						path.push_back(Trim(seg));
					}
				}
				const bool hit = !path.empty() && PathMatch(a_root, path, 0);
				if (hit != neg) {
					any = true;
					break;
				}
			}
			if (!any) {
				return false;
			}
		}
		return true;
	}

	std::vector<std::string> FlagNames(std::uint8_t a_type, std::uint32_t a_flags)
	{
		const char* const* names = nullptr;
		switch (static_cast<ChangeFormType>(a_type)) {
		case ChangeFormType::REFR:
			names = kRefrFlags;
			break;
		case ChangeFormType::ACHR:
			names = kAchrFlags;
			break;
		case ChangeFormType::NPC_:
			names = kNpcFlags;
			break;
		case ChangeFormType::QUST:
			names = kQustFlags;
			break;
		default:
			break;
		}
		std::vector<std::string> out;
		for (int b = 0; b < 32; ++b) {
			if (Bit(a_flags, b)) {
				if (names && names[b]) {
					out.push_back(Fmt("%d: %s", b, names[b]));
				} else if (b == 0) {
					out.push_back("0: CHANGE_FORM_FLAGS");
				} else if (b == 31 && (a_type == static_cast<std::uint8_t>(ChangeFormType::FLST) || a_type == static_cast<std::uint8_t>(ChangeFormType::LVLN) ||
										  a_type == static_cast<std::uint8_t>(ChangeFormType::LVLI))) {
					out.push_back("31: ADDED_FORMS");
				} else {
					out.push_back(Fmt("%d", b));
				}
			}
		}
		return out;
	}

	HavokResult ResetHavok(SaveFile& a_save, const TypeLookup& a_types, bool a_dryRun)
	{
		HavokResult r;
		for (auto& cf : a_save.changeForms) {
			if (static_cast<ChangeFormType>(cf.typeCode()) != ChangeFormType::REFR || !Bit(cf.changeFlags, 2)) {
				continue;
			}
			const auto d = Decode(a_save, cf, a_types);
			if (!d.complete || !d.hasHavok) {
				++r.failed;
				continue;
			}
			++r.reset;
			if (!a_dryRun && d.havokSize) {
				auto body = Body(cf);
				std::memset(body.data() + d.havokOffset, 0, d.havokSize);
				SetBody(cf, body);
			}
		}
		return r;
	}

	std::size_t NullEntries(const SaveFile& a_save, const ChangeForm& a_cf)
	{
		if (static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::FLST) {
			return 0;
		}
		const auto  d = Decode(a_save, a_cf);
		std::size_t n = 0;
		for (auto v : d.listRaw) {
			n += v == 0;
		}
		return d.complete ? n : 0;
	}

	PurifyResult PurifyFormLists(SaveFile& a_save, bool a_dryRun, std::uint32_t a_onlyRefID)
	{
		PurifyResult r;
		for (auto& cf : a_save.changeForms) {
			if (static_cast<ChangeFormType>(cf.typeCode()) != ChangeFormType::FLST || !Bit(cf.changeFlags, 31)) {
				continue;
			}
			if (a_onlyRefID && cf.refID.data != a_onlyRefID) {
				continue;
			}
			const auto d = Decode(a_save, cf);
			if (!d.complete) {
				++r.failed;
				continue;
			}
			std::size_t nulls = 0;
			for (auto v : d.listRaw) {
				nulls += v == 0;
			}
			if (!nulls) {
				continue;
			}
			r.entries += nulls;
			++r.forms;
			if (a_dryRun) {
				continue;
			}
			// rebuild: everything before the list stays, then count + the non-null entries
			auto       body = Body(cf);
			const auto listStart = body.size() - 4 - 3 * d.listRaw.size();
			std::vector<std::uint8_t> nb(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(listStart));
			const auto                kept = static_cast<std::uint32_t>(d.listRaw.size() - nulls);
			for (int i = 0; i < 4; ++i) {
				nb.push_back(static_cast<std::uint8_t>(kept >> (8 * i)));
			}
			for (auto v : d.listRaw) {
				if (v) {
					nb.push_back(static_cast<std::uint8_t>(v >> 16));
					nb.push_back(static_cast<std::uint8_t>(v >> 8));
					nb.push_back(static_cast<std::uint8_t>(v));
				}
			}
			SetBody(cf, nb);
		}
		return r;
	}
}

namespace ess::cf
{
	std::vector<ChargedItem> FindChargedItems(const SaveFile& a_save)
	{
		std::vector<ChargedItem> out;
		for (const auto& c : a_save.changeForms) {
			const auto t = static_cast<ChangeFormType>(c.typeCode());
			if ((t != ChangeFormType::REFR && t != ChangeFormType::ACHR) || !((c.changeFlags >> 5) & 1 || (c.changeFlags >> 27) & 1)) {
				continue;  // no inventory in this change form
			}
			const auto d = Decode(a_save, c);
			for (const auto& e : d.charged) {
				out.push_back({ c.refID.data, c.refID.isZero() ? 0 : a_save.formIDOf(c.refID), e });
			}
		}
		return out;
	}

	bool NeedsChargeFix(const InvCharge& a_item)
	{
		return (a_item.hasEnchant && a_item.enchRaw != 0 && a_item.maxCharge == 0) ||
			   (a_item.hasEnchant && a_item.hasCharge && a_item.maxCharge > 0 && a_item.charge > a_item.maxCharge);
	}

	std::size_t FixCharges(SaveFile& a_save, std::uint16_t a_maxCharge, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& a_only)
	{
		if (a_maxCharge == 0) {
			return 0;
		}
		std::size_t fixed = 0;
		for (auto& c : a_save.changeForms) {
			const auto t = static_cast<ChangeFormType>(c.typeCode());
			if ((t != ChangeFormType::REFR && t != ChangeFormType::ACHR) || !((c.changeFlags >> 5) & 1 || (c.changeFlags >> 27) & 1)) {
				continue;
			}
			const auto d = Decode(a_save, c);
			bool       any = false;
			auto       body = Body(c);
			for (const auto& e : d.charged) {
				if (!NeedsChargeFix(e)) {
					continue;
				}
				if (!a_only.empty() && std::find(a_only.begin(), a_only.end(), std::make_pair(c.refID.data, e.item)) == a_only.end()) {
					continue;
				}
				std::uint16_t maxc = e.maxCharge;
				if (maxc == 0 && e.maxChargeOffset + 2 <= body.size()) {
					maxc = a_maxCharge;
					body[e.maxChargeOffset] = static_cast<std::uint8_t>(maxc & 0xFF);
					body[e.maxChargeOffset + 1] = static_cast<std::uint8_t>(maxc >> 8);
				}
				if (e.hasCharge && e.charge > maxc && e.chargeOffset + 4 <= body.size()) {
					const float f = static_cast<float>(maxc);
					std::uint32_t bits;
					std::memcpy(&bits, &f, 4);
					for (int i = 0; i < 4; ++i) {
						body[e.chargeOffset + i] = static_cast<std::uint8_t>(bits >> (8 * i));
					}
				}
				++fixed;
				any = true;
			}
			if (any) {
				SetBody(c, body);
			}
		}
		return fixed;
	}

	bool CanRemoveFlag(const ChangeForm& a_cf, int a_bit)
	{
		return static_cast<ChangeFormType>(a_cf.typeCode()) == ChangeFormType::NPC_ && a_bit > 0 && a_bit < 32 && Bit(a_cf.changeFlags, a_bit);
	}

	std::string RemoveFlag(const SaveFile& a_save, ChangeForm& a_cf, int a_bit)
	{
		if (!CanRemoveFlag(a_cf, a_bit)) {
			return "only parts of an NPC_ change form can be removed, and only flags that are set";
		}
		const auto d = Decode(a_save, a_cf);
		if (!d.complete) {
			return "this change form could not be read completely (" + d.error + "): nothing changed";
		}
		const auto it = d.sections.find(a_bit);
		if (it == d.sections.end()) {
			return "this flag's data was not found: nothing changed";
		}
		const auto before = a_cf;
		auto       body = Body(a_cf);
		body.erase(body.begin() + static_cast<std::ptrdiff_t>(it->second.first), body.begin() + static_cast<std::ptrdiff_t>(it->second.second));
		a_cf.changeFlags &= ~(1u << a_bit);
		SetBody(a_cf, body);
		const auto after = Decode(a_save, a_cf);
		if (!after.complete) {
			a_cf = before;
			return "the result would not read back correctly: nothing changed";
		}
		return {};
	}
}
