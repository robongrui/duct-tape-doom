/* The F4 graphics options, grouped into tabs and sections. Shared by the
 * native Mac dialog (i_mac_settings.mm) and the in-game panel used on other
 * systems (settings_menu.cpp), so both always offer the same options. */
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
    std::vector<const char*> items; // Choice: one item per entry in values
    std::vector<int> values;
    float *number=nullptr;          // Slider
    double low=0,high=1,step=0.05;
    int ticks=0;                    // Slider tick marks in the Mac dialog
    OptionCondition enabled=nullptr;
    bool isEnabled(const Settings &s) const {return !enabled||enabled(s);}
};
// column places the section in the Mac dialog's two-column tab pages.
struct OptionSection { const char *title; int column; std::vector<Option> options; };
struct OptionTab { const char *title; std::vector<OptionSection> sections; };

// The options, bound to the members of s.
std::vector<OptionTab> optionTabs(Settings &s);
}
#endif
