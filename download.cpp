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

#include "animation.hpp"
#include "download.hpp"
#include "files.hpp"
#include "json.hpp"
#include "lang.hpp"
#include "screenshot.hpp"
#include "scriptUtils.hpp"
#include "stringutils.hpp"

#include <3ds.h>
#include <curl/curl.h>
#include <dirent.h>
#include <malloc.h>
#include <regex>
#include <string>
#include <unistd.h>
#include <vector>

#define USER_AGENT APP_TITLE "-" VERSION_STRING

static char *result_buf = nullptr;
static size_t result_sz = 0;
static size_t result_written = 0;

#define TIME_IN_US 1
#define TIMETYPE curl_off_t
#define TIMEOPT CURLINFO_TOTAL_TIME_T
#define MINIMAL_PROGRESS_FUNCTIONALITY_INTERVAL	3000000

curl_off_t downloadTotal = 1; // Ne pas initialiser avec 0 pour éviter la division par zéro plus tard.
curl_off_t downloadNow = 0;

static FILE *downfile = nullptr;
static size_t file_buffer_pos = 0;
static size_t file_toCommit_size = 0;
static char *g_buffers[2] = { nullptr };
static u8 g_index = 0;
static Thread fsCommitThread;
static LightEvent readyToCommit;
static LightEvent waitCommit;
static bool killThread = false;
static bool writeError = false;
#define FILE_ALLOC_SIZE 0x60000

static int curlProgress(CURL *hnd,
					curl_off_t dltotal, curl_off_t dlnow,
					curl_off_t ultotal, curl_off_t ulnow)
{
	downloadTotal = dltotal;
	downloadNow = dlnow;

	return 0;
}

bool filecommit() {
	if (!downfile) return false;
	fseek(downfile, 0, SEEK_END);
	u32 byteswritten = fwrite(g_buffers[!g_index], 1, file_toCommit_size, downfile);
	if (byteswritten != file_toCommit_size) return false;
	file_toCommit_size = 0;
	return true;
}

static void commitToFileThreadFunc(void *args) {
	LightEvent_Signal(&waitCommit);

	while (true) {
		LightEvent_Wait(&readyToCommit);
		LightEvent_Clear(&readyToCommit);
		if (killThread) threadExit(0);
		writeError = !filecommit();
		LightEvent_Signal(&waitCommit);
	}
}

static size_t file_handle_data(char *ptr, size_t size, size_t nmemb, void *userdata) {
	(void)userdata;
	const size_t bsz = size * nmemb;
	size_t tofill = 0;
	if (writeError) return 0;

	if (!g_buffers[g_index]) {
		LightEvent_Init(&waitCommit, RESET_STICKY);
		LightEvent_Init(&readyToCommit, RESET_STICKY);

		s32 prio = 0;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		fsCommitThread = threadCreate(commitToFileThreadFunc, NULL, 0x1000, prio - 1, -2, true);

		g_buffers[0] = (char*)memalign(0x1000, FILE_ALLOC_SIZE);
		g_buffers[1] = (char*)memalign(0x1000, FILE_ALLOC_SIZE);

		if (!fsCommitThread || !g_buffers[0] || !g_buffers[1]) return 0;
	}

	if (file_buffer_pos + bsz >= FILE_ALLOC_SIZE) {
		tofill = FILE_ALLOC_SIZE - file_buffer_pos;
		memcpy(g_buffers[g_index] + file_buffer_pos, ptr, tofill);

		LightEvent_Wait(&waitCommit);
		LightEvent_Clear(&waitCommit);
		file_toCommit_size = file_buffer_pos + tofill;
		file_buffer_pos = 0;
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_buffers[g_index], file_toCommit_size);
		g_index = !g_index;
		LightEvent_Signal(&readyToCommit);
	}

	memcpy(g_buffers[g_index] + file_buffer_pos, ptr + tofill, bsz - tofill);
	file_buffer_pos += bsz - tofill;
	return bsz;
}

