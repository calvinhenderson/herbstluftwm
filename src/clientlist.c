/** Copyright 2011-2013 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */

#include "clientlist.h"
#include "settings.h"
#include "globals.h"
#include "layout.h"
#include "stack.h"
#include "utils.h"
#include "hook.h"
#include "mouse.h"
#include "ewmh.h"
#include "rules.h"
#include "ipc-protocol.h"
#include "object.h"
// system
#include "glib-backports.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <regex.h>
#include <stdbool.h>
#include <string.h>
// gui
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

int g_monitor_float_treshold = 24;

int* g_window_border_width;
int* g_window_border_inner_width;
int* g_raise_on_focus;
int* g_snap_gap;
int* g_smart_window_surroundings;
unsigned long g_window_border_active_color;
unsigned long g_window_border_normal_color;
unsigned long g_window_border_urgent_color;
unsigned long g_window_border_inner_color;

static GHashTable* g_clients; // container of all clients
static HSObject*   g_client_object;

// atoms from dwm.c
// default atoms
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast };
static Atom g_wmatom[WMLast];

static void client_set_urgent_force(HSClient* client, bool state);

static HSClient* create_client() {
    HSClient* hc = g_new0(HSClient, 1);
    hsobject_init(&hc->object);
    hc->window_str = NULL;
    hc->float_size.width = 100;
    hc->float_size.height = 100;
    hc->title = g_string_new("");
    hc->urgent = false;
    hc->fullscreen = false;
    hc->ewmhfullscreen = false;
    hc->pseudotile = false;
    hc->ewmhrequests = true;
    hc->ewmhnotify = true;
    hc->sizehints = true;
    return hc;
}

static void fetch_colors() {
    g_window_border_width = &(settings_find("window_border_width")->value.i);
    g_window_border_inner_width = &(settings_find("window_border_inner_width")->value.i);
    g_window_gap = &(settings_find("window_gap")->value.i);
    g_snap_gap = &(settings_find("snap_gap")->value.i);
    g_smart_window_surroundings = &(settings_find("smart_window_surroundings")->value.i);
    g_raise_on_focus = &(settings_find("raise_on_focus")->value.i);
    char* str = settings_find("window_border_normal_color")->value.s;
    g_window_border_normal_color = getcolor(str);
    str = settings_find("window_border_active_color")->value.s;
    g_window_border_active_color = getcolor(str);
    str = settings_find("window_border_urgent_color")->value.s;
    g_window_border_urgent_color = getcolor(str);
    str = settings_find("window_border_inner_color")->value.s;
    g_window_border_inner_color = getcolor(str);
}

void clientlist_init() {
    // init regex simple..
    fetch_colors();
    g_wmatom[WMProtocols] = XInternAtom(g_display, "WM_PROTOCOLS", False);
    g_wmatom[WMDelete] = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    g_wmatom[WMState] = XInternAtom(g_display, "WM_STATE", False);
    g_wmatom[WMTakeFocus] = XInternAtom(g_display, "WM_TAKE_FOCUS", False);
    // init actual client list
    g_client_object = hsobject_create_and_link(hsobject_root(), "clients");
    g_clients = g_hash_table_new_full(g_int_hash, g_int_equal,
                                      NULL, (GDestroyNotify)client_destroy);
}

void reset_client_colors() {
    fetch_colors();
    all_monitors_apply_layout();
}

static void client_move_to_floatpos(void* key, void* client_void, void* data) {
    (void)key;
    (void)data;
    HSClient* client = client_void;
    if (client) {
        int x = client->float_size.x;
        int y = client->float_size.y;
        unsigned int w = client->float_size.width;
        unsigned int h = client->float_size.height;
        XMoveResizeWindow(g_display, client->window, x, y, w, h);
    }
}

static void client_show_window(void* key, void* client_void, void* data) {
    (void)key;
    (void)data;
    HSClient* client = client_void;
    window_show(client->window);
}

void clientlist_destroy() {
    // move all clients to their original floating position
    g_hash_table_foreach(g_clients, client_move_to_floatpos, NULL);
    g_hash_table_foreach(g_clients, client_show_window, NULL);

    g_hash_table_destroy(g_clients);
    hsobject_unlink_and_destroy(hsobject_root(), g_client_object);
}


