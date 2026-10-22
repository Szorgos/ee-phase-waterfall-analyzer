/**
 * @file main.mm
 * @brief Aqulify macOS-re: menüsor-ikon, saját ablak (WKWebView) és a motor egy alkalmazásban.
 *
 * A motor (Core Audio, AetherChain) az alkalmazás indulásától a kilépésig fut; az ablak bezárásakor csak a
 * felület áll le, a hang feldolgozása folytatódik. A felület ugyanaz a gui_integrated_system.html, mint
 * Linuxon és Windowson, és ugyanazon a WebSocket hídon (127.0.0.1:47810–47814, végső tartalék: 8080) csatlakozik.
 * A beállítások helye: ~/Library/Application Support/Aqulify, a napló: ~/Library/Logs/Aqulify/aqulify.log.
 */
#import <Cocoa/Cocoa.h>
#import <ServiceManagement/ServiceManagement.h>
#import <WebKit/WebKit.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "aq_i18n.hpp"
#include "linux_store.hpp"
#include "mac_engine.hpp"
#include "ws_bridge.hpp"

using namespace ee_analyzer;
using aq_i18n::Msg;

namespace {

// A GUI (gui_integrated_system.html: ENGINE_PORTS) ugyanezt a listát próbálja végig
const int kEnginePorts[] = { 47810, 47811, 47812, 47813, 47814, 8080 };

NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
NSString* T(Msg m) { return NS(aq_i18n::tr(m)); }

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out;
}

/** Csak ezek nyílnak meg a rendszer böngészőjében (a felület támogatás és projekt linkjei). */
bool allowedExternalUrl(NSString* url) {
    for (NSString* ok in @[ @"https://revolut.me/", @"https://github.com/Szorgos/" ])
        if ([url hasPrefix:ok]) return true;
    return false;
}

/** A felület (JavaScript) hibái a naplóba; csak az első 20, hogy egy képkockánként ismétlődő hiba ne töltse tele. */
void logGuiError(id body) {
    static int count = 0;
    if (++count > 20) {
        if (count == 21) std::cerr << "[Felület] A további hibák nem kerülnek a naplóba.\n";
        return;
    }
    NSString* s = [body isKindOfClass:[NSString class]] ? (NSString*)body : [body description];
    if (s.length > 400) s = [s substringToIndex:400];
    std::cerr << "[Felület] Hiba: " << s.UTF8String << "\n";
}

std::string logPath() {
    const char* h = std::getenv("HOME");
    return std::string(h && *h ? h : ".") + "/Library/Logs/Aqulify/aqulify.log";
}

/** Finderből indítva a szabványos kimenet sehova sem megy: a napló fájlba kerül (2 MB fölött új fájl). */
void redirectLog() {
    if (isatty(STDOUT_FILENO)) return;
    const std::string path = logPath();
    const std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && st.st_size > 2 * 1024 * 1024) std::rename(path.c_str(), (path + ".1").c_str());
    if (std::freopen(path.c_str(), "a", stdout)) setvbuf(stdout, nullptr, _IOLBF, 0);
    if (std::freopen(path.c_str(), "a", stderr)) setvbuf(stderr, nullptr, _IOLBF, 0);
}

/** A motor és a híd állapota (a híd szálából és a fő szálból is elérhető). */
struct Core {
    mac::Engine engine;

    std::mutex paramMutex;
    ParamData current = transparentParams();
    bool dirty = false;
    std::chrono::steady_clock::time_point lastChange = std::chrono::steady_clock::now();

    std::mutex presetMutex;
    std::string presetDoc;

    std::mutex outputMutex;
    std::string selectedOutput;   // UID, vagy üres: a rendszer alapértelmezettje
    std::string sinksJson = "[]", currentSink = "default";
    std::chrono::steady_clock::time_point lastSinkQuery{};

    uint64_t readPos = 0;
    std::unique_ptr<WebSocketBridge> bridge;

