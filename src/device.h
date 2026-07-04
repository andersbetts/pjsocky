/*
 * Wraps pjsua-lib's audio/video device enumeration and selection API -
 * see docs/PROTOCOL.md ("device.list_audio", "device.list_video",
 * "device.set_audio", "device.set_video").
 *
 * Deliberately has no dependency on JSON or the wire protocol - the
 * proto/dispatch.c command handlers translate between the two. Note the
 * lifetime warning in proto/jsonutil.h: an array of device info structs
 * passed to these functions has to keep living until the response JSON
 * tree built from it is fully serialized, not just until the command
 * handler returns - allocate it from the request's pool, not the stack.
 */
#ifndef PJSOCKY_DEVICE_H
#define PJSOCKY_DEVICE_H

#include <pjsua-lib/pjsua.h>

PJ_BEGIN_DECL

/*
 * Plenty for any realistic device inventory. Callers allocate
 * an array of this capacity.
 */
#define PJSOCKY_MAX_DEVICES   32

/*
 * On input, *p_count is the capacity of `devices` (PJSOCKY_MAX_DEVICES).
 * On output, the actual number of devices found.
 */
pj_status_t pjsocky_device_list_audio(pjmedia_aud_dev_info devices[],
                                       unsigned *p_count);
pj_status_t pjsocky_device_list_video(pjmedia_vid_dev_info devices[],
                                       unsigned *p_count);

/*
 * Wraps pjsua_set_snd_dev(). Takes effect on the currently open sound
 * device / the next call, per docs/PROTOCOL.md "device.set_audio".
 */
pj_status_t pjsocky_device_set_audio(int capture_id, int playback_id);

/*
 * Selects the capture device future calls with video should use.
 * Doesn't open the device itself - call.c (not written yet, build-order
 * step 8+) is what will actually wire this into call media setup. See
 * docs/PROTOCOL.md "device.set_video".
 */
void pjsocky_device_set_video_capture(pjmedia_vid_dev_index capture_id);
pjmedia_vid_dev_index pjsocky_device_get_video_capture(void);

PJ_END_DECL

#endif /* PJSOCKY_DEVICE_H */