void clientlist_foreach(GHFunc func, gpointer data) {
    g_hash_table_foreach(g_clients, func, data);
}

HSClient* get_client_from_window(Window window) {
    return (HSClient*) g_hash_table_lookup(g_clients, &window);
}

#define CLIENT_UPDATE_ATTR(FUNC,MEMBER) do { \
        HSClient* client = container_of(attr->value.b, HSClient, MEMBER); \
        FUNC(client, client->MEMBER); \
        return NULL; \
    }   \
    while (0);

static void client_attr_tag(void* data, GString* output) {
    HSClient* client = (HSClient*) data;
    g_string_append(output, client->tag->display_name->str);
}

static void client_attr_class(void* data, GString* output) {
    HSClient* client = (HSClient*) data;
    GString* ret = window_class_to_g_string(g_display, client->window);
    g_string_append(output, ret->str);
    g_string_free(ret, true);
}

static void client_attr_instance(void* data, GString* output) {
    HSClient* client = (HSClient*) data;
    GString* ret = window_instance_to_g_string(g_display, client->window);
    g_string_append(output, ret->str);
    g_string_free(ret, true);
}

static GString* client_attr_fullscreen(HSAttribute* attr) {
    CLIENT_UPDATE_ATTR(client_set_fullscreen, fullscreen);
}

static GString* client_attr_pseudotile(HSAttribute* attr) {
    CLIENT_UPDATE_ATTR(client_set_pseudotile, pseudotile);
}

static GString* client_attr_urgent(HSAttribute* attr) {
    CLIENT_UPDATE_ATTR(client_set_urgent_force, urgent);
}

HSClient* manage_client(Window win) {
    if (is_herbstluft_window(g_display, win)) {
        // ignore our own window
        return NULL;
    }
    if (get_client_from_window(win)) {
        return NULL;
    }
    // init client
    HSClient* client = create_client();
    client->pid = window_pid(g_display, win);
    HSMonitor* m = get_current_monitor();
    // set to window properties
    client->window = win;
    client_update_title(client);

    unsigned int border, depth;
    Window root_win;
    int x, y;
    unsigned int w, h;
    XGetGeometry(g_display, win, &root_win, &x, &y, &w, &h, &border, &depth);
    // treat wanted coordinates as floating coords
    client->float_size.x = x;
    client->float_size.y = y;
    client->float_size.width = w;
    client->float_size.height = h;

    // apply rules
    HSClientChanges changes;
    client_changes_init(&changes, client);
    rules_apply(client, &changes);
    if (changes.tag_name) {
        client->tag = find_tag(changes.tag_name->str);
    }

    if (!changes.manage) {
        client_changes_free_members(&changes);
        client_destroy(client);
        // map it... just to be sure
        XMapWindow(g_display, win);
        return NULL;
    }

    // actually manage it
    g_hash_table_insert(g_clients, &(client->window), client);
    client->window_str = g_string_sized_new(10);
    g_string_printf(client->window_str, "0x%lx", win);
    hsobject_link(g_client_object, &client->object, client->window_str->str);
    // insert to layout
    if (!client->tag) {
        client->tag = m->tag;
    }
    // get events from window
    XSelectInput(g_display, win, CLIENT_EVENT_MASK);
    // insert window to the stack
    client->slice = slice_create_client(client);
    stack_insert_slice(client->tag->stack, client->slice);
    // insert window to the tag
    frame_insert_client(lookup_frame(client->tag->frame, changes.tree_index->str), client);
    client_update_wm_hints(client);
    if (changes.focus) {
        // give focus to window if wanted
        // TODO: make this faster!
        // WARNING: this solution needs O(C + exp(D)) time where W is the count
        // of clients on this tag and D is the depth of the binary layout tree
        frame_focus_client(client->tag->frame, client);
    }

    client->object.data = client;

    HSAttribute attributes[] = {
        ATTRIBUTE_STRING(   "winid",        client->window_str,     ATTR_READ_ONLY),
        ATTRIBUTE_STRING(   "title",        client->title,          ATTR_READ_ONLY),
        ATTRIBUTE_CUSTOM(   "tag",          client_attr_tag,        ATTR_READ_ONLY),
        ATTRIBUTE_INT(      "pid",          client->pid,            ATTR_READ_ONLY),
        ATTRIBUTE_CUSTOM(   "class",        client_attr_class,      ATTR_READ_ONLY),
        ATTRIBUTE_CUSTOM(   "instance",     client_attr_instance,   ATTR_READ_ONLY),
        ATTRIBUTE_BOOL(     "fullscreen",   client->fullscreen,     client_attr_fullscreen),
        ATTRIBUTE_BOOL(     "pseudotile",   client->pseudotile,     client_attr_pseudotile),
        ATTRIBUTE_BOOL(     "ewmhrequests", client->ewmhrequests,   ATTR_ACCEPT_ALL),
        ATTRIBUTE_BOOL(     "ewmhnotify",   client->ewmhnotify,     ATTR_ACCEPT_ALL),
        ATTRIBUTE_BOOL(     "sizehints",    client->sizehints,      ATTR_ACCEPT_ALL),
        ATTRIBUTE_BOOL(     "urgent",       client->urgent,         client_attr_urgent),
        ATTRIBUTE_LAST,
    };
    hsobject_set_attributes(&client->object, attributes);

    ewmh_window_update_tag(client->window, client->tag);
    tag_set_flags_dirty();
    client_set_fullscreen(client, changes.fullscreen);
    ewmh_update_window_state(client);
    // add client after setting the correct tag for the new client
    // this ensures a panel can read the tag property correctly at this point
    ewmh_add_client(client->window);

    HSMonitor* monitor = find_monitor_with_tag(client->tag);
    if (monitor) {
        if (monitor != get_current_monitor()
            && changes.focus && changes.switchtag) {
            monitor_set_tag(get_current_monitor(), client->tag);
        }
        // TODO: monitor_apply_layout() maybe is called twice here if it
        // already is called by monitor_set_tag()
        monitor_apply_layout(monitor);
    } else {
        if (changes.focus && changes.switchtag) {
            monitor_set_tag(get_current_monitor(), client->tag);
        }
    }
    client_send_configure(client);
    updatesizehints(client);

    client_changes_free_members(&changes);
    grab_client_buttons(client, false);

    return client;
}

