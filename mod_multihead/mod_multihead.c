/*
 * Ion multihead module
 * based on :
 * - mod_xrandr by Ragnar Rova and Tuomo Valkonen
 * - mod_xinerama by Thomas Themel
 *
 * by Sylvain Fargier <fargier.sylvain@free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License,or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not,write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <string.h>

#include <ioncore/common.h>
#include <ioncore/eventh.h>
#include <ioncore/global.h>
#include <ioncore/event.h>
#include <ioncore/mplex.h>
#include <ioncore/resize.h>
#include <ioncore/group-ws.h>
#include <ioncore/sizepolicy.h>
#include <ioncore/../version.h>
#include <ioncore/names.h>

/* #define MOD_MULTIHEAD_DEBUG */
#ifdef MOD_MULTIHEAD_DEBUG
#include <stdio.h>
#define mh_dbg(fmt, args...) warn(fmt, ##args)
#else
#define mh_dbg(fmt, args...) do {} while (0)
#endif

char mod_multihead_ion_api_version[]=ION_API_VERSION;

static struct {
    int xrandr_evt_base;
    bool has_xinerama;
    bool has_xrandr;
} multihead;

/**
 * @brief close disconnected displays.
 */
static void xinerama_close_disconn(XineramaScreenInfo* sInfo, int nInfo)
{
    WScreen *scr, *nxt;
    WMPlexIterTmp mpl_it;
    WRegion *sub;
    int i;

    FOR_ALL_SCREENS_W_NEXT(scr, nxt)
    {
        if (screen_id(scr) == 0)
            continue;

        for (i = 0; i < nInfo; ++i)
            if (sInfo[i].screen_number == screen_id(scr))
                break;
        if (i == nInfo)
        {
            mh_dbg("closing screen %i", i);

            FOR_ALL_MANAGED_BY_MPLEX(&scr->mplex, sub, mpl_it)
            {
                if (OBJ_IS(sub, WGroupWS) &&
                        strncmp("*scratchws*", region_name(sub), 11) != 0)
                {
                    mh_dbg("region moved to main screen: %s", region_name(sub));
                    WMPlexAttachParams param = MPLEXATTACHPARAMS_INIT;
                    WRegionAttachData data;

                    param.flags = MPLEX_ATTACH_GEOM | MPLEX_ATTACH_SIZEPOLICY;
                    param.geom = REGION_GEOM(&ioncore_g.rootwins->scr);
                    param.szplcy = SIZEPOLICY_FULL_EXACT;
                    data.type = REGION_ATTACH_REPARENT;
                    data.u.reg = sub;

                    mplex_managed_remove(&scr->mplex, sub);
                    mplex_do_attach(&ioncore_g.rootwins->scr.mplex, &param,
                            &data);
                }
                else
                {
                    mh_dbg("region definitively lost: %s", region_name(sub));
                    region_rqclose_propagate((WRegion *)scr, sub);
                }
            }
            destroy_obj((Obj *)scr);
        }
    }
}

/**
 * @brief manage Xinerama screens.
 * @details add/remove WScreen objects when necessary, move them arround.
 */
