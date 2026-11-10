#include "ui_menu_overlay.h"
#include "av_settings.h"
#include <stdio.h>

// A compact bottom card within the 320x180 video area (y=30..210).
enum { X=0, W=320, BOTTOM=210, HEADER=26, ROW=24, FOOTER=26,
       LIST_TOP=BOTTOM-HEADER-UI_MENU_OVERLAY_ROWS*ROW-FOOTER,
       QUALITY_TOP=BOTTOM-108,
       CONTROLS_TOP=BOTTOM-HEADER-2*ROW-FOOTER };
#define PANEL 0x1082u
#define FOCUS 0x2249u
#define ACCENT 0x5DF6u

static bool stale(const ui_menu_overlay_t *c,const ui_menu_t *m) {
    unsigned selected=m->view==UI_VIEW_CONTROLS?m->controls_selected:m->selected;
    return !c->valid || c->view!=m->view || c->selected!=selected ||
        c->scroll!=m->scroll || c->quality!=m->quality ||
        c->quality_selected!=m->quality_selected || c->revision!=m->revision ||
        c->quality_enabled!=m->quality_enabled || c->count!=m->count;
}
static void prepare(ui_menu_overlay_t *c,const ui_menu_t *m) {
    c->valid=true;c->view=m->view;
    c->selected=m->view==UI_VIEW_CONTROLS?m->controls_selected:m->selected;c->scroll=m->scroll;
    c->quality=m->quality;c->quality_selected=m->quality_selected;
    c->revision=m->revision;c->quality_enabled=m->quality_enabled;c->count=m->count;
    c->builds++;
    char label[64];
    if (m->view==UI_VIEW_QUALITY) {
        ui_text_prepare(&c->text[0],"Quality",240);
        static const char *rates[]={"8 fps","15 fps","30 fps"};
        for (unsigned i=0;i<AV_QUALITY_COUNT;i++) {
            ui_text_prepare(&c->text[1+i*2],av_quality_name(i),76);
            ui_text_prepare(&c->text[2+i*2],rates[i],76);
        }
    } else if (m->view==UI_VIEW_CONTROLS) {
        ui_text_prepare(&c->text[0],"TV",100);
        ui_text_prepare(&c->text[2],"Channels",160);
        ui_text_prepare(&c->text[3],"Quality",160);
        snprintf(label,sizeof label,"%u",m->count);
        ui_text_prepare(&c->text[4],label,64);
        ui_text_prepare(&c->text[5],av_quality_name(m->quality),64);
    } else {
        ui_text_prepare(&c->text[0],"Channels",100);
        snprintf(label,sizeof(label),"%s",av_quality_name(m->quality));
        ui_text_prepare(&c->text[1],m->quality_enabled?label:"Live",100);
        for (unsigned i=0;i<UI_MENU_OVERLAY_ROWS;i++) {
            unsigned index=m->scroll+i;
            label[0]=0;
            if (index<ui_menu_item_count(m)) {
                snprintf(label,sizeof(label),"%02u  %s",index+1,
                    m->entries[index].name[0]?m->entries[index].name:m->entries[index].id);
            }
            ui_text_prepare(&c->text[2+i],label,252);
        }
        ui_text_prepare(&c->text[5],av_quality_name(m->quality),64);
    }
    ui_text_prepare(&c->text[7],"Up/Down  OK select",180);
    ui_text_prepare(&c->text[8],"Hold: back",88);
}
static void card(ui_surface_t s,int x,int y,int w,int h,uint16_t color) {
    // Four-pixel stepped corners, with every fill clipped to this stripe.
    ui_text_fill(s,x+4,y,w-8,1,color);
    ui_text_fill(s,x+2,y+1,w-4,2,color);
    ui_text_fill(s,x+1,y+3,w-2,1,color);
    ui_text_fill(s,x,y+4,w,h-8,color);
    ui_text_fill(s,x+1,y+h-4,w-2,1,color);
    ui_text_fill(s,x+2,y+h-3,w-4,2,color);
    ui_text_fill(s,x+4,y+h-1,w-8,1,color);
}
void ui_menu_overlay_begin(ui_menu_overlay_t *c,const ui_menu_t *m) {
    if (m->view!=UI_VIEW_MENU && m->view!=UI_VIEW_QUALITY && m->view!=UI_VIEW_CONTROLS) {
        c->view=m->view;c->valid=false;return;
    }
    if (stale(c,m)) prepare(c,m);
}
bool ui_menu_overlay_covers(const ui_menu_overlay_t *c,unsigned y,unsigned rows) {
    if (!c->valid || !rows || (c->view!=UI_VIEW_MENU && c->view!=UI_VIEW_QUALITY && c->view!=UI_VIEW_CONTROLS)) return false;
    unsigned top=c->view==UI_VIEW_MENU?LIST_TOP:c->view==UI_VIEW_CONTROLS?CONTROLS_TOP:QUALITY_TOP;
    // Only the full-width rectangular interior is opaque. Corner stripes and
    // all visible video still take the normal checked decoder path.
    return y>=top+4u && y<BOTTOM-4u && rows<=BOTTOM-4u-y;
}
void ui_menu_overlay_paint(ui_menu_overlay_t *c,const ui_menu_t *m,ui_surface_t s) {
    ui_menu_overlay_begin(c,m);
    ui_menu_overlay_draw(c,s);
}
void ui_menu_overlay_draw(const ui_menu_overlay_t *c,ui_surface_t s) {
    if (!c->valid) return;
    ui_view_t view=c->view;
    if (view!=UI_VIEW_MENU && view!=UI_VIEW_QUALITY && view!=UI_VIEW_CONTROLS) return;
    int top=view==UI_VIEW_MENU?LIST_TOP:view==UI_VIEW_CONTROLS?CONTROLS_TOP:QUALITY_TOP;
    if (s.origin_y+s.rows<=top || s.origin_y>=BOTTOM) return;
    card(s,X,top,W,BOTTOM-top,PANEL);
    ui_text_paint(s,X+12,top+6,&c->text[0],UI_COLOR_TEXT);
    if (view==UI_VIEW_MENU) {
        ui_text_paint(s,X+W-12-c->text[1].width,top+6,&c->text[1],ACCENT);
        for (unsigned i=0;i<UI_MENU_OVERLAY_ROWS;i++) {
            unsigned index=c->scroll+i;
            if (index>=c->count) break;
            int y=top+HEADER+(int)i*ROW;
            bool chosen=index==c->selected;
            if (chosen) {
                card(s,X+6,y,W-12,ROW,FOCUS);
                ui_text_fill(s,X+10,y+7,2,10,ACCENT);
            }
            ui_text_paint(s,X+18,y+4,&c->text[2+i],chosen?UI_COLOR_TEXT:UI_COLOR_DIM);
        }
    } else if (view==UI_VIEW_CONTROLS) {
        for (unsigned i=0;i<2;i++) {
            int y=top+HEADER+(int)i*ROW;
            bool chosen=i==c->selected;
            if (chosen) {
                card(s,X+6,y,W-12,ROW,FOCUS);
                ui_text_fill(s,X+10,y+7,2,10,ACCENT);
            }
            ui_text_paint(s,X+18,y+4,&c->text[2+i],chosen?UI_COLOR_TEXT:UI_COLOR_DIM);
            ui_text_paint(s,X+W-18-c->text[4+i].width,y+4,&c->text[4+i],ACCENT);
        }
    } else {
        for (unsigned i=0;i<AV_QUALITY_COUNT;i++) {
            int x=X+(W-292)/2+(int)i*100,y=top+HEADER+2;
            bool chosen=i==c->quality_selected;
            card(s,x,y,92,48,chosen?FOCUS:UI_COLOR_BACKDROP);
            ui_text_paint(s,x+(92-c->text[1+i*2].width)/2,y+5,&c->text[1+i*2],UI_COLOR_TEXT);
            ui_text_paint(s,x+(92-c->text[2+i*2].width)/2,y+25,&c->text[2+i*2],chosen?ACCENT:UI_COLOR_DIM);
            if (i==c->quality) ui_text_fill(s,x+34,y+44,24,2,ACCENT);
        }
    }
    ui_text_fill(s,X+12,BOTTOM-FOOTER,W-24,1,UI_COLOR_RULE);
    ui_text_paint(s,X+12,BOTTOM-FOOTER+6,&c->text[7],UI_COLOR_DIM);
    ui_text_paint(s,X+W-12-c->text[8].width,BOTTOM-FOOTER+6,&c->text[8],UI_COLOR_DIM);
}
