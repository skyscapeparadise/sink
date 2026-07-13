#import <Cocoa/Cocoa.h>
#include "macos_menu.h"
#include <atomic>

static std::atomic<bool> g_settings_requested{false};
static std::atomic<bool> g_copy_requested{false};
static std::atomic<bool> g_paste_requested{false};
static std::atomic<bool> g_select_all_requested{false};
static std::atomic<bool> g_new_window_requested{false};
static std::atomic<bool> g_new_tab_requested{false};
static std::atomic<bool> g_close_window_requested{false};
static std::atomic<bool> g_print_requested{false};

void set_settings_requested(bool requested) {
    g_settings_requested = requested;
}

bool get_settings_requested() {
    return g_settings_requested.exchange(false);
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

// C-linkage trigger to invoke the iteration loop frame update
extern "C" void trigger_menu_render_tick();

@interface MenuHandler : NSObject
@property (nonatomic, strong) NSTimer* menuTimer;
- (void)openSettings:(id)sender;
- (void)cut:(id)sender;
- (void)copy:(id)sender;
- (void)paste:(id)sender;
- (void)selectAll:(id)sender;
- (void)newWindow:(id)sender;
- (void)newTab:(id)sender;
- (void)closeWindow:(id)sender;
- (void)print:(id)sender;
- (void)onMenuTimer:(NSTimer*)timer;
@end

@implementation MenuHandler

- (void)openSettings:(id)sender {
    set_settings_requested(true);
}

- (void)cut:(id)sender {
    set_copy_requested(true);
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

- (void)newWindow:(id)sender {
    set_new_window_requested(true);
}

- (void)newTab:(id)sender {
    set_new_tab_requested(true);
}

- (void)closeWindow:(id)sender {
    set_close_window_requested(true);
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
        
        NSMenuItem* newWindowItem = [[NSMenuItem alloc] initWithTitle:@"New Window" action:@selector(newWindow:) keyEquivalent:@"n"];
        NSMenuItem* newTabItem = [[NSMenuItem alloc] initWithTitle:@"New Tab" action:@selector(newTab:) keyEquivalent:@"t"];
        NSMenuItem* closeWindowItem = [[NSMenuItem alloc] initWithTitle:@"Close Window" action:@selector(closeWindow:) keyEquivalent:@"w"];
        NSMenuItem* printItem = [[NSMenuItem alloc] initWithTitle:@"Print…" action:@selector(print:) keyEquivalent:@"p"];
        
        [newWindowItem setTarget:g_menu_handler];
        [newWindowItem setEnabled:YES];
        
        [newTabItem setTarget:g_menu_handler];
        [newTabItem setEnabled:YES];
        
        [closeWindowItem setTarget:g_menu_handler];
        [closeWindowItem setEnabled:YES];
        
        [printItem setTarget:g_menu_handler];
        [printItem setEnabled:YES];
        
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
        
        [cutItem setTarget:g_menu_handler];
        [cutItem setEnabled:YES];
        
        [copyItem setTarget:g_menu_handler];
        [copyItem setEnabled:YES];
        
        [pasteItem setTarget:g_menu_handler];
        [pasteItem setEnabled:YES];
        
        [selectAllItem setTarget:g_menu_handler];
        [selectAllItem setEnabled:YES];
        
        [editMenu addItem:cutItem];
        [editMenu addItem:copyItem];
        [editMenu addItem:pasteItem];
        [editMenu addItem:[NSMenuItem separatorItem]];
        [editMenu addItem:selectAllItem];
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