    /** A felület kimenetválasztója: első a rendszer alapértelmezettje, utána az eszközök. */
    void refreshSinks() {
        const auto devices = engine.outputDevices();
        std::string selected;
        {
            std::lock_guard<std::mutex> lk(outputMutex);
            selected = selectedOutput;
        }
        bool selectedPresent = false;
        std::string defaultName;
        for (const auto& d : devices) {
            if (d.uid == selected) selectedPresent = true;
            if (d.isDefault) defaultName = d.name;
        }
        // A választott eszköz eltűnt (pl. kihúzták): a motor az alapértelmezetten szól
        const bool useDefault = selected.empty() || !selectedPresent;
        // Alapértelmezett módban az az eszköz, amin a motor szól (BlackHole módban a rendszer kimenete maga a BlackHole)
        if (const std::string active = engine.activeOutputName(); useDefault && !active.empty()) defaultName = active;
        std::string json = "[{\"id\":\"default\",\"name\":\"default\",\"label\":\"" +
                           jsonEscape(aq_i18n::fmt(Msg::mac_defaultOutput, { { "name", defaultName.empty() ? "—" : defaultName } })) +
                           "\",\"active\":" + (useDefault ? "true" : "false") + "}";
        for (const auto& d : devices) {
            json += ",{\"id\":\"" + jsonEscape(d.uid) + "\",\"name\":\"" + jsonEscape(d.uid) + "\",\"label\":\"" + jsonEscape(d.name) +
                    "\",\"active\":" + (!useDefault && d.uid == selected ? "true" : "false") + "}";
        }
        json += "]";
        std::lock_guard<std::mutex> lk(outputMutex);
        sinksJson = json;
        currentSink = useDefault ? "default" : selected;
        lastSinkQuery = std::chrono::steady_clock::now();
    }

    std::string statusFields() {
        bool stale = false;
        {
            std::lock_guard<std::mutex> lk(outputMutex);
            stale = std::chrono::steady_clock::now() - lastSinkQuery > std::chrono::seconds(1);
        }
        if (stale) refreshSinks();
        char buf[160];
        std::snprintf(buf, sizeof(buf), "\"platform\":\"macos\",\"dspActive\":%s,\"sr\":%u,\"gr\":%.2f",
                      engine.processing() ? "true" : "false", engine.sampleRate(), engine.chain.gainReductionDb());
        std::string s = buf;
        const std::string src = engine.sourceName();
        s += ",\"source\":\"" + jsonEscape(src.empty() ? "Core Audio" : src) + "\"";
        switch (engine.problem()) {
        case mac::Problem::Permission: s += ",\"problem\":\"permission\""; break;
        case mac::Problem::NoSource: s += ",\"problem\":\"nosource\""; break;
        case mac::Problem::Device: s += ",\"problem\":\"device\""; break;
        case mac::Problem::None: break;
        }
        std::lock_guard<std::mutex> lk(outputMutex);
        s += ",\"currentSink\":\"" + jsonEscape(currentSink) + "\",\"outputSinks\":" + sinksJson;
        return s;
    }

    void setOutput(const std::string& target) {
        const std::string uid = target == "default" ? std::string() : target;
        if (!uid.empty()) {
            bool known = false;
            for (const auto& d : engine.outputDevices()) known = known || d.uid == uid;
            if (!known) {
                std::cerr << "[Core Audio] A kért kimenet nem található.\n";
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(outputMutex);
            selectedOutput = uid;
            lastSinkQuery = {};
        }
        engine.setOutput(uid);
        if (!linux_store::saveOutputSink(uid)) std::cerr << "[Beállítások] Figyelem: az output_sink.txt nem írható.\n";
    }

    void saveTick() {
        ParamData snapshot;
        {
            std::lock_guard<std::mutex> lk(paramMutex);
            if (!dirty || std::chrono::steady_clock::now() - lastChange < std::chrono::milliseconds(600)) return;
            dirty = false;
            snapshot = current;
        }
        if (!linux_store::saveParams(snapshot))
            std::cerr << "[Beállítások] Figyelem: a " << linux_store::configDir(false) << "/params.bin nem írható.\n";
    }
};

} // namespace

// ======================================================================== alkalmazás

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate, NSWindowDelegate, WKUIDelegate,
                                   WKNavigationDelegate, WKScriptMessageHandler>
@end

