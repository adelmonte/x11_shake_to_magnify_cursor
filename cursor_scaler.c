#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#define MIN_SCALE 1.0
#define MAX_SCALE 30.0
#define SCALE_STEP 0.8
#define SHAKE_THRESHOLD 8
#define SHAKE_TIMEOUT 0.3
#define MOVEMENT_THRESHOLD 5

typedef struct {
    double x, y;
} Point;

typedef struct {
    Display *display;
    Window window;
    int screen;
    Point last_pos;
    Point current_pos;
    char *last_direction;
    int direction_changes;
    double last_change_time;
    double current_scale;
    double target_scale;
    XcursorImage *cursor_image;
    Picture window_picture;
    Picture cursor_picture;
    Visual *visual;
    int depth;
    Cursor invisible_cursor;
    Colormap colormap;
    bool needs_update;
    bool is_shaking;
    bool cursor_hidden;
    int xi_opcode;
} CursorScaler;

CursorScaler *global_scaler = NULL;

double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (ts.tv_nsec / 1000000000.0);
}

void cleanup_and_exit(int signum) {
    if (global_scaler && global_scaler->display) {
        if (global_scaler->cursor_hidden) {
            XUndefineCursor(global_scaler->display, DefaultRootWindow(global_scaler->display));
            XFixesShowCursor(global_scaler->display, DefaultRootWindow(global_scaler->display));
        }
        XFlush(global_scaler->display);
        XCloseDisplay(global_scaler->display);
    }
    exit(0);
}

char* get_movement_direction(Point delta) {
    if (fabs(delta.x) <= MOVEMENT_THRESHOLD && fabs(delta.y) <= MOVEMENT_THRESHOLD)
        return NULL;
    return (fabs(delta.x) > fabs(delta.y)) 
        ? (delta.x > 0 ? "right" : "left")
        : (delta.y > 0 ? "down" : "up");
}

void load_system_cursor(CursorScaler *scaler) {
    scaler->cursor_image = XcursorLibraryLoadImage("left_ptr", NULL, 32);
    if (!scaler->cursor_image) {
        fprintf(stderr, "Failed to load cursor image\n");
        exit(1);
    }

    char blank_data[1] = {0};
    Pixmap blank = XCreateBitmapFromData(scaler->display, scaler->window, blank_data, 1, 1);
    XColor black = {0};
    scaler->invisible_cursor = XCreatePixmapCursor(scaler->display, blank, blank, &black, &black, 0, 0);
    XFreePixmap(scaler->display, blank);
}

void create_cursor_picture(CursorScaler *scaler) {
    XRenderPictFormat *format = XRenderFindVisualFormat(scaler->display, scaler->visual);
    if (!format) {
        fprintf(stderr, "Failed to find visual format\n");
        exit(1);
    }

    Pixmap pixmap = XCreatePixmap(scaler->display, scaler->window,
                                 scaler->cursor_image->width,
                                 scaler->cursor_image->height, 32);

    Picture picture = XRenderCreatePicture(scaler->display, pixmap, format, 0, NULL);
    GC gc = XCreateGC(scaler->display, pixmap, 0, NULL);
    
    XImage *image = XCreateImage(scaler->display, scaler->visual, 32, ZPixmap, 0,
                                (char *)scaler->cursor_image->pixels,
                                scaler->cursor_image->width,
                                scaler->cursor_image->height, 32, 0);

    XPutImage(scaler->display, pixmap, gc, image, 0, 0, 0, 0,
              scaler->cursor_image->width, scaler->cursor_image->height);

    image->data = NULL;
    XDestroyImage(image);
    XFreeGC(scaler->display, gc);
    XFreePixmap(scaler->display, pixmap);

    scaler->cursor_picture = picture;
    scaler->window_picture = XRenderCreatePicture(scaler->display, scaler->window, format, 0, NULL);
    XRenderSetPictureFilter(scaler->display, scaler->cursor_picture, "best", NULL, 0);
}

void hide_system_cursor(CursorScaler *scaler) {
    if (!scaler->cursor_hidden) {
        XFixesHideCursor(scaler->display, DefaultRootWindow(scaler->display));
        scaler->cursor_hidden = true;
    }
}

void show_system_cursor(CursorScaler *scaler) {
    if (scaler->cursor_hidden) {
        XFixesShowCursor(scaler->display, DefaultRootWindow(scaler->display));
        scaler->cursor_hidden = false;
    }
}

