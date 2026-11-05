/* Compile extracted candidate PM scheduling/render code with controlled APIs.
 * This verifies state transitions; it is not a native timer-latency benchmark. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "reactor.h"
#include "raster.h"
typedef uintptr_t HAB, HMQ, HWND, MPARAM, MRESULT;
typedef uint32_t ULONG;
typedef char *PSZ;
typedef struct { const char *title; } WcPmConfig;
typedef struct { const char *(*call)(WcOperation,const int32_t *,uint32_t *); } WcPmEngine;
#define WM_TIMER 0x24
#define SHORT1FROMMP(x) ((uint16_t)(x))
#define HWND_DESKTOP ((HWND)1)
#define FALSE 0
#define MB_OK 1
#define MB_ERROR 2
#define MB_MOVEABLE 4
#include "app.inc"
static const WcPmConfig config={"test"};
static const WcPmConfig *app_config=&config;
static const WcPmEngine *app_engine;
static uint32_t now, delay, deadline, repaint_delay=INT32_MAX;
static int starts, stops, logs, fallback_logs, renders, pointer_checks;
static int showing=1, timer_error, engine_error, raster_error, dialog_active;
static int hook_start, hook_render, hook_paint, network_ready;
static unsigned long last_frame;
static uint8_t memory[32];
static void request_frame(void);
static void frame_wake(HWND);
static void dispatch(HWND,ULONG,MPARAM);

int __lsan_is_turned_off(void) { return 1; }
static void check(int ok,const char *name) {
    if (!ok) { fprintf(stderr,"FAIL %s now=%u starts=%d stops=%d renders=%d needs=%d armed=%d rendering=%d busy=%d failed=%d\n",name,now,starts,stops,renders,app.needs_frame,app.frame_timer_armed,app.rendering,app.busy,app.failed); exit(1); }
}
static uint32_t ticks(void) { return now; }
static int WinIsWindowShowing(HWND w) { (void)w; return showing; }
static int WinStartTimer(HAB a,HWND w,int id,ULONG ms) {
    (void)a; (void)w; check(id==FRAME_TIMER,"fast timer ID");
    ++starts; delay=ms; deadline=now+ms;
    if (hook_start) { hook_start=0; request_frame(); }
    return timer_error ? 0 : id;
}
static int WinStopTimer(HAB a,HWND w,int id) {
    (void)a;(void)w;if(id==FRAME_TIMER)++stops;return 1;
}
static void log_line(const char *stage,const char *detail) {
    (void)detail;++logs;if(!strcmp(stage,"Fast frame timer"))++fallback_logs;
}
static int wc_print_finish(int x) {return x;}
static int WinMessageBox(HWND a,HWND b,char *c,PSZ d,int e,int f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    dialog_active=1;dispatch(app.client,WM_TIMER,FRAME_TIMER);dispatch(app.client,WM_TIMER,POINTER_TIMER);dialog_active=0;return 1;
}
static void wasm_float_mode(void) {}
static const char *wc_rng_error(void) {return NULL;}
static int dimensions(HWND w,int32_t *v) {(void)w;v[0]=640;v[1]=480;return 1;}
static uint8_t *wc_pm_memory(size_t *n) {*n=sizeof(memory);return memory;}
int wc_raster_frame(WcRaster *r,uint32_t w,uint32_t h,const void *p,size_t n) {(void)r;(void)w;(void)h;(void)p;(void)n;return !raster_error;}
const char *wc_raster_error(const WcRaster *r) {(void)r;return "test raster error";}
void wc_raster_memory(const WcRaster *r,WcRasterMemory *m) {(void)r;memset(m,0,sizeof(*m));}
static int update_bitmap_geometry(void) {app.bitmap_width=640;app.bitmap_height=480;return 1;}
static int WinInvalidateRect(HWND w,void *p,int f) {(void)w;(void)p;(void)f;return 1;}
static int WinUpdateWindow(HWND w) {
    (void)w;if(hook_paint){hook_paint=0;request_frame();dispatch(app.client,WM_TIMER,POINTER_TIMER);}return 1;
}
static int wc_a11y_enabled(void) {return 0;}
static void wc_a11y_update(void *p,uint32_t n) {(void)p;(void)n;}
static void after_frame(HWND w) {(void)w;}
static void check_pointer(HWND w) {(void)w;++pointer_checks;}
static int wc_network_ready(void) {int ready=network_ready;network_ready=0;return ready;}

#include "cancel.inc"
#include "fail.inc"
#include "schedule.inc"
#include "request.inc"
#include "invoke.inc"
#include "contents.inc"
#include "render.inc"
#include "wake.inc"
#include "dispatch.inc"

static const char *engine_call(WcOperation op,const int32_t *v,uint32_t *out) {
    (void)v;*out=0;
    switch(op) {
    case WCR_FRAME:
        check(app.busy && app.rendering && !dialog_active,"engine owns render");
        ++renders;
        if (hook_render) { hook_render=0;request_frame();dispatch(app.client,WM_TIMER,POINTER_TIMER); }
        return engine_error ? "test engine error" : NULL;
    case WCR_DRAW_PTR:*out=1;break;
    case WCR_DRAW_LEN:*out=16;break;
    case WCR_REPAINT_DELAY:*out=repaint_delay;break;
    default:break;
    }
    return NULL;
}
static const WcPmEngine engine={engine_call};
static void fire(void) {now=deadline;dispatch(app.client,WM_TIMER,FRAME_TIMER);}
static void reset(void) {
    memset(&app,0,sizeof(app));app.client=101;app.ready=1;app.bitmap_width=640;app.bitmap_height=480;app.frames=3;app_engine=&engine;
    now=1000;delay=deadline=0;repaint_delay=INT32_MAX;starts=stops=logs=fallback_logs=renders=pointer_checks=0;
    showing=1;timer_error=engine_error=raster_error=dialog_active=hook_start=hook_render=hook_paint=network_ready=0;
}
int main(void) {
    int i;
    reset();for(i=0;i<128;++i)request_frame();check(starts==1 && delay==FRAME_WAKE_MS,"input burst coalesced");
    now=deadline-1;dispatch(app.client,WM_TIMER,FRAME_TIMER);check(!renders && app.frame_timer_armed,"stale timer cannot advance deadline");
    fire();check(renders==1 && stops==1 && !app.frame_timer_armed,"one frame then idle");
    for(i=0;i<1000;++i)dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==1 && starts==1 && pointer_checks==1000,"idle stays four-Hz timer only");
    reset();request_frame();now=1001;dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==1 && stops==1 && !app.frame_timer_armed,"early pointer frame cancels fast wake");
    reset();app.repaint_scheduled=1;app.repaint_at=now+1000;schedule_frame_wake();for(i=0;i<3;++i){now+=250;dispatch(app.client,WM_TIMER,POINTER_TIMER);}check(!renders && starts==0 && !stops && !app.frame_timer_armed && app.repaint_at==2000,"pointer polls retain future repaint deadline without fast timer");
    puts("PASS burst/stale/idle");

    reset();hook_start=1;request_frame();check(starts==1,"timer API reentrant input coalesced");fire();check(renders==1,"reentrant request renders once");
    reset();hook_render=1;request_frame();fire();check(renders==1 && app.frame_timer_armed && starts==2 && !app.busy && !app.rendering,"engine reentrancy deferred");fire();check(renders==2 && !app.frame_timer_armed,"deferred frame drains");
    reset();hook_paint=1;request_frame();fire();check(renders==1 && app.frame_timer_armed && !app.rendering,"paint reentrancy deferred");fire();check(renders==2,"post-paint redraw");puts("PASS reentrancy");

    reset();timer_error=1;request_frame();request_frame();check(!app.frame_timer_armed && app.needs_frame && fallback_logs==1,"timer failure retains request and logs once");dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==1 && !app.failed && !app.needs_frame,"250ms fallback renders");puts("PASS timer failure fallback");

    reset();app.repaint_scheduled=1;app.repaint_at=now+1000;schedule_frame_wake();check(!starts && !app.frame_timer_armed,"egui-only deadline stays on250ms timer");request_frame();check(delay==FRAME_WAKE_MS && starts==1 && !stops,"explicit input gets fast wake while repaint pending");fire();check(renders==1 && !app.frame_timer_armed,"input frame returns to idle poll");
    reset();repaint_delay=400;request_frame();fire();check(!app.frame_timer_armed && app.repaint_scheduled && app.repaint_at==now+400,"next egui deadline retained without fast timer");now+=250;dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==1 && starts==1 && !app.frame_timer_armed,"idle pointer tick before deadline does not render");repaint_delay=INT32_MAX;now+=250;dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==2 && !app.frame_timer_armed && !app.repaint_scheduled,"due repaint runs on original polling cadence");puts("PASS repaint deadlines");

    reset();app.frames=0;request_frame();for(i=0;i<3;++i)fire();check(app.frames==3 && renders==3 && starts==3 && !app.frame_timer_armed,"three startup passes preserved");puts("PASS startup passes");

    reset();request_frame();app.busy=1;fire();check(!renders && !app.frame_timer_armed,"busy timer consumed without spin");app.busy=0;dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==1,"busy fallback recovers");
    reset();app.ready=0;request_frame();check(!starts,"unready request stays dormant");dispatch(app.client,WM_TIMER,POINTER_TIMER);check(!renders,"unready timer inert");
    reset();app.failed=1;request_frame();check(!starts,"failed request stays dormant");dispatch(app.client,WM_TIMER,POINTER_TIMER);check(!renders,"failed timer inert");
    reset();request_frame();showing=0;fire();check(!renders && !app.frame_timer_armed,"occluded client has no fast spin");showing=1;dispatch(app.client,WM_TIMER,POINTER_TIMER);check(renders==1,"visible client resumes on fallback");puts("PASS busy/unready/failed/occluded");

    reset();request_frame();engine_error=1;fire();check(app.failed && !app.frame_timer_armed && !app.rendering && !app.busy,"engine failure cleans timer and render ownership");
    reset();request_frame();raster_error=1;fire();check(app.failed && !app.frame_timer_armed && !app.rendering,"raster failure cleans timer and render ownership");
    reset();request_frame();fail("test","outside-render failure");check(app.failed && !app.frame_timer_armed && stops==1 && !renders,"error dialog nested timers cannot render");
    reset();request_frame();app.ready=0;cancel_frame_wake();check(!app.frame_timer_armed && stops==1,"shutdown cancels fast timer");puts("PASS failure/shutdown cleanup");

    reset();now=UINT32_MAX-7;request_frame();check(deadline==8,"deadline wraps");now=0;dispatch(app.client,WM_TIMER,FRAME_TIMER);check(!renders && app.frame_timer_armed,"wrapped stale deadline ignored");fire();check(renders==1,"wrapped deadline fires");
    reset();request_frame();last_frame=app.frames;now=deadline;dispatch(app.client+1,WM_TIMER,FRAME_TIMER);check(app.frames==last_frame && app.frame_timer_armed,"wrong client timer ignored");
    reset();network_ready=1;dispatch(app.client,WM_TIMER,POINTER_TIMER);check(pointer_checks==1 && renders==1,"pointer/network fallback retained");puts("PASS wrap/client/network");
    return 0;
}
