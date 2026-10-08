/* The in-game F4 graphics panel, used on Windows and Linux and on the Mac with
 * -ingamesettings. It draws into its own 640x400 image in the 8x16 Spleen font,
 * like a text-mode setup program; the renderer scales it by whole pixels over
 * the dimmed view. Same tabs and options as the Mac dialog; changes apply when
 * the panel closes. */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
extern "C" {
#include "doomtype.h"
#include "doomdef.h"
#include "s_sound.h"
#include "sounds.h"
extern boolean paused;
}
#include <SDL3/SDL.h>
#include "i_render3d.h"
#include "scene3d.h"
#include "settings_panel.h"
#include "../third_party/spleen/spleen_8x16.h"

using namespace doom3d;

namespace {
// One row of the current tab: a section heading (option null) or an option.
struct Line { const OptionSection *section; Option *option; };

struct Panel {
    bool open=false,wasPaused=false;
    Settings draft;
    std::vector<OptionTab> tabs;
    std::vector<Line> lines;
    int tab=0,selected=0,scroll=0;
    std::vector<std::pair<int,int>> tabColumns; // first and last text column of each tab name
} panel;

// The image is a grid of 8x16 character cells, 80 columns by 25 rows.
const int width=DOOM_SETTINGS_WIDTH,height=DOOM_SETTINGS_HEIGHT,cellWidth=8,cellHeight=16,columns=width/cellWidth;
const int tabRow=0,listRow=2,visibleLines=17,descriptionRow=20,descriptionLines=4,helpRow=24;
const int leftColumn=2,rightColumn=78,sliderColumns=20,valueColumns=6;

// Colors as 0xAARRGGBB, the byte order of the B8G8R8A8 texture.
const unsigned backdrop=0xc8080808,separator=0xff3a3a3a,text=0xffe6e6e6,heading=0xffe8b040,dim=0xff686868,
               description=0xffb4b4b4,selectionBar=0xff7a1414,bright=0xffffffff,onColor=0xff8cd48c,
               tabBar=0xffa01818,track=0xff505050,trackFill=0xffd03030;

unsigned *image;

void fill(int x,int y,int w,int h,unsigned color) {
    for(int row=std::max(0,y);row<std::min(height,y+h);++row)
        std::fill(image+row*width+std::max(0,x),image+row*width+std::min(width,x+w),color);
}

void drawText(const std::string &string,int column,int row,unsigned color) {
    for(size_t i=0;i<string.size();++i) {
        unsigned char c=(unsigned char)string[i];
        if(c<32||c>126)c='?';
        const unsigned char *glyph=spleen8x16[c-32];
        int x=(column+(int)i)*cellWidth,y=row*cellHeight;
        if(x+cellWidth>width)break;
        for(int line=0;line<cellHeight;++line)
            for(int bit=0;bit<8;++bit)if(glyph[line]&(0x80>>bit))image[(y+line)*width+x+bit]=color;
    }
}

int rowY(int row) { return row*cellHeight; }

void drawSeparator(int row) { fill(leftColumn*cellWidth,rowY(row)+cellHeight/2,(rightColumn-leftColumn)*cellWidth,1,separator); }

// Splits text into lines of at most lineWidth characters; the last line ends
// in "..." when the text does not fit.
std::vector<std::string> wrap(const std::string &string,size_t lineWidth,size_t maxLines) {
    std::vector<std::string> lines;
    std::string line,word;
    auto flushWord=[&] {
        if(word.empty())return;
        if(line.empty())line=word;
        else if(line.size()+1+word.size()<=lineWidth)line+=" "+word;
        else {lines.push_back(line);line=word;}
        word.clear();
    };
    for(char c:string) {if(c==' ')flushWord();else word+=c;}
    flushWord();
    if(!line.empty())lines.push_back(line);
    if(lines.size()>maxLines) {
        lines.resize(maxLines);
        std::string &last=lines.back();
        if(last.size()+3>lineWidth)last.resize(lineWidth-3);
        last+="...";
    }
    return lines;
}

std::string valueText(const Option &option) {
    if(option.kind==OptionKind::Check)return *option.flag?"On":"Off";
    if(option.kind==OptionKind::Choice) {
        for(size_t i=0;i<option.values.size();++i)if(option.values[i]==*option.flag)return option.items[i];
        return option.items.front();
    }
    char value[16];
    snprintf(value,sizeof(value),option.step>=1?"%.0f":"%.2f",*option.number);
    return value;
}

void selectTab(int tab) {
    int count=(int)panel.tabs.size();
    panel.tab=(tab%count+count)%count;
    panel.lines.clear();
    for(OptionSection &section:panel.tabs[(size_t)panel.tab].sections) {
        panel.lines.push_back({&section,nullptr});
        for(Option &option:section.options)panel.lines.push_back({&section,&option});
    }
    panel.selected=1;panel.scroll=0;
}

void keepSelectionVisible() {
    // The first option brings its section heading into view too.
    if(panel.selected==1)panel.scroll=0;
    if(panel.selected<panel.scroll)panel.scroll=panel.selected;
    if(panel.selected>=panel.scroll+visibleLines)panel.scroll=panel.selected-visibleLines+1;
}

void moveSelection(int direction) {
    int count=(int)panel.lines.size();
    do panel.selected=((panel.selected+direction)%count+count)%count;
    while(!panel.lines[(size_t)panel.selected].option);
    keepSelectionVisible();
    S_StartSound(NULL,sfx_pstop);
}

Option &selectedOption() { return *panel.lines[(size_t)panel.selected].option; }

void setSlider(Option &option,double value) {
    double steps=std::round((value-option.low)/option.step);
    *option.number=(float)std::clamp(option.low+steps*option.step,option.low,option.high);
}

void changeOption(int direction) {
    Option &option=selectedOption();
    if(!option.isEnabled(panel.draft)) {S_StartSound(NULL,sfx_oof);return;}
    if(option.kind==OptionKind::Check)*option.flag=!*option.flag;
    else if(option.kind==OptionKind::Choice) {
        int count=(int)option.values.size(),index=0;
        for(int i=0;i<count;++i)if(option.values[(size_t)i]==*option.flag)index=i;
        *option.flag=option.values[(size_t)(((index+direction)%count+count)%count)];
    } else setSlider(option,*option.number+direction*option.step);
    S_StartSound(NULL,sfx_stnmov);
}

void closePanel() {
    settings=panel.draft;
    paused=panel.wasPaused;
    panel.open=false;
    settingsChanged();
    S_StartSound(NULL,sfx_swtchx);
}

int sliderLeft() { return (rightColumn-valueColumns-sliderColumns)*cellWidth; }
int sliderWidth() { return (sliderColumns-1)*cellWidth; }

void drawSlider(const Option &option,int row,bool enabled) {
    int x=sliderLeft(),w=sliderWidth(),y=rowY(row)+cellHeight/2;
    int position=(int)std::lround((*option.number-option.low)/(option.high-option.low)*(w-1));
    fill(x,y-1,w,2,track);
    fill(x,y-1,position,2,enabled?trackFill:dim);
    fill(x+position-2,y-5,5,10,enabled?bright:dim);
}
}

