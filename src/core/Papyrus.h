// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Papyrus (global data type 1001) section of a Skyrim SE/AE, Fallout 4 or Starfield save (Starfield: PapyrusSF.cpp).
#pragma once

#include "ByteIO.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ess
{
	using EID = std::uint64_t;   // object/array ids are 64-bit in SE/AE saves
	using ThreadID = std::uint32_t;  // active script (thread) ids stay 32-bit
	using TStr = std::uint32_t;  // index into the Papyrus string table (32-bit in SE)

	// 3-byte packed form reference used throughout the save.
	struct RefID
	{
		enum class Type : std::uint8_t
		{
			kFormIdx = 0,
			kDefault = 1,
			kCreated = 2,
			kInvalid = 3
		};
		std::uint32_t data = 0;

		static RefID read(Reader& r)
		{
			RefID id;
			const std::uint32_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
			id.data = (b0 << 16) | (b1 << 8) | b2;
			return id;
		}
		void write(Writer& w) const
		{
			w.u8(static_cast<std::uint8_t>(data >> 16));
			w.u8(static_cast<std::uint8_t>(data >> 8));
			w.u8(static_cast<std::uint8_t>(data));
		}
		Type          type() const { return static_cast<Type>((data >> 22) & 3); }
		std::uint32_t value() const { return data & 0x3FFFFF; }
		bool          isZero() const { return data == 0; }
		bool          operator==(const RefID& o) const { return data == o.data; }
	};

	enum class VType : std::uint8_t
	{
		kNull = 0,
		kRef = 1,
		kString = 2,
		kInt = 3,
		kFloat = 4,
		kBool = 5,
		kVariant = 6,
		kStruct = 7,
		kRefArray = 11,
		kStringArray = 12,
		kIntArray = 13,
		kFloatArray = 14,
		kBoolArray = 15,
		kVariantArray = 16,
		kStructArray = 17,
	};

	inline bool IsRefType(VType t) { return t == VType::kRef || t == VType::kRefArray || t == VType::kStruct || t == VType::kStructArray; }
	inline bool IsArrayType(VType t) { return static_cast<std::uint8_t>(t) >= 11; }
	const char* VTypeName(VType t);
	bool        EqualsNoCase(const std::string& a, const std::string& b);

	// A Papyrus value: script variable, array element, stack-frame variable, ...
	struct Variable
	{
		VType                     type = VType::kNull;
		std::uint64_t             value = 0;    // null payload / int / bool / float bits / string index / EID
		TStr                      refType = 0;  // script name for ref and ref-array types
		std::unique_ptr<Variable> inner;        // variant payload

		Variable() = default;
		Variable(const Variable& o) { *this = o; }
		Variable& operator=(const Variable& o)
		{
			type = o.type;
			value = o.value;
			refType = o.refType;
			inner = o.inner ? std::make_unique<Variable>(*o.inner) : nullptr;
			return *this;
		}
		Variable(Variable&&) = default;
		Variable& operator=(Variable&&) = default;

		static Variable read(Reader& r);
		void            write(Writer& w) const;
		bool            isRef() const { return type == VType::kRef; }
		float           asFloat() const
		{
			float f;
			const auto lo = static_cast<std::uint32_t>(value);
			std::memcpy(&f, &lo, 4);
			return f;
		}
	};

	struct MemberDesc
	{
		TStr name = 0;
		TStr type = 0;
	};

	struct Script
	{
		TStr                    name = 0;
		TStr                    parent = 0;
		std::vector<MemberDesc> members;
		std::vector<TStr>       guards;  // Starfield: the script's guard names (none in Skyrim and Fallout 4)
	};

	// Starfield: one guard's saved state in an object's data (Starfield.exe 1.16.244 0x1428364f0 / 0x142837c10):
	// u32 owner, u32 n + n x u32, u32 m + m x u32. The lists' meaning is not known; kept as read.
	struct ObjectGuard
	{
		std::uint32_t              owner = 0;
		std::vector<std::uint32_t> first;
		std::vector<std::uint32_t> second;
	};

	struct InstanceData  // shared by script instances and references
	{
		std::uint8_t          flag = 0;
		TStr                  state = 0;
		std::uint32_t         unknown1 = 0;
		std::uint32_t         unknown2 = 0;  // present when flag & 4
		std::vector<Variable> vars;
		// Starfield (Object::Save 0x142803340, compact): the flag byte decides what is saved: state only without 0x20
		// (0 when absent), unknown1 (the lock count) only with 0x40, unknown2 with 0x04, guards with 0x80
		std::vector<ObjectGuard> guards;
	};

	struct ScriptInstance
	{
		EID                         id = 0;
		TStr                        script = 0;
		std::uint16_t               unknown2bits = 0;
		std::uint16_t               unknown = 0;
		RefID                       refID;
		std::uint8_t                unknownByte = 0;
		std::optional<std::uint8_t> unknownFO4Byte;  // Fallout 4: present when (unknown2bits & 3) == 3
		std::optional<InstanceData> data;
	};

	// Fallout 4: struct definitions and the struct objects scripts hold
	struct StructDef
	{
		TStr                    name = 0;
		std::vector<MemberDesc> members;
	};
	struct StructInstance
	{
		EID                   id = 0;
		TStr                  def = 0;  // the struct's name
		bool                  hasData = false;
		std::uint8_t          flag = 0;
		std::vector<Variable> vars;
	};

	struct Reference
	{
		EID                         id = 0;
		TStr                        script = 0;
		std::optional<InstanceData> data;
	};

	struct ArrayInfo
	{
		EID                   id = 0;
		VType                 type = VType::kNull;
		TStr                  refType = 0;
		std::uint32_t         length = 0;
		bool                  hasData = false;
		std::vector<Variable> vars;
	};

	struct Param
	{
		std::uint8_t  type = 0;  // 0 null,1 identifier,2 string,3 int,4 float,5 bool,8 unknown8
		std::uint32_t value = 0;
	};

	struct Opcode
	{
		std::uint8_t       op = 0;
		std::vector<Param> params;
	};

	struct StackFrame
	{
		std::uint8_t              flag = 0;
		std::uint8_t              fnType = 0;
		TStr                      scriptName = 0;
		TStr                      baseName = 0;
		TStr                      event = 0;
		std::optional<TStr>       status;
		std::uint8_t              opMajor = 0, opMinor = 0;
		TStr                      returnType = 0;
		TStr                      docString = 0;
		std::uint32_t             userFlags = 0;
		std::uint8_t              fnFlags = 0;
		std::vector<MemberDesc>   params;
		std::vector<MemberDesc>   locals;
		std::vector<Opcode>       code;
		std::uint32_t             ptr = 0;
		Variable                  owner;
		std::vector<Variable>     vars;
	};

	struct FragmentTask
	{
		std::uint8_t              kind = 0;   // the "unknown3" selector: 1, 2 or 3
		std::string               typeCode;   // QuestStage, SceneResults, ... (kinds 1 and 3)
		std::vector<std::uint8_t> payload;    // fragment-type specific data, kept verbatim
		std::optional<Variable>   variable;   // kinds 2 and 3
		// Starfield (GameVM 0x141fb3570): kind 1 = an event callback (typeCode + payload), kind 2 = a latent callback
		// (payload = u32 + u8, 0x141fb0b40); both end with this u32 (the callback's +0x18)
		std::uint32_t             sfTail = 0;
		bool                      terminal() const { return typeCode == "TerminalRunResults"; }
	};

	struct ActiveScriptData
	{
		std::int8_t                 major = 0, minor = 0;
		Variable                    unknownVar;
		std::uint8_t                flag = 0;
		std::uint8_t                unknownByte = 0;
		std::uint32_t               unknown2 = 0;  // present when flag & 1
		std::uint8_t                unknown3 = 0;
		std::optional<FragmentTask> fragment;
		std::optional<EID>          attached;  // Fallout 4: the object the thread is attached to
		std::vector<StackFrame>     frames;
		std::optional<std::uint8_t> unknown5;
	};

	struct ActiveScript
	{
		ThreadID                        id = 0;
		std::uint8_t                    type = 0;
		std::optional<ActiveScriptData> data;
	};

	struct FunctionMessageData
	{
		std::uint8_t          fnFlags = 0;
		TStr                  scriptName = 0;
		TStr                  event = 0;
		Variable              unknownVar;
		std::vector<Variable> vars;
	};

	struct FunctionMessage
	{
		std::uint8_t                       flag = 0;
		std::optional<ThreadID>            id;  // present when flag <= 2 (Starfield: always)
		std::uint8_t                       flags = 0;
		std::optional<FunctionMessageData> data;
		// Starfield (0x1428c7bb0): u32 sfUnknown (message +0x10), u8 flag (+0xd0), u8 sfUnknownByte (+0xc0), u32 thread
		// id (id), u8 flags (1 = data follows)
		std::uint32_t sfUnknown = 0;
		std::uint8_t  sfUnknownByte = 0;
	};

	struct SuspendedStack
	{
		ThreadID                           id = 0;
		std::uint8_t                       flag = 0;
		std::optional<FunctionMessageData> data;
	};

	struct QueuedUnbind
	{
		EID           id = 0;
		std::uint32_t unknown = 0;
	};

	// Starfield: an object handle as the game saves it (0x141f50ec0): u16 kind, u16 hi, then for kinds 0, 1 and 2 a
	// 3-byte form reference (value = its 24 bits), for any other kind a u32 (the handle's low 32 bits for kind 3, else 0)
	struct ObjectHandle
	{
		std::uint16_t kind = 0;
		std::uint16_t hi = 0;
		std::uint32_t value = 0;
		bool          isRef() const { return kind <= 2; }
	};

	// Starfield: the events a form relays to script objects (EventRelays, 0x14287e8c0): per event its name and the
	// object (script instance) ids registered for it
	struct EventRelay
	{
		struct Event
		{
			TStr             name = 0;
			std::vector<EID> objects;
		};
		ObjectHandle       handle;
		std::vector<Event> events;
	};

	// Starfield: a finished latent call's result waiting for its thread (FreezeStatusPendingLatentsAndGuards)
	struct PendingLatent
	{
		std::uint32_t id = 0;
		Variable      value;
	};
	class Papyrus
	{
	public:
		void read(Reader& r);  // errors name the part being read ("struct data 12 of 300")
		void write(Writer& w) const;
		struct ReadStage
		{
			const char* what = nullptr;
			std::size_t index = 0, total = 0;
			void        at(const char* a_what, std::size_t a_index, std::size_t a_total)
			{
				what = a_what;
				index = a_index;
				total = a_total;
			}
			std::string text() const;
		};
		// Structural problems a read can pass but a write would not reproduce faithfully: string indices
		// out of range, duplicate ids, saved data missing or given twice. Empty for a sound section.
		std::vector<std::string> check() const;

		std::uint16_t                header = 0;
		// The game flagged this Papyrus data invalid when it saved: header 0xFFFF and nothing else of the script
		// engine's data, then the SkyrimVM part's own 0xFFFF (4 bytes in all, kept in tail). SkyrimSE.exe 1.6.1170
		// writes it when its script engine is not frozen at save time (SkyrimVM::SaveGame 0x9c9ef0: "Someone tried to
		// save without freezing the Papyrus VM first", MarkSaveInvalid 0x144bc70 writes u16 0xFFFF, then 0x9a3a30
		// writes u16 0xFFFF); the freeze gives up after 30 seconds (0x9c9db0, "VM timed out while waiting to freeze!
		// Save will be bad."). Loading it, the game ignores all of it (VM load 0x144b150 "VM tried to load save
		// flagged as invalid", SkyrimVM load 0x9cdbd0 "Papyrus attempted to load a save flagged as invalid").
		// Every list below stays empty; write() writes the block back as it was.
		bool                         invalid = false;
		static constexpr std::uint16_t kInvalidHeader = 0xFFFF;
		std::vector<std::string>     strings;
		std::vector<Script>          scripts;
		std::vector<StructDef>       structs;          // Fallout 4
		std::vector<ScriptInstance>  instances;
		std::vector<Reference>       references;
		std::vector<StructInstance>  structInstances;  // Fallout 4
		bool                         fo4 = false;      // read from a Fallout 4 save
		std::vector<ArrayInfo>       arrays;
		ThreadID                     runtime = 0;
		std::vector<ActiveScript>    activeScripts;
		std::vector<FunctionMessage> functionMessages;
		std::vector<SuspendedStack>  suspended1;
		std::vector<SuspendedStack>  suspended2;
		std::uint32_t                unk1 = 0;
		std::optional<std::uint32_t> unk2;
		std::vector<EID>             unknownIDs;
		std::vector<QueuedUnbind>    unbinds;
		std::optional<std::uint16_t> saveFileVersion;
		std::vector<std::uint8_t>    tail;  // "other data" block, kept verbatim (Starfield: the GameVM part, from its u16 version)

		// ---- Starfield (global data 1001 of a .sfs; see PapyrusSF.cpp and docs/CLAUDE-HANDOFF.md) ----
		// readStarfield() reads the section into the model above (instances/references share one "object data" list,
		// threads are the stacks, unknownIDs is ResetStatus); write() writes it back in the Starfield layout when set.
		bool                                               starfield = false;
		static constexpr std::uint16_t                     kStarfieldVMVersion = 10;  // InternalVMVersion
		void                                               readStarfield(Reader& r);   // r: exactly the 1001 data
		void                                               writeStarfield(Writer& w) const;
		std::vector<EID>                                   objectDataOrder;  // instance and reference data, as saved
		std::vector<PendingLatent>                         pendingLatents;
		std::vector<std::uint32_t>                         freezeU32s;  // the u32 list after the pending latents
		std::vector<EventRelay>                            eventRelays;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> guardGlobal1, guardGlobal2;  // GuardGlobalData
		// the GameVM part's version (first u16 of tail): 37 in 1.9.71 saves, 38 in 1.16.244; -1 when there is none
		int                                                gameVMVersion() const
		{
			return starfield && tail.size() >= 2 ? (tail[0] | (tail[1] << 8)) : -1;
		}

		// The game writes the per-element data blocks in its own order; keep it so an
		// unmodified save writes back byte-for-byte identical.
		std::vector<EID>      instanceDataOrder, referenceDataOrder, structDataOrder, arrayDataOrder;
		std::vector<ThreadID> activeDataOrder;

		const std::string& str(TStr i) const
		{
			static const std::string bad = "<bad string index>";
			return i < strings.size() ? strings[i] : bad;
		}
		TStr findString(const std::string& s) const;  // returns UINT32_MAX when missing
		TStr internString(const std::string& s);      // exact match, or appended to the table

		// lookup helpers (rebuilt by reindex())
		void                  reindex();
		const Script*         findScript(TStr name) const;
		ScriptInstance*       findInstance(EID id);
		Reference*            findReference(EID id);
		ArrayInfo*            findArray(EID id);
		StructInstance*       findStruct(EID id);
		ActiveScript*         findActive(ThreadID id);
		const ScriptInstance* findInstance(EID id) const { return const_cast<Papyrus*>(this)->findInstance(id); }
		const ArrayInfo*      findArray(EID id) const { return const_cast<Papyrus*>(this)->findArray(id); }
		const ActiveScript*   findActive(ThreadID id) const { return const_cast<Papyrus*>(this)->findActive(id); }

	private:
		void readInner(Reader& r, ReadStage& a_stage);
		void readStarfieldInner(Reader& r, ReadStage& a_stage);
		std::unordered_map<TStr, std::size_t> _scriptIdx;
		std::unordered_map<EID, std::size_t>      _instIdx, _refIdx, _arrIdx, _structIdx;
		std::unordered_map<ThreadID, std::size_t> _activeIdx;
	};

	// One thread's saved data as Papyrus::write writes it (with w's width flags): for measuring a thread's size.
	void WriteThreadData(Writer& w, ThreadID a_id, const ActiveScriptData& a_data);
	// ... in the Starfield layout
	void WriteThreadDataSF(Writer& w, ThreadID a_id, const ActiveScriptData& a_data);
	// One object's saved data in the Starfield layout (id + Object::Save 0x142803340): for measuring its size
	void WriteObjectDataSF(Writer& w, EID a_id, const InstanceData& a_data);
	// A variable in the Starfield layout (None: the type byte only, Bool: u8; Starfield.exe 0x14280b1c0)
	Variable ReadVariableSF(Reader& r);
	void     WriteVariableSF(Writer& w, const Variable& v);
}
