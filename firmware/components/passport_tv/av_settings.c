#include "av_settings.h"

const char *av_quality_id(unsigned quality) {
    static const char *const ids[]={"smooth","standard","ultra"};
    return ids[quality<AV_QUALITY_COUNT?quality:AV_QUALITY_DEFAULT];
}
const char *av_quality_name(unsigned quality) {
    static const char *const names[]={"Smooth","Standard","Ultra"};
    return names[quality<AV_QUALITY_COUNT?quality:AV_QUALITY_DEFAULT];
}

// Ten levels in steps of 10%, brightest first, matching the volume steps.
// Descending order is what makes "up" mean brighter with a plain index
// decrement, which is the direction the arrow on the case points.
const uint8_t AV_BRIGHTNESS_LEVELS[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10};
const unsigned AV_BRIGHTNESS_COUNT = sizeof(AV_BRIGHTNESS_LEVELS) / sizeof(AV_BRIGHTNESS_LEVELS[0]);

uint8_t av_brightness_percent(unsigned index)
{
    // Clamped rather than rejected. A value read from storage was written by
    // some build of this firmware, and turning an out-of-range index into the
    // default is friendlier than refusing to set a backlight at all.
    if (index >= AV_BRIGHTNESS_COUNT) {
        index = AV_BRIGHTNESS_COUNT - 1u;
    }
    return AV_BRIGHTNESS_LEVELS[index];
}

unsigned av_brightness_nearest_index(uint8_t percent)
{
    unsigned best = 0;
    unsigned best_gap = 0xFFFFFFFFu;
    for (unsigned i = 0; i < AV_BRIGHTNESS_COUNT; i++) {
        uint8_t level = AV_BRIGHTNESS_LEVELS[i];
        // Unsigned distance without branching on which is larger.
        unsigned gap = (level > percent) ? (unsigned)(level - percent)
                                         : (unsigned)(percent - level);
        if (gap < best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    return best;
}

unsigned av_brightness_step(unsigned index, bool up)
{
    if (index >= AV_BRIGHTNESS_COUNT) {
        index = 0;   // start from the brightest rather than stepping from nowhere
    }
    if (up) {
        // Brighter means earlier in the table; wrap from the top back to the
        // dimmest so a held button keeps cycling instead of stopping.
        return (index == 0u) ? (AV_BRIGHTNESS_COUNT - 1u) : (index - 1u);
    }
    return (index + 1u) % AV_BRIGHTNESS_COUNT;
}

bool av_brightness_valid_percent(uint8_t percent)
{
    for (unsigned i = 0; i < AV_BRIGHTNESS_COUNT; i++) {
        if (AV_BRIGHTNESS_LEVELS[i] == percent) {
            return true;
        }
    }
    return false;
}
