// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Starfield's Papyrus section (global data type 1001 of a .sfs body), read into the same model as Skyrim's and
// Fallout 4's (Papyrus.h) and written back byte for byte.
//
// Layout from the writers in Starfield.exe 1.16.244.0 (InternalVM SaveGame 0x142852d20 and what it calls), checked on
// saves of 1.9.71 and 1.16.244 (form versions 110 and 153): every count and id lands, every save parses to the start
// of the GameVM part. Addresses below are Starfield.exe 1.16.244 VAs. What differs from Fallout 4:
// - script definitions end with a guard name list; the object table's handle has a u32 instead of refID + byte for
//   kinds other than 0..2 (the same bytes as Fallout 4's kind 3 refID + 2 bytes)
// - object data: a count, then the attached and detached objects that have data in one list, each in the compact
//   layout of Object::Save 0x142803340 (flag byte decides which fields follow; guards with 0x80)
// - variables: None is the type byte alone, Bool a u8 (0x14280b1c0)
// - threads (stacks, 0x14281fc30): always the object id; callback kind 1 (event callback: name + payload + u32) or
//   2 (latent callback: u32, u8, u32); the byte after the frames only when flag & 2
// - opcodes 47..50 (ArrayGetAllMatchingStructs, LockGuards, UnlockGuards, TryLockGuards) and parameter type 9
//   (a string index, like 1, 2 and 8)
// - function messages: u32, u8, u8, u32 thread, u8 has-data (0x1428c7bb0)
// - after the freeze status: pending latent results and a u32 list; after the unbinds: EventRelays and
//   GuardGlobalData. The GameVM part (u16 version 37/38 and its handlers) is kept raw in tail.
#include "Papyrus.h"

#include <cstdio>
#include <unordered_set>

namespace ess
{
	namespace
	{
		// The writer's per-opcode argument counts: the opcode table in Starfield.exe (name + argument descriptor,
		// 0x144a5a9c8; the save writer's decoder 0x14288b690 matches it): fixed arguments, then for the variadic ones
		// (descriptor ending in '*') an int with the count and that many more (save writer 0x142881660)
		struct OpArgs
		{
			std::uint8_t fixed;
			bool         variadic;
		};
		constexpr OpArgs kSFOpcodeArgs[51] = {
			{ 0, false }, { 3, false }, { 3, false }, { 3, false }, { 3, false }, { 3, false }, { 3, false }, { 3, false },  // 0-7
			{ 3, false }, { 3, false }, { 2, false }, { 2, false }, { 2, false }, { 2, false }, { 2, false }, { 3, false },  // 8-15
			{ 3, false }, { 3, false }, { 3, false }, { 3, false }, { 1, false }, { 2, false }, { 2, false },                // 16-22
			{ 3, true }, { 2, true }, { 3, true },                                                                          // 23-25 calls
			{ 1, false }, { 3, false }, { 3, false }, { 3, false }, { 2, false }, { 2, false }, { 3, false }, { 3, false },  // 26-33
			{ 4, false }, { 4, false }, { 3, false }, { 1, false }, { 3, false }, { 3, false }, { 5, false }, { 5, false },  // 34-41
			{ 3, false }, { 3, false }, { 1, false }, { 3, false }, { 1, false }, { 6, false },                             // 42-47
			{ 0, true }, { 0, true }, { 1, true },                                                                          // 48-50 guards
		};

		bool IsStringParam(std::uint8_t t) { return t == 1 || t == 2 || t == 8 || t == 9; }

		VType ReadVTypeSF(Reader& r)
		{
			const auto pos = r.pos();
			const auto v = r.u8();
			if (v > 17 || (v >= 8 && v <= 10)) {
				throw FormatError("invalid variable type " + std::to_string(v), pos);
			}
			return static_cast<VType>(v);
		}

		std::vector<Variable> ReadVarsSF(Reader& r, std::uint32_t n)
		{
			std::vector<Variable> v;
			v.reserve(n < 100000 ? n : 100000);
			for (std::uint32_t i = 0; i < n; ++i) {
				v.push_back(ReadVariableSF(r));
			}
			return v;
		}
		void WriteVarsSF(Writer& w, const std::vector<Variable>& v)
		{
			for (const auto& x : v) {
				WriteVariableSF(w, x);
			}
		}

		ObjectHandle ReadHandle(Reader& r)
		{
			ObjectHandle h;
			h.kind = r.u16();
			h.hi = r.u16();
			h.value = h.isRef() ? RefID::read(r).data : r.u32();
			return h;
		}
		void WriteHandle(Writer& w, const ObjectHandle& h)
		{
			w.u16(h.kind);
			w.u16(h.hi);
			if (h.isRef()) {
				RefID id;
				id.data = h.value & 0xFFFFFF;
				id.write(w);
			} else {
				w.u32(h.value);
			}
		}

