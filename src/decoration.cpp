
#include "decoration.h"
#include "clientlist.h"
#include "globals.h"
#include "settings.h"
#include "ewmh.h"

#include <stdio.h>
#include <string.h>
#include <X11/extensions/shape.h>

// public globals:
HSDecTriple g_decorations[HSDecSchemeCount];

// module intern globals:
static GHashTable* g_decwin2client = NULL;

static int* g_pseudotile_center_threshold;
static int* g_update_dragged_clients;
static HSObject* g_theme_object;
static HSObject g_theme_active_object;
static HSObject g_theme_normal_object;
static HSObject g_theme_urgent_object;
// dummy schemes for propagation
static HSDecorationScheme g_theme_scheme;
static HSDecorationScheme g_theme_active_scheme;
static HSDecorationScheme g_theme_normal_scheme;
static HSDecorationScheme g_theme_urgent_scheme;
static void init_dec_triple_object(HSDecTriple* t, const char* name);
static void init_scheme_object(HSObject* obj, HSDecorationScheme* s, HSAttrCallback cb);
static void init_scheme_attributes(HSObject* obj, HSDecorationScheme* s, HSAttrCallback cb);
static GString* PROP2MEMBERS(HSAttribute* attr);

// is called automatically after resize_outline
static void decoration_update_frame_extents(struct HSClient* client);

void decorations_init() {
    g_theme_object = hsobject_create_and_link(hsobject_root(), "theme");
    g_pseudotile_center_threshold = &(settings_find("pseudotile_center_threshold")->value.i);
    g_update_dragged_clients = &(settings_find("update_dragged_clients")->value.i);
    g_decwin2client = g_hash_table_new(g_int_hash, g_int_equal);
    // init default schemes
    // tiling //
    HSDecTriple tiling = {
        { 2, getcolor("black"),     false },    // normal
        { 2, getcolor("green"),     false },    // active
        { 2, getcolor("orange"),    false },    // urgent
    };
    g_decorations[HSDecSchemeTiling] = tiling;
    // fullscreen //
    HSDecTriple fs = {
        { 0, getcolor("black"),     false },    // normal
        { 0, getcolor("black"),     false },    // active
        { 0, getcolor("black"),     false },    // urgent
    };
    g_decorations[HSDecSchemeFullscreen] = fs;
    // floating //
    HSDecTriple fl = {
        { 1, getcolor("black"),     true  },    // normal
        { 4, getcolor("green"),     true  },    // active
        { 1, getcolor("orange"),    true  },    // urgent
    };
    g_decorations[HSDecSchemeFloating] = fl;
    // minimal //
    HSDecTriple minimal = {
        { 0, getcolor("black"),     true  },    // normal
        { 0, getcolor("green"),     true  },    // active
        { 0, getcolor("orange"),    true  },    // urgent
    };
    g_decorations[HSDecSchemeMinimal] = minimal;
    init_dec_triple_object(g_decorations + HSDecSchemeTiling, "tiling");
    init_dec_triple_object(g_decorations + HSDecSchemeFloating, "floating");
    init_dec_triple_object(g_decorations + HSDecSchemeMinimal, "minimal");
    // create mass-attribute-objects
    g_theme_scheme
        = g_theme_active_scheme
        = g_theme_normal_scheme
        = g_theme_urgent_scheme = fs.normal;
    init_scheme_object(&g_theme_active_object, &g_theme_active_scheme, PROP2MEMBERS);
    init_scheme_object(&g_theme_normal_object, &g_theme_normal_scheme, PROP2MEMBERS);
    init_scheme_object(&g_theme_urgent_object, &g_theme_urgent_scheme, PROP2MEMBERS);
    hsobject_set_attributes_always_callback(&g_theme_active_object);
    hsobject_set_attributes_always_callback(&g_theme_normal_object);
    hsobject_set_attributes_always_callback(&g_theme_urgent_object);
    init_scheme_attributes(g_theme_object, &g_theme_scheme, PROP2MEMBERS);
    hsobject_set_attributes_always_callback(g_theme_object);
    hsobject_link(g_theme_object, &g_theme_active_object, "active");
    hsobject_link(g_theme_object, &g_theme_normal_object, "normal");
    hsobject_link(g_theme_object, &g_theme_urgent_object, "urgent");
}

