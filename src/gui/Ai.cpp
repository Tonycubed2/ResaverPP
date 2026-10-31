// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Ai.h"

#include "Net.h"
#include "core/Json.h"

#include <algorithm>
#include <chrono>

namespace ai
{
	using ess::json::Quote;
	using ess::json::Value;

	const std::vector<Preset>& Presets()
	{
		static const std::vector<Preset> v = {
			{ "Anthropic (Claude)", Api::kAnthropic, "https://api.anthropic.com", "claude-sonnet-5-5", true,
				"Key from console.anthropic.com. Paid per use." },
			{ "OpenAI (ChatGPT)", Api::kOpenAI, "https://api.openai.com/v1", "", true,
				"Key from platform.openai.com. Paid per use. Use List models to pick one." },
			{ "Google Gemini", Api::kOpenAI, "https://generativelanguage.googleapis.com/v1beta/openai", "", true,
				"Key from aistudio.google.com (has a free tier). Use List models to pick one." },
			{ "OpenRouter", Api::kOpenAI, "https://openrouter.ai/api/v1", "", true,
				"One key for many engines (openrouter.ai). Pick a model that supports tools." },
			{ "Ollama (on this PC)", Api::kOpenAI, "http://localhost:11434/v1", "", false,
				"Free, runs on your PC, nothing leaves it. Needs a model with tool support (e.g. qwen3, llama3.1); small models make "
				"more mistakes." },
			{ "LM Studio (on this PC)", Api::kOpenAI, "http://localhost:1234/v1", "", false,
				"Free, runs on your PC. Start its local server first; pick a model with tool support." },
			{ "Other (OpenAI-compatible)", Api::kOpenAI, "", "", true,
				"Any server that speaks the OpenAI chat completions API (KoboldCpp, a CHIM / Mantella setup's engine...)." },
		};
		return v;
	}

	namespace
	{
		std::string Trimmed(std::string s)
		{
			while (!s.empty() && (s.back() == '/' || s.back() == ' ')) {
				s.pop_back();
			}
			return s;
		}

		std::string Endpoint(const Settings& a_s, const char* a_what)  // a_what: "chat" or "models"
		{
			auto base = Trimmed(a_s.baseUrl);
			if (a_s.api == Api::kAnthropic) {
				if (base.size() < 3 || base.substr(base.size() - 3) != "/v1") {
					base += "/v1";
				}
				return base + (std::string(a_what) == "chat" ? "/messages" : "/models");
			}
			return base + (std::string(a_what) == "chat" ? "/chat/completions" : "/models");
		}

		net::Headers HeadersFor(const Settings& a_s)
		{
			net::Headers h = { { "Content-Type", "application/json" } };
			if (a_s.api == Api::kAnthropic) {
				h.push_back({ "x-api-key", a_s.apiKey });
				h.push_back({ "anthropic-version", "2023-06-01" });
			} else if (!a_s.apiKey.empty()) {
				h.push_back({ "Authorization", "Bearer " + a_s.apiKey });
			}
			return h;
		}

		std::string ErrorText(const net::Response& r)
		{
			if (r.status == 0) {
				return r.error.empty() ? "no answer from the AI engine" : r.error;
			}
			std::string msg;
			try {
				const auto v = ess::json::Parse(r.body);
				if (const auto* e = v.get("error")) {
					msg = e->type == Value::Type::kString ? e->s : e->str("message");
				}
				if (msg.empty()) {
					msg = v.str("message");
				}
			} catch (...) {
			}
			if (msg.empty()) {
				msg = r.body.substr(0, 400);
			}
			std::string hint;
			if (r.status == 401 || r.status == 403) {
				hint = " (the API key is wrong or missing)";
			} else if (r.status == 404) {
				hint = " (wrong address or model name)";
			} else if (r.status == 429) {
				hint = " (too many requests or no credit left)";
			}
			return "the AI engine answered HTTP " + std::to_string(r.status) + hint + ": " + msg;
		}

