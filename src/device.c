#include "device.h"

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

pj_status_t pjsocky_device_set_audio(int capture_id, int playback_id)
{
    return pjsua_set_snd_dev(capture_id, playback_id);
}

/* v1 handles one active call at a time (see CONTEXT.md), so one
 * selected video capture device is enough. */
static pjmedia_vid_dev_index g_video_capture_id = PJMEDIA_VID_INVALID_DEV;

void pjsocky_device_set_video_capture(pjmedia_vid_dev_index capture_id)
{
    g_video_capture_id = capture_id;
}

pjmedia_vid_dev_index pjsocky_device_get_video_capture(void)
{
    return g_video_capture_id;
}
