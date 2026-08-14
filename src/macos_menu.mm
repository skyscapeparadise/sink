#import <Cocoa/Cocoa.h>
#include "macos_menu.h"
#include "preset_manager.hpp"
#include <atomic>
#include <cstring>
#include <vector>
#include <string>

static std::atomic<bool> g_settings_requested{false};
static std::atomic<bool> g_cut_requested{false};
static std::atomic<bool> g_copy_requested{false};
static std::atomic<bool> g_paste_requested{false};
static std::atomic<bool> g_select_all_requested{false};
static std::atomic<bool> g_new_window_requested{false};
static std::atomic<bool> g_new_tab_requested{false};
static std::atomic<bool> g_close_window_requested{false};
static std::atomic<bool> g_print_requested{false};
static std::atomic<bool> g_find_requested{false};
static std::atomic<bool> g_crt_mode_requested{false};

void set_settings_requested(bool requested) {
    g_settings_requested = requested;
}

bool get_settings_requested() {
    return g_settings_requested.exchange(false);
}

void set_cut_requested(bool req) {
    g_cut_requested = req;
}

bool get_cut_requested() {
    return g_cut_requested.exchange(false);
}

void set_copy_requested(bool req) {
    g_copy_requested = req;
}

bool get_copy_requested() {
    return g_copy_requested.exchange(false);
}

void set_paste_requested(bool req) {
    g_paste_requested = req;
}

bool get_paste_requested() {
    return g_paste_requested.exchange(false);
}

void set_select_all_requested(bool req) {
    g_select_all_requested = req;
}

bool get_select_all_requested() {
    return g_select_all_requested.exchange(false);
}

void set_new_window_requested(bool req) {
    g_new_window_requested = req;
}

bool get_new_window_requested() {
    return g_new_window_requested.exchange(false);
}

void set_new_tab_requested(bool req) {
    g_new_tab_requested = req;
}

bool get_new_tab_requested() {
    return g_new_tab_requested.exchange(false);
}

void set_close_window_requested(bool req) {
    g_close_window_requested = req;
}

bool get_close_window_requested() {
    return g_close_window_requested.exchange(false);
}

void set_print_requested(bool req) {
    g_print_requested = req;
}

bool get_print_requested() {
    return g_print_requested.exchange(false);
}

void set_find_requested(bool req) {
    g_find_requested = req;
}

bool get_find_requested() {
    return g_find_requested.exchange(false);
}

void set_crt_mode_requested(bool req) {
    g_crt_mode_requested = req;
}

bool get_crt_mode_requested() {
    return g_crt_mode_requested.exchange(false);
}

static std::atomic<bool> g_split_pane_right_requested{false};
static std::atomic<bool> g_split_pane_down_requested{false};
static std::atomic<bool> g_close_pane_requested{false};
static std::atomic<int> g_pane_focus_requested{0};

void set_split_pane_right_requested(bool req) {
    g_split_pane_right_requested = req;
}

bool get_split_pane_right_requested() {
    return g_split_pane_right_requested.exchange(false);
}

void set_split_pane_down_requested(bool req) {
    g_split_pane_down_requested = req;
}

bool get_split_pane_down_requested() {
    return g_split_pane_down_requested.exchange(false);
}

void set_close_pane_requested(bool req) {
    g_close_pane_requested = req;
}

bool get_close_pane_requested() {
    return g_close_pane_requested.exchange(false);
}

void set_pane_focus_requested(int direction) {
    g_pane_focus_requested = direction;
}

int get_pane_focus_requested() {
    return g_pane_focus_requested.exchange(0);
}

// C-linkage trigger to invoke the iteration loop frame update
extern "C" void trigger_menu_render_tick();

@interface MenuHandler : NSObject <NSMenuDelegate>
@property (nonatomic, strong) NSTimer* menuTimer;
- (void)openSettings:(id)sender;
- (void)cut:(id)sender;
- (void)copy:(id)sender;
- (void)paste:(id)sender;
- (void)selectAll:(id)sender;
- (void)find:(id)sender;
- (void)newWindowWithPreset:(id)sender;
- (void)newTabWithPreset:(id)sender;
- (void)closeWindow:(id)sender;
- (void)print:(id)sender;
- (void)splitPaneRight:(id)sender;
- (void)splitPaneDown:(id)sender;
- (void)closePane:(id)sender;
- (void)selectPaneInDirection:(id)sender;
- (void)onMenuTimer:(NSTimer*)timer;
@end