		std::string ToolsJson(const std::vector<Tool>& a_tools, Api a_api)
		{
			std::string o = "[";
			for (std::size_t i = 0; i < a_tools.size(); ++i) {
				const auto& t = a_tools[i];
				o += i ? "," : "";
				if (a_api == Api::kAnthropic) {
					o += "{\"name\":" + Quote(t.name) + ",\"description\":" + Quote(t.description) + ",\"input_schema\":" + t.schema;
					if (i + 1 == a_tools.size()) {
						o += ",\"cache_control\":{\"type\":\"ephemeral\"}";  // the tools and system text are the same every call
					}
					o += "}";
				} else {
					o += "{\"type\":\"function\",\"function\":{\"name\":" + Quote(t.name) + ",\"description\":" + Quote(t.description) +
					     ",\"parameters\":" + t.schema + "}}";
				}
			}
			return o + "]";
		}

		// args text -> a JSON object (Anthropic's tool_use input); bad JSON becomes {}
		std::string ArgsObject(const std::string& a_args)
		{
			try {
				const auto v = ess::json::Parse(a_args.empty() ? "{}" : a_args);
				if (v.type == Value::Type::kObject) {
					return ess::json::Write(v);
				}
			} catch (...) {
			}
			return "{}";
		}

		std::string OpenAIBody(const Settings& a_s, const std::string& a_system, const std::vector<Message>& a_msgs,
			const std::vector<Tool>& a_tools)
		{
			std::string m = "[{\"role\":\"system\",\"content\":" + Quote(a_system) + "}";
			for (const auto& msg : a_msgs) {
				switch (msg.role) {
				case Message::Role::kUser:
					m += ",{\"role\":\"user\",\"content\":" + Quote(msg.text) + "}";
					break;
				case Message::Role::kAssistant:
					m += ",{\"role\":\"assistant\",\"content\":" + (msg.text.empty() && !msg.calls.empty() ? std::string("null") : Quote(msg.text));
					if (!msg.calls.empty()) {
						m += ",\"tool_calls\":[";
						for (std::size_t i = 0; i < msg.calls.size(); ++i) {
							const auto& c = msg.calls[i];
							m += std::string(i ? "," : "") + "{\"id\":" + Quote(c.id) + ",\"type\":\"function\",\"function\":{\"name\":" +
							     Quote(c.name) + ",\"arguments\":" + Quote(c.args.empty() ? "{}" : c.args) + "}}";
						}
						m += "]";
					}
					m += "}";
					break;
				case Message::Role::kTool:
					m += ",{\"role\":\"tool\",\"tool_call_id\":" + Quote(msg.callId) + ",\"content\":" + Quote(msg.text) + "}";
					break;
				default: break;
				}
			}
			m += "]";
			std::string body = "{\"model\":" + Quote(a_s.model) + ",\"messages\":" + m;
			if (!a_tools.empty()) {
				body += ",\"tools\":" + ToolsJson(a_tools, Api::kOpenAI);
			}
			return body + "}";
		}

		std::string AnthropicBody(const Settings& a_s, const std::string& a_system, const std::vector<Message>& a_msgs,
			const std::vector<Tool>& a_tools)
		{
			// content blocks per role; the same role twice in a row is one message
			std::vector<std::pair<std::string, std::vector<std::string>>> turns;
			auto add = [&](const char* a_role, std::string a_block) {
				if (turns.empty() || turns.back().first != a_role) {
					turns.push_back({ a_role, {} });
				}
				turns.back().second.push_back(std::move(a_block));
			};
			for (const auto& msg : a_msgs) {
				switch (msg.role) {
				case Message::Role::kUser:
					add("user", "{\"type\":\"text\",\"text\":" + Quote(msg.text.empty() ? std::string("(empty)") : msg.text) + "}");
					break;
				case Message::Role::kAssistant:
					if (!msg.text.empty()) {
						add("assistant", "{\"type\":\"text\",\"text\":" + Quote(msg.text) + "}");
					}
					for (const auto& c : msg.calls) {
						add("assistant", "{\"type\":\"tool_use\",\"id\":" + Quote(c.id) + ",\"name\":" + Quote(c.name) +
						                     ",\"input\":" + ArgsObject(c.args) + "}");
					}
					if (msg.text.empty() && msg.calls.empty()) {
						add("assistant", "{\"type\":\"text\",\"text\":\"(no answer)\"}");
					}
					break;
				case Message::Role::kTool:
					add("user", "{\"type\":\"tool_result\",\"tool_use_id\":" + Quote(msg.callId) + ",\"content\":" +
					                Quote(msg.text.empty() ? std::string("(no output)") : msg.text) + (msg.error ? ",\"is_error\":true" : "") + "}");
					break;
				default: break;
				}
			}
			std::string m = "[";
			for (std::size_t i = 0; i < turns.size(); ++i) {
				m += std::string(i ? "," : "") + "{\"role\":\"" + turns[i].first + "\",\"content\":[";
				for (std::size_t k = 0; k < turns[i].second.size(); ++k) {
					m += (k ? "," : "") + turns[i].second[k];
				}
				m += "]}";
			}
			m += "]";
			std::string body = "{\"model\":" + Quote(a_s.model) + ",\"max_tokens\":" + std::to_string(a_s.maxTokens) +
			                   ",\"system\":[{\"type\":\"text\",\"text\":" + Quote(a_system) +
			                   ",\"cache_control\":{\"type\":\"ephemeral\"}}],\"messages\":" + m;
			if (!a_tools.empty()) {
				body += ",\"tools\":" + ToolsJson(a_tools, Api::kAnthropic);
			}
			return body + "}";
		}