static GString* RELAYOUT(HSAttribute* attr) {
    (void) attr;
    all_monitors_apply_layout();
    return NULL;
}

static GString* PROP2MEMBERS(HSAttribute* attr) {
    monitors_lock();
    // find out which object it was
    // then copy it to the appropriate floating scheme
    GString* output = g_string_new("");
    HSObject* members[4] = { NULL };
    size_t    member_cnt = 0;

    if (attr->object == &g_theme_active_object) {
        members[member_cnt++] = &(g_decorations[HSDecSchemeTiling]  .obj_active);
        members[member_cnt++] = &(g_decorations[HSDecSchemeFloating].obj_active);
    } else if (attr->object == &g_theme_normal_object) {
        members[member_cnt++] = &(g_decorations[HSDecSchemeTiling]  .obj_normal);
        members[member_cnt++] = &(g_decorations[HSDecSchemeFloating].obj_normal);
    } else if (attr->object == &g_theme_urgent_object) {
        members[member_cnt++] = &(g_decorations[HSDecSchemeTiling]  .obj_urgent);
        members[member_cnt++] = &(g_decorations[HSDecSchemeFloating].obj_urgent);
    } else if (attr->object == g_theme_object) {
        members[member_cnt++] = &g_theme_active_object;
        members[member_cnt++] = &g_theme_normal_object;
        members[member_cnt++] = &g_theme_urgent_object;
    }
    if (member_cnt > 0) {
        GString* val = hsattribute_to_string(attr);
        // set the idx'th attribute of all members of that group to the same value
        for (int i = 0; i < member_cnt; i++) {
            HSAttribute* oth_a = hsobject_find_attribute(members[i], attr->name);
            if (!oth_a) {
                HSDebug("%d: Can not find attribute %s. This should not happen!\n", i, attr->name);
                continue;
            }
            hsattribute_assign(oth_a, val->str, output);
        }
        g_string_free(val, true);
    }
    monitors_unlock();
    g_string_free(output, true);
    return NULL;
}

GString* PROPAGATE(HSAttribute* attr) {
    HSDecTriple* t = (HSDecTriple*)attr->object->data;
    monitors_lock();
    // find out which attribute it was
    int idx = attr - attr->object->attributes;
    // then copy it to active and urgent scheme
    GString* output = g_string_new("");
    GString* val = hsattribute_to_string(attr);
    hsattribute_assign(t->obj_active.attributes + idx, val->str, output);
    hsattribute_assign(t->obj_normal.attributes + idx, val->str, output);
    hsattribute_assign(t->obj_urgent.attributes + idx, val->str, output);
    monitors_unlock();
    g_string_free(output, true);
    g_string_free(val, true);
    return NULL;
}

void reset_helper(void* data, GString* output) {
    (void) data;
    g_string_append(output,
                    "Writing this resets all attributes to a default value\n");
}

static GString* trigger_attribute_reset(class HSAttribute* attr, const char* new_value) {
    (void) attr;
    (void) new_value;
    HSObject* obj = attr->object;
    HSAttribute* attrs = obj->attributes;
    size_t cnt = obj->attribute_count;
    GString* out = g_string_new("");
    monitors_lock();
    for (int i = 0; i < cnt; i ++) {
        HSAttribute* a = attrs+i;
        if (a->type == HSATTR_TYPE_INT
            || a->type == HSATTR_TYPE_UINT) {
            hsattribute_assign(a, "0", out);
        }
        else if (a->type == HSATTR_TYPE_COLOR) {
            hsattribute_assign(a, "black", out);
        }
    }
    if (out->len <= 0) {
        g_string_free(out, true);
        out = NULL;
    }
    monitors_unlock();
    return out;
}

// initializes the specified object to edit the scheme
static void init_scheme_object(HSObject* obj, HSDecorationScheme* s, HSAttrCallback cb) {
    hsobject_init(obj);
    init_scheme_attributes(obj,s,cb);
}