@implementation MenuHandler

- (void)openSettings:(id)sender {
    set_settings_requested(true);
}

- (void)cut:(id)sender {
    set_cut_requested(true);
}

- (void)copy:(id)sender {
    set_copy_requested(true);
}

- (void)paste:(id)sender {
    set_paste_requested(true);
}

- (void)selectAll:(id)sender {
    set_select_all_requested(true);
}

- (void)find:(id)sender {
    set_find_requested(true);
}

- (void)newWindowWithPreset:(id)sender {
    NSString* name = [sender representedObject];
    if (!name) return;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_USER;
    ev.user.code = 15; // New window with preset (see main.cpp)
    ev.user.data1 = strdup([name UTF8String]);
    SDL_PushEvent(&ev);
}

- (void)newTabWithPreset:(id)sender {
    NSString* name = [sender representedObject];
    if (!name) return;
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_USER;
    ev.user.code = 16; // New tab with preset (see main.cpp)
    ev.user.data1 = strdup([name UTF8String]);
    SDL_PushEvent(&ev);
}

// Repopulates the "New Window/Tab with Preset" submenus right before they're
// shown, so they always reflect whatever presets currently exist on disk
// (created/renamed/deleted via the Settings panel) without needing a
// restart. `menu.title` (set at construction, not shown to the user) tells
// the two submenus apart since they share this one delegate.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    SEL action;
    if ([menu.title isEqualToString:@"NewWindowPresetMenu"]) {
        action = @selector(newWindowWithPreset:);
    } else if ([menu.title isEqualToString:@"NewTabPresetMenu"]) {
        action = @selector(newTabWithPreset:);
    } else {
        return;
    }

    [menu removeAllItems];
    std::vector<std::string> names = presets::list_names();
    if (names.empty()) {
        NSMenuItem* empty = [[NSMenuItem alloc] initWithTitle:@"No Presets" action:nil keyEquivalent:@""];
        [empty setEnabled:NO];
        [menu addItem:empty];
        return;
    }
    for (const std::string& name : names) {
        NSString* nsName = [NSString stringWithUTF8String:name.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:nsName action:action keyEquivalent:@""];
        [item setTarget:self];
        [item setEnabled:YES];
        [item setRepresentedObject:nsName];
        [menu addItem:item];
    }
}

- (void)closeWindow:(id)sender {
    set_close_window_requested(true);
}

- (void)splitPaneRight:(id)sender {
    set_split_pane_right_requested(true);
}

- (void)splitPaneDown:(id)sender {
    set_split_pane_down_requested(true);
}

- (void)closePane:(id)sender {
    set_close_pane_requested(true);
}

// Direction is carried in the menu item's tag (1=left 2=right 3=up 4=down)
- (void)selectPaneInDirection:(id)sender {
    set_pane_focus_requested((int)[sender tag]);
}

- (void)print:(id)sender {
    set_print_requested(true);
}

- (void)onMenuTimer:(NSTimer*)timer {
    trigger_menu_render_tick();
}

- (void)dealloc {
    if (self.menuTimer) {
        [self.menuTimer invalidate];
        self.menuTimer = nil;
    }
#if !__has_feature(objc_arc)
    [super dealloc];
#endif
}

@end

static MenuHandler* g_menu_handler = nil;