		std::vector<std::uint32_t> ReadU32s(Reader& r)
		{
			const auto                 n = r.u32();
			std::vector<std::uint32_t> v;
			r.need(static_cast<std::size_t>(n) * 4);
			v.reserve(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				v.push_back(r.u32());
			}
			return v;
		}
		void WriteU32s(Writer& w, const std::vector<std::uint32_t>& v)
		{
			w.u32(static_cast<std::uint32_t>(v.size()));
			for (auto x : v) {
				w.u32(x);
			}
		}

		// Object::Save 0x142803340 / Object::Load 0x142803cf0: flag 0x01 constructed, 0x02 valid, 0x08 initialized, 0x10 lock
		// bit, 0x20 no state name, 0x40 u32 remainingPropsToInit (unknown1), 0x04 u32 owning stack id (unknown2), 0x80 guards
		InstanceData ReadObjectData(Reader& r)
		{
			InstanceData d;
			d.flag = r.u8();
			if (!(d.flag & 0x20)) {
				d.state = r.tstr();
			}
			if (d.flag & 0x40) {
				d.unknown1 = r.u32();
			}
			if (d.flag & 0x04) {
				d.unknown2 = r.u32();
			}
			const auto n = r.u32();
			d.vars = ReadVarsSF(r, n);
			if (d.flag & 0x80) {
				const auto g = r.u32();
				for (std::uint32_t i = 0; i < g; ++i) {  // 0x1428364f0 / 0x142837c10
					ObjectGuard x;
					x.owner = r.u32();
					x.first = ReadU32s(r);
					x.second = ReadU32s(r);
					d.guards.push_back(std::move(x));
				}
			}
			return d;
		}
		void WriteObjectData(Writer& w, EID id, const InstanceData& d)
		{
			w.u64(id);
			w.u8(d.flag);
			if (!(d.flag & 0x20)) {
				w.tstr(d.state);
			}
			if (d.flag & 0x40) {
				w.u32(d.unknown1);
			}
			if (d.flag & 0x04) {
				w.u32(d.unknown2);
			}
			w.u32(static_cast<std::uint32_t>(d.vars.size()));
			WriteVarsSF(w, d.vars);
			if (d.flag & 0x80) {
				w.u32(static_cast<std::uint32_t>(d.guards.size()));
				for (const auto& g : d.guards) {
					w.u32(g.owner);
					WriteU32s(w, g.first);
					WriteU32s(w, g.second);
				}
			} else if (!d.guards.empty()) {
				throw std::runtime_error("object data with guards but without flag 0x80 cannot be written");
			}
		}

		// 0x14288ae30
		Param ReadParamSF(Reader& r)
		{
			Param      p;
			const auto pos = r.pos();
			p.type = r.u8();
			if (p.type == 0) {
			} else if (IsStringParam(p.type)) {
				p.value = r.tstr();
			} else if (p.type == 3 || p.type == 4) {
				p.value = r.u32();
			} else if (p.type == 5) {
				p.value = r.u8();
			} else {
				throw FormatError("invalid opcode parameter type " + std::to_string(p.type), pos);
			}
			return p;
		}
		void WriteParamSF(Writer& w, const Param& p)
		{
			w.u8(p.type);
			if (p.type == 0) {
			} else if (IsStringParam(p.type)) {
				w.tstr(p.value);
			} else if (p.type == 5) {
				w.u8(static_cast<std::uint8_t>(p.value));
			} else {
				w.u32(p.value);
			}
		}

		Opcode ReadOpcodeSF(Reader& r)
		{
			Opcode     o;
			const auto pos = r.pos();
			o.op = r.u8();
			if (o.op >= std::size(kSFOpcodeArgs)) {
				throw FormatError("invalid opcode " + std::to_string(o.op), pos);
			}
			const auto a = kSFOpcodeArgs[o.op];
			for (int i = 0; i < a.fixed; ++i) {
				o.params.push_back(ReadParamSF(r));
			}
			if (a.variadic) {
				const auto count = ReadParamSF(r);
				if (count.type != 3) {
					throw FormatError("variable-argument opcode without an integer count", r.pos());
				}
				o.params.push_back(count);
				for (std::uint32_t i = 0; i < count.value; ++i) {
					o.params.push_back(ReadParamSF(r));
				}
			}
			return o;
		}

		std::vector<MemberDesc> ReadDescs16(Reader& r)
		{
			const auto              n = r.u16();
			std::vector<MemberDesc> v;
			v.reserve(n);
			for (std::uint16_t i = 0; i < n; ++i) {
				MemberDesc m;
				m.name = r.tstr();
				m.type = r.tstr();
				v.push_back(m);
			}
			return v;
		}
		void WriteDescs16(Writer& w, const std::vector<MemberDesc>& v)
		{
			w.u16(static_cast<std::uint16_t>(v.size()));
			for (const auto& m : v) {
				w.tstr(m.name);
				w.tstr(m.type);
			}
		}