@implementation AppDelegate {
    std::unique_ptr<Core> core_;
    NSStatusItem* statusItem_;
    NSWindow* window_;
    WKWebView* web_;
    NSTimer* saveTimer_;
    dispatch_source_t termSource_;
    dispatch_source_t intSource_;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    std::cout << "========================================================\n";
    std::cout << " Aqulify • Fázis & Akusztikai Vízesés – macOS (" AQULIFY_VERSION ")\n";
    std::cout << "========================================================\n";
    aq_i18n::init();
    core_ = std::make_unique<Core>();
    Core* c = core_.get();

    // Utolsó beállítások; első indításkor átlátszó (minden modul kikapcsolva), a felület adja át a sajátját
    if (linux_store::loadParams(c->current))
        std::cout << "[Beállítások] Utolsó DSP lánc betöltve (" << c->current.numBands << " EQ sáv).\n";
    c->engine.params.write(c->current);
    {
        IrData ir;
        if (linux_store::loadIr(ir)) {
            std::cout << "[Konvolver] Mentett impulzusválasz betöltve: " << ir.name << "\n";
            c->engine.chain.submitIr(std::move(ir));
        }
    }
    c->presetDoc = linux_store::loadPresets();
    c->selectedOutput = linux_store::loadOutputSink();
    if (!c->selectedOutput.empty()) c->engine.setOutput(c->selectedOutput);
    c->engine.start();

    if (![self startBridge]) {
        NSAlert* a = [NSAlert new];
        a.alertStyle = NSAlertStyleCritical;
        a.messageText = NS(aq_i18n::fmt(Msg::engine_portsBusy, { { "ports", "47810–47814, 8080" } }));
        a.informativeText = T(Msg::engine_portsHint);
        [a runModal];
        [NSApp terminate:nil];
        return;
    }

    [self buildMainMenu];
    statusItem_ = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    NSImage* img = nil;
    if (@available(macOS 11.0, *)) img = [NSImage imageWithSystemSymbolName:@"waveform" accessibilityDescription:@"Aqulify"];
    if (img) {
        [img setTemplate:YES];   // (a "template" C++ kulcsszó: pont-szintaxissal nem írható)
        statusItem_.button.image = img;
    } else {
        statusItem_.button.title = @"Aqulify";
    }
    statusItem_.button.toolTip = @"Aqulify";
    NSMenu* menu = [NSMenu new];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    statusItem_.menu = menu;

    saveTimer_ = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer*) { c->saveTick(); }];

    // killall / kijelentkezés: rendes leállítás (BlackHole módban a rendszer kimenete visszaáll)
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    termSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    intSource_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(termSource_, ^{ [NSApp terminate:nil]; });
    dispatch_source_set_event_handler(intSource_, ^{ [NSApp terminate:nil]; });
    dispatch_resume(termSource_);
    dispatch_resume(intSource_);

    // Bejelentkezéskor indítva csak a menüsorban fut; egyébként a felület is megnyílik
    if (![self launchedAsLoginItem]) [self showWindow:nil];
}

- (BOOL)launchedAsLoginItem {
    NSAppleEventDescriptor* ev = NSAppleEventManager.sharedAppleEventManager.currentAppleEvent;
    return ev.eventID == kAEOpenApplication && [ev paramDescriptorForKeyword:keyAEPropData].enumCodeValue == keyAELaunchedAsLogInItem;
}

