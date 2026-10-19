#pragma once
#include "phone.hpp"
#include <memory>
#include <string>

// The phone's music (phone.hpp): the songs in the Music folder beside the
// launcher, played through Windows' own media player (MCI) on a thread of its
// own. Opening a song takes MCI 60 to 250 ms, more from a slow disk, and on
// the XR worker it held the headset's frames that long at every song, an
// autoplayed one mid-swing included (October 19). Each call here returns at
// once; what plays shows in status() a moment later. A song MCI cannot play
// (an .m4a or .aac without a codec for it, a broken file) is passed over for
// the next one. AUTOPLAY and the volume are kept from session to session, in
// phone.ini beside the launcher's settings (%APPDATA%\Spidy).
namespace spidy {
class PhoneMusic {
  public:
    struct Status {
        int tracks{}, track{}; // track counts from 0; tracks 0: no songs
        bool playing{}, autoplay = true;
        // The song is one none of them would play from: none plays.
        bool failed{};
        int volume = phoneVolume; // percent
        char song[64]{};          // its shown name (phoneSongName)
    };
    PhoneMusic();
    // Stops the music.
    ~PhoneMusic();
    PhoneMusic(const PhoneMusic&) = delete;
    PhoneMusic& operator=(const PhoneMusic&) = delete;
    Status status() const;
    // Looks for songs again (mp3, wav, m4a, wma, aac) in the Music folder,
    // unless one is playing or paused, whose list stays.
    void load();
    // Plays from where it was paused, or pauses.
    void toggle();
    void next();
    void previous();
    // Percent, 0 to 100: the song playing at once, and every one after it.
    void setVolume(int percent);
    void setAutoplay(bool on);

  private:
    struct Player;
    std::unique_ptr<Player> player_;
};
// The launcher's folder: the nearest one up from this module with the
// launcher in it (the game loads a copy of the module from
// reports\bridge-modules\<hash>, tools/bridge_game.py).
std::wstring spidyHome();
// The Music folder in it (made if missing).
std::wstring phoneMusicFolder();
} // namespace spidy
