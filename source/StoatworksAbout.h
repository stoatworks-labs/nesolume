/*
 * Stoatworks Labs - About window data for NESolume.
 *
 * Written by hand at project creation; the next run of
 * stoatworks-backend/scripts/sync-about.py overwrites it once the project is
 * in the website's projects.json, which is the one place these facts are
 * written down. Guide and page are empty until those pages exist — an empty
 * string means the About block simply does not show that button.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "NESolume";
    inline constexpr auto slug = "nesolume";
    inline constexpr auto hook = "Retro console video hardware for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/nesolume";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