static void init_scheme_attributes(HSObject* obj, HSDecorationScheme* s, HSAttrCallback cb) {
    HSAttribute attributes[] = {
        ATTRIBUTE_INT(      "border_width",     s->border_width,    cb),
        ATTRIBUTE_INT(      "padding_top",      s->padding_top,     cb),
        ATTRIBUTE_INT(      "padding_right",    s->padding_right,   cb),
        ATTRIBUTE_INT(      "padding_bottom",   s->padding_bottom,  cb),
        ATTRIBUTE_INT(      "padding_left",     s->padding_left,    cb),
        ATTRIBUTE_COLOR(    "color",            s->border_color,    cb),
        ATTRIBUTE_INT(      "inner_width",      s->inner_width,     cb),
        ATTRIBUTE_COLOR(    "inner_color",      s->inner_color,     cb),
        ATTRIBUTE_INT(      "outer_width",      s->outer_width,     cb),
        ATTRIBUTE_COLOR(    "outer_color",      s->outer_color,     cb),
        ATTRIBUTE_COLOR(    "background_color", s->background_color,cb),
        ATTRIBUTE_CUSTOM(   "reset",            reset_helper,       trigger_attribute_reset),
        ATTRIBUTE_LAST,
    };
    hsobject_set_attributes(obj, attributes);
}

static void init_dec_triple_object(HSDecTriple* t, const char* name) {
    hsobject_init(&t->object);
    init_scheme_object(&t->obj_normal, &t->normal, RELAYOUT);
    init_scheme_object(&t->obj_active, &t->active, RELAYOUT);
    init_scheme_object(&t->obj_urgent, &t->urgent, RELAYOUT);
    hsobject_link(&t->object, &t->obj_normal, "normal");
    hsobject_link(&t->object, &t->obj_active, "active");
    hsobject_link(&t->object, &t->obj_urgent, "urgent");
    memset(&t->propagate, 0, sizeof(t->propagate));
    init_scheme_attributes(&t->object, &t->propagate, PROPAGATE);
    hsobject_set_attributes_always_callback(&t->object);
    t->object.data = t;
    hsobject_link(g_theme_object, &t->object, name);
}

static void free_dec_triple_object(HSDecTriple* t) {
    hsobject_unlink(g_theme_object, &t->object);
    hsobject_free(&t->object);
    hsobject_free(&t->obj_normal);
    hsobject_free(&t->obj_active);
    hsobject_free(&t->obj_urgent);
}

void decorations_destroy() {
    free_dec_triple_object(g_decorations + HSDecSchemeTiling);
    free_dec_triple_object(g_decorations + HSDecSchemeFloating);
    hsobject_unlink(g_theme_object, &g_theme_normal_object);
    hsobject_unlink(g_theme_object, &g_theme_active_object);
    hsobject_unlink(g_theme_object, &g_theme_urgent_object);
    hsobject_free(&g_theme_normal_object);
    hsobject_free(&g_theme_active_object);
    hsobject_free(&g_theme_urgent_object);
    hsobject_unlink_and_destroy(hsobject_root(), g_theme_object);
    g_hash_table_destroy(g_decwin2client);
    g_decwin2client = NULL;
}

// from openbox/frame.c
static Visual* check_32bit_client(HSClient* c)
{
    XWindowAttributes wattrib;
    Status ret;

    ret = XGetWindowAttributes(g_display, c->window, &wattrib);
    HSWeakAssert(ret != BadDrawable);
    HSWeakAssert(ret != BadWindow);

    if (wattrib.depth == 32)
        return wattrib.visual;
    return NULL;
}

void decoration_init(HSDecoration* dec, struct HSClient* client) {
    memset(dec, 0, sizeof(*dec));
    dec->client = client;
}

