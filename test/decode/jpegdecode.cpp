/*
 * Copyright (c) 2012 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * it is a real program to show how VAAPI decode work,
 * It does VLD decode for a simple JPEG clip "8x8.jpg"
 * and VA parameters are hardcoded into jpegdecode.cpp
 *
 * g++ -o  jpegdecode  jpegdecode.cpp -lva -lva-x11 -I/usr/include/va
 * ./jpegdecode  : only do decode
 * ./jpegdecode <any parameter >: decode+display
 *
 */  
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <va/va.h>

#ifdef ANDROID
#include <va/va_android.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <surfaceflinger/ISurfaceComposer.h>
#include <surfaceflinger/Surface.h>
#include <surfaceflinger/ISurface.h>
#include <surfaceflinger/SurfaceComposerClient.h>
#include <binder/MemoryHeapBase.h>
#define Display unsigned int

using namespace android;
sp<SurfaceComposerClient> client;
sp<Surface> android_surface;
sp<ISurface> android_isurface;
sp<SurfaceControl> surface_ctrl;
#include "../android_winsys.cpp"
#else
#include <va/va_x11.h>
#include <X11/Xlib.h>
#endif

#define CLIP_WIDTH  8
#define CLIP_HEIGHT 8

#define WIN_WIDTH  (CLIP_WIDTH<<4)
#define WIN_HEIGHT (CLIP_HEIGHT<<4)

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        exit(1);                                                        \
    }

/* Data dump of a 8x8 jpeg clip
 */
static unsigned char jpeg_clip[]={
    0xf8,0xbe,0x8a,0x28,0xaf,0xe5,0x33,0xfd,0xfc,0x3f	
};

/* hardcoded here without a bitstream parser helper
 * please see picture mpeg2-I.jpg for bitstream details
 */
static VAPictureParameterBufferJPEG pic_param={
 type:VA_JPEG_SOF0, /* SOFn */
 sample_precision:8,
 image_width:CLIP_WIDTH,
 image_height:CLIP_HEIGHT,
 num_components:3,
 components: {
        {
        component_id:1,
        h_sampling_factor:2,
        v_sampling_factor:2,
        quantiser_table_selector:0,
        },
        {
        component_id:2,
        h_sampling_factor:1,
        v_sampling_factor:1,
        quantiser_table_selector:1,
        },
        {
        component_id:3,
        h_sampling_factor:1,
        v_sampling_factor:1,
        quantiser_table_selector:1,
        },
    },

 /* ROI (region of interest), for JPEG2000 */
 roi: {
    enabled:0,
    start_x:0,
    start_y:0,
    end_x:0,
    end_y:0,
 },

 rotation:0
};

/* see JPEG spec65 for the defines of matrix */
static VAIQMatrixBufferJPEG iq_matrix = {
 precision: {
     0, 0, 0, 0
 },

 quantiser_matrix: {
     {
         0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x02, 0x02,
         0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x05,
         0x03, 0x03, 0x03, 0x03, 0x03, 0x06, 0x04, 0x04,
         0x03, 0x05, 0x07, 0x06, 0x07, 0x07, 0x07, 0x06,
         0x07, 0x07, 0x08, 0x09, 0x0b, 0x09, 0x08, 0x08,
         0x0a, 0x08, 0x07, 0x07, 0x0a, 0x0d, 0x0a, 0x0a,
         0x0b, 0x0c, 0x0c, 0x0c, 0x0c, 0x07, 0x09, 0x0e,
         0x0f, 0x0d, 0x0c, 0x0e, 0x0b, 0x0c, 0x0c, 0x0c,
     },
     {
         0x02, 0x02, 0x02, 0x03, 0x03, 0x03, 0x06, 0x03,
         0x03, 0x06, 0x0c, 0x08, 0x07, 0x08, 0x0c, 0x0c,
         0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
         0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
         0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
         0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
         0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
         0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
     },
 }
};

