// Adapted by HomeNX from Switube by dl3g0, GPL-3.0-or-later.
#include "transport.hpp"
#include <regex>
namespace swui {
inline std::string upgradeThumbnailUrl(const std::string& rawUrl, int targetSize = 544) {
  if (rawUrl.empty() || targetSize < 300) return rawUrl;
  std::string url = rawUrl;
  if (url.find("googleusercontent.com") != std::string::npos ||
      url.find("ggpht.com") != std::string::npos) {
    static const std::regex whRegex("=w\\d+-h\\d+");
    url = std::regex_replace(url, whRegex, "=w" + std::to_string(targetSize) + "-h" + std::to_string(targetSize));
    static const std::regex sRegex("=s\\d+");
    url = std::regex_replace(url, sRegex, "=s" + std::to_string(targetSize));
    if (url.find("=w") == std::string::npos && url.find("=s") == std::string::npos && url.find('?') == std::string::npos) {
      url += "=w" + std::to_string(targetSize) + "-h" + std::to_string(targetSize) + "-l90-rj";
    }
  } else if (url.find("i.ytimg.com") != std::string::npos) {
    auto pos = url.find("/default.jpg");
    if (pos != std::string::npos) {
      url.replace(pos, 12, "/hqdefault.jpg");
    }
  }
  return url;
}
}
#include "yt_music_client.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <random>
#include <sys/stat.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "NXConsoleLog.h"

using json = nlohmann::json;

namespace switube {

namespace {

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t totalSize = size * nmemb;
  std::string* str = static_cast<std::string*>(userp);
  if (totalSize > 2 * 1024 * 1024 - str->size()) return 0;
  str->append(static_cast<char*>(contents), totalSize);
  return totalSize;
}

// Búsqueda recursiva robusta de videoId en cualquier nivel del JSON
std::string findVideoId(const json& j, int depth = 0) {
  if (depth > 8) return "";
  if (j.is_object()) {
    if (j.contains("videoId") && j["videoId"].is_string()) {
      return j["videoId"].get<std::string>();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "trackingParams") continue;
      std::string id = findVideoId(it.value(), depth + 1);
      if (!id.empty()) return id;
    }
  } else if (j.is_array()) {
    for (const auto& elem : j) {
      std::string id = findVideoId(elem, depth + 1);
      if (!id.empty()) return id;
    }
  }
  return "";
}

// Búsqueda recursiva de browseId (Playlists, Álbumes, Artistas)
std::string findBrowseId(const json& j, int depth = 0) {
  if (depth > 8) return "";
  if (j.is_object()) {
    if (j.contains("browseId") && j["browseId"].is_string()) {
      return j["browseId"].get<std::string>();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "trackingParams") continue;
      std::string id = findBrowseId(it.value(), depth + 1);
      if (!id.empty()) return id;
    }
  } else if (j.is_array()) {
    for (const auto& elem : j) {
      std::string id = findBrowseId(elem, depth + 1);
      if (!id.empty()) return id;
    }
  }
  return "";
}

// Convierte cadenas "mm:ss" o "hh:mm:ss" a milisegundos
uint32_t parseDurationToMs(const std::string& str) {
  if (str.empty()) return 0;
  int parts[3] = {0, 0, 0};
  int partCount = 0;
  int current = 0;
  bool hasDigits = false;

  for (char c : str) {
    if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
      hasDigits = true;
    } else if (c == ':') {
      if (partCount < 2 && hasDigits) {
        parts[partCount++] = current;
        current = 0;
        hasDigits = false;
      } else {
        return 0;
      }
    } else {
      return 0;
    }
  }

  if (!hasDigits) return 0;
  parts[partCount++] = current;

  if (partCount == 2) {
    // mm:ss
    return static_cast<uint32_t>((parts[0] * 60 + parts[1]) * 1000);
  } else if (partCount == 3) {
    // hh:mm:ss
    return static_cast<uint32_t>((parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000);
  }
  return 0;
}

// Helper para extraer duración de flexColumns / fixedColumns
void extractDurationFromRow(const json& row, YtTrack& track) {
  if (row.contains("fixedColumns") && row["fixedColumns"].is_array()) {
    for (const auto& col : row["fixedColumns"]) {
      if (col.contains("musicResponsiveListItemFixedColumnRenderer")) {
        const auto& fixed = col["musicResponsiveListItemFixedColumnRenderer"];
        if (fixed.contains("text") && fixed["text"].contains("runs")) {
          const auto& runs = fixed["text"]["runs"];
          for (const auto& r : runs) {
            std::string txt = r.value("text", "");
            uint32_t ms = parseDurationToMs(txt);
            if (ms > 0) {
              track.durationMs = ms;
              track.durationStr = txt;
              return;
            }
          }
        }
      }
    }
  }

  if (row.contains("flexColumns") && row["flexColumns"].is_array()) {
    for (const auto& col : row["flexColumns"]) {
      if (col.contains("musicResponsiveListItemFlexColumnRenderer")) {
        const auto& flex = col["musicResponsiveListItemFlexColumnRenderer"];
        if (flex.contains("text") && flex["text"].contains("runs")) {
          const auto& runs = flex["text"]["runs"];
          for (const auto& r : runs) {
            std::string txt = r.value("text", "");
            uint32_t ms = parseDurationToMs(txt);
            if (ms > 0) {
              track.durationMs = ms;
              track.durationStr = txt;
              return;
            }
          }
        }
      }
    }
  }
}

// Búsqueda recursiva del token de continuación
std::string findContinuationToken(const json& j, int depth = 0) {
  if (depth > 28) return "";
  if (j.is_object()) {
    if (j.contains("nextContinuationData") && j["nextContinuationData"].contains("continuation") &&
        j["nextContinuationData"]["continuation"].is_string()) {
      return j["nextContinuationData"]["continuation"].get<std::string>();
    }
    if (j.contains("continuationCommand") && j["continuationCommand"].contains("token") &&
        j["continuationCommand"]["token"].is_string()) {
      return j["continuationCommand"]["token"].get<std::string>();
    }
    if (j.contains("reloadContinuationData") && j["reloadContinuationData"].contains("continuation") &&
        j["reloadContinuationData"]["continuation"].is_string()) {
      return j["reloadContinuationData"]["continuation"].get<std::string>();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "trackingParams") continue;
      std::string tok = findContinuationToken(it.value(), depth + 1);
      if (!tok.empty()) return tok;
    }
  } else if (j.is_array()) {
    for (const auto& elem : j) {
      std::string tok = findContinuationToken(elem, depth + 1);
      if (!tok.empty()) return tok;
    }
  }
  return "";
}

std::string extractTextNode(const json& node) {
  if (!node.is_object()) return "";
  if (node.contains("simpleText") && node["simpleText"].is_string()) {
    return node["simpleText"].get<std::string>();
  }
  if (node.contains("runs") && node["runs"].is_array() && !node["runs"].empty()) {
    std::string out;
    for (const auto& r : node["runs"]) {
      out += r.value("text", "");
    }
    return out;
  }
  return "";
}

bool parseTvTile(const json& tile, YtTrack& outTrack) {
  if (!tile.is_object()) return false;

  std::string vid;
  std::string bid;
  std::string ctype = tile.value("contentType", "");

  if (tile.contains("onSelectCommand") && tile["onSelectCommand"].is_object()) {
    const auto& osc = tile["onSelectCommand"];
    if (osc.contains("watchEndpoint") && osc["watchEndpoint"].is_object()) {
      vid = osc["watchEndpoint"].value("videoId", "");
    }
    if (osc.contains("browseEndpoint") && osc["browseEndpoint"].is_object()) {
      bid = osc["browseEndpoint"].value("browseId", "");
    }
  }

  std::string cid = tile.value("contentId", "");
  if (vid.empty() && bid.empty() && !cid.empty()) {
    if (ctype == "TILE_CONTENT_TYPE_PLAYLIST" || cid.rfind("PL", 0) == 0 || cid.rfind("OL", 0) == 0 || cid.rfind("VL", 0) == 0) {
      bid = (cid.rfind("VL", 0) == 0) ? cid : ("VL" + cid);
    } else if (cid.size() == 11) {
      vid = cid;
    }
  }

  if (!vid.empty() && vid.size() == 11) {
    outTrack.id = vid;
    outTrack.isPlaylist = false;
    outTrack.isArtist = false;
  } else if (!bid.empty()) {
    // Ignorar botones de navegación interna de TV ("Ver todo", inicio de sesión, etc.)
    if (bid.rfind("FE", 0) == 0 || bid == "VLSE") return false;
    outTrack.id = bid;
    if (bid.rfind("UC", 0) == 0 || ctype == "TILE_CONTENT_TYPE_CHANNEL") {
      outTrack.isArtist = true;
      outTrack.isPlaylist = false;
    } else {
      outTrack.isPlaylist = true;
      outTrack.isArtist = false;
    }
  } else {
    return false;
  }

  // 1. Metadata: title & artist/subtitle
  if (tile.contains("metadata") && tile["metadata"].contains("tileMetadataRenderer")) {
    const auto& meta = tile["metadata"]["tileMetadataRenderer"];
    if (meta.contains("title")) {
      outTrack.title = extractTextNode(meta["title"]);
    }
    if (meta.contains("lines") && meta["lines"].is_array() && !meta["lines"].empty()) {
      for (const auto& lineObj : meta["lines"]) {
        if (!lineObj.contains("lineRenderer")) continue;
        const auto& lr = lineObj["lineRenderer"];
        if (!lr.contains("items") || !lr["items"].is_array()) continue;
        for (const auto& item : lr["items"]) {
          if (!item.contains("lineItemRenderer")) continue;
          const auto& lir = item["lineItemRenderer"];
          if (lir.contains("text")) {
            std::string txt = extractTextNode(lir["text"]);
            if (!txt.empty() && txt != "•") {
              outTrack.artist = txt;
              break;
            }
          }
        }
        if (!outTrack.artist.empty()) break;
      }
    }
  }

  // Fallback a showMenuCommand (usado en listas y spotlights de TVHTML5)
  if (tile.contains("onLongPressCommand") && tile["onLongPressCommand"].contains("showMenuCommand")) {
    const auto& smc = tile["onLongPressCommand"]["showMenuCommand"];
    if (outTrack.title.empty() && smc.contains("title")) {
      outTrack.title = extractTextNode(smc["title"]);
    }
    if (outTrack.artist.empty() && smc.contains("subtitle")) {
      std::string sub = extractTextNode(smc["subtitle"]);
      auto bulletPos = sub.find(" • @");
      if (bulletPos != std::string::npos) {
        sub = sub.substr(0, bulletPos);
      }
      outTrack.artist = sub;
    }
  }

  // Fallback a entityMetadataRenderer dentro de onFocusCommand
  if (outTrack.title.empty() && tile.contains("onFocusCommand")) {
    std::function<void(const json&)> findSpotlight = [&](const json& n) {
      if (!outTrack.title.empty() || !n.is_object()) return;
      if (n.contains("entityMetadataRenderer") && n["entityMetadataRenderer"].is_object()) {
        const auto& emr = n["entityMetadataRenderer"];
        if (emr.contains("title")) {
          outTrack.title = extractTextNode(emr["title"]);
        }
        if (outTrack.artist.empty() && emr.contains("bylines") && emr["bylines"].is_array()) {
          for (const auto& by : emr["bylines"]) {
            if (!by.contains("lineRenderer") || !by["lineRenderer"].contains("items")) continue;
            for (const auto& it : by["lineRenderer"]["items"]) {
              if (it.contains("lineItemRenderer") && it["lineItemRenderer"].contains("text")) {
                std::string txt = extractTextNode(it["lineItemRenderer"]["text"]);
                if (!txt.empty() && txt != "•") {
                  outTrack.artist = txt;
                  break;
                }
              }
            }
            if (!outTrack.artist.empty()) break;
          }
        }
        return;
      }
      for (auto it = n.begin(); it != n.end(); ++it) {
        if (it.value().is_object()) findSpotlight(it.value());
        else if (it.value().is_array()) {
          for (const auto& el : it.value()) findSpotlight(el);
        }
      }
    };
    findSpotlight(tile["onFocusCommand"]);
  }

  // 2. Thumbnail y duración en tileHeaderRenderer
  if (tile.contains("header") && tile["header"].contains("tileHeaderRenderer")) {
    const auto& thr = tile["header"]["tileHeaderRenderer"];
    if (thr.contains("thumbnail") && thr["thumbnail"].contains("thumbnails") &&
        thr["thumbnail"]["thumbnails"].is_array() && !thr["thumbnail"]["thumbnails"].empty()) {
      outTrack.imageUrl = thr["thumbnail"]["thumbnails"].back().value("url", "");
    }
    if (thr.contains("thumbnailOverlays") && thr["thumbnailOverlays"].is_array()) {
      for (const auto& ov : thr["thumbnailOverlays"]) {
        if (ov.contains("thumbnailOverlayTimeStatusRenderer")) {
          const auto& tots = ov["thumbnailOverlayTimeStatusRenderer"];
          if (tots.contains("text")) {
            std::string durStr = extractTextNode(tots["text"]);
            uint32_t ms = parseDurationToMs(durStr);
            if (ms > 0) {
              outTrack.durationStr = durStr;
              outTrack.durationMs = ms;
              break;
            }
          }
        }
      }
    }
  }

  if (outTrack.artist.empty()) {
    outTrack.artist = "YouTube Music";
  }

  return !outTrack.title.empty();
}

} // namespace