void render_cursor(CursorScaler *scaler, int x, int y) {
    if (scaler->current_scale <= MIN_SCALE) {
        XUnmapWindow(scaler->display, scaler->window);
        show_system_cursor(scaler);
        return;
    }

    hide_system_cursor(scaler);

    int scaled_size = (int)(scaler->cursor_image->width * scaler->current_scale);
    int offset_x = x - (scaled_size * scaler->cursor_image->xhot / scaler->cursor_image->width);
    int offset_y = y - (scaled_size * scaler->cursor_image->yhot / scaler->cursor_image->height);
    
    XMoveResizeWindow(scaler->display, scaler->window, offset_x, offset_y, scaled_size, scaled_size);
    
    XTransform transform;
    memset(&transform, 0, sizeof(transform));
    double scale_recip = 1.0 / scaler->current_scale;
    transform.matrix[0][0] = XDoubleToFixed(scale_recip);
    transform.matrix[1][1] = XDoubleToFixed(scale_recip);
    transform.matrix[2][2] = XDoubleToFixed(1.0);
    
    XRenderSetPictureTransform(scaler->display, scaler->cursor_picture, &transform);
    XRenderSetPictureFilter(scaler->display, scaler->cursor_picture, "best", NULL, 0);
    
    XRenderComposite(scaler->display, PictOpSrc, scaler->cursor_picture, None,
                    scaler->window_picture, 0, 0, 0, 0, 0, 0, scaled_size, scaled_size);
    
    XMapWindow(scaler->display, scaler->window);
}

void handle_motion(CursorScaler *scaler, double x, double y) {
    Point current_pos = {x, y};
    Point delta = {
        current_pos.x - scaler->last_pos.x,
        current_pos.y - scaler->last_pos.y
    };
    
    double distance = sqrt(delta.x * delta.x + delta.y * delta.y);
    if (distance < 1.0) return;
    
    double current_time = get_time();
    scaler->current_pos = current_pos;

    if (current_time - scaler->last_change_time > SHAKE_TIMEOUT) {
        scaler->direction_changes = 0;
        scaler->is_shaking = false;
    }

    char* current_direction = get_movement_direction(delta);
    if (current_direction && current_direction != scaler->last_direction) {
        if (current_time - scaler->last_change_time < SHAKE_TIMEOUT) {
            scaler->direction_changes++;
        } else {
            scaler->direction_changes = 1;
        }
        scaler->last_change_time = current_time;
        scaler->last_direction = current_direction;
    }

    if (scaler->direction_changes >= SHAKE_THRESHOLD) {
        scaler->is_shaking = true;
        scaler->target_scale = fmin(MAX_SCALE, scaler->target_scale + 0.15);
        if (scaler->current_scale < MIN_SCALE + 0.5) {
            scaler->current_scale = MIN_SCALE + 1.0;
        }
    } else {
        scaler->target_scale = MIN_SCALE;
    }

    scaler->last_pos = current_pos;
    scaler->needs_update = true;
}

void check_shake_timeout(CursorScaler *scaler) {
    double current_time = get_time();
    
    if (current_time - scaler->last_change_time > SHAKE_TIMEOUT) {
        if (scaler->direction_changes > 0 || scaler->is_shaking) {
            scaler->direction_changes = 0;
            scaler->is_shaking = false;
            scaler->target_scale = MIN_SCALE;
            scaler->needs_update = true;
        }
    }
}

void update_scale(CursorScaler *scaler) {
    bool changed = false;
    
    if (scaler->current_scale > scaler->target_scale) {
        scaler->current_scale = fmax(scaler->target_scale, scaler->current_scale - SCALE_STEP);
        changed = true;
    } else if (scaler->current_scale < scaler->target_scale) {
        scaler->current_scale = fmin(scaler->target_scale, scaler->current_scale + SCALE_STEP);
        changed = true;
    }
    
    if (changed) {
        scaler->needs_update = true;
    }
}

