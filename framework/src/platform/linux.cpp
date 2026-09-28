// Linux (X11): a child of the host's window, run by a thread of its own on a Display connection of its
// own, presenting the GUI canvas with XPutImage and feeding it input. Only that thread touches the
// Display after platformOpen; the host's thread hands it sizes and repaints and wakes it through a pipe.
// The Gui itself runs on that thread under PlatformWindow::lock, which Editor takes (platformHold) for
// the few calls it makes from the host's thread.
#include "../core/core.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace hollow {

struct PlatformWindow {
    Gui* gui = nullptr;
    Display* dpy = nullptr;
    Window win = 0;
    GC gc = nullptr;
    XImage* img = nullptr;
    std::vector<uint32_t> frame;       // the canvas at the window's scale, what img points at
    std::thread thread;
    std::recursive_mutex lock;         // the Gui: this thread while it works, Editor's calls from the host
    std::mutex pending;                // what the host's thread asks for
    Rect dirty;
    int wantW = 0, wantH = 0;          // a size to apply, 0 = none
    std::atomic<bool> quit{false};
    int wake[2] = {-1, -1};
    Time lastClick = 0;
    int clickX = 0, clickY = 0;
};

static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static void poke(PlatformWindow* w) {
    char c = 1;
    if (write(w->wake[1], &c, 1) < 0) {}   // a full pipe is already awake
}

// The canvas scaled by pixel replication into frame, then the rect r (canvas pixels) to the window.
static void present(PlatformWindow* w, Rect r) {
    Gui& g = *w->gui;
    const std::vector<uint32_t>& px = g.pixels();
    const int s = g.scale(), W = g.width() * s, H = g.height() * s;
    if (!w->img || w->img->width != W || w->img->height != H) {
        if (w->img) {
            w->img->data = nullptr;   // frame owns the pixels
            XDestroyImage(w->img);
        }
        w->frame.assign((size_t)W * H, 0);
        Visual* vis = DefaultVisual(w->dpy, DefaultScreen(w->dpy));
        w->img = XCreateImage(w->dpy, vis, 24, ZPixmap, 0, (char*)w->frame.data(), (unsigned)W, (unsigned)H, 32, W * 4);
        r = {0, 0, g.width(), g.height()};
    }
    if (!w->img) return;
    r = r & Rect{0, 0, g.width(), g.height()};
    if (r.empty()) return;
    for (int y = r.y; y < r.y + r.h; ++y) {
        const uint32_t* src = px.data() + (size_t)y * g.width();
        for (int k = 0; k < s; ++k) {
            uint32_t* dst = w->frame.data() + (size_t)(y * s + k) * W;
            for (int x = r.x; x < r.x + r.w; ++x)
                for (int j = 0; j < s; ++j) dst[x * s + j] = src[x] & 0xffffff;
        }
    }
    XPutImage(w->dpy, w->win, w->gc, w->img, r.x * s, r.y * s, r.x * s, r.y * s, (unsigned)(r.w * s), (unsigned)(r.h * s));
}

static Key keyOf(KeySym k, bool ctrl) {
    switch (k) {
    case XK_Left: case XK_KP_Left: return KeyLeft;
    case XK_Right: case XK_KP_Right: return KeyRight;
    case XK_Up: case XK_KP_Up: return KeyUp;
    case XK_Down: case XK_KP_Down: return KeyDown;
    case XK_Home: case XK_KP_Home: return KeyHome;
    case XK_End: case XK_KP_End: return KeyEnd;
    case XK_BackSpace: return KeyBackspace;
    case XK_Delete: case XK_KP_Delete: return KeyDelete;
    case XK_Return: case XK_KP_Enter: return KeyEnter;
    case XK_Escape: return KeyEscape;
    case XK_Tab: return KeyTab;
    case XK_a: case XK_A: return ctrl ? KeySelectAll : KeyNone;
    default: return KeyNone;
    }
}

