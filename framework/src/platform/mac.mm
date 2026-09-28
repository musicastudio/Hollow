// macOS: a flipped NSView that presents the GUI canvas through a CGImage and feeds it input.
// The view class is registered at runtime under a name unique to this binary, so two Hollow
// plug-ins loaded in one host never share (or fight over) an Objective-C class.
// Works with and without ARC.
#include "../core/core.h"
#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>
#include <cmath>
#include <cstdio>

namespace hollow {

struct PlatformWindow {
    void* view = nullptr;   // NSView*, retained
    void* timer = nullptr;  // NSTimer*, owned by the run loop until invalidated
    Gui* gui = nullptr;
    int pick = -1;          // menu choice, set by the menu item action
};

static Ivar windowIvar;

static PlatformWindow*& windowOf(id view) {
    return *(PlatformWindow**)((char*)(__bridge void*)view + ivar_getOffset(windowIvar));
}

static void mouse(id self, NSEvent* e, int kind) {   // 0 down, 1 drag/move, 2 up, 3 right down, 4 right up
    PlatformWindow* w = windowOf(self);
    if (!w) return;
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int s = w->gui->scale();
    int x = (int)std::floor(p.x / s), y = (int)std::floor(p.y / s);
    bool shift = ([e modifierFlags] & NSEventModifierFlagShift) != 0;
    if (kind == 0) w->gui->mouseDown(x, y, false, [e clickCount] == 2, shift);
    else if (kind == 1) w->gui->mouseMove(x, y, shift);
    else if (kind == 2) w->gui->mouseUp(x, y, shift);
    else if (kind == 3) w->gui->mouseDown(x, y, true, false, shift);
    else w->gui->rightUp(x, y, shift);
}

static BOOL yes(id, SEL) { return YES; }
static BOOL yesEvent(id, SEL, NSEvent*) { return YES; }
static void mouseDown(id self, SEL, NSEvent* e) { mouse(self, e, 0); }
static void mouseDragged(id self, SEL, NSEvent* e) { mouse(self, e, 1); }
static void mouseUp(id self, SEL, NSEvent* e) { mouse(self, e, 2); }
static void rightMouseDown(id self, SEL, NSEvent* e) { mouse(self, e, 3); }
static void rightMouseUp(id self, SEL, NSEvent* e) { mouse(self, e, 4); }

static void mouseExited(id self, SEL, NSEvent*) {
    if (PlatformWindow* w = windowOf(self)) w->gui->mouseLeave();
}

static void scrollWheel(id self, SEL, NSEvent* e) {
    PlatformWindow* w = windowOf(self);
    if (!w) return;
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int s = w->gui->scale();
    double notches = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) notches /= 10;   // Shortcut: 10 points of trackpad scroll = one notch
    w->gui->wheel((int)std::floor(p.x / s), (int)std::floor(p.y / s), notches, ([e modifierFlags] & NSEventModifierFlagShift) != 0);
}

static void pickItem(id self, SEL, id sender) {
    if (PlatformWindow* w = windowOf(self)) w->pick = (int)[sender tag];
}

// The keyboard, only while text entry or a skinned menu has it (platformFocus makes the view first
// responder then); otherwise the view refuses it and keys stay with the host.
static BOOL acceptsFirstResponder(id self, SEL) {
    PlatformWindow* w = windowOf(self);
    return w && w->gui->wantsKeys();
}

static BOOL resignFirstResponder(id self, SEL) {
    if (PlatformWindow* w = windowOf(self)) w->gui->focusLost();
    return YES;
}

// Shortcut: characters come from the key event itself, not the text input system, so there is no
// input method (dead keys still compose, since the event carries the composed characters).
static void keyDown(id self, SEL, NSEvent* e) {
    PlatformWindow* w = windowOf(self);
    if (!w || !w->gui->wantsKeys()) {
        [[self nextResponder] keyDown:e];
        return;
    }
    NSEventModifierFlags m = [e modifierFlags];
    bool shift = (m & NSEventModifierFlagShift) != 0;
    bool cmd = (m & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
    Key k = KeyNone;
    switch ([e keyCode]) {
    case 123: k = KeyLeft; break;
    case 124: k = KeyRight; break;
    case 126: k = KeyUp; break;
    case 125: k = KeyDown; break;
    case 115: k = KeyHome; break;
    case 119: k = KeyEnd; break;
    case 51: k = KeyBackspace; break;
    case 117: k = KeyDelete; break;
    case 36: case 76: k = KeyEnter; break;
    case 53: k = KeyEscape; break;
    case 48: k = KeyTab; break;
    }
    if (cmd && [[[e charactersIgnoringModifiers] lowercaseString] isEqualToString:@"a"]) k = KeySelectAll;
    if (k != KeyNone) {
        w->gui->keyDown(k, shift, cmd);
        return;
    }
    if (cmd) return;
    NSString* s = [e characters];
    for (NSUInteger i = 0; i < [s length]; ++i) {
        unichar c = [s characterAtIndex:i];
        if (c >= 0xf700 && c <= 0xf8ff) continue;   // function keys
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < [s length]) {
            unichar d = [s characterAtIndex:++i];
            w->gui->keyChar(0x10000 + ((c - 0xd800u) << 10) + (d - 0xdc00u));
        } else {
            w->gui->keyChar(c);
        }
    }
}

