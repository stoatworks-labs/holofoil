/*
 * Stoatworks Labs - About window data for Holofoil.
 *
 * Written by hand for v0.1.0, in the shape stoatworks-backend's
 * scripts/sync-about.py generates: Holofoil is not in the website's
 * projects.json yet, so there is nothing to sync from. The first sync after
 * it is registered overwrites this file. There is no user guide, project
 * page or public repo yet, so `guide`, `page` and `repo` are empty and the
 * About block has only its support button: StoatworksAboutLinks.h leaves a
 * link out rather than show a button that opens a 404. The block is last,
 * so the buttons a sync adds move no control.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Holofoil";
    inline constexpr auto slug = "holofoil";
    inline constexpr auto hook = "Holographic trading-card foil, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "";
    inline constexpr auto repo = "";
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