		// frame 0x14281ca10, function 0x142888550
		StackFrame ReadFrameSF(Reader& r)
		{
			StackFrame f;
			const auto varCount = r.u32();
			if (varCount > 50000) {
				throw FormatError("invalid stack frame variable count", r.pos());
			}
			f.flag = r.u8();
			f.fnType = r.u8();
			f.scriptName = r.tstr();
			f.baseName = r.tstr();
			f.event = r.tstr();
			if (!(f.flag & 1) && f.fnType == 0) {
				f.status = r.tstr();
			}
			f.opMajor = r.u8();
			f.opMinor = r.u8();
			f.returnType = r.tstr();
			f.docString = r.tstr();
			f.userFlags = r.u32();
			f.fnFlags = r.u8();
			f.params = ReadDescs16(r);
			f.locals = ReadDescs16(r);
			const auto oc = r.u16();
			f.code.reserve(oc);
			for (std::uint16_t i = 0; i < oc; ++i) {
				f.code.push_back(ReadOpcodeSF(r));
			}
			f.ptr = r.u32();
			f.owner = ReadVariableSF(r);
			f.vars = ReadVarsSF(r, varCount);
			return f;
		}
		void WriteFrameSF(Writer& w, const StackFrame& f)
		{
			w.u32(static_cast<std::uint32_t>(f.vars.size()));
			w.u8(f.flag);
			w.u8(f.fnType);
			w.tstr(f.scriptName);
			w.tstr(f.baseName);
			w.tstr(f.event);
			if (!(f.flag & 1) && f.fnType == 0) {
				w.tstr(f.status.value_or(0));
			}
			w.u8(f.opMajor);
			w.u8(f.opMinor);
			w.tstr(f.returnType);
			w.tstr(f.docString);
			w.u32(f.userFlags);
			w.u8(f.fnFlags);
			WriteDescs16(w, f.params);
			WriteDescs16(w, f.locals);
			w.u16(static_cast<std::uint16_t>(f.code.size()));
			for (const auto& o : f.code) {
				w.u8(o.op);
				for (const auto& p : o.params) {
					WriteParamSF(w, p);
				}
			}
			w.u32(f.ptr);
			WriteVariableSF(w, f.owner);
			WriteVarsSF(w, f.vars);
		}

		// the event callbacks' saved payloads (their vtable +0x10), after the u32-length type name
		bool CallbackPayloadSize(const std::string& a_type, std::size_t& a_size)
		{
			static const struct
			{
				const char* name;
				std::size_t size;
			} kTypes[] = { { "QuestStage", 3 + 2 + 1 }, { "SceneResults", 3 }, { "ScenePhaseResults", 3 + 4 }, { "SceneActionResults", 3 + 4 },
				{ "TopicInfo", 0 }, { "TerminalRunResults", 0 }, { "TerminalOpenView", 0 } };
			for (const auto& t : kTypes) {
				if (a_type == t.name) {
					a_size = t.size;
					return true;
				}
			}
			return false;
		}

		// stack 0x14284c020 (id) + Stack::Save 0x14281fc30
		ActiveScriptData ReadStackSF(Reader& r)
		{
			ActiveScriptData d;
			d.major = r.i8();
			d.minor = r.i8();
			d.unknownVar = ReadVariableSF(r);
			d.flag = r.u8();
			d.unknownByte = r.u8();
			if (d.flag & 1) {
				d.unknown2 = r.u32();
			}
			d.unknown3 = r.u8();  // callback kind (GameVM 0x141fb3570)
			if (d.unknown3 == 1) {
				FragmentTask t;
				t.kind = 1;
				t.typeCode = r.lstring();
				std::size_t len = 0;
				if (!CallbackPayloadSize(t.typeCode, len)) {
					throw FormatError("unknown stack callback type '" + t.typeCode + "'", r.pos());
				}
				t.payload = r.bytes(len);
				t.sfTail = r.u32();
				d.fragment = std::move(t);
			} else if (d.unknown3 == 2) {  // LatentCallback 0x141fb0b40: u32, u8
				FragmentTask t;
				t.kind = 2;
				t.payload = r.bytes(5);
				t.sfTail = r.u32();
				d.fragment = std::move(t);
			} else if (d.unknown3 != 0) {
				throw FormatError("unknown stack callback kind " + std::to_string(d.unknown3), r.pos() - 1);
			}
			d.attached = r.u64();
			const auto count = r.u32();
			if (count > 100000) {
				throw FormatError("invalid stack frame count", r.pos());
			}
			d.frames.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				d.frames.push_back(ReadFrameSF(r));
			}
			if (d.flag & 2) {
				d.unknown5 = r.u8();
			}
			return d;
		}