void setup_macos_menu() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        NSMenu* mainMenu = [app mainMenu];
        if (!mainMenu) return;

        // The application main menu is the first menu (index 0)
        NSMenuItem* appMenuItem = [mainMenu itemAtIndex:0];
        if (!appMenuItem) return;

        NSMenu* appMenu = [appMenuItem submenu];
        if (!appMenu) return;

        // Create Cocoa target delegate if not exists
        if (!g_menu_handler) {
            g_menu_handler = [[MenuHandler alloc] init];
            
            // Spin up a permanent 60Hz timer running under NSRunLoopCommonModes
            // to ensure video playback & rendering never pauses during AppKit blocking loops
            // (menu bar tracking, window dragging/resizing, zoom button tile popups, dialogs, etc.)
            g_menu_handler.menuTimer = [NSTimer timerWithTimeInterval:1.0/60.0
                                                               target:g_menu_handler
                                                             selector:@selector(onMenuTimer:)
                                                             userInfo:nil
                                                              repeats:YES];
            [[NSRunLoop mainRunLoop] addTimer:g_menu_handler.menuTimer forMode:NSRunLoopCommonModes];
        }

        // Disable auto-enabling items so that Cocoa doesn't automatically gray out our custom target
        [appMenu setAutoenablesItems:NO];

        // Search for any existing Settings/Preferences menu item to hijack and enable
        bool settings_hijacked = false;
        for (NSMenuItem* item in [appMenu itemArray]) {
            NSString* title = [item title];
            if ([title isEqualToString:@"Settings…"] || 
                [title isEqualToString:@"Preferences…"] || 
                [title isEqualToString:@"Settings..."] || 
                [title isEqualToString:@"Preferences..."]) {
                
                [item setTarget:g_menu_handler];
                [item setAction:@selector(openSettings:)];
                [item setEnabled:YES];
                settings_hijacked = true;
                break;
            }
        }

        if (!settings_hijacked) {
            // If no settings menu item exists, create and insert it at index 2
            [appMenu insertItem:[NSMenuItem separatorItem] atIndex:1];
            
            NSMenuItem* settingsItem = [[NSMenuItem alloc] initWithTitle:@"Settings…"
                                                                  action:@selector(openSettings:)
                                                           keyEquivalent:@","];
            [settingsItem setTarget:g_menu_handler];
            [settingsItem setEnabled:YES];
            [appMenu insertItem:settingsItem atIndex:2];
        }

        // Setup the File Menu (Insert at index 1)
        NSMenuItem* fileItem = nil;
        for (NSMenuItem* item in [mainMenu itemArray]) {
            if ([[item title] isEqualToString:@"File"]) {
                fileItem = item;
                break;
            }
        }
        
        if (!fileItem) {
            fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
            NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
            [fileItem setSubmenu:fileMenu];
            [mainMenu insertItem:fileItem atIndex:1];
        }
        
        NSMenu* fileMenu = [fileItem submenu];
        [fileMenu removeAllItems];
        [fileMenu setAutoenablesItems:NO];
        
        NSMenuItem* closeWindowItem = [[NSMenuItem alloc] initWithTitle:@"Close Window" action:@selector(closeWindow:) keyEquivalent:@"w"];
        NSMenuItem* printItem = [[NSMenuItem alloc] initWithTitle:@"Print…" action:@selector(print:) keyEquivalent:@"p"];

        [closeWindowItem setTarget:g_menu_handler];
        [closeWindowItem setEnabled:YES];

        [printItem setTarget:g_menu_handler];
        [printItem setEnabled:YES];

        // "New Window"/"New Tab" are these preset submenus, not plain
        // items -- picking a preset from the list opens a window/tab with
        // that preset applied. The ⌘N/⌘T equivalents are display-only
        // (AppKit stops treating an item as actionable once it has a
        // submenu); Cmd+N/Cmd+T are handled directly in main.cpp's SDL key
        // event loop instead, hitting the same "with whatever preset is
        // currently active" path. Submenu contents are populated lazily by
        // -menuNeedsUpdate: right before they're shown (see MenuHandler) so
        // a preset created/renamed/deleted in Settings shows up without
        // restarting the app. Each submenu's *own* title (not shown to the
        // user -- the parent NSMenuItem's title is what's displayed) is how
        // the shared delegate tells the two apart.
        NSMenu* newWindowPresetMenu = [[NSMenu alloc] initWithTitle:@"NewWindowPresetMenu"];
        [newWindowPresetMenu setDelegate:g_menu_handler];
        [newWindowPresetMenu setAutoenablesItems:NO];
        NSMenuItem* newWindowItem = [[NSMenuItem alloc] initWithTitle:@"New Window" action:nil keyEquivalent:@"n"];
        [newWindowItem setSubmenu:newWindowPresetMenu];

        NSMenu* newTabPresetMenu = [[NSMenu alloc] initWithTitle:@"NewTabPresetMenu"];
        [newTabPresetMenu setDelegate:g_menu_handler];
        [newTabPresetMenu setAutoenablesItems:NO];
        NSMenuItem* newTabItem = [[NSMenuItem alloc] initWithTitle:@"New Tab" action:nil keyEquivalent:@"t"];
        [newTabItem setSubmenu:newTabPresetMenu];

        [fileMenu addItem:newWindowItem];
        [fileMenu addItem:newTabItem];
        [fileMenu addItem:[NSMenuItem separatorItem]];
        [fileMenu addItem:closeWindowItem];
        [fileMenu addItem:[NSMenuItem separatorItem]];
        [fileMenu addItem:printItem];

        // Setup the Edit Menu (Insert at index 2)
        NSMenuItem* editItem = nil;
        for (NSMenuItem* item in [mainMenu itemArray]) {
            if ([[item title] isEqualToString:@"Edit"]) {
                editItem = item;
                break;
            }
        }
        
        if (!editItem) {
            editItem = [[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""];
            NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
            [editItem setSubmenu:editMenu];
            [mainMenu insertItem:editItem atIndex:2];
        }
        
        NSMenu* editMenu = [editItem submenu];
        [editMenu removeAllItems];
        [editMenu setAutoenablesItems:NO];
        
        NSMenuItem* cutItem = [[NSMenuItem alloc] initWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        NSMenuItem* copyItem = [[NSMenuItem alloc] initWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        NSMenuItem* pasteItem = [[NSMenuItem alloc] initWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        NSMenuItem* selectAllItem = [[NSMenuItem alloc] initWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        NSMenuItem* findItem = [[NSMenuItem alloc] initWithTitle:@"Find…" action:@selector(find:) keyEquivalent:@"f"];
        
        [cutItem setTarget:g_menu_handler];
        [cutItem setEnabled:YES];
        
        [copyItem setTarget:g_menu_handler];
        [copyItem setEnabled:YES];
        
        [pasteItem setTarget:g_menu_handler];
        [pasteItem setEnabled:YES];
        
        [selectAllItem setTarget:g_menu_handler];
        [selectAllItem setEnabled:YES];
        
        [findItem setTarget:g_menu_handler];
        [findItem setEnabled:YES];
        
        [editMenu addItem:cutItem];
        [editMenu addItem:copyItem];
        [editMenu addItem:pasteItem];
        [editMenu addItem:[NSMenuItem separatorItem]];
        [editMenu addItem:selectAllItem];
        [editMenu addItem:[NSMenuItem separatorItem]];
        [editMenu addItem:findItem];

        // Split-pane commands live in the Window menu: unlike New Window/New
        // Tab (which create a new session), splitting changes this window's
        // *layout* -- the same category as Window's own Minimize/Zoom, so it
        // reads better there than under Shell. SDL's Cocoa backend creates
        // the Window menu itself (with Minimize/Zoom/the open-window list)
        // before this function runs, so it's found rather than built, and
        // its existing items are left alone -- only our block is appended.
        NSMenuItem* windowItem = nil;
        for (NSMenuItem* item in [mainMenu itemArray]) {
            if ([[item title] isEqualToString:@"Window"]) {
                windowItem = item;
                break;
            }
        }

        if (!windowItem) {
            windowItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
            NSMenu* newWindowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
            [windowItem setSubmenu:newWindowMenu];
            [mainMenu addItem:windowItem];
            [NSApp setWindowsMenu:newWindowMenu];
        }

        NSMenu* windowMenu = [windowItem submenu];
        [windowMenu setAutoenablesItems:NO];

        // Our items go in their own block up top, above whatever AppKit
        // manages (Minimize/Zoom appear above the window list historically,
        // but inserting at 0 is robust regardless of exact AppKit ordering)
        NSMenuItem* splitRightItem = [[NSMenuItem alloc] initWithTitle:@"Split Pane Right"
                                                                action:@selector(splitPaneRight:)
                                                         keyEquivalent:@"d"];
        [splitRightItem setTarget:g_menu_handler];
        [splitRightItem setEnabled:YES];

        NSMenuItem* splitDownItem = [[NSMenuItem alloc] initWithTitle:@"Split Pane Down"
                                                               action:@selector(splitPaneDown:)
                                                        keyEquivalent:@"d"];
        [splitDownItem setKeyEquivalentModifierMask:(NSEventModifierFlagCommand | NSEventModifierFlagShift)];
        [splitDownItem setTarget:g_menu_handler];
        [splitDownItem setEnabled:YES];

        NSMenuItem* closePaneItem = [[NSMenuItem alloc] initWithTitle:@"Close Pane"
                                                               action:@selector(closePane:)
                                                        keyEquivalent:@"w"];
        [closePaneItem setKeyEquivalentModifierMask:(NSEventModifierFlagCommand | NSEventModifierFlagShift)];
        [closePaneItem setTarget:g_menu_handler];
        [closePaneItem setEnabled:YES];

        struct PaneDir { NSString* title; unichar key; int tag; };
        PaneDir dirs[] = {
            { @"Select Pane on Left",  NSLeftArrowFunctionKey,  1 },
            { @"Select Pane on Right", NSRightArrowFunctionKey, 2 },
            { @"Select Pane Above",    NSUpArrowFunctionKey,    3 },
            { @"Select Pane Below",    NSDownArrowFunctionKey,  4 },
        };

        NSInteger insertAt = 0;
        [windowMenu insertItem:splitRightItem atIndex:insertAt++];
        [windowMenu insertItem:splitDownItem atIndex:insertAt++];
        for (const PaneDir& dir : dirs) {
            NSString* key = [NSString stringWithCharacters:&dir.key length:1];
            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:dir.title
                                                          action:@selector(selectPaneInDirection:)
                                                   keyEquivalent:key];
            [item setKeyEquivalentModifierMask:(NSEventModifierFlagCommand | NSEventModifierFlagOption)];
            [item setTarget:g_menu_handler];
            [item setEnabled:YES];
            [item setTag:dir.tag];
            [windowMenu insertItem:item atIndex:insertAt++];
        }
        [windowMenu insertItem:closePaneItem atIndex:insertAt++];
        [windowMenu insertItem:[NSMenuItem separatorItem] atIndex:insertAt++];
    }
}

@interface SinkWindowObserver : NSObject
@end

@implementation SinkWindowObserver
+ (void)attachToWindow:(NSWindow*)nswin {
    static SinkWindowObserver* observer = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        observer = [[SinkWindowObserver alloc] init];
    });

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [nc removeObserver:observer name:NSWindowDidResizeNotification object:nswin];
    [nc removeObserver:observer name:NSWindowDidExitFullScreenNotification object:nswin];
    [nc removeObserver:observer name:NSWindowDidEnterFullScreenNotification object:nswin];

    [nc addObserver:observer selector:@selector(windowDidResize:) name:NSWindowDidResizeNotification object:nswin];
    [nc addObserver:observer selector:@selector(windowDidExitFullScreen:) name:NSWindowDidExitFullScreenNotification object:nswin];
    [nc addObserver:observer selector:@selector(windowDidEnterFullScreen:) name:NSWindowDidEnterFullScreenNotification object:nswin];
}

