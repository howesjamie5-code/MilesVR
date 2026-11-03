// Draws the phone (phone.hpp's appendPhone, its screen's picture laid on its
// glass) face on, side by side - home with MUSIC picked, home with MENU
// picked, MUSIC, GEAR - and the home screen once more turned 35 degrees, for
// its body's depth, with sample songs and gadgets, as a BMP, to look at its
// layout without a headset. Drawn as the marks' preview draws them
// (preview_image.hpp), from 10 m off, where the eye's perspective is next to
// none: 5000 pixels a metre.
//
//   spidy_phone_preview <out.bmp>
#include "preview_image.hpp"
#include "spidy/phone.hpp"
#include <cstdio>
#include <vector>
using namespace spidy;
using namespace spidy::preview;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "spidy_phone_preview <out.bmp>\n");
        return 1;
    }
    constexpr float pixels = 5000, distance = 10;
    constexpr int each = 520, w = each * 5, h = 960;
    Image image(w, h);
    for (float& c : image.rgb)
        c = .03f;
    PhoneView view;
    view.open = true;
    view.age = 1;
    view.tracks = 42;
    view.track = 2;
    view.playing = true;
    view.volume = 70;
    std::snprintf(view.song, sizeof view.song, "%s", "SUNFLOWER");
    const char* names[] = {"WEB SHOOTER", "IMPACT WEB", "WEB BOMB", "ELECTRIC WEB", "TRIP MINE",
                           "CONCUSSION BLAST", "SPIDER-DRONE", "SUSPENSION MATRIX"};
    view.gearCount = 8;
    for (int i = 0; i < 8; ++i) {
        std::snprintf(view.gear[i].name, sizeof view.gear[i].name, "%s", names[i]);
        view.gear[i].most = i ? 4 + i % 3 : 0;
        view.gear[i].ammo = i ? 2 + i % 2 : 0;
        view.gear[i].equipped = i == 2;
    }
    const PhoneScreen screens[] = {PhoneScreen::home, PhoneScreen::home, PhoneScreen::music, PhoneScreen::gear,
                                   PhoneScreen::home};
    for (int s = 0; s < 5; ++s) {
        // The last turned about its up, its right edge away.
        const float turn = s == 4 ? .61f : 0.f;
        view.frame = {{0, 0, -distance},
                      {std::cos(turn), 0, -std::sin(turn)},
                      {0, 1, 0},
                      {std::sin(turn), 0, std::cos(turn)}};
        view.screen = screens[s];
        view.app = s == 1 ? phoneMenuApp : 0;
        view.lit = PhoneButton::play;
        view.gearTop = s == 3 ? 1 : 0;
        std::vector<Vertex> v, screen;
        appendPhone(v, view, {0, 0, 0});
        appendPhoneScreen(screen, view);
        placePhoneScreen(screen, view);
        v.insert(v.end(), screen.begin(), screen.end());
        // The phone's middle at its cell's.
        const Camera camera{pixels * distance, each * (s + .5f), h / 2.f};
        draw(image, v, camera, 0, 0, 0, 0, w, h);
    }
    return save(image, argv[1]) ? 0 : 1;
}
