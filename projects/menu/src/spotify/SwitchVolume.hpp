#pragma once

// The console's own volume: the one its buttons change, for the output in use
// (speaker, headphones, TV, Bluetooth). Spotify's player on this Switch stays
// at full volume, so this is the only volume there is.
namespace switchvolume {

int get();                // 0..100; -1 when it can't be read
bool set(int percent);    // to the nearest of the console's steps
bool step(int direction); // one of the console's steps up (+1) or down (-1)

} // namespace switchvolume