YtMusicClient::YtMusicClient() {}

YtMusicClient::~YtMusicClient() {}

bool YtMusicClient::init() {
  // HOME owns the network runtime. The native player uses guest requests.
  return true;
}

void YtMusicClient::setAuthCookies(const std::string& cookies) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex_);
  authCookies_ = cookies;
}

bool YtMusicClient::isSignedIn() const {
  std::lock_guard<std::recursive_mutex> lock(clientMutex_);
  return account_.signedIn && (!account_.accessToken.empty() || !account_.refreshToken.empty());
}

YtAccountInfo YtMusicClient::getAccountInfo() const {
  std::lock_guard<std::recursive_mutex> lock(clientMutex_);
  return account_;
}

void YtMusicClient::loadAuthConfig() {
  const char* paths[] = {
      "/config/Switube/auth.json",
      "sdmc:/config/Switube/auth.json"
  };
  for (const char* path : paths) {
    std::ifstream f(path);
    if (!f.is_open()) continue;
    try {
      json j;
      f >> j;
      std::lock_guard<std::recursive_mutex> lock(clientMutex_);
      account_.accessToken = j.value("access_token", "");
      account_.refreshToken = j.value("refresh_token", "");
      account_.expiresAtEpoch = j.value("expires_at", static_cast<int64_t>(0));
      account_.name = j.value("name", "");
      account_.handle = j.value("handle", "");
      account_.photoUrl = j.value("photo_url", "");
      account_.signedIn = !account_.refreshToken.empty() || !account_.accessToken.empty();
      if (account_.signedIn) {
        nxlog::pushf("YtMusicClient: loaded linked YouTube account '%s' (%s)",
                     account_.name.c_str(), account_.handle.c_str());
      }
      return;
    } catch (...) {}
  }
}

void YtMusicClient::saveAuthConfig() {
  mkdir("/config", 0755);
  mkdir("/config/Switube", 0755);
  YtAccountInfo info;
  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex_);
    info = account_;
  }
  json j = {
      {"access_token", info.accessToken},
      {"refresh_token", info.refreshToken},
      {"expires_at", info.expiresAtEpoch},
      {"name", info.name},
      {"handle", info.handle},
      {"photo_url", info.photoUrl}
  };
  std::ofstream f("/config/Switube/auth.json", std::ios::trunc);
  if (f.is_open()) {
    f << j.dump(2);
  }
}

void YtMusicClient::logout() {
  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex_);
    account_ = YtAccountInfo{};
    authCookies_.clear();
  }
  std::remove("/config/Switube/auth.json");
  std::remove("sdmc:/config/Switube/auth.json");
  nxlog::push("YtMusicClient: signed out and cleared auth.json");
}

void YtMusicClient::ensureVisitorData() {
  if (!visitorData_.empty()) return;

  CURL* curl = curl_easy_init();
  if (!curl) return;

  std::string responseStr;
  char errorBuffer[CURL_ERROR_SIZE] = {0};

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
  const char* url = "https://www.youtube.com/sw.js_data";
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
  switube::native::configure(curl, switube::native::cancel);

  nxlog::push("ensureVisitorData: fetching fresh visitorData from sw.js_data...");
  CURLcode res = switchu::tls::perform(curl, url, switchu::tls::kHomeCerts, [&] { responseStr.clear(); });
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK && !responseStr.empty()) {
    size_t pos = 0;
    while ((pos = responseStr.find("Cg", pos)) != std::string::npos) {
      size_t end = pos + 2;
      while (end < responseStr.size() &&
             (isalnum(static_cast<unsigned char>(responseStr[end])) ||
              responseStr[end] == '_' || responseStr[end] == '-' || responseStr[end] == '%')) {
        end++;
      }
      if (end - pos >= 40) {
        visitorData_ = responseStr.substr(pos, end - pos);
        nxlog::pushf("ensureVisitorData: acquired visitorData (%zu chars)", visitorData_.size());
        return;
      }
      pos = end;
    }
  }
}

std::string YtMusicClient::postInnerTube(const std::string& endpoint,
                                         const std::string& jsonBody,
                                         const std::string& baseHost,
                                         const std::string& userAgentOverride,
                                         bool sendOrigin) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    nxlog::push("postInnerTube: curl_easy_init failed!");
    return "";
  }

  const bool isTvClient = (jsonBody.find("\"TVHTML5\"") != std::string::npos);
  const std::string effectiveHost = isTvClient ? "https://www.youtube.com" : baseHost;
  std::string responseStr;
  std::string url = (endpoint.rfind("http", 0) == 0) ? endpoint : (effectiveHost + "/youtubei/v1/" + endpoint);
  char errorBuffer[CURL_ERROR_SIZE] = {0};

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  std::string ua = userAgentOverride.empty()
      ? (isTvClient
             ? "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version"
             : "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36")
      : userAgentOverride;
  std::string uaHdr = "User-Agent: " + ua;
  headers = curl_slist_append(headers, uaHdr.c_str());

  if (isTvClient) {
    headers = curl_slist_append(headers, "Origin: https://www.youtube.com");
    headers = curl_slist_append(headers, "Referer: https://www.youtube.com/tv");

    bool needsRefresh = false;
    std::string bearerToken;
    {
      std::lock_guard<std::recursive_mutex> lock(clientMutex_);
      if (account_.signedIn && !account_.refreshToken.empty()) {
        int64_t nowEpoch = static_cast<int64_t>(std::time(nullptr));
        if (account_.accessToken.empty() || nowEpoch >= account_.expiresAtEpoch - 120) {
          needsRefresh = true;
        } else {
          bearerToken = account_.accessToken;
        }
      }
    }
    if (needsRefresh) {
      if (refreshAccessToken()) {
        std::lock_guard<std::recursive_mutex> lock(clientMutex_);
        bearerToken = account_.accessToken;
      }
    }
    if (!bearerToken.empty()) {
      std::string authHdr = "Authorization: Bearer " + bearerToken;
      headers = curl_slist_append(headers, authHdr.c_str());
    }
  } else if (sendOrigin) {
    std::string originHdr = "Origin: " + baseHost;
    std::string refererHdr = "Referer: " + baseHost + "/";
    headers = curl_slist_append(headers, originHdr.c_str());
    headers = curl_slist_append(headers, refererHdr.c_str());
  }

  std::string cookiesCopy;
  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex_);
    cookiesCopy = authCookies_;
  }
  if (!cookiesCopy.empty() && !isTvClient) {
    std::string cookieHdr = "Cookie: " + cookiesCopy;
    headers = curl_slist_append(headers, cookieHdr.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.length()));

  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);

  switube::native::configure(curl, switube::native::cancel);

  nxlog::pushf("postInnerTube: performing POST to %s...", endpoint.c_str());
  const CURLcode res = switchu::tls::perform(curl, url, switchu::tls::kHomeCerts, [&] { responseStr.clear(); });

  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    nxlog::pushf("postInnerTube failed (code %d): %s (detail: %s)",
                 static_cast<int>(res), curl_easy_strerror(res), errorBuffer);
    return "";
  }

  nxlog::pushf("postInnerTube success: HTTP %ld, %zu bytes received", httpCode, responseStr.size());

  // Capturar visitorData de la respuesta si aún no la tenemos (sin parsear todo el JSON para no agotar el heap)
  if (visitorData_.empty() && responseStr.size() > 50) {
    size_t vpos = responseStr.find("\"visitorData\":\"");
    if (vpos != std::string::npos) {
      vpos += 15;
      size_t vend = responseStr.find('"', vpos);
      if (vend != std::string::npos && vend > vpos) {
        visitorData_ = responseStr.substr(vpos, vend - vpos);
        nxlog::pushf("postInnerTube: captured visitorData (%zu chars)", visitorData_.size());
      }
    }
  }

  return responseStr;
}

