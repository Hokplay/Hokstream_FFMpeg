/*
 * XCB input grabber
 * Copyright (C) 2014 Luca Barbato <lu_zero@gentoo.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that
 * will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

 #include "config.h"

 #include <stdio.h> // For snprintf
 #include <stdlib.h>
 #include <string.h>
 #include <xcb/xcb.h>

 #if CONFIG_LIBXCB_XFIXES
 #include <xcb/xfixes.h>
 #endif

 #if CONFIG_LIBXCB_SHM
 #include <sys/shm.h>
 #include <xcb/shm.h>
 #endif

 #if CONFIG_LIBXCB_SHAPE
 #include <xcb/shape.h>
 #endif
 #if CONFIG_LIBNPP
 #include <cuda_runtime.h>
 #include <nppi_color_conversion.h>
 #endif

 #include "libavutil/internal.h"
 #include "libavutil/log.h" // For av_log_once
 #include "libavutil/mathematics.h"
 #include "libavutil/mem.h"
 #include "libavutil/opt.h"
 #include "libavutil/parseutils.h"
 #include "libavutil/time.h"
 #include "libavutil/pixdesc.h" // For av_get_pix_fmt_name

 #include "libavformat/avformat.h"
 #include "libavformat/demux.h"
 #include "libavformat/internal.h"

 typedef struct XCBGrabContext {
     const AVClass *class;

     xcb_connection_t *conn;
     xcb_screen_t *screen;
     xcb_window_t window;
 #if CONFIG_LIBXCB_SHM
     AVBufferPool *shm_pool;
 #endif
     int64_t time_frame;
     AVRational time_base;
     int64_t frame_duration;

     xcb_window_t window_id;
     int x, y;
     int width, height;
     int frame_size; // Size of the frame grabbed from X (e.g. BGR0)
     int bpp;        // Bits per pixel of the frame grabbed from X

     int draw_mouse;
     int follow_mouse;
     int show_region;
     int region_border;
     int centered;
     int select_region;

     const char *framerate;

     int has_shm;
 #if CONFIG_LIBNPP
     int perform_npp_conversion; // Flag to indicate if NPP conversion should be done
 #endif
 } XCBGrabContext;

 #define FOLLOW_CENTER -1

 #define OFFSET(x) offsetof(XCBGrabContext, x)
 #define D AV_OPT_FLAG_DECODING_PARAM
 static const AVOption options[] = {
     { "window_id", "Window to capture.", OFFSET(window_id), AV_OPT_TYPE_INT, { .i64 = XCB_NONE }, 0, UINT32_MAX, D },
     { "x", "Initial x coordinate.", OFFSET(x), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, D },
     { "y", "Initial y coordinate.", OFFSET(y), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, D },
     { "grab_x", "Initial x coordinate.", OFFSET(x), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, D },
     { "grab_y", "Initial y coordinate.", OFFSET(y), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, D },
     { "video_size", "A string describing frame size, such as 640x480 or hd720.", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL }, 0, 0, D },
     { "framerate", "", OFFSET(framerate), AV_OPT_TYPE_STRING, {.str = "ntsc" }, 0, 0, D },
     { "draw_mouse", "Draw the mouse pointer.", OFFSET(draw_mouse), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, D },
     { "follow_mouse", "Move the grabbing region when the mouse pointer reaches within specified amount of pixels to the edge of region.",
       OFFSET(follow_mouse), AV_OPT_TYPE_INT, { .i64 = 0 },  FOLLOW_CENTER, INT_MAX, D, .unit = "follow_mouse" },
     { "centered", "Keep the mouse pointer at the center of grabbing region when following.", 0, AV_OPT_TYPE_CONST, { .i64 = -1 }, INT_MIN, INT_MAX, D, .unit = "follow_mouse" },
     { "show_region", "Show the grabbing region.", OFFSET(show_region), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, D },
     { "region_border", "Set the region border thickness.", OFFSET(region_border), AV_OPT_TYPE_INT, { .i64 = 3 }, 1, 128, D },
     { "select_region", "Select the grabbing region graphically using the pointer.", OFFSET(select_region), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, D },
     { NULL },
 };

 static const AVClass xcbgrab_class = {
     .class_name = "xcbgrab indev",
     .item_name  = av_default_item_name,
     .option     = options,
     .version    = LIBAVUTIL_VERSION_INT,
     .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT,
 };

 static int xcbgrab_reposition(AVFormatContext *s,
                               xcb_query_pointer_reply_t *p,
                               xcb_get_geometry_reply_t *geo)
 {
     XCBGrabContext *c = s->priv_data;
     int x = c->x, y = c->y;
     int w = c->width, h = c->height, f = c->follow_mouse;
     int p_x, p_y;

     if (!p || !geo)
         return AVERROR(EIO);

     p_x = p->win_x;
     p_y = p->win_y;

     if (f == FOLLOW_CENTER) {
         x = p_x - w / 2;
         y = p_y - h / 2;
     } else {
         int left   = x + f;
         int right  = x + w - f;
         int top    = y + f;
         int bottom = y + h - f;
         if (p_x > right) {
             x += p_x - right;
         } else if (p_x < left) {
             x -= left - p_x;
         }
         if (p_y > bottom) {
             y += p_y - bottom;
         } else if (p_y < top) {
             y -= top - p_y;
         }
     }

     c->x = FFMIN(FFMAX(0, x), geo->width  - w);
     c->y = FFMIN(FFMAX(0, y), geo->height - h);

     return 0;
 }

 static void xcbgrab_image_reply_free(void *opaque, uint8_t *data)
 {
     free(opaque);
 }

 static int xcbgrab_frame(AVFormatContext *s, AVPacket *pkt)
 {
     XCBGrabContext *c = s->priv_data;
     xcb_get_image_cookie_t iq;
     xcb_get_image_reply_t *img;
     xcb_drawable_t drawable = c->window_id;
     xcb_generic_error_t *e = NULL;
     uint8_t *data;
     int length;

     iq  = xcb_get_image(c->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                         c->x, c->y, c->width, c->height, ~0);

     img = xcb_get_image_reply(c->conn, iq, &e);

     if (e) {
         av_log(s, AV_LOG_ERROR,
                "Cannot get the image data "
                "event_error: response_type:%u error_code:%u "
                "sequence:%u resource_id:%u minor_code:%u major_code:%u.\n",
                e->response_type, e->error_code,
                e->sequence, e->resource_id, e->minor_code, e->major_code);
         free(e);
         return AVERROR(EACCES);
     }

     if (!img)
         return AVERROR(EAGAIN);

     data   = xcb_get_image_data(img);
     length = xcb_get_image_data_length(img);

     pkt->buf = av_buffer_create(data, length, xcbgrab_image_reply_free, img, 0);
     if (!pkt->buf) {
         free(img);
         return AVERROR(ENOMEM);
     }

     pkt->data = data;
     pkt->size = length;

     return 0;
 }

 static int64_t wait_frame(AVFormatContext *s, AVPacket *pkt)
 {
     XCBGrabContext *c = s->priv_data;
     int64_t curtime, delay;

     c->time_frame += c->frame_duration;

     for (;;) {
         curtime = av_gettime_relative();
         delay   = c->time_frame - curtime;
         if (delay <= 0)
             break;
         av_usleep(delay);
     }

     return curtime;
 }

 #if CONFIG_LIBXCB_SHM
 static int check_shm(xcb_connection_t *conn)
 {
     xcb_shm_query_version_cookie_t cookie = xcb_shm_query_version(conn);
     xcb_shm_query_version_reply_t *reply;

     reply = xcb_shm_query_version_reply(conn, cookie, NULL);
     if (reply) {
         free(reply);
         return 1;
     }

     return 0;
 }

 static void free_shm_buffer(void *opaque, uint8_t *data)
 {
     shmdt(data);
 }

 static AVBufferRef *allocate_shm_buffer(void *opaque, size_t size)
 {
     xcb_connection_t *conn = opaque;
     xcb_shm_seg_t segment;
     AVBufferRef *ref;
     uint8_t *data;
     int id;

     id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777);
     if (id == -1)
         return NULL;

     segment = xcb_generate_id(conn);
     xcb_shm_attach(conn, segment, id, 0);
     data = shmat(id, NULL, 0);
     shmctl(id, IPC_RMID, 0);
     if ((intptr_t)data == -1 || !data)
         return NULL;

     ref = av_buffer_create(data, size, free_shm_buffer, (void *)(ptrdiff_t)segment, 0);
     if (!ref)
         shmdt(data);

     return ref;
 }

 static int xcbgrab_frame_shm(AVFormatContext *s, AVPacket *pkt)
 {
     XCBGrabContext *c = s->priv_data;
     xcb_shm_get_image_cookie_t iq;
     xcb_shm_get_image_reply_t *img;
     xcb_drawable_t drawable = c->window_id;
     xcb_generic_error_t *e = NULL;
     AVBufferRef *buf;
     xcb_shm_seg_t segment;

     buf = av_buffer_pool_get(c->shm_pool);
     if (!buf) {
         av_log(s, AV_LOG_ERROR, "Could not get shared memory buffer.\n");
         return AVERROR(ENOMEM);
     }
     segment = (xcb_shm_seg_t)(uintptr_t)av_buffer_pool_buffer_get_opaque(buf);

     iq = xcb_shm_get_image(c->conn, drawable,
                            c->x, c->y, c->width, c->height, ~0,
                            XCB_IMAGE_FORMAT_Z_PIXMAP, segment, 0);
     img = xcb_shm_get_image_reply(c->conn, iq, &e);

     xcb_flush(c->conn);

     if (e) {
         av_log(s, AV_LOG_ERROR,
                "Cannot get the image data "
                "event_error: response_type:%u error_code:%u "
                "sequence:%u resource_id:%u minor_code:%u major_code:%u.\n",
                e->response_type, e->error_code,
                e->sequence, e->resource_id, e->minor_code, e->major_code);

         free(e);
         av_buffer_unref(&buf);
         return AVERROR(EACCES);
     }

     free(img);

     pkt->buf = buf;
     pkt->data = buf->data;
     pkt->size = c->frame_size;

     return 0;
 }
 #endif /* CONFIG_LIBXCB_SHM */

 #if CONFIG_LIBXCB_XFIXES
 static int check_xfixes(xcb_connection_t *conn)
 {
     xcb_xfixes_query_version_cookie_t cookie;
     xcb_xfixes_query_version_reply_t *reply;

     cookie = xcb_xfixes_query_version(conn, XCB_XFIXES_MAJOR_VERSION,
                                       XCB_XFIXES_MINOR_VERSION);
     reply  = xcb_xfixes_query_version_reply(conn, cookie, NULL);

     if (reply) {
         free(reply);
         return 1;
     }
     return 0;
 }

 #define BLEND(target, source, alpha) \
     (target) + ((source) * (255 - (alpha)) + 255 / 2) / 255

 static void xcbgrab_draw_mouse(AVFormatContext *s, AVPacket *pkt,
                                xcb_query_pointer_reply_t *p,
                                xcb_get_geometry_reply_t *geo,
                                int win_x, int win_y)
 {
     XCBGrabContext *gr = s->priv_data;
     uint32_t *cursor;
     uint8_t *image = pkt->data;
     int stride     = gr->bpp / 8; // Uses bpp of the X server frame
     xcb_xfixes_get_cursor_image_cookie_t cc;
     xcb_xfixes_get_cursor_image_reply_t *ci;
     int cx, cy, x, y, w, h, c_off, i_off;

     cc = xcb_xfixes_get_cursor_image(gr->conn);
     ci = xcb_xfixes_get_cursor_image_reply(gr->conn, cc, NULL);
     if (!ci)
         return;

     cursor = xcb_xfixes_get_cursor_image_cursor_image(ci);
     if (!cursor) {
         free(ci);
         return;
     }

     cx = ci->x - ci->xhot;
     cy = ci->y - ci->yhot;

     x = FFMAX(cx, win_x + gr->x);
     y = FFMAX(cy, win_y + gr->y);

     w = FFMIN(cx + ci->width,  win_x + gr->x + gr->width)  - x;
     h = FFMIN(cy + ci->height, win_y + gr->y + gr->height) - y;

     c_off = x - cx;
     i_off = x - gr->x - win_x;

     cursor += (y - cy) * ci->width;
     image  += (y - gr->y - win_y) * gr->width * stride;

     for (y = 0; y < h; y++) {
         cursor += c_off;
         image  += i_off * stride;
         for (x = 0; x < w; x++, cursor++, image += stride) {
             int r, g, b, a;

             r =  *cursor        & 0xff;
             g = (*cursor >>  8) & 0xff;
             b = (*cursor >> 16) & 0xff;
             a = (*cursor >> 24) & 0xff;

             if (!a)
                 continue;

             if (a == 255) {
                 image[0] = r;
                 image[1] = g;
                 image[2] = b;
             } else {
                 image[0] = BLEND(r, image[0], a);
                 image[1] = BLEND(g, image[1], a);
                 image[2] = BLEND(b, image[2], a);
             }
         }
         cursor +=  ci->width - w - c_off;
         image  += (gr->width - w - i_off) * stride;
     }

     free(ci);
 }
 #endif /* CONFIG_LIBXCB_XFIXES */

 static void xcbgrab_update_region(AVFormatContext *s, int win_x, int win_y)
 {
     XCBGrabContext *c     = s->priv_data;
     const uint32_t args[] = { win_x + c->x - c->region_border,
                               win_y + c->y - c->region_border };

     xcb_configure_window(c->conn,
                          c->window,
                          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                          args);
 }

 #if CONFIG_LIBNPP
 /* Forward declaration for bgr0_to_yuv420p_gpu */
 static int bgr0_to_yuv420p_gpu(uint8_t       *dstY,
                                uint8_t       *dstU,
                                uint8_t       *dstV,
                                const uint8_t *src,
                                int            width,
                                int            height);
 #endif

 static int xcbgrab_read_packet(AVFormatContext *s, AVPacket *pkt)
 {
     XCBGrabContext *c = s->priv_data;
     xcb_query_pointer_cookie_t pc = {0};
     xcb_get_geometry_cookie_t gc = {0};
     xcb_translate_coordinates_cookie_t tc;
     xcb_query_pointer_reply_t *p  = NULL;
     xcb_get_geometry_reply_t *geo = NULL;
     xcb_translate_coordinates_reply_t *translate = NULL;
     int ret = 0;
     int64_t pts;
     int win_x = 0, win_y = 0;

     wait_frame(s, pkt);

     if (c->follow_mouse || c->draw_mouse) {
         pc  = xcb_query_pointer(c->conn, c->window_id != XCB_NONE ? c->window_id : c->screen->root);
         gc  = xcb_get_geometry(c->conn, c->window_id != XCB_NONE ? c->window_id : c->screen->root);
         p   = xcb_query_pointer_reply(c->conn, pc, NULL);
         if (!p) {
             av_log(s, AV_LOG_ERROR, "Failed to query xcb pointer\n");
             ret = AVERROR_EXTERNAL;
             goto end;
         }
         geo = xcb_get_geometry_reply(c->conn, gc, NULL);
         if (!geo) {
             av_log(s, AV_LOG_ERROR, "Failed to get xcb geometry\n");
             ret = AVERROR_EXTERNAL;
             goto end;
         }
     }

     if (c->window_id != XCB_NONE && c->window_id != c->screen->root) {
         tc = xcb_translate_coordinates(c->conn, c->window_id, c->screen->root, 0, 0);
         translate = xcb_translate_coordinates_reply(c->conn, tc, NULL);
         if (!translate) {
             av_log(s, AV_LOG_ERROR, "Failed to translate xcb coordinates\n");
             ret = AVERROR_EXTERNAL;
             goto end;
         }
         win_x = translate->dst_x;
         win_y = translate->dst_y;
         free(translate);
         translate = NULL;
     }

     if (c->follow_mouse && p && p->same_screen)
         xcbgrab_reposition(s, p, geo);

     if (c->show_region)
         xcbgrab_update_region(s, win_x, win_y);

 #if CONFIG_LIBXCB_SHM
     if (c->has_shm) {
         ret = xcbgrab_frame_shm(s, pkt);
         if (ret < 0) {
             av_log(s, AV_LOG_WARNING, "SHM frame capture failed, falling back to non-SHM. Error: %s\n", av_err2str(ret));
             c->has_shm = 0;
         }
     }
 #endif
     if (!c->has_shm || ret < 0) {
         ret = xcbgrab_frame(s, pkt);
     }

     if (ret < 0) {
         goto end;
     }

    pkt->pts = c->time_frame - c->frame_duration;
    pkt->dts = pkt->pts; // For raw video, DTS is usually same as PTS
    pkt->duration = c->frame_duration;

 #if CONFIG_LIBXCB_XFIXES
     if (ret >= 0 && c->draw_mouse && p && p->same_screen) {
         xcbgrab_draw_mouse(s, pkt, p, geo, win_x, win_y);
     }
 #endif

 #if CONFIG_LIBNPP
     if (ret >= 0 && c->perform_npp_conversion) {
         const int y_size  = c->width * c->height;
         const int uv_size = y_size >> 2;

         AVPacket yuv_pkt;
         // av_init_packet(&yuv_pkt); // DEPRECATED - REMOVED

         if (av_new_packet(&yuv_pkt, y_size + uv_size * 2) == 0) {
             uint8_t *dstY = yuv_pkt.data;
             uint8_t *dstU = dstY + y_size;
             uint8_t *dstV = dstU + uv_size;

             if (!bgr0_to_yuv420p_gpu(dstY, dstU, dstV,
                                      pkt->data,
                                      c->width, c->height)) {
                                        yuv_pkt.pts = pkt->pts;
                                        yuv_pkt.dts = pkt->dts;
                                        yuv_pkt.duration = pkt->duration;
                                        // av_packet_copy_props(&yuv_pkt, pkt); // This copies more than just timestamps
                        
                                        av_packet_unref(pkt);
                                        *pkt = yuv_pkt;
             } else {
                 av_log(s, AV_LOG_ERROR, "NPP BGR0 to YUV420P conversion failed for a frame.\n");
                 av_packet_unref(&yuv_pkt);
                 av_packet_unref(pkt);
                 ret = AVERROR_EXTERNAL;
             }
         } else {
             av_log(s, AV_LOG_ERROR, "Failed to allocate packet for NPP YUV420P conversion.\n");
             av_packet_unref(pkt);
             ret = AVERROR(ENOMEM);
         }
     }
 #endif

end:
     free(p);
     free(geo);
     free(translate);

     return ret;
 }

 static av_cold int xcbgrab_read_close(AVFormatContext *s)
 {
     XCBGrabContext *ctx = s->priv_data;

 #if CONFIG_LIBXCB_SHM
     av_buffer_pool_uninit(&ctx->shm_pool);
 #endif

     xcb_disconnect(ctx->conn);

     return 0;
 }

 static xcb_screen_t *get_screen(const xcb_setup_t *setup, int screen_num)
 {
     xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
     xcb_screen_t *screen     = NULL;

     for (; it.rem > 0; xcb_screen_next (&it)) {
         if (!screen_num) {
             screen = it.data;
             break;
         }

         screen_num--;
     }

     return screen;
 }

 static int pixfmt_from_pixmap_format(AVFormatContext *s, int depth,
                                      int *pix_fmt, int *bpp)
 {
     XCBGrabContext *c        = s->priv_data;
     const xcb_setup_t *setup = xcb_get_setup(c->conn);
     const xcb_format_t *fmt  = xcb_setup_pixmap_formats(setup);
     int length               = xcb_setup_pixmap_formats_length(setup);

     *pix_fmt = AV_PIX_FMT_NONE;

     while (length--) {
         if (fmt->depth == depth) {
             switch (depth) {
             case 32:
                 if (fmt->bits_per_pixel == 32)
                     *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                                AV_PIX_FMT_BGR0 : AV_PIX_FMT_0RGB;
                 break;
             case 24:
                 if (fmt->bits_per_pixel == 32)
                     *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                                AV_PIX_FMT_BGR0 : AV_PIX_FMT_0RGB;
                 else if (fmt->bits_per_pixel == 24)
                     *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                                AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24;
                 break;
             case 16:
                 if (fmt->bits_per_pixel == 16)
                     *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                                AV_PIX_FMT_RGB565LE : AV_PIX_FMT_RGB565BE;
                 break;
             case 15:
                 if (fmt->bits_per_pixel == 16)
                     *pix_fmt = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST ?
                                AV_PIX_FMT_RGB555LE : AV_PIX_FMT_RGB555BE;
                 break;
             case 8:
                 if (fmt->bits_per_pixel == 8)
                     *pix_fmt = AV_PIX_FMT_PAL8;
                 break;
             }
         }

         if (*pix_fmt != AV_PIX_FMT_NONE) {
             *bpp        = fmt->bits_per_pixel;
             return 0;
         }

         fmt++;
     }
     avpriv_report_missing_feature(s, "Mapping X11 pixmap format (depth %d) to AVjścietFormat", depth);

     return AVERROR_PATCHWELCOME;
 }

 static int create_stream(AVFormatContext *s)
 {
     XCBGrabContext *c = s->priv_data;
     AVStream *st      = avformat_new_stream(s, NULL);
     xcb_get_geometry_cookie_t gc;
     xcb_get_geometry_reply_t *geo;
     int64_t frame_size_bits;
     int ret;
     enum AVPixelFormat native_pix_fmt;

     if (!st)
         return AVERROR(ENOMEM);

     ret = av_parse_video_rate(&st->avg_frame_rate, c->framerate);
     if (ret < 0) {
         av_log(s, AV_LOG_ERROR, "Failed to parse framerate: %s\n", c->framerate);
         return ret;
     }

     avpriv_set_pts_info(st, 64, 1, 1000000);

     gc  = xcb_get_geometry(c->conn, c->window_id != XCB_NONE ? c->window_id : c->screen->root);
     geo = xcb_get_geometry_reply(c->conn, gc, NULL);
     if (!geo) {
         av_log(s, AV_LOG_ERROR, "Can't find window '0x%x', aborting.\n", c->window_id);
         return AVERROR_EXTERNAL;
     }

     if (!c->width || !c->height) {
         c->width = geo->width;
         c->height = geo->height;
     }

     if (c->x + c->width > geo->width ||
         c->y + c->height > geo->height) {
         av_log(s, AV_LOG_ERROR,
                "Capture area %dx%d at position %d.%d "
                "outside the screen size %dx%d\n",
                c->width, c->height,
                c->x, c->y,
                geo->width, geo->height);
         free(geo);
         return AVERROR(EINVAL);
     }

     c->time_base  = (AVRational){ st->avg_frame_rate.den,
                                   st->avg_frame_rate.num };
     c->frame_duration = av_rescale_q(1, c->time_base, AV_TIME_BASE_Q);
     c->time_frame = av_gettime_relative();

     ret = pixfmt_from_pixmap_format(s, geo->depth, &native_pix_fmt, &c->bpp);
     free(geo);
     geo = NULL;
     if (ret < 0)
         return ret;

     st->codecpar->format = native_pix_fmt;

     frame_size_bits = (int64_t)c->width * c->height * c->bpp;
     if (frame_size_bits <= 0 || (frame_size_bits / 8 + AV_INPUT_BUFFER_PADDING_SIZE > INT_MAX) ) {
         av_log(s, AV_LOG_ERROR, "Captured area is too large or invalid (bpp: %d, w:%d, h:%d)\n", c->bpp, c->width, c->height);
         return AVERROR_INVALIDDATA;
     }
     c->frame_size = frame_size_bits / 8;

 #if CONFIG_LIBXCB_SHM
     c->shm_pool = av_buffer_pool_init2(c->frame_size + AV_INPUT_BUFFER_PADDING_SIZE,
                                            c->conn, allocate_shm_buffer, NULL);
     if (!c->shm_pool) {
         av_log(s, AV_LOG_WARNING, "Failed to initialize SHM buffer pool, proceeding without SHM.\n");
         c->has_shm = 0;
     }
 #endif

     st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
     st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
     st->codecpar->width      = c->width;
     st->codecpar->height     = c->height;
     st->codecpar->bit_rate   = av_rescale(frame_size_bits, st->avg_frame_rate.num, st->avg_frame_rate.den);
     st->codecpar->sample_aspect_ratio = (AVRational){1, 1};


 #if CONFIG_LIBNPP
     c->perform_npp_conversion = 0;
     if (native_pix_fmt == AV_PIX_FMT_BGR0) {
         av_log(s, AV_LOG_INFO, "XCBGrab: Native format is BGR0. Attempting to output YUV420P using NPP.\n");
         st->codecpar->format = AV_PIX_FMT_YUV420P;
         c->perform_npp_conversion = 1;
         if (c->bpp > 0) {
             st->codecpar->bit_rate = av_rescale(st->codecpar->bit_rate, 12, c->bpp);
         } else {
             av_log(s, AV_LOG_WARNING, "Original bpp is 0, cannot accurately adjust bitrate for YUV420P.\n");
         }
     }
 #endif

     return 0;
 }

 static void draw_rectangle(AVFormatContext *s)
 {
     XCBGrabContext *c = s->priv_data;
     xcb_gcontext_t gc = xcb_generate_id(c->conn);
     uint32_t mask     = XCB_GC_FOREGROUND |
                         XCB_GC_BACKGROUND |
                         XCB_GC_LINE_WIDTH |
                         XCB_GC_LINE_STYLE |
                         XCB_GC_FILL_STYLE;
     uint32_t values[] = { c->screen->black_pixel,
                           c->screen->white_pixel,
                           c->region_border,
                           XCB_LINE_STYLE_DOUBLE_DASH,
                           XCB_FILL_STYLE_SOLID };
     xcb_rectangle_t r = { 1, 1,
                           (uint16_t)(c->width  + c->region_border * 2 - 3), // Cast for safety
                           (uint16_t)(c->height + c->region_border * 2 - 3) }; // Cast for safety

     xcb_create_gc(c->conn, gc, c->window, mask, values);

     xcb_poly_rectangle(c->conn, c->window, gc, 1, &r);
     xcb_free_gc(c->conn, gc);
 }

 static void setup_window(AVFormatContext *s)
 {
     XCBGrabContext *c = s->priv_data;
     uint32_t mask     = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
     uint32_t values[] = { 1,
                           XCB_EVENT_MASK_EXPOSURE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY };
     av_unused xcb_rectangle_t rect = { 0, 0, (uint16_t)c->width, (uint16_t)c->height };

 #if CONFIG_LIBXCB_SHAPE
     const xcb_query_extension_reply_t *shape_reply; // Moved declaration
 #endif

     c->window = xcb_generate_id(c->conn);

     xcb_create_window(c->conn, XCB_COPY_FROM_PARENT,
                       c->window,
                       c->screen->root,
                       (int16_t)(c->x - c->region_border),
                       (int16_t)(c->y - c->region_border),
                       (uint16_t)(c->width + c->region_border * 2),
                       (uint16_t)(c->height + c->region_border * 2),
                       0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT,
                       XCB_COPY_FROM_PARENT,
                       mask, values);

 #if CONFIG_LIBXCB_SHAPE
     shape_reply = xcb_get_extension_data(c->conn, &xcb_shape_id); // Assignment
     if (shape_reply && shape_reply->present) {
         xcb_shape_rectangles(c->conn, XCB_SHAPE_SO_SUBTRACT,
                              XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                              c->window,
                              (int16_t)c->region_border, (int16_t)c->region_border,
                              1, &rect);
     } else {
        av_log(s, AV_LOG_WARNING, "XShape extension not available, cannot create transparent hole in region window.\n");
     }
 #endif

     xcb_map_window(c->conn, c->window);
     xcb_flush(c->conn);

     draw_rectangle(s);
     xcb_flush(c->conn);
 }

 #define CROSSHAIR_CURSOR 34

 static xcb_rectangle_t rectangle_from_corners(xcb_point_t *corner_a,
                                               xcb_point_t *corner_b)
 {
     xcb_rectangle_t rectangle;
     rectangle.x = FFMIN(corner_a->x, corner_b->x);
     rectangle.y = FFMIN(corner_a->y, corner_b->y);
     rectangle.width = FFABS(corner_a->x - corner_b->x);
     rectangle.height = FFABS(corner_a->y - corner_b->y);
     return rectangle;
 }

 static int select_region(AVFormatContext *s)
 {
     XCBGrabContext *c = s->priv_data;
     xcb_connection_t *conn = c->conn;
     xcb_screen_t *screen = c->screen;

     int ret = 0, done = 0, was_pressed = 0;
     xcb_cursor_t cursor = XCB_NONE;
     xcb_font_t cursor_font = XCB_NONE;
     xcb_point_t press_position = {0, 0};
     xcb_generic_event_t *event;
     xcb_rectangle_t rectangle = { 0 };
     xcb_grab_pointer_reply_t *reply;
     xcb_grab_pointer_cookie_t cookie;

     xcb_window_t root_window = screen->root;
     xcb_gcontext_t gc = xcb_generate_id(conn);
     uint32_t mask = XCB_GC_FUNCTION | XCB_GC_SUBWINDOW_MODE;
     uint32_t values[] = { XCB_GX_INVERT, XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS };
     xcb_create_gc(conn, gc, root_window, mask, values);

     cursor_font = xcb_generate_id(conn);
     xcb_open_font_checked(conn, cursor_font, strlen("cursor"), "cursor");

     cursor = xcb_generate_id(conn);
     xcb_create_glyph_cursor_checked(conn, cursor, cursor_font, cursor_font,
                             CROSSHAIR_CURSOR, CROSSHAIR_CURSOR + 1,
                             0, 0, 0,
                             0xFFFF, 0xFFFF, 0xFFFF);

     cookie = xcb_grab_pointer(conn, 0, root_window,
                               XCB_EVENT_MASK_BUTTON_PRESS |
                               XCB_EVENT_MASK_BUTTON_RELEASE |
                               XCB_EVENT_MASK_POINTER_MOTION,
                               XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                               root_window, cursor, XCB_CURRENT_TIME);
     reply = xcb_grab_pointer_reply(conn, cookie, NULL);
     if (!reply || reply->status != XCB_GRAB_STATUS_SUCCESS) {
         av_log(s, AV_LOG_ERROR,
                "Failed to select region. Could not grab pointer (status: %d).\n",
                reply ? reply->status : -1);
         ret = AVERROR(EIO);
         free(reply);
         goto fail;
     }
     free(reply);

     xcb_grab_server(conn);
     xcb_flush(conn);

     while (!done && (event = xcb_wait_for_event(conn))) {
         switch (event->response_type & ~0x80) {
         case XCB_BUTTON_PRESS: {
             xcb_button_press_event_t *press = (xcb_button_press_event_t *)event;
             if (press->detail != 1) break;
             press_position = (xcb_point_t){ press->event_x, press->event_y };
             rectangle.x = press_position.x;
             rectangle.y = press_position.y;
             rectangle.width = 0;
             rectangle.height = 0;
             xcb_poly_rectangle(conn, root_window, gc, 1, &rectangle);
             was_pressed = 1;
             break;
         }
         case XCB_MOTION_NOTIFY: {
             if (was_pressed) {
                 xcb_motion_notify_event_t *motion =
                     (xcb_motion_notify_event_t *)event;
                 xcb_point_t cursor_position = { motion->event_x, motion->event_y };
                 xcb_poly_rectangle(conn, root_window, gc, 1, &rectangle);
                 rectangle = rectangle_from_corners(&press_position, &cursor_position);
                 xcb_poly_rectangle(conn, root_window, gc, 1, &rectangle);
             }
             break;
         }
         case XCB_BUTTON_RELEASE: {
             xcb_button_release_event_t *release = (xcb_button_release_event_t *)event;
             if (release->detail != 1) break;
             if (was_pressed) {
                 xcb_poly_rectangle(conn, root_window, gc, 1, &rectangle);
             }
             done = 1;
             break;
         }
         default:
             break;
         }
         xcb_flush(conn);
         free(event);
     }

     if (rectangle.width == 0 || rectangle.height == 0) {
         av_log(s, AV_LOG_WARNING, "Selected region has zero width or height. Using full screen or previous settings.\n");
     } else {
         c->width  = rectangle.width;
         c->height = rectangle.height;
         c->x = rectangle.x;
         c->y = rectangle.y;
     }

     xcb_ungrab_server(conn);
     xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
     xcb_flush(conn);

 fail:
     if (cursor != XCB_NONE) xcb_free_cursor(conn, cursor);
     if (cursor_font != XCB_NONE) xcb_close_font(conn, cursor_font);
     xcb_free_gc(conn, gc);
     return ret;
 }

 