- (BOOL)startBridge {
    Core* c = core_.get();
    BridgeCallbacks cb;
    cb.pullAudio = [c](float* lr, size_t maxFrames) { return readTapRing(c->engine.visualizerTap, c->readPos, lr, maxFrames); };
    cb.sampleRate = [c]() { return static_cast<double>(c->engine.sampleRate()); };
    cb.getParams = [c]() {
        std::lock_guard<std::mutex> lk(c->paramMutex);
        return c->current;
    };
    cb.setParams = [c](const ParamData& p) {
        {
            std::lock_guard<std::mutex> lk(c->paramMutex);
            c->current = p;
            c->dirty = true;
            c->lastChange = std::chrono::steady_clock::now();
        }
        c->engine.params.write(p);
    };
    cb.gainReduction = [c]() { return c->engine.chain.gainReductionDb(); };
    cb.bassHarmonics = [c]() { return c->engine.chain.bassHarmonicsDb(); };
    cb.setOutputSink = [c](const std::string& target) { c->setOutput(target); };
    cb.statusFields = [c]() { return c->statusFields(); };
    cb.setIr = [c](IrData&& ir) {
        if (!linux_store::saveIr(ir)) std::cerr << "[Konvolver] Figyelem: az IR nem menthető.\n";
        c->engine.chain.submitIr(std::move(ir));
    };
    cb.getIr = [c]() { return c->engine.chain.irCopy(); };
    cb.getPresets = [c]() {
        std::lock_guard<std::mutex> lk(c->presetMutex);
        return c->presetDoc;
    };
    cb.setPresets = [c](const std::string& doc) {
        std::lock_guard<std::mutex> lk(c->presetMutex);
        c->presetDoc = doc;
        if (!linux_store::savePresets(doc)) std::cerr << "[Profilok] Figyelem: a presets.json nem írható.\n";
    };
    cb.magicDiagJson = [c]() {
        {
            std::lock_guard<std::mutex> lk(c->paramMutex);
            if (!(c->current.has(kFlagMagic) && c->current.magicAmount > 0.0f)) return std::string();
        }
        magic::MagicDiag d;
        return c->engine.chain.magicDiag(d) ? magic::diagToJson(d) : std::string();
    };
    cb.getLanguage = [] { return aq_i18n::savedLanguage(); };
    __weak AppDelegate* weakSelf = self;
    cb.setLanguage = [weakSelf](const std::string& l) {
        if (aq_i18n::set(l) && !aq_i18n::saveLanguage(l)) std::cerr << "[Nyelv] Figyelem: a language.txt nem írható.\n";
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf languageChanged]; });
    };

    for (int p : kEnginePorts) {
        auto b = std::make_unique<WebSocketBridge>(p);
        if (b->start(cb)) {
            c->bridge = std::move(b);
            return YES;
        }
        std::cout << "[Info] A " << p << " port foglalt, a következőt próbálom.\n";
    }
    std::cerr << "[Hiba] Minden kapcsolódási port foglalt; a felület nem tud csatlakozni.\n";
    return NO;
}

/** A felületen választott nyelv: a menüsor menüi és az alapértelmezett kimenet felirata is ezen a nyelven. */
- (void)languageChanged {
    [self buildMainMenu];
    if (!core_) return;
    std::lock_guard<std::mutex> lk(core_->outputMutex);
    core_->lastSinkQuery = {};
}

- (void)applicationWillTerminate:(NSNotification*)note {
    if (!core_) return;
    [saveTimer_ invalidate];
    if (core_->bridge) core_->bridge->stop();
    core_->engine.stop();
    {
        std::lock_guard<std::mutex> lk(core_->paramMutex);
        if (core_->dirty) linux_store::saveParams(core_->current);
    }
    std::cout << "[Info] Leállítás.\n";
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    [self showWindow:nil];
    return NO;
}

- (void)applicationDidBecomeActive:(NSNotification*)note {
    // Visszatérés a Rendszerbeállításokból: ha közben engedélyt adott, vagy telepítette a BlackHole-t
    if (core_ && core_->engine.problem() != mac::Problem::None) core_->engine.retry();
}

// ------------------------------------------------------------------ menük