		FunctionMessageData ReadFMDataSF(Reader& r)  // 0x1428b8b30
		{
			FunctionMessageData d;
			d.fnFlags = r.u8();
			d.scriptName = r.tstr();
			d.event = r.tstr();
			d.unknownVar = ReadVariableSF(r);
			const auto n = r.u32();
			d.vars = ReadVarsSF(r, n);
			return d;
		}
		void WriteFMDataSF(Writer& w, const FunctionMessageData& d)
		{
			w.u8(d.fnFlags);
			w.tstr(d.scriptName);
			w.tstr(d.event);
			WriteVariableSF(w, d.unknownVar);
			w.u32(static_cast<std::uint32_t>(d.vars.size()));
			WriteVarsSF(w, d.vars);
		}

		std::vector<SuspendedStack> ReadSuspendedSF(Reader& r)  // 0x142873d60
		{
			std::vector<SuspendedStack> v;
			const auto                  n = r.u32();
			for (std::uint32_t i = 0; i < n; ++i) {
				SuspendedStack s;
				s.id = r.u32();
				s.flag = r.u8();
				if (s.flag != 0) {
					s.data = ReadFMDataSF(r);
				}
				v.push_back(std::move(s));
			}
			return v;
		}
		void WriteSuspendedSF(Writer& w, const std::vector<SuspendedStack>& v)
		{
			w.u32(static_cast<std::uint32_t>(v.size()));
			for (const auto& s : v) {
				w.u32(s.id);
				w.u8(s.data ? (s.flag ? s.flag : 1) : 0);
				if (s.data) {
					WriteFMDataSF(w, *s.data);
				}
			}
		}

		void WriteDescs32(Writer& w, const std::vector<MemberDesc>& v)
		{
			w.u32(static_cast<std::uint32_t>(v.size()));
			for (const auto& m : v) {
				w.tstr(m.name);
				w.tstr(m.type);
			}
		}
		std::vector<MemberDesc> ReadDescs32(Reader& r)
		{
			const auto              n = r.u32();
			std::vector<MemberDesc> v;
			for (std::uint32_t i = 0; i < n; ++i) {
				MemberDesc m;
				m.name = r.tstr();
				m.type = r.tstr();
				v.push_back(m);
			}
			return v;
		}