static bool handle_xinerama_display()
{
    XineramaScreenInfo* sInfo;
    WFitParams root_fp;

    int nRects;
    int i;
    sInfo = XineramaQueryScreens(ioncore_g.dpy, &nRects);

    if(!sInfo)
    {
        warn(TR("Could not retrieve Xinerama screen info, sorry."));
        return FALSE ;
    }

    xinerama_close_disconn(sInfo, nRects);

    root_fp.mode = REGION_FIT_EXACT;
    root_fp.g.x = root_fp.g.y = 0;
    root_fp.g.w = root_fp.g.h = 0;

    for (i = 0; i < nRects; ++i)
    {
        if (sInfo[i].x_org + sInfo[i].width > root_fp.g.w)
            root_fp.g.w = sInfo[i].x_org + sInfo[i].width;
        if (sInfo[i].y_org + sInfo[i].height > root_fp.g.h)
            root_fp.g.h = sInfo[i].y_org + sInfo[i].height;

    }
    REGION_GEOM(&ioncore_g.rootwins->scr) = root_fp.g;
    mplex_managed_geom(&ioncore_g.rootwins->scr.mplex, &(root_fp.g));
    mplex_do_fit_managed(&ioncore_g.rootwins->scr.mplex, &(root_fp));

    for (i = 0; i < nRects; ++i)
    {
        WScreen *scr = ioncore_find_screen_id(sInfo[i].screen_number);
        WFitParams fp;

        mh_dbg("Rectangle #%d: id=%d x=%d y=%d width=%u height=%u\n",
                i+1, sInfo[i].screen_number, sInfo[i].x_org, sInfo[i].y_org,
                sInfo[i].width, sInfo[i].height);

        fp.g.x = sInfo[i].x_org;
        fp.g.y = sInfo[i].y_org;
        fp.g.w = sInfo[i].width;
        fp.g.h = sInfo[i].height;
        fp.mode = REGION_FIT_EXACT;
        fp.gravity = ForgetGravity;

        if (scr != NULL)
        {
            if (scr->id == 0)
            {
                /* This mplex is our rootwindow, only it's content are resized */
                screen_set_managed_offset(scr, &fp.g);
            }
            else
            {
                if (rectangle_compare(&REGION_GEOM(scr), &fp.g) != RECTANGLE_SAME)
                {
                    if (!region_fitrep((WRegion *) scr, NULL, &fp))
                        warn("Failed to fit WScreen(%i)", screen_id(scr));
                }
            }
        }
        else
        {
            WMPlexAttachParams par = MPLEXATTACHPARAMS_INIT;
            par.flags = MPLEX_ATTACH_GEOM | MPLEX_ATTACH_SIZEPOLICY |
                MPLEX_ATTACH_UNNUMBERED | MPLEX_ATTACH_PSEUDOMODAL ;
            par.geom = fp.g;
            par.szplcy = SIZEPOLICY_FULL_EXACT;

            scr = (WScreen*) mplex_do_attach_new(&ioncore_g.rootwins->scr.mplex,
                    &par, (WRegionCreateFn *) create_screen, NULL);

            if(scr == NULL) {
                warn(TR("Unable to create Xinerama workspace %d."), i);
                XFree(sInfo);
                return FALSE;
            }

            scr->id = sInfo[i].screen_number;
            if (screen_init_layout(scr, extl_table_none()) == FALSE)
                warn("Failed to initialize layout on screen %d.",
                        sInfo[i].screen_number);
            if (!region_fitrep((WRegion *) scr, NULL, &fp))
                warn("Failed to fit WScreen(%i)", screen_id(scr));
        }
    }

    XFree(sInfo);
    return TRUE;
}

static bool xrandr_event(XEvent *ev)
{
    if (ev && ev->type == multihead.xrandr_evt_base + RRScreenChangeNotify)
    {
        if (multihead.has_xinerama)
            return handle_xinerama_display();
        else
        {
            XRRScreenChangeNotifyEvent *rev = (XRRScreenChangeNotifyEvent *) ev;
            WFitParams fp;

            fp.g.x = fp.g.y = 0;
            fp.g.w = rev->width;
            fp.g.h = rev->height;

            REGION_GEOM(&ioncore_g.rootwins->scr) = fp.g;
            mplex_managed_geom(&ioncore_g.rootwins->scr.mplex, &(fp.g));
            mplex_do_fit_managed(&ioncore_g.rootwins->scr.mplex, &fp);
        }
        return TRUE;
    }
    return FALSE;
}

bool mod_multihead_init()
{
    WScreen* new;
    int evt_base, error_base;

    multihead.has_xinerama = XineramaQueryExtension(ioncore_g.dpy, &evt_base,
            &error_base);
    multihead.has_xrandr = XRRQueryExtension(ioncore_g.dpy,
            &multihead.xrandr_evt_base, &error_base);

    if (multihead.has_xinerama)
        handle_xinerama_display();

    if (multihead.has_xrandr)
    {
        XRRSelectInput(ioncore_g.dpy, ioncore_g.rootwins->dummy_win,
                RRScreenChangeNotifyMask);
    }

    hook_add(ioncore_handle_event_alt, (WHookDummy *) xrandr_event);

    return TRUE;
}


bool mod_multihead_deinit()
{
    hook_remove(ioncore_handle_event_alt,
            (WHookDummy *) xrandr_event);

    return TRUE;
}

