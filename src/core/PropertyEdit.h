// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Filling a script's object property (a Spell, a Weapon, a Quest...) in the save, the way the Creation Kit
// fills it in the plugin. In the save, an object value is the id of a Papyrus object: a script instance bound
// to that form. Pointing a variable at a form uses the object the save already has for it, or, for the
// game's own form types (Spell, Weapon, Form... which carry no variables), adds one like the game does.
#pragma once

#include "Papyrus.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ess
{
	class SaveFile;
}

namespace ess::props
{
	// The record type (SPEL, WEAP...) a Papyrus object type stands for; empty for types that can be any
	// form (Form, ObjectReference, script types).
	std::string RecordTypeFor(const std::string& a_papyrusType);

	// Reads a form id as typed: "0A012345", "0x0A012345", "FE001ABC" (the game's console form ids, in the
	// save's load order), or "Plugin.esp:012345" / "Plugin.esp|012345". 0 if it cannot be read or the
	// plugin is not in the save.
	std::uint32_t ParseFormID(const SaveFile& a_save, const std::string& a_text);

	// The save's reference to a form: its entry in the save's form id table (added if missing).
	RefID RefFor(SaveFile& a_save, std::uint32_t a_formID);

	// The objects of a type already in the save (instances of that script, bound to a form): what a
	// property of that type can point at without adding anything.
	struct Candidate
	{
		EID           id = 0;
		std::uint32_t formID = 0;
	};
	std::vector<Candidate> Existing(const SaveFile& a_save, const std::string& a_type);

	struct Outcome
	{
		bool        ok = false;
		bool        added = false;  // a new object was added for the form
		std::string message;        // what was done, or why not
	};

	// Points a_var at a_formID as an object of type a_type (the property's declared type). A variable that
	// held nothing (None, never filled) becomes an object variable of that type.
	Outcome PointAt(SaveFile& a_save, Variable& a_var, const std::string& a_type, std::uint32_t a_formID);
}
