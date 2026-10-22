// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#ifdef _WIN32
#	include <windows.h>
#	include <shellapi.h>
#	include <objbase.h>
#endif

#include "App.h"
#include "Platform.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	App*                     g_app = nullptr;
	std::vector<std::string> g_dropped;

	void OnDrop(GLFWwindow*, int count, const char** paths)
	{
		for (int i = 0; i < count; ++i) {
			g_dropped.emplace_back(paths[i]);
		}
	}

	ImFont* g_mono = nullptr;

	// fixed-width font for the script viewer
	void LoadMonoFont(float a_scale)
	{
		ImGuiIO&    io = ImGui::GetIO();
		const char* candidates[] = {
			"C:\\Windows\\Fonts\\consola.ttf",
			"C:\\Windows\\Fonts\\cour.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
			"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
		};
		for (const char* f : candidates) {
			std::error_code ec;
			if (std::filesystem::exists(f, ec)) {
				if ((g_mono = io.Fonts->AddFontFromFileTTF(f, 16.0f * a_scale)) != nullptr) {
					return;
				}
			}
		}
		ImFontConfig cfg;
		cfg.SizePixels = 13.0f * a_scale;
		g_mono = io.Fonts->AddFontDefault(&cfg);  // ProggyClean is fixed-width too
	}

	void LoadFonts(float a_scale)
	{
		ImGuiIO& io = ImGui::GetIO();
		const float size = 17.0f * a_scale;
		const char* candidates[] = {
			"C:\\Windows\\Fonts\\segoeui.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"/usr/share/fonts/TTF/DejaVuSans.ttf",
		};
		for (const char* f : candidates) {
			std::error_code ec;
			if (std::filesystem::exists(f, ec)) {
				if (io.Fonts->AddFontFromFileTTF(f, size)) {
					return;
				}
			}
		}
		ImFontConfig cfg;
		cfg.SizePixels = 13.0f * a_scale;
		io.Fonts->AddFontDefault(&cfg);
	}

	void Style()
	{
		ImGui::StyleColorsDark();
		ImGuiStyle& s = ImGui::GetStyle();
		s.WindowRounding = 4;
		s.FrameRounding = 3;
		s.ChildRounding = 3;
		s.GrabRounding = 3;
		s.FramePadding = ImVec2(8, 4);
		s.ItemSpacing = ImVec2(8, 5);
		auto* c = s.Colors;
		c[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.11f, 0.12f, 1);
		c[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.13f, 0.14f, 1);
		c[ImGuiCol_Header] = ImVec4(0.28f, 0.36f, 0.48f, 0.85f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.32f, 0.42f, 0.56f, 0.9f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.48f, 0.64f, 1);
		c[ImGuiCol_Button] = ImVec4(0.24f, 0.32f, 0.44f, 1);
		c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.40f, 0.55f, 1);
		c[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.46f, 0.62f, 1);
	}
}

int RunApp(int argc, char** argv)
{
#ifdef _WIN32
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for the folder picker
#endif
	if (!glfwInit()) {
		std::fputs("failed to initialise GLFW\n", stderr);
		return 1;
	}
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	GLFWwindow* window = glfwCreateWindow(1600, 950, "ReSaver++", nullptr, nullptr);
	if (!window) {
		std::fputs("failed to create a window (OpenGL 3.0 required)\n", stderr);
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);
	glfwSetDropCallback(window, OnDrop);

	float xs = 1, ys = 1;
	glfwGetWindowContentScale(window, &xs, &ys);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	Style();
	ImGui::GetStyle().ScaleAllSizes(xs);
	LoadFonts(xs);
	LoadMonoFont(xs);
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	App app;
	g_app = &app;
	app.monoFont = g_mono;
	app.uploadTexture = [](const std::uint8_t* px, int w, int h) -> std::uint64_t {
		GLuint tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		return tex;
	};
	app.freeTexture = [](std::uint64_t t) {
		GLuint tex = static_cast<GLuint>(t);
		glDeleteTextures(1, &tex);
	};

	if (argc > 1) {
		app.openAny(platform::FromUtf8(argv[1]));
	}
	if (const char* t = std::getenv("RESAVERPP_TEST_CAT")) {
		app.testSelect(std::atoi(t), std::getenv("RESAVERPP_TEST_FILTER"), std::getenv("RESAVERPP_TEST_ANALYSIS") != nullptr);
		if (const char* rm = std::getenv("RESAVERPP_TEST_REMOVE")) {
			app.testRemove(rm);
		}
		if (const char* w = std::getenv("RESAVERPP_TEST_OPEN")) {
			app.testOpen(w);
		}
		if (std::getenv("RESAVERPP_TEST_COMPILE")) {
			app.testOpen("compile");
		}
		if (std::getenv("RESAVERPP_TEST_ROWVIEW")) {
			app.testOpen("rowview");
		}
	}

	// optional: RESAVERPP_SCREENSHOT=<file.ppm> renders a few frames, saves the window and exits (used for testing)
	const char* shotPath = std::getenv("RESAVERPP_SCREENSHOT");
	int         frame = 0;
	const double shotDelay = std::getenv("RESAVERPP_SHOT_DELAY") ? std::atof(std::getenv("RESAVERPP_SHOT_DELAY")) : 0.0;

	while (!app.wantsQuit()) {
		glfwPollEvents();
		if (glfwWindowShouldClose(window)) {
			glfwSetWindowShouldClose(window, GLFW_FALSE);
			app.requestQuit();
		}
		if (!g_dropped.empty()) {
			app.openAny(platform::FromUtf8(g_dropped.back()));
			g_dropped.clear();
		}
		if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
			ImGui_ImplGlfw_Sleep(50);
			continue;
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		app.draw();
		ImGui::Render();

		int w, h;
		glfwGetFramebufferSize(window, &w, &h);
		glViewport(0, 0, w, h);
		glClearColor(0.08f, 0.08f, 0.09f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		if (shotPath && ++frame >= 6 && glfwGetTime() >= shotDelay) {
			std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 3);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
			if (FILE* f = std::fopen(shotPath, "wb")) {
				std::fprintf(f, "P6\n%d %d\n255\n", w, h);
				for (int y = h - 1; y >= 0; --y) {
					std::fwrite(px.data() + static_cast<std::size_t>(y) * w * 3, 1, static_cast<std::size_t>(w) * 3, f);
				}
				std::fclose(f);
			}
			break;
		}
		glfwSwapBuffers(window);
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int    argc = 0;
	LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::vector<std::string> args;
	std::vector<char*>       argv;
	for (int i = 0; i < argc; ++i) {
		args.push_back(platform::ToUtf8(std::filesystem::path(wargv[i])));
	}
	LocalFree(wargv);
	for (auto& a : args) {
		argv.push_back(a.data());
	}
	return RunApp(static_cast<int>(argv.size()), argv.data());
}
#else
int main(int argc, char** argv)
{
	return RunApp(argc, argv);
}
#endif
