#include "PCH.h"
#include "SaveDialog.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace SaveDialog
{
	namespace
	{
		std::atomic<bool> g_busy{ false };

		// releases a COM pointer when it goes out of scope
		template <class T>
		struct Com
		{
			T* p = nullptr;
			Com() = default;
			Com(const Com&) = delete;
			Com& operator=(const Com&) = delete;
			~Com()
			{
				if (p) {
					p->Release();
				}
			}
			T*  operator->() const { return p; }
			T** operator&() { return &p; }
			explicit operator bool() const { return p != nullptr; }
		};

		std::string Utf8(const std::wstring& a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int n = WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
			if (n > 0) {
				WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), out.data(), n, nullptr, nullptr);
			}
			return out;
		}
		std::string Text(const std::filesystem::path& a_path) { return Utf8(a_path.wstring()); }

		// the game's window: the largest visible top-level window of this process with no owner
		HWND GameWindow()
		{
			struct Find
			{
				HWND best = nullptr;
				long area = 0;
			} f;
			EnumWindows(
				[](HWND a_wnd, LPARAM a_param) -> BOOL {
					auto* f = reinterpret_cast<Find*>(a_param);
					DWORD pid = 0;
					GetWindowThreadProcessId(a_wnd, &pid);
					if (pid != GetCurrentProcessId() || !IsWindowVisible(a_wnd) || GetWindow(a_wnd, GW_OWNER)) {
						return TRUE;
					}
					RECT r{};
					GetWindowRect(a_wnd, &r);
					const long area = (r.right - r.left) * (r.bottom - r.top);
					if (area > f->area) {
						f->area = area;
						f->best = a_wnd;
					}
					return TRUE;
				},
				reinterpret_cast<LPARAM>(&f));
			return f.best;
		}

		// puts the dialog on top of the game and in front when it opens (its first OnFolderChange comes while Show()
		// builds the window). Nothing else: every other event keeps the dialog's default behaviour.
		class Events final : public IFileDialogEvents
		{
		public:
			IFACEMETHODIMP QueryInterface(REFIID a_iid, void** a_out) override
			{
				if (!a_out) {
					return E_POINTER;
				}
				if (IsEqualIID(a_iid, IID_IUnknown) || IsEqualIID(a_iid, IID_IFileDialogEvents)) {
					*a_out = static_cast<IFileDialogEvents*>(this);
					AddRef();
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			IFACEMETHODIMP_(ULONG) AddRef() override { return ++_refs; }
			IFACEMETHODIMP_(ULONG) Release() override
			{
				const ULONG n = --_refs;
				if (n == 0) {
					delete this;
				}
				return n;
			}
			IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
			IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
			IFACEMETHODIMP OnFolderChange(IFileDialog* a_dialog) override
			{
				if (!_raised && a_dialog) {
					_raised = true;
					Com<IOleWindow> ole;
					HWND            wnd = nullptr;
					if (SUCCEEDED(a_dialog->QueryInterface(IID_IOleWindow, reinterpret_cast<void**>(&ole))) && ole && SUCCEEDED(ole->GetWindow(&wnd)) && wnd) {
						SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
						SetForegroundWindow(wnd);
					}
				}
				return S_OK;
			}
			IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
			IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* a_r) override
			{
				if (a_r) {
					*a_r = FDESVR_DEFAULT;
				}
				return S_OK;
			}
			IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
			IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* a_r) override
			{
				if (a_r) {
					*a_r = FDEOR_DEFAULT;
				}
				return S_OK;
			}

		private:
			std::atomic<ULONG> _refs{ 1 };
			bool               _raised = false;
		};

		enum class Ask
		{
			kChosen,
			kCancelled,
			kNoDialog,
		};

		// shows the dialog (on the calling thread, COM initialised); a_hr says why when kNoDialog
		Ask Show(const std::filesystem::path& a_folder, const std::filesystem::path& a_name, const std::string& a_what,
			std::filesystem::path& a_chosen, HRESULT& a_hr)
		{
			Com<IFileSaveDialog> dlg;
			a_hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, reinterpret_cast<void**>(&dlg));
			if (FAILED(a_hr) || !dlg) {
				return Ask::kNoDialog;
			}
			static const COMDLG_FILTERSPEC kTypes[] = { { L"Text file (*.txt)", L"*.txt" }, { L"All files (*.*)", L"*.*" } };
			dlg->SetFileTypes(2, kTypes);
			dlg->SetFileTypeIndex(1);
			dlg->SetDefaultExtension(L"txt");
			const auto title = std::wstring(L"ReSaver Plus: save ") + std::filesystem::path(a_what).wstring();
			dlg->SetTitle(title.c_str());
			dlg->SetFileName(a_name.wstring().c_str());
			FILEOPENDIALOGOPTIONS opts = 0;
			dlg->GetOptions(&opts);
			// FOS_NOCHANGEDIR: the game's working directory (its folder, where Data is) must not change
			dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
			if (!a_folder.empty()) {
				Com<IShellItem> folder;
				if (SUCCEEDED(SHCreateItemFromParsingName(a_folder.wstring().c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&folder))) && folder) {
					dlg->SetFolder(folder.p);  // SetFolder, not SetDefaultFolder: always start in the proposed folder
				}
			}
			auto* events = new Events();
			DWORD cookie = 0;
			const bool advised = SUCCEEDED(dlg->Advise(events, &cookie));
			a_hr = dlg->Show(nullptr);  // no owner: see SaveDialog.h
			if (advised) {
				dlg->Unadvise(cookie);
			}
			events->Release();
			if (a_hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
				return Ask::kCancelled;
			}
			if (FAILED(a_hr)) {
				return Ask::kNoDialog;
			}
			Com<IShellItem> item;
			PWSTR           path = nullptr;
			a_hr = dlg->GetResult(&item);
			if (FAILED(a_hr) || !item || FAILED(a_hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
				return Ask::kNoDialog;
			}
			a_chosen = path;
			CoTaskMemFree(path);
			return Ask::kChosen;
		}

		Result Run(const std::filesystem::path& a_folder, const std::filesystem::path& a_name, const std::string& a_text, const std::string& a_what)
		{
			Result     r;
			const auto fallback = a_folder / a_name;
			auto       writeDefault = [&](const std::string& a_why) {
                std::string err;
                if (WriteText(fallback, a_text, err)) {
                    r.kind = Result::Kind::kDefault;
                    r.path = fallback;
                    r.message = a_why + ": saved to " + Text(fallback) + " instead.";
                } else {
                    r.kind = Result::Kind::kFailed;
                    r.message = a_why + ", and " + err + ".";
                }
			};
			if (ExclusiveFullscreen()) {
				spdlog::info("F1 page: exclusive fullscreen, no Save As dialog for {}", a_what);
				writeDefault("The game runs in exclusive fullscreen, where a Save As dialog cannot be shown");
				return r;
			}
			const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			const HWND    game = GameWindow();
			std::filesystem::path chosen;
			HRESULT               hr = S_OK;
			const auto            ask = SUCCEEDED(init) ? Show(a_folder, a_name, a_what, chosen, hr) : (hr = init, Ask::kNoDialog);
			if (SUCCEEDED(init)) {
				CoUninitialize();
			}
			if (game) {
				SetForegroundWindow(game);  // back to the game (this process had the front: the dialog)
			}
			switch (ask) {
			case Ask::kCancelled:
				r.kind = Result::Kind::kCancelled;
				r.message = "Not saved: the Save As dialog was closed.";
				break;
			case Ask::kNoDialog:
				spdlog::warn("F1 page: the Save As dialog could not be shown (HRESULT {:08X})", static_cast<std::uint32_t>(hr));
				writeDefault(std::format("The Save As dialog could not be shown (error {:08X})", static_cast<std::uint32_t>(hr)));
				break;
			case Ask::kChosen:
				{
					std::string err;
					if (WriteText(chosen, a_text, err)) {
						r.kind = Result::Kind::kSaved;
						r.path = chosen;
						r.message = "Saved: " + Text(chosen);
					} else {
						r.kind = Result::Kind::kFailed;
						r.message = "Not saved: " + err + ".";
					}
				}
				break;
			}
			return r;
		}
	}

	bool Busy() { return g_busy; }

	bool SaveText(std::filesystem::path a_folder, std::filesystem::path a_name, std::string a_text, std::string a_what,
		std::function<void(const Result&)> a_done)
	{
		if (g_busy.exchange(true)) {
			return false;
		}
		std::thread([folder = std::move(a_folder), name = std::move(a_name), text = std::move(a_text), what = std::move(a_what),
						done = std::move(a_done)]() {
			Result r;
			try {
				r = Run(folder, name, text, what);
			} catch (const std::exception& e) {
				r.kind = Result::Kind::kFailed;
				r.message = std::string("Not saved: ") + e.what();
			} catch (...) {
				r.kind = Result::Kind::kFailed;
				r.message = "Not saved (unknown error).";
			}
			spdlog::info("F1 page: {}", r.message);
			g_busy = false;
			if (done) {
				try {
					done(r);
				} catch (...) {
				}
			}
		}).detach();
		return true;
	}

	std::filesystem::path Desktop()
	{
		PWSTR                 p = nullptr;
		std::filesystem::path out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
			out = p;
		}
		if (p) {
			CoTaskMemFree(p);
		}
		return out;
	}

	bool ExclusiveFullscreen()
	{
		QUERY_USER_NOTIFICATION_STATE st{};
		return SUCCEEDED(SHQueryUserNotificationState(&st)) && st == QUNS_RUNNING_D3D_FULL_SCREEN;
	}

	bool WriteText(const std::filesystem::path& a_path, const std::string& a_text, std::string& a_error)
	{
		std::error_code ec;
		if (a_path.has_parent_path()) {
			std::filesystem::create_directories(a_path.parent_path(), ec);
		}
		std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
		if (out) {
			out << a_text;
		}
		if (!out) {
			a_error = "could not write " + Text(a_path);
			return false;
		}
		return true;
	}

	std::string PathText(const std::filesystem::path& a_path) { return Text(a_path); }
}