		std::string ArgsText(const Value& a_v)
		{
			if (a_v.type == Value::Type::kString) {
				return a_v.s.empty() ? "{}" : a_v.s;
			}
			if (a_v.type == Value::Type::kObject) {
				return ess::json::Write(a_v);  // some local servers give an object, not text
			}
			return "{}";
		}

		// the engine's answer -> one assistant message; false with a_error when it cannot be read
		bool ParseAnswer(Api a_api, const std::string& a_body, Message& a_out, std::string& a_stop, std::string& a_error)
		{
			Value v;
			try {
				v = ess::json::Parse(a_body);
			} catch (const std::exception& e) {
				a_error = std::string("could not read the AI engine's answer: ") + e.what();
				return false;
			}
			a_out.role = Message::Role::kAssistant;
			static int counter = 0;
			if (a_api == Api::kAnthropic) {
				a_stop = v.str("stop_reason");
				const auto* content = v.get("content");
				if (!content || content->type != Value::Type::kArray) {
					a_error = "the AI engine's answer has no content";
					return false;
				}
				for (const auto& b : content->a) {
					const auto type = b.str("type");
					if (type == "text") {
						a_out.text += (a_out.text.empty() ? "" : "\n") + b.str("text");
					} else if (type == "tool_use") {
						ToolCall c;
						c.id = b.str("id");
						c.name = b.str("name");
						if (const auto* in = b.get("input")) {
							c.args = ArgsText(*in);
						}
						a_out.calls.push_back(std::move(c));
					}
				}
				return true;
			}
			const auto* choices = v.get("choices");
			if (!choices || choices->type != Value::Type::kArray || choices->a.empty()) {
				a_error = "the AI engine's answer has no choices";
				if (const auto* e = v.get("error")) {
					a_error += ": " + (e->type == Value::Type::kString ? e->s : e->str("message"));
				}
				return false;
			}
			const auto& ch = choices->a.front();
			a_stop = ch.str("finish_reason");
			const auto* msg = ch.get("message");
			if (!msg) {
				a_error = "the AI engine's answer has no message";
				return false;
			}
			a_out.text = msg->str("content");
			if (const auto* calls = msg->get("tool_calls"); calls && calls->type == Value::Type::kArray) {
				for (const auto& c : calls->a) {
					ToolCall t;
					t.id = c.str("id");
					if (t.id.empty()) {
						t.id = "call_" + std::to_string(++counter);
					}
					if (const auto* fn = c.get("function")) {
						t.name = fn->str("name");
						if (const auto* args = fn->get("arguments")) {
							t.args = ArgsText(*args);
						}
					}
					a_out.calls.push_back(std::move(t));
				}
			}
			return true;
		}
	}

	Chat::~Chat()
	{
		stop();
		if (_thread.joinable()) {
			_thread.join();
		}
	}