static VASliceParameterBufferJPEG slice_param={
 slice_data_size:10,	/* number of bytes in the slice data buffer for this slice */
 slice_data_offset:0,	/* the offset to the first byte of slice data */
 slice_data_flag:VA_SLICE_DATA_FLAG_ALL,	/* see VA_SLICE_DATA_FLAG_XXX definitions */
 slice_horizontal_position:0,
 slice_vertical_position:0,

 num_components:3,
 components: {
        {
        component_id:1,
        dc_selector:0,
        ac_selector:0,
        },
        {
        component_id:2,
        dc_selector:1,
        ac_selector:1,
        },
        {
        component_id:3,
        dc_selector:1,
        ac_selector:1,
        },
    },

 restart_interval:0, /* specifies the number of MCUs in restart interval, defined in DRI marker */
 num_mcus:1       /* indicates the number of MCUs in a scan */
};

static VAHuffmanTableBufferJPEG huffman_table_param = {
 huffman_table: {
     {
     dc_bits:{0,1,5,1,
              1,1,1,1,
              1,0,0,0},
     dc_huffval:{0x00,0x01,0x02,0x03,
                 0x04,0x05,0x06,0x07,
                 0x08,0x09,0x0a,0x0b},
     ac_bits:{0,2,1,3,
              3,2,4,3,
              5,5,4,4,
              0,0,1,125},
     ac_huffval:{0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
                 0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
                 0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
                 0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
                 0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
                 0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
                 0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
                 0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
                 0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
                 0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
                 0xf9,0xfa},
     },
     {
     dc_bits:{0,3,1,1,
              1,1,1,1,
              1,1,1,0},
     dc_huffval:{0x0,0x1,0x2,0x3,
                 0x4,0x5,0x6,0x7,
                 0x8,0x9,0xa,0xb},
     ac_bits:{0x0,0x2,0x1,0x2,
              0x4,0x4,0x3,0x4,
              0x7,0x5,0x4,0x4,
              0x0,0x1,0x2,0x77},
     ac_huffval:{
         0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
         0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
         0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
         0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
         0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
         0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
         0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
         0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
         0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
         0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
         0xf9,0xfa
     }
     },
 }
};