static void event(PlatformWindow* w, XEvent& e) {
    Gui& g = *w->gui;
    const int s = g.scale();
    switch (e.type) {
    case Expose:
        present(w, {floorDiv(e.xexpose.x, s), floorDiv(e.xexpose.y, s), e.xexpose.width / s + 2, e.xexpose.height / s + 2});
        break;
    case ButtonPress: {
        const int x = floorDiv(e.xbutton.x, s), y = floorDiv(e.xbutton.y, s);
        const bool shift = (e.xbutton.state & ShiftMask) != 0;
        if (e.xbutton.button == 4 || e.xbutton.button == 5) {   // the wheel arrives as buttons
            g.wheel(x, y, e.xbutton.button == 4 ? 1.0 : -1.0, shift);
        } else if (e.xbutton.button == 1) {
            const bool dbl = e.xbutton.time - w->lastClick < 400 && std::abs(e.xbutton.x - w->clickX) < 4 && std::abs(e.xbutton.y - w->clickY) < 4;
            w->lastClick = dbl ? 0 : e.xbutton.time;
            w->clickX = e.xbutton.x;
            w->clickY = e.xbutton.y;
            g.mouseDown(x, y, false, dbl, shift);
        } else if (e.xbutton.button == 3) {
            g.mouseDown(x, y, true, false, shift);
        }
        break;
    }
    case ButtonRelease: {
        const int x = floorDiv(e.xbutton.x, s), y = floorDiv(e.xbutton.y, s);
        const bool shift = (e.xbutton.state & ShiftMask) != 0;
        if (e.xbutton.button == 1) g.mouseUp(x, y, shift);
        else if (e.xbutton.button == 3) g.rightUp(x, y, shift);
        break;
    }
    case MotionNotify:
        g.mouseMove(floorDiv(e.xmotion.x, s), floorDiv(e.xmotion.y, s), (e.xmotion.state & ShiftMask) != 0);
        break;
    case LeaveNotify:
        if (!(e.xcrossing.state & Button1Mask)) g.mouseLeave();   // a drag keeps its implicit grab
        break;
    case KeyPress: {
        if (!g.wantsKeys()) break;
        char buf[16] = {};
        KeySym sym = 0;
        const int n = XLookupString(&e.xkey, buf, sizeof buf - 1, &sym, nullptr);
        const bool ctrl = (e.xkey.state & ControlMask) != 0;
        const Key k = keyOf(sym, ctrl);
        if (k != KeyNone) g.keyDown(k, (e.xkey.state & ShiftMask) != 0, ctrl);
        else if (n == 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) g.keyChar((unsigned char)buf[0]);   // Latin-1, as the fonts are
        break;
    }
    case FocusOut:
        g.focusLost();
        break;
    }
}

static void run(PlatformWindow* w) {
    auto next = std::chrono::steady_clock::now();
    const int xfd = ConnectionNumber(w->dpy);
    while (!w->quit) {
        const int wait = (int)std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()).count());
        pollfd fds[2] = {{xfd, POLLIN, 0}, {w->wake[0], POLLIN, 0}};
        if (!XPending(w->dpy)) poll(fds, 2, wait);
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (read(w->wake[0], buf, sizeof buf) > 0) {}
        }
        if (w->quit) break;
        std::lock_guard<std::recursive_mutex> g(w->lock);
        while (XPending(w->dpy)) {
            XEvent e;
            XNextEvent(w->dpy, &e);
            event(w, e);
        }
        if (std::chrono::steady_clock::now() >= next) {
            next += std::chrono::milliseconds(33);
            if (next < std::chrono::steady_clock::now()) next = std::chrono::steady_clock::now() + std::chrono::milliseconds(33);
            w->gui->tick();
        }
        Rect r;
        int ww, wh;
        {
            std::lock_guard<std::mutex> p(w->pending);
            r = w->dirty;
            w->dirty = {};
            ww = w->wantW;
            wh = w->wantH;
            w->wantW = w->wantH = 0;
        }
        if (ww > 0 && wh > 0) XResizeWindow(w->dpy, w->win, (unsigned)ww, (unsigned)wh);
        if (!r.empty()) present(w, r);
        XFlush(w->dpy);
    }
}

PlatformWindow* platformOpen(void* parent, Gui* gui) {
    auto* w = new PlatformWindow;
    w->gui = gui;
    w->dpy = XOpenDisplay(nullptr);
    if (!w->dpy || pipe(w->wake) != 0) {
        if (w->dpy) XCloseDisplay(w->dpy);
        delete w;
        return nullptr;
    }
    for (int fd : w->wake) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    const int screen = DefaultScreen(w->dpy);
    XSetWindowAttributes a = {};
    a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | KeyPressMask | FocusChangeMask;
    a.background_pixel = BlackPixel(w->dpy, screen);
    const Window host = parent ? (Window)(uintptr_t)parent : RootWindow(w->dpy, screen);
    w->win = XCreateWindow(w->dpy, host, 0, 0, (unsigned)(gui->width() * gui->scale()), (unsigned)(gui->height() * gui->scale()), 0,
                           CopyFromParent, InputOutput, CopyFromParent, CWEventMask | CWBackPixel, &a);
    w->gc = XCreateGC(w->dpy, w->win, 0, nullptr);
    XMapWindow(w->dpy, w->win);
    XFlush(w->dpy);
    w->thread = std::thread(run, w);
    return w;
}