void unmanage_client(Window win) {
    HSClient* client = get_client_from_window(win);
    if (!client) {
        return;
    }
    if (client->dragged) {
        mouse_stop_drag();
    }
    // remove from tag
    frame_remove_client(client->tag->frame, client);
    // ignore events from it
    XSelectInput(g_display, win, 0);
    //XUngrabButton(g_display, AnyButton, AnyModifier, win);
    // permanently remove it
    HSTag* tag = client->tag;
    g_hash_table_remove(g_clients, &win);
    // and arrange monitor after the client has been removed from the stack
    HSMonitor* m = find_monitor_with_tag(tag);
    tag_update_focus_layer(tag);
    if (m) monitor_apply_layout(m);
    ewmh_remove_client(win);
    tag_set_flags_dirty();
}

// destroys a special client
void client_destroy(HSClient* client) {
    hsobject_unlink(g_client_object, &client->object);
    if (client->tag && client->slice) {
        stack_remove_slice(client->tag->stack, client->slice);
    }
    if (client->slice) {
        slice_destroy(client->slice);
    }
    if (client->title) {
        /* free window title */
        g_string_free(client->title, true);
    }
    if (client->window_str) {
        g_string_free(client->window_str, true);
    }
    hsobject_free(&client->object);
    g_free(client);
}

void window_unfocus(Window window) {
    HSDebug("window_unfocus NORMAL\n");
    window_update_border(window, g_window_border_normal_color);
    HSClient* c = get_client_from_window(window);
    if (c) {
        if (c->urgent) {
            HSDebug("window_unfocus URGENT\n");
            window_update_border(window, g_window_border_urgent_color);
        }
        grab_client_buttons(c, false);
    }
}

static Window lastfocus = 0;
void window_unfocus_last() {
    if (lastfocus) {
        window_unfocus(lastfocus);
    }
    hsobject_unlink_by_name(g_client_object, "focus");
    // give focus to root window
    XSetInputFocus(g_display, g_root, RevertToPointerRoot, CurrentTime);
    if (lastfocus) {
        /* only emit the hook if the focus *really* changes */
        hook_emit_list("focus_changed", "0x0", "", NULL);
        ewmh_update_active_window(None);
        tag_update_each_focus_layer();
    }
    lastfocus = 0;
}

