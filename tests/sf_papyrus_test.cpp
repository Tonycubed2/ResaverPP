// ReSaver++ - Starfield Papyrus section (global data 1001) tests.
// 1. Synthetic: a section built in memory with every Starfield-only part (guards, kind-3 handle, callbacks, opcodes
//    47..50, parameter type 9, pending latents, event relays, guard data) is written, read back and written again.
// 2. Real saves (RESAVERPP_SF_SAVES = .sfs files or folders, ';'-separated; the decompressed bodies (.raw) work too):
//    the 1001 section is found with a small test-only reader of the container and body, read, written back byte for
//    byte, then every Papyrus edit of Ops.h is applied to a fresh copy and the result must read back cleanly.
#include "core/Ops.h"
#include "core/Papyrus.h"

#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace ess;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                         \
	do {                                                                                 \
		if (c) {                                                                         \
			++g_pass;                                                                    \
		} else {                                                                         \
			++g_fail;                                                                    \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                    \
		}                                                                                \
	} while (0)

// ---- test-only .sfs reader: just enough to find global data 1001 (FORMAT: BCPS container, body header, FLT) ----
namespace
{
	using Bytes = std::vector<std::uint8_t>;

	Bytes ReadFile(const std::filesystem::path& p)
	{
		std::ifstream f(p, std::ios::binary);
		return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	}
	std::uint32_t U32(const Bytes& b, std::size_t at)
	{
		if (at + 4 > b.size()) {
			throw std::runtime_error("past end");
		}
		std::uint32_t v;
		std::memcpy(&v, b.data() + at, 4);
		return v;
	}
	std::uint64_t U64(const Bytes& b, std::size_t at)
	{
		if (at + 8 > b.size()) {
			throw std::runtime_error("past end");
		}
		std::uint64_t v;
		std::memcpy(&v, b.data() + at, 8);
		return v;
	}

	// "BCPS": chunk size table at [0x08], first chunk at [0x18], total size [0x20], chunk size [0x30], alignment [0x38];
	// each chunk a zlib stream (stored raw when its size equals its length), starts aligned
	Bytes Decompress(const Bytes& d)
	{
		const auto table = U64(d, 0x08), first = U64(d, 0x18), total = U64(d, 0x20), chunk = U64(d, 0x30), align = U64(d, 0x38);
		const auto n = (total + chunk - 1) / chunk;
		Bytes      out(total);
		auto       at = first;
		for (std::uint64_t i = 0; i < n; ++i) {
			const auto  size = U32(d, table + 4 * i);
			const auto  len = std::min<std::uint64_t>(chunk, total - i * chunk);
			mz_ulong    got = static_cast<mz_ulong>(len);
			if (size == len) {
				std::memcpy(out.data() + i * chunk, d.data() + at, len);
			} else if (at + size > d.size() || mz_uncompress(out.data() + i * chunk, &got, d.data() + at, size) != MZ_OK || got != len) {
				throw std::runtime_error("chunk " + std::to_string(i) + " does not unpack");
			}
			at += (size + align - 1) / align * align;
		}
		return out;
	}

	// body: "SFS_SAVEGAME", u32 header size + header, u8 form version, wstr x2, u32 plugin info size + info, FLT (25 x u32);
	// FLT[5] = global table 3, FLT[8] its count. Table 3 entries: 1000 = u32 type, vsval n, u16[n], u32 size, data;
	// 1001 = u32 type, u32 size, data.
	Bytes Papyrus1001(const Bytes& file)
	{
		const Bytes body = file.size() >= 4 && std::memcmp(file.data(), "BCPS", 4) == 0 ? Decompress(file) : file;
		if (body.size() < 16 || std::memcmp(body.data(), "SFS_SAVEGAME", 12) != 0) {
			throw std::runtime_error("not a Starfield save body");
		}
		std::size_t p = 12;
		p += 4 + U32(body, p);  // header
		p += 1;                 // form version
		for (int i = 0; i < 2; ++i) {
			p += 2 + (body.at(p) | (body.at(p + 1) << 8));
		}
		p += 4 + U32(body, p);  // plugin info
		const auto t3 = U32(body, p + 5 * 4), n3 = U32(body, p + 8 * 4);
		p = t3;
		for (std::uint32_t i = 0; i < n3; ++i) {
			const auto type = U32(body, p);
			p += 4;
			if (type == 1001) {
				const auto size = U32(body, p);
				if (p + 4 + size > body.size()) {
					throw std::runtime_error("1001 runs past the end");
				}
				return Bytes(body.begin() + p + 4, body.begin() + p + 4 + size);
			}
			if (type != 1000) {
				throw std::runtime_error("unexpected global type " + std::to_string(type) + " before 1001");
			}
			const auto b0 = body.at(p);
			const std::uint32_t raw = (b0 & 3) == 0 ? b0 : (b0 & 3) == 1 ? (b0 | (body.at(p + 1) << 8)) : U32(body, p);
			p += (b0 & 3) == 0 ? 1 : (b0 & 3) == 1 ? 2 : 4;
			p += 2 * (raw >> 2);
			p += 4 + U32(body, p);
		}
		throw std::runtime_error("no global data 1001");
	}
}