bool YtMusicClient::resolveAudioStream(const std::string& videoId, AudioStreamUrl& outStream) {
  if (videoId.empty() || videoId.size() != 11) {
    nxlog::pushf("resolveAudioStream rejected: '%s' is not an 11-char videoId", videoId.c_str());
    return false;
  }

  auto attemptResolve = [this, &videoId, &outStream]() -> bool {
    ensureVisitorData();

    json clientObj = {
        {"clientName", "VISIONOS"},
        {"clientVersion", "1.02"},
        {"deviceMake", "Apple"},
        {"deviceModel", "RealityDevice17,1"},
        {"osName", "visionOS"},
        {"osVersion", "26.5.23O471"},
        {"hl", "en"}, {"gl", "US"}
    };

    if (!visitorData_.empty()) {
      clientObj["visitorData"] = visitorData_;
    }

    json payload = {
        {"context", {
            {"client", clientObj}
        }},
        {"videoId", videoId},
        {"contentCheckOk", true},
        {"racyCheckOk", true}
    };

    std::string visionUa = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
    std::string resp = postInnerTube("player", payload.dump(), "https://www.youtube.com", visionUa, false);
    if (resp.empty()) return false;

    try {
      auto j = json::parse(resp);
      if (!j.contains("streamingData")) {
        std::string status = j.value("playabilityStatus", json::object()).value("status", "UNKNOWN");
        std::string reason = j.value("playabilityStatus", json::object()).value("reason", "");
        nxlog::pushf("resolveAudioStream: no streamingData (status: %s, reason: %s)", status.c_str(), reason.c_str());
        return false;
      }

      const auto& streamingData = j["streamingData"];
      if (!streamingData.contains("adaptiveFormats")) {
        nxlog::push("resolveAudioStream: streamingData does not contain adaptiveFormats");
        return false;
      }

      const auto& formats = streamingData["adaptiveFormats"];

      // Prioridad 1: Opus (itag 251)
      for (const auto& fmt : formats) {
        if (fmt.contains("mimeType") && fmt.contains("url")) {
          std::string mime = fmt["mimeType"];
          if (mime.find("audio/webm; codecs=\"opus\"") != std::string::npos) {
            outStream.url = fmt["url"];
            outStream.mimeType = mime;
            outStream.codec = "opus";
            outStream.bitrate = fmt.value("bitrate", 160000);
            if (fmt.contains("approxDurationMs")) {
              std::string durStr = fmt["approxDurationMs"];
              outStream.approxDurationMs = static_cast<uint32_t>(std::stoul(durStr));
            }
            if (fmt.contains("contentLength")) {
              try {
                outStream.contentLength = std::stoull(fmt["contentLength"].get<std::string>());
              } catch (...) {}
            }
            outStream.isDirect = true;
            nxlog::pushf("resolveAudioStream SUCCESS: direct Opus URL found (bitrate: %d, bytes: %llu)",
                         outStream.bitrate, static_cast<unsigned long long>(outStream.contentLength));
            return true;
          }
        }
      }

      // Prioridad 2: AAC (itag 140 - mp4a)
      for (const auto& fmt : formats) {
        if (fmt.contains("mimeType") && fmt.contains("url")) {
          std::string mime = fmt["mimeType"];
          if (mime.find("audio/mp4; codecs=\"mp4a") != std::string::npos) {
            outStream.url = fmt["url"];
            outStream.mimeType = mime;
            outStream.codec = "aac";
            outStream.bitrate = fmt.value("bitrate", 128000);
            if (fmt.contains("approxDurationMs")) {
              std::string durStr = fmt["approxDurationMs"];
              outStream.approxDurationMs = static_cast<uint32_t>(std::stoul(durStr));
            }
            if (fmt.contains("contentLength")) {
              try {
                outStream.contentLength = std::stoull(fmt["contentLength"].get<std::string>());
              } catch (...) {}
            }
            outStream.isDirect = true;
            nxlog::pushf("resolveAudioStream SUCCESS: direct AAC URL found (bitrate: %d, bytes: %llu)",
                         outStream.bitrate, static_cast<unsigned long long>(outStream.contentLength));
            return true;
          }
        }
      }
    } catch (const std::exception& e) {
      nxlog::pushf("resolveAudioStream parse error: %s", e.what());
      return false;
    }

    return false;
  };

  nxlog::pushf("resolveAudioStream: requesting stream for videoId '%s' (VisionOS)...", videoId.c_str());
  if (attemptResolve()) return true;

  // Si VisionOS falló, probar con cliente ANDROID_VR
  auto attemptAndroidVr = [this, &videoId, &outStream]() -> bool {
    json clientObj = {
        {"clientName", "ANDROID_VR"},
        {"clientVersion", "1.61.48"},
        {"deviceMake", "Oculus"},
        {"deviceModel", "Quest 3"},
        {"osName", "Android"},
        {"osVersion", "12"},
        {"hl", "en"}, {"gl", "US"}
    };
    if (!visitorData_.empty()) clientObj["visitorData"] = visitorData_;

    json payload = {
        {"context", {{"client", clientObj}}},
        {"videoId", videoId},
        {"contentCheckOk", true},
        {"racyCheckOk", true}
    };
    std::string vrUa = "Mozilla/5.0 (Linux; Android 12; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Mobile Safari/537.36";
    std::string resp = postInnerTube("player", payload.dump(), "https://www.youtube.com", vrUa, false);
    if (resp.empty()) return false;

    try {
      auto j = json::parse(resp);
      if (!j.contains("streamingData")) return false;
      const auto& formats = j["streamingData"]["adaptiveFormats"];
      for (const auto& fmt : formats) {
        if (fmt.contains("mimeType") && fmt.contains("url")) {
          std::string mime = fmt["mimeType"];
          if (mime.find("audio/webm; codecs=\"opus\"") != std::string::npos ||
              mime.find("audio/mp4; codecs=\"mp4a") != std::string::npos) {
            outStream.url = fmt["url"];
            outStream.mimeType = mime;
            outStream.codec = (mime.find("opus") != std::string::npos) ? "opus" : "aac";
            outStream.bitrate = fmt.value("bitrate", 128000);
            if (fmt.contains("approxDurationMs")) {
              outStream.approxDurationMs = static_cast<uint32_t>(std::stoul(fmt["approxDurationMs"].get<std::string>()));
            }
            if (fmt.contains("contentLength")) {
              try { outStream.contentLength = std::stoull(fmt["contentLength"].get<std::string>()); } catch (...) {}
            }
            outStream.isDirect = true;
            nxlog::pushf("resolveAudioStream SUCCESS: ANDROID_VR fallback stream found (bitrate: %d)", outStream.bitrate);
            return true;
          }
        }
      }
    } catch (...) {}
    return false;
  };

  nxlog::push("resolveAudioStream: trying ANDROID_VR fallback...");
  if (attemptAndroidVr()) return true;

  // Intento de recuperación final: renovar visitorData y reintentar VisionOS
  nxlog::push("resolveAudioStream: initial attempts failed, refreshing visitorData and retrying...");
  visitorData_.clear();
  ensureVisitorData();
  return attemptResolve();
}

