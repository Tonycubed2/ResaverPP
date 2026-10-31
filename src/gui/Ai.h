// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The AI chat: a conversation with an AI engine (any OpenAI-compatible server: OpenAI, Google Gemini,
// OpenRouter, Ollama, LM Studio...; or Anthropic's Claude) that can use ReSaver Plus through tools. The
// engine is called on a background thread; the tools it asks for run through a callback the app gives
// (AppAi.cpp). Changes can be set to need the user's OK first (askBeforeChanges).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ai
{
	enum class Api
	{
		kOpenAI,    // POST <base>/chat/completions
		kAnthropic  // POST <base>/v1/messages
	};

	struct Preset
	{
		const char* name;
		Api         api;
		const char* baseUrl;
		const char* model;     // a suggestion ("" = pick one with List models)
		bool        needsKey;
		const char* note;
	};
	const std::vector<Preset>& Presets();
	// the API kind a preset uses; "Other" keeps the user's choice
	void FixApi(struct Settings& a_settings);
	// every request and answer of the chat is written here ("ReSaverPP AI.log" next to the program); the key is never written
	void SetLogFile(const std::filesystem::path& a_path);
	void Log(const std::string& a_line);

	struct Settings
	{
		int         preset = 0;  // index in Presets()
		Api         api = Api::kAnthropic;  // follows the preset (only "Other" lets the user choose)
		std::string baseUrl, apiKey, model;
		int         maxTokens = 8192;
		int         timeoutSeconds = 300;
		bool        askBeforeChanges = true;
		bool        configured() const { return !baseUrl.empty() && !model.empty(); }
	};

	struct ToolCall
	{
		std::string id, name, args;  // args: JSON text
		std::string extra;           // OpenAI kind: the call's other fields as the engine sent them (JSON object text), sent back
		                             // as they were (Gemini's thought signature is in extra_content and must come back)
	};
	struct Message
	{
		enum class Role
		{
			kUser,
			kAssistant,
			kTool,
			kNote  // shown in the chat only (errors, "stopped"); never sent
		} role = Role::kUser;
		std::string           text;
		std::vector<ToolCall> calls;     // assistant: tools it asked for
		std::string           extra;     // assistant, OpenAI kind: the message's extra_content, sent back as it was
		std::string           callId;    // tool: which call this answers
		std::string           toolName;  // tool: for the chat
		std::string           shown;     // tool: what the chat shows as the action ("resaverpp-cli Save89.ess --quests")
		bool                  error = false;
	};
	struct Tool
	{
		std::string name, description, schema;  // schema: JSON Schema text of the arguments
	};

	struct Approval
	{
		bool        pending = false;
		std::string what;     // what the AI wants to do, for the user
		std::string command;  // the exact command
	};

	class Chat
	{
	public:
		// runs a tool on the background thread; the app may use RunOnUi / Approve from inside it.
		// a_shown: set to a short line for the chat ("ran resaverpp-cli ..."); return the tool's result text
		using Executor = std::function<std::string(const ToolCall& a_call, std::string& a_shown, bool& a_error)>;

		~Chat();
		void        setTools(std::vector<Tool> a_tools) { _tools = std::move(a_tools); }
		void        setExecutor(Executor a_exec) { _exec = std::move(a_exec); }
		Settings&   settings() { return _settings; }
		// a_system: built when the message is sent (the lesson book plus the app's state)
		void        send(const std::string& a_text, std::function<std::string()> a_system);
		void        stop();
		void        clear();
		bool        busy() const { return _busy; }
		std::string status() const;
		// the conversation, locked while a_fn reads it (the UI draws from it)
		void        read(const std::function<void(const std::vector<Message>&)>& a_fn) const;
		std::size_t revision() const { return _revision; }

		// from the executor (background thread)
		bool        approve(const std::string& a_what, const std::string& a_command);  // false: denied or stopped
		std::string runOnUi(std::function<std::string()> a_fn);                          // waits until the UI ran it
		bool        stopping() const { return _stop; }
		const std::atomic<bool>& stopFlag() const { return _stop; }

		// from the UI thread, every frame
		void     pollUi();
		Approval approval() const;
		void     answer(int a_choice);  // 0 deny, 1 allow, 2 allow every change in this chat

		// lists the engine's models (GET <base>/models); blocking, call from a thread
		static std::vector<std::string> ListModels(const Settings& a_settings, std::string& a_error);

	private:
		void run(std::string a_system);
		bool step(const std::string& a_system, std::string& a_error);  // one call to the engine; false: done or failed
		void note(const std::string& a_text, bool a_error);

		Settings              _settings;
		std::vector<Tool>     _tools;
		Executor              _exec;
		mutable std::mutex    _m;
		std::vector<Message>  _messages;
		std::atomic<std::size_t> _revision{ 0 };
		std::thread           _thread;
		std::atomic<bool>     _busy{ false }, _stop{ false };
		std::string           _status;
		bool                  _allowAll = false;
		// approval asked from the background thread
		Approval                     _approval;
		std::promise<int>            _approvalAnswer;
		// work for the UI thread
		std::deque<std::pair<std::function<std::string()>, std::shared_ptr<std::promise<std::string>>>> _uiJobs;
	};
}