// ---- helpers ----
static Papyrus Read(const Bytes& b)
{
	Reader  r(b.data(), b.size());
	Papyrus p;
	p.readStarfield(r);
	return p;
}
static Bytes Write(const Papyrus& p)
{
	Writer w;
	p.write(w);
	return w.buf;
}

// ---- synthetic ----
static void TestSynthetic()
{
	// variables: None = the type byte only, Bool = u8, ints u32, refs tstr + u64
	{
		Writer   w;
		Variable none, b, i, ref;
		b.type = VType::kBool;
		b.value = 1;
		i.type = VType::kInt;
		i.value = 0xDEADBEEF;
		ref.type = VType::kRef;
		ref.refType = 7;
		ref.value = 0x1122334455667788ull;
		for (const auto* v : { &none, &b, &i, &ref }) {
			WriteVariableSF(w, *v);
		}
		const Bytes want = { 0, 5, 1, 3, 0xEF, 0xBE, 0xAD, 0xDE, 1, 7, 0, 0, 0, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
		CHECK(w.buf == want);
		Reader r(w.buf.data(), w.buf.size());
		CHECK(ReadVariableSF(r).type == VType::kNull);
		CHECK(ReadVariableSF(r).value == 1);
		CHECK(ReadVariableSF(r).value == 0xDEADBEEF);
		const auto x = ReadVariableSF(r);
		CHECK(x.refType == 7 && x.value == 0x1122334455667788ull && r.remaining() == 0);
	}

	Papyrus p;
	p.starfield = true;
	p.header = Papyrus::kStarfieldVMVersion;
	p.strings = { "", "Quest", "MyScript", "iVal", "Int", "GuardA", "OnEvent", "MyStruct", "Fn", "None" };
	Script s;
	s.name = 2;
	s.parent = 1;
	s.members = { { 3, 4 } };
	s.guards = { 5 };
	p.scripts.push_back(s);
	StructDef sd;
	sd.name = 7;
	sd.members = { { 3, 4 } };
	p.structs.push_back(sd);
	ScriptInstance a;  // kind 0 with a form
	a.id = 0x100;
	a.script = 2;
	a.refID.data = 0x400014;
	a.unknownByte = 1;
	InstanceData ad;
	ad.flag = 0x80 | 0x40 | 0x04 | 0x08;  // state, lock count, u32, guards
	ad.state = 0;
	ad.unknown1 = 3;
	ad.unknown2 = 9;
	Variable v;
	v.type = VType::kInt;
	v.value = 42;
	ad.vars = { v };
	ad.guards = { ObjectGuard{ 1, { 2, 3 }, { 4 } }, ObjectGuard{} };
	a.data = ad;
	p.instances.push_back(a);
	ScriptInstance k3;  // kind 3: a u32 and the flag byte
	k3.id = 0x200;
	k3.script = 2;
	k3.unknown2bits = 3;
	k3.refID.data = 0xABCDEF;
	k3.unknownByte = 0x12;
	k3.unknownFO4Byte = 1;
	p.instances.push_back(k3);
	Reference rf;
	rf.id = 0x300;
	rf.script = 2;
	InstanceData rd;
	rd.flag = 0x20;  // no state
	rf.data = rd;
	p.references.push_back(rf);
	StructInstance si;
	si.id = 0x400;
	si.def = 7;
	si.hasData = true;
	si.vars = { v };
	p.structInstances.push_back(si);
	ArrayInfo arr;
	arr.id = 0x500;
	arr.type = VType::kBoolArray;
	arr.hasData = true;
	Variable bv;
	bv.type = VType::kBool;
	arr.vars = { bv, Variable{} };
	arr.length = 2;
	p.arrays.push_back(arr);
	p.objectDataOrder = { 0x300, 0x100 };  // the reference first: a mixed order must survive

	auto frame = [&](std::vector<Opcode> code) {
		StackFrame f;
		f.flag = 0;
		f.fnType = 0;
		f.scriptName = 2;
		f.baseName = 2;
		f.event = 8;
		f.status = 0;
		f.opMajor = 3;
		f.opMinor = 12;
		f.returnType = 9;
		f.code = std::move(code);
		f.owner.type = VType::kRef;
		f.owner.refType = 2;
		f.owner.value = 0x100;
		f.vars = { v, bv };
		return f;
	};
	auto prm = [](std::uint8_t t, std::uint32_t val) { return Param{ t, val }; };
	ActiveScript t1;  // event callback (QuestStage), flag 1|2: caller id and the byte after the frames
	t1.id = 11;
	t1.type = 0;
	ActiveScriptData d1;
	d1.major = 3;
	d1.minor = 1;
	d1.flag = 3;
	d1.unknown2 = 12;
	d1.unknown3 = 1;
	FragmentTask ft;
	ft.kind = 1;
	ft.typeCode = "QuestStage";
	ft.payload = { 0, 0, 0x14, 10, 0, 1 };
	ft.sfTail = 77;
	d1.fragment = ft;
	d1.attached = 0x100;
	d1.frames.push_back(frame({ Opcode{ 48, { prm(3, 1), prm(9, 5) } }, Opcode{ 50, { prm(1, 3), prm(3, 1), prm(9, 5) } },
		Opcode{ 47, { prm(1, 3), prm(1, 3), prm(2, 3), prm(1, 3), prm(3, 0), prm(3, 1) } }, Opcode{ 41, { prm(1, 3), prm(1, 3), prm(2, 3), prm(1, 3), prm(3, 0) } },
		Opcode{ 23, { prm(1, 8), prm(1, 3), prm(1, 3), prm(3, 2), prm(5, 1), prm(4, 0x3F800000) } } }));
	d1.unknown5 = 0;
	t1.data = d1;
	ActiveScript t2;  // latent callback, flag 0
	t2.id = 12;
	t2.type = 1;
	ActiveScriptData d2;
	d2.unknown3 = 2;
	FragmentTask lt;
	lt.kind = 2;
	lt.payload = { 1, 2, 3, 4, 1 };
	lt.sfTail = 5;
	d2.fragment = lt;
	d2.frames.push_back(frame({ Opcode{ 49, { prm(3, 0) } } }));
	t2.data = d2;
	p.activeScripts = { t1, t2 };
	FunctionMessage m;
	m.sfUnknown = 22;
	m.flag = 1;
	m.sfUnknownByte = 1;
	m.id = 11;
	m.flags = 1;
	FunctionMessageData md;
	md.scriptName = 2;
	md.event = 8;
	md.vars = { v };
	m.data = md;
	p.functionMessages.push_back(m);
	SuspendedStack ss;
	ss.id = 12;
	p.suspended1.push_back(ss);
	p.unk1 = 1;
	p.unk2 = 12;
	p.pendingLatents.push_back(PendingLatent{ 12, v });
	p.freezeU32s = { 1, 2 };
	p.unknownIDs = { 0x100 };
	p.unbinds.push_back(QueuedUnbind{ 0x200, 1 });
	EventRelay er;
	er.handle.kind = 0;
	er.handle.value = 0x400014;
	er.events.push_back(EventRelay::Event{ 6, { 0x100, 0x200 } });
	p.eventRelays.push_back(er);
	EventRelay er3;
	er3.handle.kind = 3;
	er3.handle.value = 0x12345678;
	er3.events.push_back(EventRelay::Event{ 6, { 0x200 } });
	p.eventRelays.push_back(er3);
	p.guardGlobal1 = { { 1, 2 } };
	p.guardGlobal2 = { { 3, 4 }, { 5, 6 } };
	p.tail = { 38, 0, 0xAA, 0xBB };
	p.reindex();

	const Bytes b1 = Write(p);
	Papyrus     q;
	try {
		q = Read(b1);
	} catch (const std::exception& e) {
		std::printf("FAIL synthetic read: %s\n", e.what());
		++g_fail;
		return;
	}
	CHECK(Write(q) == b1);
	CHECK(q.check().empty());
	CHECK(q.scripts.size() == 1 && q.scripts[0].guards == std::vector<TStr>{ 5 });
	CHECK(q.objectDataOrder == (std::vector<EID>{ 0x300, 0x100 }));
	CHECK(q.instances[0].data && q.instances[0].data->guards.size() == 2 && q.instances[0].data->guards[0].first.size() == 2);
	CHECK(!q.instances[1].data && q.instances[1].unknownFO4Byte == 1);
	CHECK(q.references[0].data && q.references[0].data->flag == 0x20);
	CHECK(q.activeScripts[0].data->fragment->sfTail == 77 && q.activeScripts[0].data->unknown5);
	CHECK(q.activeScripts[0].data->frames[0].code.size() == 5 && q.activeScripts[0].data->frames[0].code[1].params.size() == 3);
	CHECK(q.activeScripts[1].data->fragment->kind == 2 && !q.activeScripts[1].data->unknown5);
	CHECK(q.pendingLatents.size() == 1 && q.freezeU32s.size() == 2 && q.eventRelays.size() == 2 && q.guardGlobal2.size() == 2);
	CHECK(q.gameVMVersion() == 38);

	// deleting an instance takes it out of the event relays; a relay left with nothing goes
	ops::DeleteInstances(q, { 0x200 });
	const Papyrus z = Read(Write(q));
	CHECK(z.eventRelays.size() == 1 && z.eventRelays[0].events[0].objects == std::vector<EID>{ 0x100 });
	CHECK(z.instances.size() == 1 && z.check().empty());

	// the invalid marker goes back as it was
	const Bytes inv = { 0xFF, 0xFF, 0xFF, 0xFF };
	const auto  pi = Read(inv);
	CHECK(pi.invalid && Write(pi) == inv);
	// an unknown version is refused
	const Bytes v11 = { 11, 0, 0, 0, 0, 0 };
	bool        refused = false;
	try {
		Read(v11);
	} catch (const FormatError&) {
		refused = true;
	}
	CHECK(refused);
}

// ---- real saves ----
static std::size_t CountRelayIds(const Papyrus& p)
{
	std::size_t n = 0;
	for (const auto& e : p.eventRelays) {
		for (const auto& ev : e.events) {
			n += ev.objects.size();
		}
	}
	return n;
}

static void TestSave(const std::filesystem::path& a_path)
{
	const auto name = a_path.filename().string();
	Bytes      sec;
	try {
		sec = Papyrus1001(ReadFile(a_path));
	} catch (const std::exception& e) {
		std::printf("FAIL %s: finding 1001: %s\n", name.c_str(), e.what());
		++g_fail;
		return;
	}
	Papyrus p;
	try {
		p = Read(sec);
	} catch (const std::exception& e) {
		std::printf("FAIL %s: read: %s\n", name.c_str(), e.what());
		++g_fail;
		return;
	}
	const Bytes out = Write(p);
	const bool  same = out == sec;
	CHECK(same);
	if (!same) {
		std::size_t i = 0;
		while (i < out.size() && i < sec.size() && out[i] == sec[i]) {
			++i;
		}
		std::printf("  %s: MISMATCH at byte %zu (sizes %zu / %zu)\n", name.c_str(), i, out.size(), sec.size());
	}
	const auto problems = p.check();
	CHECK(problems.empty());
	for (const auto& s : problems) {
		std::printf("  check: %s\n", s.c_str());
	}
	std::size_t withData = 0;
	for (const auto& i : p.instances) {
		withData += i.data ? 1 : 0;
	}
	std::printf("%s: 1001 = %zu bytes, round trip %s; %zu strings, %zu scripts, %zu structs, %zu instances (%zu with data), %zu "
				"references, %zu struct instances, %zu arrays, %zu threads, %zu function messages, %zu unbinds, %zu event relays (%zu "
				"ids), GameVM version %d (%zu bytes)\n",
		name.c_str(), sec.size(), same ? "identical" : "DIFFERENT", p.strings.size(), p.scripts.size(), p.structs.size(), p.instances.size(),
		withData, p.references.size(), p.structInstances.size(), p.arrays.size(), p.activeScripts.size(), p.functionMessages.size(),
		p.unbinds.size(), p.eventRelays.size(), CountRelayIds(p), p.gameVMVersion(), p.tail.size());

	// each edit on a fresh copy: written, read back, checked, written again identically
	auto op = [&](const char* what, const std::function<std::size_t(Papyrus&)>& a_edit, const std::function<bool(const Papyrus&)>& a_after) {
		Papyrus q = Read(sec);
		const auto n = a_edit(q);
		Bytes      b;
		Papyrus    r;
		try {
			b = Write(q);
			r = Read(b);
		} catch (const std::exception& e) {
			std::printf("FAIL %s: %s: %s\n", name.c_str(), what, e.what());
			++g_fail;
			return;
		}
		const bool clean = r.check().empty(), again = Write(r) == b, after = a_after(r);
		CHECK(clean);
		CHECK(again);
		CHECK(after);
		std::printf("  %s: %zu changed, %zu -> %zu bytes, reads back %s\n", what, n, sec.size(), b.size(), clean && again && after ? "cleanly" : "WITH PROBLEMS");
	};

	// instances: some with data, some without, some registered in event relays
	std::set<EID> insts;
	{
		std::set<EID> inRelay;
		for (const auto& e : p.eventRelays) {
			for (const auto& ev : e.events) {
				inRelay.insert(ev.objects.begin(), ev.objects.end());
			}
		}
		std::size_t a = 0, b = 0, c = 0;
		for (const auto& i : p.instances) {
			if (inRelay.contains(i.id) && a < 50) {
				insts.insert(i.id), ++a;
			} else if (i.data && b < 50) {
				insts.insert(i.id), ++b;
			} else if (!i.data && c < 50) {
				insts.insert(i.id), ++c;
			}
		}
	}
	op("delete 150 instances", [&](Papyrus& q) { return ops::DeleteInstances(q, insts); },
		[&](const Papyrus& r) {
			for (const auto& e : r.eventRelays) {
				for (const auto& ev : e.events) {
					for (EID id : ev.objects) {
						if (insts.contains(id)) {
							return false;
						}
					}
				}
			}
			return r.instances.size() == p.instances.size() - insts.size();
		});
	op("delete every reference", [&](Papyrus& q) {
		std::set<EID> ids;
		for (const auto& x : q.references) {
			ids.insert(x.id);
		}
		return ops::DeleteReferences(q, ids); },
		[&](const Papyrus& r) { return r.references.empty(); });
	op("delete 100 arrays", [&](Papyrus& q) {
		std::set<EID> ids;
		for (std::size_t i = 0; i < q.arrays.size() && i < 100; ++i) {
			ids.insert(q.arrays[i * 7 % q.arrays.size()].id);
		}
		return ops::DeleteArrays(q, ids); },
		[&](const Papyrus& r) { return r.arrays.size() < p.arrays.size(); });
	TStr busiest = 0;
	{
		std::unordered_map<TStr, std::size_t> n;
		for (const auto& i : p.instances) {
			++n[i.script];
		}
		std::size_t best = 0;
		for (const auto& s : p.scripts) {
			if (n[s.name] > best && !s.guards.empty()) {
				best = n[s.name], busiest = s.name;
			}
		}
	}
	op(("delete script " + p.str(busiest) + " (with guards) and its instances").c_str(), [&](Papyrus& q) { return ops::DeleteScripts(q, { busiest }); },
		[&](const Papyrus& r) {
			return !r.findScript(busiest) && std::none_of(r.instances.begin(), r.instances.end(), [&](const ScriptInstance& i) { return i.script == busiest; });
		});
	op("terminate every thread", [&](Papyrus& q) {
		std::set<ThreadID> ids;
		for (const auto& a : q.activeScripts) {
			ids.insert(a.id);
		}
		return ops::TerminateThreads(q, ids); },
		[&](const Papyrus& r) {
			for (const auto& a : r.activeScripts) {
				for (const auto& f : a.data->frames) {
					for (const auto& o : f.code) {
						if (o.op != 0 || !o.params.empty()) {
							return false;
						}
					}
				}
			}
			return r.activeScripts.size() == p.activeScripts.size();
		});
	op("remove every thread (with its messages and stacks)", [&](Papyrus& q) {
		std::set<ThreadID> ids;
		for (const auto& a : q.activeScripts) {
			ids.insert(a.id);
		}
		const auto d = ops::RemoveThreads(q, ids);
		return d.threads + d.messages + d.stacks; },
		[&](const Papyrus& r) { return r.activeScripts.empty() && r.functionMessages.empty(); });
	op("remove the first function message", [&](Papyrus& q) {
		const auto d = ops::RemoveFunctionMessages(q, { 0 });
		return d.threads + d.messages; },
		[&](const Papyrus& r) { return p.functionMessages.empty() || r.functionMessages.size() < p.functionMessages.size(); });
	op("remove stranded threads", [&](Papyrus& q) { return ops::RemoveStrandedThreads(q).threads; },
		[&](const Papyrus& r) { return ops::StrandedThreads(r).empty(); });
	op("delete every queued unbind", [&](Papyrus& q) {
		std::set<EID> ids;
		for (const auto& u : q.unbinds) {
			ids.insert(u.id);
		}
		return ops::DeleteUnbinds(q, ids); },
		[&](const Papyrus& r) { return r.unbinds.empty(); });
	op("remove unattached instances", [&](Papyrus& q) { return ops::RemoveUnattachedInstances(q); },
		[&](const Papyrus& r) { return std::none_of(r.instances.begin(), r.instances.end(), [](const ScriptInstance& i) { return ops::IsUnattached(i); }); });
	// (an instance whose script has no definition in a Starfield save is not undefined: the game defines only the scripts
	// of objects with data; these saves have no undefined script, so nothing goes)
	op("remove undefined elements", [&](Papyrus& q) { return ops::RemoveUndefinedElements(q); },
		[&](const Papyrus& r) { return r.instances.size() == p.instances.size() && r.scripts.size() == p.scripts.size(); });
	op("give a dataless instance data (written after the saved order)", [&](Papyrus& q) {
		for (auto& i : q.instances) {
			if (!i.data) {
				InstanceData d;
				d.flag = 0x20;
				i.data = d;
				return std::size_t{ 1 };
			}
		}
		return std::size_t{ 0 }; },
		[&](const Papyrus& r) { return r.objectDataOrder.size() == p.objectDataOrder.size() + 1; });
}

int main()
{
	TestSynthetic();
	std::vector<std::filesystem::path> saves;
	if (const char* env = std::getenv("RESAVERPP_SF_SAVES"); env && *env) {
		std::string s = env;
		std::size_t at = 0;
		while (at <= s.size()) {
			const auto end = s.find(';', at);
			const auto one = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
			if (!one.empty()) {
				std::error_code ec;
				if (std::filesystem::is_directory(one, ec)) {
					for (const auto& e : std::filesystem::directory_iterator(one)) {
						const auto ext = e.path().extension().string();
						if (ext == ".sfs" || ext == ".raw") {
							saves.push_back(e.path());
						}
					}
				} else {
					saves.emplace_back(one);
				}
			}
			if (end == std::string::npos) {
				break;
			}
			at = end + 1;
		}
		std::sort(saves.begin(), saves.end());
		if (saves.empty()) {
			std::printf("FAIL RESAVERPP_SF_SAVES lists no saves\n");
			++g_fail;
		}
	} else {
		std::printf("(Starfield Papyrus: RESAVERPP_SF_SAVES not set, real saves skipped)\n");
	}
	for (const auto& s : saves) {
		TestSave(s);
	}
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