bool YtMusicClient::getPlaylistTracks(const std::string& browseId,
                                      std::vector<YtTrack>& outTracks,
                                      std::string& outContinuationToken,
                                      const std::string& continuationToken) {
  outContinuationToken.clear();
  nxlog::pushf("getPlaylistTracks: fetching tracks for browseId '%s' (cont: %s)...",
               browseId.c_str(), continuationToken.empty() ? "NONE" : "YES");

  const bool isPersonalList = (browseId == "VLLM" || browseId == "FEmusic_liked_videos" || browseId == "LL");
  const bool isTvCont = (continuationToken.rfind("TV:", 0) == 0);
  const bool isWrCont = (continuationToken.rfind("WR:", 0) == 0);

  if (!isPersonalList && !isTvCont) {
    json clientObj = {
        {"clientName", "WEB_REMIX"},
        {"clientVersion", "1.20240101.01.00"},
        {"hl", "en"},
        {"gl", "US"}
    };
    if (!visitorData_.empty()) {
      clientObj["visitorData"] = visitorData_;
    }

    std::string endpoint = "browse";
    std::string rawWrTok;
    json payload = {{"context", {{"client", clientObj}}}};
    if (isWrCont) {
      rawWrTok = continuationToken.substr(3);
      endpoint = "browse?ctoken=" + rawWrTok + "&continuation=" + rawWrTok + "&type=next";
      payload["continuation"] = rawWrTok;
    } else if (!continuationToken.empty()) {
      rawWrTok = continuationToken;
      endpoint = "browse?ctoken=" + rawWrTok + "&continuation=" + rawWrTok + "&type=next";
      payload["continuation"] = rawWrTok;
    } else {
      payload["browseId"] = browseId;
    }

    std::string resp = postInnerTube(endpoint, payload.dump());
    if (!resp.empty()) {
      try {
        auto j = json::parse(resp);

        std::string nextTok = findContinuationToken(j);
        if (!nextTok.empty() && nextTok != rawWrTok) {
          outContinuationToken = "WR:" + nextTok;
        }

        // Extraer metadatos de cabecera del álbum / single / playlist (artista, título y portada por defecto)
        std::string defaultAlbum;
        std::string defaultArtist;
        std::string defaultThumb;

        std::function<void(const json&)> findHeaderMeta = [&](const json& node) {
          if (!node.is_object() && !node.is_array()) return;
          if (node.is_object()) {
            const json* hdr = nullptr;
            if (node.contains("musicResponsiveHeaderRenderer")) {
              hdr = &node["musicResponsiveHeaderRenderer"];
            } else if (node.contains("musicDetailHeaderRenderer")) {
              hdr = &node["musicDetailHeaderRenderer"];
            }
            if (hdr && hdr->is_object()) {
              if (defaultAlbum.empty() && hdr->contains("title") && (*hdr)["title"].contains("runs") &&
                  (*hdr)["title"]["runs"].is_array() && !(*hdr)["title"]["runs"].empty()) {
                defaultAlbum = (*hdr)["title"]["runs"][0].value("text", "");
              }
              if (defaultArtist.empty() && hdr->contains("straplineTextOne") &&
                  (*hdr)["straplineTextOne"].contains("runs") &&
                  (*hdr)["straplineTextOne"]["runs"].is_array() &&
                  !(*hdr)["straplineTextOne"]["runs"].empty()) {
                defaultArtist = (*hdr)["straplineTextOne"]["runs"][0].value("text", "");
              }
              if (defaultArtist.empty() && hdr->contains("subtitle") &&
                  (*hdr)["subtitle"].contains("runs") &&
                  (*hdr)["subtitle"]["runs"].is_array()) {
                const auto& sruns = (*hdr)["subtitle"]["runs"];
                if (sruns.size() >= 3) {
                  defaultArtist = sruns[2].value("text", "");
                } else if (!sruns.empty()) {
                  defaultArtist = sruns[0].value("text", "");
                }
              }
              if (defaultThumb.empty() && hdr->contains("thumbnail") &&
                  (*hdr)["thumbnail"].contains("musicThumbnailRenderer")) {
                const auto& mtr = (*hdr)["thumbnail"]["musicThumbnailRenderer"];
                if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                    mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                  defaultThumb = mtr["thumbnail"]["thumbnails"].back().value("url", "");
                }
              }
            }
            for (auto it = node.begin(); it != node.end(); ++it) {
              if (it.key() == "trackingParams") continue;
              findHeaderMeta(it.value());
            }
          } else if (node.is_array()) {
            for (const auto& item : node) {
              findHeaderMeta(item);
            }
          }
        };
        findHeaderMeta(j);

        std::function<void(const json&)> findTrackRows = [&](const json& node) {
          if (node.is_object()) {
            if (node.contains("musicResponsiveListItemRenderer")) {
              const auto& row = node["musicResponsiveListItemRenderer"];
              std::string vid = findVideoId(row);
              if (!vid.empty() && vid.size() == 11) {
                YtTrack t;
                t.id = vid;
                t.isPlaylist = false;
                t.isArtist = false;
                if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
                  const auto& col0 = row["flexColumns"][0];
                  if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
                    const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
                    if (flex0.contains("text") && flex0["text"].contains("runs") &&
                        flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                      t.title = flex0["text"]["runs"][0].value("text", "Unknown");
                    }
                  }
                  if (row["flexColumns"].size() > 1) {
                    const auto& col1 = row["flexColumns"][1];
                    if (col1.contains("musicResponsiveListItemFlexColumnRenderer")) {
                      const auto& flex1 = col1["musicResponsiveListItemFlexColumnRenderer"];
                      if (flex1.contains("text") && flex1["text"].contains("runs") &&
                          flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                        const auto& runs = flex1["text"]["runs"];
                        t.artist = runs[0].value("text", "");
                        if (runs.size() >= 3) t.album = runs[2].value("text", "");
                      }
                    }
                  }
                }
                if (t.artist.empty()) t.artist = defaultArtist.empty() ? "Unknown Artist" : defaultArtist;
                if (t.album.empty() && !defaultAlbum.empty()) t.album = defaultAlbum;

                if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
                  const auto& mtr = row["thumbnail"]["musicThumbnailRenderer"];
                  if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                      mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                    t.imageUrl = mtr["thumbnail"]["thumbnails"].back().value("url", "");
                  }
                }
                if (t.imageUrl.empty() && !defaultThumb.empty()) {
                  t.imageUrl = defaultThumb;
                }

                // Extraer duración precisa
                extractDurationFromRow(row, t);

                outTracks.push_back(t);
              }
            } else {
              for (auto it = node.begin(); it != node.end(); ++it) {
                if (it.key() == "trackingParams") continue;
                findTrackRows(it.value());
              }
            }
          } else if (node.is_array()) {
            for (const auto& item : node) {
              findTrackRows(item);
            }
          }
        };

        findTrackRows(j);
      } catch (const std::exception& e) {
        nxlog::pushf("getPlaylistTracks WEB_REMIX error: %s", e.what());
      }
    }
  }

  // Fallback o resolución autenticada mediante TVHTML5 (para listas personales, VLLM, etc.)
  if (outTracks.empty()) {
    outContinuationToken.clear();
    std::string tvBrowseId = (browseId == "FEmusic_liked_videos") ? "VLLM" : browseId;
    if (tvBrowseId.rfind("PL", 0) == 0 || tvBrowseId.rfind("OL", 0) == 0 || tvBrowseId == "LL") {
      tvBrowseId = "VL" + tvBrowseId;
    }

    json tvPayload = {
        {"context", {
            {"client", {
                {"clientName", "TVHTML5"},
                {"clientVersion", "7.20250101.00.00"},
                {"hl", "en"},
                {"gl", "US"}
            }}
        }}
    };

    std::string rawTvTok;
    if (isTvCont) {
      rawTvTok = continuationToken.substr(3);
      tvPayload["continuation"] = rawTvTok;
    } else if (!continuationToken.empty() && !isWrCont) {
      rawTvTok = continuationToken;
      tvPayload["continuation"] = rawTvTok;
    } else {
      tvPayload["browseId"] = tvBrowseId;
    }

    std::string tvResp = postInnerTube("browse", tvPayload.dump());
    if (!tvResp.empty()) {
      try {
        auto tj = json::parse(tvResp);
        std::string nextTok = findContinuationToken(tj);
        if (!nextTok.empty() && nextTok != rawTvTok) {
          outContinuationToken = "TV:" + nextTok;
        }

        std::function<void(const json&)> findTvTiles = [&](const json& node) {
          if (node.is_object()) {
            if (node.contains("tileRenderer")) {
              YtTrack t;
              if (parseTvTile(node["tileRenderer"], t) && !t.isPlaylist && !t.isArtist && t.id.size() == 11) {
                outTracks.push_back(t);
              }
              return;
            }
            for (auto it = node.begin(); it != node.end(); ++it) {
              if (it.key() == "trackingParams") continue;
              findTvTiles(it.value());
            }
          } else if (node.is_array()) {
            for (const auto& elem : node) {
              findTvTiles(elem);
            }
          }
        };
        findTvTiles(tj);
      } catch (const std::exception& e) {
        nxlog::pushf("getPlaylistTracks TVHTML5 error: %s", e.what());
      }
    }
  }

  nxlog::pushf("getPlaylistTracks: extracted %zu tracks from '%s' (hasMore: %s)",
               outTracks.size(), browseId.c_str(), outContinuationToken.empty() ? "NO" : "YES");
  return !outTracks.empty();
}

