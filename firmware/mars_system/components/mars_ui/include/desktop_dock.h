#pragma once

/* Shared by the default home and the in-memory migration of old stock docks. */
#define MARS_NATIVE_DOCK_ELEMENTS \
    "{\"id\":\"mars-dock-shadow\",\"type\":\"block\",\"x\":48,\"y\":190,\"width\":224,\"height\":48,\"radius\":24,\"background\":\"#080d18\"}," \
    "{\"id\":\"mars-dock\",\"type\":\"block\",\"x\":48,\"y\":186,\"width\":224,\"height\":48,\"radius\":24,\"background\":\"#172235\",\"borderWidth\":1,\"borderColor\":\"#40536b\"}," \
    "{\"type\":\"button\",\"x\":60,\"y\":190,\"width\":48,\"height\":40,\"radius\":6,\"background\":\"#25334a\",\"text\":\"APP\",\"fontSize\":16,\"color\":\"#e4edff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.apps.open\"}}," \
    "{\"type\":\"button\",\"x\":136,\"y\":190,\"width\":48,\"height\":40,\"radius\":18,\"background\":\"#234657\",\"icon\":\"clock\",\"iconSize\":22,\"color\":\"#8cddff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.fluid_clock.open\"}}," \
    "{\"type\":\"button\",\"x\":212,\"y\":190,\"width\":48,\"height\":40,\"radius\":18,\"background\":\"#25334a\",\"icon\":\"settings\",\"iconSize\":20,\"color\":\"#e4edff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.system.open\"}}"