Result downloadToFile(const std::string &url, const std::string &path) {
	bool needToDelete = false;
	downloadTotal = 1;
	downloadNow = 0;

	CURLcode curlResult;
	CURL *hnd;
	Result retcode = 0;
	int res;

	printf("Downloading from:\n%s\nto:\n%s\n", url.c_str(), path.c_str());

	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) {
		retcode = -1;
		goto exit;
	}

	res = socInit((u32 *)socubuf, 0x100000);
	if (R_FAILED(res)) {
		retcode = res;
		goto exit;
	}

	/* Faire des annuaires. */
	for (char *slashpos = strchr(path.c_str() + 1, '/'); slashpos != NULL; slashpos = strchr(slashpos + 1, '/')) {
		char bak = *(slashpos);
		*(slashpos) = '\0';

		mkdir(path.c_str(), 0777);

		*(slashpos) = bak;
	}

	downfile = fopen(path.c_str(), "wb");
	if (!downfile) {
		retcode = -2;
		goto exit;
	}

	hnd = curl_easy_init();
	curl_easy_setopt(hnd, CURLOPT_BUFFERSIZE, FILE_ALLOC_SIZE);
	curl_easy_setopt(hnd, CURLOPT_URL, url.c_str());
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(hnd, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(hnd, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(hnd, CURLOPT_ACCEPT_ENCODING, "gzip");
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_XFERINFOFUNCTION, curlProgress);
	curl_easy_setopt(hnd, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, file_handle_data);
	curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(hnd, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(hnd, CURLOPT_STDERR, stdout);

	curlResult = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);

	if (curlResult != CURLE_OK) {
		retcode = -curlResult;
		needToDelete = true;
		goto exit;
	}

	LightEvent_Wait(&waitCommit);
	LightEvent_Clear(&waitCommit);

	file_toCommit_size = file_buffer_pos;
	svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_buffers[g_index], file_toCommit_size);
	g_index = !g_index;

	if (!filecommit()) {
		retcode = -3;
		needToDelete = true;
		goto exit;
	}

	fflush(downfile);

exit:
	if (fsCommitThread) {
		killThread = true;
		LightEvent_Signal(&readyToCommit);
		threadJoin(fsCommitThread, U64_MAX);
		killThread = false;
		fsCommitThread = nullptr;
	}

	socExit();

	if (socubuf) free(socubuf);

	if (downfile) {
		fclose(downfile);
		downfile = nullptr;
	}

	if (g_buffers[0]) {
		free(g_buffers[0]);
		g_buffers[0] = nullptr;
	}

	if (g_buffers[1]) {
		free(g_buffers[1]);
		g_buffers[1] = nullptr;
	}

	g_index = 0;
	file_buffer_pos = 0;
	file_toCommit_size = 0;
	writeError = false;

	if (needToDelete) {
		if (access(path.c_str(), F_OK) == 0) deleteFile(path.c_str()); // Supprimer le fichier, cause pas entièrement téléchargé.
	}

	return retcode;
}

/*
	fonction suivante est de
	https://github.com/angelsl/libctrfgh/blob/master/curl_test/src/main.c
*/
static size_t handle_data(char *ptr, size_t size, size_t nmemb, void *userdata) {
	(void)userdata;
	const size_t bsz = size*nmemb;

	if (result_sz == 0 || !result_buf) {
		result_sz = 0x1000;
		result_buf = (char *)malloc(result_sz);
	}

	bool need_realloc = false;
	while (result_written + bsz > result_sz) {
		result_sz <<= 1;
		need_realloc = true;
	}

	if (need_realloc) {
		char *new_buf = (char *)realloc(result_buf, result_sz);
		if (!new_buf) return 0;

		result_buf = new_buf;
	}

	if (!result_buf) return 0;

	memcpy(result_buf + result_written, ptr, bsz);
	result_written += bsz;
	return bsz;
}

