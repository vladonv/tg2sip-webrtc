/*
 * Copyright (C) 2017-2018 infactum (infactum@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cctype>
#include <filesystem>
#include "utils.h"

bool is_digits(const std::string &str) { return std::all_of(str.begin(), str.end(), ::isdigit); };

std::string resolve_config_path(int argc, char **argv) {
    if (argc > 1) {
        return argv[1];
    }
    if (std::filesystem::exists("/etc/tg2sip-webrtc/config.ini")) {
        return "/etc/tg2sip-webrtc/config.ini";
    }
    return "settings.ini";
}