- (void)buildMainMenu {
    NSMenu* bar = [NSMenu new];

    NSMenu* app = [NSMenu new];
    [app addItemWithTitle:T(Msg::mac_about) action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:T(Msg::mac_hide) action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:T(Msg::mac_quit) action:@selector(terminate:) keyEquivalent:@"q"];
    [bar addItemWithTitle:@"Aqulify" action:nil keyEquivalent:@""].submenu = app;

    NSMenu* edit = [[NSMenu alloc] initWithTitle:T(Msg::mac_edit)];
    [edit addItemWithTitle:T(Msg::mac_undo) action:@selector(undo:) keyEquivalent:@"z"];
    [edit addItemWithTitle:T(Msg::mac_redo) action:@selector(redo:) keyEquivalent:@"Z"];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItemWithTitle:T(Msg::mac_cut) action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:T(Msg::mac_copy) action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:T(Msg::mac_paste) action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:T(Msg::mac_selectAll) action:@selector(selectAll:) keyEquivalent:@"a"];
    [bar addItemWithTitle:T(Msg::mac_edit) action:nil keyEquivalent:@""].submenu = edit;

    NSMenu* win = [[NSMenu alloc] initWithTitle:T(Msg::mac_window)];
    [win addItemWithTitle:T(Msg::mac_minimize) action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [win addItemWithTitle:T(Msg::mac_closeWindow) action:@selector(performClose:) keyEquivalent:@"w"];
    [bar addItemWithTitle:T(Msg::mac_window) action:nil keyEquivalent:@""].submenu = win;
    NSApp.windowsMenu = win;

    NSApp.mainMenu = bar;
}

- (NSMenuItem*)addItem:(NSMenu*)menu title:(NSString*)title action:(SEL)action {
    NSMenuItem* it = [menu addItemWithTitle:title action:action keyEquivalent:@""];
    it.target = self;
    return it;
}

/** A menüsor-ikon menüje minden megnyitáskor friss állapottal (és a felületen választott nyelven) épül. */
- (void)menuNeedsUpdate:(NSMenu*)menu {
    [menu removeAllItems];
    Core* c = core_.get();
    if (!c) return;
    const mac::Problem problem = c->engine.problem();
    std::vector<std::string> lines;
    if (c->engine.processing()) {
        lines.push_back(aq_i18n::fmt(Msg::tray_source, { { "src", c->engine.sourceName() } }));
        lines.push_back(aq_i18n::fmt(Msg::mac_output, { { "name", c->engine.activeOutputName() } }));
    } else if (problem == mac::Problem::Permission) {
        lines.push_back(aq_i18n::tr(Msg::mac_statusPermission));
    } else if (problem == mac::Problem::NoSource) {
        lines.push_back(aq_i18n::tr(Msg::mac_statusNoSource));
    } else if (problem == mac::Problem::Device) {
        lines.push_back(aq_i18n::tr(Msg::mac_statusDevice));
    } else {
        lines.push_back(aq_i18n::tr(Msg::mac_statusStarting));
    }
    for (const auto& l : lines) [menu addItemWithTitle:NS(l) action:nil keyEquivalent:@""].enabled = NO;   // állapotsor
    if (problem == mac::Problem::Permission) [self addItem:menu title:T(Msg::mac_openPrivacy) action:@selector(openPrivacy:)];
    if (problem == mac::Problem::NoSource || problem == mac::Problem::Permission)
        [self addItem:menu title:T(Msg::mac_getBlackHole) action:@selector(openBlackHole:)];
    [menu addItem:NSMenuItem.separatorItem];

    [self addItem:menu title:T(Msg::tray_open) action:@selector(showWindow:)];
    if (@available(macOS 13.0, *)) {
        NSMenuItem* login = [self addItem:menu title:T(Msg::mac_loginItem) action:@selector(toggleLoginItem:)];
        login.state = SMAppService.mainAppService.status == SMAppServiceStatusEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [self addItem:menu title:T(Msg::tray_log) action:@selector(openLog:)];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:T(Msg::mac_quit) action:@selector(terminate:) keyEquivalent:@"q"].target = NSApp;
}

- (void)openPrivacy:(id)sender {
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture"]];
}

- (void)openBlackHole:(id)sender {
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"https://existential.audio/blackhole/"]];
}

- (void)openLog:(id)sender {
    NSURL* url = [NSURL fileURLWithPath:NS(logPath())];
    if ([NSFileManager.defaultManager fileExistsAtPath:url.path]) [NSWorkspace.sharedWorkspace openURL:url];
}

- (void)toggleLoginItem:(id)sender {
    if (@available(macOS 13.0, *)) {
        SMAppService* s = SMAppService.mainAppService;
        NSError* err = nil;
        if (s.status == SMAppServiceStatusEnabled) {
            [s unregisterAndReturnError:&err];
        } else {
            [s registerAndReturnError:&err];
            if (s.status == SMAppServiceStatusRequiresApproval) [SMAppService openSystemSettingsLoginItems];
        }
        if (err) std::cerr << "[Indítás] A bejelentkezési elem nem állítható: " << err.localizedDescription.UTF8String << "\n";
    }
}