void decoration_setup_frame(HSClient* client) {
    HSDecoration* dec = &(client->dec);
    XSetWindowAttributes at;
    long mask = 0;
    // copy attributes from client and not from the root window
    Visual* visual = check_32bit_client(client);
    if (visual) {
        /* client has a 32-bit visual */
        mask = CWColormap | CWBackPixel | CWBorderPixel;
        /* create a colormap with the visual */
        dec->colormap = at.colormap =
            XCreateColormap(g_display, g_root, visual, AllocNone);
        at.background_pixel = BlackPixel(g_display, g_screen);
        at.border_pixel = BlackPixel(g_display, g_screen);
    } else {
        dec->colormap = 0;
    }
    dec->depth = visual
                 ? 32
                 : (DefaultDepth(g_display, DefaultScreen(g_display)));
    dec->decwin = XCreateWindow(g_display, g_root, 0,0, 30, 30, 0,
                        dec->depth,
                        InputOutput,
                        visual
                            ? visual
                            : DefaultVisual(g_display, DefaultScreen(g_display)),
                        mask, &at);
    mask = 0;
    if (visual) {
        /* client has a 32-bit visual */
        mask = CWColormap | CWBackPixel | CWBorderPixel;
        // TODO: why does DefaultColormap work in openbox but crashes hlwm here?
        // It somehow must be incompatible to the visual and thus causes the
        // BadMatch on XCreateWindow
        at.colormap = dec->colormap;
        at.background_pixel = BlackPixel(g_display, g_screen);
        at.border_pixel = BlackPixel(g_display, g_screen);
    }
    dec->bgwin = 0;
    dec->bgwin = XCreateWindow(g_display, dec->decwin, 0,0, 30, 30, 0,
                        dec->depth,
                        InputOutput,
                        CopyFromParent,
                        mask, &at);
    XMapWindow(g_display, dec->bgwin);
    // use a clients requested initial floating size as the initial size
    dec->last_rect_inner = true;
    dec->last_inner_rect = client->float_size;
    dec->last_outer_rect = inner_rect_to_outline(client->float_size, dec->last_scheme);
    dec->last_actual_rect = dec->last_inner_rect;
    dec->last_actual_rect.x -= dec->last_outer_rect.x;
    dec->last_actual_rect.y -= dec->last_outer_rect.y;
    dec->pixmap = 0;
    g_hash_table_insert(g_decwin2client, &(dec->decwin), client);
    // set wm_class for window
    XClassHint *hint = XAllocClassHint();
    hint->res_name = (char*)HERBST_DECORATION_CLASS;
    hint->res_class = (char*)HERBST_DECORATION_CLASS;
    XSetClassHint(g_display, dec->decwin, hint);
    XFree(hint);
}

void decoration_free(HSDecoration* dec) {
    if (g_decwin2client) {
        g_hash_table_remove(g_decwin2client, &(dec->decwin));
    }
    if (dec->colormap) {
        XFreeColormap(g_display, dec->colormap);
    }
    if (dec->pixmap) {
        XFreePixmap(g_display, dec->pixmap);
    }
    if (dec->bgwin) {
        XDestroyWindow(g_display, dec->bgwin);
    }
    if (dec->decwin) {
        XDestroyWindow(g_display, dec->decwin);
    }
}

HSClient* get_client_from_decoration(Window decwin) {
    return (HSClient*) g_hash_table_lookup(g_decwin2client, &decwin);
}

Rectangle outline_to_inner_rect(Rectangle rect, HSDecorationScheme s) {
    return Rectangle(
        rect.x + s.border_width + s.padding_left,
        rect.y + s.border_width + s.padding_top,
        rect.width  - 2* s.border_width - s.padding_left - s.padding_right,
        rect.height - 2* s.border_width - s.padding_top - s.padding_bottom
    );
}

Rectangle inner_rect_to_outline(Rectangle rect, HSDecorationScheme s) {
    return Rectangle(
        rect.x - s.border_width - s.padding_left,
        rect.y - s.border_width - s.padding_top,
        rect.width  + 2* s.border_width + s.padding_left + s.padding_right,
        rect.height + 2* s.border_width + s.padding_top + s.padding_bottom
    );
}

void decoration_resize_inner(HSClient* client, Rectangle inner,
                             HSDecorationScheme scheme) {
    decoration_resize_outline(client, inner_rect_to_outline(inner, scheme), scheme);
    client->dec.last_rect_inner = true;
}