void window_focus(Window window) {
    HSClient* client = get_client_from_window(window);
    assert(client != NULL);
    // set keyboard focus
    if (!client->neverfocus) {
        XSetInputFocus(g_display, window, RevertToPointerRoot, CurrentTime);
    }
    else client_sendevent(client, g_wmatom[WMTakeFocus]);

    if (window != lastfocus) {
        /* FIXME: this is a workaround because window_focus always is called
         * twice.  see BUGS for more information
         *
         * only emit the hook if the focus *really* changes */
        // unfocus last one
        window_unfocus(lastfocus);
        hsobject_link(g_client_object, &client->object, "focus");
        ewmh_update_active_window(window);
        tag_update_each_focus_layer();
        char* title = client ? client->title->str : "?";
        char winid_str[STRING_BUF_SIZE];
        snprintf(winid_str, STRING_BUF_SIZE, "0x%x", (unsigned int)window);
        hook_emit_list("focus_changed", winid_str, title, NULL);
    }

    // change window-colors
    HSDebug("window_focus ACTIVE\n");
    window_update_border(window, g_window_border_active_color);

    lastfocus = window;
    /* do some specials for the max layout */
    bool is_max_layout = frame_focused_client(g_cur_frame) == client
                         && g_cur_frame->content.clients.layout == LAYOUT_MAX
                         && get_current_monitor()->tag->floating == false;
    if (*g_raise_on_focus || is_max_layout) {
        client_raise(client);
    }
    tag_update_focus_layer(get_current_monitor()->tag);
    grab_client_buttons(get_client_from_window(window), true);
    client_set_urgent(client, false);
}

void client_setup_border(HSClient* client, bool focused) {
    unsigned long colors[] = {
        g_window_border_normal_color,
        g_window_border_active_color,
    };
    if (client->urgent) {
        HSDebug("client_setup_border URGENT\n");
        window_update_border(client->window, g_window_border_urgent_color);
    } else {
        HSDebug("client_setup_border %s\n", (focused ? "ACTIVE" : "NORMAL"));
        window_update_border(client->window, colors[focused ? 1 : 0]);
    }
}

static void client_resize_fullscreen(HSClient* client, HSMonitor* m) {
    if (!client || !m) {
        HSDebug("client_resize_fullscreen() got invalid parameters\n");
        return;
    }
    XSetWindowBorderWidth(g_display, client->window, 0);
    client->last_size = m->rect;
    client->last_border_width = 0;
    XMoveResizeWindow(g_display, client->window,
                      m->rect.x, m->rect.y, m->rect.width, m->rect.height);
    client_send_configure(client);
    XSync(g_display, False);
}

void client_raise(HSClient* client) {
    assert(client);
    stack_raise_slide(client->tag->stack, client->slice);
}


void client_resize_tiling(HSClient* client, Rectangle rect, HSFrame* frame) {
    HSMonitor* m;
    if (client->fullscreen && (m = find_monitor_with_tag(client->tag))) {
        client_resize_fullscreen(client, m);
        return;
    }
    // ensure minimum size
    if (rect.width < WINDOW_MIN_WIDTH) {
        rect.width = WINDOW_MIN_WIDTH;
    }
    if (rect.height < WINDOW_MIN_HEIGHT) {
        rect.height = WINDOW_MIN_HEIGHT;
    }
    if (!client) {
        HSDebug("Warning: client_resize(NULL, ...) was called\n");
        return;
    }
    Window win = client->window;
    if (client->pseudotile) {
        Rectangle size = client->float_size;
        // floating sizes don't include window border, tiling sizes do.
        // so convert the floating size to a tiling size
        size.width  += *g_window_border_width * 2;
        size.height += *g_window_border_width * 2;
        // ensure size is not larger than rect-tile
        size.width  = MIN(size.width, rect.width);
        size.height = MIN(size.height, rect.height);

        // center it
        rect.x = rect.x + rect.width/2 - size.width/2;
        rect.y = rect.y + rect.height/2 - size.height/2;
        rect.width = size.width;
        rect.height = size.height;
    }
    int border_width = *g_window_border_width;
    if (*g_smart_window_surroundings && !client->pseudotile
        && (frame->content.clients.count == 1
            || frame->content.clients.layout == LAYOUT_MAX)) {
        border_width = 0;
    }
    if (RECTANGLE_EQUALS(client->last_size, rect)
        && client->last_border_width == border_width) {
        return;
    }
    client->last_border_width = border_width;

    // apply border width
    bool is_max_layout = frame->content.clients.layout != LAYOUT_MAX;
    bool only_one_client = frame->content.clients.count != 1;
    bool smart_surroundings_are_applied = *g_smart_window_surroundings && is_max_layout && only_one_client;
    if (!client->pseudotile && !smart_surroundings_are_applied) {
        // apply window gap
        rect.width -= *g_window_gap;
        rect.height -= *g_window_gap;
    }

    client->last_size = rect;
    rect.width -= border_width * 2;
    rect.height -= border_width * 2;

    XWindowChanges changes = {
      .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height,
      .border_width = border_width
    };
    int mask = CWX | CWY | CWWidth | CWHeight | CWBorderWidth;
    XConfigureWindow(g_display, win, mask, &changes);
    if (*g_window_border_inner_width > 0
        && *g_window_border_inner_width < *g_window_border_width) {
        unsigned long current_border_color = get_window_border_color(client);
        HSDebug("client_resize %s\n",
                current_border_color == g_window_border_active_color
                ? "ACTIVE" : "NORMAL");
        set_window_double_border(g_display, win, *g_window_border_inner_width,
                                 g_window_border_inner_color,
                                 current_border_color);
    }
    //// send new size to client
    client_send_configure(client);
    XSync(g_display, False);
}


