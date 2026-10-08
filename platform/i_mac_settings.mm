/* Native macOS graphics settings dialog for the 3D renderer (F4). */
extern "C" {
#include "doomtype.h"
#include "m_argv.h"
extern boolean paused;
}
#include <SDL3/SDL.h>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <vector>
#include "i_render3d.h"
#include "scene3d.h"
#include "settings_panel.h"

using namespace doom3d;

// Controls write into a draft of the settings as they change, so options that
// refine another one grey out while it is off. Done keeps the draft.
@interface DoomSettingsTarget : NSObject
@property(nonatomic,copy) void (^changed)(void);
- (void)controlChanged:(id)sender;
@end
@implementation DoomSettingsTarget
- (void)controlChanged:(id)sender { (void)sender; self.changed(); }
@end

namespace {
// One native control per option, in the order of the shared table.
struct Row { Option *option; NSControl *control; };

NSString *text(const char *string) { return string?[NSString stringWithUTF8String:string]:nil; }

CGFloat rowHeight(const Option &option) { return option.kind==OptionKind::Check?27:31; }

const CGFloat columnWidth=390,columnGap=28,margin=14,sectionGap=14,headingHeight=26,labelWidth=150;

CGFloat columnHeight(const OptionTab &tab,int column) {
    CGFloat height=0;
    for(const OptionSection &section:tab.sections) {
        if(section.column!=column)continue;
        if(height>0)height+=sectionGap;
        height+=headingHeight;
        for(const Option &option:section.options)height+=rowHeight(option);
    }
    return height;
}

NSControl *addControl(NSView *page,const Option &option,CGFloat x,CGFloat y,DoomSettingsTarget *target) {
    if(option.kind!=OptionKind::Check) {
        NSTextField *label=[NSTextField labelWithString:text(option.title)];
        label.frame=NSMakeRect(x,y+3,labelWidth,22);label.toolTip=text(option.tip);[page addSubview:label];
    }
    CGFloat controlX=x+labelWidth+5,controlWidth=columnWidth-labelWidth-5;
    NSControl *control;
    if(option.kind==OptionKind::Check) {
        NSButton *button=[NSButton checkboxWithTitle:text(option.title) target:target action:@selector(controlChanged:)];
        button.frame=NSMakeRect(x,y,columnWidth,24);button.state=*option.flag?NSControlStateValueOn:NSControlStateValueOff;
        control=button;
    } else if(option.kind==OptionKind::Choice) {
        NSPopUpButton *popup=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(controlX,y,controlWidth,28) pullsDown:NO];
        NSInteger selected=0;
        for(size_t i=0;i<option.items.size();++i) {
            [popup addItemWithTitle:text(option.items[i])];
            if(option.values[i]==*option.flag)selected=(NSInteger)i;
        }
        [popup selectItemAtIndex:selected];popup.target=target;popup.action=@selector(controlChanged:);
        control=popup;
    } else {
        NSSlider *bar=[NSSlider sliderWithValue:*option.number minValue:option.low maxValue:option.high
                                         target:target action:@selector(controlChanged:)];
        bar.frame=NSMakeRect(controlX,y+3,controlWidth,24);bar.numberOfTickMarks=option.ticks;
        control=bar;
    }
    control.toolTip=text(option.tip);[page addSubview:control];
    return control;
}

void readControls(const std::vector<Row> &rows) {
    for(const Row &row:rows) {
        const Option &option=*row.option;
        if(option.kind==OptionKind::Check)*option.flag=((NSButton*)row.control).state==NSControlStateValueOn;
        else if(option.kind==OptionKind::Choice)*option.flag=option.values[(size_t)((NSPopUpButton*)row.control).indexOfSelectedItem];
        else *option.number=((NSSlider*)row.control).floatValue;
    }
}

void refreshEnabled(const std::vector<Row> &rows,const Settings &draft) {
    for(const Row &row:rows)row.control.enabled=row.option->isEnabled(draft);
}
}

void I_Render3DSettings(void) {
    if(M_CheckParm((char*)"-ingamesettings")) {I_Render3DSettingsMenu();return;}
    static NSInteger lastTab=0;
    @autoreleasepool {
        Settings draft=settings;
        std::vector<OptionTab> tabs=optionTabs(draft);
        std::vector<Row> rows;
        DoomSettingsTarget *target=[DoomSettingsTarget new];
        std::vector<Row> *rowsRef=&rows;Settings *draftRef=&draft;
        target.changed=^{readControls(*rowsRef);refreshEnabled(*rowsRef,*draftRef);};

        CGFloat pageHeight=0;
        for(const OptionTab &tab:tabs)pageHeight=std::max({pageHeight,columnHeight(tab,0),columnHeight(tab,1)});
        pageHeight+=2*margin;
        const CGFloat pageWidth=2*margin+2*columnWidth+columnGap;
        NSTabView *tabView=[[NSTabView alloc]initWithFrame:NSMakeRect(0,0,pageWidth,pageHeight)];
        NSRect content=tabView.contentRect;
        [tabView setFrameSize:NSMakeSize(2*pageWidth-content.size.width,2*pageHeight-content.size.height)];
        for(OptionTab &tab:tabs) {
            NSView *page=[[NSView alloc]initWithFrame:NSMakeRect(0,0,pageWidth,pageHeight)];
            for(int column=0;column<2;++column) {
                CGFloat x=margin+column*(columnWidth+columnGap),y=pageHeight-margin;
                bool first=true;
                for(OptionSection &section:tab.sections) {
                    if(section.column!=column)continue;
                    if(!first)y-=sectionGap;
                    first=false;
                    NSTextField *heading=[NSTextField labelWithString:text(section.title)];
                    heading.font=[NSFont boldSystemFontOfSize:NSFont.systemFontSize];
                    heading.frame=NSMakeRect(x,y-22,columnWidth,20);[page addSubview:heading];
                    y-=headingHeight;
                    for(Option &option:section.options) {
                        y-=rowHeight(option);
                        rows.push_back({&option,addControl(page,option,x,y,target)});
                    }
                }
            }
            NSTabViewItem *item=[[NSTabViewItem alloc]initWithIdentifier:text(tab.title)];
            item.label=text(tab.title);item.view=page;[tabView addTabViewItem:item];
        }
        refreshEnabled(rows,draft);
        if(lastTab>=0&&lastTab<tabView.numberOfTabViewItems)[tabView selectTabViewItemAtIndex:lastTab];

        NSAlert *alert=[NSAlert new];alert.messageText=@"Graphics";
        alert.informativeText=@"Hover over an option for details.";
        [alert addButtonWithTitle:@"Done"];[alert addButtonWithTitle:@"Reset"];
        alert.accessoryView=tabView;
        bool wasPaused=paused;paused=true;
        SDL_SetWindowRelativeMouseMode(gameWindow,false);
        NSInteger response=[alert runModal];paused=wasPaused;
        lastTab=[tabView indexOfTabViewItem:tabView.selectedTabViewItem];
        if(response==NSAlertSecondButtonReturn)settings=Settings{};
        else {readControls(rows);settings=draft;}
        settingsChanged();
    }
}