/*
	Ce + ci-dessus est utilisé pour aucune écriture de fichier et à la place dans la RAM.
*/
static Result setupContext(CURL *hnd, const char *url) {
	curl_easy_setopt(hnd, CURLOPT_BUFFERSIZE, 102400L);
	curl_easy_setopt(hnd, CURLOPT_URL, url);
	curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(hnd, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(hnd, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
	curl_easy_setopt(hnd, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, handle_data);
	curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(hnd, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(hnd, CURLOPT_STDERR, stdout);

	return 0;
}

/*
	Télécharger un fichier d’une version GitHub.

	const std::string &url : Const Reference to the URL. (https://github.com/Owner/Repo)
	const std::string &asset : Const Référence à l’Asset. (File.filetype)
	const std::string &path : Const Reference, où stocker. (sdmc:/File.filetype)
	bool includePrereleases : Si incluant les Pre-Lease.
*/
Result downloadFromRelease(const std::string &url, const std::string &asset, const std::string &path, bool includePrereleases) {
	Result ret = 0;
	CURL *hnd;

	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) {
		return -1;
	}

	ret = socInit((u32*)socubuf, 0x100000);
	if (R_FAILED(ret)) {
		free(socubuf);
		return ret;
	}

	std::regex parseUrl("github\\.com\\/(.+)\\/(.+)");
	std::smatch result;
	regex_search(url, result, parseUrl);

	std::string repoOwner = result[1].str(), repoName = result[2].str();

	std::stringstream apiurlStream;
	apiurlStream << "https://api.github.com/repos/" << repoOwner << "/" << repoName << (includePrereleases ? "/releases" : "/releases/latest");
	std::string apiurl = apiurlStream.str();

	printf("Downloading latest release from repo:\n%s\nby:\n%s\n", repoName.c_str(), repoOwner.c_str());
	printf("Crafted API url:\n%s\n", apiurl.c_str());

	hnd = curl_easy_init();

	ret = setupContext(hnd, apiurl.c_str());
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = NULL;
		result_sz = 0;
		result_written = 0;
		return ret;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte to end it as a proper C style string.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return -1;
	}

	printf("Looking for asset with matching name:\n%s\n", asset.c_str());
	std::string assetUrl;

	if (nlohmann::json::accept(result_buf)) {
		nlohmann::json parsedAPI = nlohmann::json::parse(result_buf);

		if (parsedAPI.size() == 0) ret = -2; // All were prereleases and those are being ignored.

		if (ret != -2) {
			if (includePrereleases) parsedAPI = parsedAPI[0];

			if (parsedAPI["assets"].is_array()) {
				for (auto jsonAsset : parsedAPI["assets"]) {
					if (jsonAsset.is_object() && jsonAsset["name"].is_string() && jsonAsset["browser_download_url"].is_string()) {
						std::string assetName = jsonAsset["name"];

						if (ScriptUtils::matchPattern(asset, assetName)) {
							assetUrl = jsonAsset["browser_download_url"];
							break;
						}
					}
				}
			}
		}

	} else {
		ret = -3;
	}

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	if (assetUrl.empty() || ret != 0) {
		ret = DL_ERROR_GIT;

	} else {
		ret = downloadToFile(assetUrl, path);
	}

	return ret;
}

/*
	Vérifiez l’état du Wi-Fi.
	@return True si le Wi-Fi est connecté ; false si non.
*/
bool checkWifiStatus(void) {
	//return true; // Pour le citra.
	u32 wifiStatus;
	bool res = false;

	if (R_SUCCEEDED(ACU_GetWifiStatus(&wifiStatus)) && wifiStatus) res = true;

	return res;
}

void downloadFailed(void) { Msg::waitMsg(Lang::get("DOWNLOAD_FAILED")); }

void notImplemented(void) { Msg::waitMsg(Lang::get("NOT_IMPLEMENTED")); }

void doneMsg(void) { Msg::waitMsg(Lang::get("DONE")); }

void notConnectedMsg(void) { Msg::waitMsg(Lang::get("CONNECT_WIFI")); }

/*
	Retourner, si une mise à jour est disponible.

	const std::string &URL : Const Référence à l’URL de l'eShop.
	int revCurrent : La révision en cours. (-1 si inutilisée)
*/
bool IsUpdateAvailable(const std::string &URL, int revCurrent) {
	Msg::DisplayMsg(Lang::get("CHECK_ESHOP_UPDATES"));
	Result ret = 0;

	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) return false;

	ret = socInit((u32 *)socubuf, 0x100000);

	if (R_FAILED(ret)) {
		free(socubuf);
		return false;
	}

	CURL *hnd = curl_easy_init();

	ret = setupContext(hnd, URL.c_str());
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte pour le terminer comme une chaîne de style C appropriée.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	if (nlohmann::json::accept(result_buf)) {
		nlohmann::json parsedAPI = nlohmann::json::parse(result_buf);

		if (parsedAPI.contains("storeInfo") && parsedAPI.contains("storeContent")) {
			if (parsedAPI["storeInfo"].contains("revision") && parsedAPI["storeInfo"]["revision"].is_number()) {
				const int rev = parsedAPI["storeInfo"]["revision"];
				socExit();
				free(result_buf);
				free(socubuf);
				result_buf = nullptr;
				result_sz = 0;
				result_written = 0;

				return rev > revCurrent;
			}
		}
	}

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	return false;
}

