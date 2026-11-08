#pragma once

// What the in-game "ReSaver Plus" page (F1, SKSE Menu Framework; MenuPage.h) reads from the game and does in it
// (version 15). Every function here runs on the game's main thread: the page queues them with SKSE's task interface
// and shows the copies they return. Nothing here runs unless the page asks for it.
namespace LiveInfo
{
	// ---------------------------------------------------------------- the player's active effects
	enum class Group : std::uint8_t
	{
		kWorn,         // an enchantment of something the player wears
		kNotWorn,      // an item's enchantment, the item not worn (the stuck ones are among these)
		kSpells,       // spells, powers, shouts, scrolls
		kConsumables,  // potions, food, ingredients, poisons
		kAbilities,    // abilities (race, standing stone, perks, mods)
		kDiseases,     // diseases and addictions
		kOthers,       // put on the player by someone else
		kUnknown,      // no magic item (its plugin is gone)
		kCount
	};
	const char* GroupName(Group a_group);

	struct Effect
	{
		std::uint16_t uid = 0;  // ActiveEffect::usUniqueID: finds the effect again when it is removed
		std::string   name;
		float         magnitude = 0, elapsed = 0, duration = 0;
		std::uint32_t flags = 0;  // 0x8000 switched off (condition not met), 0x40000 ended (dispelled)
		bool          stuck = false;
		bool          ended() const { return (flags & 0x40000u) != 0; }
		bool          off() const { return !ended() && (flags & 0x8000u) != 0; }
	};

	struct Source
	{
		Group                group = Group::kUnknown;
		RE::FormID           spell = 0;  // the magic item (spell, enchantment, potion...)
		RE::FormID           item = 0;   // the object it came from (ActiveEffect::source), 0 if none
		bool                 fromOther = false;
		std::string          spellName, itemName;
		int                  spellType = -1;  // MagicSystem::SpellType
		bool                 itemWorn = false, itemCarried = false;
		std::string          stuckWhy;  // set when effects of this source are stuck
		std::vector<Effect>  effects;
		std::size_t          running = 0, stuckCount = 0;
	};

	struct Effects
	{
		bool                valid = false;  // a game is loaded
		std::vector<Source> sources;        // sorted by group
		std::size_t         total = 0, running = 0, off = 0, ended = 0, stuck = 0;
	};
	Effects ReadEffects();

	// The game's own ways (each verified in SkyrimSE.exe 1.6.1170, see the handoff note):
	//   one effect: ActiveEffect::Dispel(false), what ActiveMagicEffect.Dispel() does
	//   all of a source: a worn item is taken off (ActorEquipManager::UnequipObject, what Actor.UnequipItem does);
	//   an ability, disease or addiction is removed (Actor::RemoveSpell, what Actor.RemoveSpell does);
	//   anything else, or the stuck effects of an item not worn: each effect dispelled as above.
	// Each returns one line saying what was done.
	std::string RemoveEffect(std::uint16_t a_uid);
	std::string RemoveSource(RE::FormID a_spell, RE::FormID a_item, bool a_fromOther, bool a_onlyStuck);

	// ---------------------------------------------------------------- followers
	struct Follower
	{
		RE::FormID  id = 0;
		std::string key, name, why, where;  // why: vanilla follower quest / follower faction / teammate
		bool        skip = false;           // in sGatherSkip
		bool        teammate = false;
		bool        vanilla = false;  // held by the vanilla follower quest (DialogueFollower): can be dismissed with its stage 400
	};
	struct Followers
	{
		bool                  valid = false;
		std::vector<Follower> list;
		std::string           race;  // the player's race (editor id), to spot werewolf / Vampire Lord form
		bool                  werewolf = false, vampireLord = false;
	};
	Followers ReadFollowers();  // Rescue::ListFollowers plus the teammate flag

	// ---------------------------------------------------------------- quests
	struct QuestRow
	{
		RE::FormID    id = 0;
		std::string   editorID, name;
		std::uint16_t stage = 0;
	};
	std::vector<QuestRow> ReadRunningQuests();  // every running quest (Game::Running), by name

	struct QuestDetail
	{
		bool          found = false, running = false, completed = false;
		RE::FormID    id = 0;
		std::string   editorID, name;
		std::uint16_t stage = 0;
		struct Stage
		{
			std::uint16_t index = 0;
			std::uint8_t  flags = 0;  // 2 start-up stage, 4 shut-down stage
		};
		std::vector<Stage> stages;  // the stages the quest has, in the game's order
		struct Objective
		{
			std::uint16_t index = 0;
			std::uint8_t  state = 0;  // 0 not shown, 1 shown, 2/3 completed, 4/5 failed
			std::string   text;
		};
		std::vector<Objective> objectives;
	};
	QuestDetail ReadQuest(RE::FormID a_quest);

	// ---------------------------------------------------------------- script health (the Papyrus VM)
	struct VMSample
	{
		bool          valid = false;
		double        time = 0;           // seconds since the sampler started
		std::uint32_t stacks = 0;         // every script thread the VM has (allRunningStacks)
		std::uint32_t latent = 0;         // waiting on a latent call: Utility.Wait, timers... (waitingLatentReturns)
		std::uint32_t suspended = 0;      // waiting their turn (both suspend queues and their overflow arrays)
		std::uint32_t queuedCalls = 0;    // function calls queued, not started yet (uiWaitingFunctionMessages)
		std::uint32_t frozen = 0;         // frozenStacksCount
		std::uint32_t freezeState = 0;    // 0 not frozen, 1 freezing, 2 frozen (the VM freezes only while saving / loading)
		std::uint32_t unreadable = 0;     // stacks whose current function could not be read
		std::vector<std::pair<std::string, std::uint32_t>> topScripts;  // "Script.Function" of the current frame, most stacks first
	};
	VMSample SampleVM(double a_time);

	// ---------------------------------------------------------------- tests
	// Spawns a creature (Skyrim.esm base NPC: 0 wolf EncWolf 00023ABE, 1 deer EncDeer 000CF89D, 2 sabre cat EncSabreCat
	// 00023AB5) with Papyrus PlaceAtMe on the player, then MoveTo 300 units in front of the player. a_spawned gets the new
	// reference's form id (0 if it failed) on the main thread, once the VM has run the calls.
	std::string Spawn(int a_which, std::function<void(RE::FormID)> a_spawned);
	// Papyrus Actor.Kill(None) on a character (a reference form id); "" when dispatched, else why not
	std::string Kill(RE::FormID a_ref);
	// the character under the crosshair, or else the console's selected reference: 0 if none
	RE::FormID TargetActor(std::string& a_name);

	// ---------------------------------------------------------------- report
	// Writes <SKSE log folder>\ReSaverHelper-report.txt (game and helper versions, the cell, nearby characters with
	// problems, script health, plugin counts; a_extra: lines the page adds, e.g. the menu framework's version).
	// Returns the file, or "" (a_error says why).
	std::filesystem::path WriteReport(const VMSample& a_vm, const std::string& a_extra, std::string& a_error);

	// folders: SKSE log folder (Documents\My Games\<game>\SKSE) and the Papyrus log (...\Logs\Script\Papyrus.0.log)
	std::filesystem::path LogFolder();
	std::filesystem::path PapyrusLog();
}
