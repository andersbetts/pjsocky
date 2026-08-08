#include "device.h"

#include "account.h"

#include <pj/errno.h>
#include <pj/log.h>

#define THIS_FILE "device.c"

/* No account/call-style validation needed here - pjsua_enum_aud_devs()
 * etc. are safe to call in any daemon state. */

pj_status_t pjsocky_device_list_audio(pjmedia_aud_dev_info devices[],
                                       unsigned *p_count)
{
    return pjsua_enum_aud_devs(devices, p_count);
}

pj_status_t pjsocky_device_list_video(pjmedia_vid_dev_info devices[],
                                       unsigned *p_count)
{
    return pjsua_vid_enum_devs(devices, p_count);
}

/*
 * Select the devices call audio will use, without opening them.
 *
 * pjsua_set_snd_dev() opens the hardware there and then, and pjsua only
 * arms its auto-close timer when a call tears down - so a selection made
 * while idle left ALSA capture and playback streams running with nothing
 * connected to them in the conference bridge. Observed on a TP4: the
 * controlling application selects devices at registration time, no call
 * ever follows, and pjmedia's delay buffers underflow at ~50 Hz forever,
 * each one a log line. That is 93% of the device's log volume, evicting
 * everything else from a RAM-backed journal within minutes, plus a
 * capture stream held open against a camera mic that is not being used.
 *
 * PJSUA_SND_DEV_NO_IMMEDIATE_OPEN records the selection and leaves the
 * hardware alone; the conference bridge opens it when a call's media
 * actually connects (pjsua_aud.c, conf connect path), and the existing
 * snd_auto_close_time closes it a second after the call ends. So the
 * device is open exactly while there is audio to carry.
 *
 * The flag only defers when the device is currently closed - pjsua takes
 * the early-return in pjsua_set_snd_dev2() on !snd_is_on. If a selection
 * arrives while the device is open, that is either mid-call (where
 * switching immediately is the wanted behaviour) or a leftover open
 * device with no call, which is the very state this exists to end. Hence
 * the explicit close in the second case, via the only public API that
 * does it - and set_snd_dev2() clears the "no sound" flag that leaves
 * behind, so the next call still opens real hardware rather than
 * silently getting null audio.
 */
pj_status_t pjsocky_device_set_audio(int capture_id, int playback_id)
{
    pjsua_snd_dev_param param;

    if (pjsua_snd_is_active() && pjsua_call_get_count() == 0) {
        PJ_LOG(4, (THIS_FILE, "set_audio: closing idle sound device before "
                   "selecting capture=%d playback=%d", capture_id, playback_id));
        pjsua_set_no_snd_dev();
    }

    pjsua_snd_dev_param_default(&param);
    param.capture_dev = capture_id;
    param.playback_dev = playback_id;
    param.mode = PJSUA_SND_DEV_NO_IMMEDIATE_OPEN;

    return pjsua_set_snd_dev2(&param);
}

/* v1 handles one active call at a time (see CONTEXT.md), so one
 * selected video capture device is enough. */
static pjmedia_vid_dev_index g_video_capture_id = PJMEDIA_VID_INVALID_DEV;

void pjsocky_device_set_video_capture(pjmedia_vid_dev_index capture_id)
{
    g_video_capture_id = capture_id;

    /* Live-update the already-configured account's default capture device
     * too, not just call.c's apply_video_capture_device() (which only takes
     * effect once a call's video stream already exists). lift_emg_tel calls
     * account.configure before device.set_video, so by the time a call is
     * dialed the account already exists -- and pjsua-lib resolves the
     * INITIAL capture device from the account's own vid_cap_dev at call
     * setup time, before apply_video_capture_device() ever runs. Without
     * this, that initial setup still uses whatever vid_cap_dev the account
     * was created with, ignoring this selection until the next call. */
    pjsua_acc_id acc_id = pjsocky_account_get_id();
    PJ_LOG(3, (THIS_FILE, "set_video_capture: capture_id=%d acc_id=%d",
               capture_id, acc_id));
    if (acc_id != PJSUA_INVALID_ID) {
        pj_pool_t *pool = pjsua_pool_create("pjsocky_set_video_capture", 512, 512);
        if (pool != NULL) {
            pjsua_acc_config acc_cfg;
            pj_status_t get_status = pjsua_acc_get_config(acc_id, pool, &acc_cfg);
            if (get_status == PJ_SUCCESS) {
                PJ_LOG(3, (THIS_FILE, "set_video_capture: acc %d vid_cap_dev before=%d",
                           acc_id, acc_cfg.vid_cap_dev));
                acc_cfg.vid_cap_dev = capture_id;
                pj_status_t mod_status = pjsua_acc_modify(acc_id, &acc_cfg);
                if (mod_status != PJ_SUCCESS) {
                    PJ_PERROR(1, (THIS_FILE, mod_status,
                                  "set_video_capture: pjsua_acc_modify failed"));
                } else {
                    pjsua_acc_config verify_cfg;
                    if (pjsua_acc_get_config(acc_id, pool, &verify_cfg) == PJ_SUCCESS) {
                        PJ_LOG(3, (THIS_FILE,
                                   "set_video_capture: acc %d vid_cap_dev after=%d",
                                   acc_id, verify_cfg.vid_cap_dev));
                    }
                }
            } else {
                PJ_PERROR(1, (THIS_FILE, get_status,
                              "set_video_capture: pjsua_acc_get_config failed"));
            }
            pj_pool_release(pool);
        }
    }
}

pjmedia_vid_dev_index pjsocky_device_get_video_capture(void)
{
    return g_video_capture_id;
}
