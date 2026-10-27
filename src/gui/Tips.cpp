#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <unordered_map>

namespace tips
{
	bool Ready()
	{
		if (!g_enabled) {
			return false;
		}
		ImGui::GetStyle().HoverDelayNormal = g_delay;
		return ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_AllowWhenDisabled);
	}

	namespace
	{
		void Show(const char* a_text)
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
			ImGui::TextUnformatted(a_text);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	void Tip(const char* a_text)
	{
		if (a_text && a_text[0] && Ready()) {
			Show(a_text);
		}
	}

	void Tipf(const char* a_fmt, ...)
	{
		if (!a_fmt || !Ready()) {
			return;
		}
		char buf[4096];
		va_list ap;
		va_start(ap, a_fmt);
		std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
		va_end(ap);
		Show(buf);
	}

	namespace
	{
		std::string Key(std::string_view a_label)
		{
			std::string s(a_label);
			// "EXTRADATA (5)" -> "EXTRADATA", "Change Forms (69380)" -> "Change Forms", "LocRef" stays
			if (const auto p = s.find(" ("); p != std::string::npos && !s.empty() && s.back() == ')') {
				s.erase(p);
			}
			// "25: REFR_PROMOTED" -> "REFR_PROMOTED"
			if (const auto c = s.find(": "); c != std::string::npos && c <= 3 &&
				std::all_of(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(c), [](unsigned char ch) { return std::isdigit(ch); })) {
				s.erase(0, c + 2);
			}
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
			return s;
		}

		const std::unordered_map<std::string, const char*>& Glossary()
		{
			static const std::unordered_map<std::string, const char*> g = [] {
				std::unordered_map<std::string, const char*> m;
				auto add = [&](const char* k, const char* v) { m[Key(k)] = v; };
				// ---- sidebar ----
				add("Overview", "A summary of the save: who and where you are, its size, and what stands out.");
				add("Mods", "Every mod (plugin) the save mentions, with how many scripts, script instances and change forms each one left in "
							"the save. Mods no longer installed show here too.");
				add("Quests", "The quests in this save: running, completed and failed, with their stage and journal objectives.");
				add("Plugins", "The save's load order: every plugin (.esm/.esp/.esl) that was active when it was saved. Click one to see "
							   "everything it put in the save.");
				add("Change Forms",
					"Everything the game changed from what the plugins say: moved or killed actors, opened doors, picked up items, "
					"changed quests and so on. One change form per changed thing; ACHR are actors, REFR objects, NPC_ actor bases.");
				add("Strings", "The text table of the script engine: every script, variable and function name and every text value "
							   "scripts store.");
				add("Scripts", "The script types (one per .pex file) that have data in the save.");
				add("Script Instances", "Scripts attached to something (an actor, an object, a quest...), each with its own variables.");
				add("References", "Script objects that are not attached to a form: structures some scripts create to hold data.");
				add("Arrays", "Script arrays (lists of values) that scripts keep in the save.");
				add("Active Scripts", "Scripts that were running when you saved (threads). Many threads of the same script, or threads "
									  "stuck waiting, are a common cause of script lag.");
				add("Function Messages", "Script calls queued but not started yet when you saved.");
				add("Suspended Stacks", "Script calls paused when you saved, waiting to continue.");
				add("Queued Unbinds", "Scripts waiting to be detached from their objects.");
				// ---- change form types ----
				add("ACHR", "An actor reference: a placed NPC or creature, or you. Holds its position, inventory, AI, stats and more.");
				add("REFR", "An object reference: a placed object (item, door, container, furniture...) whose state changed.");
				add("NPC_", "An actor base: the NPC record itself (name, face, race, skills, factions, outfit), when the game changed it.");
				add("QUST", "A quest: its stage, objectives, flags and aliases.");
				add("CELL", "A cell (an interior or a square of the world): which parts of it changed.");
				add("FLST", "A form list: a list of forms some scripts add to and remove from.");
				add("FACT", "A faction: its flags, its relations to other factions, and crime counts.");
				// ---- change flags ----
				add("CHANGE_FORM_FLAGS", "The record's own flags changed (e.g. disabled, deleted, persistent).");
				add("REFR_MOVE", "It was moved: its saved position, rotation and cell replace the plugin's.");
				add("REFR_HAVOK_MOVE", "Its physics (Havok) state is saved: how it lies or tumbles.");
				add("REFR_CELL_CHANGED", "It moved to another cell than the one the plugin places it in.");
				add("REFR_SCALE", "Its size was changed.");
				add("REFR_INVENTORY", "What it carries changed (items added or removed).");
				add("REFR_EXTRA_OWNERSHIP", "Its owner changed.");
				add("REFR_BASEOBJECT", "What it is an instance of (its base object) changed.");
				add("REFR_PROMOTED", "It was made persistent (promoted), usually because a quest or script points at it.");
				add("REFR_ANIMATION", "Its animation state is saved (pose, idle, what it is doing).");
				add("REFR_LEVELED_INVENTORY", "Its leveled items were rolled and saved.");
				add("REFR_EXTRA_ENCOUNTER_ZONE", "Its encounter zone (level range and reset rules) changed.");
				add("REFR_EXTRA_GAME_ONLY", "It carries extra data the game only keeps in saves.");
				add("REFR_EXTRA_CREATED_ONLY", "Extra data of an object created during play.");
				add("REFR_EXTRA_ACTIVATING_CHILDREN", "What it activates when used changed.");
				add("ACTOR_EXTRA_PACKAGE_DATA", "Data of the AI package it is running.");
				add("LIFESTATE", "Alive, dead, unconscious, bleeding out...");
				add("ACTOR_EXTRA_MERCHANT_CONTAINER", "Its merchant chest.");
				add("ACTOR_EXTRA_DISMEMBERED_LIMBS", "Limbs it lost.");
				add("ACTOR_LEVELED_ACTOR", "Which actor its leveled list rolled.");
				add("ACTOR_DISPOSITION_MODIFIERS", "How it feels about others.");
				add("ACTOR_TEMP_MODIFIERS", "Temporary changes to its actor values.");
				add("ACTOR_DAMAGE_MODIFIERS", "Damage to its actor values (lost health, magicka, stamina).");
				add("ACTOR_OVERRIDE_MODIFIERS", "Overrides of its actor values.");
				add("ACTOR_PERMANENT_MODIFIERS", "Permanent changes to its actor values.");
				add("ACTOR_BASE_DATA", "Base stats: level, offsets of health, magicka and stamina, speed.");
				add("ACTOR_BASE_AIDATA", "AI settings: aggression, confidence, morality...");
				add("ACTOR_BASE_SPELLLIST", "Spells and shouts it knows.");
				add("ACTOR_BASE_FULLNAME", "Its name was changed.");
				add("ACTOR_BASE_FACTIONS", "Factions it belongs to.");
				add("NPC_SKILLS", "Its skills.");
				add("NPC_CLASS", "Its class.");
				add("NPC_FACE", "Its face (head parts, morphs, tint). Removing it gives back the plugin's face.");
				add("DEFAULT_OUTFIT", "What it wears normally.");
				add("SLEEP_OUTFIT", "What it wears to bed.");
				add("NPC_GENDER", "Its gender changed.");
				add("NPC_RACE", "Its race changed.");
				// ---- decoded groups and values ----
				add("INITIAL", "Where it is: cell, position and rotation, saved when it moved.");
				add("CELL", "The cell (interior, or a square of the world) it is in.");
				add("POS", "Position in game units (x, y, z).");
				add("ROT", "Rotation in radians (x, y, z).");
				add("HAVOK", "Physics state (how it lies, how it moves). Reset Havok clears it.");
				add("PROCESS LEVEL",
					"How much AI is loaded for this actor: 0 high (near you), 1 middle-high, 2 middle-low, 3 low, -1 none (not loaded).");
				add("BOOL BITS", "Actor state flags (e.g. dead, in combat, ghost, essential...).");
				add("EXTRADATA", "Extra data attached to it: owner, encounter zone, aliases, scene, package and many more.");
				add("INVENTORY", "Items it carries in addition to (or instead of) what the plugins give it.");
				add("ANIMATIONS", "Its animation graph state (the pose and animations it is in).");
				add("AI PROCESS", "Its AI state: current package and procedure, targets, equipped items, dialogue, knowledge of others.");
				add("CURRENT PACKAGE", "The AI package it is running (follow, sandbox, travel, eat...) and how far it got.");
				add("PACKAGE", "An AI package: what the actor is told to do.");
				add("PACKAGE DATA", "The running package's own state (its targets and procedure).");
				add("PACKAGE TYPE", "The package's procedure type (18 = a package built from a template, the usual kind).");
				add("MIDDLE HIGH", "AI data kept while the actor is close to you.");
				add("HIGH", "AI data kept while the actor is close to you and active.");
				add("ACTOR", "The actor's own data: timers, state, spells being cast, actor values, added spells, movement and combat.");
				add("ACTOR VALUES", "Stats: Health, Magicka, skills, resistances, speed... Base is the saved value; modifiers change it.");
				add("BASE", "Saved base values of its actor values.");
				add("MODIFIERS", "Changes on top of the base values: permanent, temporary (active effects) and damage.");
				add("ADDED SPELLS", "Spells, powers and abilities given during play.");
				add("MOVEMENT CONTROLLER", "How the actor moves: its path, pathing requests and the parts that steer it.");
				add("COMBAT CONTROLLER", "Its combat state: target, aiming, combat area, weapons and spells it may use, behavior.");
				add("ARBITERS", "Parts of the movement controller that decide how it moves.");
				add("AGENTS", "Parts of the movement controller that apply the movement (path following, avoiding others...).");
				add("PLAYER", "Data only the player has: skills progress, camera, crimes, perk points, race and more.");
				add("CAMERA", "The camera states (first person, third person, horse, dragon...).");
				add("SKILLS", "Skill levels and progress toward the next level.");
				add("CRIME GOLD", "Your bounty and crime totals with each faction.");
				add("STOLEN ITEM VALUES", "The value of items stolen from each faction.");
				add("SKIN", "Your race and skin tint data.");
				add("PERK POINTS", "Unspent perk points.");
				add("XP", "Experience toward the next character level.");
				add("XP FOR NEXT LEVEL", "Experience needed to reach the next character level.");
				add("FACTIONRANKS", "Factions the actor belongs to and its rank in each.");
				add("ACBS", "Base stats: flags, health/magicka/stamina offsets, level, speed, disposition.");
				add("AIDT", "AI data: aggression, confidence, energy, morality, mood, assistance.");
				add("DNAM", "Skills and the health, magicka and stamina values.");
				add("FACEDATA", "Face: hair color, skin tone, head parts, morphs and presets.");
				// ---- actor values ----
				add("Aggression", "AI: 0 unaggressive, 1 aggressive (attacks enemies on sight), 2 very aggressive, 3 frenzied (attacks anyone).");
				add("Confidence", "AI: 0 cowardly (always flees) to 4 foolhardy (never flees).");
				add("Energy", "AI: how much the actor moves around while sandboxing.");
				add("Morality", "AI: 0 any crime to 3 no crime: what it agrees to do for you.");
				add("Mood", "AI: the facial mood it shows (neutral, angry, happy...).");
				add("Assistance", "AI: 0 helps nobody, 1 helps allies, 2 helps friends and allies in a fight.");
				add("Magicka", "Magicka: spent casting spells.");
				add("Stamina", "Stamina: spent sprinting, power attacking and bashing.");
				add("HealRate", "Health regained per second while out of combat, in percent of the maximum.");
				add("MagickaRate", "Magicka regained per second, in percent of the maximum.");
				add("StaminaRate", "Stamina regained per second, in percent of the maximum.");
				add("SpeedMult", "Movement speed in percent (100 = normal). Odd values make you very slow or very fast.");
				add("InventoryWeight", "The weight of what you carry (the game recalculates it).");
				add("CarryWeight", "How much you can carry before you are overencumbered.");
				add("CriticalChance", "Chance of a critical hit, in percent.");
				add("MeleeDamage", "Extra melee damage.");
				add("UnarmedDamage", "Damage of unarmed (fist) attacks.");
				add("Mass", "Physical mass (how hard you are to push).");
				add("VoicePoints", "Shout power.");
				add("VoiceRate", "How fast shouts recover.");
				add("DamageResist", "Armor rating.");
				add("PoisonResist", "Resistance to poison, in percent.");
				add("ResistFire", "Resistance to fire, in percent.");
				add("ResistShock", "Resistance to shock, in percent.");
				add("ResistFrost", "Resistance to frost, in percent.");
				add("ResistMagic", "Resistance to all magic, in percent.");
				add("ResistDisease", "Resistance to disease, in percent.");
				add("Paralysis", "Above 0 while paralyzed. A stuck value leaves you frozen.");
				add("Invisibility", "Above 0 while invisible. A stuck value keeps you invisible.");
				add("NightEye", "Above 0 while Night Eye is active.");
				add("DetectLifeRange", "Range of Detect Life.");
				add("WaterBreathing", "Above 0 while you can breathe under water.");
				add("WaterWalking", "Above 0 while you can walk on water.");
				add("Fame", "Fame: good deeds the game counts.");
				add("Infamy", "Infamy: bad deeds the game counts.");
				add("JumpingBonus", "Extra jump height.");
				add("WardPower", "Strength of your active ward.");
				add("ArmorPerks", "Armor rating bonus from perks, in percent.");
				add("ShieldPerks", "Shield bonus from perks, in percent.");
				add("BowSpeedBonus", "Bow draw-speed bonus (slow time while aiming).");
				add("WeaponSpeedMult", "Weapon swing speed multiplier (right hand). 0 or 1 = normal.");
				add("LeftWeaponSpeedMultiply", "Weapon swing speed multiplier (left hand).");
				add("ShoutRecoveryMult", "Shout cooldown multiplier (1 = normal, lower = faster).");
				add("MovementNoiseMult", "How much noise you make moving (1 = normal).");
				add("DragonSouls", "Dragon souls not yet spent on shout words.");
				add("CombatHealthRegenMultiply", "Health regeneration during combat (vanilla: 0.7 for the player).");
				add("Variable01", "A spare value the game and mods use for their own purposes.");
				add("WaitingForPlayer", "Set while a follower waits for you.");
				// ---- extra data ----
				add("Ownership", "Who owns it (an NPC or faction): taking it is stealing.");
				add("EncounterZone", "Its encounter zone: the level range of what spawns, and when it resets.");
				add("AliasInstanceArray", "Quest aliases it fills: the quests that point at it. Removing these can break quests.");
				add("LeveledCreature", "The leveled list this actor came from and the actor it rolled.");
				add("PromotedRef", "Why it is persistent: the things that point at it.");
				add("Package", "The package it is running.");
				add("PackageData", "The running package's data.");
				add("PackageStartLocation", "Where its current package started.");
				add("RunOncePacks", "Run-once packages it already ran.");
				add("SceneData", "The scene it is in.");
				add("FactionChanges", "Factions it joined or left during play, and rank changes.");
				add("DismemberedLimbs", "Limbs it lost.");
				add("AttachedArrows3D", "Arrows stuck in it.");
				add("Ghost", "It is a ghost (attacks pass through it).");
				add("TextDisplayData", "A name set during play.");
				add("CanTalkToPlayer", "Whether it can talk to you.");
				add("FollowerSwimBreadcrumbs", "The trail it follows its leader by through water.");
				add("GuardedRefData", "What it remembers guarding.");
				add("PlayerCrimeList", "Crimes it saw you commit.");
				add("ForcedTarget", "A target it was forced to attack.");
				add("HeadingTarget", "Something it was told to turn toward.");
				add("ForcedLandingMarker", "Where it (usually a dragon) was forced to land.");
				add("FriendHits", "How often you hit it while it was friendly.");
				add("CachedScale", "Its scale, cached.");
				add("ActorCause", "What created it (e.g. a spell that summoned it).");
				add("AshPileRef", "The ash pile it left (ghosts, vampires...).");
				add("StartingWorldOrCell", "The world or cell it started in.");
				add("Worn", "Worn by the actor.");
				add("Count", "How many items this stack holds.");
				add("Charge", "Enchantment charge left.");
				add("Health", "For an actor: its health. For an item: its condition (tempering, 1.0 = normal).");
				add("Enchantment", "An enchantment added during play (your own enchanting).");
				add("LocRef", "A place given as a reference and a radius.");
				add("LocPt", "A place given as a point in a cell or world.");
				add("LocCell", "A place given as a cell.");
				return m;
			}();
			return g;
		}
	}

	const char* Explain(std::string_view a_label)
	{
		const auto& g = Glossary();
		if (const auto it = g.find(Key(a_label)); it != g.end()) {
			return it->second;
		}
		return nullptr;
	}
}