void I_Render3DSettingsMenu(void) {
    if(panel.open)return;
    panel.draft=settings;
    panel.tabs=optionTabs(panel.draft);
    selectTab(panel.tab);
    panel.wasPaused=paused;paused=true;
    panel.open=true;
    S_StartSound(NULL,sfx_swtchn);
}

int I_Render3DSettingsOpen(void) { return panel.open; }

void I_Render3DSettingsKey(int scancode,int shift) {
    if(!panel.open)return;
    switch(scancode) {
    case SDL_SCANCODE_UP: moveSelection(-1); break;
    case SDL_SCANCODE_DOWN: moveSelection(1); break;
    case SDL_SCANCODE_LEFT: changeOption(-1); break;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
        changeOption(1); break;
    case SDL_SCANCODE_TAB: selectTab(panel.tab+(shift?-1:1)); S_StartSound(NULL,sfx_pstop); break;
    case SDL_SCANCODE_PAGEUP: selectTab(panel.tab-1); S_StartSound(NULL,sfx_pstop); break;
    case SDL_SCANCODE_PAGEDOWN: selectTab(panel.tab+1); S_StartSound(NULL,sfx_pstop); break;
    case SDL_SCANCODE_R: panel.draft=Settings{}; S_StartSound(NULL,sfx_stnmov); break;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F4: case SDL_SCANCODE_BACKSPACE: closePanel(); break;
    default:
        if(scancode>=SDL_SCANCODE_1&&scancode<SDL_SCANCODE_1+(int)panel.tabs.size()) {
            selectTab(scancode-SDL_SCANCODE_1);S_StartSound(NULL,sfx_pstop);
        }
        break;
    }
}