bool YtMusicClient::getArtistPage(const std::string& artistBrowseId, YtArtistPage& outPage) {
  outPage.id = artistBrowseId;
  nxlog::pushf("getArtistPage: fetching artist profile for '%s'...", artistBrowseId.c_str());

  json payload = {
      {"context", {
          {"client", {
              {"clientName", "WEB_REMIX"},
              {"clientVersion", "1.20240101.01.00"},
              {"hl", "en"}, {"gl", "US"}
          }}
      }},
      {"browseId", artistBrowseId}
  };

  std::string resp = postInnerTube("browse", payload.dump());
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);

    // 1. Cabecera (Header: nombre, foto, suscriptores)
    const json* headerObj = nullptr;
    if (j.contains("header")) {
      const auto& h = j["header"];
      if (h.contains("musicImmersiveHeaderRenderer")) {
        headerObj = &h["musicImmersiveHeaderRenderer"];
      } else if (h.contains("musicVisualHeaderRenderer")) {
        headerObj = &h["musicVisualHeaderRenderer"];
      } else if (h.contains("musicHeaderRenderer")) {
        headerObj = &h["musicHeaderRenderer"];
      }
    }

    if (headerObj) {
      if (headerObj->contains("title") && (*headerObj)["title"].contains("runs") && !(*headerObj)["title"]["runs"].empty()) {
        outPage.name = (*headerObj)["title"]["runs"][0].value("text", "");
      }
      if (headerObj->contains("description") && (*headerObj)["description"].contains("runs") && !(*headerObj)["description"]["runs"].empty()) {
        outPage.description = (*headerObj)["description"]["runs"][0].value("text", "");
      }

      const json* thumbNode = nullptr;
      if (headerObj->contains("thumbnail")) thumbNode = &(*headerObj)["thumbnail"];
      else if (headerObj->contains("foregroundThumbnail")) thumbNode = &(*headerObj)["foregroundThumbnail"];

      if (thumbNode && thumbNode->contains("musicThumbnailRenderer")) {
        const auto& thumbs = (*thumbNode)["musicThumbnailRenderer"]["thumbnail"]["thumbnails"];
        if (!thumbs.empty()) {
          outPage.thumbnailUrl = swui::upgradeThumbnailUrl(thumbs.back().value("url", ""), 544);
        }
      }

      if (headerObj->contains("subscriptionButton") && (*headerObj)["subscriptionButton"].contains("subscribeButtonRenderer")) {
        const auto& btn = (*headerObj)["subscriptionButton"]["subscribeButtonRenderer"];
        if (btn.contains("subscriberCountText") && btn["subscriberCountText"].contains("runs") && !btn["subscriberCountText"]["runs"].empty()) {
          outPage.subscriberCount = btn["subscriberCountText"]["runs"][0].value("text", "");
        } else if (btn.contains("subscriberCountWithSubscribeText") && btn["subscriberCountWithSubscribeText"].contains("runs") && !btn["subscriberCountWithSubscribeText"]["runs"].empty()) {
          outPage.subscriberCount = btn["subscriberCountWithSubscribeText"]["runs"][0].value("text", "");
        }
      }
    }

    // 2. Secciones
    if (!j.contains("contents")) return !outPage.name.empty();
    const auto& singleCol = j["contents"].value("singleColumnBrowseResultsRenderer", json::object());
    if (!singleCol.contains("tabs") || singleCol["tabs"].empty()) return !outPage.name.empty();

    const auto& tab = singleCol["tabs"][0].value("tabRenderer", json::object());
    const auto& contentObj = tab.value("content", json::object());
    const auto& sectionList = contentObj.value("sectionListRenderer", json::object());
    const auto& sections = sectionList.value("contents", json::array());

    for (const auto& sec : sections) {
      // 2a. Canciones principales (musicShelfRenderer)
      if (sec.contains("musicShelfRenderer")) {
        const auto& shelf = sec["musicShelfRenderer"];
        if (shelf.contains("bottomEndpoint") && shelf["bottomEndpoint"].contains("browseEndpoint")) {
          outPage.allSongsBrowseId = shelf["bottomEndpoint"]["browseEndpoint"].value("browseId", "");
        } else if (shelf.contains("title") && shelf["title"].contains("runs") && !shelf["title"]["runs"].empty()) {
          const auto& run0 = shelf["title"]["runs"][0];
          if (run0.contains("navigationEndpoint") && run0["navigationEndpoint"].contains("browseEndpoint")) {
            outPage.allSongsBrowseId = run0["navigationEndpoint"]["browseEndpoint"].value("browseId", "");
          }
        }

        if (shelf.contains("contents")) {
          for (const auto& item : shelf["contents"]) {
            if (!item.contains("musicResponsiveListItemRenderer")) continue;
            const auto& row = item["musicResponsiveListItemRenderer"];
            std::string vid = findVideoId(row);
            if (vid.empty() || vid.size() != 11) continue;

            YtTrack t;
            t.id = vid;
            t.artist = outPage.name;
            t.isPlaylist = false;
            t.isArtist = false;

            if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
              const auto& col0 = row["flexColumns"][0];
              if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
                const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
                if (flex0.contains("text") && flex0["text"].contains("runs") &&
                    flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                  t.title = flex0["text"]["runs"][0].value("text", "Unknown");
                }
              }
              if (row["flexColumns"].size() > 1) {
                const auto& col1 = row["flexColumns"][1];
                if (col1.contains("musicResponsiveListItemFlexColumnRenderer")) {
                  const auto& flex1 = col1["musicResponsiveListItemFlexColumnRenderer"];
                  if (flex1.contains("text") && flex1["text"].contains("runs") &&
                      flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                    const auto& runs = flex1["text"]["runs"];
                    t.artist = runs[0].value("text", outPage.name);
                    if (runs.size() >= 3) t.album = runs[2].value("text", "");
                  }
                }
              }
            }

            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              const auto& mtr = row["thumbnail"]["musicThumbnailRenderer"];
              if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                  mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                t.imageUrl = mtr["thumbnail"]["thumbnails"].back().value("url", "");
              }
            }

            extractDurationFromRow(row, t);
            outPage.topSongs.push_back(t);
          }
        }
      }
      // 2b. Carruseles (Álbumes, Singles, Vídeos, Directo, Destacado en, Listas, Similares)
      else if (sec.contains("musicCarouselShelfRenderer")) {
        const auto& carousel = sec["musicCarouselShelfRenderer"];
        YtShelf shelf;

        if (carousel.contains("header") && carousel["header"].contains("musicCarouselShelfBasicHeaderRenderer")) {
          const auto& hdr = carousel["header"]["musicCarouselShelfBasicHeaderRenderer"];
          if (hdr.contains("title") && hdr["title"].contains("runs") &&
              hdr["title"]["runs"].is_array() && !hdr["title"]["runs"].empty()) {
            shelf.title = hdr["title"]["runs"][0].value("text", "");
          }
        }
        if (shelf.title.empty()) continue;

        if (carousel.contains("contents") && carousel["contents"].is_array()) {
          for (const auto& citem : carousel["contents"]) {
            const json* rowPtr = nullptr;
            if (citem.contains("musicTwoRowItemRenderer")) {
              rowPtr = &citem["musicTwoRowItemRenderer"];
            } else if (citem.contains("musicResponsiveListItemRenderer")) {
              rowPtr = &citem["musicResponsiveListItemRenderer"];
            }
            if (!rowPtr || !rowPtr->is_object()) continue;
            const auto& row = *rowPtr;

            YtTrack track;
            if (row.contains("title") && row["title"].contains("runs") &&
                row["title"]["runs"].is_array() && !row["title"]["runs"].empty()) {
              track.title = row["title"]["runs"][0].value("text", "");
            }
            if (row.contains("subtitle") && row["subtitle"].contains("runs") &&
                row["subtitle"]["runs"].is_array() && !row["subtitle"]["runs"].empty()) {
              std::string sub;
              for (const auto& r : row["subtitle"]["runs"]) {
                sub += r.value("text", "");
              }
              track.artist = sub;
            }

            const json* thumbObj = nullptr;
            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              thumbObj = &row["thumbnail"]["musicThumbnailRenderer"];
            } else if (row.contains("thumbnailRenderer") && row["thumbnailRenderer"].contains("musicThumbnailRenderer")) {
              thumbObj = &row["thumbnailRenderer"]["musicThumbnailRenderer"];
            }
            if (thumbObj && (*thumbObj).contains("thumbnail") && (*thumbObj)["thumbnail"].contains("thumbnails")) {
              const auto& thumbs = (*thumbObj)["thumbnail"]["thumbnails"];
              if (!thumbs.empty()) track.imageUrl = thumbs.back().value("url", "");
            }

            extractDurationFromRow(row, track);

            std::string vid;
            if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("watchEndpoint")) {
              vid = row["navigationEndpoint"]["watchEndpoint"].value("videoId", "");
            }
            if (vid.empty() && row.contains("playlistItemData")) {
              vid = row["playlistItemData"].value("videoId", "");
            }

            std::string bid;
            std::string pageType;
            if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("browseEndpoint")) {
              const auto& bep = row["navigationEndpoint"]["browseEndpoint"];
              bid = bep.value("browseId", "");
              if (bep.contains("browseEndpointContextSupportedConfigs")) {
                const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
                if (cfg.contains("browseEndpointContextMusicConfig")) {
                  pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
                }
              }
            }

            if (!vid.empty() && vid.size() == 11) {
              track.id = vid;
              track.isPlaylist = false;
              track.isArtist = false;
            } else if (!bid.empty()) {
              track.id = bid;
              if (pageType.find("ARTIST") != std::string::npos || (bid.rfind("UC", 0) == 0 && vid.empty())) {
                track.isArtist = true;
                track.isPlaylist = false;
              } else {
                track.isPlaylist = true;
                track.isArtist = false;
              }
            } else {
              continue;
            }

            if (!track.id.empty() && !track.title.empty()) {
              shelf.tracks.push_back(track);
            }
          }
        }

        if (!shelf.tracks.empty()) {
          outPage.sections.push_back(shelf);
        }
      }
    }

    nxlog::pushf("getArtistPage: artist '%s' loaded (topSongs: %zu, sections: %zu)",
                 outPage.name.c_str(), outPage.topSongs.size(), outPage.sections.size());
    return true;
  } catch (const std::exception& e) {
    nxlog::pushf("getArtistPage parse error: %s", e.what());
    return false;
  }
}

