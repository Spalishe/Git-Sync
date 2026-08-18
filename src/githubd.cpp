#include "githubd.hpp"
#include "assets.hpp"
#include "render.hpp"

#include "miniaudio.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static ma_engine* AudioEngine = NULL;
static bool HasAudio;

// Helper function to replace all occurrences of a substring
void replaceAll(std::string& str, const std::string& from, const std::string& to)
{
	if(from.empty()) return;
	size_t start_pos = 0;
	while((start_pos = str.find(from, start_pos)) != std::string::npos)
	{
		str.replace(start_pos, from.length(), to);
		start_pos += to.length();
	}
}

// Helper function to split a string by a delimiter (equivalent to string.Explode)
std::vector<std::string> explode(const std::string& delimiter, const std::string& str)
{
	std::vector<std::string> arr;
	if(str.empty()) return arr;

	size_t start = 0;
	size_t end	 = str.find(delimiter);
	while(end != std::string::npos)
	{
		arr.push_back(str.substr(start, end - start));
		start = end + delimiter.length();
		end	  = str.find(delimiter, start);
	}
	arr.push_back(str.substr(start));
	return arr;
}

Color hexToColor(std::string hexStr)
{
	// Remove the leading '#' if it exists
	if(!hexStr.empty() && hexStr[0] == '#')
	{
		hexStr.erase(0, 1);
	}

	// Convert hex string to a single unsigned long integer
	unsigned long value = std::stoul(hexStr, nullptr, 16);

	Color color = Color((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);

	return color;
}

namespace
{
	static Color COLOR_REPOSITORY	  = Color(0, 175, 255);
	static Color COLOR_COMMIT_MESSAGE = Color(120, 120, 120);
	static Color COLOR_COMMIT_AUTHOR  = Color(220, 220, 220);
	static Color COLOR_WHITE		  = Color(255, 255, 255);
	static Color COLOR_GREEN		  = Color(0, 200, 0);
	static Color COLOR_RED			  = Color(200, 0, 0);
	static Color COMMIT_COLORS[]	  = {
		Color(220, 255, 220),
		Color(255, 220, 220),
		Color(255, 225, 180)
	};
	static ImageID COMMIT_ICONS[]	   = { IMG_BRICK_ADD, IMG_BRICK_DELETE, IMG_BRICK_EDIT };
	static ImageID COMMIT_FILE_ICONS[] = { IMG_SCRIPT_ADD, IMG_SCRIPT_DELETE, IMG_SCRIPT_EDIT };

	struct GitHubResponse
	{
		long code = 0;
		std::string body;
		std::string etag;
		std::string last_modified;
		std::string poll_interval;
	};

	enum FileChangeType
	{
		FILE_ADD	= 1,
		FILE_DELETE = 2,
		FILE_MODIFY = 3
	};

	struct LabelInfo
	{
		std::string name;
		std::string color;
	};

	struct LabelEventKey
	{
		std::string repository;
		int number;
		bool pull_request;

		bool operator==(const LabelEventKey& other) const
		{
			return repository == other.repository && number == other.number && pull_request == other.pull_request;
		}
	};

	struct LabelEventKeyHash
	{
		size_t operator()(const LabelEventKey& key) const
		{
			size_t h1 = std::hash<std::string>{}(key.repository);

			size_t h2 = std::hash<int>{}(key.number);

			size_t h3 = std::hash<bool>{}(key.pull_request);

			return h1 ^ (h2 << 1) ^ (h3 << 2);
		}
	};

	struct LabelBatch
	{
		std::string repository;
		std::string actor;
		std::string title;
		int number		  = 0;
		bool pull_request = false;

		std::vector<LabelInfo> labels;
	};

	struct CommitFile
	{
		std::string filename;
		std::string status;
		int additions = 0;
		int deletions = 0;
		int changes	  = 0;
	};

	FileChangeType get_file_type(const CommitFile& file)
	{
		if(file.status == "added")
			return FILE_ADD;

		if(file.status == "removed")
			return FILE_DELETE;

		return FILE_MODIFY;
	}

	struct CommitInfo
	{
		std::string sha;
		std::string title;
		std::string author;
		std::vector<CommitFile> files;
	};
	enum CommitType
	{
		COMMIT_ADD	  = 1,
		COMMIT_DELETE = 2,
		COMMIT_MIXED  = 3
	};
	CommitType get_commit_type(const CommitInfo& commit)
	{
		bool has_add	= false;
		bool has_delete = false;
		bool has_modify = false;

		for(const CommitFile& file : commit.files)
		{
			if(file.status == "added")
				has_add = true;
			else if(file.status == "removed")
				has_delete = true;
			else
				has_modify = true;
		}

		if(has_add && !has_delete && !has_modify)
			return COMMIT_ADD;

		if(has_delete && !has_add && !has_modify)
			return COMMIT_DELETE;

		return COMMIT_MIXED;
	}
	struct PushInfo
	{
		std::string actor;
		std::string branch;
		std::vector<CommitInfo> commits;
	};

	struct RepoState
	{
		std::string latest_event_id;
		std::string latest_issue_event_id;
		std::string latest_commit_sha;
	};

	struct NotificationState
	{
		bool initialized = false;
		std::unordered_map<std::string, std::string> seen;
		std::unordered_set<std::string> processed_comments;
	};
	struct NotificationIssueState
	{
		bool initialized  = false;
		bool pull_request = false;

		std::string state;
		bool merged = false;
	};
	std::unordered_map<std::string, NotificationIssueState>
		notification_issue_states;

	std::unordered_map<std::string, RepoState> repo_states;
	NotificationState notification_state;

	std::vector<std::string> subscriptions;
	std::mutex state_mutex;

	std::mt19937 random_engine{ std::random_device{}() };

	int random_seconds(int min, int max)
	{
		std::uniform_int_distribution<int> distribution(min, max);
		return distribution(random_engine);
	}

	size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
	{
		size_t totalSize = size * nmemb;
		std::string* s	 = static_cast<std::string*>(userp);

		s->append(static_cast<char*>(contents), totalSize);

		return totalSize;
	}

	size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
	{
		size_t totalSize = size * nitems;

		std::string header(buffer, totalSize);
		std::string* output = static_cast<std::string*>(userdata);

		auto colon = header.find(':');
		if(colon == std::string::npos)
			return totalSize;

		std::string key	  = header.substr(0, colon);
		std::string value = header.substr(colon + 1);

		while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
			value.erase(value.begin());

		while(!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
		{
			value.pop_back();
		}

		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});

		if(key == "etag")
			*output = "etag:" + value;
		else if(key == "last-modified")
			*output = "last-modified:" + value;
		else if(key == "x-poll-interval")
			*output = "poll-interval:" + value;

		return totalSize;
	}

	bool github_get(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& url,
		GitHubResponse& response,
		const std::string& extra_header = "")
	{
		response = {};

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

		std::string body;
		std::string response_headers;

		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

		struct curl_slist* request_headers = headers;

		if(!extra_header.empty())
			request_headers = curl_slist_append(request_headers, extra_header.c_str());

		if(!extra_header.empty())
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);

		CURLcode res = curl_easy_perform(curl);

		if(!extra_header.empty())
			curl_slist_free_all(request_headers);

		if(res != CURLE_OK)
		{
			std::cerr << "curl_easy_perform() failed: "
					  << curl_easy_strerror(res) << '\n';
			return false;
		}

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.code);

		response.body = std::move(body);

		size_t pos = 0;
		while(pos < response_headers.size())
		{
			size_t end = response_headers.find('\n', pos);

			if(end == std::string::npos)
				end = response_headers.size();

			std::string line = response_headers.substr(pos, end - pos);

			if(line.rfind("etag:", 0) == 0)
				response.etag = line.substr(5);
			else if(line.rfind("last-modified:", 0) == 0)
				response.last_modified = line.substr(14);
			else if(line.rfind("poll-interval:", 0) == 0)
				response.poll_interval = line.substr(15);

			pos = end + 1;
		}

		return true;
	}

	/*
	 * ---- Output handlers ----
	 *
	 * These are the functions where your Wayland renderer can later be called.
	 */

	void on_commits(
		const std::string& repository,
		const PushInfo& push)
	{

		std::cout << "\n=== PUSH ===\n";

		std::cout
			<< repository
			<< "\nActor: "
			<< push.actor
			<< "\nBranch: "
			<< push.branch
			<< "\nCommits: "
			<< push.commits.size()
			<< "\n";
		for(const CommitInfo& commit : push.commits)
		{
			std::cout
				<< "\n"
				<< commit.title
				<< "\n";

			for(const CommitFile& file : commit.files)
			{
				std::cout
					<< "  "
					<< file.status
					<< " +"
					<< file.additions
					<< " -"
					<< file.deletions
					<< " "
					<< file.filename
					<< "\n";
			}
		}

		// Audio
		ma_engine_play_sound(AudioEngine, "sfx", NULL);

		// Now call gui.
		int commits_c = push.commits.size();
		add_message(0, 16, IMG_INFORMATION, false,
					std::to_string(commits_c),
					std::string(" commit") + (commits_c > 1 ? "s" : ""),
					std::string(" appeared on "),
					COLOR_REPOSITORY, repository, COLOR_WHITE, std::string(" by "), COLOR_COMMIT_AUTHOR, push.actor);

		for(int i = 1; i <= push.commits.size(); i++)
		{
			auto& commit  = push.commits[i - 1];
			int commit_id = (int)get_commit_type(commit);
			Color color	  = COMMIT_COLORS[commit_id - 1];
			double time	  = 10 + (0.15f * i);

			std::string prcs_str = commit.title;
			replaceAll(prcs_str, "\t", " ");
			std::vector<std::string> commit_lines = explode("\n", prcs_str);

			add_message(32, time, COMMIT_ICONS[commit_id - 1], false, COLOR_COMMIT_MESSAGE, commit_lines[0]);
			for(int i1 = 1; i1 < commit_lines.size(); i++)
			{
				add_message(32, time, COMMIT_ICONS[commit_id - 1], false, COLOR_COMMIT_MESSAGE, commit_lines[i]);
			}

			int file_count = commit.files.size();
			for(int k = 0; k < file_count; k++)
			{
				auto& file	= commit.files[k];
				int file_id = (int)get_file_type(file);

				add_message(64, time + (0.15 * (k + 1)), COMMIT_FILE_ICONS[file_id - 1], false, COMMIT_COLORS[file_id - 1], file.filename, std::string("  "),
							(file.additions > 0 ? COLOR_GREEN : COLOR_WHITE), (file.additions > 0 ? std::string("+") + std::to_string(file.additions) : std::string("")),
							std::string(" "),
							(file.deletions > 0 ? COLOR_RED : COLOR_WHITE), (file.deletions > 0 ? std::string("-") + std::to_string(file.deletions) : std::string("")));
			}
		}
	}

	void on_issue_or_pr_created(
		const std::string& repository,
		bool pull_request,
		int number,
		const std::string& title,
		const std::string& body,
		const std::string& author)
	{
		std::cout
			<< "\n=== "
			<< (pull_request ? "PR" : "ISSUE")
			<< " CREATED ===\n"
			<< repository
			<< "#"
			<< number
			<< "\nTitle: "
			<< title
			<< "\nAuthor: "
			<< author
			<< "\n";

		// Audio
		ma_engine_play_sound(AudioEngine, "sfx", NULL);

		// Now call gui.
		if(pull_request)
			add_message(0, 16, IMG_INFORMATION, false,
						COLOR_COMMIT_AUTHOR, author, COLOR_WHITE, std::string(" opened a pull request on "),
						COLOR_REPOSITORY, (repository + "#" + std::to_string(number)));
		else
			add_message(0, 16, IMG_INFORMATION, false,
						COLOR_COMMIT_AUTHOR, author, COLOR_WHITE, std::string(" created an issue on "),
						COLOR_REPOSITORY, (repository + "#" + std::to_string(number)));

		double time = 10.15f;
		add_message(32, time, IMG_NONE, false, COLOR_COMMIT_MESSAGE, title);
		if(body.empty())
		{
			add_message(64, time + (0.15f), IMG_NONE, false, COLOR_COMMIT_MESSAGE, "* empty *");
		}
		else
		{
			std::string prcs_str = body;
			prcs_str			 = prcs_str.substr(0, 100); // 100 is max symbols
			if(body.length() > 100) prcs_str = prcs_str + "...";
			replaceAll(prcs_str, "\t", " ");
			std::vector<std::string> lines = explode("\n", prcs_str);
			for(int i = 0; i < lines.size(); i++)
			{
				add_message(64, time + (0.15f * (i + 1)), IMG_COMMENT, false, COLOR_WHITE, lines[i]);
			}
		}
	}

	void on_issue_comment(
		const std::string& repository,
		int issue_number,
		const std::string& title,
		const std::string& username,
		const std::string& color,
		const std::string& message)
	{
		std::cout
			<< "\n=== ISSUE COMMENT ===\n"
			<< repository
			<< "#"
			<< issue_number
			<< '\n'
			<< "User: "
			<< username
			<< '\n'
			<< "Color: "
			<< (color.empty() ? "none" : color)
			<< '\n'
			<< message
			<< '\n';

		// Audio
		ma_engine_play_sound(AudioEngine, "sfx", NULL);

		// Now call gui.
		add_message(0, 16, IMG_INFORMATION, false,
					COLOR_COMMIT_AUTHOR, username, COLOR_WHITE, std::string(" commented an issue on "),
					COLOR_REPOSITORY, (repository + "#" + std::to_string(issue_number)));

		double time = 10.15f;
		add_message(32, time, IMG_NONE, false, COLOR_COMMIT_MESSAGE, title);
		std::string prcs_str = message;
		prcs_str			 = prcs_str.substr(0, 100); // 100 is max symbols
		if(message.length() > 100) prcs_str = prcs_str + "...";
		replaceAll(prcs_str, "\t", " ");
		std::vector<std::string> lines = explode("\n", prcs_str);
		for(int i = 0; i < lines.size(); i++)
		{
			add_message(64, time + (0.15f * (i + 1)), IMG_COMMENT, false, COLOR_WHITE, lines[i]);
		}
	}

	void on_pull_request_comment(
		const std::string& repository,
		int pull_number,
		const std::string& title,
		const std::string& username,
		const std::string& color,
		const std::string& message)
	{
		std::cout
			<< "\n=== PR COMMENT ===\n"
			<< repository
			<< "#"
			<< pull_number
			<< '\n'
			<< "User: "
			<< username
			<< '\n'
			<< "Color: "
			<< (color.empty() ? "none" : color)
			<< '\n'
			<< message
			<< '\n';

		// Audio
		ma_engine_play_sound(AudioEngine, "sfx", NULL);

		// Now call gui.
		add_message(0, 16, IMG_INFORMATION, false,
					COLOR_COMMIT_AUTHOR, username, COLOR_WHITE, std::string(" commented an pull request on "),
					COLOR_REPOSITORY, (repository + "#" + std::to_string(pull_number)));

		double time = 10.15f;
		add_message(32, time, IMG_NONE, false, COLOR_COMMIT_MESSAGE, title);
		std::string prcs_str = message;
		prcs_str			 = prcs_str.substr(0, 100); // 100 is max symbols
		if(message.length() > 100) prcs_str = prcs_str + "...";
		replaceAll(prcs_str, "\t", " ");
		std::vector<std::string> lines = explode("\n", prcs_str);
		for(int i = 0; i < lines.size(); i++)
		{
			add_message(64, time + (0.15f * (i + 1)), IMG_COMMENT, false, COLOR_WHITE, lines[i]);
		}
	}

	void on_issue_or_pr_state_change(
		const std::string& repository,
		bool pull_request,
		int number,
		const std::string& author,
		const std::string& action,
		const std::string& extra)
	{
		std::cout
			<< "\n=== "
			<< (pull_request ? "PR" : "ISSUE")
			<< " STATE ===\n"
			<< repository
			<< "#"
			<< number
			<< "\nAuthor: "
			<< author
			<< "\nAction: "
			<< action;

		if(!extra.empty())
			std::cout
				<< "\nExtra: "
				<< extra;

		std::cout << '\n';

		ma_engine_play_sound(AudioEngine, "sfx", NULL);

		static const std::unordered_map<std::string, std::string> st_list{
			{ "reopened", " re-opened" },
			{ "closed",	" closed"	  },
			{ "merged",	" merged"	  }
		};

		const std::string text = pull_request
									 ? " a pull request on "
									 : " an issue on ";

		std::string ext = std::string(!extra.empty() ? ("(as " + extra + ")") : "");

		add_message(
			0, 16, IMG_INFORMATION, false,
			COLOR_COMMIT_AUTHOR, author,
			COLOR_WHITE, st_list.at(action), ext, text, COLOR_REPOSITORY,
			repository + "#" + std::to_string(number));
	}

	void on_labels_changed(
		const std::string& repository,
		const std::string& actor,
		const std::string& title,
		int number,
		bool pull_request,
		const std::vector<LabelInfo>& labels)
	{
		std::cout
			<< "\n=== "
			<< (pull_request ? "PR" : "ISSUE")
			<< " LABELS ===\n"
			<< repository
			<< "#"
			<< number
			<< '\n'
			<< "Actor: "
			<< actor
			<< '\n';

		for(const auto& label : labels)
		{
			std::cout
				<< "  "
				<< label.name
				<< " #"
				<< label.color
				<< '\n';
		}

		ma_engine_play_sound(AudioEngine, "sfx", NULL);

		const std::string text = pull_request
									 ? " changed labels on a pull request "
									 : " changed labels on an issue ";

		add_message(
			0, 16, IMG_INFORMATION, false,
			COLOR_COMMIT_AUTHOR, actor,
			COLOR_WHITE, text,
			COLOR_REPOSITORY,
			repository + "#" + std::to_string(number));

		double time = 10.15;

		add_message(
			32, time, IMG_NONE, false,
			COLOR_COMMIT_MESSAGE, title);

		int i = 0;

		for(const auto& label : labels)
		{
			++i;

			const Color color = hexToColor(label.color);

			add_message(
				64,
				time + (0.15 * i),
				IMG_NONE,
				false,
				color,
				label.name);
		}
	}

	/*
	 * ---- /user/subscriptions ----
	 */

	bool update_subscriptions(CURL* curl, struct curl_slist* headers)
	{
		std::vector<std::string> new_subscriptions;

		for(int page = 1;; ++page)
		{
			const std::string url = "https://api.github.com/user/subscriptions"
									"?per_page=100&page="
									+ std::to_string(page);

			GitHubResponse response;

			if(!github_get(curl, headers, url, response))
				return false;

			if(response.code != 200)
			{
				std::cerr
					<< "Subscriptions API error: "
					<< response.code
					<< "\n"
					<< response.body
					<< "\n";

				return false;
			}

			json parsed_json;

			try
			{
				parsed_json = json::parse(response.body);
			}
			catch(const json::exception& e)
			{
				std::cerr
					<< "Subscriptions JSON parse error: "
					<< e.what()
					<< '\n';

				return false;
			}

			if(!parsed_json.is_array())
				return false;

			for(const auto& repository : parsed_json)
			{
				if(!repository.contains("full_name"))
					continue;

				new_subscriptions.push_back(
					repository["full_name"].get<std::string>());
			}

			if(parsed_json.size() < 100)
				break;
		}

		{
			std::lock_guard<std::mutex> lock(state_mutex);

			for(const std::string& repository : new_subscriptions)
			{
				if(!repo_states.contains(repository))
					repo_states.emplace(repository, RepoState{});
			}

			for(auto it = repo_states.begin(); it != repo_states.end();)
			{
				if(std::find(
					   new_subscriptions.begin(),
					   new_subscriptions.end(),
					   it->first)
				   == new_subscriptions.end())
				{
					it = repo_states.erase(it);
				}
				else
				{
					++it;
				}
			}

			subscriptions = std::move(new_subscriptions);
		}

		std::cout
			<< "Updated subscriptions: "
			<< subscriptions.size()
			<< '\n';

		return true;
	}

	/*
	 * ---- Commit polling ----
	 */

	bool get_commit(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository,
		const std::string& sha,
		CommitInfo& result)
	{
		GitHubResponse response;

		const std::string url = "https://api.github.com/repos/" + repository + "/commits/" + sha;

		if(!github_get(curl, headers, url, response))
			return false;

		if(response.code != 200)
		{
			std::cerr
				<< "Commit API error: "
				<< response.code
				<< " "
				<< repository
				<< " "
				<< sha
				<< '\n';

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(...)
		{
			return false;
		}

		result.sha = parsed_json.value("sha", sha);

		if(parsed_json.contains("commit"))
		{
			const auto& commit = parsed_json["commit"];

			std::string message = commit.value("message", "");

			size_t newline = message.find('\n');

			if(newline != std::string::npos)
				message.resize(newline);

			result.title = std::move(message);

			if(commit.contains("author") && commit["author"].is_object())
			{
				result.author = commit["author"].value("name", "");
			}
		}

		if(parsed_json.contains("files") && parsed_json["files"].is_array())
		{
			for(const auto& file : parsed_json["files"])
			{
				CommitFile commit_file;

				commit_file.filename = file.value("filename", "");

				commit_file.status = file.value("status", "");

				commit_file.additions = file.value("additions", 0);

				commit_file.deletions = file.value("deletions", 0);

				commit_file.changes = file.value("changes", 0);

				result.files.push_back(
					std::move(commit_file));
			}
		}

		return true;
	}
	bool get_push_commits(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository,
		const std::string& before,
		const std::string& head,
		std::vector<CommitInfo>& commits)
	{
		const std::string url = "https://api.github.com/repos/" + repository + "/compare/" + before + "..." + head;

		GitHubResponse response;

		if(!github_get(curl, headers, url, response))
			return false;

		if(response.code != 200)
		{
			std::cerr
				<< "Compare API error: "
				<< response.code
				<< " "
				<< repository
				<< "\n"
				<< response.body
				<< "\n";

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Compare JSON parse error: "
				<< e.what()
				<< "\n";

			return false;
		}

		if(!parsed_json.contains("commits") || !parsed_json["commits"].is_array())
		{
			return true;
		}

		for(const auto& commit : parsed_json["commits"])
		{
			const std::string sha = commit.value("sha", "");

			if(sha.empty())
				continue;

			CommitInfo commit_info;

			if(get_commit(
				   curl,
				   headers,
				   repository,
				   sha,
				   commit_info))
			{
				commits.push_back(
					std::move(commit_info));
			}
		}

		return true;
	}

	bool load_issue_or_pr_labels(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository,
		int number,
		bool pull_request,
		std::string& title,
		std::vector<LabelInfo>& labels)
	{
		GitHubResponse response;

		const std::string url = "https://api.github.com/repos/"
								+ repository
								+ (pull_request ? "/pulls/" : "/issues/")
								+ std::to_string(number);

		if(!github_get(curl, headers, url, response))
			return false;

		if(response.code != 200)
		{
			std::cerr
				<< "Labels API error: "
				<< response.code
				<< " "
				<< repository
				<< "#"
				<< number
				<< '\n';

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Labels JSON parse error: "
				<< e.what()
				<< '\n';

			return false;
		}

		if(!parsed_json.contains("labels") || !parsed_json["labels"].is_array())
		{
			return true;
		}

		title = parsed_json.value("title", "");
		std::unordered_set<std::string> seen;

		for(const auto& label : parsed_json["labels"])
		{
			if(!label.is_object())
				continue;

			const std::string name = label.value("name", "");

			if(name.empty())
				continue;

			if(!seen.insert(name).second)
				continue;

			LabelInfo info;

			info.name  = name;
			info.color = label.value("color", "");

			labels.push_back(std::move(info));
		}

		return true;
	}
	bool load_pull_request(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository,
		int pull_number,
		std::string& title,
		std::string& body,
		std::string& author)
	{
		GitHubResponse response;

		const std::string url = "https://api.github.com/repos/" + repository + "/pulls/" + std::to_string(pull_number);

		if(!github_get(curl, headers, url, response))
			return false;

		if(response.code != 200)
		{
			std::cerr
				<< "Pull request API error: "
				<< response.code
				<< " "
				<< repository
				<< "#"
				<< pull_number
				<< "\n"
				<< response.body
				<< "\n";

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Pull request JSON parse error: "
				<< e.what()
				<< "\n";

			return false;
		}

		title = parsed_json.value("title", "");

		body = "";
		if(parsed_json.contains("body") && parsed_json["body"].is_string())
		{
			body = parsed_json["body"].get<std::string>();
		}

		if(parsed_json.contains("user") && parsed_json["user"].is_object())
			author = parsed_json["user"].value("login", "");

		return true;
	}

	bool get_label_event(
		const json& event,
		LabelBatch& batch)
	{
		const std::string type = event.value("type", "");

		if(type != "IssuesEvent" && type != "PullRequestEvent")
		{
			return false;
		}

		if(!event.contains("payload") || !event["payload"].is_object())
		{
			return false;
		}

		const auto& payload = event["payload"];

		const std::string action = payload.value("action", "");

		if(action != "labeled" && action != "unlabeled")
		{
			return false;
		}

		const json* object = nullptr;

		if(type == "IssuesEvent")
		{
			if(!payload.contains("issue") || !payload["issue"].is_object())
			{
				return false;
			}

			object = &payload["issue"];
		}
		else
		{
			if(!payload.contains("pull_request") || !payload["pull_request"].is_object())
			{
				return false;
			}

			object = &payload["pull_request"];
		}

		const auto& issue_or_pr = *object;

		if(!event.contains("repo") || !event["repo"].is_object())
		{
			return false;
		}

		if(!event.contains("actor") || !event["actor"].is_object())
		{
			return false;
		}

		batch.repository = event["repo"].value("name", "");

		batch.actor = event["actor"].value("login", "");

		batch.title = issue_or_pr.value("title", "");

		batch.number = issue_or_pr.value("number", 0);

		batch.pull_request = type == "PullRequestEvent";

		return !batch.repository.empty() && batch.number > 0;
	}

	void process_repository_event(
		CURL* curl,
		struct curl_slist* headers,
		const json& event)
	{
		const std::string type = event.value("type", "");

		if(!event.contains("payload") || !event["payload"].is_object())
		{
			return;
		}

		const auto& payload = event["payload"];

		const std::string action = payload.value("action", "");

		const std::string repository = event["repo"].value("name", "");

		std::string author;

		if(event.contains("actor") && event["actor"].is_object() && event["actor"].contains("login") && event["actor"]["login"].is_string())
		{
			author = event["actor"]["login"].get<std::string>();
		}

		if(type == "IssuesEvent")
		{
			if(!payload.contains("issue") || !payload["issue"].is_object())
			{
				return;
			}

			const auto& issue = payload["issue"];
			std::string title = issue.value("title", "");

			const int number = issue.value("number", 0);

			if(action == "opened")
			{
				std::string body;

				if(issue.contains("body") && issue["body"].is_string())
				{
					body = issue["body"].get<std::string>();
				}

				std::string issue_author;

				if(issue.contains("user") && issue["user"].is_object() && issue["user"].contains("login") && issue["user"]["login"].is_string())
				{
					issue_author = issue["user"]["login"].get<std::string>();
				}

				on_issue_or_pr_created(
					repository,
					false,
					number,
					title,
					body,
					issue_author);

				return;
			}

			if(action == "closed" || action == "reopened")
			{
				std::string extra;

				if(action == "closed" && issue.contains("state_reason") && issue["state_reason"].is_string())
				{
					const std::string state_reason = issue["state_reason"].get<std::string>();

					if(state_reason == "not_planned")
						extra = "not planned";
					if(state_reason == "duplicate")
						extra = "duplicate";
				}

				on_issue_or_pr_state_change(
					repository,
					false,
					number,
					author,
					action,
					extra);

				return;
			}
			return;
		}

		if(type == "PullRequestEvent")
		{
			if(!payload.contains("pull_request") || !payload["pull_request"].is_object())
			{
				return;
			}

			const auto& pull_request = payload["pull_request"];

			const int number = payload.value("number", 0);

			if(number <= 0)
				return;

			if(action == "opened")
			{
				std::string title;
				std::string body;
				std::string pr_author;

				if(!load_pull_request(
					   curl,
					   headers,
					   repository,
					   number,
					   title,
					   body,
					   pr_author))
				{
					return;
				}

				on_issue_or_pr_created(
					repository,
					true,
					number,
					title,
					body,
					pr_author);

				return;
			}

			if(action == "closed" || action == "merged" || action == "reopened")
			{
				std::string extra;
				std::string state_action = action;

				if(action == "closed")
				{
					const bool merged = pull_request.value("merged", false);

					if(merged)
					{
						state_action = "merged";
					}
					else if(pull_request.contains("state_reason") && pull_request["state_reason"].is_string())
					{
						const std::string state_reason = pull_request["state_reason"].get<std::string>();

						if(state_reason == "not_planned")
							extra = "not planned";
						else if(state_reason == "completed")
							extra = "completed";
					}
				}

				on_issue_or_pr_state_change(
					repository,
					true,
					number,
					author,
					action,
					extra);

				return;
			}

			return;
		}
	}

	bool update_repository_commits(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository)
	{
		GitHubResponse response;

		const std::string url = "https://api.github.com/repos/"
								+ repository
								+ "/commits?per_page=100";

		if(!github_get(
			   curl,
			   headers,
			   url,
			   response))
		{
			return false;
		}

		if(response.code != 200)
		{
			std::cerr
				<< "Commits API error: "
				<< response.code
				<< " "
				<< repository
				<< "\n"
				<< response.body
				<< "\n";

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Commits JSON parse error: "
				<< e.what()
				<< "\n";

			return false;
		}

		if(!parsed_json.is_array())
			return false;

		std::string previous_commit_sha;

		{
			std::lock_guard<std::mutex> lock(state_mutex);

			previous_commit_sha = repo_states[repository].latest_commit_sha;
		}

		/*
		 * First request:
		 * establish the commit cursor without
		 * generating notifications for old commits.
		 */
		if(previous_commit_sha.empty())
		{
			if(!parsed_json.empty())
			{
				const std::string sha = parsed_json[0].value("sha", "");

				if(!sha.empty())
				{
					std::lock_guard<std::mutex> lock(state_mutex);

					repo_states[repository].latest_commit_sha = sha;
				}
			}

			return true;
		}

		std::vector<json> new_commits;

		/*
		 * API returns newest -> oldest.
		 */
		for(const auto& commit : parsed_json)
		{
			const std::string sha = commit.value("sha", "");

			if(sha.empty())
				continue;

			if(sha == previous_commit_sha)
				break;

			new_commits.push_back(commit);
		}

		/*
		 * Process oldest -> newest.
		 */
		std::reverse(
			new_commits.begin(),
			new_commits.end());

		for(const auto& commit_event : new_commits)
		{
			const std::string sha = commit_event.value("sha", "");

			if(sha.empty())
				continue;

			CommitInfo commit;

			if(!get_commit(
				   curl,
				   headers,
				   repository,
				   sha,
				   commit))
			{
				continue;
			}

			PushInfo push;

			if(commit_event.contains("author")
			   && commit_event["author"].is_object())
			{
				push.actor = commit_event["author"].value(
					"login",
					"");
			}

			/*
			 * /commits does not provide a push event's
			 * branch information in the same way as
			 * PushEvent.
			 *
			 * Leave it empty for now.
			 */
			push.branch.clear();

			push.commits.push_back(
				std::move(commit));

			on_commits(
				repository,
				push);
		}

		/*
		 * Advance cursor to newest commit.
		 */
		{
			std::lock_guard<std::mutex> lock(state_mutex);

			repo_states[repository].latest_commit_sha = parsed_json[0].value(
				"sha",
				"");
		}

		return true;
	}
	bool update_repository_events(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository)
	{
		GitHubResponse response;

		const std::string url = "https://api.github.com/repos/" + repository + "/events?per_page=100";

		if(!github_get(curl, headers, url, response))
			return false;

		if(response.code != 200)
		{
			std::cerr
				<< "Events API error: "
				<< response.code
				<< " "
				<< repository
				<< "\n"
				<< response.body
				<< "\n";

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Events JSON parse error: "
				<< e.what()
				<< "\n";

			return false;
		}

		if(!parsed_json.is_array())
			return false;

		std::string previous_event_id;

		{
			std::lock_guard<std::mutex> lock(state_mutex);

			previous_event_id = repo_states[repository].latest_event_id;
		}

		/*
		 * First request:
		 * establish the event cursor without generating
		 * notifications for old events.
		 */
		if(previous_event_id.empty())
		{
			if(!parsed_json.empty())
			{
				std::lock_guard<std::mutex> lock(state_mutex);

				repo_states[repository].latest_event_id = parsed_json[0].value("id", "");
			}

			return true;
		}

		std::vector<json> new_events;

		/*
		 * API returns newest -> oldest.
		 */
		for(const auto& event : parsed_json)
		{
			const std::string event_id = event.value("id", "");

			if(event_id.empty())
				continue;

			if(event_id == previous_event_id)
				break;

			const std::string type = event.value("type", "");

			if(type == "IssuesEvent" || type == "PullRequestEvent" || type == "IssueCommentEvent")
			{
				new_events.push_back(event);
			}
		}

		/*
		 * Process oldest -> newest.
		 *
		 * This is important for label changes:
		 *
		 *   unlabeled A
		 *   labeled   A
		 *
		 * must stay in exactly this order.
		 */
		std::reverse(
			new_events.begin(),
			new_events.end());

		std::unordered_map<
			LabelEventKey,
			LabelBatch,
			LabelEventKeyHash>
			label_batches;
		for(const auto& event : new_events)
		{
			try
			{
				const std::string type = event.value("type", "");
				if(type == "IssuesEvent" || type == "PullRequestEvent")
				{
					const std::string action = event.contains("payload") && event["payload"].is_object()
												   ? event["payload"].value("action", "")
												   : "";

					if(action == "labeled" || action == "unlabeled")
					{
						LabelBatch batch;

						if(!get_label_event(event, batch))
							continue;

						LabelEventKey key{
							batch.repository,
							batch.number,
							batch.pull_request
						};

						/*
						 * Keep only the latest label event for this issue/PR.
						 *
						 * new_events are processed oldest -> newest,
						 * so overwriting here leaves us with the
						 * newest event for this object.
						 */
						label_batches[key] = std::move(batch);

						continue;
					}

					process_repository_event(
						curl,
						headers,
						event);

					continue;
				}

				if(type == "IssueCommentEvent")
				{
					const auto& payload = event["payload"];

					if(payload.value("action", "") != "created")
						continue;

					if(!payload.contains("issue") || !payload["issue"].is_object() || !payload.contains("comment") || !payload["comment"].is_object())
					{
						continue;
					}

					const auto& issue = payload["issue"];

					const auto& comment = payload["comment"];

					const std::string repository = event["repo"].value("name", "");

					const int issue_number = issue.value("number", 0);

					const std::string title = issue.value("title", "");

					const std::string username = comment["user"].value("login", "");

					const std::string message = comment.value("body", "");

					const bool pull_request = issue.contains("pull_request");

					if(pull_request)
					{
						on_pull_request_comment(
							repository,
							issue_number,
							title,
							username,
							"",
							message);
					}
					else
					{
						on_issue_comment(
							repository,
							issue_number,
							title,
							username,
							"",
							message);
					}

					continue;
				}
			}
			catch(const json::exception& e)
			{
				std::cerr
					<< "Repository event JSON error: "
					<< e.what()
					<< "\nEvent:\n"
					<< event.dump(2)
					<< "\n";

				continue;
			}
		}
		for(auto& [key, batch] : label_batches)
		{
			if(!load_issue_or_pr_labels(
				   curl,
				   headers,
				   batch.repository,
				   batch.number,
				   batch.pull_request,
				   batch.title,
				   batch.labels))
			{
				continue;
			}

			on_labels_changed(
				batch.repository,
				batch.actor,
				batch.title,
				batch.number,
				batch.pull_request,
				batch.labels);
		}

		/*
		 * Advance cursor to the newest event regardless
		 * of whether it was PushEvent.
		 */
		{
			std::lock_guard<std::mutex> lock(state_mutex);

			repo_states[repository].latest_event_id = parsed_json[0].value("id", "");
		}

		return true;
	}

	/*
	 * ---- Notification processing ----
	 */
	bool get_issue_timeline(
		CURL* curl,
		struct curl_slist* headers,
		const std::string& repository,
		int number,
		std::vector<json>& events)
	{
		GitHubResponse response;

		const std::string url = "https://api.github.com/repos/"
								+ repository
								+ "/issues/"
								+ std::to_string(number)
								+ "/timeline?per_page=100";

		if(!github_get(
			   curl,
			   headers,
			   url,
			   response))
		{
			return false;
		}

		if(response.code != 200)
		{
			std::cerr
				<< "Timeline API error: "
				<< response.code
				<< " "
				<< repository
				<< "#"
				<< number
				<< "\n"
				<< response.body
				<< "\n";

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Timeline JSON parse error: "
				<< e.what()
				<< '\n';

			return false;
		}

		if(!parsed_json.is_array())
			return false;

		events.clear();
		events.reserve(parsed_json.size());

		for(const auto& event : parsed_json)
		{
			if(event.is_object())
				events.push_back(event);
		}

		return true;
	}
	bool find_notification_event(
		const json& notification,
		const std::vector<json>& events,
		json& result)
	{
		const auto& subject = notification["subject"];

		const std::string updated_at = notification.value("updated_at", "");

		if(updated_at.empty())
			return false;

		bool found = false;
		std::string best_time;

		/*
		 * Find the latest timeline event that happened
		 * no later than notification.updated_at.
		 *
		 * This works for:
		 *
		 *   commented
		 *   labeled
		 *   unlabeled
		 *   closed
		 *   reopened
		 *   merged
		 *   opened
		 *   etc.
		 */
		for(const auto& event : events)
		{
			const std::string event_time = event.value("created_at", "");

			if(event_time.empty())
				continue;

			if(event_time > updated_at)
				continue;

			if(!found || event_time > best_time)
			{
				found	  = true;
				best_time = event_time;
				result	  = event;
			}
		}

		return found;
	}
	std::chrono::system_clock::time_point parse_github_time(
		const std::string& value)
	{
		std::tm tm = {};

		std::istringstream stream(value);

		stream >> std::get_time(
			&tm,
			"%Y-%m-%dT%H:%M:%SZ");

		if(stream.fail())
			return {};

#if defined(_WIN32)
		return std::chrono::system_clock::from_time_t(_mkgmtime(&tm));
#else
		return std::chrono::system_clock::from_time_t(timegm(&tm));
#endif
	}

	bool load_notification_issue_or_pr(
		CURL* curl,
		struct curl_slist* headers,
		const json& notification,
		std::string& repository,
		int& number,
		bool& pull_request,
		std::string& title,
		std::string& body,
		std::string& author,
		std::string& state,
		bool& merged)
	{
		const auto& subject = notification["subject"];

		const std::string type = subject.value("type", "");

		if(type != "Issue" && type != "PullRequest")
		{
			return false;
		}

		const std::string url = subject.value("url", "");

		if(url.empty())
			return false;

		const size_t slash = url.find_last_of('/');

		if(slash == std::string::npos)
			return false;

		try
		{
			number = std::stoi(url.substr(slash + 1));
		}
		catch(...)
		{
			return false;
		}

		repository = notification["repository"]
						 .value("full_name", "");

		if(repository.empty() || number <= 0)
		{
			return false;
		}

		pull_request = type == "PullRequest";

		GitHubResponse response;

		if(!github_get(
			   curl,
			   headers,
			   url,
			   response))
		{
			return false;
		}

		if(response.code != 200)
		{
			std::cerr
				<< "Notification Issue/PR API error: "
				<< response.code
				<< " "
				<< repository
				<< "#"
				<< number
				<< '\n';

			return false;
		}

		json parsed_json;

		try
		{
			parsed_json = json::parse(
				response.body);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Notification Issue/PR JSON parse error: "
				<< e.what()
				<< '\n';

			return false;
		}

		title = parsed_json.value(
			"title",
			"");

		body.clear();

		if(parsed_json.contains("body") && parsed_json["body"].is_string())
		{
			body = parsed_json["body"]
					   .get<std::string>();
		}

		author.clear();

		if(parsed_json.contains("user") && parsed_json["user"].is_object() && parsed_json["user"].contains("login") && parsed_json["user"]["login"].is_string())
		{
			author = parsed_json["user"]["login"]
						 .get<std::string>();
		}

		state = parsed_json.value(
			"state",
			"");

		merged = parsed_json.value(
			"merged",
			false);

		return true;
	}
	bool process_issue_or_pr(
		CURL* curl,
		struct curl_slist* headers,
		const json& notification)
	{
		const auto& subject = notification["subject"];

		const std::string type = subject.value("type", "");

		if(type != "Issue" && type != "PullRequest")
			return false;

		/*
		 * State/open/merge/etc. notification.
		 */
		const std::string repository = notification["repository"].value(
			"full_name",
			"");

		const std::string subject_url = subject.value("url", "");

		if(repository.empty() || subject_url.empty())
			return false;

		const size_t slash = subject_url.find_last_of('/');

		if(slash == std::string::npos)
			return false;

		int number = 0;

		try
		{
			number = std::stoi(
				subject_url.substr(slash + 1));
		}
		catch(...)
		{
			return false;
		}

		if(number <= 0)
			return false;

		std::vector<json> events;

		if(!get_issue_timeline(
			   curl,
			   headers,
			   repository,
			   number,
			   events))
		{
			return false;
		}

		json event;

		if(!find_notification_event(
			   notification,
			   events,
			   event))
		{
			std::cerr
				<< "Could not find timeline event for notification "
				<< notification.value("id", "")
				<< " ("
				<< repository
				<< "#"
				<< number
				<< ")\n";

			return false;
		}

		const std::string action = event.value("event", "");

		std::string author;

		if(event.contains("actor") && event["actor"].is_object())
		{
			author = event["actor"].value(
				"login",
				"");
		}

		const bool pull_request = type == "PullRequest";

		std::cout
			<< "\n=== NOTIFICATION ===\n"
			<< repository
			<< "#"
			<< number
			<< "\nType: "
			<< type
			<< "\nAction: "
			<< action
			<< "\nActor: "
			<< author
			<< "\n";

		if(action == "commented")
		{
			const std::string comment_url = event.value("url", "");

			if(comment_url.empty())
				return false;

			{
				std::lock_guard<std::mutex> lock(state_mutex);

				if(!notification_state.processed_comments
						.insert(comment_url)
						.second)
				{
					return true;
				}
			}

			GitHubResponse response;

			if(!github_get(
				   curl,
				   headers,
				   comment_url,
				   response))
			{
				return false;
			}

			if(response.code != 200)
				return false;

			json comment;

			try
			{
				comment = json::parse(response.body);
			}
			catch(const json::exception&)
			{
				return false;
			}

			std::string username;

			if(comment.contains("user") && comment["user"].is_object())
			{
				username = comment["user"].value(
					"login",
					"");
			}

			const std::string message = comment.value("body", "");

			const std::string title = subject.value("title", "");

			if(pull_request)
			{
				on_pull_request_comment(
					repository,
					number,
					title,
					username,
					"",
					message);
			}
			else
			{
				on_issue_comment(
					repository,
					number,
					title,
					username,
					"",
					message);
			}

			return true;
		}
		if(action == "opened")
		{
			std::string title;
			std::string body;
			std::string issue_author;

			if(pull_request)
			{
				if(!load_pull_request(
					   curl,
					   headers,
					   repository,
					   number,
					   title,
					   body,
					   issue_author))
				{
					return false;
				}
			}
			else
			{
				GitHubResponse response;

				const std::string url = "https://api.github.com/repos/"
										+ repository
										+ "/issues/"
										+ std::to_string(number);

				if(!github_get(
					   curl,
					   headers,
					   url,
					   response))
				{
					return false;
				}

				if(response.code != 200)
					return false;

				try
				{
					const json issue = json::parse(response.body);

					title = issue.value(
						"title",
						"");

					body = issue.value(
						"body",
						"");

					if(issue.contains("user") && issue["user"].is_object())
					{
						issue_author = issue["user"].value(
							"login",
							"");
					}
				}
				catch(const json::exception& e)
				{
					std::cerr
						<< "Issue JSON parse error: "
						<< e.what()
						<< '\n';

					return false;
				}
			}

			on_issue_or_pr_created(
				repository,
				pull_request,
				number,
				title,
				body,
				issue_author);

			return true;
		}

		if(action == "closed" || action == "reopened" || action == "merged")
		{
			std::string extra;
			std::string state_action = action;

			if(!pull_request && action == "closed")
			{
				GitHubResponse response;

				const std::string url = "https://api.github.com/repos/"
										+ repository
										+ "/issues/"
										+ std::to_string(number);

				if(github_get(
					   curl,
					   headers,
					   url,
					   response)
				   && response.code == 200)
				{
					try
					{
						const json issue = json::parse(response.body);

						const std::string state_reason = issue.value(
							"state_reason",
							"");

						if(state_reason == "not_planned")
							extra = "not planned";
						else if(state_reason == "duplicate")
							extra = "duplicate";
					}
					catch(const json::exception&)
					{
					}
				}
			}

			if(pull_request && action == "closed")
			{
				GitHubResponse response;

				const std::string url = "https://api.github.com/repos/"
										+ repository
										+ "/pulls/"
										+ std::to_string(number);

				if(github_get(
					   curl,
					   headers,
					   url,
					   response)
				   && response.code == 200)
				{
					try
					{
						const json pull = json::parse(response.body);

						if(pull.value(
							   "merged",
							   false))
						{
							state_action = "merged";
						}
						else
						{
							const std::string state_reason = pull.value(
								"state_reason",
								"");

							if(state_reason == "not_planned")
								extra = "not planned";
							else if(state_reason == "completed")
								extra = "completed";
						}
					}
					catch(const json::exception&)
					{
					}
				}
			}

			on_issue_or_pr_state_change(
				repository,
				pull_request,
				number,
				author,
				state_action,
				extra);

			return true;
		}
		if(action == "labeled" || action == "unlabeled")
		{
			LabelBatch batch;

			batch.repository   = repository;
			batch.actor		   = author;
			batch.number	   = number;
			batch.pull_request = pull_request;

			if(!load_issue_or_pr_labels(
				   curl,
				   headers,
				   repository,
				   number,
				   pull_request,
				   batch.title,
				   batch.labels))
			{
				return false;
			}

			on_labels_changed(
				batch.repository,
				batch.actor,
				batch.title,
				batch.number,
				batch.pull_request,
				batch.labels);

			return true;
		}

		/*
		 * No existing callback for this notification type.
		 */
		return false;
	}
	void parse_response(CURL* curl, struct curl_slist* headers, const std::string& resp)
	{
		json parsed_json;

		try
		{
			parsed_json = json::parse(resp);
		}
		catch(const json::exception& e)
		{
			std::cerr
				<< "Notifications JSON parse error: "
				<< e.what()
				<< '\n';

			return;
		}

		if(!parsed_json.is_array())
			return;

		for(const auto& notification : parsed_json)
		{
			const std::string id = notification.value(
				"id",
				"");

			if(id.empty())
				continue;

			const std::string updated_at = notification.value(
				"updated_at",
				"");

			bool changed = false;

			{
				std::lock_guard<std::mutex> lock(
					state_mutex);

				auto it = notification_state.seen.find(id);

				if(it == notification_state.seen.end())
				{
					/*
					 * First time we see this thread.
					 *
					 * Establish the cursor.
					 */
					notification_state.seen.emplace(
						id,
						updated_at);
				}
				else if(it->second != updated_at)
				{
					/*
					 * Same thread, new notification.
					 */
					it->second = updated_at;

					changed = true;
				}
			}

			if(!changed)
				continue;

			const std::string type = notification.contains("subject") && notification["subject"].is_object()
										 ? notification["subject"].value(
											   "type",
											   "")
										 : "";

			if(type != "Issue" && type != "PullRequest")
			{
				continue;
			}

			try
			{
				process_issue_or_pr(
					curl,
					headers,
					notification);
			}
			catch(const std::exception& e)
			{
				std::cerr
					<< "Notification processing error: "
					<< e.what()
					<< '\n';
			}
		}

		notification_state.initialized = true;

		/*
		 * Prevent unlimited growth.
		 */
		if(notification_state.seen.size() > 5000)
		{
			std::lock_guard<std::mutex> lock(
				state_mutex);

			while(notification_state.seen.size() > 2500)
			{
				notification_state.seen.erase(
					notification_state.seen.begin());
			}
		}
	}

	/*
	 * ---- Main daemon ----
	 */

	void daemon_worker(const std::string& token)
	{
		CURL* curl = curl_easy_init();

		if(!curl)
		{
			std::cerr << "CURL init fail\n";
			return;
		}

		struct curl_slist* headers = nullptr;

		headers = curl_slist_append(
			headers,
			"Accept: application/vnd.github+json");

		headers = curl_slist_append(
			headers,
			"User-Agent: C++ Git-Sync");

		const std::string authHeader = "Authorization: Bearer " + token;

		headers = curl_slist_append(
			headers,
			authHeader.c_str());

		headers = curl_slist_append(
			headers,
			"X-GitHub-Api-Version: 2026-03-10");

		std::cout << "Github Daemon started\n";

		/*
		 * Immediate startup sync.
		 */
		update_subscriptions(
			curl,
			headers);

		/*
		 * Establish commit baselines immediately.
		 */
		{
			std::vector<std::string> repos;

			{
				std::lock_guard<std::mutex> lock(state_mutex);
				repos = subscriptions;
			}

			for(const std::string& repository : repos)
			{
				update_repository_events(
					curl,
					headers,
					repository);
			}
		}

		auto next_subscriptions = std::chrono::steady_clock::now();

		auto next_repositories = std::chrono::steady_clock::now();

		auto next_notifications = std::chrono::steady_clock::now();

		int notification_poll_interval = 10;

		while(github_daemon_keepRunning)
		{
			try
			{
				const auto now = std::chrono::steady_clock::now();

				/*
				 * Subscriptions:
				 * every 20 minutes.
				 */
				if(now >= next_subscriptions)
				{
					if(update_subscriptions(
						   curl,
						   headers))
					{
						next_subscriptions = now + std::chrono::minutes(20);
					}
					else
					{
						next_subscriptions = now + std::chrono::minutes(1);
					}
				}

				/*
				 * Repository commits:
				 * every 20-25 seconds.
				 */
				if(now >= next_repositories)
				{
					std::vector<std::string> repos;

					{
						std::lock_guard<std::mutex> lock(state_mutex);
						repos = subscriptions;
					}

					for(const std::string& repository : repos)
					{
						if(!github_daemon_keepRunning)
							break;

						update_repository_events(
							curl,
							headers,
							repository);

						update_repository_commits(
							curl,
							headers,
							repository);
					}

					next_repositories = now + std::chrono::seconds(random_seconds(20, 25));
				}

				/*
				 * Notifications:
				 * 20-25 seconds, but obey GitHub's
				 * X-Poll-Interval when it is larger.
				 */
				if(now >= next_notifications)
				{
					GitHubResponse response;

					const std::string url = "https://api.github.com/"
											"notifications"
											"?per_page=50";

					if(github_get(
						   curl,
						   headers,
						   url,
						   response))
					{
						if(response.code == 200)
						{
							parse_response(
								curl,
								headers,
								response.body);

							if(!response.poll_interval.empty())
							{
								try
								{
									notification_poll_interval = std::stoi(
										response.poll_interval);
								}
								catch(...)
								{
								}
							}
						}
						else if(response.code == 304)
						{
							/*
							 * Nothing changed.
							 */
						}
						else
						{
							std::cerr
								<< "Notifications API error: "
								<< response.code
								<< '\n'
								<< response.body
								<< '\n';
						}
					}

					const int requested_interval = random_seconds(20, 25);

					const int actual_interval = std::max(
						requested_interval,
						notification_poll_interval);

					next_notifications = now + std::chrono::seconds(actual_interval);
				}

				std::this_thread::sleep_for(
					std::chrono::milliseconds(250));
			}
			catch(const std::exception& e)
			{
				std::cerr
					<< "Daemon exception: "
					<< e.what()
					<< '\n';
			}
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}

} // namespace

bool is_token_valid(const std::string& token)
{
	CURL* curl = curl_easy_init();

	if(!curl)
		return false;

	std::string readBuffer;

	curl_easy_setopt(
		curl,
		CURLOPT_URL,
		"https://api.github.com");

	curl_easy_setopt(
		curl,
		CURLOPT_WRITEFUNCTION,
		WriteCallback);

	curl_easy_setopt(
		curl,
		CURLOPT_WRITEDATA,
		&readBuffer);

	struct curl_slist* headers = nullptr;

	headers = curl_slist_append(
		headers,
		"Accept: application/json");

	headers = curl_slist_append(
		headers,
		"Content-Type: application/json");

	headers = curl_slist_append(
		headers,
		"User-Agent: C++ Git-Sync");

	const std::string auth = "Authorization: Bearer " + token;

	headers = curl_slist_append(
		headers,
		auth.c_str());

	curl_easy_setopt(
		curl,
		CURLOPT_HTTPHEADER,
		headers);

	CURLcode res = curl_easy_perform(curl);

	bool result = false;

	if(res != CURLE_OK)
	{
		std::cerr
			<< "curl_easy_perform() failed: "
			<< curl_easy_strerror(res)
			<< '\n';
	}
	else
	{
		long response_code = 0;

		curl_easy_getinfo(
			curl,
			CURLINFO_RESPONSE_CODE,
			&response_code);

		result = response_code == 200;
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return result;
}

void start_daemon(const std::string& token, ma_engine* engine, bool audio)
{
	AudioEngine = engine;
	HasAudio	= audio;
	std::thread thr(
		daemon_worker, token);

	thr.detach();
}
