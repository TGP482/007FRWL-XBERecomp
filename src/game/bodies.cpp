#include "common/options.h"
#include "xdk/hle.h"

// Persistent bodies: dead enemies, and the guns and shields they drop, are never faded out and deleted.

// enemy death done (0x46D70, every enemy kind): after the corpse leaves the collision, true skips the second long
// fade of the body and its held items, the last of which deletes the body
bool KeepCorpse(X86Context&, uint8_t*) { return game::Opt().persistent_bodies; }

// item dropped (0xA6CE0): true leaves its fade timer unset, a minute then a two second fade and delete
bool KeepDroppedItem(X86Context&, uint8_t*) { return game::Opt().persistent_bodies; }