// from dwm.c
bool applysizehints(HSClient *c, int *x, int *y, int *w, int *h) {
    bool baseismin;

    /* set minimum possible */
    *w = MAX(1, *w);
    *h = MAX(1, *h);
    if(*h < WINDOW_MIN_HEIGHT)
        *h = WINDOW_MIN_HEIGHT;
    if(*w < WINDOW_MIN_WIDTH)
        *w = WINDOW_MIN_WIDTH;
    if(c->sizehints) { // TODO || c->isfloating
        /* see last two sentences in ICCCM 4.1.2.3 */
        baseismin = c->basew == c->minw && c->baseh == c->minh;
        if(!baseismin) { /* temporarily remove base dimensions */
            *w -= c->basew;
            *h -= c->baseh;
        }
        /* adjust for aspect limits */
        if(c->mina > 0 && c->maxa > 0) {
            if(c->maxa < (float)*w / *h)
                *w = *h * c->maxa + 0.5;
            else if(c->mina < (float)*h / *w)
                *h = *w * c->mina + 0.5;
        }
        if(baseismin) { /* increment calculation requires this */
            *w -= c->basew;
            *h -= c->baseh;
        }
        /* adjust for increment value */
        if(c->incw)
            *w -= *w % c->incw;
        if(c->inch)
            *h -= *h % c->inch;
        /* restore base dimensions */
        *w += c->basew;
        *h += c->baseh;
        *w = MAX(*w, c->minw);
        *h = MAX(*h, c->minh);
        if(c->maxw)
            *w = MIN(*w, c->maxw);
        if(c->maxh)
            *h = MIN(*h, c->maxh);
    }
    int bw = c->last_border_width;
    return *x != c->last_size.x || *y != c->last_size.y
        || *w != c->last_size.width - 2*bw || *h != c->last_size.height - 2*bw;
}

// from dwm.c
void updatesizehints(HSClient *c) {
    long msize;
    XSizeHints size;

    if(!XGetWMNormalHints(g_display, c->window, &size, &msize))
        /* size is uninitialized, ensure that size.flags aren't used */
        size.flags = PSize;
    if(size.flags & PBaseSize) {
        c->basew = size.base_width;
        c->baseh = size.base_height;
    }
    else if(size.flags & PMinSize) {
        c->basew = size.min_width;
        c->baseh = size.min_height;
    } else {
        c->basew = c->baseh = 0;
    }
    if(size.flags & PResizeInc) {
        c->incw = size.width_inc;
        c->inch = size.height_inc;
    }
    else
        c->incw = c->inch = 0;
    if(size.flags & PMaxSize) {
        c->maxw = size.max_width;
        c->maxh = size.max_height;
    } else {
        c->maxw = c->maxh = 0;
    }
    if(size.flags & PMinSize) {
        c->minw = size.min_width;
        c->minh = size.min_height;
    }
    else if(size.flags & PBaseSize) {
        c->minw = size.base_width;
        c->minh = size.base_height;
    } else {
        c->minw = c->minh = 0;
    }
    if(size.flags & PAspect) {
        c->mina = (float)size.min_aspect.y / size.min_aspect.x;
        c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
    } else {
        c->maxa = c->mina = 0.0;
    }
    //c->isfixed = (c->maxw && c->minw && c->maxh && c->minh
    //             && c->maxw == c->minw && c->maxh == c->minh);
}




