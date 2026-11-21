#include "PCH.h"
#include "BodyWatch.h"

#include "Game.h"

#include <unordered_map>

namespace BodyWatch
{
	namespace
	{
		using Set3D_t = void(RE::Character*, RE::NiAVObject*, bool);
		REL::Relocation<Set3D_t> g_set3D;

		std::mutex                                                          g_lock;
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_removedAt;  // 3D removed near the player
		std::unordered_map<RE::FormID, std::string>                         g_lastState;   // the poll's last logged state
		std::atomic<int>                                                    g_logged{ 0 };

		std::string NameOf(RE::Actor* a)
		{
			const char* n = a ? a->GetDisplayFullName() : nullptr;
			return n && *n ? n : "(no name)";
		}

		bool IsFollower(RE::Actor* a)
		{
			if (a->IsPlayerTeammate()) {
				return true;
			}
			if (!a->GetActorBase()) {
				return false;  // no NPC record: the faction check reads it (crash fixed in version 20)
			}
			static RE::TESFaction* current = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);  // CurrentFollowerFaction
			return current && a->GetFactionRank(current, false) >= 0;
		}

		float DistanceToPlayer(RE::Actor* a)
		{
			auto* p = RE::PlayerCharacter::GetSingleton();
			if (!p || a->GetWorldspace() != p->GetWorldspace() || (!a->GetWorldspace() && a->GetParentCell() != p->GetParentCell())) {
				return -1.0f;
			}
			return a->GetPosition().GetDistance(p->GetPosition());
		}