/*
	Téléchargez l'eShop et revenez si la révision est plus élevée que la version actuelle.

	const std::string &URL : Const Référence à l’URL de l'eShop.
	int currentRev : Const Référence à la révision en cours. (-1 si inutilisée)
	std::string &fl : Sortie du chemin de fichier.
	bool isDownload : Si téléchargement ou mise à jour.
	bool isUDB : Si téléchargement ghosteshop.eshop ou non.
*/
bool DownloadEshop(const std::string &URL, int currentRev, std::string &fl, bool isDownload, bool isUDB) {
	if (isUDB) Msg::DisplayMsg(Lang::get("DOWNLOADING_ESHOP_DB"));
	else {
		if (currentRev > -1) Msg::DisplayMsg(Lang::get("CHECK_ESHOP_UPDATES"));
		else Msg::DisplayMsg((isDownload ? Lang::get("DOWNLOADING_ESHOP") : Lang::get("UPDATING_ESHOP")));
	}

	Result ret = 0;

	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) return false;

	ret = socInit((u32 *)socubuf, 0x100000);

	if (R_FAILED(ret)) {
		free(socubuf);
		return false;
	}

	CURL *hnd = curl_easy_init();

	ret = setupContext(hnd, URL.c_str());
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte pour le terminer comme une chaîne de style C appropriée.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	if (nlohmann::json::accept(result_buf)) {
		nlohmann::json parsedAPI = nlohmann::json::parse(result_buf);

		if (parsedAPI.contains("storeInfo") && parsedAPI.contains("storeContent")) {
			/* Vérifier, version == _ESHOP_VERSION. */
			if (parsedAPI["storeInfo"].contains("version") && parsedAPI["storeInfo"]["version"].is_number()) {
				if (parsedAPI["storeInfo"]["version"] == 3 || parsedAPI["storeInfo"]["version"] == 4) {
					if (currentRev > -1) {

						if (parsedAPI["storeInfo"].contains("revision") && parsedAPI["storeInfo"]["revision"].is_number()) {
							const int rev = parsedAPI["storeInfo"]["revision"];

							if (rev > currentRev) {
								Msg::DisplayMsg(Lang::get("UPDATING_ESHOP"));
								if (parsedAPI["storeInfo"].contains("file") && parsedAPI["storeInfo"]["file"].is_string()) {
									fl = parsedAPI["storeInfo"]["file"];

									/* Assurez-vous que ce n’est pas "/", sinon ça casse. */
									if (!(fl.find("/") != std::string::npos)) {

										FILE *out = fopen((std::string(_STORE_PATH) + fl).c_str(), "w");
										fwrite(result_buf, sizeof(char), result_written, out);
										fclose(out);

										socExit();
										free(result_buf);
										free(socubuf);
										result_buf = nullptr;
										result_sz = 0;
										result_written = 0;

										return true;

									} else {
										Msg::waitMsg(Lang::get("FILE_SLASH"));
									}
								}
							}
						}

					} else {
						if (parsedAPI["storeInfo"].contains("file") && parsedAPI["storeInfo"]["file"].is_string()) {
							fl = parsedAPI["storeInfo"]["file"];

							/* Assurez-vous que ce n’est pas "/", sinon ça casse. */
							if (!(fl.find("/") != std::string::npos)) {

								FILE *out = fopen((std::string(_STORE_PATH) + fl).c_str(), "w");
								fwrite(result_buf, sizeof(char), result_written, out);
								fclose(out);

								socExit();
								free(result_buf);
								free(socubuf);
								result_buf = nullptr;
								result_sz = 0;
								result_written = 0;

								return true;

							} else {
								Msg::waitMsg(Lang::get("FILE_SLASH"));
							}
						}
					}

				} else if (parsedAPI["storeInfo"]["version"] < 3) {
					Msg::waitMsg(Lang::get("ESHOP_TOO_OLD"));

				} else if (parsedAPI["storeInfo"]["version"] > _ESHOP_VERSION) {
					Msg::waitMsg(Lang::get("ESHOP_TOO_NEW"));

				}
			}

		} else {
			Msg::waitMsg(Lang::get("ESHOP_INVALID_ERROR"));
		}
	}

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	return false;
}