static void selectAll(id self, SEL, id) {   // Edit > Select All of the host's menu bar
    if (PlatformWindow* w = windowOf(self)) w->gui->keyDown(KeySelectAll, false, true);
}

static void drawRect(id self, SEL, NSRect) {
    PlatformWindow* w = windowOf(self);
    if (!w) return;
    Gui& g = *w->gui;
    const std::vector<uint32_t>& px = g.pixels();
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, px.data(), px.size() * 4, nullptr);
    CGImageRef img = CGImageCreate(g.width(), g.height(), 8, 32, g.width() * 4, cs,
                                   (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, dp, nullptr, false, kCGRenderingIntentDefault);
    int s = g.scale();
    CGContextSaveGState(ctx);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);   // pixel replication
    CGContextTranslateCTM(ctx, 0, g.height() * s);                  // the view is flipped, images are not
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, g.width() * s, g.height() * s), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

static Class viewClass() {
    static Class cls = nil;
    if (cls) return cls;
    char name[64];
    std::snprintf(name, sizeof name, "HollowView_%p", (void*)&viewClass);   // unique per loaded binary
    cls = objc_allocateClassPair([NSView class], name, 0);
    class_addIvar(cls, "hollowWindow", sizeof(void*), (uint8_t)std::log2(sizeof(void*)), "^v");
    auto types = [](SEL s) { return method_getTypeEncoding(class_getInstanceMethod([NSView class], s)); };
    class_addMethod(cls, @selector(isFlipped), (IMP)yes, types(@selector(isFlipped)));
    class_addMethod(cls, @selector(acceptsFirstMouse:), (IMP)yesEvent, types(@selector(acceptsFirstMouse:)));
    class_addMethod(cls, @selector(drawRect:), (IMP)drawRect, types(@selector(drawRect:)));
    class_addMethod(cls, @selector(mouseDown:), (IMP)mouseDown, types(@selector(mouseDown:)));
    class_addMethod(cls, @selector(mouseDragged:), (IMP)mouseDragged, types(@selector(mouseDragged:)));
    class_addMethod(cls, @selector(mouseMoved:), (IMP)mouseDragged, types(@selector(mouseMoved:)));
    class_addMethod(cls, @selector(mouseUp:), (IMP)mouseUp, types(@selector(mouseUp:)));
    class_addMethod(cls, @selector(rightMouseDown:), (IMP)rightMouseDown, types(@selector(rightMouseDown:)));
    class_addMethod(cls, @selector(rightMouseUp:), (IMP)rightMouseUp, types(@selector(rightMouseUp:)));
    class_addMethod(cls, @selector(mouseExited:), (IMP)mouseExited, types(@selector(mouseExited:)));
    class_addMethod(cls, @selector(scrollWheel:), (IMP)scrollWheel, types(@selector(scrollWheel:)));
    class_addMethod(cls, @selector(acceptsFirstResponder), (IMP)acceptsFirstResponder, types(@selector(acceptsFirstResponder)));
    class_addMethod(cls, @selector(resignFirstResponder), (IMP)resignFirstResponder, types(@selector(resignFirstResponder)));
    class_addMethod(cls, @selector(keyDown:), (IMP)keyDown, types(@selector(keyDown:)));
    class_addMethod(cls, @selector(selectAll:), (IMP)selectAll, "v@:@");
    class_addMethod(cls, NSSelectorFromString(@"hollowPick:"), (IMP)pickItem, "v@:@");
    objc_registerClassPair(cls);
    windowIvar = class_getInstanceVariable(cls, "hollowWindow");
    return cls;
}

PlatformWindow* platformOpen(void* parent, Gui* gui) {
    NSView* p = (__bridge NSView*)parent;
    if (!p) return nullptr;
    auto* w = new PlatformWindow;
    w->gui = gui;
    NSView* v = [[viewClass() alloc] initWithFrame:NSMakeRect(0, 0, gui->width() * gui->scale(), gui->height() * gui->scale())];
    windowOf(v) = w;
    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                          owner:v
                                                       userInfo:nil];
    [v addTrackingArea:area];
    [p addSubview:v];
    w->view = (void*)CFBridgingRetain(v);
#if !__has_feature(objc_arc)
    [area release];
    [v release];