		bool Loading()
		{
			auto* ui = RE::UI::GetSingleton();
			return !Game::InGame() || (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
		}

		// the caller chain as module+offset, the game's own frames folded
		std::string Stack()
		{
			void*      frames[48]{};
			const auto n = RtlCaptureStackBackTrace(2, 48, frames, nullptr);
			std::string out;
			int         shown = 0;
			for (USHORT i = 0; i < n && shown < 24; ++i) {
				HMODULE mod = nullptr;
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						static_cast<LPCWSTR>(frames[i]), &mod) ||
					!mod) {
					continue;
				}
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(mod, path, MAX_PATH);
				const auto name = std::filesystem::path(path).filename().string();
				out += std::format("{}{}+{:X}", out.empty() ? "" : " < ", name,
					reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(mod));
				++shown;
			}
			return out;
		}

		std::string Mods(const std::string& a_stack)
		{
			// the DLLs in the chain other than the game, this plugin and the system
			std::string mods;
			std::size_t pos = 0;
			while (pos < a_stack.size()) {
				auto end = a_stack.find(" < ", pos);
				if (end == std::string::npos) {
					end = a_stack.size();
				}
				auto       item = a_stack.substr(pos, end - pos);
				const auto plus = item.find('+');
				auto       mod = item.substr(0, plus);
				std::string low = mod;
				std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (low != "skyrimse.exe" && low != "resaverhelper.dll" && low != "ntdll.dll" && low != "kernel32.dll" && low != "kernelbase.dll" &&
					low.find("ucrtbase") == std::string::npos && mods.find(mod) == std::string::npos) {
					mods += (mods.empty() ? "" : ", ") + mod;
				}
				pos = end + 3;
			}
			return mods.empty() ? std::string("only SkyrimSE.exe (the game itself, or a Papyrus script through the game)") : mods;
		}

		void Set3DHook(RE::Character* a_this, RE::NiAVObject* a_obj, bool a_queue)
		{
			if (a_this && !Loading()) {
				const bool had = a_this->Get3D(false) != nullptr;
				if (!a_obj && had) {
					const bool dead = a_this->IsDead();
					if ((dead || IsFollower(a_this))) {
						const float d = DistanceToPlayer(a_this);
						if (d >= 0.0f && d < 8192.0f && g_logged < 2000) {
							++g_logged;
							const auto stack = Stack();
							spdlog::warn("BodyWatch: {} {:08X} ({}{}) lost its 3D, {:.0f} from the player. Removed by: {}", dead ? "dead body" : "follower",
								a_this->GetFormID(), NameOf(a_this), a_this->IsDisabled() ? ", switched off" : "", d, Mods(stack));
							spdlog::warn("BodyWatch:   call stack: {}", stack);
							std::scoped_lock l(g_lock);
							g_removedAt[a_this->GetFormID()] = std::chrono::steady_clock::now();
						}
					}
				} else if (a_obj && !had) {
					std::scoped_lock l(g_lock);
					if (auto it = g_removedAt.find(a_this->GetFormID()); it != g_removedAt.end()) {
						const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - it->second).count();
						spdlog::info("BodyWatch: {:08X} ({}) has its 3D again ({:.1f} s later)", a_this->GetFormID(), NameOf(a_this), ms / 1000.0);
						g_removedAt.erase(it);
					}
				}
			}
			g_set3D(a_this, a_obj, a_queue);
		}

		void Poll()
		{
			if (Loading()) {
				return;
			}
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			std::vector<RE::Actor*> watch;
			for (auto& h : lists->highActorHandles) {
				if (auto a = h.get(); a && IsFollower(a.get())) {
					watch.push_back(a.get());
				}
			}
			for (auto* a : watch) {
				const float d = DistanceToPlayer(a);
				if (d < 0.0f || d > 4096.0f) {
					continue;
				}
				bool       problem = false;
				const auto state = Describe(a, &problem);
				std::scoped_lock l(g_lock);
				auto& last = g_lastState[a->GetFormID()];
				if (state != last) {
					if (problem || !last.empty()) {
						spdlog::info("BodyWatch: follower {:08X} ({}) {:.0f} from the player: {}", a->GetFormID(), NameOf(a), d, state);
					}
					last = state;
				}
			}
		}
	}

	std::string Describe(RE::Actor* a, bool* a_problem)
	{
		bool        problem = true;
		std::string s;
		if (!a) {
			s = "gone";
		} else if (a->IsDisabled()) {
			s = "switched off (disabled)";
		} else if (a->IsDeleted()) {
			s = "deleted";
		} else if (auto* root = a->Get3D(false); !root) {
			s = "NO BODY (3D not loaded)";
		} else if (root->GetAppCulled()) {
			s = "hidden (3D culled)";
		} else if (const float al = a->GetAlpha(); al < 0.05f) {
			s = std::format("see-through (alpha {:.2f})", al);
		} else if (const float fade = root->AsFadeNode() ? root->AsFadeNode()->GetRuntimeData().currentFade : 1.0f; fade < 0.05f) {
			s = std::format("faded out (fade {:.2f})", fade);
		} else {
			s = "shown";
			problem = false;
		}
		if (a_problem) {
			*a_problem = problem;
		}
		return s;
	}

	bool Rebuild(RE::FormID a_id)
	{
		auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id);
		if (!a) {
			return false;
		}
		spdlog::info("BodyWatch: rebuilding {:08X} ({}): was {}", a_id, NameOf(a), Describe(a));
		a->Disable();
		std::thread([a_id] {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			SKSE::GetTaskInterface()->AddTask([a_id] {
				if (auto* b = RE::TESForm::LookupByID<RE::Actor>(a_id)) {
					b->Enable(false);
					spdlog::info("BodyWatch: {:08X} ({}) switched on again", a_id, NameOf(b));
				}
			});
		}).detach();
		return true;
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
		g_set3D = vtbl.write_vfunc(0x6C, &Set3DHook);
		spdlog::info("BodyWatch: watching followers' and dead bodies' 3D (Character::Set3D)");
		std::thread([] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (auto* t = SKSE::GetTaskInterface()) {
					t->AddTask([] { Poll(); });
				}
			}
		}).detach();
	}
}
