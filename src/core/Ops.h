// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save editing operations and analysis. Semantics follow ReSaver's Papyrus/ESS operations.
#pragma once

#include <unordered_map>

#include "GhostActors.h"
#include "Save.h"
#include "ScriptFlood.h"

#include <set>
#include <string>
#include <vector>

namespace ess::ops
{
	// ---- element classification (same rules as ReSaver) ---------------------------------
	bool IsNativeScriptName(const std::string& a_name);
	// Engine object types (Form, Actor, Quest, ReferenceAlias, ...). An "instance" of one of these
	// is only the game's link between a form and Papyrus, not a mod script that runs.
	bool IsEngineType(const std::string& a_name);
	bool IsScriptUndefined(const Papyrus& p, const Script& s);
	bool IsInstanceUndefined(const Papyrus& p, TStr a_script);
	bool IsUnattached(const ScriptInstance& s);
	bool IsFrameNative(const StackFrame& f);
	bool IsFrameZeroed(const StackFrame& f);
	bool IsThreadSuspended(const Papyrus& p, ThreadID id);
	bool IsThreadTerminated(const Papyrus& p, const ActiveScript& a);
	bool IsThreadUndefined(const Papyrus& p, const ActiveScript& a);
	bool IsCreatedWithoutForm(SaveFile& s, const ScriptInstance& i);
	// ReSaver's extra filters
	std::vector<std::string> ExtendedMembers(const Papyrus& p, TStr a_script);  // parents' members first
	bool                     IsMemberless(const Papyrus& p, const ScriptInstance& i);  // script has members, instance has no data
	bool                     HasCanary(const Papyrus& p, const ScriptInstance& i);     // ::iPapyrusDataVerification_var
	int                      CanaryValue(const Papyrus& p, const ScriptInstance& i);
	bool                     IsNoParent(const Papyrus& p, const Script& s);       // empty parent, not a base script
	bool                     IsMissingParent(const Papyrus& p, const Script& s);  // parent not defined in the save

