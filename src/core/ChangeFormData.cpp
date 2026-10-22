// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "ChangeFormData.h"

#include "ByteIO.h"

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
			case 36: one("Count", [&] { p.i16("COUNT"); }); break;
			case 37: one("Health", [&] { p.f32("HEALTH"); }); break;
			case 39: one("TimeLeft", [&] { p.i32("TIME"); }); break;
			case 40: one("Charge", [&] { p.f32("CHARGE"); }); break;
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
				one("TEST", [&] {
					for (int i = 1; i <= 4; ++i) {
						p.ref(Fmt("R%d", i));
					}
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
					p.ref("ENCH");
					p.i16("CHARGE");
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
				p.ref("ITEM");
				p.i32("COUNT");
				ExtraData(p);
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

		void Achr(P& p, const RefID& id, std::uint32_t flags)
		{
			const int t = InitialType(id, flags);
			Initial(p, t);
			Havok(p, flags);
			p.i32("UNKNOWN_INTEGER");
			p.bytes("UNKNOWN_BYTES", 4);
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
		}

		void NPC(P& p, std::uint32_t flags, bool)
		{
			if (Bit(flags, 0)) {
				p.changeFormFlags();
			}
			if (Bit(flags, 1)) {
				p.bytes("ACBS", 24);
			}
			if (Bit(flags, 6)) {
				p.vsArray("FACTIONRANKS", [&] {
					p.ref("FACTION");
					p.u8("RANK");
				});
			}
			if (Bit(flags, 4)) {
				p.vsArray("SPELLS", [&] { p.ref("SPELL"); });
				p.vsArray("SPELLS_LEVELLED", [&] { p.ref("SPELL"); });
				p.vsArray("SHOUTS", [&] { p.ref("SHOUT"); });
			}
			if (Bit(flags, 3)) {
				p.bytes("AIDT", 20);
			}
			if (Bit(flags, 5)) {
				p.wstring("FULLNAME");
			}
			if (Bit(flags, 9)) {
				p.bytes("DNAM", 52);
			}
			if (Bit(flags, 10)) {
				p.ref("CLASS");
			}
			if (Bit(flags, 25)) {
				p.ref("RACE");
				p.ref("OLDRACE");
			}
			if (Bit(flags, 11)) {
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
			}
			if (Bit(flags, 24)) {
				p.u8("GENDER");
			}
			if (Bit(flags, 12)) {
				p.ref("DEFAULT_OUTFIT");
			}
			if (Bit(flags, 13)) {
				p.ref("SLEEP_OUTFIT");
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
		const auto flags = a_cf.changeFlags;
		const auto t = static_cast<ChangeFormType>(a_cf.typeCode());
		out.supported = true;
		try {
			switch (t) {
			case ChangeFormType::REFR:
				Refr(p, a_cf.refID, flags);
				break;
			case ChangeFormType::ACHR:
				Achr(p, a_cf.refID, flags);
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
