// Track metadata adapted from Switube by dl3g0 (GPL-3.0-or-later).
#pragma once
#include <cstdint>
#include <string>

namespace switube {

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

} // namespace switube