		// items in their recorded order, then any not in it (as Papyrus.cpp's WriteInOrder)
		template <class T, class ID, class F>
		void WriteInOrderSF(const std::vector<ID>& a_order, const std::vector<T>& a_items, F&& a_writeOne)
		{
			std::unordered_map<ID, const T*> byId;
			byId.reserve(a_items.size());
			for (const auto& it : a_items) {
				byId.emplace(it.id, &it);
			}
			std::unordered_set<ID> done;
			for (ID id : a_order) {
				auto f = byId.find(id);
				if (f != byId.end() && done.insert(id).second) {
					a_writeOne(*f->second);
				}
			}
			for (const auto& it : a_items) {
				if (!done.contains(it.id)) {
					a_writeOne(it);
				}
			}
		}
	}

	void WriteObjectDataSF(Writer& w, EID a_id, const InstanceData& a_data) { WriteObjectData(w, a_id, a_data); }

	Variable ReadVariableSF(Reader& r)
	{
		Variable v;
		v.type = ReadVTypeSF(r);
		switch (v.type) {
		case VType::kNull:
			break;
		case VType::kString:
			v.value = r.tstr();
			break;
		case VType::kInt:
		case VType::kFloat:
			v.value = r.u32();
			break;
		case VType::kBool:
			v.value = r.u8();
			break;
		case VType::kRef:
		case VType::kStruct:
			v.refType = r.tstr();
			v.value = r.u64();
			break;
		case VType::kVariant:
			v.inner = std::make_unique<Variable>(ReadVariableSF(r));
			break;
		default:  // arrays
			if (IsRefType(v.type)) {
				v.refType = r.tstr();
			}
			v.value = r.u64();
			break;
		}
		return v;
	}

	void WriteVariableSF(Writer& w, const Variable& v)
	{
		w.u8(static_cast<std::uint8_t>(v.type));
		switch (v.type) {
		case VType::kNull:
			break;
		case VType::kString:
			w.tstr(static_cast<std::uint32_t>(v.value));
			break;
		case VType::kInt:
		case VType::kFloat:
			w.u32(static_cast<std::uint32_t>(v.value));
			break;
		case VType::kBool:
			w.u8(static_cast<std::uint8_t>(v.value));
			break;
		case VType::kRef:
		case VType::kStruct:
			w.tstr(v.refType);
			w.u64(v.value);
			break;
		case VType::kVariant:
			if (v.inner) {
				WriteVariableSF(w, *v.inner);
			} else {
				w.u8(0);
			}
			break;
		default:
			if (IsRefType(v.type)) {
				w.tstr(v.refType);
			}
			w.u64(v.value);
			break;
		}
	}

	void WriteThreadDataSF(Writer& w, ThreadID a_id, const ActiveScriptData& d)
	{
		w.u32(a_id);
		w.i8(d.major);
		w.i8(d.minor);
		WriteVariableSF(w, d.unknownVar);
		w.u8(d.flag);
		w.u8(d.unknownByte);
		if (d.flag & 1) {
			w.u32(d.unknown2);
		}
		const std::uint8_t kind = d.fragment ? d.fragment->kind : 0;
		w.u8(kind);
		if (d.fragment) {
			if (kind == 1) {
				w.lstring(d.fragment->typeCode);
			} else if (kind != 2) {
				throw std::runtime_error("Starfield thread callback kind " + std::to_string(kind) + " cannot be written");
			}
			w.bytes(d.fragment->payload);
			w.u32(d.fragment->sfTail);
		}
		w.u64(d.attached.value_or(0));
		w.u32(static_cast<std::uint32_t>(d.frames.size()));
		for (const auto& f : d.frames) {
			WriteFrameSF(w, f);
		}
		if (d.flag & 2) {
			w.u8(d.unknown5.value_or(0));
		}
	}

	void Papyrus::readStarfield(Reader& r)
	{
		ReadStage st;
		try {
			readStarfieldInner(r, st);
		} catch (const FormatError& e) {
			throw FormatError(e.message + " while reading " + st.text(), e.pos);
		} catch (const std::exception& e) {
			throw std::runtime_error(std::string(e.what()) + " while reading " + st.text());
		}
	}

	void Papyrus::readStarfieldInner(Reader& r, ReadStage& st)
	{
		r.str16 = false;  // u32 string indices, u64 ids (the writers' u16 string mode was never seen)
		r.id32 = false;
		r.le = false;
		r.fo4 = false;
		starfield = true;
		fo4 = false;
		st.at("the header", 0, 0);
		header = r.u16();
		invalid = header == kInvalidHeader;
		if (invalid) {  // the VM was not frozen when the game saved (as in Skyrim and Fallout 4): nothing else of it
			tail = r.bytes(r.remaining());
			return;
		}
		if (header != kStarfieldVMVersion) {
			throw FormatError("unsupported Starfield Papyrus version " + std::to_string(header) + " (10 known)", 0);
		}

		const auto strCount = r.u32();  // StringTable 0x142828600
		for (std::uint32_t i = 0; i < strCount; ++i) {
			st.at("strings", i, strCount);
			strings.push_back(r.wstring());
		}

		const auto scriptCount = r.u32();  // TypeTable 0x142829da0
		const auto structCount = r.u32();
		for (std::uint32_t i = 0; i < scriptCount; ++i) {  // 0x14282a000
			st.at("script definitions", i, scriptCount);
			Script s;
			s.name = r.tstr();
			s.parent = r.tstr();
			s.members = ReadDescs32(r);
			const auto g = r.u32();
			for (std::uint32_t k = 0; k < g; ++k) {
				s.guards.push_back(r.tstr());
			}
			scripts.push_back(std::move(s));
		}
		for (std::uint32_t i = 0; i < structCount; ++i) {
			st.at("struct definitions", i, structCount);
			StructDef s;
			s.name = r.tstr();
			s.members = ReadDescs32(r);
			structs.push_back(std::move(s));
		}

		const auto instCount = r.u32();  // ObjectTable 0x14286d180, entries 0x14284ac80
		instances.reserve(instCount);
		for (std::uint32_t i = 0; i < instCount; ++i) {
			st.at("script instances", i, instCount);
			ScriptInstance s;
			s.id = r.u64();
			s.script = r.tstr();
			s.unknown2bits = r.u16();  // the handle's kind (0x141f50ec0)
			s.unknown = r.u16();
			// kinds 0..2: a 3-byte form reference, then the object's flag byte; other kinds: a u32, then the flag byte,
			// kept as Fallout 4 keeps its kind 3 (the u32's bytes in refID + unknownByte, the flag in unknownFO4Byte)
			s.refID = RefID::read(r);
			s.unknownByte = r.u8();
			if (s.unknown2bits > 2) {
				s.unknownFO4Byte = r.u8();
			}
			instances.push_back(std::move(s));
		}
		const auto refCount = r.u32();  // detached objects 0x14284b030
		for (std::uint32_t i = 0; i < refCount; ++i) {
			st.at("references", i, refCount);
			Reference x;
			x.id = r.u64();
			x.script = r.tstr();
			references.push_back(std::move(x));
		}
		const auto structInstCount = r.u32();
		for (std::uint32_t i = 0; i < structInstCount; ++i) {
			st.at("struct instances", i, structInstCount);
			StructInstance x;
			x.id = r.u64();
			x.def = r.tstr();
			structInstances.push_back(std::move(x));
		}
		const auto arrCount = r.u32();
		for (std::uint32_t i = 0; i < arrCount; ++i) {
			st.at("arrays", i, arrCount);
			ArrayInfo a;
			a.id = r.u64();
			a.type = ReadVTypeSF(r);
			if (IsRefType(a.type)) {
				a.refType = r.tstr();
			}
			a.length = r.u32();
			arrays.push_back(std::move(a));
		}

		runtime = r.u32();  // StackTable 0x142870430
		const auto stackCount = r.u32();
		for (std::uint32_t i = 0; i < stackCount; ++i) {
			st.at("threads", i, stackCount);
			ActiveScript a;
			a.id = r.u32();
			a.type = r.u8();
			activeScripts.push_back(std::move(a));
		}
		reindex();

		// ObjectData 0x142871670: a count, then attached and detached objects with data, in one list
		const auto odCount = r.u32();
		for (std::uint32_t i = 0; i < odCount; ++i) {
			st.at("object data", i, odCount);
			const auto pos = r.pos();
			const EID  id = r.u64();
			if (auto* inst = findInstance(id)) {
				if (inst->data) {
					throw FormatError("object data given twice", pos);
				}
				inst->data = ReadObjectData(r);
				instanceDataOrder.push_back(id);
			} else if (auto* ref = findReference(id)) {
				if (ref->data) {
					throw FormatError("object data given twice", pos);
				}
				ref->data = ReadObjectData(r);
				referenceDataOrder.push_back(id);
			} else {
				throw FormatError("object data for unknown id", pos);
			}
			objectDataOrder.push_back(id);
		}

		for (std::size_t i = 0; i < structInstances.size(); ++i) {  // StructData 0x14287b330, one per struct
			st.at("struct data", i, structInstances.size());
			const auto pos = r.pos();
			const EID  id = r.u64();
			auto*      x = findStruct(id);
			if (!x || x->hasData) {
				throw FormatError(x ? "struct data given twice" : "struct data for unknown id", pos);
			}
			x->flag = r.u8();
			const auto n = r.u32();
			x->vars = ReadVarsSF(r, n);
			x->hasData = true;
			structDataOrder.push_back(id);
		}

		for (std::size_t i = 0; i < arrays.size(); ++i) {  // ArrayData 0x142872510
			st.at("array data", i, arrays.size());
			const auto pos = r.pos();
			const EID  id = r.u64();
			auto*      a = findArray(id);
			if (!a || a->hasData) {
				throw FormatError(a ? "array data given twice" : "array data for unknown id", pos);
			}
			a->vars = ReadVarsSF(r, a->length);
			a->hasData = true;
			arrayDataOrder.push_back(id);
		}

		for (std::size_t i = 0; i < activeScripts.size(); ++i) {  // StackData 0x142872b60
			st.at("thread data", i, activeScripts.size());
			const auto     pos = r.pos();
			const ThreadID id = r.u32();
			auto*          a = findActive(id);
			if (!a || a->data) {
				char b[120];
				std::snprintf(b, sizeof(b), "thread data for %s id 0x%08X", a ? "an already read" : "an unknown", id);
				throw FormatError(b, pos);
			}
			a->data = ReadStackSF(r);
			activeDataOrder.push_back(id);
		}

		const auto fmCount = r.u32();  // FunctionMessageQueue 0x1428c7bb0
		for (std::uint32_t i = 0; i < fmCount; ++i) {
			st.at("function messages", i, fmCount);
			FunctionMessage m;
			m.sfUnknown = r.u32();
			m.flag = r.u8();
			m.sfUnknownByte = r.u8();
			m.id = r.u32();
			m.flags = r.u8();
			if (m.flags != 0) {
				m.data = ReadFMDataSF(r);
			}
			functionMessages.push_back(std::move(m));
		}

		st.at("suspended stacks 1", 0, 0);
		suspended1 = ReadSuspendedSF(r);
		st.at("suspended stacks 2", 0, 0);
		suspended2 = ReadSuspendedSF(r);

		st.at("freeze status", 0, 0);  // FreezeStatusPendingLatentsAndGuards (in 0x142852d20)
		unk1 = r.u32();
		if (unk1 != 0) {
			unk2 = r.u32();
		}
		const auto latentCount = r.u32();
		for (std::uint32_t i = 0; i < latentCount; ++i) {
			st.at("pending latent results", i, latentCount);
			PendingLatent l;
			l.id = r.u32();
			l.value = ReadVariableSF(r);
			pendingLatents.push_back(std::move(l));
		}
		st.at("the freeze status's u32 list", 0, 0);
		freezeU32s = ReadU32s(r);

		const auto resetCount = r.u32();  // ResetStatus
		for (std::uint32_t i = 0; i < resetCount; ++i) {
			st.at("reset status ids", i, resetCount);
			unknownIDs.push_back(r.u64());
		}

		const auto unbindCount = r.u32();  // QueuedUnbindArray 0x142873470
		for (std::uint32_t i = 0; i < unbindCount; ++i) {
			st.at("queued unbinds", i, unbindCount);
			QueuedUnbind u;
			u.id = r.u64();
			u.unknown = r.u32();
			unbinds.push_back(u);
		}

		const auto relayCount = r.u32();  // EventRelays 0x14287e8c0
		for (std::uint32_t i = 0; i < relayCount; ++i) {
			st.at("event relays", i, relayCount);
			EventRelay e;
			e.handle = ReadHandle(r);
			const auto n = r.u32();
			for (std::uint32_t k = 0; k < n; ++k) {
				EventRelay::Event ev;
				ev.name = r.tstr();
				const auto m = r.u32();
				r.need(static_cast<std::size_t>(m) * 8);
				for (std::uint32_t j = 0; j < m; ++j) {
					ev.objects.push_back(r.u64());
				}
				e.events.push_back(std::move(ev));
			}
			eventRelays.push_back(std::move(e));
		}

		st.at("guard global data", 0, 0);  // GuardGlobalData 0x142839560
		for (auto* list : { &guardGlobal1, &guardGlobal2 }) {
			const auto n = r.u32();
			r.need(static_cast<std::size_t>(n) * 8);
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto a = r.u32();
				list->emplace_back(a, r.u32());
			}
		}

		st.at("the GameVM part", 0, 0);
		if (r.remaining() < 2) {
			throw FormatError("the GameVM part is missing", r.pos());
		}
		tail = r.bytes(r.remaining());
	}

	void Papyrus::writeStarfield(Writer& w) const
	{
		w.str16 = false;
		w.id32 = false;
		w.le = false;
		w.fo4 = false;
		if (invalid) {
			w.u16(header);
			w.bytes(tail);
			return;
		}
		w.u16(header);
		w.u32(static_cast<std::uint32_t>(strings.size()));
		for (const auto& s : strings) {
			w.wstring(s);
		}

		w.u32(static_cast<std::uint32_t>(scripts.size()));
		w.u32(static_cast<std::uint32_t>(structs.size()));
		for (const auto& s : scripts) {
			w.tstr(s.name);
			w.tstr(s.parent);
			WriteDescs32(w, s.members);
			w.u32(static_cast<std::uint32_t>(s.guards.size()));
			for (TStr g : s.guards) {
				w.tstr(g);
			}
		}
		for (const auto& s : structs) {
			w.tstr(s.name);
			WriteDescs32(w, s.members);
		}

		w.u32(static_cast<std::uint32_t>(instances.size()));
		for (const auto& s : instances) {
			w.u64(s.id);
			w.tstr(s.script);
			w.u16(s.unknown2bits);
			w.u16(s.unknown);
			s.refID.write(w);
			w.u8(s.unknownByte);
			if (s.unknown2bits > 2) {
				w.u8(s.unknownFO4Byte.value_or(0));
			}
		}
		w.u32(static_cast<std::uint32_t>(references.size()));
		for (const auto& x : references) {
			w.u64(x.id);
			w.tstr(x.script);
		}
		w.u32(static_cast<std::uint32_t>(structInstances.size()));
		for (const auto& x : structInstances) {
			w.u64(x.id);
			w.tstr(x.def);
		}
		w.u32(static_cast<std::uint32_t>(arrays.size()));
		for (const auto& a : arrays) {
			w.u64(a.id);
			w.u8(static_cast<std::uint8_t>(a.type));
			if (IsRefType(a.type)) {
				w.tstr(a.refType);
			}
			w.u32(a.hasData ? static_cast<std::uint32_t>(a.vars.size()) : a.length);
		}

		w.u32(runtime);
		w.u32(static_cast<std::uint32_t>(activeScripts.size()));
		for (const auto& a : activeScripts) {
			w.u32(a.id);
			w.u8(a.type);
		}

		// object data: the saved order (instances and references mixed), then any object given data since
		{
			std::unordered_map<EID, const InstanceData*> data;
			for (const auto& i : instances) {
				if (i.data) {
					data.emplace(i.id, &*i.data);
				}
			}
			for (const auto& x : references) {
				if (x.data) {
					data.emplace(x.id, &*x.data);
				}
			}
			std::vector<EID> order;
			order.reserve(data.size());
			std::unordered_set<EID> done;
			for (EID id : objectDataOrder) {
				if (data.contains(id) && done.insert(id).second) {
					order.push_back(id);
				}
			}
			for (const auto& i : instances) {
				if (i.data && done.insert(i.id).second) {
					order.push_back(i.id);
				}
			}
			for (const auto& x : references) {
				if (x.data && done.insert(x.id).second) {
					order.push_back(x.id);
				}
			}
			w.u32(static_cast<std::uint32_t>(order.size()));
			for (EID id : order) {
				WriteObjectData(w, id, *data.at(id));
			}
		}

		WriteInOrderSF(structDataOrder, structInstances, [&](const StructInstance& x) {
			w.u64(x.id);
			w.u8(x.flag);
			w.u32(static_cast<std::uint32_t>(x.vars.size()));
			WriteVarsSF(w, x.vars);
		});
		WriteInOrderSF(arrayDataOrder, arrays, [&](const ArrayInfo& a) {
			w.u64(a.id);
			if (!a.hasData && a.length) {
				throw std::runtime_error("an array without its saved data cannot be written");
			}
			WriteVarsSF(w, a.vars);
		});
		WriteInOrderSF(activeDataOrder, activeScripts, [&](const ActiveScript& a) {
			if (!a.data) {
				throw std::runtime_error("a thread without its saved data cannot be written");
			}
			WriteThreadDataSF(w, a.id, *a.data);
		});

		w.u32(static_cast<std::uint32_t>(functionMessages.size()));
		for (const auto& m : functionMessages) {
			if (!m.id) {
				throw std::runtime_error("a Starfield function message without its thread cannot be written");
			}
			w.u32(m.sfUnknown);
			w.u8(m.flag);
			w.u8(m.sfUnknownByte);
			w.u32(*m.id);
			w.u8(m.data ? (m.flags ? m.flags : 1) : 0);
			if (m.data) {
				WriteFMDataSF(w, *m.data);
			}
		}
		WriteSuspendedSF(w, suspended1);
		WriteSuspendedSF(w, suspended2);

		w.u32(unk1);  // a count; when not 0, the id of the first thread in it follows
		if (unk1 != 0) {
			w.u32(unk2.value_or(0));
		}
		w.u32(static_cast<std::uint32_t>(pendingLatents.size()));
		for (const auto& l : pendingLatents) {
			w.u32(l.id);
			WriteVariableSF(w, l.value);
		}
		WriteU32s(w, freezeU32s);
		w.u32(static_cast<std::uint32_t>(unknownIDs.size()));
		for (EID id : unknownIDs) {
			w.u64(id);
		}
		w.u32(static_cast<std::uint32_t>(unbinds.size()));
		for (const auto& u : unbinds) {
			w.u64(u.id);
			w.u32(u.unknown);
		}

		// event relays: a script object that is no longer in the save is no longer registered for anything (in the
		// saves read, every registered id is a script instance); an event or relay left with nothing goes with it
		{
			std::unordered_set<EID> live;
			live.reserve(instances.size() + references.size());
			for (const auto& i : instances) {
				live.insert(i.id);
			}
			for (const auto& x : references) {
				live.insert(x.id);
			}
			std::vector<std::pair<const EventRelay*, std::vector<std::pair<const EventRelay::Event*, std::vector<EID>>>>> keep;
			for (const auto& e : eventRelays) {
				std::vector<std::pair<const EventRelay::Event*, std::vector<EID>>> evs;
				for (const auto& ev : e.events) {
					std::vector<EID> ids;
					for (EID id : ev.objects) {
						if (live.contains(id)) {
							ids.push_back(id);
						}
					}
					if (!ids.empty() || ev.objects.empty()) {
						evs.emplace_back(&ev, std::move(ids));
					}
				}
				if (!evs.empty() || e.events.empty()) {
					keep.emplace_back(&e, std::move(evs));
				}
			}
			w.u32(static_cast<std::uint32_t>(keep.size()));
			for (const auto& [e, evs] : keep) {
				WriteHandle(w, e->handle);
				w.u32(static_cast<std::uint32_t>(evs.size()));
				for (const auto& [ev, ids] : evs) {
					w.tstr(ev->name);
					w.u32(static_cast<std::uint32_t>(ids.size()));
					for (EID id : ids) {
						w.u64(id);
					}
				}
			}
		}

		for (const auto* list : { &guardGlobal1, &guardGlobal2 }) {
			w.u32(static_cast<std::uint32_t>(list->size()));
			for (const auto& [a, b] : *list) {
				w.u32(a);
				w.u32(b);
			}
		}
		w.bytes(tail);
	}
}
