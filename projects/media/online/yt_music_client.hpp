#include <mutex>
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

namespace switube {

enum class YtSearchFilter {
  All,
  Songs,
  Videos,
  Albums,
  Artists,
  Playlists
};

struct YtTrack {
  std::string id;          // videoId ej. "dQw4w9WgXcQ", playlistId "VLPL..." / "MPRE...", o artistId "UC..."
  std::string title;
  std::string artist;
  std::string album;
  std::string durationStr; // ej. "3:45"
  uint32_t durationMs = 0;
  std::string imageUrl;
  bool isPlaylist = false;
  bool isArtist = false;
};

struct YtShelf {
  std::string title;
  std::vector<YtTrack> tracks;
};

struct YtArtistPage {
  std::string id;
  std::string name;
  std::string description;
  std::string thumbnailUrl;
  std::string subscriberCount;
  std::string allSongsBrowseId;
  std::vector<YtTrack> topSongs;
  std::vector<YtShelf> sections; // Álbumes, Singles & EPs, Vídeos, Directo, Destacado en, Listas, Similares
};

struct AudioStreamUrl {
  std::string url;
  std::string mimeType;   // ej. "audio/webm; codecs=\"opus\""
  std::string codec;      // "opus" o "aac"
  int bitrate = 0;        // ej. 160000
  uint32_t approxDurationMs = 0;
  uint64_t contentLength = 0;
  bool isDirect = true;
};

struct YtDeviceCode {
  std::string deviceCode;
  std::string userCode;
  std::string verificationUrl;
  int intervalSec = 5;
  int expiresInSec = 1800;
};

struct YtAccountInfo {
  bool signedIn = false;
  std::string name;
  std::string handle;
  std::string photoUrl;
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresAtEpoch = 0;
};

class YtMusicClient {
 public:
  YtMusicClient();
  ~YtMusicClient();

  bool init();

  // 1. Resuelve la URL directa de audio Opus/AAC
  bool resolveAudioStream(const std::string& videoId, AudioStreamUrl& outStream);

  // 2. Búsqueda de pistas en YouTube Music con filtros y paginación
  bool searchTracks(const std::string& query,
                    YtSearchFilter filter,
                    std::vector<YtTrack>& outTracks,
                    std::string& outContinuationToken,
                    const std::string& continuationToken = "",
                    size_t limit = 25);

  bool searchTracks(const std::string& query, std::vector<YtTrack>& outTracks, size_t limit = 25) {
    std::string dummyToken;
    return searchTracks(query, YtSearchFilter::All, outTracks, dummyToken, "", limit);
  }

  // 3. Home Feed (Recomendaciones, temas populares) con paginación
  bool getHomeFeed(std::vector<YtShelf>& outShelves,
                   std::string& outContinuationToken,
                   const std::string& continuationToken = "");

  bool getHomeFeed(std::vector<YtShelf>& outShelves) {
    std::string dummyToken;
    return getHomeFeed(outShelves, dummyToken, "");
  }

  // 4. Obtener pistas dentro de una playlist o álbum (browseId) con paginación
  bool getPlaylistTracks(const std::string& browseId,
                         std::vector<YtTrack>& outTracks,
                         std::string& outContinuationToken,
                         const std::string& continuationToken = "");

  bool getPlaylistTracks(const std::string& browseId, std::vector<YtTrack>& outTracks) {
    std::string dummyToken;
    return getPlaylistTracks(browseId, outTracks, dummyToken, "");
  }

  // 5. Obtener página completa de artista (Canciones principales, álbumes, singles, vídeos, directo, listas, similares)
  bool getArtistPage(const std::string& artistBrowseId, YtArtistPage& outPage);

  // 6. Autenticación OAuth 2.0 Device Flow (estilo Google TV / YouTube on TV) y sincronización
  bool requestDeviceCode(YtDeviceCode& outCode);
  // Devuelve: 0 = pendiente, 1 = autorizado con éxito, -1 = expirado o error
  int pollDeviceToken(const std::string& deviceCode);
  bool refreshAccessToken();
  bool fetchAccountProfile();
  void logout();

  bool isSignedIn() const;
  YtAccountInfo getAccountInfo() const;

  // 7. Biblioteca y Me gusta de la cuenta vinculada (con soporte de paginación)
  bool getLikedTracks(std::vector<YtTrack>& outTracks,
                      std::string& outContinuationToken,
                      const std::string& continuationToken = "");

  bool getLikedTracks(std::vector<YtTrack>& outTracks) {
    std::string dummyToken;
    return getLikedTracks(outTracks, dummyToken, "");
  }

  bool getLibraryPlaylists(std::vector<YtTrack>& outPlaylists,
                           std::string& outContinuationToken,
                           const std::string& continuationToken = "");

  bool getLibraryPlaylists(std::vector<YtTrack>& outPlaylists) {
    std::string dummyToken;
    return getLibraryPlaylists(outPlaylists, dummyToken, "");
  }

  bool rateTrack(const std::string& videoId, bool like);

  void setAuthCookies(const std::string& cookies);

 private:
  std::string authCookies_;
  std::string visitorData_;
  YtAccountInfo account_;
  mutable std::recursive_mutex clientMutex_;

  void loadAuthConfig();
  void saveAuthConfig();
  void ensureVisitorData();

  std::string postInnerTube(const std::string& endpoint, const std::string& jsonBody,
                           const std::string& baseHost = "https://music.youtube.com",
                           const std::string& userAgentOverride = "",
                           bool sendOrigin = true);
};

} // namespace switube
