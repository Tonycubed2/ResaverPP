#pragma once
// Calling Papyrus functions from the helper, the way a script calls them, so the game's own rules run.
// LE's script engine is the same as SE's; its call functions are the same virtual functions, one slot
// lower (LE has no Initialize at 01). Checked against the game's own error strings in libSkyrim's
// VMState.h ("Unable to call static function", "Unable to call member function"):
//   0x23 SendEvent   0x25 DispatchStaticCall   0x26 DispatchMethodCall (object)   0x27 DispatchMethodCall (handle)
// The result arrives in a callback object (BSScript::IStackCallbackFunctor, LE layout: vtable, ref count;
// ~dtor, operator()(Variable), CanSave, SetObject - libSkyrim BSScriptStack.h).
// Call these on the game's main thread.

namespace papyrus
{
	// one argument of a call
	struct Arg
	{
		enum class Kind
		{
			kNone,
			kInt,
			kFloat,
			kBool,
			kString,
			kForm,   // a form or reference, by its own form type
			kAlias,  // a quest alias (the quest and the alias)
		};
		Kind          kind = Kind::kNone;
		SInt32        i = 0;
		float         f = 0.0f;
		bool          b = false;
		std::string   s;
		void*         object = nullptr;
		std::uint32_t typeID = 0;

		static Arg Int(SInt32 a_v) { Arg a; a.kind = Kind::kInt; a.i = a_v; return a; }
		static Arg Float(float a_v) { Arg a; a.kind = Kind::kFloat; a.f = a_v; return a; }
		static Arg Bool(bool a_v) { Arg a; a.kind = Kind::kBool; a.b = a_v; return a; }
		static Arg String(std::string a_v) { Arg a; a.kind = Kind::kString; a.s = std::move(a_v); return a; }
		static Arg Form(TESForm* a_form)
		{
			Arg a;
			a.kind = a_form ? Kind::kForm : Kind::kNone;
			a.object = a_form;
			a.typeID = a_form ? a_form->formType : 0;
			return a;
		}
	};
	using Args = std::vector<Arg>;

	// the value a Papyrus function returned
	struct Result
	{
		std::uint32_t type = 0;  // 0 none, 3 int, 4 float, 5 bool (VMValue types)
		std::uint32_t data = 0;
		bool          IsBool() const { return type == 5; }
		bool          GetBool() const { return (data & 0xFF) != 0; }
		bool          IsInt() const { return type == 3; }
		SInt32        GetInt() const { return static_cast<SInt32>(data); }
		// an object (e.g. the reference PlaceAtMe made): its form, when it has one
		TESForm*      form = nullptr;
	};
	using Done = std::function<void(const Result&)>;

	// the script engine's handle of a form (0 when it has none)
	UInt64 HandleOf(TESForm* a_form);

	// a_class.a_fn(...) on a_object (its scripts or its native Papyrus type). a_done may be empty.
	// False when the call could not even be queued (then a_done is not called).
	bool CallMethod(TESForm* a_object, const char* a_class, const char* a_fn, Args a_args, Done a_done = {});

	// a global function: Debug.Notification, Game.TeachWord...
	bool CallStatic(const char* a_class, const char* a_fn, Args a_args, Done a_done = {});

	// Set a Bool variable of the script a_class bound to a_object (a script's own variable, e.g. "__shiftingBack";
	// not a property). Checked in TESV.exe 1.9.32.0 (decrypted from Tony's Steam copy, SteamStub 2.1):
	//   - the script object: VM vtable slot 0x1B (0xC4D5F0) = FindBoundObject(UInt64 handle, const char* class,
	//     BSScriptObject** out): the class name is made a BSFixedString inside (0xC4C3C3 -> 0x43BA00), the object
	//     found gets a reference (0xC306A0) that the caller releases (0xC30720, = SKSE's VMIdentifier::DecrementLock;
	//     at 0 Destroy 0xC30E90 + free, = VMIdentifier::Destroy). SKSE's PackHandle uses it the same way (Unk_1A).
	//     The VM vtable is 0x0114C1B0 (set by its constructor 0xC54DE0, made by 0xC33360 from SkyrimVM's 0x8D82A4).
	//   - the variable's index: VMClassInfo::GetVariable 0xC33E30 (BSFixedString* name): searched in the class and
	//     its parents, index counted over the whole chain (parents' variables first, 0xC335F0 = their total), -1 none.
	//   - the variables: BSScriptObject + 0x20, 8 bytes each (VMValue), as the VM's own GetVariableValue (vtable
	//     slot 0x21, 0xC47E90) reads them: only when bit 2 of the object's first word (valid) is set and the index
	//     is below the chain's total.
	// Main thread. Returns 1 set (a_was = the old value), 0 the object has no such Bool variable, -1 no such script
	// on the object.
	int SetBoolVariable(TESForm* a_object, const char* a_class, const char* a_name, bool a_value, bool* a_was = nullptr);
}