#if CONFIG_LIBNPP
/**
 * GPU-accelerated conversion: host BGR0 → planar YUV 4:2:0
 * Fixed version with proper synchronization
 */
static int bgr0_to_yuv420p_gpu(uint8_t       *dstY,
                               uint8_t       *dstU,
                               uint8_t       *dstV,
                               const uint8_t *src,
                               int            width,
                               int            height)
{
    const int srcPitch  = width * 4;
    const int pitchY    = width;
    const int pitchUV   = width >> 1;
    NppiSize roi        = { width, height };

    uint8_t *d_src = NULL, *d_y = NULL, *d_u = NULL, *d_v = NULL;
    size_t   d_srcPitch = 0, d_pitchY = 0, d_pitchUV = 0;

    Npp8u *pDstPlanes_NPP[3];
    int    pDstSteps_NPP[3];
    cudaError_t cuda_err;
    NppStatus npp_stat;
    
    // Create CUDA stream for this operation
    cudaStream_t stream;
    cuda_err = cudaStreamCreate(&stream);
    if (cuda_err != cudaSuccess) {
        av_log(NULL, AV_LOG_ERROR, "cudaStreamCreate failed: %s\n", cudaGetErrorString(cuda_err));
        return AVERROR_EXTERNAL;
    }

    #define LOG_CUDA_ERROR(err, msg) if (err != cudaSuccess) { av_log(NULL, AV_LOG_ERROR, "%s: %s\n", msg, cudaGetErrorString(err)); goto fail; }
    #define LOG_NPP_ERROR(stat, msg) if (stat != NPP_SUCCESS) { av_log(NULL, AV_LOG_ERROR, "%s: NPP error %d\n", msg, stat); goto fail; }

    // Allocate GPU memory
    cuda_err = cudaMallocPitch((void **)&d_src, &d_srcPitch, srcPitch, height);
    LOG_CUDA_ERROR(cuda_err, "cudaMallocPitch d_src failed");
    cuda_err = cudaMallocPitch((void **)&d_y,   &d_pitchY,   pitchY,   height);
    LOG_CUDA_ERROR(cuda_err, "cudaMallocPitch d_y failed");
    cuda_err = cudaMallocPitch((void **)&d_u,   &d_pitchUV,  pitchUV,  height >> 1);
    LOG_CUDA_ERROR(cuda_err, "cudaMallocPitch d_u failed");
    cuda_err = cudaMallocPitch((void **)&d_v,   &d_pitchUV,  pitchUV,  height >> 1);
    LOG_CUDA_ERROR(cuda_err, "cudaMallocPitch d_v failed");

    // Copy input data to GPU with stream
    cuda_err = cudaMemcpy2DAsync(d_src, d_srcPitch, src, srcPitch, srcPitch, height,
                                cudaMemcpyHostToDevice, stream);
    LOG_CUDA_ERROR(cuda_err, "cudaMemcpy2DAsync d_src failed");

    // Wait for copy to complete before NPP operation
    cuda_err = cudaStreamSynchronize(stream);
    LOG_CUDA_ERROR(cuda_err, "cudaStreamSynchronize after H2D copy failed");

    // Set up NPP conversion parameters
    pDstPlanes_NPP[0] = d_y;
    pDstPlanes_NPP[1] = d_u;
    pDstPlanes_NPP[2] = d_v;

    pDstSteps_NPP[0] = (int)d_pitchY;
    pDstSteps_NPP[1] = (int)d_pitchUV;
    pDstSteps_NPP[2] = (int)d_pitchUV;

    // Perform NPP conversion
    npp_stat = nppiBGRToYUV420_8u_AC4P3R(d_src, (int)d_srcPitch,
                                         pDstPlanes_NPP, pDstSteps_NPP,
                                         roi);
    LOG_NPP_ERROR(npp_stat, "nppiBGRToYUV420_8u_AC4P3R failed");

    // Copy results back to host with stream
    cuda_err = cudaMemcpy2DAsync(dstY, pitchY,  d_y, d_pitchY,  pitchY,  height,
                                cudaMemcpyDeviceToHost, stream);
    LOG_CUDA_ERROR(cuda_err, "cudaMemcpy2DAsync dstY failed");
    cuda_err = cudaMemcpy2DAsync(dstU, pitchUV, d_u, d_pitchUV, pitchUV, height >> 1,
                                cudaMemcpyDeviceToHost, stream);
    LOG_CUDA_ERROR(cuda_err, "cudaMemcpy2DAsync dstU failed");
    cuda_err = cudaMemcpy2DAsync(dstV, pitchUV, d_v, d_pitchUV, pitchUV, height >> 1,
                                cudaMemcpyDeviceToHost, stream);
    LOG_CUDA_ERROR(cuda_err, "cudaMemcpy2DAsync dstV failed");

    // CRITICAL: Wait for all operations to complete
    cuda_err = cudaStreamSynchronize(stream);
    LOG_CUDA_ERROR(cuda_err, "cudaStreamSynchronize after D2H copy failed");

    // Clean up
    cudaStreamDestroy(stream);
    cudaFree(d_src); cudaFree(d_y); cudaFree(d_u); cudaFree(d_v);
    return 0;

fail:
    if (stream) cudaStreamDestroy(stream);
    if (d_src) cudaFree(d_src);
    if (d_y)   cudaFree(d_y);
    if (d_u)   cudaFree(d_u);
    if (d_v)   cudaFree(d_v);
    return AVERROR_EXTERNAL;
}
#endif

 static av_cold int xcbgrab_read_header(AVFormatContext *s)
{
    XCBGrabContext *c = s->priv_data;
    int screen_num = 0; // Default screen number, xcb_connect can update this
    int ret = 0;
    const xcb_setup_t *setup;

    char *url_copy = NULL;
    char *display_spec_for_xcb = NULL; // String to be passed to xcb_connect
    char *plus_sign_ptr;

    // AVOption defaults will set c->x and c->y to 0 initially.

    if (s->url && s->url[0]) { // If URL is provided
        url_copy = av_strdup(s->url);
        if (!url_copy) {
            return AVERROR(ENOMEM);
        }

        plus_sign_ptr = strrchr(url_copy, '+');
        if (plus_sign_ptr) {
            int parsed_x, parsed_y;
            // Try to parse x,y after the last '+'
            if (sscanf(plus_sign_ptr, "+%d,%d", &parsed_x, &parsed_y) == 2) {
                c->x = parsed_x;
                c->y = parsed_y;
                *plus_sign_ptr = '\0'; // Null-terminate url_copy at '+'
                                       // Now url_copy contains only the display part, or is empty
                if (url_copy[0] != '\0') { // If there was something before "+x,y"
                    display_spec_for_xcb = url_copy;
                } else { // Input was just "+x,y"
                    display_spec_for_xcb = NULL; // Use default display
                }
            } else {
                // '+' was found, but x,y did not parse correctly.
                // Assume the whole string is the display name and log a warning.
                av_log(s, AV_LOG_WARNING,
                       "Found '+' in URL '%s', but could not parse x,y coordinates after it. "
                       "Interpreting entire string as display name.\n", s->url);
                display_spec_for_xcb = url_copy; // Pass the (unmodified) full string
            }
        } else {
            // No '+' found, the whole string is the display name.
            display_spec_for_xcb = url_copy;
        }
    } else {
        // s->url is NULL or empty, use default display (display_spec_for_xcb remains NULL).
        display_spec_for_xcb = NULL;
    }

    c->conn = xcb_connect(display_spec_for_xcb, &screen_num);

    // Check for connection errors
    if (!c->conn) { // xcb_connect itself returned NULL
        av_log(s, AV_LOG_ERROR, "xcb_connect failed for input URL '%s'. Effective display string used: '%s'.\n",
               s->url, display_spec_for_xcb ? display_spec_for_xcb : "default (NULL)");
        av_freep(&url_copy);
        return AVERROR(EIO);
    }

    ret = xcb_connection_has_error(c->conn);
    if (ret) { // xcb_connect succeeded but connection is in an error state
        av_log(s, AV_LOG_ERROR, "XCB connection error %d for input URL '%s'. Effective display string used: '%s'.\n",
               ret, s->url, display_spec_for_xcb ? display_spec_for_xcb : "default (NULL)");
        xcb_disconnect(c->conn); // Clean up the errored connection
        c->conn = NULL;
        av_freep(&url_copy);
        return AVERROR(EIO);
    }

    av_freep(&url_copy); // url_copy is no longer needed

    setup = xcb_get_setup(c->conn);

    c->screen = get_screen(setup, screen_num);
    if (!c->screen) {
        av_log(s, AV_LOG_ERROR, "The screen %d (0-indexed) does not exist for display '%s'.\n",
               screen_num, display_spec_for_xcb ? display_spec_for_xcb : "default (NULL)");
        xcbgrab_read_close(s); // Disconnects c->conn
        return AVERROR(EIO);
    }

    // ... (rest of the function: window_id logic, select_region, create_stream, etc.)
    // Ensure this part is identical to your previous working version before the parsing change.

    if (c->window_id == XCB_NONE) {
        // Default handled later if still XCB_NONE after select_region
    } else {
        if (c->select_region) {
            av_log(s, AV_LOG_WARNING, "select_region ignored when window_id is specified.\n");
            c->select_region = 0;
        }
        if (c->follow_mouse) {
            av_log(s, AV_LOG_WARNING, "follow_mouse ignored when window_id is specified.\n");
            c->follow_mouse = 0;
        }
    }

    if (c->select_region) {
        ret = select_region(s);
        if (ret < 0) {
            xcbgrab_read_close(s);
            return ret;
        }
    }
    // If window_id was not specified by user and not set by select_region, default to root.
    if (c->window_id == XCB_NONE) {
        c->window_id = c->screen->root;
    }


    ret = create_stream(s);
    if (ret < 0) {
        xcbgrab_read_close(s);
        return ret;
    }

#if CONFIG_LIBXCB_SHM
    if (c->shm_pool) { // Only check for SHM if pool was initialized
        c->has_shm = check_shm(c->conn);
        if (!c->has_shm) {
            av_log(s, AV_LOG_WARNING, "XCB SHM extension not available or failed, proceeding without SHM.\n");
            av_buffer_pool_uninit(&c->shm_pool); // Free pool if SHM not usable
        }
    } else {
        c->has_shm = 0; // Ensure SHM is off if pool is NULL
    }
#endif

#if CONFIG_LIBXCB_XFIXES
    if (c->draw_mouse) {
        if (!(c->draw_mouse = check_xfixes(c->conn))) {
            av_log(s, AV_LOG_WARNING,
                   "XFixes not available, cannot draw the mouse.\n");
        }
        if (c->draw_mouse && c->bpp < 24) {
            av_log(s, AV_LOG_WARNING,
                   "Drawing mouse on %d bpp screen may not look correct. Disabling draw_mouse.\n", c->bpp);
            c->draw_mouse = 0;
        }
    }
#endif

    if (c->show_region)
        setup_window(s);

    return 0;
}
 const FFInputFormat ff_xcbgrab_demuxer = {
     .p.name         = "x11grab",
     .p.long_name    = NULL_IF_CONFIG_SMALL("X11 screen capture, using XCB"),
     .p.flags        = AVFMT_NOFILE,
     .p.priv_class   = &xcbgrab_class,
     .priv_data_size = sizeof(XCBGrabContext),
     .read_header    = xcbgrab_read_header,
     .read_packet    = xcbgrab_read_packet,
     .read_close     = xcbgrab_read_close,
 };