#include "miniaudio.h"
#include <atomic>
#include <string>

inline std::atomic<bool> github_daemon_keepRunning(true);
bool is_token_valid(const std::string& token);
void start_daemon(const std::string& token, ma_engine* engine, bool audio);
