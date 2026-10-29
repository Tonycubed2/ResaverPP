// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "PropertyEdit.h"

#include "Save.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

namespace ess::props
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
		std::string Trim(std::string s)
		{
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
				s.pop_back();
			}
			std::size_t i = 0;
			while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
				++i;
			}
			return s.substr(i);
		}
		bool ParseHex(std::string s, std::uint32_t& a_out)
		{
			s = Trim(s);
			if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
				s = s.substr(2);
			}
			if (s.empty() || s.size() > 8) {
				return false;
			}
			std::uint32_t v = 0;
			for (char c : s) {
				if (!std::isxdigit(static_cast<unsigned char>(c))) {
					return false;
				}
				v = v * 16 + static_cast<std::uint32_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(c) - 'a' + 10);
			}
			a_out = v;
			return true;
		}
		// the script of that name, compared without case (the game's string table keeps one spelling)
		const Script* FindScriptNamed(const Papyrus& p, const std::string& a_name)
		{
			const auto want = Lower(a_name);
			for (const auto& s : p.scripts) {
				if (Lower(p.str(s.name)) == want) {
					return &s;
				}
			}
			return nullptr;
		}
		std::size_t MemberCount(const Papyrus& p, const Script* s)
		{
			std::size_t n = 0;
			for (int depth = 0; s && depth < 32; ++depth) {
				n += s->members.size();
				if (p.str(s->parent).empty()) {
					break;
				}
				s = p.findScript(s->parent);
			}
			return n;
		}
	}

	std::string RecordTypeFor(const std::string& a_papyrusType)
	{
		static const std::map<std::string, std::string> m = {
			{ "spell", "SPEL" }, { "weapon", "WEAP" }, { "armor", "ARMO" }, { "miscobject", "MISC" }, { "potion", "ALCH" },
			{ "ingredient", "INGR" }, { "book", "BOOK" }, { "key", "KEYM" }, { "ammo", "AMMO" }, { "scroll", "SCRL" },
			{ "shout", "SHOU" }, { "wordofpower", "WOOP" }, { "perk", "PERK" }, { "faction", "FACT" }, { "quest", "QUST" },
			{ "globalvariable", "GLOB" }, { "formlist", "FLST" }, { "leveleditem", "LVLI" }, { "leveledactor", "LVLN" },
			{ "leveledspell", "LVSP" }, { "magiceffect", "MGEF" }, { "enchantment", "ENCH" }, { "keyword", "KYWD" },
			{ "location", "LCTN" }, { "locationreftype", "LCRT" }, { "message", "MESG" }, { "package", "PACK" }, { "race", "RACE" },
			{ "static", "STAT" }, { "sound", "SNDR" }, { "sounddescriptor", "SNDR" }, { "voicetype", "VTYP" }, { "outfit", "OTFT" },
			{ "container", "CONT" }, { "activator", "ACTI" }, { "actorbase", "NPC_" }, { "light", "LIGH" }, { "door", "DOOR" },
			{ "furniture", "FURN" }, { "idle", "IDLE" }, { "imagespacemodifier", "IMAD" }, { "topic", "DIAL" }, { "topicinfo", "INFO" },
			{ "scene", "SCEN" }, { "encounterzone", "ECZN" }, { "worldspace", "WRLD" }, { "cell", "CELL" }, { "explosion", "EXPL" },
			{ "projectile", "PROJ" }, { "hazard", "HAZD" }, { "weather", "WTHR" }, { "class", "CLAS" }, { "art", "ARTO" },
			{ "effectshader", "EFSH" }, { "visualeffect", "RFCT" }, { "actor", "ACHR" }, { "soulgem", "SLGM" }, { "flora", "FLOR" },
			{ "tree", "TREE" }, { "talkingactivator", "TACT" }, { "shaderparticlegeometry", "SPGD" }, { "associationtype", "ASTP" },
			{ "constructibleobject", "COBJ" }, { "equipslot", "EQUP" }, { "headpart", "HDPT" }, { "textureset", "TXST" },
			{ "impactdataset", "IPDS" }, { "musictype", "MUSC" }, { "colorform", "CLFM" }, { "combatstyle", "CSTY" },
			{ "dialoguebranch", "DLBR" }, { "camerashot", "CAMS" }, { "action", "AACT" },
		};
		const auto it = m.find(Lower(a_papyrusType));
		return it == m.end() ? std::string() : it->second;
	}

	std::uint32_t ParseFormID(const SaveFile& a_save, const std::string& a_text)
	{
		const auto t = Trim(a_text);
		const auto sep = t.find_first_of(":|");
		std::uint32_t v = 0;
		if (sep == std::string::npos) {  // a plain form id
			return ParseHex(t, v) ? v : 0;
		}
		const auto plugin = Lower(Trim(t.substr(0, sep)));
		if (!ParseHex(t.substr(sep + 1), v)) {
			return 0;
		}
		for (std::size_t i = 0; i < a_save.plugins.full.size(); ++i) {
			if (Lower(a_save.plugins.full[i]) == plugin) {
				return static_cast<std::uint32_t>(i << 24) | (v & 0xFFFFFF);
			}
		}
		for (std::size_t i = 0; i < a_save.plugins.lite.size(); ++i) {
			if (Lower(a_save.plugins.lite[i]) == plugin) {
				return 0xFE000000u | static_cast<std::uint32_t>(i << 12) | (v & 0xFFF);
			}
		}
		return 0;
	}

	RefID RefFor(SaveFile& a_save, std::uint32_t a_formID)
	{
		RefID r;
		if ((a_formID >> 24) == 0xFF) {
			r.data = (2u << 22) | (a_formID & 0x3FFFFF);  // created form
			return r;
		}
		for (std::size_t i = 0; i < a_save.formIDs.size(); ++i) {
			if (a_save.formIDs[i] == a_formID) {
				r.data = static_cast<std::uint32_t>(i + 1);
				return r;
			}
		}
		if (a_save.formIDs.size() + 1 >= 0x3FFFFF) {
			return r;  // full (never in practice)
		}
		a_save.formIDs.push_back(a_formID);
		r.data = static_cast<std::uint32_t>(a_save.formIDs.size());
		return r;
	}

	std::vector<Candidate> Existing(const SaveFile& a_save, const std::string& a_type)
	{
		std::vector<Candidate> out;
		const auto*            p = a_save.papyrus();
		if (!p) {
			return out;
		}
		const auto want = Lower(a_type);
		for (const auto& i : p->instances) {
			if (Lower(p->str(i.script)) == want && !i.refID.isZero()) {
				if (const auto f = a_save.formIDOf(i.refID)) {
					out.push_back({ i.id, f });
				}
			}
		}
		return out;
	}

	Outcome PointAt(SaveFile& a_save, Variable& a_var, const std::string& a_type, std::uint32_t a_formID)
	{
		Outcome o;
		auto*   pp = a_save.papyrus();
		if (!pp || a_formID == 0 || a_type.empty()) {
			o.message = "Nothing to point at.";
			return o;
		}
		auto&     p = *pp;
		Variable* v = &a_var;
		if (v->type == VType::kVariant && v->inner) {
			v = v->inner.get();
		}
		if (v->type != VType::kRef && v->type != VType::kNull) {
			o.message = "This variable does not hold an object.";
			return o;
		}
		const auto want = Lower(a_type);
		// 1. the object the save already has for this form, as this type
		for (const auto& i : p.instances) {
			if (Lower(p.str(i.script)) == want && a_save.formIDOf(i.refID) == a_formID) {
				v->type = VType::kRef;
				v->refType = p.internString(p.str(i.script));
				v->value = i.id;
				o.ok = true;
				o.message = "Pointed at the object the save already has for this form.";
				return o;
			}
		}
		// 2. a new object, copied from another object of the same type. Only for types without variables (the
		//    game's own types: Spell, Weapon, Form...): an object of a script type carries that script's data,
		//    which is the script being attached, not a property being filled.
		const Script* sc = FindScriptNamed(p, a_type);
		if (!sc) {
			o.message = "This save has no object of type " + a_type +
						" at all, so there is nothing to copy one from. Load the game, let the mod use the form once, save, and try again.";
			return o;
		}
		if (MemberCount(p, sc) != 0) {
			o.message = a_type + " is a script with its own variables: a new object of it would attach that script to the form. Only an object "
								 "the save already has can be used (none is bound to this form).";
			return o;
		}
		const ScriptInstance* tmpl = nullptr;
		for (const auto& i : p.instances) {
			if (i.script == sc->name && i.data && !i.refID.isZero()) {
				tmpl = &i;
				break;
			}
		}
		if (!tmpl) {
			o.message = "This save has no object of type " + a_type + " to copy the layout from.";
			return o;
		}
		const RefID ref = RefFor(a_save, a_formID);
		if (ref.isZero()) {
			o.message = "The save's form table is full.";
			return o;
		}
		// a new id, above every object, array and reference id in the save
		EID top = 0;
		for (const auto& i : p.instances) {
			top = std::max(top, i.id);
		}
		for (const auto& r : p.references) {
			top = std::max(top, r.id);
		}
		for (const auto& a : p.arrays) {
			top = std::max(top, a.id);
		}
		for (const auto id : p.unknownIDs) {
			top = std::max(top, id);
		}
		const bool wide = !a_save.isLE();
		if (!wide && top >= 0xFFFFFFF0u) {
			o.message = "No free object id is left in this save.";
			return o;
		}
		ScriptInstance n;
		n.id = top + 1;
		n.script = tmpl->script;
		n.unknown2bits = tmpl->unknown2bits;
		n.unknown = tmpl->unknown;
		n.refID = ref;
		n.unknownByte = tmpl->unknownByte;
		InstanceData d = *tmpl->data;
		d.vars.clear();
		n.data = std::move(d);
		const auto newID = n.id;
		const auto scriptName = p.str(sc->name);
		// the variable first: it may live inside p.instances, which the new object can move
		v->type = VType::kRef;
		v->refType = p.internString(scriptName);
		v->value = newID;
		v = nullptr;
		p.instances.push_back(std::move(n));
		p.instanceDataOrder.push_back(newID);
		p.reindex();
		o.ok = true;
		o.added = true;
		o.message = "Added the save's object for this form (as " + scriptName + ") and pointed at it.";
		return o;
	}
}
