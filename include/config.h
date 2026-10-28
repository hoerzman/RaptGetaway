#pragma once

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Copy include/config_local.example.h to include/config_local.h and set a unique setup password."
#endif

constexpr char SETUP_AP_SSID[] = "RaptGateway-Setup";