// ------------------------------------------------------------------ ablak

- (NSURL*)guiDirectory {
    NSURL* dir = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"gui" isDirectory:YES];
    if ([NSFileManager.defaultManager fileExistsAtPath:[dir URLByAppendingPathComponent:@"gui_integrated_system.html"].path]) return dir;
    return nil;
}

- (void)showWindow:(id)sender {
    if (!window_ && ![self createWindow]) return;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];   // ablak nyitva: a Dockban is látszik
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)createWindow {
    NSURL* dir = [self guiDirectory];
    if (!dir) {
        NSAlert* a = [NSAlert new];
        a.messageText = T(Msg::mac_guiMissing);
        [a runModal];
        return NO;
    }
    const NSRect screen = NSScreen.mainScreen ? NSScreen.mainScreen.visibleFrame : NSMakeRect(0, 0, 1480, 940);
    const CGFloat w = MIN(1480, screen.size.width - 40), h = MIN(940, screen.size.height - 40);
    const NSRect frame = NSMakeRect(NSMidX(screen) - w / 2, NSMidY(screen) - h / 2, w, h);
    window_ = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    window_.title = @"Aqulify";
    window_.releasedWhenClosed = NO;
    window_.delegate = self;
    window_.minSize = NSMakeSize(1000, 640);
    // A felület sötét: a címsor is (a rendszertémától függetlenül), betöltés előtt sincs fehér villanás
    window_.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    NSColor* bg = [NSColor colorWithSRGBRed:10 / 255.0 green:15 / 255.0 blue:21 / 255.0 alpha:1.0];
    window_.backgroundColor = bg;
    window_.titlebarAppearsTransparent = YES;
    [window_ setFrameAutosaveName:@"AqulifyMain"];

    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    WKUserContentController* ucc = [WKUserContentController new];
    [ucc addScriptMessageHandler:self name:@"aetherOpenUrl"];
    [ucc addUserScript:[[WKUserScript alloc]
                           initWithSource:@"window.aetherOpenUrl = function (u) { window.webkit.messageHandlers.aetherOpenUrl.postMessage(String(u)); };"
                            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                         forMainFrameOnly:YES]];
    // A felület hibái a naplóba (az ablakban nincs fejlesztői konzol)
    [ucc addScriptMessageHandler:self name:@"aetherLog"];
    [ucc addUserScript:[[WKUserScript alloc]
                           initWithSource:@"(function () {"
                                           "  var log = function (m) { try { window.webkit.messageHandlers.aetherLog.postMessage(String(m)); } catch (e) {} };"
                                           "  window.addEventListener('error', function (e) {"
                                           "    log(e.message + ' (' + String(e.filename || '').split('/').pop() + ':' + e.lineno + ')'); });"
                                           "  window.addEventListener('unhandledrejection', function (e) {"
                                           "    log('Promise: ' + (e.reason && e.reason.message ? e.reason.message : e.reason)); });"
                                           "})();"
                            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                         forMainFrameOnly:YES]];
    cfg.userContentController = ucc;
    web_ = [[WKWebView alloc] initWithFrame:window_.contentView.bounds configuration:cfg];
    web_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    web_.UIDelegate = self;
    web_.navigationDelegate = self;
    @try {
        [web_ setValue:@NO forKey:@"drawsBackground"];
    } @catch (NSException*) {
    }
    if (@available(macOS 12.0, *)) web_.underPageBackgroundColor = bg;
    window_.contentView = web_;
    [web_ loadFileURL:[dir URLByAppendingPathComponent:@"gui_integrated_system.html"] allowingReadAccessToURL:dir];
    return YES;
}

