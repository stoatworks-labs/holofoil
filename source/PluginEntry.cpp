#include "Holofoil.h"

/**
    The one registration.

    This file is listed directly in the Holofoil MODULE target, not in
    holofoil_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC
    archive the linker is entitled to drop the whole translation unit --
    giving a bundle that loads, exports `plugMain`, and reports that it
    contains no plugins. The core stays an OBJECT library for the same reason.

    The name is `SW Holofoil`, eleven characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class HolofoilEffect : public holofoil::HolofoilPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< HolofoilEffect >,// Create method
	"HF01",                         // Plugin unique ID of maximum length 4
	"SW Holofoil",                  // Plugin name
	2,                              // API major version number
	1,                              // API minor version number
	0,                              // Plugin major version number
	1,                              // Plugin minor version number
	FF_EFFECT,                      // Plugin type
	"Holographic foil, for trading cards.\n\nThe clip is a printed card with a foil under it, held under a lamp. The "
	"foil is a diffraction grating: each wavelength leaves it at its own angle, so what colour reaches the eye depends "
	"on where the eye and the lamp are, and changes as the card tilts. Put the foil in the art window (a holo), around "
	"it (a reverse holo), over the whole card, or where the print is light or dark; emboss it plain, crosshatched, "
	"with discs, in shards or as glitter.",
	"Holofoil FFGL effect"// About
);

extern "C" const char* HolofoilBuildStamp()
{
	return "holofoil " HOLOFOIL_VERSION ", built " __DATE__ " " __TIME__;
}