- (void)windowDidResize:(NSNotification*)note {
    NSWindow* win = [note object];
    [self updateLayoutForWindow:win];
}

- (void)windowDidExitFullScreen:(NSNotification*)note {
    NSWindow* win = [note object];
    [win setStyleMask:([win styleMask] | NSWindowStyleMaskFullSizeContentView)];
    [win setTitlebarAppearsTransparent:YES];
    [self updateLayoutForWindow:win];
}

- (void)windowDidEnterFullScreen:(NSNotification*)note {
    NSWindow* win = [note object];
    NSView* contentView = [win contentView];
    for (NSView* subview in [contentView subviews]) {
        if ([subview isKindOfClass:[NSVisualEffectView class]] && subview.frame.size.height <= 36.0) {
            [subview setHidden:YES];
        }
    }
}

- (void)updateLayoutForWindow:(NSWindow*)win {
    NSView* contentView = [win contentView];
    if (!contentView) return;
    NSRect b = [contentView bounds];
    for (NSView* subview in [contentView subviews]) {
        if ([subview isKindOfClass:[NSVisualEffectView class]]) {
            if (subview.frame.size.height <= 36.0) {
                [subview setFrame:NSMakeRect(0, b.size.height - 32, b.size.width, 32)];
                if ([win titleVisibility] == NSWindowTitleVisible) {
                    [subview setHidden:NO];
                }
            } else {
                [subview setFrame:b];
            }
        }
    }
}
@end