/** Bezáráskor a felület leáll (a motor fut tovább a menüsorban), és az alkalmazás eltűnik a Dockból. */
- (void)windowWillClose:(NSNotification*)note {
    if (note.object != window_) return;
    WKWebView* web = web_;
    NSWindow* win = window_;
    web_ = nil;
    window_ = nil;
    [web.configuration.userContentController removeScriptMessageHandlerForName:@"aetherOpenUrl"];
    [web.configuration.userContentController removeScriptMessageHandlerForName:@"aetherLog"];
    web.UIDelegate = nil;
    web.navigationDelegate = nil;
    win.delegate = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        [web removeFromSuperview];
        (void)win;   // az ablak csak a bezárás befejezése után szabadul fel
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    });
}

// ------------------------------------------------------------------ WKWebView

- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)message {
    if ([message.name isEqualToString:@"aetherLog"]) {
        logGuiError(message.body);
        return;
    }
    if (![message.name isEqualToString:@"aetherOpenUrl"] || ![message.body isKindOfClass:[NSString class]]) return;
    NSString* url = message.body;
    if (allowedExternalUrl(url)) [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:url]];
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    NSURL* url = action.request.URL;
    NSString* scheme = url.scheme.lowercaseString;
    if ([scheme isEqualToString:@"file"] || [scheme isEqualToString:@"about"] || [scheme isEqualToString:@"data"] ||
        [scheme isEqualToString:@"blob"]) {
        decisionHandler(WKNavigationActionPolicyAllow);
        return;
    }
    if (allowedExternalUrl(url.absoluteString)) [NSWorkspace.sharedWorkspace openURL:url];
    decisionHandler(WKNavigationActionPolicyCancel);
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
    NSURL* url = action.request.URL;
    if (url && allowedExternalUrl(url.absoluteString)) [NSWorkspace.sharedWorkspace openURL:url];
    return nil;
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
    std::cerr << "[Felület] A WebKit tartalomfolyamata leállt; újratöltés.\n";
    [webView reload];
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    std::cerr << "[Felület] Nem tölthető be: " << error.localizedDescription.UTF8String << "\n";
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
    std::cerr << "[Felület] Betöltési hiba: " << error.localizedDescription.UTF8String << "\n";
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler {
    NSAlert* a = [NSAlert new];
    a.messageText = @"Aqulify";
    a.informativeText = message;
    [a addButtonWithTitle:@"OK"];
    [a beginSheetModalForWindow:window_ completionHandler:^(NSModalResponse) { completionHandler(); }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))completionHandler {
    NSAlert* a = [NSAlert new];
    a.messageText = @"Aqulify";
    a.informativeText = message;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:T(Msg::btn_cancel)];
    [a beginSheetModalForWindow:window_
              completionHandler:^(NSModalResponse r) { completionHandler(r == NSAlertFirstButtonReturn); }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:(void (^)(NSString*))completionHandler {
    NSAlert* a = [NSAlert new];
    a.messageText = @"Aqulify";
    a.informativeText = prompt;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:T(Msg::btn_cancel)];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    field.stringValue = defaultText ?: @"";
    a.accessoryView = field;
    a.window.initialFirstResponder = field;
    [a beginSheetModalForWindow:window_
              completionHandler:^(NSModalResponse r) { completionHandler(r == NSAlertFirstButtonReturn ? field.stringValue : nil); }];
}

- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>*))completionHandler {
    NSOpenPanel* p = [NSOpenPanel openPanel];
    p.allowsMultipleSelection = parameters.allowsMultipleSelection;
    p.canChooseDirectories = NO;
    p.canChooseFiles = YES;
    [p beginSheetModalForWindow:window_
              completionHandler:^(NSModalResponse r) { completionHandler(r == NSModalResponseOK ? p.URLs : nil); }];
}

@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        redirectLog();
        // Egy példány: a második indítás a már futó ablakát hozza elő
        NSString* bid = NSBundle.mainBundle.bundleIdentifier;
        if (bid) {
            for (NSRunningApplication* other in [NSRunningApplication runningApplicationsWithBundleIdentifier:bid]) {
                if (other.processIdentifier == getpid()) continue;
                [other activateWithOptions:NSApplicationActivateAllWindows];
                return 0;
            }
        }
        NSApplication* app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
        (void)argc;
        (void)argv;
    }
    return 0;
}