int main() {
    CursorScaler scaler = {0};
    global_scaler = &scaler;
    
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    signal(SIGHUP, cleanup_and_exit);
    
    scaler.display = XOpenDisplay(NULL);
    if (!scaler.display) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    
    scaler.screen = DefaultScreen(scaler.display);

    int event, error;
    if (!XQueryExtension(scaler.display, "XInputExtension", &scaler.xi_opcode, &event, &error)) {
        fprintf(stderr, "XInput extension not available\n");
        return 1;
    }

    int major = 2, minor = 0;
    if (XIQueryVersion(scaler.display, &major, &minor) != Success) {
        fprintf(stderr, "XInput2 not available\n");
        return 1;
    }

    XVisualInfo vinfo_template = {.screen = scaler.screen};
    int nvisuals;
    XVisualInfo *vinfo = XGetVisualInfo(scaler.display, VisualScreenMask, &vinfo_template, &nvisuals);
    
    Visual *argb_visual = NULL;
    for (int i = 0; i < nvisuals; i++) {
        XRenderPictFormat *format = XRenderFindVisualFormat(scaler.display, vinfo[i].visual);
        if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
            argb_visual = vinfo[i].visual;
            scaler.depth = vinfo[i].depth;
            break;
        }
    }
    XFree(vinfo);

    if (!argb_visual) {
        fprintf(stderr, "No ARGB visual found\n");
        return 1;
    }

    scaler.visual = argb_visual;
    scaler.colormap = XCreateColormap(scaler.display, DefaultRootWindow(scaler.display), argb_visual, AllocNone);
    
    XSetWindowAttributes attrs = {
        .override_redirect = True,
        .background_pixel = 0,
        .border_pixel = 0,
        .colormap = scaler.colormap
    };
    
    scaler.window = XCreateWindow(scaler.display, DefaultRootWindow(scaler.display),
                                0, 0, 32, 32, 0, scaler.depth, InputOutput, scaler.visual,
                                CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap, &attrs);
    
    load_system_cursor(&scaler);
    create_cursor_picture(&scaler);
    
    scaler.current_scale = MIN_SCALE;
    scaler.target_scale = MIN_SCALE;
    scaler.cursor_hidden = false;
    double start_time = get_time();
    scaler.last_change_time = start_time;
    
    Window root = DefaultRootWindow(scaler.display);
    
    XIEventMask mask;
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = XIMaskLen(XI_RawMotion);
    mask.mask = calloc(mask.mask_len, sizeof(char));
    XISetMask(mask.mask, XI_RawMotion);
    XISelectEvents(scaler.display, root, &mask, 1);
    free(mask.mask);
    
    Window root_return, child_return;
    int root_x, root_y, win_x, win_y;
    unsigned int mask_return;
    XQueryPointer(scaler.display, root, &root_return, &child_return,
                 &root_x, &root_y, &win_x, &win_y, &mask_return);
    scaler.last_pos.x = root_x;
    scaler.last_pos.y = root_y;
    scaler.current_pos = scaler.last_pos;
    
    XEvent ev;
    struct timespec timeout;
    fd_set fds;
    int x11_fd = ConnectionNumber(scaler.display);
    
    while (1) {
        bool needs_animation = (scaler.current_scale > MIN_SCALE);
        bool is_active = (needs_animation || 
                         scaler.direction_changes > 0 ||
                         scaler.is_shaking);
        
        FD_ZERO(&fds);
        FD_SET(x11_fd, &fds);
        
        if (is_active) {
            timeout.tv_sec = 0;
            timeout.tv_nsec = 16666666;
        } else {
            timeout.tv_sec = 1;
            timeout.tv_nsec = 0;
        }
        
        int ret = pselect(x11_fd + 1, &fds, NULL, NULL, &timeout, NULL);
        
        if (ret > 0) {
            while (XPending(scaler.display)) {
                XNextEvent(scaler.display, &ev);
                
                XGenericEventCookie *cookie = &ev.xcookie;
                if (cookie->type == GenericEvent && 
                    cookie->extension == scaler.xi_opcode &&
                    XGetEventData(scaler.display, cookie)) {
                    
                    if (cookie->evtype == XI_RawMotion) {
                        XQueryPointer(scaler.display, root, &root_return, &child_return,
                                    &root_x, &root_y, &win_x, &win_y, &mask_return);
                        
                        handle_motion(&scaler, root_x, root_y);
                    }
                    
                    XFreeEventData(scaler.display, cookie);
                }
            }
        }
        
        if (is_active) {
            check_shake_timeout(&scaler);
            update_scale(&scaler);
            
            if (scaler.needs_update) {
                render_cursor(&scaler, (int)scaler.current_pos.x, (int)scaler.current_pos.y);
                scaler.needs_update = false;
                XFlush(scaler.display);
            }
        }
    }
    
    return 0;
}
