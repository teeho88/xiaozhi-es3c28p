#ifndef ZING_CLIENT_H
#define ZING_CLIENT_H

#include <string>

// Minimal on-device Zing MP3 client: searches for a song by name and resolves a
// playable 128 kbps MP3 stream URL. Request signing (HMAC-SHA512 + SHA256) is
// done with mbedtls. The API_KEY/SECRET_KEY are the long-stable community keys;
// if Zing ever rotates them, update the constants in zing_client.cc.
struct ZingSong {
    std::string id;         // encodeId
    std::string title;      // UTF-8 (may contain Vietnamese diacritics)
    std::string artists;    // artistsNames, UTF-8
    std::string thumbnail;  // album-art JPEG URL (thumbnailM), may be empty
};

namespace zing {

// Search Zing MP3, return the best-matching song. false on network/parse error.
bool SearchTopSong(const std::string& query, ZingSong& out);

// Resolve a direct 128 kbps MP3 URL for the encodeId. false on error or when the
// song is VIP/region locked (no free stream). url_out is an http(s) URL.
bool GetStreamUrl(const std::string& song_id, std::string& url_out);

}  // namespace zing

#endif  // ZING_CLIENT_H
