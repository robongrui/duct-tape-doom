/* Native macOS graphics settings dialog for the 3D renderer (F4). */
extern "C" {
#include "doomtype.h"
extern boolean paused;
}
#include <SDL3/SDL.h>
#import <Cocoa/Cocoa.h>
#include "i_render3d.h"
#include "scene3d.h"

using namespace doom3d;

// Enables the performance switches only while baked static lights are on.
@interface DoomBakeDependents : NSObject
@property(nonatomic,strong) NSArray<NSButton*> *buttons;
- (void)bakeChanged:(NSButton *)sender;
@end
@implementation DoomBakeDependents
- (void)bakeChanged:(NSButton *)sender {
    for(NSButton *button in self.buttons)button.enabled=sender.state==NSControlStateValueOn;
}
@end

void I_Render3DSettings(void) {
    @autoreleasepool {
        NSAlert *alert=[NSAlert new];alert.messageText=@"Graphics";
        alert.informativeText=@"The 3D renderer draws the world at your window's pixel resolution.\nMouse look changes the view; combat keeps DOOM's automatic aiming.";
        [alert addButtonWithTitle:@"Done"];[alert addButtonWithTitle:@"Reset"];
        NSView *view=[[NSView alloc]initWithFrame:NSMakeRect(0,0,800,802)];
        auto label=[&](NSString *text,CGFloat y){NSTextField *field=[NSTextField labelWithString:text];field.frame=NSMakeRect(0,y,145,24);[view addSubview:field];};
        label(@"Renderer",568);NSPopUpButton *renderer=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(150,565,235,28) pullsDown:NO];
        [renderer addItemsWithTitles:@[@"Accelerated 3D",@"Classic software"]];[renderer selectItemAtIndex:settings.accelerated?0:1];[view addSubview:renderer];
        label(@"Resolution",533);NSPopUpButton *resolution=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(150,530,235,28) pullsDown:NO];
        [resolution addItemsWithTitles:@[@"Native (100%)",@"75%",@"50%"]];[resolution selectItemAtIndex:settings.scale==75?1:settings.scale==50?2:0];[view addSubview:resolution];
        label(@"Field of view",463);NSSlider *fov=[NSSlider sliderWithValue:settings.fov minValue:60 maxValue:120 target:nil action:nil];
        fov.frame=NSMakeRect(150,463,235,24);[view addSubview:fov];
        label(@"Sprites",498);NSPopUpButton *spriteMode=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(150,495,235,28) pullsDown:NO];
        [spriteMode addItemsWithTitles:@[@"Crisp pixels",@"Soft (bilinear)",@"Pixel-art upscale (xBR)"]];
        [spriteMode selectItemAtIndex:settings.spriteFilter];[view addSubview:spriteMode];
        NSArray<NSString*> *names=@[@"Widescreen",@"Crosshair",@"Mouse look",@"Retro scanlines",@"Show FPS in window title",@"Emissive textures"];
        int values[]={settings.widescreen,settings.crosshair,settings.look,settings.retro,settings.fps,settings.emissive};
        NSMutableArray<NSButton*> *checks=[NSMutableArray new];
        for(int i=0;i<6;++i) {
            NSButton *button=[NSButton checkboxWithTitle:names[i] target:nil action:nil];
            // The second row holds the wall and floor filter.
            button.frame=NSMakeRect(0,423-(i?i+1:0)*31,385,25);button.state=values[i]?NSControlStateValueOn:NSControlStateValueOff;
            [checks addObject:button];[view addSubview:button];
        }
        label(@"Walls and floors",392);NSPopUpButton *wallFilter=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(150,389,235,28) pullsDown:NO];
        [wallFilter addItemsWithTitles:@[@"Crisp pixels",@"Smooth (bilinear)",@"Sharp bilinear"]];[wallFilter selectItemAtIndex:settings.filter];
        wallFilter.toolTip=@"Sharp bilinear keeps every texel flat and crisp and only softens the step between texels, "
            "by about a screen pixel plus the edge softness set on the right.";
        [view addSubview:wallFilter];
        label(@"Flashlight tint",201);
        NSSlider *tint=[NSSlider sliderWithValue:settings.flashlightTintGain minValue:0 maxValue:2 target:nil action:nil];
        tint.frame=NSMakeRect(150,201,235,24);tint.numberOfTickMarks=5;
        tint.toolTip=@"Subtle target-color tint: left is off, middle is the default, right is stronger.";
        [view addSubview:tint];
        NSButton *fog=[NSButton checkboxWithTitle:@"Atmospheric fog" target:nil action:nil];
        fog.frame=NSMakeRect(0,165,385,25);fog.state=settings.fog?NSControlStateValueOn:NSControlStateValueOff;
        fog.toolTip=@"Height fog and soft cloud layers derived from pits, liquids and damaging floors.";
        [view addSubview:fog];
        NSButton *paletteMode=[NSButton checkboxWithTitle:@"Doom palette colors" target:nil action:nil];
        paletteMode.frame=NSMakeRect(0,132,385,25);paletteMode.state=settings.palette?NSControlStateValueOn:NSControlStateValueOff;
        paletteMode.toolTip=@"Quantize the final lighting and fog to the current WAD palette.";
        [view addSubview:paletteMode];
        NSButton *detail=[NSButton checkboxWithTitle:@"Surface detail" target:nil action:nil];
        detail.frame=NSMakeRect(0,101,385,25);detail.state=settings.detail?NSControlStateValueOn:NSControlStateValueOff;
        detail.toolTip=@"Bump detail and metal gloss derived from the WAD artwork; affects dynamic lights, and static ones with bake-only lights.";
        [view addSubview:detail];
        NSButton *softLight=[NSButton checkboxWithTitle:@"Soft light seams and contact shading" target:nil action:nil];
        softLight.frame=NSMakeRect(0,70,385,25);softLight.state=settings.softLight?NSControlStateValueOn:NSControlStateValueOff;
        softLight.toolTip=@"Blend sector light across openings and darken floors along walls.";
        [view addSubview:softLight];
        NSButton *reflections=[NSButton checkboxWithTitle:@"Liquid reflections" target:nil action:nil];
        reflections.frame=NSMakeRect(0,39,385,25);reflections.state=settings.reflections?NSControlStateValueOn:NSControlStateValueOff;
        reflections.toolTip=@"Mirror the scene in nearby water, nukage, slime and blood; renders the view a second time at half resolution.";
        [view addSubview:reflections];
        NSButton *retroReflections=[NSButton checkboxWithTitle:@"Retro reflections (PS1 style)" target:nil action:nil];
        retroReflections.frame=NSMakeRect(0,8,385,25);retroReflections.state=settings.retroReflections?NSControlStateValueOn:NSControlStateValueOff;
        retroReflections.toolTip=@"With liquid reflections on: quarter resolution, hard pixels, Doom palette colors and a stepped wobble.";
        [view addSubview:retroReflections];
        // Lift the controls above to make room for the baked lighting and caustics rows at the bottom.
        for(NSView *subview in view.subviews)[subview setFrameOrigin:NSMakePoint(subview.frame.origin.x,subview.frame.origin.y+124)];
        NSButton *caustics=[NSButton checkboxWithTitle:@"Liquid caustics" target:nil action:nil];
        caustics.frame=NSMakeRect(0,101,385,25);caustics.state=settings.caustics?NSControlStateValueOn:NSControlStateValueOff;
        caustics.toolTip=@"Rippling light from water, nukage, slime, blood and lava on the walls around them; pattern taken from the level's liquid flat.";
        [view addSubview:caustics];
        NSButton *sun=[NSButton checkboxWithTitle:@"Sun shadows" target:nil action:nil];
        sun.frame=NSMakeRect(0,70,385,25);sun.state=settings.sun?NSControlStateValueOn:NSControlStateValueOff;
        sun.toolTip=@"Shadows and sunlit patches baked at level load from the sky texture and map geometry.";
        [view addSubview:sun];
        NSButton *bakedLights=[NSButton checkboxWithTitle:@"Baked static lights" target:nil action:nil];
        bakedLights.frame=NSMakeRect(0,39,385,25);bakedLights.state=settings.bakedLights?NSControlStateValueOn:NSControlStateValueOff;
        bakedLights.toolTip=@"Torches, lamps and glowing textures light the whole level, baked at level load; nearby ones still flicker.";
        [view addSubview:bakedLights];
        NSButton *bounce=[NSButton checkboxWithTitle:@"Bounce light" target:nil action:nil];
        bounce.frame=NSMakeRect(0,8,385,25);bounce.state=settings.bounce?NSControlStateValueOn:NSControlStateValueOff;
        bounce.toolTip=@"Sunlit and lamp-lit surfaces tint and lift what faces them, including ceilings; baked at level load.";
        [view addSubview:bounce];
        // Experimental effects in a second column, one switch each so any can be walked back.
        NSTextField *heading=[NSTextField labelWithString:@"Experimental effects"];
        heading.font=[NSFont boldSystemFontOfSize:NSFont.systemFontSize];heading.frame=NSMakeRect(410,692,385,24);[view addSubview:heading];
        struct Effect { NSString *title,*tip; int *value; };
        const Effect effects[]={
            {@"Blood splats and pools",@"Hitscan hits leave splats on floors and nearby walls; corpses grow a pool. Colors come from the WAD's blood sprite.",&settings.blood},
            {@"Wet blood shine",@"Fresh blood on the floor glints in a brighter palette red under dynamic lights, then dries matte.",&settings.bloodShine},
            {@"Flashlight silhouettes",@"Things caught in the flashlight throw their sprite outline onto the wall behind them.",&settings.flashlightShadows},
            {@"Soft effect sprites",@"Explosions, puffs and teleport fog stop slicing into walls and dissolve on a dither at floors.",&settings.softSprites},
            {@"Heat haze",@"Rows of pixels shimmer above lava, hot damaging floors and flames.",&settings.heatHaze},
            {@"Eye adaptation",@"Brief stepped over- or underexposure when you move between bright and dark areas.",&settings.eyeAdaptation},
            {@"Liquid splashes",@"Droplets and ripple rings where things land, explode or wade in liquids; rings disturb reflections.",&settings.splashes},
            {@"Sunbeams",@"Soft shafts of sunlight slant down through ceiling holes and windows, where the dust motes glint. Cheap: no shadows, just glowing ribbons.",&settings.sunShafts},
            {@"Sun in the sky",@"A faint sun disc and soft glow where the baked sunlight comes from, over the sky texture's brightest part.",&settings.sunDisc},
            {@"Dust motes",@"Specks that glint in indoor sunbeams and the flashlight beam.",&settings.dust},
            {@"Player shadow",@"Your own sprite's shadow from the sun or the strongest light above you.",&settings.playerShadow},
            {@"Door light spill",@"Light from a brighter room spills through a door as it opens.",&settings.doorLight},
            {@"Texel-aligned baked light",@"Baked lights, sun patches and bounce blend smoothly but step per texture pixel, instead of in hard 4-5 unit map cells.",&settings.texelLight},
            {@"Sky light",@"A cool fill from the visible sky, tinted by the sky texture: windows glow faintly, overhangs and alcoves outdoors stay darker. Baked at level load.",&settings.skyLight},
            {@"Baked ambient occlusion",@"Corners, ledges, stair risers and alcoves darken in light-level steps. Baked at level load.",&settings.bakedAO},
            {@"Decoration shadows",@"Columns, trees and hanging bodies cast baked sun and lamp shadows shaped like their sprites. Changing it re-bakes the level.",&settings.thingShadows},
            {@"Sector light flow",@"Brighter rooms light the floors and walls beyond their openings; flickering rooms flicker faintly next door.",&settings.lightFlow},
            {@"Ceiling caustics",@"With liquid caustics on, the ripples also play on the ceiling above water, nukage, slime and lava.",&settings.ceilingCaustics},
            {@"Glossy monitor screens",@"Screens found in computer textures get curved CRT glass: the picture bends and shifts behind it as you move, and it catches sharp highlights from lamps, shots and the flashlight.",&settings.glossyScreens}};
        NSMutableArray<NSButton*> *effectChecks=[NSMutableArray new];
        for(size_t i=0;i<sizeof(effects)/sizeof(effects[0]);++i) {
            NSButton *button=[NSButton checkboxWithTitle:effects[i].title target:nil action:nil];
            button.frame=NSMakeRect(410,660-(CGFloat)i*31,385,25);button.state=*effects[i].value?NSControlStateValueOn:NSControlStateValueOff;
            button.toolTip=effects[i].tip;[effectChecks addObject:button];[view addSubview:button];
        }
        // Performance: cheaper stand-ins for per-frame light work, below the effects.
        NSTextField *performance=[NSTextField labelWithString:@"Performance"];
        performance.font=[NSFont boldSystemFontOfSize:NSFont.systemFontSize];performance.frame=NSMakeRect(410,119,385,24);[view addSubview:performance];
        NSButton *bakeOnly=[NSButton checkboxWithTitle:@"Bake-only static lights" target:nil action:nil];
        bakeOnly.frame=NSMakeRect(410,87,385,25);bakeOnly.state=settings.bakeOnlyLights?NSControlStateValueOn:NSControlStateValueOff;
        bakeOnly.toolTip=@"Torches, lamps and glowing textures stop costing per-frame light work: their flicker, bump detail and gloss come from the bake, "
            "and lava and nukage pools glow as one light each. Shots, explosions and the flashlight stay dynamic. Needs baked static lights; changing it re-bakes the level.";
        [view addSubview:bakeOnly];
        NSButton *gridLight=[NSButton checkboxWithTitle:@"Grid lighting for monsters" target:nil action:nil];
        gridLight.frame=NSMakeRect(410,56,385,25);gridLight.state=settings.gridSpriteLight?NSControlStateValueOn:NSControlStateValueOff;
        gridLight.toolTip=@"Monsters and items take static light, and the light their shadow falls from, from a grid baked at level load "
            "instead of tracing rays every frame. Needs baked static lights.";
        [view addSubview:gridLight];
        DoomBakeDependents *dependents=[DoomBakeDependents new];dependents.buttons=@[bakeOnly,gridLight];
        bakedLights.target=dependents;bakedLights.action=@selector(bakeChanged:);[dependents bakeChanged:bakedLights];
        // Texture filtering and detail textures: room at the bottom of the right column.
        for(NSView *subview in view.subviews)[subview setFrameOrigin:NSMakePoint(subview.frame.origin.x,subview.frame.origin.y+80)];
        auto rowLabel=[&](NSString *title,CGFloat y) {
            NSTextField *field=[NSTextField labelWithString:title];field.frame=NSMakeRect(410,y,145,24);[view addSubview:field];
        };
        auto rowSlider=[&](NSString *title,double value,double low,double high,CGFloat y,NSString *tip) {
            rowLabel(title,y);
            NSSlider *slider=[NSSlider sliderWithValue:value minValue:low maxValue:high target:nil action:nil];
            slider.frame=NSMakeRect(560,y,235,24);slider.toolTip=tip;[view addSubview:slider];
            return slider;
        };
        NSSlider *sharpSoftness=rowSlider(@"Edge softness",settings.sharpSoftness,0,1,101,
            @"With sharp bilinear walls and floors: how far texel edges blend, in texels. Left keeps only a one-pixel edge; right approaches bilinear.");
        NSButton *paletteMips=[NSButton checkboxWithTitle:@"Palette mipmaps for distant textures" target:nil action:nil];
        paletteMips.frame=NSMakeRect(410,70,385,25);paletteMips.state=settings.paletteMips?NSControlStateValueOn:NSControlStateValueOff;
        paletteMips.toolTip=@"With smooth or sharp walls and floors: far and grazing surfaces read smaller versions snapped to the WAD palette, "
            "as software Quake did, instead of shimmering. Crisp pixels stay vanilla.";
        [view addSubview:paletteMips];
        rowLabel(@"Detail textures",39);
        NSPopUpButton *detailMode=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(560,36,235,28) pullsDown:NO];
        [detailMode addItemsWithTitles:@[@"Off",@"With smooth or sharp walls",@"Always"]];[detailMode selectItemAtIndex:settings.detailTextures];
        detailMode.toolTip=@"A fine grain over walls and floors that shows only up close and fades out with distance, picked per texture "
            "from its colors (stone, metal, wood, flesh). Scale and distance are in graphics.cfg (detail_scale, detail_fade).";
        [view addSubview:detailMode];
        NSSlider *detailStrength=rowSlider(@"Detail strength",settings.detailStrength,0.05,0.6,8,@"How far the grain lightens and darkens the artwork.");
        alert.accessoryView=view;bool wasPaused=paused;paused=true;
        SDL_SetWindowRelativeMouseMode(gameWindow,false);
        NSInteger response=[alert runModal];paused=wasPaused;
        if(response==NSAlertSecondButtonReturn)settings=Settings{};
        else {
            settings.accelerated=renderer.indexOfSelectedItem==0;
            settings.scale=resolution.indexOfSelectedItem==1?75:resolution.indexOfSelectedItem==2?50:100;
            settings.fov=fov.floatValue;
            settings.flashlightTintGain=tint.floatValue;
            settings.fog=fog.state==NSControlStateValueOn;
            settings.palette=paletteMode.state==NSControlStateValueOn;
            settings.detail=detail.state==NSControlStateValueOn;
            settings.softLight=softLight.state==NSControlStateValueOn;
            settings.reflections=reflections.state==NSControlStateValueOn;
            settings.retroReflections=retroReflections.state==NSControlStateValueOn;
            settings.sun=sun.state==NSControlStateValueOn;
            settings.bakedLights=bakedLights.state==NSControlStateValueOn;
            settings.bounce=bounce.state==NSControlStateValueOn;
            settings.caustics=caustics.state==NSControlStateValueOn;
            settings.bakeOnlyLights=bakeOnly.state==NSControlStateValueOn;
            settings.gridSpriteLight=gridLight.state==NSControlStateValueOn;
            settings.detailTextures=(int)detailMode.indexOfSelectedItem;
            settings.detailStrength=detailStrength.floatValue;
            settings.sharpSoftness=sharpSoftness.floatValue;settings.paletteMips=paletteMips.state==NSControlStateValueOn;
            settings.spriteFilter=(int)spriteMode.indexOfSelectedItem;
            settings.widescreen=checks[0].state==NSControlStateValueOn;settings.filter=(int)wallFilter.indexOfSelectedItem;
            settings.crosshair=checks[1].state==NSControlStateValueOn;settings.look=checks[2].state==NSControlStateValueOn;
            settings.retro=checks[3].state==NSControlStateValueOn;settings.fps=checks[4].state==NSControlStateValueOn;
            settings.emissive=checks[5].state==NSControlStateValueOn;
            for(size_t i=0;i<sizeof(effects)/sizeof(effects[0]);++i)*effects[i].value=effectChecks[i].state==NSControlStateValueOn;
        }
        settingsChanged();
    }
}