void client_send_configure(HSClient *c) {
    XConfigureEvent ce;
    int bw = c->last_border_width;
    ce.type = ConfigureNotify;
    ce.display = g_display;
    ce.event = c->window;
    ce.window = c->window;
    ce.x = c->last_size.x;
    ce.y = c->last_size.y;
    ce.width = c->last_size.width - 2*bw;
    ce.height = c->last_size.height - 2*bw;
    ce.border_width = bw;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(g_display, c->window, False, StructureNotifyMask, (XEvent *)&ce);
}

void client_resize_floating(HSClient* client, HSMonitor* m) {
    if (!client || !m) return;
    if (client->fullscreen) {
        client_resize_fullscreen(client, m);
        return;
    }

    // ensure minimal size
    if (client->float_size.width < WINDOW_MIN_WIDTH)
        client->float_size.width = WINDOW_MIN_WIDTH;
    if (client->float_size.height < WINDOW_MIN_HEIGHT)
        client->float_size.height = WINDOW_MIN_HEIGHT;

    bool border_changed = false;
    if (client->last_border_width != *g_window_border_width) {
        client->last_border_width = *g_window_border_width;
        border_changed = true;
    }
    Rectangle rect = client->float_size;
    rect.x += m->rect.x + m->pad_left;
    rect.y += m->rect.y + m->pad_up;

    // ensure position is on monitor
    int space = g_monitor_float_treshold;
    rect.x =
        CLAMP(rect.x,
              m->rect.x + m->pad_left - client->last_size.width + space,
              m->rect.x + m->rect.width - m->pad_left - m->pad_right - space);
    rect.y =
        CLAMP(rect.y,
              m->rect.y + m->pad_up - client->last_size.height + space,
              m->rect.y + m->rect.height - m->pad_up - m->pad_down - space);
    if (applysizehints(client, &rect.x, &rect.y, &rect.width, &rect.height)) {
        XMoveResizeWindow(g_display, client->window,
            rect.x, rect.y, rect.width, rect.height);
    }
    if (border_changed) {
        XSetWindowBorderWidth(g_display, client->window, *g_window_border_width);
    }
    // add window border to last_size
    client->last_size = rect;
    client->last_size.width += 2 * client->last_border_width;
    client->last_size.height += 2 * client->last_border_width;
    if (*g_window_border_inner_width > 0
        && *g_window_border_inner_width < *g_window_border_width) {
        unsigned long current_border_color = get_window_border_color(client);
        HSDebug("client_resize %s\n",
                current_border_color == g_window_border_active_color
                ? "ACTIVE" : "NORMAL");
        set_window_double_border(g_display, client->window,
                                 *g_window_border_inner_width,
                                 g_window_border_inner_color,
                                 current_border_color);
    }
    client_send_configure(client);
    XSync(g_display, False);
}

Rectangle client_outer_floating_rect(HSClient* client) {
    Rectangle rect = client->float_size;
    rect.width  += *g_window_border_width * 2 + *g_snap_gap;
    rect.height += *g_window_border_width * 2 + *g_snap_gap;
    return rect;
}

int close_command(int argc, char** argv, GString* output) {
    Window win;
    HSClient* client = NULL;
    win = string_to_client((argc > 1) ? argv[1] : "", &client);
    if (win) window_close(win);
    else return HERBST_INVALID_ARGUMENT;
    return 0;
}

bool is_client_floated(HSClient* client) {
    return (client->tag->floating || client->pseudotile);
}

void window_close(Window window) {
    XEvent ev;
    ev.type = ClientMessage;
    ev.xclient.window = window;
    ev.xclient.message_type = g_wmatom[WMProtocols];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = g_wmatom[WMDelete];
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(g_display, window, False, NoEventMask, &ev);
}

