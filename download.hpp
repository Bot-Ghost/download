/*
*   This file is part of Ghost eShop Alternative 3DS
*   Copyright (C) 2020 Ghost eShop Team
*	
*	This code is used for Ghost eShop Alternative 3ds,
*	being a closed source code projects, therefore,
*	the only person authorized to modify this code is Ghost eShop Team,
*	or any other contributor directly authorized by Ghost0159.
*
*	For all requests, questions...
*	You can directly contact Ghost0159 being the creator and the main developer:
*		Email: contact.ghost0159@gmail.com
*
*	For others information, visit the official website:
*		https://ghosteshop.com/
*/

#ifndef _GHOST_ESHOP_DOWNLOAD_HPP
#define _GHOST_ESHOP_DOWNLOAD_HPP

#include "common.hpp"

#define APP_TITLE "Ghost eShop"
#define VERSION_STRING "12.0"

enum DownloadError {
	DL_ERROR_NONE = 0,
	DL_ERROR_WRITEFILE,
	DL_ERROR_ALLOC,
	DL_ERROR_STATUSCODE,
	DL_ERROR_GIT,
	DL_CANCEL, // Aucune idée si c’est nécessaire.
};

struct StoreList {
	std::string Title;
	std::string Author;
	std::string URL;
	std::string Description;
};

Result downloadToFile(const std::string &url, const std::string &path);
Result downloadFromRelease(const std::string &url, const std::string &asset, const std::string &path, bool includePrereleases);

/*
	Vérifiez l’état du Wi-Fi.
	@return True si le Wi-Fi est connecté ; false si non.
*/
bool checkWifiStatus(void);

/*
	Afficher "Veuillez vous connecter au Wi-Fi" pour 2s.
*/
void notConnectedMsg(void);

/*
	Affiche "Not Implemented Yet" pour 2s.
*/
void notImplemented(void);

/*
	Afficher le msg done.
*/
void doneMsg(void);

bool IsUpdateAvailable(const std::string &URL, int revCurrent);
bool DownloadEshop(const std::string &URL, int currentRev, std::string &fl, bool isDownload = false, bool isUDB = false);
bool DownloadSpriteSheet(const std::string &URL, const std::string &file);
bool IsGEUpdateAvailable();
void UpdateAction();
std::vector<StoreList> FetchStores();
C2D_Image FetchScreenshot(const std::string &URL);

#endif