	// ---- edits (each returns how many elements it changed) -------------------------------
	std::size_t DeleteInstances(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteReferences(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteArrays(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteThreads(Papyrus& p, const std::set<ThreadID>& a_ids);
	std::size_t TerminateThreads(Papyrus& p, const std::set<ThreadID>& a_ids);
	// Deletes threads together with their suspended stacks and queued function messages.
	std::size_t DeleteThreadsFully(Papyrus& p, const std::set<ThreadID>& a_ids);
	// Only the entries of the two suspended stack tables. Their threads stay: use RemoveSuspendedStacks unless the
	// caller deletes those threads itself.
	std::size_t DeleteSuspendedStacks(Papyrus& p, const std::set<ThreadID>& a_ids);
	std::size_t DeleteFunctionMessages(Papyrus& p, const std::set<std::size_t>& a_indices);

	// ---- threads and what holds them ------------------------------------------------------------
	// Every thread (stack) a save lists is held by exactly one of: an entry of a suspended stack table (it waits to make
	// its call), a queued function message with its id (the call it was made for), the latent-wait list (thread type
	// byte not 0: a latent native such as Utility.Wait), or the script engine's one extra slot (unk1 != 0, unk2 = its
	// id). Checked on every Skyrim SE and Fallout 4 save we have (31 + 168). SkyrimSE.exe 1.6.1170 counts exactly these
	// to decide the engine is paused (0x14517c0: suspend queues and their overflow arrays + queued messages + latent
	// waits + the extra slot >= running threads); a thread held by none of them is never resumed, never finishes, and
	// keeps the engine from pausing, so every later save of that game session is written with its Papyrus data
	// flagged invalid (see Papyrus::invalid).
	// Threads held by none of them.
	std::vector<ThreadID> StrandedThreads(const Papyrus& p);

	// Deletes that keep every thread held: a suspended stack, a queued call with a thread id, or a thread goes
	// together with the thread and everything else that names that thread.
	struct ThreadDelete
	{
		std::size_t threads = 0, stacks = 0, messages = 0;
	};
	ThreadDelete RemoveSuspendedStacks(Papyrus& p, const std::set<ThreadID>& a_ids);        // by thread id
	ThreadDelete RemoveFunctionMessages(Papyrus& p, const std::set<std::size_t>& a_indices);  // by index
	ThreadDelete RemoveThreads(Papyrus& p, const std::set<ThreadID>& a_ids);
	ThreadDelete RemoveStrandedThreads(Papyrus& p);
	std::size_t DeleteUnbinds(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteScripts(Papyrus& p, const std::set<TStr>& a_names);  // also deletes their instances
	std::size_t DeleteChangeForms(SaveFile& s, const std::set<std::uint32_t>& a_refIDs);

	// ReSaver "Clean" menu
	std::size_t RemoveUnattachedInstances(Papyrus& p);
	std::size_t RemoveUndefinedElements(Papyrus& p);  // scripts, instances, references; zeroes undefined threads
	std::size_t TerminateUndefinedThreads(Papyrus& p);
	std::size_t RemoveNonexistentCreated(SaveFile& s);

	// Removes every trace of a script that is known to loop forever: its threads (any frame
	// running that script or running on one of its instances), their suspended stacks and
	// function messages, queued calls that pass one of its instances, its instances, and
	// optionally the created references those instances were attached to.
	// a_keepInstances: only remove the running/queued work (threads, stacks, messages) and keep
	// the instances with their data, so properties other scripts hold to them stay valid. Use
	// this for a mod you keep installed (e.g. a stuck MCM registration).
	// ---- ActiveMagicEffect instances whose effect is still running -------------------------------
	// An ActiveMagicEffect script instance is saved under its effect's Papyrus handle: unknown2bits 2, unknown = the
	// effect's unique id (ActiveEffect +0x84 in SkyrimSE.exe 1.6.1170, read by ActiveEffect::LoadGame 0x5ad0d0 after
	// 16 bytes, four 3-byte refs, the hit list's vsval count and a u32: effect data offset 33), RefID = the actor
	// (Fallout 4 the same, see fo4cf::ActiveEffect::uniqueID). Checked on Tony's saves: every such handle names a unique
	// id the actor's saved effects use or an actor whose effects are not saved, and no handle uses unique id 0.
	// When the effect is still in the actor's saved active effect list, the cleanups below keep the instance (and its
	// script definition): removing only the script would hand the game a running effect whose script object is gone,
	// and taking the effect off the actor as well is the kind of edit that came right before the load crash in Tony's
	// Save140 (docs/CLAUDE-HANDOFF.md, "Save140"). The game itself wrote the effect and its instance together, so
	// keeping both changes nothing the game did not already have.
	struct KeptInstance
	{
		EID           id = 0;
		std::string   script;
		std::uint32_t actor = 0;     // the actor's form ID
		std::uint16_t uniqueID = 0;  // the effect's unique id
		bool          unread = false;  // the actor's effects could not be read to the unique ids: kept to be safe
	};
	// The instances of a_candidates that are kept. a_changeFormsGoing: change forms (refID.data) the same edit removes;
	// an actor among them takes its effects with it, so its instances are not kept.
	std::vector<KeptInstance> LiveEffectInstances(const SaveFile& s, const std::set<EID>& a_candidates,
		const std::set<std::uint32_t>* a_changeFormsGoing = nullptr);
	// The same, and takes them out of a_ids.
	std::vector<KeptInstance> KeepLiveEffectInstances(const SaveFile& s, std::set<EID>& a_ids, const std::set<std::uint32_t>* a_changeFormsGoing = nullptr);
	std::string               KeptText(const std::vector<KeptInstance>& a_kept);  // details for the log ("" when none)

	struct CleanResult
	{
		std::size_t               removed = 0;
		std::vector<KeptInstance> kept;
	};
	// Remove unattached instances / undefined elements on a save: as the Papyrus versions, but an ActiveMagicEffect
	// instance whose effect is still running stays (and its script definition with it).
	CleanResult RemoveUnattachedInstances(SaveFile& s);
	CleanResult RemoveUndefinedElements(SaveFile& s);
	// DeleteScripts on a save: a script with a kept instance stays, with that instance.
	CleanResult DeleteScripts(SaveFile& s, const std::set<TStr>& a_names);

	struct PurgeResult
	{
		std::size_t               threads = 0, stacks = 0, messages = 0, instances = 0, forms = 0;
		std::vector<KeptInstance> kept;  // ActiveMagicEffect instances left because their effect is running
	};
	PurgeResult PurgeScript(SaveFile& s, const std::string& a_scriptName, bool a_deleteCreatedForms, bool a_keepInstances = false);
	// The same for a set of instances: their threads (frames running on them), those threads' suspended stacks and queued
	// calls, calls made on them or passing them, their queued unbinds, then the instances.
	PurgeResult PurgeInstances(SaveFile& s, const std::set<EID>& a_ids);
	// What would still name one of these instances after PurgeInstances: variables of other instances, references,
	// arrays, struct instances, and frames of threads PurgeInstances keeps (one line each; empty: nothing).
	std::vector<std::string> ReferrersOutside(const Papyrus& p, const std::set<EID>& a_ids);

	// SkyUI MCM registration jam: removes every thread that is registering a menu (any frame in
	// OnConfigManagerReady / OnConfigManagerReset) or running inside SKI_ConfigManager, their
	// suspended stacks and queued calls, and puts SKI_ConfigManager back into its normal state if
	// the save caught it in its "BUSY" state. Menus that were already registered stay registered;
	// SkyUI sends "config manager ready" again on the next load and the rest register then.
	struct McmFixResult
	{
		std::size_t threads = 0, stacks = 0, messages = 0;
		bool        managerFound = false, stateReset = false;
		std::size_t registered = 0;  // menus already registered (non-empty _modConfigs entries)
		std::string text() const;
	};
	std::size_t  CountMcmJam(const Papyrus& p);  // threads that a fix would remove
	McmFixResult FixMcmJam(Papyrus& p, bool a_dryRun = false);

	// ReSaver's plugin actions: delete a plugin's change forms and/or the script instances on its
	// forms (threads running on those instances go with them).
	struct PluginPurge
	{
		std::set<std::uint32_t> changeForms;  // refID.data
		std::set<EID>           instances;
		std::set<ThreadID>      threads;
	};
	PluginPurge SelectPluginElements(const SaveFile& s, const std::set<std::string>& a_plugins, bool a_changeForms, bool a_instances);
	// returns the ActiveMagicEffect instances it left (their effect is running on an actor the purge keeps)
	std::vector<KeptInstance> ApplyPluginPurge(SaveFile& s, const PluginPurge& a_sel);

	// ---- queries ---------------------------------------------------------------------------
	struct Referrer
	{
		enum class Kind
		{
			kInstance,
			kReference,
			kArray,
			kThread,
			kSuspended,
			kMessage
		} kind;
		std::uint64_t id = 0;
		std::string   where;
	};
	std::vector<Referrer> FindReferrers(const Papyrus& p, EID a_target);

	// ---- analysis ------------------------------------------------------------------------
	struct Count
	{
		std::string name;
		std::size_t n = 0;
	};
	struct Report
	{
		std::size_t scripts = 0, instances = 0, references = 0, arrays = 0, threads = 0, functionMessages = 0, suspended = 0, changeForms = 0, strings = 0;
		std::size_t unattached = 0, undefinedScripts = 0, undefinedInstances = 0, undefinedThreads = 0, terminatedThreads = 0, createdWithoutForm = 0;
		std::size_t stranded = 0;  // StrandedThreads (Skyrim SE)
		std::vector<Count> threadsByTopScript;      // script of the event frame (frame 0)
		std::vector<Count> threadsByCurrentFrame;   // innermost frame: what the thread is waiting in
		std::vector<Count> suspendedByFunction;
		std::vector<Count> messagesByFunction;
		std::vector<Count> instancesByScript;
		std::vector<Count> changeFormsByType;
		std::vector<std::string> warnings;          // plain-language findings
		std::vector<std::string> loopingScripts;    // scripts with 20+ threads parked in Utility.Wait
		bool                     mcmJam = false;    // SkyUI registration was mid-way (FixMcmJam applies)
		std::vector<flood::Flood> floods;           // script-call floods (flood::Find)
		std::vector<ghost::Actor> ghosts;           // spawned actors whose created base is not in the save (ghost::Find)
	};
	Report Analyze(SaveFile& s);
	std::string FormatReport(const Report& r, std::size_t a_top = 20);

	// Points the save at a plugin that was renamed (Mod.esp -> Mod.esl, ...): the save's plugin list entry is
	// renamed, so the game finds the plugin again by its new name and keeps everything the save holds from it.
	// Only for a rename: if the plugin's records were renumbered (compacted), the save's ids no longer match.
	// Returns "" when done, else why not.
	std::string RenamePlugin(SaveFile& s, const std::string& a_from, const std::string& a_to);
	// Installed plugins that look like a renamed copy of a_missing (same name, other extension).
	std::vector<std::string> RenameCandidates(const SaveFile& s, const std::string& a_missing, const std::vector<std::string>& a_installed);
	// the same for many missing plugins at once (the save and the installed list read once)
	class RenameFinder
	{
	public:
		RenameFinder(const SaveFile& s, const std::vector<std::string>& a_installed);
		std::vector<std::string> find(const std::string& a_missing) const;

	private:
		std::unordered_map<std::string, std::vector<std::string>> _byStem;
	};
}