void window_set_visible(Window win, bool visible) {
    static int (*action[])(Display*,Window) = {
        XUnmapWindow,
        XMapWindow,
    };
    unsigned long event_mask = CLIENT_EVENT_MASK;
    XGrabServer(g_display);
    XSelectInput(g_display, win, event_mask & ~StructureNotifyMask);
    XSelectInput(g_display, g_root, ROOT_EVENT_MASK & ~SubstructureNotifyMask);
    action[visible](g_display, win);
    XSelectInput(g_display, win, event_mask);
    XSelectInput(g_display, g_root, ROOT_EVENT_MASK);
    XUngrabServer(g_display);
}

void window_show(Window win) {
    window_set_visible(win, true);
}

void window_hide(Window win) {
    window_set_visible(win, false);
}

// heavily inspired by dwm.c
void client_set_urgent(HSClient* client, bool state) {
    if (client->urgent == state) {
        // nothing to do
        return;
    }
    client_set_urgent_force(client, state);
}

static void client_set_urgent_force(HSClient* client, bool state) {
    char winid_str[STRING_BUF_SIZE];
    snprintf(winid_str, STRING_BUF_SIZE, "0x%lx", client->window);
    hook_emit_list("urgent", state ? "on" : "off", winid_str, NULL);

    client->urgent = state;

    client_setup_border(client, client == frame_focused_client(g_cur_frame));

    XWMHints *wmh;
    if(!(wmh = XGetWMHints(g_display, client->window)))
        return;

    if (state) {
        wmh->flags |= XUrgencyHint;
    } else {
        wmh->flags &= ~XUrgencyHint;
    }

    XSetWMHints(g_display, client->window, wmh);
    XFree(wmh);
    // report changes to tags
    tag_set_flags_dirty();
}

// heavily inspired by dwm.c
void client_update_wm_hints(HSClient* client) {
    XWMHints* wmh = XGetWMHints(g_display, client->window);
    if (!wmh) {
        return;
    }

    HSClient* focused_client = frame_focused_client(g_cur_frame);
    if ((focused_client == client)
        && wmh->flags & XUrgencyHint) {
        // remove urgency hint if window is focused
        wmh->flags &= ~XUrgencyHint;
        XSetWMHints(g_display, client->window, wmh);
    } else {
        bool newval = (wmh->flags & XUrgencyHint) ? true : false;
        if (newval != client->urgent) {
            client->urgent = newval;
            char winid_str[STRING_BUF_SIZE];
            snprintf(winid_str, STRING_BUF_SIZE, "0x%lx", client->window);
            client_setup_border(client, focused_client == client);
            hook_emit_list("urgent", client->urgent ? "on":"off", winid_str, NULL);
            tag_set_flags_dirty();
        }
    }
    if (wmh->flags & InputHint) {
        client->neverfocus = !wmh->input;
    } else {
        client->neverfocus = false;
    }
    XFree(wmh);
}

void client_update_title(HSClient* client) {
    GString* new_name = window_property_to_g_string(g_display,
        client->window, g_netatom[NetWmName]);
    if (!new_name) {
        char* ch_new_name = NULL;
        /* if ewmh name isn't set, then fall back to WM_NAME */
        if (0 != XFetchName(g_display, client->window, &ch_new_name)) {
            new_name = g_string_new(ch_new_name);
            XFree(ch_new_name);
        } else {
            new_name = g_string_new("");
            HSDebug("no title for window %lx found, using \"\"\n",
                    client->window);
        }
    }
    bool changed = (0 != strcmp(client->title->str, new_name->str));
    g_string_free(client->title, true);
    client->title = new_name;
    if (changed && get_current_client() == client) {
        char buf[STRING_BUF_SIZE];
        snprintf(buf, STRING_BUF_SIZE, "0x%lx", client->window);
        hook_emit_list("window_title_changed", buf, client->title->str, NULL);
    }
}

HSClient* get_current_client() {
    return frame_focused_client(g_cur_frame);
}

void client_set_fullscreen(HSClient* client, bool state) {
    client->fullscreen = state;
    if (client->ewmhnotify) {
        client->ewmhfullscreen = state;
    }
    HSStack* stack = client->tag->stack;
    if (state) {
        stack_slice_add_layer(stack, client->slice, LAYER_FULLSCREEN);
    } else {
        stack_slice_remove_layer(stack, client->slice, LAYER_FULLSCREEN);
    }
    tag_update_focus_layer(client->tag);
    monitor_apply_layout(find_monitor_with_tag(client->tag));

    char buf[STRING_BUF_SIZE];
    snprintf(buf, STRING_BUF_SIZE, "0x%lx", client->window);
    ewmh_update_window_state(client);
    hook_emit_list("fullscreen", state ? "on" : "off", buf, NULL);
}