int main(int argc,char **argv)
{
    VAEntrypoint entrypoints[5];
    int num_entrypoints,vld_entrypoint;
    VAConfigAttrib attrib;
    VAConfigID config_id;
    VASurfaceID surface_id;
    VAContextID context_id;
    VABufferID pic_param_buf,iqmatrix_buf,huffmantable_buf,slice_param_buf,slice_data_buf;
    int major_ver, minor_ver;
    Display *x11_display;
    VADisplay	va_dpy;
    VAStatus va_status;
    int putsurface=0;

    if (argc > 1)
        putsurface=1;
#ifdef ANDROID 
    x11_display = (Display*)malloc(sizeof(Display));
    *(x11_display ) = 0x18c34078;
#else
    x11_display = XOpenDisplay(":0.0");
#endif

    if (x11_display == NULL) {
        fprintf(stderr, "Can't connect X server!\n");
        exit(-1);
    }

    assert(x11_display);
    
    va_dpy = vaGetDisplay(x11_display);
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
    assert(va_status == VA_STATUS_SUCCESS);
    
    va_status = vaQueryConfigEntrypoints(va_dpy, VAProfileJPEGBaseline, entrypoints, 
                                         &num_entrypoints);
    CHECK_VASTATUS(va_status, "vaQueryConfigEntrypoints");

    for	(vld_entrypoint = 0; vld_entrypoint < num_entrypoints; vld_entrypoint++) {
        if (entrypoints[vld_entrypoint] == VAEntrypointVLD)
            break;
    }
    if (vld_entrypoint == num_entrypoints) {
        /* not find VLD entry point */
        assert(0);
    }

    /* Assuming finding VLD, find out the format for the render target */
    attrib.type = VAConfigAttribRTFormat;
    vaGetConfigAttributes(va_dpy, VAProfileJPEGBaseline, VAEntrypointVLD,
                          &attrib, 1);
    if ((attrib.value & VA_RT_FORMAT_YUV420) == 0) {
        /* not find desired YUV420 RT format */
        assert(0);
    }
    
    va_status = vaCreateConfig(va_dpy, VAProfileJPEGBaseline, VAEntrypointVLD,
                               &attrib, 1,&config_id);
    CHECK_VASTATUS(va_status, "vaQueryConfigEntrypoints");

    va_status = vaCreateSurfaces(va_dpy,CLIP_WIDTH,CLIP_HEIGHT,
                                 VA_RT_FORMAT_YUV420, 1, &surface_id);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    /* Create a context for this decode pipe */
    va_status = vaCreateContext(va_dpy, config_id,
                                CLIP_WIDTH,
                                ((CLIP_HEIGHT+15)/16)*16,
                                VA_PROGRESSIVE,
                                &surface_id,
                                1,
                                &context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAPictureParameterBufferType,
                               sizeof(VAPictureParameterBufferJPEG),
                               1, &pic_param,
                               &pic_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");
    
    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAIQMatrixBufferType,
                               sizeof(VAIQMatrixBufferJPEG),
                               1, &iq_matrix,
                               &iqmatrix_buf );
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy, context_id,
                               VAHuffmanTableBufferType,
                               sizeof(VAHuffmanTableBufferJPEG),
                               1, &huffman_table_param,
                               &huffmantable_buf );
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy, context_id,
                               VASliceParameterBufferType,
                               sizeof(VASliceParameterBufferJPEG),
                               1,
                               &slice_param, &slice_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(va_dpy, context_id,
                               VASliceDataBufferType,
                               10,
                               1,
                               jpeg_clip,
                               &slice_data_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaBeginPicture(va_dpy, context_id, surface_id);
    CHECK_VASTATUS(va_status, "vaBeginPicture");

    va_status = vaRenderPicture(va_dpy,context_id, &pic_param_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture");
    
    va_status = vaRenderPicture(va_dpy,context_id, &iqmatrix_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture");

    va_status = vaRenderPicture(va_dpy,context_id, &huffmantable_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture");
    
    va_status = vaRenderPicture(va_dpy,context_id, &slice_param_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture");
    
    va_status = vaRenderPicture(va_dpy,context_id, &slice_data_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture");
    
    va_status = vaEndPicture(va_dpy,context_id);
    CHECK_VASTATUS(va_status, "vaEndPicture");

    va_status = vaSyncSurface(va_dpy, surface_id);
    CHECK_VASTATUS(va_status, "vaSyncSurface");

    if (putsurface) {
#ifdef ANDROID 
        sp<ProcessState> proc(ProcessState::self());
        ProcessState::self()->startThreadPool();

        printf("Create window0 for thread0\n");
        SURFACE_CREATE(client,surface_ctrl,android_surface, android_isurface, 0, 0, WIN_WIDTH, WIN_HEIGHT);

        va_status = vaPutSurface(va_dpy, surface_id, android_isurface,
                                 0,0,CLIP_WIDTH,CLIP_HEIGHT,
                                 0,0,WIN_WIDTH,WIN_HEIGHT,
                                 NULL,0,0);
#else
        Window  win;
        win = XCreateSimpleWindow(x11_display, RootWindow(x11_display, 0), 0, 0,
                                  WIN_WIDTH,WIN_HEIGHT, 0, 0, WhitePixel(x11_display, 0));
        XMapWindow(x11_display, win);
        XSync(x11_display, False);
        va_status = vaPutSurface(va_dpy, surface_id, win,
                                 0,0,CLIP_WIDTH,CLIP_HEIGHT,
                                 0,0,WIN_WIDTH,WIN_HEIGHT,
                                 NULL,0,0);
#endif
        CHECK_VASTATUS(va_status, "vaPutSurface");
    }
    printf("press any key to exit\n");
    getchar();

    vaDestroySurfaces(va_dpy,&surface_id,1);
    vaDestroyConfig(va_dpy,config_id);
    vaDestroyContext(va_dpy,context_id);

    vaTerminate(va_dpy);
#ifdef ANDROID
    free(x11_display);
#else
    XCloseDisplay(x11_display);
#endif
    
    return 0;
}