void enable_macos_window_vibrancy(SDL_Window* sdl_win, bool enable) {
    @autoreleasepool {
        if (!sdl_win) return;
        SDL_PropertiesID props = SDL_GetWindowProperties(sdl_win);
        NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(props, "SDL.window.cocoa.window", NULL);
        if (!nswin) return;
        
        [SinkWindowObserver attachToWindow:nswin];

        NSView* contentView = [nswin contentView];
        if (!contentView) return;

        NSMutableArray* queue = [NSMutableArray arrayWithObject:contentView];
        while ([queue count] > 0) {
            NSView* v = [queue firstObject];
            [queue removeObjectAtIndex:0];
            [v setWantsLayer:YES];
            if ([v layer]) {
                [[v layer] setOpaque:NO];
                [[v layer] setBackgroundColor:[NSColor clearColor].CGColor];
            }
            [queue addObjectsFromArray:[v subviews]];
        }

        NSVisualEffectView* fullEffectView = nil;
        NSVisualEffectView* titlebarEffectView = nil;

        for (NSView* subview in [contentView subviews]) {
            if ([subview isKindOfClass:[NSVisualEffectView class]]) {
                if (subview.frame.size.height <= 60.0) {
                    titlebarEffectView = (NSVisualEffectView*)subview;
                } else {
                    fullEffectView = (NSVisualEffectView*)subview;
                }
            }
        }

        if (!fullEffectView) {
            fullEffectView = [[NSVisualEffectView alloc] initWithFrame:[contentView bounds]];
            [fullEffectView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [fullEffectView setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
            [fullEffectView setMaterial:NSVisualEffectMaterialHUDWindow];
            [fullEffectView setState:NSVisualEffectStateActive];
            [contentView addSubview:fullEffectView positioned:NSWindowBelow relativeTo:nil];
        }

        float titlebar_h = 32.0f; // Pixel-matched titlebar height (between 28pt and 38pt)
        if (!titlebarEffectView) {
            NSRect b = [contentView bounds];
            titlebarEffectView = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, b.size.height - titlebar_h, b.size.width, titlebar_h)];
            [titlebarEffectView setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
            [titlebarEffectView setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
            [titlebarEffectView setMaterial:NSVisualEffectMaterialHUDWindow];
            [titlebarEffectView setState:NSVisualEffectStateActive];
            [contentView addSubview:titlebarEffectView positioned:NSWindowAbove relativeTo:nil];
        }

        [nswin setOpaque:NO];
        [nswin setBackgroundColor:[NSColor clearColor]];
        [nswin setTitlebarAppearsTransparent:YES];
        [nswin setStyleMask:([nswin styleMask] | NSWindowStyleMaskFullSizeContentView)];

        if (enable) { // title bar: ON (Native Liquid Glass Title Bar over Desktop, Native "sink" Title)
            NSRect b = [contentView bounds];
            [titlebarEffectView setFrame:NSMakeRect(0, b.size.height - titlebar_h, b.size.width, titlebar_h)];
            [titlebarEffectView setMaterial:NSVisualEffectMaterialHUDWindow];
            [titlebarEffectView setHidden:NO];
            [nswin setTitleVisibility:NSWindowTitleVisible];
        } else { // title bar: OFF (Full Bleed Floating Buttons, NO Title)
            [titlebarEffectView setHidden:YES];
            [nswin setTitleVisibility:NSWindowTitleHidden];
        }
    }
}

void zoom_macos_window(SDL_Window* sdl_win) {
    @autoreleasepool {
        if (!sdl_win) return;
        SDL_PropertiesID props = SDL_GetWindowProperties(sdl_win);
        NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(props, "SDL.window.cocoa.window", NULL);
        if (!nswin) return;
        // -performZoom: is what AppKit itself calls when a double-click on a
        // native title bar is detected: toggles between the window's current
        // frame and its zoomed frame, with AppKit's own native animation,
        // and lets AppKit track/restore the "un-zoomed" frame -- avoiding
        // the frame bookkeeping (and window-tiling edge cases) a manual
        // setFrame:-based implementation would need to get right itself.
        [nswin performZoom:nil];
    }
}

void add_window_as_tab(SDL_Window* parent_sdl_win, SDL_Window* child_sdl_win) {
    @autoreleasepool {
        if (!parent_sdl_win || !child_sdl_win) return;
        
        SDL_PropertiesID parent_props = SDL_GetWindowProperties(parent_sdl_win);
        SDL_PropertiesID child_props = SDL_GetWindowProperties(child_sdl_win);
        
        NSWindow* parent_nswin = (__bridge NSWindow*)SDL_GetPointerProperty(parent_props, "SDL.window.cocoa.window", NULL);
        NSWindow* child_nswin = (__bridge NSWindow*)SDL_GetPointerProperty(child_props, "SDL.window.cocoa.window", NULL);
        
        if (parent_nswin && child_nswin) {
            [parent_nswin setTabbingMode:NSWindowTabbingModeAutomatic];
            [child_nswin setTabbingMode:NSWindowTabbingModeAutomatic];
            
            [parent_nswin addTabbedWindow:child_nswin ordered:NSWindowAbove];
            [child_nswin makeKeyAndOrderFront:nil];
        }
    }
}

float get_native_content_top_inset(SDL_Window* sdl_win) {
    @autoreleasepool {
        if (!sdl_win) return -1.0f;
        SDL_PropertiesID props = SDL_GetWindowProperties(sdl_win);
        NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(props, "SDL.window.cocoa.window", NULL);
        if (!nswin) return -1.0f;
        NSView* contentView = [nswin contentView];
        if (!contentView) return -1.0f;
        // contentLayoutRect excludes whatever AppKit is currently drawing
        // above the content -- the title bar strip alone with one tab, or
        // title bar + tab strip once a window has more than one. Diffing
        // its height against the full content view height gives that
        // combined inset without sink needing to know the tab bar's exact
        // (version-dependent, undocumented) height itself.
        NSRect layoutRect = [nswin contentLayoutRect];
        return (float)(contentView.bounds.size.height - layoutRect.size.height);
    }
}

void open_url_if_safe(const char* uri) {
    @autoreleasepool {
        if (!uri) return;
        NSString* uriStr = [NSString stringWithUTF8String:uri];
        if (!uriStr) return; // invalid UTF-8

        NSURL* url = [NSURL URLWithString:uriStr];
        if (!url || !url.scheme) return;

        NSString* scheme = [url.scheme lowercaseString];
        static NSSet<NSString*>* allowedSchemes = [NSSet setWithArray:@[@"http", @"https", @"mailto"]];
        if (![allowedSchemes containsObject:scheme]) return;

        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

void trigger_print_dialog(const char* text_utf8) {
    @autoreleasepool {
        NSString* textStr = [NSString stringWithUTF8String:text_utf8];
        
        // Create an offscreen NSTextView
        NSTextView* textView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 468, 648)];
        [textView setString:textStr];
        [textView setFont:[NSFont fontWithName:@"Courier" size:10.0]];
        
        NSPrintInfo* printInfo = [NSPrintInfo sharedPrintInfo];
        [printInfo setHorizontalPagination:NSPrintingPaginationModeFit];
        [printInfo setVerticalPagination:NSPrintingPaginationModeAutomatic];
        [printInfo setVerticallyCentered:NO];
        [printInfo setHorizontallyCentered:NO];
        
        NSPrintOperation* printOp = [NSPrintOperation printOperationWithView:textView printInfo:printInfo];
        [printOp setShowsPrintPanel:YES];
        
        dispatch_async(dispatch_get_main_queue(), ^{
            [printOp runOperation];
        });
    }
}

std::string get_bundle_resource_path(const std::string& filename) {
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
        NSString* path = [bundle pathForResource:[NSString stringWithUTF8String:filename.c_str()] ofType:nil];
        if (path) {
            return [path UTF8String];
        }
        return filename;
    }
}