bool YtMusicClient::searchTracks(const std::string& query,
                                 YtSearchFilter filter,
                                 std::vector<YtTrack>& outTracks,
                                 std::string& outContinuationToken,
                                 const std::string& continuationToken,
                                 size_t limit) {
  outContinuationToken.clear();

  if (continuationToken == "ALL_FALLBACK_SONGS") {
    return searchTracks(query, YtSearchFilter::Songs, outTracks, outContinuationToken, "", limit);
  }

  std::string endpoint = "search";
  json payload = {
      {"context", {
          {"client", {
              {"clientName", "WEB_REMIX"},
              {"clientVersion", "1.20240101.01.00"},
              {"hl", "en"}, {"gl", "US"}
          }}
      }}
  };

  if (continuationToken.empty()) {
    payload["query"] = query;

    // BitChord exact InnerTube filter params
    std::string params = "";
    switch (filter) {
      case YtSearchFilter::Songs:
        params = "EgWKAQIIAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Videos:
        params = "EgWKAQIQAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Albums:
        params = "EgWKAQIYAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Artists:
        params = "EgWKAQIgAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Playlists:
        params = "EgWKAQIoAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::All:
      default:
        break;
    }
    if (!params.empty()) {
      payload["params"] = params;
    }
    nxlog::pushf("searchTracks: querying '%s' (filter: %d)...", query.c_str(), static_cast<int>(filter));
  } else {
    endpoint = "search?continuation=" + continuationToken + "&ctoken=" + continuationToken + "&type=next";
    payload["continuation"] = continuationToken;
    nxlog::pushf("searchTracks: fetching next page with continuation token (%zu chars)...", continuationToken.size());
  }

  std::string resp = postInnerTube(endpoint, payload.dump());
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);

    // Extraer token de continuación si existe
    std::string nextTok = findContinuationToken(j);
    if (!nextTok.empty() && nextTok != continuationToken) {
      outContinuationToken = nextTok;
      nxlog::pushf("searchTracks: found next continuationToken (%zu chars)", outContinuationToken.size());
    }

    std::function<void(const json&)> findSearchTracks = [&](const json& node) {
      if (outTracks.size() >= limit) return;
      if (node.is_object()) {
        if (node.contains("musicResponsiveListItemRenderer")) {
          const auto& row = node["musicResponsiveListItemRenderer"];
          std::string bid;
          std::string pageType;
          if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("browseEndpoint")) {
            const auto& bep = row["navigationEndpoint"]["browseEndpoint"];
            bid = bep.value("browseId", "");
            if (bep.contains("browseEndpointContextSupportedConfigs")) {
              const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
              if (cfg.contains("browseEndpointContextMusicConfig")) {
                pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
              }
            }
          }
          if (bid.empty() && row.contains("flexColumns") && !row["flexColumns"].empty()) {
            const auto& col0 = row["flexColumns"][0];
            if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
              const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
              if (flex0.contains("text") && flex0["text"].contains("runs") && !flex0["text"]["runs"].empty()) {
                const auto& r0 = flex0["text"]["runs"][0];
                if (r0.contains("navigationEndpoint") && r0["navigationEndpoint"].contains("browseEndpoint")) {
                  const auto& bep = r0["navigationEndpoint"]["browseEndpoint"];
                  bid = bep.value("browseId", "");
                  if (bep.contains("browseEndpointContextSupportedConfigs")) {
                    const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
                    if (cfg.contains("browseEndpointContextMusicConfig")) {
                      pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
                    }
                  }
                }
              }
            }
          }

          const bool forceBrowseFilter =
              (filter == YtSearchFilter::Albums ||
               filter == YtSearchFilter::Artists ||
               filter == YtSearchFilter::Playlists);
          if (bid.empty() && forceBrowseFilter) {
            bid = findBrowseId(row);
          }

          std::string vid;
          if (bid.empty() && !forceBrowseFilter) {
            if (row.contains("playlistItemData") && row["playlistItemData"].is_object()) {
              vid = row["playlistItemData"].value("videoId", "");
            }
            if (vid.empty() && row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("watchEndpoint")) {
              vid = row["navigationEndpoint"]["watchEndpoint"].value("videoId", "");
            }
            if (vid.empty()) {
              vid = findVideoId(row);
            }
          }

          YtTrack track;
          if (!bid.empty() || forceBrowseFilter) {
            if (bid.empty()) bid = findBrowseId(row);
            track.id = bid;
            if (filter == YtSearchFilter::Artists ||
                pageType.find("ARTIST") != std::string::npos ||
                pageType.find("USER_CHANNEL") != std::string::npos ||
                bid.rfind("UC", 0) == 0) {
              track.isArtist = true;
              track.isPlaylist = false;
            } else {
              track.isPlaylist = true;
              track.isArtist = false;
            }
          } else if (!vid.empty() && vid.size() == 11) {
            track.id = vid;
            track.isPlaylist = false;
            track.isArtist = false;
          } else {
            std::string fallbackBid = findBrowseId(row);
            if (!fallbackBid.empty()) {
              track.id = fallbackBid;
              if (fallbackBid.rfind("UC", 0) == 0) {
                track.isArtist = true;
                track.isPlaylist = false;
              } else {
                track.isPlaylist = true;
                track.isArtist = false;
              }
            }
          }

          if (!track.id.empty()) {
            if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
              const auto& col0 = row["flexColumns"][0];
              if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
                const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
                if (flex0.contains("text") && flex0["text"].contains("runs") &&
                    flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                  track.title = flex0["text"]["runs"][0].value("text", "Unknown");
                }
              }
              if (row["flexColumns"].size() > 1) {
                const auto& col1 = row["flexColumns"][1];
                if (col1.contains("musicResponsiveListItemFlexColumnRenderer")) {
                  const auto& flex1 = col1["musicResponsiveListItemFlexColumnRenderer"];
                  if (flex1.contains("text") && flex1["text"].contains("runs") &&
                      flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                    const auto& runs = flex1["text"]["runs"];
                    if (track.isPlaylist || track.isArtist || filter == YtSearchFilter::All) {
                      std::string sub;
                      for (const auto& r : runs) {
                        sub += r.value("text", "");
                      }
                      track.artist = sub;
                    } else {
                      const auto type = runs.front().value("text", "");
                      const bool typePrefix = runs.size() > 2 && runs[1].value("text", "") == " • " &&
                          (type == "Song" || type == "Video");
                      auto artist = typePrefix ? 2u : 0u;
                      for (; artist < runs.size(); ++artist) {
                        const auto text = runs[artist].value("text", "");
                        if (text == " • ") break;
                        track.artist += text;
                      }
                      if (artist + 1 < runs.size()) track.album = runs[artist + 1].value("text", "");
                    }
                  }
                }
              }
            }

            if (track.title.empty() && row.contains("title") && row["title"].contains("runs") &&
                row["title"]["runs"].is_array() && !row["title"]["runs"].empty()) {
              track.title = row["title"]["runs"][0].value("text", "");
            }
            if (track.artist.empty() && row.contains("subtitle") && row["subtitle"].contains("runs") &&
                row["subtitle"]["runs"].is_array() && !row["subtitle"]["runs"].empty()) {
              std::string sub;
              for (const auto& r : row["subtitle"]["runs"]) {
                sub += r.value("text", "");
              }
              track.artist = sub;
            }

            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              const auto& mtr = row["thumbnail"]["musicThumbnailRenderer"];
              if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                  mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                track.imageUrl = mtr["thumbnail"]["thumbnails"].front().value("url", "");
              }
            }

            // Extraer duración
            extractDurationFromRow(row, track);

            outTracks.push_back(track);
          }
        } else {
          for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "trackingParams") continue;
            findSearchTracks(it.value());
          }
        }
      } else if (node.is_array()) {
        for (const auto& elem : node) {
          findSearchTracks(elem);
        }
      }
    };

    findSearchTracks(j);
    if (outContinuationToken.empty() && continuationToken.empty() &&
        filter == YtSearchFilter::All && !outTracks.empty()) {
      outContinuationToken = "ALL_FALLBACK_SONGS";
    }
    nxlog::pushf("searchTracks: parsed %zu tracks (hasMore: %s)",
                 outTracks.size(), outContinuationToken.empty() ? "NO" : "YES");
    return !outTracks.empty();
  } catch (const std::exception& e) {
    nxlog::pushf("searchTracks parse error: %s", e.what());
    return false;
  }
}