	void Chat::send(const std::string& a_text, std::function<std::string()> a_system)
	{
		if (_busy) {
			return;
		}
		if (_thread.joinable()) {
			_thread.join();
		}
		{
			std::lock_guard l(_m);
			Message m;
			m.role = Message::Role::kUser;
			m.text = a_text;
			_messages.push_back(std::move(m));
			_status = "Thinking...";
		}
		++_revision;
		_stop = false;
		_busy = true;
		_thread = std::thread([this, sys = a_system ? a_system() : std::string()] { run(sys); });
	}

	void Chat::stop()
	{
		_stop = true;
		std::lock_guard l(_m);
		if (_approval.pending) {
			_approval.pending = false;
			_approvalAnswer.set_value(0);
		}
		for (auto& [fn, p] : _uiJobs) {
			p->set_value("stopped by the user");
		}
		_uiJobs.clear();
	}

	void Chat::clear()
	{
		stop();
		if (_thread.joinable()) {
			_thread.join();
		}
		std::lock_guard l(_m);
		_messages.clear();
		_status.clear();
		_allowAll = false;
		++_revision;
	}

	std::string Chat::status() const
	{
		std::lock_guard l(_m);
		return _status;
	}

	void Chat::read(const std::function<void(const std::vector<Message>&)>& a_fn) const
	{
		std::lock_guard l(_m);
		a_fn(_messages);
	}

	void Chat::note(const std::string& a_text, bool a_error)
	{
		std::lock_guard l(_m);
		Message m;
		m.role = Message::Role::kNote;
		m.text = a_text;
		m.error = a_error;
		_messages.push_back(std::move(m));
		++_revision;
	}

	bool Chat::approve(const std::string& a_what, const std::string& a_command)
	{
		std::future<int> f;
		{
			std::lock_guard l(_m);
			if (_stop) {
				return false;
			}
			if (_allowAll || !_settings.askBeforeChanges) {
				return true;
			}
			_approvalAnswer = std::promise<int>();
			f = _approvalAnswer.get_future();
			_approval = { true, a_what, a_command };
			_status = "Waiting for your OK...";
		}
		++_revision;
		const int a = f.get();
		std::lock_guard l(_m);
		_status = "Working...";
		if (a == 2) {
			_allowAll = true;
		}
		return a > 0 && !_stop;
	}

	Approval Chat::approval() const
	{
		std::lock_guard l(_m);
		return _approval;
	}

	void Chat::answer(int a_choice)
	{
		std::lock_guard l(_m);
		if (_approval.pending) {
			_approval.pending = false;
			_approvalAnswer.set_value(a_choice);
		}
		++_revision;
	}

	std::string Chat::runOnUi(std::function<std::string()> a_fn)
	{
		auto p = std::make_shared<std::promise<std::string>>();
		auto f = p->get_future();
		{
			std::lock_guard l(_m);
			if (_stop) {
				return "stopped by the user";
			}
			_uiJobs.push_back({ std::move(a_fn), p });
		}
		return f.get();
	}

	void Chat::pollUi()
	{
		for (;;) {
			std::pair<std::function<std::string()>, std::shared_ptr<std::promise<std::string>>> job;
			{
				std::lock_guard l(_m);
				if (_uiJobs.empty()) {
					break;
				}
				job = std::move(_uiJobs.front());
				_uiJobs.pop_front();
			}
			std::string r;
			try {
				r = job.first();
			} catch (const std::exception& e) {
				r = std::string("error: ") + e.what();
			}
			job.second->set_value(r);
		}
		if (!_busy && _thread.joinable()) {
			_thread.join();
		}
	}