void I_Render3DSettingsClick(int x,int y,int button) {
    if(!panel.open||x<0||y<0||x>=width||y>=height)return;
    int column=x/cellWidth,row=y/cellHeight;
    if(row==tabRow) {
        if(button!=1)return;
        for(size_t i=0;i<panel.tabColumns.size();++i)
            if(column>=panel.tabColumns[i].first-1&&column<=panel.tabColumns[i].second+1) {
                if((int)i!=panel.tab) {selectTab((int)i);S_StartSound(NULL,sfx_pstop);}
                return;
            }
        return;
    }
    int index=panel.scroll+row-listRow;
    if(row<listRow||row>=listRow+visibleLines||index>=(int)panel.lines.size()||!panel.lines[(size_t)index].option)return;
    panel.selected=index;
    if(button==0)return; // Hovering highlights the option.
    Option &option=selectedOption();
    if(option.kind==OptionKind::Slider&&button==1&&x>=sliderLeft()-cellWidth) {
        if(!option.isEnabled(panel.draft)) {S_StartSound(NULL,sfx_oof);return;}
        double fraction=(double)(x-sliderLeft())/(sliderWidth()-1);
        setSlider(option,option.low+std::clamp(fraction,0.0,1.0)*(option.high-option.low));
        S_StartSound(NULL,sfx_stnmov);
    } else changeOption(button==1?1:-1);
}

void I_Render3DSettingsDraw(unsigned *pixels) {
    if(!panel.open)return;
    image=pixels;
    fill(0,0,width,height,backdrop);

    // Tabs across the top, the current one on a red bar.
    panel.tabColumns.clear();
    int column=leftColumn;
    for(size_t i=0;i<panel.tabs.size();++i) {
        std::string title=panel.tabs[i].title;
        bool current=(int)i==panel.tab;
        if(current)fill((column-1)*cellWidth,rowY(tabRow),((int)title.size()+2)*cellWidth,cellHeight,tabBar);
        drawText(title,column,tabRow,current?bright:description);
        panel.tabColumns.push_back({column,column+(int)title.size()-1});
        column+=(int)title.size()+4;
    }
    std::string hint="F4 Graphics";
    drawText(hint,rightColumn-(int)hint.size(),tabRow,dim);
    drawSeparator(tabRow+1);

    for(int i=0;i<visibleLines&&panel.scroll+i<(int)panel.lines.size();++i) {
        const Line &line=panel.lines[(size_t)(panel.scroll+i)];
        int row=listRow+i;
        if(!line.option) {drawText(line.section->title,leftColumn,row,heading);continue;}
        const Option &option=*line.option;
        bool selected=panel.scroll+i==panel.selected,enabled=option.isEnabled(panel.draft);
        if(selected)fill((leftColumn+1)*cellWidth,rowY(row),(rightColumn-leftColumn)*cellWidth,cellHeight,selectionBar);
        unsigned color=!enabled?dim:selected?bright:text;
        drawText(option.title,leftColumn+2,row,color);
        std::string value=valueText(option);
        unsigned valueColor=enabled&&option.kind==OptionKind::Check&&*option.flag?onColor:color;
        drawText(value,rightColumn-(int)value.size(),row,valueColor);
        if(option.kind==OptionKind::Slider)drawSlider(option,row,enabled);
    }
    if(panel.scroll>0)drawText("^",rightColumn+1,listRow,dim);
    if(panel.scroll+visibleLines<(int)panel.lines.size())drawText("v",rightColumn+1,listRow+visibleLines-1,dim);
    drawSeparator(descriptionRow-1);

    const Option &option=selectedOption();
    std::vector<std::string> lines=wrap(option.tip?option.tip:"",(size_t)(rightColumn-leftColumn),descriptionLines);
    for(size_t i=0;i<lines.size();++i)drawText(lines[i],leftColumn,descriptionRow+(int)i,description);
    std::string help="Up/Down select   Left/Right change   Tab page   R defaults   Esc done";
    drawText(help,(columns-(int)help.size())/2,helpRow,dim);
    // The renderer repeats the border texels beyond the image, so they stay plain backdrop.
    fill(0,0,width,1,backdrop);fill(0,height-1,width,1,backdrop);
    fill(0,0,1,height,backdrop);fill(width-1,0,1,height,backdrop);
}

#ifndef __APPLE__
void I_Render3DSettings(void) { I_Render3DSettingsMenu(); }
#endif