void platformClose(PlatformWindow* w) {
    w->quit = true;
    poke(w);
    if (w->thread.joinable()) w->thread.join();
    if (w->img) {
        w->img->data = nullptr;
        XDestroyImage(w->img);
    }
    XFreeGC(w->dpy, w->gc);
    XDestroyWindow(w->dpy, w->win);
    XCloseDisplay(w->dpy);
    close(w->wake[0]);
    close(w->wake[1]);
    delete w;
}

void platformHold(PlatformWindow* w, bool hold) {
    if (hold) w->lock.lock();
    else w->lock.unlock();
}

void platformInvalidate(PlatformWindow* w, Rect r) {
    {
        std::lock_guard<std::mutex> p(w->pending);
        w->dirty = w->dirty | r;
    }
    if (std::this_thread::get_id() != w->thread.get_id()) poke(w);
}

void platformSize(PlatformWindow* w, int width, int height) {
    {
        std::lock_guard<std::mutex> p(w->pending);
        w->wantW = width;
        w->wantH = height;
    }
    platformInvalidate(w, {0, 0, w->gui->width(), w->gui->height()});
}

void platformResizeParents(PlatformWindow*, int, int) {}   // X11 hosts size their own frames

// Native menus are the skin's own on Linux: a skin without a "menu" style gets no menus here.
const bool kNativeMenus = false;
int platformMenu(PlatformWindow*, const std::vector<MenuEntry>&, int, int) { return -1; }

void platformTip(PlatformWindow*, const std::string&) {}   // skinned tooltips only, as for menus

void platformFocus(PlatformWindow* w, bool on) {
    if (on) XSetInputFocus(w->dpy, w->win, RevertToParent, CurrentTime);   // only ever called on the window's own thread
}

// Runs a program with arguments, no shell, collecting its standard output into out when given. Its exit
// status once it ends (0 at once without out), -1 when it cannot be started.
static int spawn(const std::vector<std::string>& args, std::string* out) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int fd[2] = {-1, -1};
    if (out && pipe(fd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out) {
        posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
        posix_spawn_file_actions_addclose(&fa, fd[0]);
    }
    pid_t pid = 0;
    const bool ok = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    if (out) {
        close(fd[1]);
        char buf[4096];
        for (ssize_t n; ok && (n = read(fd[0], buf, sizeof buf)) != 0;) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) break;
            out->append(buf, (size_t)n);
        }
        close(fd[0]);
    }
    if (!ok) return -1;
    if (!out) return 0;   // a browser keeps running
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void platformOpenUrl(const std::string& url) {
    if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) spawn({"xdg-open", url}, nullptr);
}

// zenity, else kdialog: the file dialogs every desktop has one of. False when neither is installed.
bool platformFileDialog(PlatformWindow*, bool save, const std::string& title, const Vars& types, const std::string& name, std::string& path) {
    std::vector<std::string> z = {"zenity", "--file-selection", "--title=" + title};
    std::string kfilter;
    if (save) {
        z.push_back("--save");
        z.push_back("--confirm-overwrite");
        if (!name.empty()) z.push_back("--filename=" + name);
    }
    for (auto& t : types) {
        std::string pats;
        for (size_t a = 0; a <= t.second.size();) {
            size_t b = t.second.find(';', a);
            std::string e = t.second.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!e.empty()) pats += (pats.empty() ? "*." : " *.") + e;
            if (b == std::string::npos) break;
            a = b + 1;
        }
        z.push_back("--file-filter=" + t.first + " | " + pats);
        kfilter += (kfilter.empty() ? "" : "\n") + t.first + " (" + pats + ")";
    }
    std::string out;
    int rc = spawn(z, &out);
    if (rc < 0 || rc > 1) {   // no zenity (1 is a cancel)
        out.clear();
        std::vector<std::string> k = {"kdialog", "--title", title, save ? "--getsavefilename" : "--getopenfilename", save && !name.empty() ? name : ".", kfilter};
        rc = spawn(k, &out);
    }
    if (rc != 0) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    path = out;
    return !path.empty();
}

} // namespace hollow