/*
	Téléchargez une feuille SpriteSheet.

	const std::string &URL : Const Référence à l’URL de SpriteSheet.
	const std::string &file : Const Référence au chemin de fichier.
*/
bool DownloadSpriteSheet(const std::string &URL, const std::string &file) {
	if (file.find("/") != std::string::npos) return false;
	Result ret = 0;

	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) return false;

	ret = socInit((u32 *)socubuf, 0x100000);

	if (R_FAILED(ret)) {
		free(socubuf);
		return false;
	}

	CURL *hnd = curl_easy_init();

	ret = setupContext(hnd, URL.c_str());
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte pour le terminer comme une chaîne de style C appropriée.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	C2D_SpriteSheet sheet = C2D_SpriteSheetLoadFromMem(result_buf, result_written);

	if (sheet) {
		if (C2D_SpriteSheetCount(sheet) > 0) {
			FILE *out = fopen((std::string(_STORE_PATH) + file).c_str(), "w");
			fwrite(result_buf, sizeof(char), result_written, out);
			fclose(out);

			socExit();
			free(result_buf);
			free(socubuf);
			result_buf = nullptr;
			result_sz = 0;
			result_written = 0;

			C2D_SpriteSheetFree(sheet);
			return true;
		}
	}

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	return false;
}

/*
	Vérifier les mises à jour de Ghost eShop
*/
bool IsGEUpdateAvailable() {
	if (!checkWifiStatus()) return false;

	Msg::DisplayMsg(Lang::get("CHECK_GE_UPDATES"));
	Result ret = 0;

	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) return false;

	ret = socInit((u32 *)socubuf, 0x100000);

	if (R_FAILED(ret)) {
		free(socubuf);
		return false;
	}

	CURL *hnd = curl_easy_init();

	ret = setupContext(hnd, "https://api.github.com/repos/Bot-Ghost/GHA/releases/latest");
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte pour le terminer comme une chaîne de style C appropriée.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return false;
	}

	if (nlohmann::json::accept(result_buf)) {
		nlohmann::json parsedAPI = nlohmann::json::parse(result_buf);

		if (parsedAPI.contains("tag_name") && parsedAPI["tag_name"].is_string()) {
			const std::string tag = parsedAPI["tag_name"];

			socExit();
			free(result_buf);
			free(socubuf);
			result_buf = nullptr;
			result_sz = 0;
			result_written = 0;

			return strcasecmp(StringUtils::lower_case(tag).c_str(), StringUtils::lower_case(C_V).c_str()) > 0;
		}
	}

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	return false;
}

extern bool is3DSX, exiting;
extern std::string _3dsxPath;