bool YtMusicClient::getHomeFeed(std::vector<YtShelf>& outShelves,
                                std::string& outContinuationToken,
                                const std::string& continuationToken) {
  outContinuationToken.clear();
  auto parseCarouselSections = [this, &outShelves](const json& sections) {
    if (!sections.is_array()) return;
    for (const auto& sec : sections) {
      if (!sec.is_object() || !sec.contains("musicCarouselShelfRenderer")) continue;
      const auto& carousel = sec["musicCarouselShelfRenderer"];
      YtShelf shelf;

      if (carousel.contains("header") && carousel["header"].contains("musicCarouselShelfBasicHeaderRenderer")) {
        const auto& hdr = carousel["header"]["musicCarouselShelfBasicHeaderRenderer"];
        if (hdr.contains("title") && hdr["title"].contains("runs") &&
            hdr["title"]["runs"].is_array() && !hdr["title"]["runs"].empty()) {
          shelf.title = hdr["title"]["runs"][0].value("text", "");
        }
      }
      if (shelf.title.empty() ||
          shelf.title.find("nimo") != std::string::npos ||
          shelf.title.find("neros") != std::string::npos ||
          shelf.title.find("Moods") != std::string::npos ||
          shelf.title.find("Genres") != std::string::npos ||
          shelf.title.find("pisodios") != std::string::npos) {
        continue;
      }

      // Evitar secciones duplicadas
      bool exists = false;
      for (const auto& s : outShelves) {
        if (s.title == shelf.title) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      if (carousel.contains("contents") && carousel["contents"].is_array()) {
        for (const auto& item : carousel["contents"]) {
          const json* rowPtr = nullptr;
          if (item.contains("musicTwoRowItemRenderer")) {
            rowPtr = &item["musicTwoRowItemRenderer"];
          } else if (item.contains("musicResponsiveListItemRenderer")) {
            rowPtr = &item["musicResponsiveListItemRenderer"];
          }

          if (!rowPtr || !rowPtr->is_object()) continue;
          const auto& row = *rowPtr;
          YtTrack track;

          // 1. Verificar primero navigationEndpoint.browseEndpoint (Álbumes, Singles, Playlists, Artistas)
          std::string bid;
          std::string pageType;
          if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("browseEndpoint")) {
            const auto& bep = row["navigationEndpoint"]["browseEndpoint"];
            bid = bep.value("browseId", "");
            if (bep.contains("browseEndpointContextSupportedConfigs")) {
              const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
              if (cfg.contains("browseEndpointContextMusicConfig")) {
                pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
              }
            }
          }

          // Ignorar categorías de ánimo/géneros y episodios de podcast no musicales
          if (bid == "FEmusic_moods_and_genres_category" ||
              bid.rfind("MPED", 0) == 0 ||
              bid.rfind("MPSP", 0) == 0) {
            continue;
          }

          std::string vid;
          if (bid.empty()) {
            if (row.contains("playlistItemData") && row["playlistItemData"].is_object()) {
              vid = row["playlistItemData"].value("videoId", "");
            }
            if (vid.empty() && row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("watchEndpoint")) {
              vid = row["navigationEndpoint"]["watchEndpoint"].value("videoId", "");
            }
            if (vid.empty()) {
              vid = findVideoId(row);
            }
          }

          if (!bid.empty()) {
            track.id = bid;
            if (pageType.find("ARTIST") != std::string::npos ||
                pageType.find("USER_CHANNEL") != std::string::npos ||
                bid.rfind("UC", 0) == 0) {
              track.isArtist = true;
              track.isPlaylist = false;
            } else {
              track.isPlaylist = true;
              track.isArtist = false;
            }
          } else if (!vid.empty() && vid.size() == 11) {
            track.id = vid;
            track.isPlaylist = false;
            track.isArtist = false;
          } else {
            std::string fallbackBid = findBrowseId(row);
            if (fallbackBid.empty() ||
                fallbackBid == "FEmusic_moods_and_genres_category" ||
                fallbackBid.rfind("MPED", 0) == 0 ||
                fallbackBid.rfind("MPSP", 0) == 0) {
              continue;
            }
            track.id = fallbackBid;
            if (fallbackBid.rfind("UC", 0) == 0) {
              track.isArtist = true;
              track.isPlaylist = false;
            } else {
              track.isPlaylist = true;
              track.isArtist = false;
            }
          }

          // 2. Title & Artist (flexColumns para musicResponsiveListItemRenderer)
          if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
            const auto& flex = row["flexColumns"];
            if (flex.size() > 0 && flex[0].contains("musicResponsiveListItemFlexColumnRenderer")) {
              const auto& flex0 = flex[0]["musicResponsiveListItemFlexColumnRenderer"];
              if (flex0.contains("text") && flex0["text"].contains("runs") &&
                  flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                track.title = flex0["text"]["runs"][0].value("text", "");
              }
            }
            if (flex.size() > 1 && flex[1].contains("musicResponsiveListItemFlexColumnRenderer")) {
              const auto& flex1 = flex[1]["musicResponsiveListItemFlexColumnRenderer"];
              if (flex1.contains("text") && flex1["text"].contains("runs") &&
                  flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                track.artist = flex1["text"]["runs"][0].value("text", "");
              }
            }
          }

          // Fallback a title / subtitle runs (musicTwoRowItemRenderer)
          if (track.title.empty() && row.contains("title") && row["title"].contains("runs") &&
              row["title"]["runs"].is_array() && !row["title"]["runs"].empty()) {
            track.title = row["title"]["runs"][0].value("text", "");
          }
          if (track.artist.empty() && row.contains("subtitle") && row["subtitle"].contains("runs") &&
              row["subtitle"]["runs"].is_array() && !row["subtitle"]["runs"].empty()) {
            std::string sub;
            for (const auto& r : row["subtitle"]["runs"]) {
              sub += r.value("text", "");
            }
            track.artist = sub;
          }

          // 3. Miniatura / Carátula (thumbnail y thumbnailRenderer)
          const json* thumbObj = nullptr;
          if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
            thumbObj = &row["thumbnail"]["musicThumbnailRenderer"];
          } else if (row.contains("thumbnailRenderer") && row["thumbnailRenderer"].contains("musicThumbnailRenderer")) {
            thumbObj = &row["thumbnailRenderer"]["musicThumbnailRenderer"];
          }

          if (thumbObj && thumbObj->contains("thumbnail") && (*thumbObj)["thumbnail"].contains("thumbnails") &&
              (*thumbObj)["thumbnail"]["thumbnails"].is_array() && !(*thumbObj)["thumbnail"]["thumbnails"].empty()) {
            track.imageUrl = (*thumbObj)["thumbnail"]["thumbnails"].back().value("url", "");
          }

          // Extraer duración
          extractDurationFromRow(row, track);

          if (!track.id.empty() && !track.title.empty()) {
            shelf.tracks.push_back(track);
          }
        }
      }

      if (!shelf.tracks.empty()) {
        outShelves.push_back(shelf);
      }
    }
  };

  auto fetchBrowseWithContinuations = [this, &parseCarouselSections, &outContinuationToken](
                                          const std::string& browseId,
                                          int maxContinuations,
                                          const std::string& startContToken = "") {
    std::string ctok = startContToken;
    if (ctok.empty()) {
      json clientObj = {
          {"clientName", "WEB_REMIX"},
          {"clientVersion", "1.20240101.01.00"},
          {"hl", "en"},
          {"gl", "US"}
      };
      if (!visitorData_.empty()) {
        clientObj["visitorData"] = visitorData_;
      }
      json payload = {
          {"context", {{"client", clientObj}}},
          {"browseId", browseId}
      };

      std::string resp = postInnerTube("browse", payload.dump());
      if (resp.empty()) return;

      try {
        auto j = json::parse(resp);
        if (j.contains("contents")) {
          auto singleCol = j["contents"].value("singleColumnBrowseResultsRenderer", json::object());
          if (singleCol.contains("tabs") && !singleCol["tabs"].empty()) {
            auto tab = singleCol["tabs"][0].value("tabRenderer", json::object());
            auto contentObj = tab.value("content", json::object());
            auto sectionList = contentObj.value("sectionListRenderer", json::object());
            parseCarouselSections(sectionList.value("contents", json::array()));
            ctok = findContinuationToken(sectionList);
          }
        }
      } catch (const std::exception& e) {
        nxlog::pushf("getHomeFeed parse error (%s): %s", browseId.c_str(), e.what());
      }
    }

    // Seguir páginas de continuación de FEmusic_home (como hace BitChord)
    for (int page = 0; page < maxContinuations && !ctok.empty(); ++page) {
      json contClient = {
          {"clientName", "WEB_REMIX"},
          {"clientVersion", "1.20240101.01.00"},
          {"hl", "en"},
          {"gl", "US"}
      };
      if (!visitorData_.empty()) {
        contClient["visitorData"] = visitorData_;
      }
      json contPayload = {
          {"context", {{"client", contClient}}},
          {"continuation", ctok}
      };
      std::string endpoint = "browse?ctoken=" + ctok + "&continuation=" + ctok + "&type=next";
      std::string prevTok = ctok;
      ctok.clear();

      std::string contResp = postInnerTube(endpoint, contPayload.dump());
      if (contResp.empty()) break;

      try {
        auto cj = json::parse(contResp);
        if (cj.contains("continuationContents") &&
            cj["continuationContents"].contains("sectionListContinuation")) {
          const auto& slc = cj["continuationContents"]["sectionListContinuation"];
          if (slc.contains("contents")) {
            parseCarouselSections(slc["contents"]);
          }
          std::string nextTok = findContinuationToken(slc);
          if (!nextTok.empty() && nextTok != prevTok) {
            ctok = nextTok;
          }
        }
      } catch (const std::exception& e) {
        nxlog::pushf("getHomeFeed continuation parse error: %s", e.what());
        break;
      }
    }
    if (!ctok.empty() && browseId == "FEmusic_home") {
      outContinuationToken = ctok;
    }
  };

  if (!continuationToken.empty()) {
    fetchBrowseWithContinuations("FEmusic_home", 2, continuationToken);
    nxlog::pushf("getHomeFeed continuation completed: %zu shelves (hasMore: %s)",
                 outShelves.size(), outContinuationToken.empty() ? "NO" : "YES");
    return !outShelves.empty();
  }

  // 0. Si el usuario vinculó su cuenta (TVHTML5), traer primero sus carruseles musicales personalizados
  if (isSignedIn()) {
    json tvPayload = {
        {"context", {
            {"client", {
                {"clientName", "TVHTML5"},
                {"clientVersion", "7.20250101.00.00"},
                {"hl", "en"},
                {"gl", "US"}
            }}
        }},
        {"browseId", "FEtopics_music"}
    };
    std::string tvResp = postInnerTube("browse", tvPayload.dump());
    if (!tvResp.empty()) {
      try {
        auto tj = json::parse(tvResp);
        std::function<void(const json&)> findTvShelves = [&](const json& node) {
          if (node.is_object()) {
            if (node.contains("shelfRenderer") && node["shelfRenderer"].is_object()) {
              const auto& sr = node["shelfRenderer"];
              YtShelf shelf;
              if (sr.contains("title")) {
                shelf.title = extractTextNode(sr["title"]);
              }
              if (shelf.title.empty() && sr.contains("headerRenderer") &&
                  sr["headerRenderer"].contains("shelfHeaderRenderer")) {
                const auto& shr = sr["headerRenderer"]["shelfHeaderRenderer"];
                if (shr.contains("title")) {
                  shelf.title = extractTextNode(shr["title"]);
                } else if (shr.contains("avatarLockup") &&
                           shr["avatarLockup"].contains("avatarLockupRenderer") &&
                           shr["avatarLockup"]["avatarLockupRenderer"].contains("title")) {
                  shelf.title = extractTextNode(shr["avatarLockup"]["avatarLockupRenderer"]["title"]);
                }
              }

              if (!shelf.title.empty() && sr.contains("content") &&
                  sr["content"].contains("horizontalListRenderer") &&
                  sr["content"]["horizontalListRenderer"].contains("items")) {
                const auto& items = sr["content"]["horizontalListRenderer"]["items"];
                if (items.is_array()) {
                  for (const auto& item : items) {
                    if (!item.contains("tileRenderer")) continue;
                    YtTrack trk;
                    if (parseTvTile(item["tileRenderer"], trk)) {
                      shelf.tracks.push_back(trk);
                    }
                  }
                }
              }
              if (!shelf.title.empty() && !shelf.tracks.empty()) {
                outShelves.push_back(shelf);
              }
              return;
            }
            for (auto it = node.begin(); it != node.end(); ++it) {
              if (it.key() == "trackingParams") continue;
              findTvShelves(it.value());
            }
          } else if (node.is_array()) {
            for (const auto& el : node) {
              findTvShelves(el);
            }
          }
        };
        findTvShelves(tj);
      } catch (const std::exception& e) {
        nxlog::pushf("getHomeFeed TVHTML5 parse error: %s", e.what());
      }
    }
  }

  // 1. Carruseles principales de Inicio + sus páginas de continuación (Selección rápida, Nuevos lanzamientos, etc.)
  fetchBrowseWithContinuations("FEmusic_home", 3);

  // 2. Carruseles de Explorar (Álbumes y sencillos nuevos, Tendencias, Videos musicales nuevos)
  fetchBrowseWithContinuations("FEmusic_explore", 0);

  // 3. Fallback de seguridad
  if (outShelves.empty()) {
    nxlog::push("getHomeFeed: FEmusic yielded no tracks, querying Top Hits for Home...");
    std::vector<YtTrack> topTracks;
    std::string dummyTok;
    if (searchTracks("Top Hits", YtSearchFilter::All, topTracks, dummyTok, "", 20)) {
      YtShelf trendingShelf;
      trendingShelf.title = "Tendencias y Canciones Populares";
      trendingShelf.tracks = topTracks;
      outShelves.push_back(trendingShelf);
    }
  }

  nxlog::pushf("getHomeFeed completed: %zu shelves ready for display", outShelves.size());
  return !outShelves.empty();
}

// =========================================================================
// OAuth 2.0 Device Flow (Google TV / YouTube on TV) & Sincronización
// =========================================================================

namespace {
constexpr const char* kTvOAuthClientId =
    "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com";
constexpr const char* kTvOAuthClientSecret =
    "SboVhoG9s0rNafixCSGGKXAT";

std::string performFormPost(const std::string& url, const std::string& formBody) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string responseStr;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, formBody.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(formBody.length()));
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 12000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
  switube::native::configure(curl, switube::native::cancel);

  CURLcode res = switchu::tls::perform(curl, url, switchu::tls::kHomeCerts, [&] { responseStr.clear(); });
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) return "";
  return responseStr;
}

std::string generateUuidV4() {
  static const char* hex = "0123456789abcdef";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 15);
  std::string out = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
  for (char& c : out) {
    if (c == 'x') {
      c = hex[dist(gen)];
    } else if (c == 'y') {
      c = hex[(dist(gen) & 0x3) | 0x8];
    }
  }
  return out;
}
} // namespace

