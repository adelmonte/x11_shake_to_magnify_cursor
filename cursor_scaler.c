#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#define MIN_SCALE 1.0
#define MAX_SCALE 30.0
#define SCALE_STEP 0.3
#define SHAKE_THRESHOLD 8
#define SHAKE_TIMEOUT 0.3
#define MOVEMENT_THRESHOLD 5

typedef struct {
    double x;
    double y;
} Point;

typedef struct {
    Display *display;
    Window window;
    GC gc;
    int screen;
    Point last_pos;
    char *last_direction;
    int direction_changes;
    double last_change_time;
    double last_movement_time;
    int is_active;
    int is_scaling;
    double current_scale;
    double target_scale;
    XcursorImage *cursor_image;
    Picture window_picture;
    Picture cursor_picture;
    Visual *visual;
    int depth;
    Cursor invisible_cursor;
    Colormap colormap;
} CursorScaler;

double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (ts.tv_nsec / 1000000000.0);
}

char* get_movement_direction(Point delta) {
    if (fabs(delta.x) <= MOVEMENT_THRESHOLD && fabs(delta.y) <= MOVEMENT_THRESHOLD)
        return NULL;
        
    if (fabs(delta.x) > fabs(delta.y))
        return delta.x > 0 ? "right" : "left";
    else if (fabs(delta.y) > fabs(delta.x))
        return delta.y > 0 ? "down" : "up";
    
    return NULL;
}

void load_system_cursor(CursorScaler *scaler) {
    scaler->cursor_image = XcursorLibraryLoadImage("left_ptr", NULL, 32);
    if (!scaler->cursor_image) {
        fprintf(stderr, "Failed to load cursor image\n");
        exit(1);
    }

    XColor black;
    char blank_data[1] = {0};
    Pixmap blank = XCreateBitmapFromData(scaler->display, 
                                        scaler->window,
                                        blank_data, 1, 1);
    black.red = black.green = black.blue = 0;
    scaler->invisible_cursor = XCreatePixmapCursor(scaler->display, 
                                                 blank, blank,
                                                 &black, &black, 0, 0);
    XFreePixmap(scaler->display, blank);
}

void create_cursor_picture(CursorScaler *scaler) {
    XRenderPictFormat *format = XRenderFindVisualFormat(scaler->display, scaler->visual);
    if (!format) {
        fprintf(stderr, "Failed to find visual format\n");
        exit(1);
    }

    Pixmap pixmap = XCreatePixmap(scaler->display,
                                 scaler->window,
                                 scaler->cursor_image->width,
                                 scaler->cursor_image->height,
                                 32);

    Picture picture = XRenderCreatePicture(scaler->display,
                                         pixmap,
                                         format,
                                         0, NULL);

    GC gc = XCreateGC(scaler->display, pixmap, 0, NULL);
    
    XImage *image = XCreateImage(scaler->display,
                                scaler->visual,
                                32,
                                ZPixmap,
                                0,
                                (char *)scaler->cursor_image->pixels,
                                scaler->cursor_image->width,
                                scaler->cursor_image->height,
                                32,
                                0);

    XPutImage(scaler->display,
              pixmap,
              gc,
              image,
              0, 0, 0, 0,
              scaler->cursor_image->width,
              scaler->cursor_image->height);

    image->data = NULL;
    XDestroyImage(image);
    XFreeGC(scaler->display, gc);
    XFreePixmap(scaler->display, pixmap);

    scaler->cursor_picture = picture;
    scaler->window_picture = XRenderCreatePicture(scaler->display,
                                                 scaler->window,
                                                 format,
                                                 0, NULL);
                                                 
    // Set antialiasing filter for the cursor picture
    XRenderSetPictureFilter(scaler->display, scaler->cursor_picture, "best", NULL, 0);
}

void update_scale(CursorScaler *scaler) {
    if (!scaler->is_scaling) {
        scaler->current_scale = fmax(MIN_SCALE, scaler->current_scale - 1.0);
        if (scaler->current_scale <= MIN_SCALE) {
            XUnmapWindow(scaler->display, scaler->window);
            return;
        }
    }

    if (scaler->is_scaling && scaler->current_scale < scaler->target_scale) {
        scaler->current_scale = fmin(scaler->target_scale, scaler->current_scale + SCALE_STEP);
        XMapWindow(scaler->display, scaler->window);
    }
}