	void Chat::run(std::string a_system)
	{
		std::string error;
		int         steps = 0;
		while (!_stop) {
			if (++steps > 40) {
				note("Stopped after 40 steps without a final answer. Say \"go on\" to let it continue.", true);
				break;
			}
			if (!step(a_system, error)) {
				break;
			}
		}
		if (!error.empty()) {
			note(error, true);
		} else if (_stop) {
			note("Stopped.", false);
		}
		{
			std::lock_guard l(_m);
			// every tool call needs an answer before the next message
			std::vector<std::pair<std::string, std::string>> missing;
			for (std::size_t i = 0; i < _messages.size(); ++i) {
				if (_messages[i].role != Message::Role::kAssistant) {
					continue;
				}
				for (const auto& c : _messages[i].calls) {
					bool answered = false;
					for (std::size_t k = i + 1; k < _messages.size() && !answered; ++k) {
						answered = _messages[k].role == Message::Role::kTool && _messages[k].callId == c.id;
					}
					if (!answered) {
						Message t;
						t.role = Message::Role::kTool;
						t.callId = c.id;
						t.toolName = c.name;
						t.text = "not run: stopped by the user";
						t.error = true;
						_messages.push_back(std::move(t));
					}
				}
			}
			_status.clear();
		}
		++_revision;
		_busy = false;
	}

	bool Chat::step(const std::string& a_system, std::string& a_error)
	{
		std::string body;
		{
			std::lock_guard l(_m);
			body = _settings.api == Api::kAnthropic ? AnthropicBody(_settings, a_system, _messages, _tools)
			                                         : OpenAIBody(_settings, a_system, _messages, _tools);
			_status = "Thinking...";
		}
		++_revision;
		net::Response r;
		for (int attempt = 0;; ++attempt) {
			r = net::Request("POST", Endpoint(_settings, "chat"), HeadersFor(_settings), body, _stop, _settings.timeoutSeconds);
			const bool busyServer = r.status == 429 || r.status == 529 || r.status == 503 || r.status == 502;
			if (!busyServer || attempt >= 3 || _stop) {
				break;
			}
			{
				std::lock_guard l(_m);
				_status = "The AI engine is busy, trying again...";
			}
			for (int i = 0; i < (attempt + 1) * 30 && !_stop; ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
		if (_stop) {
			return false;
		}
		if (r.status != 200) {
			a_error = ErrorText(r);
			return false;
		}
		Message     answer;
		std::string stopReason;
		if (!ParseAnswer(_settings.api, r.body, answer, stopReason, a_error)) {
			return false;
		}
		const auto calls = answer.calls;
		{
			std::lock_guard l(_m);
			_messages.push_back(answer);
		}
		++_revision;
		if (calls.empty()) {
			if (stopReason == "length" || stopReason == "max_tokens") {
				note("The answer was cut off (too long). Say \"go on\" to get the rest.", false);
			}
			return false;
		}
		for (const auto& c : calls) {
			if (_stop) {
				return false;
			}
			{
				std::lock_guard l(_m);
				_status = "Running " + c.name + "...";
			}
			++_revision;
			Message t;
			t.role = Message::Role::kTool;
			t.callId = c.id;
			t.toolName = c.name;
			try {
				t.text = _exec ? _exec(c, t.shown, t.error) : "no tools";
			} catch (const std::exception& e) {
				t.text = std::string("error: ") + e.what();
				t.error = true;
			}
			std::lock_guard l(_m);
			_messages.push_back(std::move(t));
			++_revision;
		}
		return true;
	}

	std::vector<std::string> Chat::ListModels(const Settings& a_settings, std::string& a_error)
	{
		std::vector<std::string> out;
		std::atomic<bool>        never{ false };
		auto h = HeadersFor(a_settings);
		h.erase(h.begin());  // no body
		const auto r = net::Request("GET", Endpoint(a_settings, "models"), h, "", never, 30);
		if (r.status != 200) {
			a_error = ErrorText(r);
			return out;
		}
		try {
			const auto v = ess::json::Parse(r.body);
			const Value* list = v.get("data");
			if (!list) {
				list = v.get("models");
			}
			if (list && list->type == Value::Type::kArray) {
				for (const auto& m : list->a) {
					auto id = m.str("id");
					if (id.empty()) {
						id = m.str("name");
					}
					if (id.rfind("models/", 0) == 0) {
						id = id.substr(7);  // Gemini
					}
					if (!id.empty()) {
						out.push_back(id);
					}
				}
			}
		} catch (const std::exception& e) {
			a_error = e.what();
		}
		std::sort(out.begin(), out.end());
		if (out.empty() && a_error.empty()) {
			a_error = "the engine listed no models";
		}
		return out;
	}
}