/*
	Exécuter l’action de mise à jour Ghost eShop.
*/
void UpdateAction() {
	if (ScriptUtils::downloadRelease("Bot-Ghost/GHA", (is3DSX ? "GhostEshop.3dsx" : "GhostEshop.cia"),
	(is3DSX ? _3dsxPath : "sdmc:/GhostEshop.cia"),
	false, Lang::get("DOWNLOADING_GHOST_ESHOP")) == 0) {

		if (is3DSX) {
			Msg::waitMsg(Lang::get("UPDATE_DONE"));
			exiting = true;
			return;
		}

		ScriptUtils::installFile("sdmc:/GhostEshop.cia", false, Lang::get("INSTALL_GHOST_ESHOP"));
		ScriptUtils::removeFile("sdmc:/GhostEshop.cia", Lang::get("DELETE_UNNEEDED_FILE"));
		Msg::waitMsg(Lang::get("UPDATE_DONE"));
		exiting = true;
	}
}

static StoreList fetch(const std::string &entry, nlohmann::json &js) {
	StoreList store = { "", "", "", "" };
	if (!js.contains(entry)) return store;

	if (js[entry].contains("title") && js[entry]["title"].is_string()) store.Title = js[entry]["title"];
	if (js[entry].contains("author") && js[entry]["author"].is_string()) store.Author = js[entry]["author"];
	if (js[entry].contains("url") && js[entry]["url"].is_string()) store.URL = js[entry]["url"];
	if (js[entry].contains("description") && js[entry]["description"].is_string()) store.Description = js[entry]["description"];

	return store;
}
/*
	Fetch Store list for available eShop.
*/
std::vector<StoreList> FetchStores() {
	Msg::DisplayMsg(Lang::get("FETCHING_RECOMMENDED_ESHOP"));
	std::vector<StoreList> stores = { };

	Result ret = 0;
	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) return stores;

	ret = socInit((u32 *)socubuf, 0x100000);

	if (R_FAILED(ret)) {
		free(socubuf);
		return stores;
	}

	CURL *hnd = curl_easy_init();

	ret = setupContext(hnd, "https://cdn.ghosteshop.com/script/eShop.json");
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return stores;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte to end it as a proper C style string.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return stores;
	}

	if (nlohmann::json::accept(result_buf)) {
		nlohmann::json parsedAPI = nlohmann::json::parse(result_buf);

		for(auto it = parsedAPI.begin(); it != parsedAPI.end(); ++it) {
			stores.push_back( fetch(it.key(), parsedAPI) );
		}
	}

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	return stores;
}

C2D_Image FetchScreenshot(const std::string &URL) {
	if (URL == "") return { };

	C2D_Image img = { };

	Result ret = 0;
	void *socubuf = memalign(0x1000, 0x100000);
	if (!socubuf) return img;

	ret = socInit((u32 *)socubuf, 0x100000);

	if (R_FAILED(ret)) {
		free(socubuf);
		return img;
	}

	CURL *hnd = curl_easy_init();

	ret = setupContext(hnd, URL.c_str());
	if (ret != 0) {
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return img;
	}

	CURLcode cres = curl_easy_perform(hnd);
	curl_easy_cleanup(hnd);
	char *newbuf = (char *)realloc(result_buf, result_written + 1);
	result_buf = newbuf;
	result_buf[result_written] = 0; // nullbyte to end it as a proper C style string.

	if (cres != CURLE_OK) {
		printf("Error in:\ncurl\n");
		socExit();
		free(result_buf);
		free(socubuf);
		result_buf = nullptr;
		result_sz = 0;
		result_written = 0;
		return img;
	}

	std::vector<u8> buffer;
	for (int i = 0; i < (int)result_written; i++) {
		buffer.push_back( result_buf[i] );
	}

	img = Screenshot::ConvertFromBuffer(buffer);

	socExit();
	free(result_buf);
	free(socubuf);
	result_buf = nullptr;
	result_sz = 0;
	result_written = 0;

	return img;
}
