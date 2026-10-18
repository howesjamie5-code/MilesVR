#pragma once
#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// OpenXR add-ons (API layers) Spidy cannot run with. The OpenXR loader puts
// every installed implicit layer into each program that starts OpenXR, as
// the program creates its instance: with Spidy that program is the game.
// OpenXR Toolkit hooks the program's graphics there, and with it the game
// closed at the Sony logo, seconds after it started, with an unhandled
// exception from outside its own code (a player's report and game log,
// October 9; he traced it to the Toolkit).
//
// The loader has the way round it: every implicit layer's manifest must name
// an environment variable that switches the layer off in a process that has
// it set (disable_environment). Spidy sets that variable in the game while
// OpenXR starts there, for the layers listed here and no others; the layer
// stays on in the player's other games.
namespace spidy::xr_layers {
struct Layer {
    // As its manifest names it, and as the player knows it.
    std::string_view id, name;
};
inline constexpr Layer unsupported[] = {{"XR_APILAYER_MBUCCHIA_toolkit", "OpenXR Toolkit"}};

// The string a manifest (JSON text) gives `key`; "" without one.
inline std::string member(std::string_view manifest, std::string_view key) {
    const std::string quoted = '"' + std::string(key) + '"';
    const auto space = [&](size_t at) {
        while (at < manifest.size() && std::isspace(static_cast<unsigned char>(manifest[at])))
            ++at;
        return at;
    };
    for (size_t found = manifest.find(quoted); found != std::string_view::npos;
         found = manifest.find(quoted, found + 1)) {
        size_t at = space(found + quoted.size());
        if (at >= manifest.size() || manifest[at] != ':')
            continue; // the text as a value, not a key
        at = space(at + 1);
        if (at >= manifest.size() || manifest[at] != '"')
            continue;
        std::string value;
        for (++at; at < manifest.size() && manifest[at] != '"'; ++at) {
            if (manifest[at] == '\\' && at + 1 < manifest.size())
                ++at;
            value += manifest[at];
        }
        return at < manifest.size() ? value : std::string();
    }
    return {};
}
// What switches the layer of a manifest off in this process, when it is one
// of `layers`.
struct Switch {
    // The environment variable to set; "" for a layer Spidy runs with, and
    // for a manifest that names none a process could set.
    std::string variable;
    std::string_view name;
};
inline Switch switchOff(std::string_view manifest, std::span<const Layer> layers = unsupported) {
    const auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        const auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
        for (size_t i = 0; i < a.size(); ++i)
            if (lower(a[i]) != lower(b[i]))
                return false;
        return true;
    };
    const auto id = member(manifest, "name");
    for (const auto& layer : layers) {
        if (!same(id, layer.id))
            continue;
        auto variable = member(manifest, "disable_environment");
        if (variable.empty() || variable.size() > 127)
            return {};
        for (const char c : variable)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return {};
        return {std::move(variable), layer.name};
    }
    return {};
}
} // namespace spidy::xr_layers
