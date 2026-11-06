#include "Tips.h"

#include "core/Lang.h"

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
				add("Overview", N_("A summary of the save: who and where you are, its size, and what stands out."));
				add("Mods", N_("Every mod (plugin) the save mentions, with how many scripts, script instances and change forms each one left in "
							"the save. Mods no longer installed show here too."));
				add("Quests", N_("The quests in this save: running, completed and failed, with their stage and journal objectives."));
				add("Plugins", N_("The save's load order: every plugin (.esm/.esp/.esl) that was active when it was saved. Click one to see "
							   "everything it put in the save."));
				add("Change Forms",
					N_("Everything the game changed from what the plugins say: moved or killed actors, opened doors, picked up items, "
					"changed quests and so on. One change form per changed thing; ACHR are actors, REFR objects, NPC_ actor bases."));
				add("Strings", N_("The text table of the script engine: every script, variable and function name and every text value "
							   "scripts store."));
				add("Scripts", N_("The script types (one per .pex file) that have data in the save."));
				add("Script Instances", N_("Scripts attached to something (an actor, an object, a quest...), each with its own variables."));
				add("References", N_("Script objects that are not attached to a form: structures some scripts create to hold data."));
				add("Arrays", N_("Script arrays (lists of values) that scripts keep in the save."));
				add("Active Scripts", N_("Scripts that were running when you saved (threads). Many threads of the same script, or threads "
									  "stuck waiting, are a common cause of script lag."));
				add("Function Messages", N_("Script calls queued but not started yet when you saved."));
				add("Suspended Stacks", N_("Script calls paused when you saved, waiting to continue."));
				add("Queued Unbinds", N_("Scripts waiting to be detached from their objects."));
				// ---- change form types ----
				add("ACHR", N_("An actor reference: a placed NPC or creature, or you. Holds its position, inventory, AI, stats and more."));
				add("REFR", N_("An object reference: a placed object (item, door, container, furniture...) whose state changed."));
				add("NPC_", N_("An actor base: the NPC record itself (name, face, race, skills, factions, outfit), when the game changed it."));
				add("QUST", N_("A quest: its stage, objectives, flags and aliases."));
				add("CELL", N_("A cell (an interior or a square of the world): which parts of it changed."));
				add("FLST", N_("A form list: a list of forms some scripts add to and remove from."));
				add("FACT", N_("A faction: its flags, its relations to other factions, and crime counts."));
				// ---- change flags ----
				add("CHANGE_FORM_FLAGS", N_("The record's own flags changed (e.g. disabled, deleted, persistent)."));
				add("REFR_MOVE", N_("It was moved: its saved position, rotation and cell replace the plugin's."));
				add("REFR_HAVOK_MOVE", N_("Its physics (Havok) state is saved: how it lies or tumbles."));
				add("REFR_CELL_CHANGED", N_("It moved to another cell than the one the plugin places it in."));
				add("REFR_SCALE", N_("Its size was changed."));
				add("REFR_INVENTORY", N_("What it carries changed (items added or removed)."));
				add("REFR_EXTRA_OWNERSHIP", N_("Its owner changed."));
				add("REFR_BASEOBJECT", N_("What it is an instance of (its base object) changed."));
				add("REFR_PROMOTED", N_("It was made persistent (promoted), usually because a quest or script points at it."));
				add("REFR_ANIMATION", N_("Its animation state is saved (pose, idle, what it is doing)."));
				add("REFR_LEVELED_INVENTORY", N_("Its leveled items were rolled and saved."));
				add("REFR_EXTRA_ENCOUNTER_ZONE", N_("Its encounter zone (level range and reset rules) changed."));
				add("REFR_EXTRA_GAME_ONLY", N_("It carries extra data the game only keeps in saves."));
				add("REFR_EXTRA_CREATED_ONLY", N_("Extra data of an object created during play."));
				add("REFR_EXTRA_ACTIVATING_CHILDREN", N_("What it activates when used changed."));
				add("ACTOR_EXTRA_PACKAGE_DATA", N_("Data of the AI package it is running."));
				add("LIFESTATE", N_("Alive, dead, unconscious, bleeding out..."));
				add("ACTOR_EXTRA_MERCHANT_CONTAINER", N_("Its merchant chest."));
				add("ACTOR_EXTRA_DISMEMBERED_LIMBS", N_("Limbs it lost."));
				add("ACTOR_LEVELED_ACTOR", N_("Which actor its leveled list rolled."));
				add("LEVELED_BASE", N_("The base NPC the game builds for this actor from its template every time the save loads. It is a created "
				                    "FF form that is never stored in the save, so searching for it finds this actor instead. A crash log that "
				                    "names it means this actor."));
				add("LEVELED_TEMPLATE", N_("The plugin's NPC or leveled list this actor is built from."));
				add("ACTOR_DISPOSITION_MODIFIERS", N_("How it feels about others."));
				add("ACTOR_TEMP_MODIFIERS", N_("Temporary changes to its actor values."));
				add("ACTOR_DAMAGE_MODIFIERS", N_("Damage to its actor values (lost health, magicka, stamina)."));
				add("ACTOR_OVERRIDE_MODIFIERS", N_("Overrides of its actor values."));
				add("ACTOR_PERMANENT_MODIFIERS", N_("Permanent changes to its actor values."));
				add("ACTOR_BASE_DATA", N_("Base stats: level, offsets of health, magicka and stamina, speed."));
				add("ACTOR_BASE_AIDATA", N_("AI settings: aggression, confidence, morality..."));
				add("ACTOR_BASE_SPELLLIST", N_("Spells and shouts it knows."));
				add("ACTOR_BASE_FULLNAME", N_("Its name was changed."));
				add("ACTOR_BASE_FACTIONS", N_("Factions it belongs to."));
				add("NPC_SKILLS", N_("Its skills."));
				add("NPC_CLASS", N_("Its class."));
				add("NPC_FACE", N_("Its face (head parts, morphs, tint). Removing it gives back the plugin's face."));
				add("DEFAULT_OUTFIT", N_("What it wears normally."));
				add("SLEEP_OUTFIT", N_("What it wears to bed."));
				add("NPC_GENDER", N_("Its gender changed."));
				add("NPC_RACE", N_("Its race changed."));
				// ---- decoded groups and values ----
				add("INITIAL", N_("Where it is: cell, position and rotation, saved when it moved."));
				add("CELL", N_("The cell (interior, or a square of the world) it is in."));
				add("POS", N_("Position in game units (x, y, z)."));
				add("ROT", N_("Rotation in radians (x, y, z)."));
				add("HAVOK", N_("Physics state (how it lies, how it moves). Reset Havok clears it."));
				add("PROCESS LEVEL",
					N_("How much AI is loaded for this actor: 0 high (near you), 1 middle-high, 2 middle-low, 3 low, -1 none (not loaded)."));
				add("BOOL BITS", N_("Actor state flags (e.g. dead, in combat, ghost, essential...)."));
				add("EXTRADATA", N_("Extra data attached to it: owner, encounter zone, aliases, scene, package and many more."));
				add("INVENTORY", N_("Items it carries in addition to (or instead of) what the plugins give it."));
				add("ANIMATIONS", N_("Its animation graph state (the pose and animations it is in)."));
				add("AI PROCESS", N_("Its AI state: current package and procedure, targets, equipped items, dialogue, knowledge of others."));
				add("CURRENT PACKAGE", N_("The AI package it is running (follow, sandbox, travel, eat...) and how far it got."));
				add("PACKAGE", N_("An AI package: what the actor is told to do."));
				add("PACKAGE DATA", N_("The running package's own state (its targets and procedure)."));
				add("PACKAGE TYPE", N_("The package's procedure type (18 = a package built from a template, the usual kind)."));
				add("MIDDLE HIGH", N_("AI data kept while the actor is close to you."));
				add("HIGH", N_("AI data kept while the actor is close to you and active."));
				add("ACTOR", N_("The actor's own data: timers, state, spells being cast, actor values, added spells, movement and combat."));
				add("ACTOR VALUES", N_("Stats: Health, Magicka, skills, resistances, speed... Base is the saved value; modifiers change it."));
				add("BASE", N_("Saved base values of its actor values."));
				add("MODIFIERS", N_("Changes on top of the base values: permanent, temporary (active effects) and damage."));
				add("ADDED SPELLS", N_("Spells, powers and abilities given during play."));
				add("MOVEMENT CONTROLLER", N_("How the actor moves: its path, pathing requests and the parts that steer it."));
				add("COMBAT CONTROLLER", N_("Its combat state: target, aiming, combat area, weapons and spells it may use, behavior."));
				add("ARBITERS", N_("Parts of the movement controller that decide how it moves."));
				add("AGENTS", N_("Parts of the movement controller that apply the movement (path following, avoiding others...)."));
				add("PLAYER", N_("Data only the player has: skills progress, camera, crimes, perk points, race and more."));
				add("CAMERA", N_("The camera states (first person, third person, horse, dragon...)."));
				add("SKILLS", N_("Skill levels and progress toward the next level."));
				add("CRIME GOLD", N_("Your bounty and crime totals with each faction."));
				add("STOLEN ITEM VALUES", N_("The value of items stolen from each faction."));
				add("SKIN", N_("Your race and skin tint data."));
				add("PERK POINTS", N_("Unspent perk points."));
				add("XP", N_("Experience toward the next character level."));
				add("XP FOR NEXT LEVEL", N_("Experience needed to reach the next character level."));
				add("FACTIONRANKS", N_("Factions the actor belongs to and its rank in each."));
				add("ACBS", N_("Base stats: flags, health/magicka/stamina offsets, level, speed, disposition."));
				add("AIDT", N_("AI data: aggression, confidence, energy, morality, mood, assistance."));
				add("DNAM", N_("Skills and the health, magicka and stamina values."));
				add("FACEDATA", N_("Face: hair color, skin tone, head parts, morphs and presets."));
				// ---- actor values ----
				add("Aggression", N_("AI: 0 unaggressive, 1 aggressive (attacks enemies on sight), 2 very aggressive, 3 frenzied (attacks anyone)."));
				add("Confidence", N_("AI: 0 cowardly (always flees) to 4 foolhardy (never flees)."));
				add("Energy", N_("AI: how much the actor moves around while sandboxing."));
				add("Morality", N_("AI: 0 any crime to 3 no crime: what it agrees to do for you."));
				add("Mood", N_("AI: the facial mood it shows (neutral, angry, happy...)."));
				add("Assistance", N_("AI: 0 helps nobody, 1 helps allies, 2 helps friends and allies in a fight."));
				add("Magicka", N_("Magicka: spent casting spells."));
				add("Stamina", N_("Stamina: spent sprinting, power attacking and bashing."));
				add("HealRate", N_("Health regained per second while out of combat, in percent of the maximum."));
				add("MagickaRate", N_("Magicka regained per second, in percent of the maximum."));
				add("StaminaRate", N_("Stamina regained per second, in percent of the maximum."));
				add("SpeedMult", N_("Movement speed in percent (100 = normal). Odd values make you very slow or very fast."));
				add("InventoryWeight", N_("The weight of what you carry (the game recalculates it)."));
				add("CarryWeight", N_("How much you can carry before you are overencumbered."));
				add("CriticalChance", N_("Chance of a critical hit, in percent."));
				add("MeleeDamage", N_("Extra melee damage."));
				add("UnarmedDamage", N_("Damage of unarmed (fist) attacks."));
				add("Mass", N_("Physical mass (how hard you are to push)."));
				add("VoicePoints", N_("Shout power."));
				add("VoiceRate", N_("How fast shouts recover."));
				add("DamageResist", N_("Armor rating."));
				add("PoisonResist", N_("Resistance to poison, in percent."));
				add("ResistFire", N_("Resistance to fire, in percent."));
				add("ResistShock", N_("Resistance to shock, in percent."));
				add("ResistFrost", N_("Resistance to frost, in percent."));
				add("ResistMagic", N_("Resistance to all magic, in percent."));
				add("ResistDisease", N_("Resistance to disease, in percent."));
				add("Paralysis", N_("Above 0 while paralyzed. A stuck value leaves you frozen."));
				add("Invisibility", N_("Above 0 while invisible. A stuck value keeps you invisible."));
				add("NightEye", N_("Above 0 while Night Eye is active."));
				add("DetectLifeRange", N_("Range of Detect Life."));
				add("WaterBreathing", N_("Above 0 while you can breathe under water."));
				add("WaterWalking", N_("Above 0 while you can walk on water."));
				add("Fame", N_("Fame: good deeds the game counts."));
				add("Infamy", N_("Infamy: bad deeds the game counts."));
				add("JumpingBonus", N_("Extra jump height."));
				add("WardPower", N_("Strength of your active ward."));
				add("ArmorPerks", N_("Armor rating bonus from perks, in percent."));
				add("ShieldPerks", N_("Shield bonus from perks, in percent."));
				add("BowSpeedBonus", N_("Bow draw-speed bonus (slow time while aiming)."));
				add("WeaponSpeedMult", N_("Weapon swing speed multiplier (right hand). 0 or 1 = normal."));
				add("LeftWeaponSpeedMultiply", N_("Weapon swing speed multiplier (left hand)."));
				add("ShoutRecoveryMult", N_("Shout cooldown multiplier (1 = normal, lower = faster)."));
				add("MovementNoiseMult", N_("How much noise you make moving (1 = normal)."));
				add("DragonSouls", N_("Dragon souls not yet spent on shout words."));
				add("CombatHealthRegenMultiply", N_("Health regeneration during combat (vanilla: 0.7 for the player)."));
				add("Variable01", N_("A spare value the game and mods use for their own purposes."));
				add("WaitingForPlayer", N_("Set while a follower waits for you."));
				// ---- extra data ----
				add("Ownership", N_("Who owns it (an NPC or faction): taking it is stealing."));
				add("EncounterZone", N_("Its encounter zone: the level range of what spawns, and when it resets."));
				add("AliasInstanceArray", N_("Quest aliases it fills: the quests that point at it. Removing these can break quests."));
				add("LeveledCreature", N_("The leveled list this actor came from and the actor it rolled."));
				add("PromotedRef", N_("Why it is persistent: the things that point at it."));
				add("Package", N_("The package it is running."));
				add("PackageData", N_("The running package's data."));
				add("PackageStartLocation", N_("Where its current package started."));
				add("RunOncePacks", N_("Run-once packages it already ran."));
				add("SceneData", N_("The scene it is in."));
				add("FactionChanges", N_("Factions it joined or left during play, and rank changes."));
				add("DismemberedLimbs", N_("Limbs it lost."));
				add("AttachedArrows3D", N_("Arrows stuck in it."));
				add("Ghost", N_("It is a ghost (attacks pass through it)."));
				add("TextDisplayData", N_("A name set during play."));
				add("CanTalkToPlayer", N_("Whether it can talk to you."));
				add("FollowerSwimBreadcrumbs", N_("The trail it follows its leader by through water."));
				add("GuardedRefData", N_("What it remembers guarding."));
				add("PlayerCrimeList", N_("Crimes it saw you commit."));
				add("ForcedTarget", N_("A target it was forced to attack."));
				add("HeadingTarget", N_("Something it was told to turn toward."));
				add("ForcedLandingMarker", N_("Where it (usually a dragon) was forced to land."));
				add("FriendHits", N_("How often you hit it while it was friendly."));
				add("CachedScale", N_("Its scale, cached."));
				add("ActorCause", N_("What created it (e.g. a spell that summoned it)."));
				add("AshPileRef", N_("The ash pile it left (ghosts, vampires...)."));
				add("StartingWorldOrCell", N_("The world or cell it started in."));
				add("Worn", N_("Worn by the actor."));
				add("Count", N_("How many items this stack holds."));
				add("Charge", N_("Enchantment charge left."));
				add("Health", N_("For an actor: its health. For an item: its condition (tempering, 1.0 = normal)."));
				add("Enchantment", N_("An enchantment added during play (your own enchanting)."));
				add("LocRef", N_("A place given as a reference and a radius."));
				add("LocPt", N_("A place given as a point in a cell or world."));
				add("LocCell", N_("A place given as a cell."));
				return m;
			}();
			return g;
		}
	}

	const char* Explain(std::string_view a_label)
	{
		const auto& g = Glossary();
		if (const auto it = g.find(Key(a_label)); it != g.end()) {
			return tr(it->second);
		}
		return nullptr;
	}
}
