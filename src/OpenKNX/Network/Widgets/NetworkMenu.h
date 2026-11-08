/**
 * @file        NetworkMenu.h
 * @brief       OFM-Network on the device display: the menu "Network" with the IP setting
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#pragma once
#if defined(DEVICE_DISPLAY_MODULE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

/**
 * @brief Register the root "Network" at DeviceDisplay; its rows are built from the live values on entry.
 * @return false while DeviceDisplay has no menu registry yet
 */
bool registerNetworkMenu();

#endif