void client_set_pseudotile(HSClient* client, bool state) {
    client->pseudotile = state;
    monitor_apply_layout(find_monitor_with_tag(client->tag));
}

int client_set_property_command(int argc, char** argv) {
    char* action = (argc > 1) ? argv[1] : "toggle";

    HSClient* client = get_current_client();
    if (!client) {
        // nothing to do
        return 0;
    }

    struct {
        char* name;
        void (*func)(HSClient*, bool);
        bool* value;
    } properties[] = {
        { "fullscreen",   client_set_fullscreen, &client->fullscreen    },
        { "pseudotile",   client_set_pseudotile, &client->pseudotile    },
    };

    // find the property
    int i;
    for  (i = 0; i < LENGTH(properties); i++) {
        if (!strcmp(properties[i].name, argv[0])) {
            break;
        }
    }
    if (i >= LENGTH(properties)) {
        return HERBST_INVALID_ARGUMENT;
    }

    // if found, then change it
    bool old_value = *(properties[i].value);
    bool state = string_to_bool(action, *(properties[i].value));
    if (state != old_value) {
        properties[i].func(client, state);
    }
    return 0;
}

void window_update_border(Window window, unsigned long color) {
    if (*g_window_border_inner_width > 0
        && *g_window_border_inner_width < *g_window_border_width) {
        set_window_double_border(g_display, window,
                                 *g_window_border_inner_width,
                                 g_window_border_inner_color, color);
    } else {
        XSetWindowBorder(g_display, window, color);
    }
}

unsigned long get_window_border_color(HSClient* client) {
    unsigned long current_border_color = (client == frame_focused_client(g_cur_frame)
                                          ? g_window_border_active_color
                                          : g_window_border_normal_color);
    if (client->urgent)
        current_border_color = g_window_border_urgent_color;
    return current_border_color;
}

static bool is_client_urgent(void* key, HSClient* client, void* data) {
    (void) key;
    (void) data;
    return client->urgent;
}

HSClient* get_urgent_client() {
    return g_hash_table_find(g_clients, (GHRFunc)is_client_urgent, NULL);
}

/**
 * \brief   Resolve a window description to a client or a window id
 *
 * \param   str     Describes the window: "" means the focused one, "urgent"
 *                  resolves to a arbitrary urgent window, "0x..." just
 *                  resolves to the given window given its hexadecimal window id,
 *                  a decimal number its decimal window id.
 * \param   ret_client  The client pointer is stored there if ret_client is
 *                      given and the specified window is managed.
 * \return          The resolved window id is stored there if the according
 *                  window has been found
 */
Window string_to_client(char* str, HSClient** ret_client) {
    Window win = 0;
    if (!strcmp(str, "")) {
        HSClient* client = get_current_client();
        win = client ? client->window : 0;
        if (ret_client) {
            *ret_client = client;
        }
    } else if (!strcmp(str, "urgent")) {
        HSClient* client = get_urgent_client();
        if (client) {
            win = client->window;
        }
        if (ret_client) {
            *ret_client = client;
        }
    } else if (1 == sscanf(str, "0x%lx", (long unsigned int*)&win)) {
        if (ret_client) {
            *ret_client = get_client_from_window(win);
        }
    } else if (1 == sscanf(str, "%lu", (long unsigned int*)&win)) {
        if (ret_client) {
            *ret_client = get_client_from_window(win);
        }
    }
    return win;
}

// mainly from dwm.c
bool client_sendevent(HSClient *client, Atom proto) {
    int n;
    Atom *protocols;
    bool exists = false;
    XEvent ev;

    if (XGetWMProtocols(g_display, client->window, &protocols, &n)) {
        while (!exists && n--)
            exists = protocols[n] == proto;
        XFree(protocols);
    }
    if (exists) {
        ev.type = ClientMessage;
        ev.xclient.window = client->window;
        ev.xclient.message_type = g_wmatom[WMProtocols];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = proto;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(g_display, client->window, False, NoEventMask, &ev);
    }
    return exists;
}

