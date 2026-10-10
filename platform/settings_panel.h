/* The F4 graphics options, grouped into tabs and sections, as the in-game
 * panel (settings_menu.cpp) shows them. */
#ifndef DOOM_SETTINGS_PANEL_H
#define DOOM_SETTINGS_PANEL_H
#include <vector>
#include "scene3d.h"

namespace doom3d {
enum class OptionKind { Check, Choice, Slider };
// Whether an option has an effect with the other settings as they are.
using OptionCondition=bool(*)(const Settings&);
struct Option {
    OptionKind kind;
    const char *title,*tip;
    int *flag=nullptr;              // Check and Choice
    std::vector<const char*> items; // Choice: one item per entry in values; Slider: a label per step, if any
    std::vector<int> values;
    float *number=nullptr;          // Slider
    double low=0,high=1,step=0.05;
    OptionCondition enabled=nullptr;
    bool isEnabled(const Settings &s) const {return !enabled||enabled(s);}
};
struct OptionSection { const char *title; std::vector<Option> options; };
struct OptionTab { const char *title; std::vector<OptionSection> sections; };

// The options, bound to the members of s.
std::vector<OptionTab> optionTabs(Settings &s);
}
#endif