void decoration_resize_outline(HSClient* client, Rectangle outline,
                               HSDecorationScheme scheme)
{
    Rectangle inner = outline_to_inner_rect(outline, scheme);
    // get relative coordinates
    Window decwin = client->dec.decwin;
    Window win = client->window;

    Rectangle tile = inner;
    applysizehints(client, &inner.width, &inner.height);
    if (!scheme.tight_decoration) {
        // center the window in the outline tile
        // but only if it's relative coordinates would not be too close to the
        // upper left tile border
        int threshold = *g_pseudotile_center_threshold;
        int dx = tile.width/2 - inner.width/2;
        int dy = tile.height/2 - inner.height/2;
        inner.x = tile.x + ((dx < threshold) ? 0 : dx);
        inner.y = tile.y + ((dy < threshold) ? 0 : dy);
    }

    //if (RECTANGLE_EQUALS(client->last_size, rect)
    //    && client->last_border_width == border_width) {
    //    return;
    //}

    if (scheme.tight_decoration) {
        outline = inner_rect_to_outline(inner, scheme);
    }
    client->dec.last_inner_rect = inner;
    inner.x -= outline.x;
    inner.y -= outline.y;
    XWindowChanges changes;
    changes.x = inner.x;
    changes.y = inner.y;
    changes.width = inner.width;
    changes.height = inner.height;
    changes.border_width = 0;

    int mask = CWX | CWY | CWWidth | CWHeight | CWBorderWidth;
    //if (*g_window_border_inner_width > 0
    //    && *g_window_border_inner_width < *g_window_border_width) {
    //    unsigned long current_border_color = get_window_border_color(client);
    //    HSDebug("client_resize %s\n",
    //            current_border_color == g_window_border_active_color
    //            ? "ACTIVE" : "NORMAL");
    //    set_window_double_border(g_display, win, *g_window_border_inner_width,
    //                             g_window_border_inner_color,
    //                             current_border_color);
    //}
    // send new size to client
    // update structs
    bool size_changed = outline.width != client->dec.last_outer_rect.width
                     || outline.height != client->dec.last_outer_rect.height;
    client->dec.last_outer_rect = outline;
    client->dec.last_rect_inner = false;
    client->last_size = inner;
    client->dec.last_scheme = scheme;
    // redraw
    // TODO: reduce flickering
    if (!client->dragged || *g_update_dragged_clients) {
        client->dec.last_actual_rect.x = changes.x;
        client->dec.last_actual_rect.y = changes.y;
        client->dec.last_actual_rect.width = changes.width;
        client->dec.last_actual_rect.height = changes.height;
    }
    decoration_redraw_pixmap(client);
    XSetWindowBackgroundPixmap(g_display, decwin, client->dec.pixmap);
    if (!size_changed) {
        // if size changes, then the window is cleared automatically
        XClearWindow(g_display, decwin);
    }
    if (!client->dragged || *g_update_dragged_clients) {
        XConfigureWindow(g_display, win, mask, &changes);
        XMoveResizeWindow(g_display, client->dec.bgwin,
                          changes.x, changes.y,
                          changes.width, changes.height);
    }
    XMoveResizeWindow(g_display, decwin,
                      outline.x, outline.y, outline.width, outline.height);
    decoration_update_frame_extents(client);
    if (!client->dragged || *g_update_dragged_clients) {
        client_send_configure(client);
    }
    XSync(g_display, False);
}

static void decoration_update_frame_extents(struct HSClient* client) {
    int left = client->dec.last_inner_rect.x - client->dec.last_outer_rect.x;
    int top  = client->dec.last_inner_rect.y - client->dec.last_outer_rect.y;
    int right = client->dec.last_outer_rect.width - client->dec.last_inner_rect.width - left;
    int bottom = client->dec.last_outer_rect.height - client->dec.last_inner_rect.height - top;
    ewmh_update_frame_extents(client->window, left,right, top,bottom);
}

