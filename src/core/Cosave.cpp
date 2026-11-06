// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Script extender co-saves (.skse / .f4se): see Cosave.h for the format and where it comes from.
#include "Cosave.h"

#include "ByteIO.h"
#include "ScriptSources.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace ess::cosave
{
	namespace
	{
		constexpr std::uint32_t kSKSE = 0x45534B53;  // file bytes "SKSE" (MACRO_SWAP32('SKSE'))
		constexpr std::uint32_t kF4SE = 0x45533446;  // "F4SE"

		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[512];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		// the value of a multi-character literal: 'SKEE' = 0x534B4545, 'OST' = 0x004F5354
		constexpr std::uint32_t MC(const char* s)
		{
			std::uint32_t v = 0;
			for (; *s; ++s) {
				v = (v << 8) | static_cast<std::uint8_t>(*s);
			}
			return v;
		}
		constexpr std::uint32_t BS(std::uint32_t v)  // _byteswap_ulong / std::byteswap
		{
			return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
		}

		bool Printable(std::uint8_t c) { return c >= 0x20 && c < 0x7F; }
	}

	// ---------------------------------------------------------------------------------------------- reading

	const Plugin* File::find(std::uint32_t a_uid) const
	{
		for (const auto& p : plugins) {
			if (p.uid == a_uid) {
				return &p;
			}
		}
		return nullptr;
	}

	bool LooksLikeCosave(const std::vector<std::uint8_t>& a_bytes)
	{
		if (a_bytes.size() < 20) {
			return false;
		}
		std::uint32_t sig;
		std::memcpy(&sig, a_bytes.data(), 4);
		return sig == kSKSE || sig == kF4SE;
	}

	File Parse(const std::vector<std::uint8_t>& a_bytes)
	{
		File   f;
		Reader r(a_bytes.data(), a_bytes.size());
		f.size = a_bytes.size();
		f.signature = r.u32();
		if (f.signature != kSKSE && f.signature != kF4SE) {
			throw FormatError("not an SKSE / F4SE co-save (the file does not start with \"SKSE\" or \"F4SE\")", 0);
		}
		f.fo4 = f.signature == kF4SE;
		f.formatVersion = r.u32();
		f.xseVersion = r.u32();
		f.runtimeVersion = r.u32();
		f.numPlugins = r.u32();
		if (f.formatVersion != 1) {
			f.notes.push_back(Fmt("format version %u (the script extenders write 1 and refuse anything newer)", f.formatVersion));
		}
		// as the loader: plugin blocks while a whole plugin header is left
		while (r.remaining() >= 12) {
			const std::size_t at = r.pos();
			Plugin            p;
			p.uid = r.u32();
			p.declaredChunks = r.u32();
			p.declaredLength = r.u32();
			if (p.declaredLength > r.remaining()) {
				// the loader would read the chunks it can and then seek past the end: keep the rest as it is
				f.notes.push_back(Fmt("plugin %s at byte %zu says %u bytes, only %zu are left", IdText(p.uid).c_str(), at, p.declaredLength, r.remaining()));
				p.regular = false;
				p.raw = r.bytes(r.remaining());
				f.plugins.push_back(std::move(p));
				break;
			}
			p.raw = r.bytes(p.declaredLength);
			Reader c(p.raw.data(), p.raw.size());
			while (c.remaining() >= 12) {
				Chunk ch;
				ch.type = c.u32();
				ch.version = c.u32();
				const std::uint32_t len = c.u32();
				if (len > c.remaining()) {
					p.regular = false;
					break;
				}
				ch.data = c.bytes(len);
				p.chunks.push_back(std::move(ch));
			}
			if (c.remaining() != 0 || p.chunks.size() != p.declaredChunks) {
				p.regular = false;
			}
			if (!p.regular) {
				f.notes.push_back(Fmt("plugin %s: its %u chunk(s) do not fill its %u bytes exactly (kept as stored)", IdText(p.uid).c_str(),
					p.declaredChunks, p.declaredLength));
			}
			f.plugins.push_back(std::move(p));
		}
		if (r.remaining()) {
			f.trailing = r.bytes(r.remaining());
			f.notes.push_back(Fmt("%zu byte(s) after the last plugin (ignored by the loader, kept)", f.trailing.size()));
		}
		if (f.numPlugins != f.plugins.size()) {
			f.notes.push_back(Fmt("the header counts %u plugin(s), the file holds %zu (the loader does not use the count)", f.numPlugins, f.plugins.size()));
		}
		return f;
	}

	std::vector<std::uint8_t> Write(const File& a_file)
	{
		Writer w;
		w.u32(a_file.signature);
		w.u32(a_file.formatVersion);
		w.u32(a_file.xseVersion);
		w.u32(a_file.runtimeVersion);
		w.u32(a_file.numPlugins);
		for (const auto& p : a_file.plugins) {
			w.u32(p.uid);
			if (!p.regular) {
				w.u32(p.declaredChunks);
				w.u32(p.declaredLength);
				w.bytes(p.raw);
				continue;
			}
			std::size_t len = 0;
			for (const auto& c : p.chunks) {
				len += 12 + c.data.size();
			}
			w.u32(static_cast<std::uint32_t>(p.chunks.size()));
			w.u32(static_cast<std::uint32_t>(len));
			for (const auto& c : p.chunks) {
				w.u32(c.type);
				w.u32(c.version);
				w.u32(static_cast<std::uint32_t>(c.data.size()));
				w.bytes(c.data);
			}
		}
		w.bytes(a_file.trailing);
		return std::move(w.buf);
	}

	// ---------------------------------------------------------------------------------------------- names

	std::string Hex(std::uint32_t a_uid) { return Fmt("%08X", a_uid); }

	std::string FourCC(std::uint32_t a_v)
	{
		std::string s;
		bool        lead = true;
		for (int sh = 24; sh >= 0; sh -= 8) {
			const auto c = static_cast<std::uint8_t>(a_v >> sh);
			if (lead && c == 0 && sh) {
				continue;  // 'OST' = 0x004F5354
			}
			lead = false;
			s += Printable(c) ? static_cast<char>(c) : '.';
		}
		return s;
	}

	std::string IdText(std::uint32_t a_uid)
	{
		if (a_uid == 0) {
			return "0";
		}
		const auto cc = FourCC(a_uid);
		if (cc.find('.') == std::string::npos && cc.size() >= 3) {
			return "'" + cc + "'";
		}
		// a byte-swapped literal (_byteswap_ulong('OST')) reads right the other way round
		const auto sw = FourCC(BS(a_uid));
		if (sw.find('.') == std::string::npos && sw.size() >= 3) {
			return Hex(a_uid) + " (byteswap '" + sw + "')";
		}
		return Hex(a_uid) + " (" + cc + ")";
	}

	std::string PackedVersion(std::uint32_t a_v)
	{
		// MAKE_EXE_VERSION_EX(major, minor, build, sub): major << 24 | minor << 16 | build << 4 | sub
		const unsigned major = a_v >> 24, minor = (a_v >> 16) & 0xFF, build = (a_v >> 4) & 0xFFF, sub = a_v & 0xF;
		std::string    s = Fmt("%u.%u.%u", major, minor, build);
		if (sub) {
			s += Fmt(".%u", sub);
		}
		return s;
	}

	const std::vector<KnownPlugin>& KnownTable()
	{
		// Every id below is the value passed to SetUniqueID in the plugin's source (GitHub repository and file
		// named), or, where marked "DLL", the immediate passed to SetUniqueID in the shipped DLL (found by
		// FindIds on a real install, 2026-11-06). game: 0 Skyrim, 1 Fallout 4, 2 both.
		static const std::vector<KnownPlugin> t = {
			{ 0, "Script extender (its own data)", "", "skse64/InternalSerialization.cpp, f4se/InternalSerialization.cpp: SetUniqueID(0, 0)", 2 },
			// Skyrim
			{ MC("SKEE"), "RaceMenu (skee64)", "skee64.dll", "expired6978/SKSE64Plugins skee64/main.cpp", 0 },
			{ MC("NIOV"), "NiOverride (LE)", "nioverride.dll", "expired6978/SKSEPlugins skse/nioverride/main.cpp", 0 },
			{ MC("FCGN"), "CharGen (LE RaceMenu)", "chargen.dll", "expired6978/SKSEPlugins skse/chargen/main.cpp", 0 },
			{ papu::kUid, "PapyrusUtil (StorageUtil)", "PapyrusUtil.dll", "eeveelo/PapyrusUtil main.cpp: 884715692 + 227106806", 0 },
			{ jc::kUid, "JContainers", "JContainers64.dll", "SilverIce/JContainers src/jcontainers_constants.h: storage_chunk = 'JSTR'", 0 },
			{ MC("P3PE"), "powerofthree's Papyrus Extender", "po3_PapyrusExtender.dll", "powerof3/PapyrusExtenderSSE include/Serialization/Manager.h", 0 },
			{ MC("SPID"), "Spell Perk Item Distributor (outfits)", "po3_SpellPerkItemDistributor.dll", "powerof3/Spell-Perk-Item-Distributor SPID/src/Outfits/OutfitsManager+SaveLoad.cpp", 0 },
			{ MC("FECK"), "Frozen Electrocuted Combustion (FEC)", "po3_FEC.dll", "powerof3/FEC include/Serialization.h", 0 },
			{ MC("SESN"), "Seasons of Skyrim", "po3_SeasonsOfSkyrim.dll", "powerof3/SeasonsOfSkyrim include/Papyrus.h", 0 },
			{ BS(MC("OST")), "OStim NG", "OStim.dll", "VersuchDrei/OStimNG skse/src/Main.cpp: _byteswap_ulong('OST')", 0 },
			{ BS(MC("OLVN")), "OStim Lovense", "", "VersuchDrei/OStimLovense src/Main.cpp", 0 },
			{ MC("OSLA"), "OSL Aroused", "OSLAroused.dll", "ozooma10/OSLAroused src/PersistedData.h", 0 },
			{ MC("slpp"), "SexLab P+", "SexLabUtil.dll", "SLP-Community/SexLabpp src/main.cpp", 0 },
			{ BS(MC("SLAN")), "SexLab Aroused NG", "", "clayne/SexlabArousedNG src/Main.cpp", 0 },
			{ BS(MC("SLIF")), "SLIF NG", "", "crajjjj/SLIFNG src/main.cpp", 0 },
			{ BS(MC("DDNG")), "Devious Devices NG", "DeviousDevices.dll", "IHateMyKite/DeviousDevicesNG src/Main.cpp", 0 },
			{ MC("DDi1"), "Devious Devices (dd_plugin)", "", "DeviousDevices/DDi 00 Core/dd_plugin_src/main.cpp", 0 },
			{ BS(MC("BF10")), "Beeing Female NG", "", "crajjjj/Beeing-Female-NG src/main.cpp", 0 },
			{ BS(MC("GTSP")), "Size Matters (GTS plugin)", "", "Sermitse/GTS_Plugin src/xSEPlugin.cpp", 0 },
			{ BS(MC("DEVR")), "Devourment", "", "clayne/DevourmentPlugin src/main.cpp", 0 },
			{ BS(MC("SVPP")), "Skyrim Vore++", "", "Mr12451/SkyrimVorePP src/XSEPlugin.cpp", 0 },
			{ BS(MC("SODS")), "State of Dress", "", "SkyLover37/State-of-Dress src/Main.cpp", 0 },
			{ MC("SOS"), "Schlongs of Skyrim (AE NG)", "", "SpongeBobHentaiSimulator/Schlongs-of-Skyrim-AE-NG src/main.cpp", 0 },
			{ MC("DbSF"), "Dylbill's DbSkseFunctions", "DbSkseFunctions.dll", "Dylbill-Iroh/DbSkseFunctions DbSkseFunctions/plugin.cpp", 0 },
			{ MC("SKpi"), "Coc On Load (Dylbill)", "", "Dylbill-Iroh/Skyrim_CocOnLoad plugin.cpp", 0 },
			{ MC("QOLI"), "QOL Item Tags (Dylbill)", "", "Dylbill-Iroh/Skyrim_QOLItemTags plugin.cpp", 0 },
			{ MC("CTTL"), "skyrim-character-menu (jpsteel)", "", "jpsteel/skyrim-character-menu src/main.cpp", 0 },
			{ MC("AAVF"), "Dynamic Armor Variants", "DynamicArmorVariants.dll", "Exit-9B/DynamicArmorVariants src/main/Serialization.h", 0 },
			{ MC("CSFW"), "Custom Skills Framework", "CustomSkills.dll", "Exit-9B/CustomSkills src/CustomSkills/Serialization.h", 0 },
			{ MC("FEC0"), "Follower Equip Control", "", "iRonoa9/FollowerEquipControl src/core/serialization/Serialization.cpp", 0 },
			{ MC("OIFL"), "Object Impact Framework", "", "arachnocid/ObjectImpactFramework src/RuleManager.cpp", 0 },
			{ MC("ECF"), "Scrambled Bugs", "ScrambledBugs.dll", "KernalsEgg/SKSE64Plugins ScrambledBugs/Include/Serialization.h", 0 },
			{ MC("ASDS"), "Simple Dual Sheath", "SimpleDualSheath.dll", "SlavicPotato/SimpleDualSheath SimpleDualSheath/SDS/Main.cpp", 0 },
			{ MC("EWBC"), "CBP Physics (SlavicPotato)", "", "SlavicPotato/CBPSSE CBP/Drivers/events.cpp", 0 },
			{ MC("SWE1"), "Dynamic Wetness", "", "DanjelPiDev/TES5-DynamicWetness include/Main.h", 0 },
			{ MC("achr"), "Acheron", "Acheron.dll", "KrisV-777/Acheron src/main.cpp", 0 },
			{ MC("dbd"), "Dynamic Body Distribution", "", "KrisV-777/Dynamic-Body-Distribution src/main.cpp", 0 },
			{ MC("NiNi"), "Nightmare Night", "", "KrisV-777/NightmareNight-SKSE src/main.cpp", 0 },
			{ MC("scpe"), "Scrab's Papyrus Extender", "", "KrisV-777/Papyrus-Extender src/Serialization/Serialization.h", 0 },
			{ MC("cuco"), "ConsoleUtil Extended", "", "KrisV-777/ConsoleUtil-Extended src/Serialization/Serialization.h", 0 },
			{ MC("EXPE"), "Experience", "Experience.dll", "zax-ftw/Experience src/Main.cpp", 0 },
			{ BS(MC("NGD")), "Next-Gen Decapitations", "", "Seb263/SkyrimSE_NextGenDecapitations src/Serialization.hpp: std::byteswap('NGD')", 0 },
			{ BS(MC("DISM")), "Dismembering Framework", "", "Seb263/SkyrimSE_DismemberingFramework src/Serialization.hpp: std::byteswap('DISM')", 0 },
			{ MC("DPF1"), "Dynamic Persistent Forms", "", "Thiago099/DPF-Dynamic-Persistent-Forms SKSE_Plugin/plugin.cpp", 0 },
			{ MC("OMO9"), "Object Manipulation Overhaul", "", "Thiago099/Object-Manipulation-Overhaul src/plugin.cpp", 0 },
			{ MC("EEFr"), "Enchantment Framework", "", "egocarib/EnchantmentFramework src/main.cpp", 0 },
			{ MC("IEQP"), "iEquip", "iEquip.dll", "pijaczkar8/iEquipUtil src/main.cpp", 0 },
			{ MC("JNKT"), "Junk It NG", "", "raziell74/skyrim-junk-it-ng-skse src/plugin.cpp", 0 },
			{ MC("EHKS"), "Extended Hotkey System", "", "Vermunds/ExtendedHotkeySystem-SE src/main.cpp", 0 },
			{ BS(MC("UIHS")), "Hotkeys System (neogulcity)", "", "neogulcity/HotkeysSystem src/Main.cpp", 0 },
			{ MC("SHSE"), "SmartHarvest SE", "", "SteveTownsend/SmartHarvestSE src/main.cpp", 0 },
			{ MC("QTFM"), "Flight Framework", "", "QTR-Modding/Flight-Framework src/Cosave.cpp", 0 },
			{ MC("BOKP"), "Books of Power", "", "QTR-Modding/BooksOfPower src/Persistence.cpp", 0 },
			{ MC("DSTV"), "Project Staff", "", "QTR-Modding/ProjectStaff src/Persistence.cpp", 0 },
			{ BS(MC("BPAP")), "PAPER (Papyrus Extender for Papyrus Extender Requirements)", "", "DennisSoemers/PAPER src/Main.cpp", 0 },
			{ BS(MC("XPBD")), "MXPBD", "", "Acro748/MXPBD src/Main.cpp", 0 },
			{ BS(MC("MDNM")), "Mu Dynamic Normal Map", "", "Acro748/MuDynamicNormalMap src/Main.cpp", 0 },
			{ BS(MC("MFEE")), "Mu Facial Expression Extended", "", "Acro748/MuFacialExpressionExtended src/Main.cpp", 0 },
			{ BS(MC("MKNC")), "Mu CK Keyword Name Conditions", "", "Acro748/MuCKKeywordNameConditions src/Main.cpp", 0 },
			{ BS(MC("MUJF")), "Mu Joint Fix NG", "", "Acro748/MuJointFix-NG src/Main.cpp", 0 },
			{ BS(MC("MUIF")), "Mu Impact Framework", "", "Acro748/MuImpactFramework src/Main.cpp", 0 },
			{ MC("VISN"), "NPC Area (vinymayan)", "", "vinymayan/NPC-Area src/plugin.cpp", 0 },
			{ MC("NSMV"), "New Skill Menu (vinymayan)", "", "vinymayan/New-Skill-Menu src/plugin.cpp", 0 },
			{ MC("MOVE"), "AutoMove NG", "", "GregLando113/AutoMove-NG src/main.cpp", 0 },
			{ MC("DIRP"), "Skyrim Directional Combat", "", "machinegod420/SkyrimDirectionalCombat src/main.cpp", 0 },
			{ MC("ERHS"), "Erin Transform Head Scale Fix", "", "ItsSpyce/ErinTransformHeadScaleFix src/main.cpp", 0 },
			{ MC("BQNG"), "Bounty Quests Redone NG", "", "digital-apple/BountyQuestsRedoneNG source/Main.cpp", 0 },
			{ MC("FLSR"), "Follower Leveling System Redone", "", "TrumanGIT/Follower-Leveling-System-Redone src/plugin.cpp", 0 },
			{ MC("LUMA"), "LumaUtil", "", "Minizbot2012/LumaUtil src/Main.cpp", 0 },
			{ MC("DFAI"), "Double Favorite As Important", "", "JerryYOJ/DoubleFavoriteAsImportant-SKSE src/plugin.cpp", 0 },
			{ MC("StGE"), "Sleep To Gain Experience", "", "Zebrina/SkyrimSleepToGainExperience Main.cpp", 0 },
			{ MC("GPHK"), "Grappling Hook", "", "clayne/GrapplingHook src/main.cpp", 0 },
			{ 0xDAF6A2A7, "Dynamic Appearance Framework", "", "clayne/DynamicAppearanceFramework src/DAFPlugin/src/main.cpp", 0 },
			{ 0x360FC0D6, "Go To Bed sleepwear", "", "andrelo1/gotobed-sleepwear-se src/init.cpp", 0 },
			{ MC("BSTY"), "Dragonborn's Bestiary", "", "Juan-MZ/DragonbornsBestiary-1.7.99-Patch source/complete-source/src/main.cpp", 0 },
			{ MC("IICH"), "Inventory Injector Improved", "", "codepuncher/InventoryInjectorImproved src/Plugin.cpp", 0 },
			{ MC("TFNG"), "True Flasks NG", "", "Newrite/TrueFlasksNG src/main.cpp", 0 },
			{ MC("TITD"), "Tempered Item Distribution", "", "adhj1400/TemperedItemDistribution src/main.cpp", 0 },
			{ MC("HSKL"), "Headshots Kill", "", "DCCStudios/SkyrimSE-HeadshotsKillSKSE src/main.cpp", 0 },
			{ MC("CMMK"), "Custom Markers", "", "rijosan87/CustomMarkers src/plugin.cpp", 0 },
			{ MC("PYSR"), "Pace Yourself", "", "redrew89/paceyourself-re src/main.cpp", 0 },
			{ MC("SLAM"), "SLAM (qudix)", "", "qudix/skse-slam src/Papyrus.h", 0 },
			{ MC("NET"), "Non-Exploitable Torches", "", "hydraaawr/Non-Exploitable-Torches src/main.cpp", 0 },
			{ BS(MC("IOS_")), "Outfit System (Idrinth)", "", "Idrinth/OutfitSystem src/OutfitSystem.cpp", 0 },
			{ BS(MC("OFB")), "OFootBlock", "", "xn2018/OFootBlock src/Main.cpp", 0 },
			{ MC("CPPS"), "Complete Perk Points Control", "", "ebaryl/skyrim-perk-points-control CustomSkillGlobals.cpp", 0 },
			{ MC("DDWP"), "DungeonDoorWarp", "DungeonDoorWarp.dll", "Tonycubed2 DungeonDoorWarp src/Main.cpp", 0 },
			// found in the DLL (SetUniqueID immediate), no public source checked
			{ MC("AVG"), "Actor Value Generator", "ActorValueGenerator.dll", "DLL: mov edx, imm32 in ActorValueGenerator.dll", 0 },
			{ MC("GAIA"), "CHIM / AI Agent", "AIAgent.dll", "DLL: mov edx, imm32 in AIAgent.dll", 0 },
			{ MC("PORD"), "Death Drop Overhaul", "DeathDropOverhaul.dll", "DLL: mov edx, imm32 in DeathDropOverhaul.dll", 0 },
			{ MC("NYDS"), "DynDOLOD DLL NG", "DynDOLOD.dll", "DLL: mov edx, imm32 in DynDOLOD.DLL", 0 },
			{ MC("FLBM"), "Face Lighting", "FaceLighting.dll", "DLL: mov edx, imm32 in FaceLighting.dll", 0 },
			{ MC("STYL"), "SkyPatcher", "SkyPatcher.dll", "DLL: mov edx, imm32 in SkyPatcher.dll", 0 },
			{ MC("SIBU"), "Sleep in Bed SKSE", "SleepInBedSKSE.dll", "DLL: mov edx, imm32 in SleepInBedSKSE.dll", 0 },
			{ MC("SICU"), "Unsearched Corpse Indicator", "UnsearchedCorpsesIndicator.dll", "DLL: mov edx, imm32 in UnsearchedCorpsesIndicator.dll", 0 },
			{ 0xE219D7E9, "Spell Hotbar", "SpellHotbar.dll", "DLL: mov edx, imm32 in SpellHotbar.dll", 0 },
			{ 0x0FB37673, "STB Widgets", "STB_Widgets.dll", "DLL: mov edx, imm32 in STB_Widgets.dll", 0 },
			// Fallout 4
			{ MC("F4EE"), "LooksMenu (F4EE)", "f4ee.dll", "expired6978/F4SEPlugins f4ee/main.cpp", 1 },
			{ MC("MCM"), "Mod Configuration Menu", "mcm.dll", "reg2k/f4mcm src/MCM.cpp", 1 },
			{ MC("SAM"), "Screen Archer Menu", "ScreenArcherMenu.dll", "maximusmaxy/ScreenArcherMenu SAM/main.cpp", 1 },
			{ MC("SAF"), "Screen Archer Framework", "", "maximusmaxy/ScreenArcherMenu SAF/main.cpp", 1 },
			{ MC("NAF"), "Native Animation Framework", "NAF.dll", "Deweh/Native-Animation-Framework f4se-plugin/src/main.cpp", 1 },
			{ MC("CCAM"), "Custom Camera", "", "qudix/f4se-custom-camera src/main.cpp", 1 },
			{ MC("BPCS"), "Baka Perk Cards", "", "shad0wshayd3-FO4/BakaPerkCards src/PerkCard/PerkCard.h", 1 },
			{ MC("MSF"), "Mod Switch Framework", "", "CasualCoderGuy/FO4-ModSwitchFramework-src main.cpp", 1 },
			{ MC("CHW"), "Shav's F4SE plugins (chw)", "", "shavkacagarikia/ShavF4SEPlugins src/chw/main.cpp", 1 },
			{ MC("PRKF"), "PRKF (Neanka)", "", "Neanka/f4se_AE PRKF/main.cpp", 1 },
			{ MC("PBT"), "Pipboy Tabs", "", "Neanka/f4se_AE PipboyTabs/main.cpp", 1 },
			{ MC("SEEE"), "F4SE EE (Neanka)", "", "Neanka/f4se_AE f4seee/main.cpp", 1 },
		};
		return t;
	}

	const KnownPlugin* Known(std::uint32_t a_uid, bool a_fo4)
	{
		for (const auto& k : KnownTable()) {
			if (k.uid == a_uid && (k.game == 2 || k.game == (a_fo4 ? 1 : 0))) {
				return &k;
			}
		}
		return nullptr;
	}

	std::string ChunkLabel(std::uint32_t a_uid, std::uint32_t a_type, bool a_fo4)
	{
		if (a_uid == 0) {
			// skse64/InternalSerialization.cpp Core_SaveCallback, skse64/PapyrusEvents.h, f4se/InternalSerialization.cpp
			static const std::unordered_map<std::uint32_t, const char*> core = {
				{ MC("PLGN"), "plugin list" },
				{ MC("MODS"), "plugin list (old)" },
				{ MC("LMOD"), "light plugin list (old)" },
				{ MC("LIMD"), "light plugin list (old)" },
				{ MC("MENR"), "menu open/close event registrations" },
				{ MC("KEYR"), "key event registrations" },
				{ MC("KEYG"), "gamepad button event registrations" },
				{ MC("CTLR"), "control event registrations" },
				{ MC("MCBR"), "mod event registrations (RegisterForModEvent)" },
				{ MC("CHRR"), "crosshair reference event registrations" },
				{ MC("CAMR"), "camera event registrations" },
				{ MC("AACT"), "actor action event registrations" },
				{ MC("NINU"), "NiNode update event registrations" },
				{ MC("EXEV"), "external event registrations" },
				{ MC("FRNR"), "furniture event registrations" },
				{ MC("REGS"), "registrations for one event" },
				{ MC("REGE"), "end of a registration list" },
				{ MC("OBMG"), "persistent Papyrus objects" },
				{ MC("DFMG"), "delayed Papyrus calls" },
			};
			const auto it = core.find(a_type);
			return it == core.end() ? "" : it->second;
		}
		if (a_uid == papu::kUid && !a_fo4) {
			// eeveelo/PapyrusUtil Serialize.cpp
			static const std::unordered_map<std::uint32_t, const char*> pu = {
				{ MC("PLGN"), "plugin list" },
				{ MC("MODS"), "plugin list (old)" },
				{ MC("INTV"), "StorageUtil int values" },
				{ MC("FLOV"), "StorageUtil float values" },
				{ MC("STRV"), "StorageUtil string values" },
				{ MC("FORV"), "StorageUtil form values" },
				{ MC("INTL"), "StorageUtil int lists" },
				{ MC("FLOL"), "StorageUtil float lists" },
				{ MC("STRL"), "StorageUtil string lists" },
				{ MC("FORL"), "StorageUtil form lists" },
				{ MC("PKGO"), "package overrides (ActorUtil)" },
			};
			const auto it = pu.find(a_type);
			return it == pu.end() ? "" : it->second;
		}
		if (a_uid == jc::kUid && a_type == jc::kUid) {
			return "JContainers object tree";
		}
		return "";
	}

	// ---------------------------------------------------------------------------------------------- DLLs

	DllRoots RootsOf(const ScriptSources& a_sources)
	{
		DllRoots r;
		r.scanned = a_sources.scanned();
		r.root = a_sources.root;
		r.overwrite = a_sources.overwriteDir;
		r.gameData = a_sources.gameData;
		r.ranked = a_sources.ranked;
		r.vortex = a_sources.vortex;
		r.enabled = a_sources.enabledFolders;
		return r;
	}

	std::vector<Dll> ListPluginDlls(const ScriptSources* a_sources, const std::filesystem::path& a_data, bool a_fo4)
	{
		if (a_sources) {
			return ListPluginDlls(RootsOf(*a_sources), a_data, a_fo4);
		}
		return ListPluginDlls(DllRoots{}, a_data, a_fo4);
	}

	std::vector<Dll> ListPluginDlls(const DllRoots& a_roots, const std::filesystem::path& a_data, bool a_fo4)
	{
		const DllRoots* a_sources = a_roots.scanned ? &a_roots : nullptr;
		namespace fs = std::filesystem;
		std::vector<Dll> out;
		const std::string xse = a_fo4 ? "f4se" : "skse";
		auto              add = [&](const fs::path& a_dir, const std::string& a_mod, bool a_enabled) {
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(a_dir, ec)) {
                if (!e.is_directory(ec) || Lower(e.path().filename().string()) != xse) {
                    continue;
                }
                for (const auto& s : fs::directory_iterator(e.path(), ec)) {
                    if (!s.is_directory(ec) || Lower(s.path().filename().string()) != "plugins") {
                        continue;
                    }
                    for (const auto& d : fs::directory_iterator(s.path(), ec)) {
                        if (d.is_regular_file(ec) && Lower(d.path().extension().string()) == ".dll") {
                            out.push_back({ d.path(), a_mod, a_enabled });
                        }
                    }
                }
            }
		};
		std::error_code ec;
		if (a_sources) {
			if (!a_sources->vortex && !a_sources->root.empty() && fs::is_directory(a_sources->root, ec)) {
				bool isData = false;
				for (const auto& e : fs::directory_iterator(a_sources->root, ec)) {
					const auto n = Lower(e.path().filename().string());
					isData = isData || n == xse || n.ends_with(".esm");
				}
				if (isData) {
					add(a_sources->root, "", true);
				} else {
					for (const auto& m : fs::directory_iterator(a_sources->root, ec)) {
						if (!m.is_directory(ec)) {
							continue;
						}
						const auto name = m.path().filename().string();
						const bool enabled = !a_sources->ranked || a_sources->enabled.contains(name);
						add(m.path(), name, enabled);
					}
				}
			}
			// (Vortex deploys into the game's Data folder: that is where the game loads its DLLs from)
			if (!a_sources->overwrite.empty()) {
				add(a_sources->overwrite, "Overwrite", true);
			}
			if (!a_sources->gameData.empty()) {
				add(a_sources->gameData, "", true);
			}
		}
		if (!a_data.empty() && fs::is_directory(a_data, ec)) {
			// a Data folder (it holds plugin files) or a single mod: its own SKSE\Plugins; a mods folder: every mod in it
			bool dataFolder = Lower(a_data.filename().string()) == "data";
			for (const auto& e : fs::directory_iterator(a_data, ec)) {
				const auto n = Lower(e.path().filename().string());
				if (e.is_regular_file(ec) && (n.ends_with(".esm") || n.ends_with(".esp") || n.ends_with(".esl"))) {
					dataFolder = true;
				}
			}
			add(a_data, dataFolder ? "" : a_data.filename().string(), true);
			if (!dataFolder) {
				for (const auto& m : fs::directory_iterator(a_data, ec)) {
					if (m.is_directory(ec) && Lower(m.path().filename().string()) != xse) {
						add(m.path(), m.path().filename().string(), true);
					}
				}
			}
		}
		// the same file twice (Data given and scanned): keep the first
		std::set<std::string> seen;
		std::vector<Dll>      uniq;
		for (auto& d : out) {
			std::error_code e2;
			const auto      key = Lower(fs::weakly_canonical(d.path, e2).string());
			if (seen.insert(key).second) {
				uniq.push_back(std::move(d));
			}
		}
		return uniq;
	}

	std::map<std::uint32_t, std::vector<std::size_t>> FindIds(const std::vector<Dll>& a_dlls, const std::set<std::uint32_t>& a_uids, bool a_x86,
		std::size_t* a_read)
	{
		std::map<std::uint32_t, std::vector<std::size_t>> out;
		if (a_read) {
			*a_read = 0;
		}
		// ids too short to tell apart from ordinary constants are not searched for
		std::vector<std::uint32_t> uids;
		for (auto u : a_uids) {
			if (u >= 0x10000) {
				uids.push_back(u);
			}
		}
		if (uids.empty()) {
			return out;
		}
		for (std::size_t i = 0; i < a_dlls.size(); ++i) {
			std::error_code ec;
			const auto      sz = std::filesystem::file_size(a_dlls[i].path, ec);
			if (ec || sz < 64 || sz > 512ull * 1024 * 1024) {
				continue;
			}
			std::ifstream f(a_dlls[i].path, std::ios::binary);
			if (!f) {
				continue;
			}
			std::vector<std::uint8_t> d(static_cast<std::size_t>(sz));
			f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
			if (static_cast<std::size_t>(f.gcount()) != d.size()) {
				continue;
			}
			if (a_read) {
				++*a_read;
			}
			std::set<std::uint32_t> hit;
			for (std::size_t p = 1; p + 4 <= d.size(); ++p) {
				const std::uint8_t op = d[p - 1];
				// x64: mov edx, imm32 (BA; CommonLib SetUniqueID(uid)), mov ecx (B9), mov r8d (41 B8; SetUniqueID(handle, uid)),
				// mov r9d (41 B9). x86 (LE): push imm32 (68) or mov r32, imm32 (B8..BF).
				const bool ok = a_x86 ? (op == 0x68 || (op >= 0xB8 && op <= 0xBF)) : (op == 0xBA || op == 0xB9 || (op == 0xB8 && p >= 2 && d[p - 2] == 0x41));
				if (!ok) {
					continue;
				}
				std::uint32_t v;
				std::memcpy(&v, d.data() + p, 4);
				if (std::binary_search(uids.begin(), uids.end(), v)) {
					hit.insert(v);
				}
			}
			for (auto u : hit) {
				out[u].push_back(i);
			}
		}
		return out;
	}

	const char* StatusText(Status a_s)
	{
		switch (a_s) {
		case Status::kExtender: return "script extender";
		case Status::kInstalled: return "installed";
		case Status::kDisabled: return "disabled";
		case Status::kNotInstalled: return "not installed";
		default: return "not checked";
		}
	}

	View MakeView(const File& a_file, const std::vector<Dll>& a_dlls, bool a_x86)
	{
		std::set<std::uint32_t> uids;
		for (const auto& p : a_file.plugins) {
			uids.insert(p.uid);
		}
		std::map<std::uint32_t, std::vector<std::size_t>> found;
		std::size_t                                       searched = 0;
		if (!a_dlls.empty()) {
			found = FindIds(a_dlls, uids, a_x86, &searched);
		}
		return MakeView(a_file, a_dlls, found, searched);
	}

	View MakeView(const File& a_file, const std::vector<Dll>& a_dlls, const std::map<std::uint32_t, std::vector<std::size_t>>& found, std::size_t a_searched)
	{
		View v;
		v.dllsSearched = a_searched;
		v.searched = a_searched > 0;
		for (const auto& p : a_file.plugins) {
			PluginRow r;
			r.uid = p.uid;
			r.bytes = p.fileSize();
			r.chunks = p.chunkCount();
			const auto* k = Known(p.uid, a_file.fo4);
			if (k) {
				r.name = k->name;
				r.source = k->source;
			}
			bool enabled = false, any = false;
			if (const auto it = found.find(p.uid); it != found.end()) {
				for (auto i : it->second) {
					const auto& d = a_dlls[i];
					r.dlls.push_back((d.mod.empty() ? std::string("Data") : d.mod) + ": " + d.path.filename().string() + (d.enabled ? "" : " (disabled)"));
					enabled = enabled || d.enabled;
					any = true;
				}
				if (r.name.empty()) {
					r.name = a_dlls[it->second.front()].path.filename().string();
					r.source = "the DLL that sets this id";
				}
			} else if (k && k->dll[0] && v.searched) {
				// a known plugin whose DLL is there but its id was not found as an immediate (computed or loaded)
				for (std::size_t i = 0; i < a_dlls.size(); ++i) {
					if (Lower(a_dlls[i].path.filename().string()) == Lower(k->dll)) {
						r.dlls.push_back((a_dlls[i].mod.empty() ? std::string("Data") : a_dlls[i].mod) + ": " + a_dlls[i].path.filename().string() +
										 (a_dlls[i].enabled ? "" : " (disabled)") + " (by name)");
						enabled = enabled || a_dlls[i].enabled;
						any = true;
					}
				}
			}
			if (r.name.empty()) {
				r.name = IdText(p.uid);
			}
			if (p.uid == 0) {
				r.status = Status::kExtender;
			} else if (!v.searched) {
				r.status = Status::kUnknown;
			} else if (any) {
				r.status = enabled ? Status::kInstalled : Status::kDisabled;
			} else if (p.uid < 0x10000) {
				r.status = Status::kUnknown;  // too short an id to search for
			} else {
				r.status = Status::kNotInstalled;
			}
			if (r.status == Status::kInstalled) {
				++v.installedCount;
			}
			if (r.status == Status::kNotInstalled || r.status == Status::kDisabled) {
				v.notInstalledBytes += r.bytes;
				++v.notInstalledCount;
			}
			v.rows.push_back(std::move(r));
		}
		return v;
	}

	// ---------------------------------------------------------------------------------------------- PLGN

	bool ReadPlgn(const Chunk& a_chunk, std::vector<PlgnEntry>& a_out)
	{
		a_out.clear();
		try {
			Reader     r(a_chunk.data.data(), a_chunk.data.size());
			const auto n = r.u16();
			for (std::uint32_t i = 0; i < n; ++i) {
				PlgnEntry e;
				e.index = r.u8();
				if (e.index == 0xFE) {
					e.light = r.u16();
				}
				const auto len = r.u16();
				const auto b = r.bytes(len);
				e.name.assign(b.begin(), b.end());
				a_out.push_back(std::move(e));
			}
			return r.remaining() == 0;
		} catch (const std::exception&) {
			return false;
		}
	}

	// ---------------------------------------------------------------------------------------------- PapyrusUtil

	namespace papu
	{
		bool IsStorageRecord(std::uint32_t a_type)
		{
			switch (a_type) {
			case MC("INTV"):
			case MC("FLOV"):
			case MC("STRV"):
			case MC("FORV"):
			case MC("INTL"):
			case MC("FLOL"):
			case MC("STRL"):
			case MC("FORL"):
				return true;
			default:
				return false;
			}
		}

		namespace
		{
			// whitespace-separated tokens, as std::istream >> reads them
			struct Tok
			{
				const std::vector<std::uint8_t>& d;
				std::size_t                      p = 0;
				std::size_t                      start = 0;  // where the last token began
				static bool ws(std::uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
				bool        next(std::string& a_out)
				{
					while (p < d.size() && ws(d[p])) {
						++p;
					}
					if (p >= d.size()) {
						return false;
					}
					start = p;
					while (p < d.size() && !ws(d[p])) {
						++p;
					}
					a_out.assign(d.begin() + start, d.begin() + p);
					return true;
				}
			};
			bool ToInt(const std::string& s, long long& v)
			{
				if (s.empty()) {
					return false;
				}
				char* e = nullptr;
				v = std::strtoll(s.c_str(), &e, 10);
				return e && *e == 0;
			}
			bool ToU64(const std::string& s, std::uint64_t& v)
			{
				if (s.empty() || s[0] == '-') {
					return false;
				}
				char* e = nullptr;
				v = std::strtoull(s.c_str(), &e, 10);
				return e && *e == 0;
			}
		}

		Record ParseRecord(const Chunk& a_chunk)
		{
			Record rec;
			rec.type = a_chunk.type;
			rec.list = a_chunk.type == MC("INTL") || a_chunk.type == MC("FLOL") || a_chunk.type == MC("STRL") || a_chunk.type == MC("FORL");
			if (a_chunk.version != 1) {
				rec.error = Fmt("record version %u (PapyrusUtil loads version 1 only)", a_chunk.version);
				return rec;
			}
			Tok         t{ a_chunk.data };
			std::string s;
			if (!t.next(s) || !ToInt(s, rec.declared)) {
				rec.error = "no object count";
				return rec;
			}
			rec.countEnd = t.p;
			std::unordered_map<std::string, std::size_t> keyIdx;
			for (long long i = 0; i < rec.declared; ++i) {
				Object o;
				o.begin = t.p;  // the separator before the key belongs to the object
				if (!t.next(s)) {
					break;  // objects with no keys are counted but not written
				}
				long long n = 0;
				std::string keyText = s;
				if (!ToU64(keyText, o.key) || !t.next(s) || !ToInt(s, n) || n < 0) {
					rec.error = Fmt("object %lld: not a form key and key count", i);
					return rec;
				}
				o.keys = static_cast<std::size_t>(n);
				for (long long k = 0; k < n; ++k) {
					std::string key;
					const std::size_t kb = t.p;
					if (!t.next(key)) {
						rec.error = Fmt("object %lld: cut off in its keys", i);
						return rec;
					}
					std::size_t vals = 1;
					if (rec.list) {
						long long m = 0;
						if (!t.next(s) || !ToInt(s, m) || m < 0) {
							rec.error = Fmt("object %lld key %s: no list size", i, key.c_str());
							return rec;
						}
						vals = static_cast<std::size_t>(m);
					}
					for (std::size_t vI = 0; vI < vals; ++vI) {
						if (!t.next(s)) {
							rec.error = Fmt("object %lld key %s: cut off in its values", i, key.c_str());
							return rec;
						}
					}
					o.values += vals;
					std::replace(key.begin(), key.end(), '\x07', ' ');
					auto [it, isNew] = keyIdx.try_emplace(key, rec.keys.size());
					if (isNew) {
						rec.keys.push_back({ key });
					}
					auto& ks = rec.keys[it->second];
					++ks.objects;
					ks.values += vals;
					ks.bytes += t.p - kb;
				}
				o.end = t.p;
				rec.objects.push_back(o);
			}
			rec.ok = true;
			return rec;
		}

		std::string Owner(std::uint64_t a_key, const std::vector<PlgnEntry>& a_plgn)
		{
			const auto id = static_cast<std::uint32_t>(a_key);
			if (a_key == 0 || id == 0) {
				return "";
			}
			const std::uint32_t mod = id >> 24;
			if (mod == 0xFF) {
				return "";  // made during play
			}
			for (const auto& e : a_plgn) {
				if (mod == 0xFE ? (e.index == 0xFE && e.light == ((id >> 12) & 0xFFF)) : (e.index == mod)) {
					return e.name;
				}
			}
			return Fmt("[%02X]", mod);  // an index the plugin list does not have
		}

		Breakdown Analyze(const File& a_file)
		{
			Breakdown b;
			const auto* p = a_file.fo4 ? nullptr : a_file.find(kUid);
			if (!p || !p->regular) {
				return b;
			}
			b.present = true;
			for (const auto& c : p->chunks) {
				if (c.type == MC("PLGN")) {
					ReadPlgn(c, b.plgn);
				}
			}
			std::unordered_map<std::string, std::size_t> modIdx, keyIdx;
			for (const auto& c : p->chunks) {
				if (!IsStorageRecord(c.type)) {
					continue;
				}
				const auto     rec = ParseRecord(c);
				Breakdown::Rec r;
				r.type = c.type;
				r.bytes = c.data.size() + 12;
				r.ok = rec.ok;
				r.error = rec.error;
				r.objects = rec.objects.size();
				for (const auto& o : rec.objects) {
					r.keys += o.keys;
					r.values += o.values;
					auto owner = Owner(o.key, b.plgn);
					if (owner.empty()) {
						owner = (o.key == 0 || static_cast<std::uint32_t>(o.key) == 0) ? "(global)" : "(made in game)";
					}
					auto [it, isNew] = modIdx.try_emplace(owner, b.mods.size());
					if (isNew) {
						b.mods.push_back({ owner });
					}
					auto& m = b.mods[it->second];
					++m.objects;
					m.keys += o.keys;
					m.bytes += o.bytes();
					b.objects.push_back({ c.type, o.key, owner, o.keys, o.values, o.bytes() });
				}
				for (const auto& k : rec.keys) {
					auto [it, isNew] = keyIdx.try_emplace(k.name, b.keys.size());
					if (isNew) {
						b.keys.push_back({ k.name });
					}
					auto& ks = b.keys[it->second];
					ks.objects += k.objects;
					ks.values += k.values;
					ks.bytes += k.bytes;
				}
				b.records.push_back(r);
			}
			auto bigFirst = [](const auto& x, const auto& y) { return x.bytes > y.bytes; };
			std::stable_sort(b.mods.begin(), b.mods.end(), bigFirst);
			std::stable_sort(b.keys.begin(), b.keys.end(), bigFirst);
			std::stable_sort(b.objects.begin(), b.objects.end(), bigFirst);
			return b;
		}
	}

	namespace jc
	{
		namespace
		{
			const Chunk* StorageChunk(const File& a_file)
			{
				const auto* p = a_file.fo4 ? nullptr : a_file.find(kUid);
				if (!p) {
					return nullptr;
				}
				for (const auto& c : p->chunks) {
					if (c.type == kUid) {
						return &c;
					}
				}
				return nullptr;
			}

			// the header: its length as decimal text, then that many bytes of JSON (older files: the JSON alone)
			bool SplitHeader(const std::vector<std::uint8_t>& a_d, std::string& a_json, std::size_t& a_end)
			{
				std::size_t i = 0, len = 0;
				while (i < a_d.size() && i < 10 && a_d[i] >= '0' && a_d[i] <= '9') {
					len = len * 10 + (a_d[i++] - '0');
				}
				if (i > 0 && i + len <= a_d.size() && len > 0 && a_d[i] == '{') {
					a_json.assign(a_d.begin() + i, a_d.begin() + i + len);
					a_end = i + len;
					return true;
				}
				if (!a_d.empty() && a_d[0] == '{') {
					const auto e = std::find(a_d.begin(), a_d.end(), '}');
					if (e != a_d.end()) {
						a_json.assign(a_d.begin(), e + 1);
						a_end = static_cast<std::size_t>(e - a_d.begin()) + 1;
						return true;
					}
				}
				return false;
			}

			struct Fail : std::runtime_error
			{
				using std::runtime_error::runtime_error;
			};

			// boost binary_iarchive framing, as boost 1.67 writes it (see Cosave.h)
			class Archive
			{
			public:
				Archive(const std::vector<std::uint8_t>& a_d, std::size_t a_at) :
					d(a_d), at(a_at) {}

				const std::vector<std::uint8_t>& d;
				std::size_t                      at;
				unsigned                         longSize = 4;

				template <class T>
				T get()
				{
					if (at + sizeof(T) > d.size()) {
						throw Fail(Fmt("the archive ends early (byte %zu)", at));
					}
					T v;
					std::memcpy(&v, d.data() + at, sizeof(T));
					at += sizeof(T);
					return v;
				}
				std::string str()
				{
					const auto n = get<std::uint64_t>();
					if (n > d.size() - at) {
						throw Fail(Fmt("a text of %llu bytes at byte %zu does not fit", static_cast<unsigned long long>(n), at));
					}
					std::string s(reinterpret_cast<const char*>(d.data() + at), static_cast<std::size_t>(n));
					at += static_cast<std::size_t>(n);
					return s;
				}
				std::int64_t getLong() { return longSize == 8 ? get<std::int64_t>() : get<std::int32_t>(); }

				struct Class
				{
					int      id = 0;
					bool     tracked = false;
					unsigned version = 0;
				};
				// a class written as an object: its tracking + version the first time; then its object id when tracked.
				// Returns false when it is a reference to an object read before (nothing else follows).
				bool object(const std::string& a_class, unsigned a_maxVersion, unsigned* a_version = nullptr)
				{
					auto it = classes.find(a_class);
					if (it == classes.end()) {
						Class c;
						c.id = count++;
						c.tracked = get<std::uint8_t>() != 0;
						c.version = get<std::uint32_t>();
						it = classes.emplace(a_class, c).first;
						byId[c.id] = a_class;
					}
					if (it->second.version > a_maxVersion) {
						throw Fail(Fmt("%s is version %u (only up to %u is known), byte %zu", a_class.c_str(), it->second.version, a_maxVersion, at));
					}
					if (a_version) {
						*a_version = it->second.version;
					}
					if (it->second.tracked) {
						const auto oid = get<std::uint32_t>();
						if (oid < objects) {
							return false;
						}
						++objects;
					}
					return true;
				}
				// a pointer: "" null; else the class read; a_new false when it points at an object read before
				std::string pointer(const std::string& a_static, bool a_polymorphic, bool& a_new, unsigned& a_version)
				{
					a_new = false;
					const auto cid = get<std::int16_t>();
					if (cid == -1) {
						return {};
					}
					std::string name;
					if (auto b = byId.find(cid); b != byId.end()) {
						name = b->second;
					} else {
						if (cid != count) {
							throw Fail(Fmt("class id %d where %d was expected, byte %zu", cid, count, at));
						}
						name = a_polymorphic ? str() : a_static;
						if (classes.contains(name)) {
							throw Fail(Fmt("class %s seen twice, byte %zu", name.c_str(), at));
						}
						Class c;
						c.id = count++;
						c.tracked = get<std::uint8_t>() != 0;
						c.version = get<std::uint32_t>();
						classes.emplace(name, c);
						byId[c.id] = name;
					}
					const auto& c = classes[name];
					a_version = c.version;
					if (c.tracked) {
						const auto oid = get<std::uint32_t>();
						if (oid < objects) {
							return name;
						}
						++objects;
					}
					a_new = true;
					return name;
				}

			private:
				std::map<std::string, Class> classes;
				std::map<int, std::string>   byId;
				int                          count = 0;
				std::uint32_t                objects = 0;
			};

			class Reader
			{
			public:
				Reader(Archive& a_ar, Breakdown& a_b) :
					ar(a_ar), b(a_b) {}

				void master()
				{
					if (!ar.object("master", 0)) {
						throw Fail("master read as a reference");
					}
					context(b.rootId);
					const auto n = ar.get<std::uint32_t>();
					if (n > 10000) {
						throw Fail(Fmt("%u domains, byte %zu", n, ar.at));
					}
					for (std::uint32_t i = 0; i < n; ++i) {
						domain = ar.str();
						b.domains.push_back(domain);
						std::uint32_t root = 0;
						context(root);
					}
					domain.clear();
					// form_observer v3: u32 count, shared_ptr<form_entry> each
					if (!ar.object("form_observer", 3)) {
						throw Fail("form_observer read as a reference");
					}
					const auto forms = ar.get<std::uint32_t>();
					for (std::uint32_t i = 0; i < forms; ++i) {
						formEntry();
					}
					b.watchedForms = forms;
				}

			private:
				Archive&    ar;
				Breakdown&  b;
				std::string domain;
				int         depth = 0;

				std::uint64_t size()
				{
					const auto n = ar.get<std::uint64_t>();
					if (n > ar.d.size()) {
						throw Fail(Fmt("a list of %llu entries at byte %zu", static_cast<unsigned long long>(n), ar.at));
					}
					return n;
				}

				void context(std::uint32_t& a_root)
				{
					ar.object("tes_context", 2);
					ar.object("object_context", 0);
					ar.object("object_registry", 1);
					// unordered_set<object_base*>: count, bucket count, item version, the pointers
					ar.object("unordered_set<object_base*>", 0);
					const auto n = size();
					ar.get<std::uint64_t>();
					ar.get<std::uint32_t>();
					for (std::uint64_t i = 0; i < n; ++i) {
						objectPointer();
					}
					ar.object("id_generator", 1);
					ar.object("deque<range>", 0);
					const auto ranges = size();
					ar.get<std::uint32_t>();
					for (std::uint64_t i = 0; i < ranges; ++i) {
						ar.object("range", 0);
						ar.get<std::uint32_t>();
						ar.get<std::uint32_t>();
					}
					ar.get<std::uint64_t>();  // the current range
					unsigned v = 0;
					ar.object("autorelease_queue", 2, &v);
					if (v != 2) {
						throw Fail(Fmt("autorelease_queue version %u (only 2 is read), byte %zu", v, ar.at));
					}
					ar.get<std::uint32_t>();  // tick counter
					ar.object("deque<queue_object_ref>", 0);
					const auto q = size();
					ar.get<std::uint32_t>();
					for (std::uint64_t i = 0; i < q; ++i) {
						ar.object("queue_object_ref", 0);
						objectPointer();
					}
					b.queue += static_cast<std::size_t>(q);
					a_root = ar.get<std::uint32_t>();
				}

				// the form id when this entry is written here (0: none, or one read before)
				std::uint32_t formEntry()
				{
					ar.object("shared_ptr<form_entry>", 1);
					bool     fresh = false;
					unsigned v = 0;
					if (!ar.pointer("form_entry", false, fresh, v).empty() && fresh) {
						const auto id = ar.get<std::uint32_t>();
						ar.getLong();  // deleted
						return id;
					}
					return 0;
				}

				void objectPointer()
				{
					bool       fresh = false;
					unsigned   v = 0;
					const auto start = ar.at;
					const auto type = ar.pointer("object_base", true, fresh, v);
					if (type.empty() || !fresh) {
						return;
					}
					if (++depth > 2000) {
						throw Fail("objects nested too deep");
					}
					Object o;
					o.type = type;
					o.domain = domain;
					const bool array = type == "kJArray", map = type == "kJMap", fmap = type == "kJFormMap", imap = type == "kJIntegerMap";
					if (!array && !map && !fmap && !imap) {
						throw Fail(Fmt("unknown object class %s, byte %zu", type.c_str(), ar.at));
					}
					if (fmap && v != 1) {
						throw Fail(Fmt("kJFormMap version %u (only 1 is read), byte %zu", v, ar.at));
					}
					unsigned bv = 0;
					if (ar.object("object_base", 2, &bv)) {
						if (bv != 2) {
							throw Fail(Fmt("object_base version %u (only 2 is read), byte %zu", bv, ar.at));
						}
						ar.get<std::uint32_t>();  // push time
						o.tesRefs = ar.get<std::int32_t>();
						o.id = ar.get<std::uint32_t>();
						o.tag = ar.str();
					}
					std::size_t inner = 0;  // bytes of objects first written inside this one
					const std::string coll = array ? "vector<item>" : map ? "map<string,item>" : fmap ? "map<form_ref,item>" : "map<int,item>";
					ar.object(coll, 0);
					const auto n = size();
					ar.get<std::uint32_t>();
					o.entries = static_cast<std::size_t>(n);
					for (std::uint64_t i = 0; i < n; ++i) {
						if (!array) {
							ar.object("pair<" + coll.substr(4), 0);
							if (map) {
								auto k = ar.str();
								if (o.keys.size() < 12) {
									o.keys.push_back(std::move(k));
								}
							} else if (imap) {
								const auto k = ar.get<std::int32_t>();
								if (o.keys.size() < 12) {
									o.keys.push_back(std::to_string(k));
								}
							} else {
								ar.object("form_ref", 2);
								const auto id = formEntry();
								if (o.keys.size() < 12) {
									o.keys.push_back(id ? Fmt("%08X", id) : std::string("(form)"));
								}
							}
						}
						inner += item();
					}
					o.bytes = ar.at - start - std::min(inner, ar.at - start);
					(array ? b.arrays : map ? b.maps : fmap ? b.formMaps : b.intMaps)++;
					b.objects.push_back(std::move(o));
					--depth;
					last = ar.at - start;
				}

				std::size_t last = 0;

				// one item; returns the bytes of an object first written inside it
				std::size_t item()
				{
					ar.object("item", 3);
					unsigned v = 0;
					ar.object("variant", 0, &v);
					const auto which = ar.get<std::int32_t>();
					++b.values;
					switch (which) {
					case 0:
						ar.get<std::uint8_t>();  // boost::blank, a primitive of one byte
						++b.empty;
						return 0;
					case 1:
						ar.get<std::int32_t>();
						++b.numbers;
						return 0;
					case 2:
						ar.get<float>();
						++b.numbers;
						return 0;
					case 3:
						ar.object("form_ref", 2);
						formEntry();
						++b.forms;
						return 0;
					case 4: {
						ar.object("internal_object_ref", 0);
						++b.objectRefs;
						const auto before = b.objects.size();
						last = 0;
						objectPointer();
						return b.objects.size() > before ? last : 0;
					}
					case 5:
						ar.str();
						++b.strings;
						return 0;
					default:
						throw Fail(Fmt("value type %d, byte %zu", which, ar.at));
					}
				}
			};
		}

		std::string Header(const File& a_file)
		{
			const auto* c = StorageChunk(a_file);
			std::string json;
			std::size_t end = 0;
			return c && SplitHeader(c->data, json, end) ? json : std::string();
		}

		Breakdown AnalyzeBytes(const std::vector<std::uint8_t>& a_d)
		{
			Breakdown b;
			b.present = true;
			b.bytes = a_d.size();
			std::size_t at = 0;
			if (!SplitHeader(a_d, b.header, at)) {
				b.error = "no JSON header";
				return b;
			}
			if (b.header.find("\"commonVersion\": 6") == std::string::npos && b.header.find("\"commonVersion\":6") == std::string::npos) {
				b.error = "the data is in another JContainers format (" + b.header + "); only commonVersion 6 is read";
				return b;
			}
			try {
				Archive ar(a_d, at);
				if (ar.str() != "serialization::archive") {
					throw Fail("not a boost archive");
				}
				b.library = ar.get<std::uint16_t>();
				const auto szInt = ar.get<std::uint8_t>(), szLong = ar.get<std::uint8_t>(), szFloat = ar.get<std::uint8_t>(), szDouble = ar.get<std::uint8_t>();
				if (ar.get<std::int32_t>() != 1 || szInt != 4 || szFloat != 4 || szDouble != 8 || (szLong != 4 && szLong != 8)) {
					throw Fail("an archive of another machine type");
				}
				if (b.library < 8) {
					throw Fail(Fmt("boost archive library version %u (8 or newer is read)", b.library));
				}
				ar.longSize = szLong;
				Reader r(ar, b);
				r.master();
				if (ar.at != a_d.size()) {
					throw Fail(Fmt("%zu bytes left after the object tree", a_d.size() - ar.at));
				}
				b.ok = true;
			} catch (const std::exception& e) {
				b.error = e.what();
				b.objects.clear();
				return b;
			}
			std::map<std::string, std::size_t> tags;
			for (const auto& o : b.objects) {
				if (!o.tag.empty()) {
					++tags[o.tag];
				}
			}
			b.tags.assign(tags.begin(), tags.end());
			std::stable_sort(b.tags.begin(), b.tags.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
			std::stable_sort(b.objects.begin(), b.objects.end(), [](const Object& x, const Object& y) {
				return x.entries != y.entries ? x.entries > y.entries : x.bytes > y.bytes;
			});
			return b;
		}

		Breakdown Analyze(const File& a_file)
		{
			const auto* c = StorageChunk(a_file);
			if (!c) {
				return {};
			}
			return AnalyzeBytes(c->data);
		}
	}

	// ---------------------------------------------------------------------------------------------- cleaning

	const char* kRemoveExplanation =
		"Removing a plugin's data from the co-save deletes what that SKSE / F4SE plugin saved for this game: its settings for "
		"this character, the values Papyrus scripts stored through it, its registrations. The script extender skips the data "
		"of plugins that are not installed anyway (\"plugin with signature ... not loaded\"), so for a plugin that is gone this "
		"only makes the file smaller. If you install that plugin again later, it starts fresh on this save, as on a new game. "
		"The script extender's own block (id 0) is never removed.";

	CleanResult Clean(File& a_file, const CleanOptions& a_opt)
	{
		CleanResult res;
		res.before = Write(a_file).size();
		// 1. whole plugin blocks
		std::vector<Plugin> kept;
		for (auto& p : a_file.plugins) {
			if (p.uid != 0 && a_opt.removeUids.contains(p.uid)) {
				++res.plugins;
				res.pluginBytes += p.fileSize();
				res.lines.push_back(Fmt("removed %s: %zu chunk(s), %zu bytes", IdText(p.uid).c_str(), p.chunkCount(), p.fileSize()));
				continue;
			}
			kept.push_back(std::move(p));
		}
		if (res.plugins) {
			// keep the header count as the script extender writes it (= the blocks); a count that was already off stays off by the same
			const auto had = a_file.plugins.size();
			a_file.numPlugins = a_file.numPlugins == had ? static_cast<std::uint32_t>(kept.size())
			                                             : static_cast<std::uint32_t>(a_file.numPlugins >= res.plugins ? a_file.numPlugins - res.plugins : 0);
		}
		a_file.plugins = std::move(kept);

		// 2. PapyrusUtil objects on forms of plugins that are gone (PapyrusUtil skips them on load: the same result, smaller)
		if (!a_opt.gonePlugins.empty() && !a_file.fo4) {
			for (auto& p : a_file.plugins) {
				if (p.uid != papu::kUid || !p.regular) {
					continue;
				}
				std::vector<PlgnEntry> plgn;
				bool                   havePlgn = false;
				for (const auto& c : p.chunks) {
					if (c.type == MC("PLGN")) {
						havePlgn = ReadPlgn(c, plgn);
					}
				}
				if (!havePlgn) {
					res.lines.push_back("PapyrusUtil: no readable plugin list (PLGN): its objects were left alone");
					break;
				}
				std::vector<Chunk> chunks;
				for (auto& c : p.chunks) {
					if (!papu::IsStorageRecord(c.type)) {
						chunks.push_back(std::move(c));
						continue;
					}
					const auto rec = papu::ParseRecord(c);
					if (!rec.ok) {
						res.lines.push_back("PapyrusUtil " + FourCC(c.type) + ": " + rec.error + " (left alone)");
						chunks.push_back(std::move(c));
						continue;
					}
					std::vector<const papu::Object*> keep;
					std::size_t                      dropped = 0, droppedBytes = 0;
					for (const auto& o : rec.objects) {
						const auto owner = Lower(papu::Owner(o.key, plgn));
						if (!owner.empty() && a_opt.gonePlugins.contains(owner)) {
							++dropped;
							droppedBytes += o.bytes();
						} else {
							keep.push_back(&o);
						}
					}
					if (!dropped) {
						chunks.push_back(std::move(c));
						continue;
					}
					res.papuObjects += dropped;
					if (keep.empty()) {
						res.papuBytes += c.data.size() + 12;
						res.lines.push_back(Fmt("PapyrusUtil %s: all %zu object(s) were on removed plugins' forms: record removed", FourCC(c.type).c_str(), dropped));
						continue;  // PapyrusUtil writes no record for empty storage either
					}
					Chunk nc;
					nc.type = c.type;
					nc.version = c.version;
					const auto count = std::to_string(rec.declared - static_cast<long long>(dropped));
					nc.data.assign(count.begin(), count.end());
					std::size_t lastEnd = rec.countEnd;
					for (const auto* o : keep) {
						nc.data.insert(nc.data.end(), c.data.begin() + o->begin, c.data.begin() + o->end);
					}
					if (!rec.objects.empty()) {
						lastEnd = rec.objects.back().end;
					}
					nc.data.insert(nc.data.end(), c.data.begin() + lastEnd, c.data.end());  // whatever followed (normally nothing)
					res.papuBytes += c.data.size() - nc.data.size();
					res.lines.push_back(Fmt("PapyrusUtil %s: %zu object(s) on removed plugins' forms removed (%zu bytes)", FourCC(c.type).c_str(), dropped, droppedBytes));
					chunks.push_back(std::move(nc));
				}
				p.chunks = std::move(chunks);
			}
		}
		res.after = Write(a_file).size();
		return res;
	}

	std::vector<std::uint8_t> CleanBytes(const std::vector<std::uint8_t>& a_bytes, const CleanOptions& a_opt, CleanResult* a_result)
	{
		auto       f = Parse(a_bytes);
		const auto r = Clean(f, a_opt);
		auto       out = Write(f);
		// read back: the same plugins, the same chunks
		const auto back = Parse(out);
		if (back.plugins.size() != f.plugins.size()) {
			throw std::runtime_error("the cleaned co-save does not read back the same (plugin count)");
		}
		for (std::size_t i = 0; i < f.plugins.size(); ++i) {
			const auto& a = f.plugins[i];
			const auto& b = back.plugins[i];
			if (a.uid != b.uid || a.chunkCount() != b.chunkCount() || (a.regular && b.regular && a.chunks.size() != b.chunks.size())) {
				throw std::runtime_error("the cleaned co-save does not read back the same (plugin " + IdText(a.uid) + ")");
			}
			for (std::size_t c = 0; a.regular && c < a.chunks.size(); ++c) {
				if (a.chunks[c].type != b.chunks[c].type || a.chunks[c].data != b.chunks[c].data) {
					throw std::runtime_error("the cleaned co-save does not read back the same (plugin " + IdText(a.uid) + " chunk " + std::to_string(c) + ")");
				}
			}
			if (a.uid == papu::kUid) {
				for (const auto& c : b.chunks) {
					if (papu::IsStorageRecord(c.type)) {
						if (const auto pr = papu::ParseRecord(c); !pr.ok) {
							throw std::runtime_error("the cleaned PapyrusUtil record " + FourCC(c.type) + " does not read back (" + pr.error + ")");
						}
					}
				}
			}
		}
		if (a_result) {
			*a_result = r;
		}
		return out;
	}

	// ---------------------------------------------------------------------------------------------- report

	std::string SizeText(std::size_t a_bytes)
	{
		if (a_bytes >= 1024 * 1024) {
			return Fmt("%.1f MB", a_bytes / 1048576.0);
		}
		if (a_bytes >= 10 * 1024) {
			return Fmt("%zu KB", (a_bytes + 512) / 1024);
		}
		if (a_bytes >= 1024) {
			return Fmt("%.1f KB", a_bytes / 1024.0);
		}
		return Fmt("%zu bytes", a_bytes);
	}

	std::string Report(const File& a_file, const View& a_view, std::size_t a_top, bool a_chunks)
	{
		std::string s;
		s += Fmt("%s co-save: %s, format %u, %s %s, game %s, %zu plugin(s)\n", a_file.xseName().c_str(), SizeText(a_file.size).c_str(), a_file.formatVersion,
			a_file.xseName().c_str(), PackedVersion(a_file.xseVersion).c_str(), PackedVersion(a_file.runtimeVersion).c_str(), a_file.plugins.size());
		for (const auto& n : a_file.notes) {
			s += "  note: " + n + "\n";
		}
		if (a_view.searched) {
			s += Fmt("%zu DLL(s) searched for the plugins' ids\n", a_view.dllsSearched);
		} else {
			s += "installed DLLs not searched (give a mods folder or Data folder)\n";
		}
		std::vector<std::size_t> order(a_view.rows.size());
		for (std::size_t i = 0; i < order.size(); ++i) {
			order[i] = i;
		}
		std::stable_sort(order.begin(), order.end(), [&](auto x, auto y) { return a_view.rows[x].bytes > a_view.rows[y].bytes; });
		s += Fmt("  %-28s %-40s %10s %7s  %s\n", "id", "plugin", "size", "chunks", "status");
		for (auto i : order) {
			const auto& r = a_view.rows[i];
			s += Fmt("  %-28s %-40s %10s %7zu  %s\n", IdText(r.uid).c_str(), r.name.substr(0, 40).c_str(), SizeText(r.bytes).c_str(), r.chunks,
				StatusText(r.status));
			for (const auto& d : r.dlls) {
				s += "      " + d + "\n";
			}
			if (a_chunks) {
				const auto* p = a_file.find(r.uid);
				if (p && p->regular) {
					std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> byType;
					for (const auto& c : p->chunks) {
						auto& t = byType[c.type];
						++t.first;
						t.second += 12 + c.data.size();
					}
					for (const auto& [t, v] : byType) {
						const auto l = ChunkLabel(r.uid, t, a_file.fo4);
						s += Fmt("      %-6s x%-5zu %10s  %s\n", FourCC(t).c_str(), v.first, SizeText(v.second).c_str(), l.c_str());
					}
				}
			}
		}
		if (a_view.searched && !a_view.plausible()) {
			s += "none of these plugins was found in the DLLs searched: probably not the folder the game loads from\n";
		} else if (a_view.searched && a_view.notInstalledCount) {
			s += Fmt("%s from %zu plugin(s) no longer installed (or disabled)\n", SizeText(a_view.notInstalledBytes).c_str(), a_view.notInstalledCount);
		}
		const auto b = papu::Analyze(a_file);
		if (b.present) {
			s += Fmt("PapyrusUtil (StorageUtil): %zu plugins in its list\n", b.plgn.size());
			for (const auto& r : b.records) {
				s += Fmt("  %s %-28s %10s  %zu object(s), %zu key(s), %zu value(s)%s\n", FourCC(r.type).c_str(), ChunkLabel(papu::kUid, r.type, false).c_str(),
					SizeText(r.bytes).c_str(), r.objects, r.keys, r.values, r.ok ? "" : ("  (" + r.error + ")").c_str());
			}
			s += "  by the plugin of the object the data is on:\n";
			for (std::size_t i = 0; i < b.mods.size() && i < a_top; ++i) {
				const auto& m = b.mods[i];
				s += Fmt("    %-40s %10s  %zu object(s), %zu key(s)\n", m.plugin.c_str(), SizeText(m.bytes).c_str(),
					m.objects, m.keys);
			}
			s += "  biggest keys:\n";
			for (std::size_t i = 0; i < b.keys.size() && i < a_top; ++i) {
				const auto& k = b.keys[i];
				s += Fmt("    %-40s %10s  on %zu object(s), %zu value(s)\n", k.name.substr(0, 40).c_str(), SizeText(k.bytes).c_str(), k.objects, k.values);
			}
			s += "  biggest objects:\n";
			for (std::size_t i = 0; i < b.objects.size() && i < a_top; ++i) {
				const auto& o = b.objects[i];
				s += Fmt("    %s %08X (form type %u) %-28s %10s  %zu key(s), %zu value(s)\n", FourCC(o.record).c_str(), static_cast<std::uint32_t>(o.key),
					static_cast<unsigned>(o.key >> 32), o.plugin.substr(0, 28).c_str(), SizeText(o.bytes).c_str(), o.keys, o.values);
			}
		}
		if (const auto* j = a_file.fo4 ? nullptr : a_file.find(jc::kUid)) {
			const auto b = jc::Analyze(a_file);
			s += Fmt("JContainers: %s%s\n", SizeText(j->fileSize()).c_str(), b.header.empty() ? "" : (", " + b.header).c_str());
			if (!b.ok) {
				s += "  not broken down: " + (b.error.empty() ? std::string("no JSTR chunk") : b.error) + "\n";
			} else {
				s += Fmt("  %zu object(s): %zu map(s), %zu array(s), %zu form map(s), %zu integer map(s); %zu value(s): %zu text, %zu number(s), "
						 "%zu form(s), %zu object reference(s), %zu empty; %zu form(s) watched, %zu object(s) waiting to be freed, JDB root %u%s\n",
					b.objects.size(), b.maps, b.arrays, b.formMaps, b.intMaps, b.values, b.strings, b.numbers, b.forms, b.objectRefs, b.empty,
					b.watchedForms, b.queue, b.rootId, b.domains.empty() ? "" : Fmt(", %zu other domain(s)", b.domains.size()).c_str());
				for (std::size_t i = 0; i < b.tags.size() && i < a_top; ++i) {
					s += Fmt("  tag %-30s %zu object(s)\n", b.tags[i].first.substr(0, 30).c_str(), b.tags[i].second);
				}
				s += "  biggest objects:\n";
				for (std::size_t i = 0; i < b.objects.size() && i < a_top; ++i) {
					const auto& o = b.objects[i];
					std::string keys;
					for (const auto& k : o.keys) {
						keys += (keys.empty() ? "" : ", ") + k;
					}
					s += Fmt("    %-12s id %-6u %-20s %6zu entries %10s  %s\n", o.type.c_str(), o.id, o.tag.substr(0, 20).c_str(), o.entries,
						SizeText(o.bytes).c_str(), keys.substr(0, 80).c_str());
				}
			}
		}
		return s;
	}
}
