#include "PCH.h"
#include "Papyrus.h"

namespace papyrus
{
	namespace
	{
		VMClassRegistry* Registry()
		{
			return g_skyrimVM && *g_skyrimVM ? (*g_skyrimVM)->GetClassRegistry() : nullptr;
		}

		// BSScript::IStackCallbackFunctor as the game expects it: vtable at 00, reference count at 04.
		// The game holds its own reference while the call runs and releases it after calling us.
		class Callback
		{
		public:
			explicit Callback(Done a_done) :
				_done(std::move(a_done))
			{}

			virtual ~Callback() = default;  // 00 (the game deletes through it when the count reaches 0)

			// 01: the function's return value, passed by value (the callee destroys it)
			virtual void Call(VMValue a_result)
			{
				if (!_done) {
					return;
				}
				Result r;
				r.type = a_result.type;
				r.data = a_result.data.u;
				if (a_result.IsIdentifier() && a_result.data.id) {
					// an object: the form behind it (PlaceAtMe returns the new reference)
					const UInt64 handle = a_result.data.id->GetHandle();
					auto*        policy = g_objectHandlePolicy ? *g_objectHandlePolicy : nullptr;
					if (policy && policy->Unk_02(handle)) {
						for (const UInt32 type : { static_cast<UInt32>(kFormType_Reference), static_cast<UInt32>(kFormType_Character) }) {
							if (policy->IsType(type, handle)) {
								r.form = static_cast<TESForm*>(policy->Resolve(type, handle));
								break;
							}
						}
					}
				}
				try {
					_done(r);
				} catch (const std::exception& e) {
					spdlog::error("Papyrus callback failed: {}", e.what());
				} catch (...) {
					spdlog::error("Papyrus callback failed");
				}
			}
			virtual bool CanSave() { return false; }  // 02
			virtual void SetObject(void*) {}          // 03 (const BSTSmartPointer<Object>&)

			volatile LONG refCount = 1;  // 04: our own reference, released after the dispatch

			static void Release(Callback* a_cb)
			{
				if (a_cb && InterlockedDecrement(&a_cb->refCount) == 0) {
					delete a_cb;
				}
			}

		private:
			Done _done;
		};

		// the arguments, copied into the call's stack frame by the game. Made with new and never deleted
		// here: CommonLibSSE does the same (whether the game frees it or not, this is safe).
		class Arguments : public IFunctionArguments
		{
		public:
			explicit Arguments(Args a_args) :
				_args(std::move(a_args))
			{}
			virtual ~Arguments() {}

			virtual bool Copy(Output* a_dst) override
			{
				auto* reg = Registry();
				a_dst->Resize(static_cast<UInt32>(_args.size()));
				for (UInt32 i = 0; i < _args.size(); ++i) {
					VMValue* v = a_dst->Get(i);
					if (!v) {
						continue;
					}
					const auto& a = _args[i];
					switch (a.kind) {
					case Arg::Kind::kInt:
						v->SetInt(a.i);
						break;
					case Arg::Kind::kFloat:
						v->SetFloat(a.f);
						break;
					case Arg::Kind::kBool:
						v->SetBool(a.b);
						break;
					case Arg::Kind::kString:
						v->SetString(a.s.c_str());
						break;
					case Arg::Kind::kForm:
					case Arg::Kind::kAlias:
						if (reg && a.object) {
							PackHandle(v, a.object, a.typeID, reg);
						} else {
							v->SetNone();
						}
						break;
					default:
						v->SetNone();
						break;
					}
				}
				return true;
			}

		private:
			Args _args;
		};

		void* Slot(VMClassRegistry* a_vm, std::size_t a_index) { return (*reinterpret_cast<void***>(a_vm))[a_index]; }
	}

	UInt64 HandleOf(TESForm* a_form)
	{
		auto* reg = Registry();
		auto* policy = reg ? reg->GetHandlePolicy() : nullptr;
		if (!policy || !a_form) {
			return 0;
		}
		const UInt64 h = policy->Create(a_form->formType, a_form);
		return h == policy->GetInvalidHandle() ? 0 : h;
	}

	bool CallMethod(TESForm* a_object, const char* a_class, const char* a_fn, Args a_args, Done a_done)
	{
		auto*        reg = Registry();
		const UInt64 handle = HandleOf(a_object);
		if (!reg || !handle) {
			return false;
		}
		using Fn = bool(__thiscall*)(VMClassRegistry*, UInt64, const BSFixedString*, const BSFixedString*, IFunctionArguments*, Callback**);
		const BSFixedString cls(a_class), fn(a_fn);
		auto*               cb = new Callback(std::move(a_done));
		auto*               args = new Arguments(std::move(a_args));
		const bool          ok = reinterpret_cast<Fn>(Slot(reg, 0x27))(reg, handle, &cls, &fn, args, &cb);
		Callback::Release(cb);
		return ok;
	}

	int SetBoolVariable(TESForm* a_object, const char* a_class, const char* a_name, bool a_value, bool* a_was)
	{
		auto*        reg = Registry();
		const UInt64 handle = HandleOf(a_object);
		if (!reg || !handle) {
			return -1;
		}
		using Find = bool(__thiscall*)(VMClassRegistry*, UInt64, const char*, VMIdentifier**);
		VMIdentifier* obj = nullptr;
		if (!reinterpret_cast<Find>(Slot(reg, 0x1B))(reg, handle, a_class, &obj) || !obj) {
			return -1;
		}
		int result = 0;
		auto* type = obj->m_type;
		if ((*reinterpret_cast<const UInt32*>(obj) & 4) != 0 && type) {
			// the chain's variable count, as 0xC335F0 counts it (only linked classes: low two bits of +0x10 = 3)
			UInt32 total = 0;
			for (auto* c = type; c && (c->unk10 & 3) == 3; c = c->parent) {
				total += (c->unk10 >> 8) & 0x3FF;
			}
			BSFixedString name(a_name);
			const SInt32  index = CALL_MEMBER_FN(type, GetVariable)(&name);
			if (index >= 0 && static_cast<UInt32>(index) < total) {
				auto* v = reinterpret_cast<VMValue*>(reinterpret_cast<UInt8*>(obj) + 0x20) + index;
				if (v->type == VMValue::kType_Bool) {
					if (a_was) {
						*a_was = v->data.b;
					}
					v->data.u = 0;  // a Bool holds no reference: the value is the whole data word
					v->data.b = a_value;
					result = 1;
				}
			}
		}
		if (!obj->DecrementLock()) {
			obj->Destroy();
		}
		return result;
	}

	bool CallStatic(const char* a_class, const char* a_fn, Args a_args, Done a_done)
	{
		auto* reg = Registry();
		if (!reg) {
			return false;
		}
		using Fn = bool(__thiscall*)(VMClassRegistry*, const BSFixedString*, const BSFixedString*, IFunctionArguments*, Callback**);
		const BSFixedString cls(a_class), fn(a_fn);
		auto*               cb = new Callback(std::move(a_done));
		auto*               args = new Arguments(std::move(a_args));
		const bool          ok = reinterpret_cast<Fn>(Slot(reg, 0x25))(reg, &cls, &fn, args, &cb);
		Callback::Release(cb);
		return ok;
	}
}