void decoration_change_scheme(struct HSClient* client,
                              HSDecorationScheme scheme) {
    if (client->dec.last_inner_rect.width < 0) {
        // TODO: do something useful here
        return;
    }
    if (client->dec.last_rect_inner) {
        decoration_resize_inner(client, client->dec.last_inner_rect, scheme);
    } else {
        decoration_resize_outline(client, client->dec.last_outer_rect, scheme);
    }
}

static unsigned int get_client_color(HSClient* client, unsigned int pixel) {
    if (client->dec.colormap) {
        XColor xcol;
        xcol.pixel = pixel;
        /* get rbg value out of default colormap */
        XQueryColor(g_display, DefaultColormap(g_display, g_screen), &xcol);
        /* get pixel value back appropriate for client */
        XAllocColor(g_display, client->dec.colormap, &xcol);
        return xcol.pixel;
    } else {
        return pixel;
    }
}

// draw a decoration to the client->dec.pixmap
void decoration_redraw_pixmap(struct HSClient* client) {
    HSDecorationScheme s = client->dec.last_scheme;
    HSDecoration *const dec = &client->dec;
    Window win = client->dec.decwin;
    Rectangle outer = client->dec.last_outer_rect;
    unsigned int depth = client->dec.depth;

    int *rad = &(settings_find("window_radius")->value.i);

    // TODO: maybe do something like pixmap recreate threshhold?
    bool recreate_pixmap = (dec->pixmap == 0) || (dec->pixmap_width != outer.width)
                                              || (dec->pixmap_height != outer.height);
    if (recreate_pixmap) {
        if (dec->pixmap) {
            XFreePixmap(g_display, dec->pixmap);
        }
        dec->pixmap = XCreatePixmap(g_display, win, outer.width, outer.height, depth);
    }
    Pixmap pix = dec->pixmap;
    GC gc = XCreateGC(g_display, pix, 0, NULL);

    // draw background
    XSetForeground(g_display, gc, get_client_color(client, s.border_color));
    XFillRectangle(g_display, pix, gc, 0, 0, outer.width, outer.height);

    // Draw inner border
    int iw = s.inner_width;
    Rectangle inner = client->dec.last_inner_rect;
    inner.x -= client->dec.last_outer_rect.x;
    inner.y -= client->dec.last_outer_rect.y;
    if (iw > 0) {
        /* fill rectangles because drawing does not work */
        XRectangle rects[] = {
            { inner.x - iw, inner.y - iw, inner.width + 2*iw, iw }, /* top */
            { inner.x - iw, inner.y, iw, inner.height },  /* left */
            { inner.x + inner.width, inner.y, iw, inner.height }, /* right */
            { inner.x - iw, inner.y + inner.height, inner.width + 2*iw, iw }, /* bottom */
        };
        XSetForeground(g_display, gc, get_client_color(client, s.inner_color));
        XFillRectangles(g_display, pix, gc, rects, LENGTH(rects));
    }

    // Draw outer border
    int ow = s.outer_width;
    outer.x -= client->dec.last_outer_rect.x;
    outer.y -= client->dec.last_outer_rect.y;
    if (ow > 0) {
        ow = MIN(ow, (outer.height+1) / 2);
        XRectangle rects[] = {
            { 0, 0, outer.width, ow }, /* top */
            { 0, ow, ow, outer.height - 2*ow }, /* left */
            { outer.width-ow, ow, ow, outer.height - 2*ow }, /* right */
            { 0, outer.height - ow, outer.width, ow }, /* bottom */
        };
        XSetForeground(g_display, gc, get_client_color(client, s.outer_color));
        XFillRectangles(g_display, pix, gc, rects, LENGTH(rects));
    }
    // fill inner rect that is not covered by the client
    XSetForeground(g_display, gc, get_client_color(client, s.background_color));
    if (dec->last_actual_rect.width < inner.width) {
        XFillRectangle(g_display, pix, gc,
                       dec->last_actual_rect.x + dec->last_actual_rect.width,
                       dec->last_actual_rect.y,
                       inner.width - dec->last_actual_rect.width,
                       dec->last_actual_rect.height);
    }
    if (dec->last_actual_rect.height < inner.height) {
        XFillRectangle(g_display, pix, gc,
                       dec->last_actual_rect.x,
                       dec->last_actual_rect.y + dec->last_actual_rect.height,
                       inner.width,
                       inner.height - dec->last_actual_rect.height);
    }

    // MASK
    int bw = s.border_width;

    int dia = *rad * 2;

    // TODO: BORDER DISPLAY IS NOT OFFSET PROPERLY

    int x = outer.x;
    int y = outer.y;
    int w = outer.width;
    int h = outer.height;

    int barcs[][6] = {
        { x,       y,       dia, dia, 0, 360 << 6 },
        { x,       y+h-dia, dia, dia, 0, 360 << 6 },
        { x+w-dia, y,       dia, dia, 0, 360 << 6 },
        { x+w-dia, y+h-dia, dia, dia, 0, 360 << 6 },
    };

    XRectangle brects[] = {
        { *rad, 0, w-dia, h },
        { 0, *rad, w, h-dia }
    };

    *rad -= bw; dia = *rad * 2;

    int carcs[][6] = {
        { x+bw,    y+bw,    dia, dia, 0, 360 << 6 },
        { x+bw,    y+h-dia, dia, dia, 0, 360 << 6 },
        { x+w-dia, y+bw,    dia, dia, 0, 360 << 6 },
        { x+w-dia, y+h-dia, dia, dia, 0, 360 << 6 },
    };

    XRectangle crects[] = {
        { bw+*rad, bw,     w-dia-bw, h        },
        { bw,     bw+*rad, w,        h-dia-bw },
    };

    // BEGIN MASKING

    Pixmap bpid = XCreatePixmap(g_display, win, outer.width+2*bw, outer.height+2*bw, 1);
    Pixmap cpid = XCreatePixmap(g_display, win, outer.width,      outer.height,      1);

    GC bgc = XCreateGC(g_display, bpid, 0, NULL);
    GC cgc = XCreateGC(g_display, cpid, 0, NULL);

    XRectangle bounding = { -bw, -bw, w+2*bw, h+2*bw };
    XSetForeground(g_display, bgc, 0);
    XFillRectangle(g_display, bpid, bgc, bounding.x, bounding.y, bounding.width, bounding.height);

    XSetForeground(g_display, bgc, 1);
    XFillRectangles(g_display, bpid, bgc, brects, LENGTH(brects));
    for (int i=0; i < sizeof(barcs)/sizeof(barcs[0]); ++i) {
        XFillArc(g_display, bpid, bgc,
            barcs[i][0], barcs[i][1],
            barcs[i][2], barcs[i][3],
            barcs[i][4], barcs[i][5]);
    }

    XRectangle clipping = { 0, 0, w, h };
    XSetForeground(g_display, cgc, 0);
    XFillRectangle(g_display, cpid, cgc, clipping.x, clipping.y, clipping.width, clipping.height);

    XSetForeground(g_display, cgc, 1);
    XFillRectangles(g_display, cpid, cgc, crects, LENGTH(crects));
    for (int i=0; i < sizeof(carcs)/sizeof(carcs[0]); ++i) {
        XFillArc(g_display, cpid, cgc,
            carcs[i][0], carcs[i][1],
            carcs[i][2], carcs[i][3],
            carcs[i][4], carcs[i][5]);
    }

    // Mask
    XShapeCombineMask(g_display, win, ShapeBounding, -bw, -bw, bpid, ShapeSet);
    XShapeCombineMask(g_display, win, ShapeClip,     0,   0,   cpid, ShapeSet);
    XShapeCombineMask(g_display, client->window, ShapeClip,     0,   0,   cpid, ShapeSet);

    XSetForeground(g_display, gc, get_client_color(client, s.border_color));
    XFillRectangle(g_display, win, gc, 0, 0, outer.width, outer.height);

    // clean up masks
    XFreePixmap(g_display, bpid);
    XFreePixmap(g_display, cpid);

    // clean up
    XFreeGC(g_display, bgc);
    XFreeGC(g_display, cgc);
    XFreeGC(g_display, gc);
}