bool YtMusicClient::requestDeviceCode(YtDeviceCode& outCode) {
  std::string uuid = generateUuidV4();
  std::string body = std::string("client_id=") + kTvOAuthClientId +
                     "&scope=http://gdata.youtube.com%20https://www.googleapis.com/auth/youtube-paid-content" +
                     "&device_id=" + uuid +
                     "&device_model=ytlr::";

  std::string resp = performFormPost("https://www.youtube.com/o/oauth2/device/code", body);
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);
    outCode.deviceCode = j.value("device_code", "");
    outCode.userCode = j.value("user_code", "");
    outCode.verificationUrl = j.value("verification_url", "https://www.google.com/device");
    outCode.intervalSec = j.value("interval", 5);
    outCode.expiresInSec = j.value("expires_in", 1800);
    if (outCode.deviceCode.empty() || outCode.userCode.empty()) {
      return false;
    }
    nxlog::pushf("YtMusicClient: device code obtained (%s -> %s)",
                 outCode.userCode.c_str(), outCode.verificationUrl.c_str());
    return true;
  } catch (const std::exception& e) {
    nxlog::pushf("requestDeviceCode parse error: %s", e.what());
    return false;
  }
}

int YtMusicClient::pollDeviceToken(const std::string& deviceCode) {
  if (deviceCode.empty()) return -1;

  std::string body = std::string("client_id=") + kTvOAuthClientId +
                     "&client_secret=" + kTvOAuthClientSecret +
                     "&code=" + deviceCode +
                     "&grant_type=http://oauth.net/grant_type/device/1.0";

  std::string resp = performFormPost("https://www.youtube.com/o/oauth2/token", body);
  if (resp.empty()) return 0; // Mantener intentando si hubo hipo de red

  try {
    auto j = json::parse(resp);
    if (j.contains("error")) {
      std::string err = j["error"].is_string() ? j["error"].get<std::string>() : "";
      if (err == "authorization_pending" || err == "slow_down") {
        return 0;
      }
      nxlog::pushf("pollDeviceToken error: %s", err.c_str());
      return -1;
    }

    std::string accessTok = j.value("access_token", "");
    std::string refreshTok = j.value("refresh_token", "");
    int expiresIn = j.value("expires_in", 3600);
    if (accessTok.empty()) return 0;

    {
      std::lock_guard<std::recursive_mutex> lock(clientMutex_);
      account_.accessToken = accessTok;
      if (!refreshTok.empty()) {
        account_.refreshToken = refreshTok;
      }
      account_.expiresAtEpoch = static_cast<int64_t>(std::time(nullptr)) + expiresIn;
      account_.signedIn = true;
      account_.name = "YouTube Music";
    }

    fetchAccountProfile();
    saveAuthConfig();
    nxlog::push("YtMusicClient: OAuth device authorization completed and saved!");
    return 1;
  } catch (...) {
    return 0;
  }
}

bool YtMusicClient::refreshAccessToken() {
  std::string refreshTok;
  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex_);
    refreshTok = account_.refreshToken;
  }
  if (refreshTok.empty()) return false;

  std::string body = std::string("client_id=") + kTvOAuthClientId +
                     "&client_secret=" + kTvOAuthClientSecret +
                     "&refresh_token=" + refreshTok +
                     "&grant_type=refresh_token";

  std::string resp = performFormPost("https://www.youtube.com/o/oauth2/token", body);
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);
    std::string accessTok = j.value("access_token", "");
    int expiresIn = j.value("expires_in", 3600);
    if (accessTok.empty()) return false;

    {
      std::lock_guard<std::recursive_mutex> lock(clientMutex_);
      account_.accessToken = accessTok;
      account_.expiresAtEpoch = static_cast<int64_t>(std::time(nullptr)) + expiresIn;
      account_.signedIn = true;
    }
    saveAuthConfig();
    nxlog::push("YtMusicClient: refreshed OAuth access token");
    return true;
  } catch (...) {
    return false;
  }
}

bool YtMusicClient::fetchAccountProfile() {
  if (!isSignedIn()) return false;

  auto extractProfileFromJson = [this](const json& j) -> bool {
    std::string foundName;
    std::string foundHandle;
    std::string foundPhoto;

    std::function<void(const json&, int)> walk = [&](const json& node, int depth) {
      if (depth > 12 || (!foundName.empty() && !foundPhoto.empty())) return;
      if (node.is_object()) {
        if (foundName.empty() && node.contains("accountName") && node["accountName"].is_object()) {
          const auto& an = node["accountName"];
          if (an.contains("runs") && an["runs"].is_array() && !an["runs"].empty()) {
            foundName = an["runs"][0].value("text", "");
          } else if (an.contains("simpleText")) {
            foundName = an.value("simpleText", "");
          }
        }
        if (foundHandle.empty() && node.contains("channelHandle") && node["channelHandle"].is_object()) {
          const auto& ch = node["channelHandle"];
          if (ch.contains("runs") && ch["runs"].is_array() && !ch["runs"].empty()) {
            foundHandle = ch["runs"][0].value("text", "");
          } else if (ch.contains("simpleText")) {
            foundHandle = ch.value("simpleText", "");
          }
        }
        if (foundHandle.empty() && node.contains("accountByline") && node["accountByline"].is_object()) {
          const auto& ab = node["accountByline"];
          if (ab.contains("runs") && ab["runs"].is_array() && !ab["runs"].empty()) {
            foundHandle = ab["runs"][0].value("text", "");
          } else if (ab.contains("simpleText")) {
            foundHandle = ab.value("simpleText", "");
          }
        }
        if (foundPhoto.empty() && node.contains("accountPhoto") && node["accountPhoto"].is_object()) {
          const auto& ap = node["accountPhoto"];
          if (ap.contains("thumbnails") && ap["thumbnails"].is_array() && !ap["thumbnails"].empty()) {
            foundPhoto = ap["thumbnails"].back().value("url", "");
          }
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
          if (it.key() == "trackingParams") continue;
          walk(it.value(), depth + 1);
        }
      } else if (node.is_array()) {
        for (const auto& elem : node) {
          walk(elem, depth + 1);
        }
      }
    };

    walk(j, 0);
    if (!foundName.empty()) {
      std::lock_guard<std::recursive_mutex> lock(clientMutex_);
      account_.name = foundName;
      if (!foundHandle.empty()) account_.handle = foundHandle;
      if (!foundPhoto.empty()) account_.photoUrl = foundPhoto;
      return true;
    }
    return false;
  };

  // Consultar account/accounts_list con TVHTML5
  json tvPayload = {
      {"context", {
          {"client", {
              {"clientName", "TVHTML5"},
              {"clientVersion", "7.20250101.00.00"},
              {"hl", "en"},
              {"gl", "US"}
          }}
      }}
  };
  std::string tvResp = postInnerTube("account/accounts_list", tvPayload.dump());
  if (!tvResp.empty()) {
    try {
      if (extractProfileFromJson(json::parse(tvResp))) {
        return true;
      }
    } catch (...) {}
  }

  return false;
}

bool YtMusicClient::getLikedTracks(std::vector<YtTrack>& outTracks,
                                   std::string& outContinuationToken,
                                   const std::string& continuationToken) {
  outTracks.clear();
  outContinuationToken.clear();
  if (!isSignedIn()) return false;

  return getPlaylistTracks("VLLM", outTracks, outContinuationToken, continuationToken);
}

bool YtMusicClient::getLibraryPlaylists(std::vector<YtTrack>& outPlaylists,
                                        std::string& outContinuationToken,
                                        const std::string& continuationToken) {
  outPlaylists.clear();
  outContinuationToken.clear();
  if (!isSignedIn()) return false;

  auto fetchTvLibraryBrowse = [this, &outPlaylists, &outContinuationToken](
                                  const std::string& browseId,
                                  const std::string& contTok = "") {
    json tvPayload = {
        {"context", {
            {"client", {
                {"clientName", "TVHTML5"},
                {"clientVersion", "7.20250101.00.00"},
                {"hl", "en"},
                {"gl", "US"}
            }}
        }}
    };
    if (!contTok.empty()) {
      tvPayload["continuation"] = contTok;
    } else {
      tvPayload["browseId"] = browseId;
    }

    std::string resp = postInnerTube("browse", tvPayload.dump());
    if (resp.empty()) return;

    try {
      auto j = json::parse(resp);
      std::string nextTok = findContinuationToken(j);
      if (!nextTok.empty() && nextTok != contTok && outContinuationToken.empty()) {
        outContinuationToken = nextTok;
      }
      std::function<void(const json&, int)> walk = [&](const json& node, int depth) {
        if (depth > 20) return;
        if (node.is_object()) {
          if (node.contains("tileRenderer") && node["tileRenderer"].is_object()) {
            YtTrack item;
            if (parseTvTile(node["tileRenderer"], item) && item.isPlaylist) {
              if (item.id != "VLLM" && item.id != "VLWL" && item.id != "VLSE") {
                bool dup = false;
                for (const auto& existing : outPlaylists) {
                  if (existing.id == item.id) {
                    dup = true;
                    break;
                  }
                }
                if (!dup) {
                  outPlaylists.push_back(item);
                }
              }
            }
            return;
          }
          for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "trackingParams") continue;
            walk(it.value(), depth + 1);
          }
        } else if (node.is_array()) {
          for (const auto& elem : node) {
            walk(elem, depth + 1);
          }
        }
      };
      walk(j, 0);
    } catch (const std::exception& e) {
      nxlog::pushf("getLibraryPlaylists TVHTML5 parse error (%s): %s", browseId.c_str(), e.what());
    }
  };

  if (!continuationToken.empty()) {
    fetchTvLibraryBrowse("FEplaylist_aggregation", continuationToken);
  } else {
    fetchTvLibraryBrowse("FEplaylist_aggregation");
    fetchTvLibraryBrowse("FElibrary");
  }

  nxlog::pushf("getLibraryPlaylists: loaded %zu playlists/albums from account (hasMore: %s)",
               outPlaylists.size(), outContinuationToken.empty() ? "NO" : "YES");
  return !outPlaylists.empty();
}

bool YtMusicClient::rateTrack(const std::string& videoId, bool like) {
  if (!isSignedIn() || videoId.empty()) return false;

  json payload = {
      {"context", {
          {"client", {
              {"clientName", "TVHTML5"},
              {"clientVersion", "7.20250101.00.00"},
              {"hl", "en"},
              {"gl", "US"}
          }}
      }},
      {"target", {
          {"videoId", videoId}
      }}
  };

  std::string endpoint = like ? "like/like" : "like/removelike";
  std::string resp = postInnerTube(endpoint, payload.dump());
  return !resp.empty();
}

} // namespace switube
