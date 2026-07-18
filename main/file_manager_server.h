#pragma once

#include <cstdint>

bool file_manager_server_start(uint16_t port = 80);
void file_manager_server_stop();
uint16_t file_manager_server_get_port();
