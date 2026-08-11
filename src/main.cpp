#include "main.hpp"
#include "assets.hpp"
#include "githubd.hpp"
#include "miniaudio.h"
#include "render.hpp"
#include "wayland.h"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
namespace fs = std::filesystem;

int main(int argc, char* argv[])
{
	ma_engine engine;
	bool audio = true;
	if(ma_engine_init(NULL, &engine) != MA_SUCCESS)
	{
		audio = false;
	}
	else
	{
		ma_resource_manager_register_encoded_data(
			ma_engine_get_resource_manager(&engine), "sfx", panel_open_wav, panel_open_wav_len);
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	wayland_backend();

	char buffer[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if(len != -1)
	{
		buffer[len] = '\0';
	}
	std::ifstream file(fs::path(buffer).parent_path().string() + "/.secret");
	if(!file.is_open())
	{
		std::cerr << "Error: Could not open the secret file!" << std::endl;
		return 1;
	}
	std::string line;
	std::getline(file, line); // Read only first line
	file.close();

	bool is_valid = is_token_valid(line);
	if(!is_valid)
	{
		std::cerr << "Github token is not valid!" << std::endl;
		return 1;
	}

	start_daemon(line, &engine, audio);
	printf("Wayland dispatcher started\n");

	while(true)
	{
		wayland_dispatch();
	}
	ma_engine_uninit(&engine);
	curl_global_cleanup();
	return 0;
}