void update_cursor(CursorScaler *scaler) {
    Window root, child;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    
    XQueryPointer(scaler->display, DefaultRootWindow(scaler->display),
                 &root, &child, &root_x, &root_y, &win_x, &win_y, &mask);
    
    Point current_pos = {root_x, root_y};
    Point delta = {
        current_pos.x - scaler->last_pos.x,
        current_pos.y - scaler->last_pos.y
    };
    
    double distance = sqrt(delta.x * delta.x + delta.y * delta.y);
    double current_time = get_time();

    if (distance > MOVEMENT_THRESHOLD) {
        scaler->last_movement_time = current_time;
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
    }

    if (current_time - scaler->last_change_time > SHAKE_TIMEOUT) {
        scaler->direction_changes = 0;
    }

    if (scaler->direction_changes >= SHAKE_THRESHOLD) {
        if (!scaler->is_scaling) {
            scaler->is_scaling = 1;
            scaler->current_scale = MIN_SCALE;
            scaler->target_scale = MIN_SCALE + 1.0;
        } else {
            scaler->target_scale = fmin(MAX_SCALE, scaler->target_scale + 0.1);
        }
    } else {
        scaler->is_scaling = 0;
        scaler->target_scale = MIN_SCALE;
    }

    update_scale(scaler);
    
    if (scaler->current_scale > MIN_SCALE) {
        int scaled_size = (int)(scaler->cursor_image->width * scaler->current_scale);
        
        int x = current_pos.x - (scaled_size * scaler->cursor_image->xhot / scaler->cursor_image->width);
        int y = current_pos.y - (scaled_size * scaler->cursor_image->yhot / scaler->cursor_image->height);
        
        XMoveResizeWindow(scaler->display, scaler->window,
                         x, y, scaled_size, scaled_size);

        XTransform transform;
        memset(&transform, 0, sizeof(transform));
        transform.matrix[0][0] = XDoubleToFixed(1.0 / scaler->current_scale);
        transform.matrix[1][1] = XDoubleToFixed(1.0 / scaler->current_scale);
        transform.matrix[2][2] = XDoubleToFixed(1.0);
        
        XRenderSetPictureTransform(scaler->display, scaler->cursor_picture, &transform);
        
        // Set antialiasing filter before compositing
        XRenderSetPictureFilter(scaler->display, scaler->cursor_picture, "best", NULL, 0);
        
        XRenderComposite(scaler->display,
                        PictOpOver,
                        scaler->cursor_picture,
                        None,
                        scaler->window_picture,
                        0, 0,
                        0, 0,
                        0, 0,
                        scaled_size, scaled_size);
    }

    scaler->last_pos = current_pos;
}

int main() {
    CursorScaler scaler = {0};
    
    scaler.display = XOpenDisplay(NULL);
    if (!scaler.display) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    
    scaler.screen = DefaultScreen(scaler.display);

    // Get RGBA Visual
    XVisualInfo vinfo_template;
    vinfo_template.screen = scaler.screen;
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
    
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.colormap = scaler.colormap;
    
    scaler.window = XCreateWindow(scaler.display, 
                                DefaultRootWindow(scaler.display),
                                0, 0, 32, 32,
                                0,
                                scaler.depth,
                                InputOutput,
                                scaler.visual,
                                CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap,
                                &attrs);
    
    load_system_cursor(&scaler);
    create_cursor_picture(&scaler);
    
    XStoreName(scaler.display, scaler.window, "Cursor Scaler");
    
    // Hide the system cursor
    XDefineCursor(scaler.display, DefaultRootWindow(scaler.display), 
                 scaler.invisible_cursor);
    
    scaler.current_scale = MIN_SCALE;
    scaler.target_scale = MIN_SCALE;
    scaler.last_movement_time = get_time();
    
    while (1) {
        update_cursor(&scaler);
        XFlush(scaler.display);
        usleep(16666);  // ~60 FPS
    }
    
    return 0;
}