#endif
    NSTimer* t = [NSTimer timerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer* timer) { (void)timer; gui->tick(); }];
    [[NSRunLoop currentRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
    w->timer = (__bridge void*)t;
    return w;
}

void platformClose(PlatformWindow* w) {
    [(__bridge NSTimer*)w->timer invalidate];
    NSView* v = (__bridge NSView*)w->view;
    windowOf(v) = nullptr;   // late events find no window
    [v removeFromSuperview];
    (void)CFBridgingRelease(w->view);
    delete w;
}

const bool kNativeMenus = true;
void platformHold(PlatformWindow*, bool) {}   // the GUI runs on the host's thread

void platformInvalidate(PlatformWindow* w, Rect r) {
    int s = w->gui->scale();
    [(__bridge NSView*)w->view setNeedsDisplayInRect:NSMakeRect(r.x * s, r.y * s, r.w * s, r.h * s)];
}

void platformSize(PlatformWindow* w, int width, int height) {
    NSView* v = (__bridge NSView*)w->view;
    [v setFrameSize:NSMakeSize(width, height)];
    [v setNeedsDisplay:YES];
}

// Entries report their id through the item tag; submenus nest. Limit: NSMenu has no columns, so
// column breaks are ignored.
static NSMenu* buildMenu(NSView* v, const std::vector<MenuEntry>& items) {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    [menu setAutoenablesItems:NO];
    for (auto& e : items) {
        if (e.separator) { [menu addItem:[NSMenuItem separatorItem]]; continue; }
        NSString* title = [NSString stringWithUTF8String:e.label.c_str()];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:(title ? title : @"?") action:NSSelectorFromString(@"hollowPick:") keyEquivalent:@""];
        [item setTarget:v];
        [item setTag:(NSInteger)e.id];
        [item setEnabled:YES];
        [item setState:e.checked ? NSControlStateValueOn : NSControlStateValueOff];
        if (!e.items.empty()) {
            NSMenu* sub = buildMenu(v, e.items);
            [item setSubmenu:sub];
#if !__has_feature(objc_arc)
            [sub release];
#endif
        }
        [menu addItem:item];
#if !__has_feature(objc_arc)
        [item release];
#endif
    }
    return menu;
}

int platformMenu(PlatformWindow* w, const std::vector<MenuEntry>& items, int x, int y) {
    NSView* v = (__bridge NSView*)w->view;
    NSMenu* menu = buildMenu(v, items);
    w->pick = -1;
    int s = w->gui->scale();
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(x * s, y * s) inView:v];   // returns after the choice
#if !__has_feature(objc_arc)
    [menu release];
#endif
    return w->pick;
}

void platformTip(PlatformWindow* w, const std::string& text) {
    NSString* s = text.empty() ? nil : [NSString stringWithUTF8String:text.c_str()];
    [(__bridge NSView*)w->view setToolTip:s];
}

// Limit: on macOS the host sizes the view's superviews itself; nothing is walked here.
void platformResizeParents(PlatformWindow*, int, int) {}

void platformFocus(PlatformWindow* w, bool on) {
    NSView* v = (__bridge NSView*)w->view;
    NSWindow* win = [v window];
    if (on) [win makeFirstResponder:v];
    else if ([win firstResponder] == v) [win makeFirstResponder:nil];
}

void platformOpenUrl(const std::string& url) {
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return;
    NSString* s = [NSString stringWithUTF8String:url.c_str()];
    NSURL* u = s ? [NSURL URLWithString:s] : nil;
    if (u) [[NSWorkspace sharedWorkspace] openURL:u];
}

// clap-wrapper's standalone app delegate answers its Audio Settings menu item with this action.
const bool kAudioSettings = true;
void platformAudioSettings(PlatformWindow*) {
    [NSApp sendAction:NSSelectorFromString(@"openAudioSettingsWindow:") to:nil from:nil];
}

// NSOpenPanel / NSSavePanel, run modal; the types' extensions become the allowed ones.
bool platformFileDialog(PlatformWindow*, bool save, const std::string& title, const Vars& types, const std::string& name, std::string& path) {
    NSMutableArray* exts = [NSMutableArray array];
    for (auto& t : types) {
        size_t a = 0;
        while (a <= t.second.size()) {
            size_t b = t.second.find(';', a);
            std::string e = t.second.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!e.empty()) [exts addObject:[NSString stringWithUTF8String:e.c_str()]];
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    NSSavePanel* panel = save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
    if (!title.empty()) [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if ([exts count]) [panel setAllowedFileTypes:exts];
#pragma clang diagnostic pop
    if (save && !name.empty()) [panel setNameFieldStringValue:[NSString stringWithUTF8String:name.c_str()]];
    if ([panel runModal] != NSModalResponseOK) return false;
    NSURL* u = [panel URL];
    const char* p = u ? [[u path] UTF8String] : nullptr;
    if (!p) return false;
    path = p;
    return true;
}

} // namespace hollow
