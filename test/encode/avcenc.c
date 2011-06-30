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
 * Simple AVC encoder based on libVA.
 *
 * Usage:
 * ./avcenc <width> <height> <input file> <output file> [qp]
 */  

#include "sysdeps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>

#include <va/va.h>
#include "va_display.h"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2

#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        exit(1);                                                        \
    }

static VADisplay va_dpy;

static int picture_width, picture_width_in_mbs;
static int picture_height, picture_height_in_mbs;
static int frame_size;
static unsigned char *newImageBuffer = 0;

static int qp_value = 26;

static int intra_period = 30;
static int pb_period = 5;
static int frame_bit_rate = -1;

#define BR_CBR          0
#define BR_VBR          1
#define BR_CQP          2

#define MAX_SLICES      32

static int
build_packed_pic_buffer(unsigned char **header_buffer);

static int
build_packed_seq_buffer(unsigned char **header_buffer);

struct packed_data_format
{
    unsigned int length_in_bits;
    unsigned char flag;
    unsigned char num_skip_bytes;
    unsigned char pad[2];
};

struct {
    VAEncSequenceParameterBufferH264Ext seq_param;
    VAEncPictureParameterBufferH264Ext pic_param;
    VAEncSliceParameterBufferH264Ext slice_param[MAX_SLICES];
    VAEncH264DecRefPicMarkingBuffer dec_ref_pic_marking;
    VAContextID context_id;
    VAConfigID config_id;
    VABufferID seq_param_buf_id;                /* Sequence level parameter */
    VABufferID pic_param_buf_id;                /* Picture level parameter */
    VABufferID slice_param_buf_id[MAX_SLICES];  /* Slice level parameter, multil slices */
    VABufferID dec_ref_pic_marking_buf_id;
    VABufferID codedbuf_buf_id;                 /* Output buffer, compressed data */
    VABufferID packed_seq_buf_id;
    VABufferID packed_pic_buf_id;
    int num_slices;
    int codedbuf_i_size;
    int codedbuf_pb_size;
} avcenc_context;

static void create_encode_pipe()
{
    VAEntrypoint entrypoints[5];
    int num_entrypoints,slice_entrypoint;
    VAConfigAttrib attrib[2];
    int major_ver, minor_ver;
    VAStatus va_status;

    va_dpy = va_open_display();
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");

    vaQueryConfigEntrypoints(va_dpy, VAProfileH264Baseline, entrypoints, 
                             &num_entrypoints);

    for	(slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
        if (entrypoints[slice_entrypoint] == VAEntrypointEncSlice)
            break;
    }

    if (slice_entrypoint == num_entrypoints) {
        /* not find Slice entry point */
        assert(0);
    }

    /* find out the format for the render target, and rate control mode */
    attrib[0].type = VAConfigAttribRTFormat;
    attrib[1].type = VAConfigAttribRateControl;
    vaGetConfigAttributes(va_dpy, VAProfileH264Baseline, VAEntrypointEncSlice,
                          &attrib[0], 2);

    if ((attrib[0].value & VA_RT_FORMAT_YUV420) == 0) {
        /* not find desired YUV420 RT format */
        assert(0);
    }

    if ((attrib[1].value & VA_RC_VBR) == 0) {
        /* Can't find matched RC mode */
        printf("VBR mode doesn't found, exit\n");
        assert(0);
    }

    attrib[0].value = VA_RT_FORMAT_YUV420; /* set to desired RT format */
    attrib[1].value = VA_RC_VBR; /* set to desired RC mode */

    va_status = vaCreateConfig(va_dpy, VAProfileH264Baseline, VAEntrypointEncSlice,
                               &attrib[0], 2,&avcenc_context.config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    /* Create a context for this decode pipe */
    va_status = vaCreateContext(va_dpy, avcenc_context.config_id,
                                picture_width, picture_height,
                                VA_PROGRESSIVE, 
                                0, 0,
                                &avcenc_context.context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");
}

static void destory_encode_pipe()
{
    vaDestroyContext(va_dpy,avcenc_context.context_id);
    vaDestroyConfig(va_dpy,avcenc_context.config_id);
    vaTerminate(va_dpy);
    va_close_display(va_dpy);
}

/***************************************************
 *
 *  The encode pipe resource define 
 *
 ***************************************************/
#define SID_INPUT_PICTURE                       0
#define SID_REFERENCE_PICTURE_L0                1
#define SID_REFERENCE_PICTURE_L1				2
#define SID_RECON_PICTURE                       3
#define SID_NUMBER                              SID_RECON_PICTURE + 1
static  VASurfaceID surface_ids[SID_NUMBER];

static int enc_frame_number;

/***************************************************/

static void alloc_encode_resource()
{
    VAStatus va_status;

    // Create surface
    va_status = vaCreateSurfaces(
        va_dpy,
        VA_RT_FORMAT_YUV420, picture_width, picture_height,
        &surface_ids[0], SID_NUMBER,
        NULL, 0
    );

    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    newImageBuffer = (unsigned char *)malloc(frame_size);
}

static void release_encode_resource()
{
    free(newImageBuffer);

    // Release all the surfaces resource
    vaDestroySurfaces(va_dpy, &surface_ids[0], SID_NUMBER);	
}

static void avcenc_update_picture_parameter(int slice_type, int frame_num, int display_num, int is_idr)
{
    VAEncPictureParameterBufferH264Ext *pic_param;
    VAStatus va_status;

    // Picture level
    pic_param = &avcenc_context.pic_param;
    pic_param->CurrPic.picture_id = surface_ids[SID_RECON_PICTURE];
    pic_param->CurrPic.TopFieldOrderCnt = display_num * 2;
    pic_param->ReferenceFrames[0].picture_id = surface_ids[SID_REFERENCE_PICTURE_L0];
    pic_param->ReferenceFrames[1].picture_id = surface_ids[SID_REFERENCE_PICTURE_L1];
    pic_param->ReferenceFrames[2].picture_id = VA_INVALID_ID;
    assert(avcenc_context.codedbuf_buf_id != VA_INVALID_ID);
    pic_param->CodedBuf = avcenc_context.codedbuf_buf_id;
    pic_param->frame_num = frame_num;
    pic_param->pic_fields.bits.idr_pic_flag = !!is_idr;
    pic_param->pic_fields.bits.reference_pic_flag = (slice_type != SLICE_TYPE_B);

    va_status = vaCreateBuffer(va_dpy,
                               avcenc_context.context_id,
                               VAEncPictureParameterBufferExtType,
                               sizeof(*pic_param), 1, pic_param,
                               &avcenc_context.pic_param_buf_id);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");
}

static void upload_yuv_to_surface(FILE *yuv_fp, VASurfaceID surface_id)
{
    VAImage surface_image;
    VAStatus va_status;
    void *surface_p = NULL;
    unsigned char *y_src, *u_src, *v_src;
    unsigned char *y_dst, *u_dst, *v_dst;
    int y_size = picture_width * picture_height;
    int u_size = (picture_width >> 1) * (picture_height >> 1);
    int row, col;
    size_t n_items;

    do {
        n_items = fread(newImageBuffer, frame_size, 1, yuv_fp);
    } while (n_items != 1);

    va_status = vaDeriveImage(va_dpy, surface_id, &surface_image);
    CHECK_VASTATUS(va_status,"vaDeriveImage");

    vaMapBuffer(va_dpy, surface_image.buf, &surface_p);
    assert(VA_STATUS_SUCCESS == va_status);
        
    y_src = newImageBuffer;
    u_src = newImageBuffer + y_size; /* UV offset for NV12 */
    v_src = newImageBuffer + y_size + u_size;

    y_dst = surface_p + surface_image.offsets[0];
    u_dst = surface_p + surface_image.offsets[1]; /* UV offset for NV12 */
    v_dst = surface_p + surface_image.offsets[2];

    /* Y plane */
    for (row = 0; row < surface_image.height; row++) {
        memcpy(y_dst, y_src, surface_image.width);
        y_dst += surface_image.pitches[0];
        y_src += picture_width;
    }

    if (surface_image.format.fourcc == VA_FOURCC_NV12) { /* UV plane */
        for (row = 0; row < surface_image.height / 2; row++) {
            for (col = 0; col < surface_image.width / 2; col++) {
                u_dst[col * 2] = u_src[col];
                u_dst[col * 2 + 1] = v_src[col];
            }

            u_dst += surface_image.pitches[1];
            u_src += (picture_width / 2);
            v_src += (picture_width / 2);
        }
    } else {
        /* FIXME: fix this later */
        assert(0);
    }

    vaUnmapBuffer(va_dpy, surface_image.buf);
    vaDestroyImage(va_dpy, surface_image.image_id);
}

static void avcenc_update_slice_parameter(int slice_type)
{
    VAEncSliceParameterBufferH264Ext *slice_param;
    VAStatus va_status;
    int i;

    // Slice level
    i = 0;
    slice_param = &avcenc_context.slice_param[i];
    slice_param->start_row_number = 0;
    slice_param->slice_height = picture_height_in_mbs/16; /* Measured by MB */
    slice_param->pic_parameter_set_id = 0;
    slice_param->slice_type = slice_type;
    slice_param->direct_spatial_mv_pred_flag = 0;
    slice_param->num_ref_idx_l0_active_minus1 = 0;      /* FIXME: ??? */
    slice_param->num_ref_idx_l1_active_minus1 = 0;
    slice_param->cabac_init_idc = 0;
    slice_param->slice_qp_delta = 0;
    slice_param->disable_deblocking_filter_idc = 0;
    slice_param->slice_alpha_c0_offset_div2 = 2;
    slice_param->slice_beta_offset_div2 = 2;
    slice_param->idr_pic_id = 0;

    /* ref_pic_list_modification() */
    slice_param->ref_pic_list_modification_flag_l0 = 0;
    slice_param->ref_pic_list_modification_flag_l1 = 0;
    /* FIXME: fill other fields */

    va_status = vaCreateBuffer(va_dpy,
                               avcenc_context.context_id,
                               VAEncSliceParameterBufferExtType,
                               sizeof(*slice_param), 1, slice_param,
                               &avcenc_context.slice_param_buf_id[i]);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    i++;

    avcenc_context.num_slices = i;
}

static int begin_picture(FILE *yuv_fp, int frame_num, int display_num, int slice_type, int is_idr)
{
    VAStatus va_status;

    if (frame_num == 0) {
        unsigned char *packed_seq_buffer = NULL, *packed_pic_buffer = NULL;
        int seq_length, pic_length;

        assert(slice_type == SLICE_TYPE_I);
        seq_length = build_packed_seq_buffer(&packed_seq_buffer);
        va_status = vaCreateBuffer(va_dpy,
                                   avcenc_context.context_id,
                                   VAEncPackedSequenceParameterBufferType,
                                   (seq_length + 7) / 8, 1, packed_seq_buffer,
                                   &avcenc_context.packed_seq_buf_id);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");;

        pic_length = build_packed_pic_buffer(&packed_pic_buffer);
        va_status = vaCreateBuffer(va_dpy,
                                   avcenc_context.context_id,
                                   VAEncPackedPictureParameterBufferType,
                                   (pic_length + 7) / 8 , 1, packed_pic_buffer,
                                   &avcenc_context.packed_pic_buf_id);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");;

        free(packed_seq_buffer);
        free(packed_pic_buffer);
    }

    /* sequence parameter set */
    VAEncSequenceParameterBufferH264Ext *seq_param = &avcenc_context.seq_param;
    va_status = vaCreateBuffer(va_dpy,
                               avcenc_context.context_id,
                               VAEncSequenceParameterBufferExtType,
                               sizeof(*seq_param), 1, seq_param,
                               &avcenc_context.seq_param_buf_id);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    /* slice parameter */
    avcenc_update_slice_parameter(slice_type);

    /* Copy Image to target surface according input YUV data. */
    fseek(yuv_fp, frame_size * display_num, SEEK_SET);
    upload_yuv_to_surface(yuv_fp, surface_ids[SID_INPUT_PICTURE]);

    return 0;
}

static int avcenc_render_picture()
{
    VAStatus va_status;
    VABufferID va_buffers[8];
    unsigned int num_va_buffers = 0;

    va_buffers[num_va_buffers++] = avcenc_context.seq_param_buf_id;
    va_buffers[num_va_buffers++] = avcenc_context.pic_param_buf_id;

    if (avcenc_context.dec_ref_pic_marking_buf_id != VA_INVALID_ID)
        va_buffers[num_va_buffers++] = avcenc_context.dec_ref_pic_marking_buf_id;

    if (avcenc_context.packed_seq_buf_id != VA_INVALID_ID)
        va_buffers[num_va_buffers++] = avcenc_context.packed_seq_buf_id;

    if (avcenc_context.packed_pic_buf_id != VA_INVALID_ID)
        va_buffers[num_va_buffers++] = avcenc_context.packed_pic_buf_id;

    va_status = vaBeginPicture(va_dpy,
                               avcenc_context.context_id,
                               surface_ids[SID_INPUT_PICTURE]);
    CHECK_VASTATUS(va_status,"vaBeginPicture");

    va_status = vaRenderPicture(va_dpy,
                                avcenc_context.context_id,
                                va_buffers,
                                num_va_buffers);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    va_status = vaRenderPicture(va_dpy,
                                avcenc_context.context_id,
                                &avcenc_context.slice_param_buf_id[0],
                                avcenc_context.num_slices);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    va_status = vaEndPicture(va_dpy, avcenc_context.context_id);
    CHECK_VASTATUS(va_status,"vaEndPicture");

    return 0;
}

static int avcenc_destroy_buffers(VABufferID *va_buffers, unsigned int num_va_buffers)
{
    VAStatus va_status;
    unsigned int i;

    for (i = 0; i < num_va_buffers; i++) {
        if (va_buffers[i] != VA_INVALID_ID) {
            va_status = vaDestroyBuffer(va_dpy, va_buffers[i]);
            CHECK_VASTATUS(va_status,"vaDestroyBuffer");
            va_buffers[i] = VA_INVALID_ID;
        }
    }

    return 0;
}

static void end_picture(int slice_type, int next_is_bpic)
{
    VABufferID tempID;

    /* Prepare for next picture */
    tempID = surface_ids[SID_RECON_PICTURE];  

    if (slice_type != SLICE_TYPE_B) {
        if (next_is_bpic) {
            surface_ids[SID_RECON_PICTURE] = surface_ids[SID_REFERENCE_PICTURE_L1]; 
            surface_ids[SID_REFERENCE_PICTURE_L1] = tempID;	
        } else {
            surface_ids[SID_RECON_PICTURE] = surface_ids[SID_REFERENCE_PICTURE_L0]; 
            surface_ids[SID_REFERENCE_PICTURE_L0] = tempID;
        }
    } else {
        if (!next_is_bpic) {
            surface_ids[SID_RECON_PICTURE] = surface_ids[SID_REFERENCE_PICTURE_L0]; 
            surface_ids[SID_REFERENCE_PICTURE_L0] = surface_ids[SID_REFERENCE_PICTURE_L1];
            surface_ids[SID_REFERENCE_PICTURE_L1] = tempID;
        }
    }

    avcenc_destroy_buffers(&avcenc_context.seq_param_buf_id, 1);
    avcenc_destroy_buffers(&avcenc_context.pic_param_buf_id, 1);
    avcenc_destroy_buffers(&avcenc_context.dec_ref_pic_marking_buf_id, 1);
    avcenc_destroy_buffers(&avcenc_context.packed_seq_buf_id, 1);
    avcenc_destroy_buffers(&avcenc_context.packed_pic_buf_id, 1);
    avcenc_destroy_buffers(&avcenc_context.slice_param_buf_id[0], avcenc_context.num_slices);
    avcenc_destroy_buffers(&avcenc_context.codedbuf_buf_id, 1);
    memset(avcenc_context.slice_param, 0, sizeof(avcenc_context.slice_param));
    avcenc_context.num_slices = 0;
}

#define BITSTREAM_ALLOCATE_STEPPING     4096

struct __bitstream {
    unsigned int *buffer;
    int bit_offset;
    int max_size_in_dword;
};

typedef struct __bitstream bitstream;

#if 0
static int 
get_coded_bitsteam_length(unsigned char *buffer, int buffer_length)
{
    int i;

    for (i = 0; i < buffer_length - 3; i++) {
        if (!buffer[i] &&
            !buffer[i + 1] &&
            !buffer[i + 2] &&
            !buffer[i + 3])
            break;
    }

    return i;
}
#endif

static unsigned int 
va_swap32(unsigned int val)
{
    unsigned char *pval = (unsigned char *)&val;

    return ((pval[0] << 24)     |
            (pval[1] << 16)     |
            (pval[2] << 8)      |
            (pval[3] << 0));
}

static void
bitstream_start(bitstream *bs)
{
    bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
    bs->buffer = calloc(bs->max_size_in_dword * sizeof(int), 1);
    bs->bit_offset = sizeof(struct packed_data_format) * 8; /* the first 64 bits used for format */
}

static void
bitstream_end(bitstream *bs)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;
    struct packed_data_format *format;

    if (bit_offset) {
        bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
    }

    format = (struct packed_data_format *)bs->buffer;
    format->length_in_bits = bs->bit_offset - sizeof(struct packed_data_format) * 8;
    format->flag |= 1;
    format->num_skip_bytes = 5; /* ignore start code & nal type for emulation prevetion check */
}
 
static void
bitstream_put_ui(bitstream *bs, unsigned int val, int size_in_bits)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (!size_in_bits)
        return;

    bs->bit_offset += size_in_bits;

    if (bit_left > size_in_bits) {
        bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
    } else {
        size_in_bits -= bit_left;
        bs->buffer[pos] = (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
        bs->buffer[pos] = va_swap32(bs->buffer[pos]);

        if (pos + 1 == bs->max_size_in_dword) {
            bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
            bs->buffer = realloc(bs->buffer, bs->max_size_in_dword * sizeof(unsigned int));
        }

        bs->buffer[pos + 1] = val;
    }
}

static void
bitstream_put_ue(bitstream *bs, unsigned int val)
{
    int size_in_bits = 0;
    int tmp_val = ++val;

    while (tmp_val) {
        tmp_val >>= 1;
        size_in_bits++;
    }

    bitstream_put_ui(bs, 0, size_in_bits - 1); // leading zero
    bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(bitstream *bs, int val)
{
    unsigned int new_val;

    if (val <= 0)
        new_val = -2 * val;
    else
        new_val = 2 * val - 1;

    bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(bitstream *bs, int bit)
{
    int bit_offset = (bs->bit_offset & 0x7);
    int bit_left = 8 - bit_offset;
    int new_val;

    if (!bit_offset)
        return;

    assert(bit == 0 || bit == 1);

    if (bit)
        new_val = (1 << bit_left) - 1;
    else
        new_val = 0;

    bitstream_put_ui(bs, new_val, bit_left);
}

static void 
rbsp_trailing_bits(bitstream *bs)
{
    bitstream_put_ui(bs, 1, 1);
    bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream *bs)
{
    bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
    bitstream_put_ui(bs, 0, 1);                /* forbidden_zero_bit: 0 */
    bitstream_put_ui(bs, nal_ref_idc, 2);
    bitstream_put_ui(bs, nal_unit_type, 5);
}

static void sps_rbsp(bitstream *bs)
{
    VAEncSequenceParameterBufferH264Ext *seq_param = &avcenc_context.seq_param;

    bitstream_put_ui(bs, seq_param->profile_idc, 8);    /* profile_idc */
    bitstream_put_ui(bs, 0, 1);                         /* constraint_set0_flag */
    bitstream_put_ui(bs, 1, 1);                         /* constraint_set1_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constraint_set2_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constraint_set3_flag */
    bitstream_put_ui(bs, 0, 4);                         /* reserved_zero_4bits */
    bitstream_put_ui(bs, seq_param->level_idc, 8);      /* level_idc */
    bitstream_put_ue(bs, seq_param->seq_parameter_set_id);      /* seq_parameter_set_id */

    if (seq_param->profile_idc >= PROFILE_IDC_HIGH) {
        /* FIXME: fix for high profile */
        assert(0);
    }

    bitstream_put_ue(bs, seq_param->log2_max_frame_num_minus4); /* log2_max_frame_num_minus4 */
    bitstream_put_ue(bs, seq_param->pic_order_cnt_type);        /* pic_order_cnt_type */

    if (seq_param->pic_order_cnt_type == 0)
        bitstream_put_ue(bs, seq_param->log2_max_pic_order_cnt_lsb_minus4);     /* log2_max_pic_order_cnt_lsb_minus4 */
    else {
        assert(0);
    }

    bitstream_put_ue(bs, seq_param->max_num_ref_frames);        /* num_ref_frames */
    bitstream_put_ui(bs, 0, 1);                                 /* gaps_in_frame_num_value_allowed_flag */

    bitstream_put_ue(bs, seq_param->picture_width_in_mbs - 1);  /* pic_width_in_mbs_minus1 */
    bitstream_put_ue(bs, seq_param->picture_height_in_mbs - 1); /* pic_height_in_map_units_minus1 */
    bitstream_put_ui(bs, seq_param->frame_mbs_only_flag, 1);    /* frame_mbs_only_flag */

    if (!seq_param->frame_mbs_only_flag) {
        assert(0);
    }

    bitstream_put_ui(bs, seq_param->direct_8x8_inference_flag, 1);      /* direct_8x8_inference_flag */
    bitstream_put_ui(bs, seq_param->frame_cropping_flag, 1);            /* frame_cropping_flag */

    if (seq_param->frame_cropping_flag) {
        bitstream_put_ue(bs, seq_param->frame_crop_left_offset);        /* frame_crop_left_offset */
        bitstream_put_ue(bs, seq_param->frame_crop_right_offset);       /* frame_crop_right_offset */
        bitstream_put_ue(bs, seq_param->frame_crop_top_offset);         /* frame_crop_top_offset */
        bitstream_put_ue(bs, seq_param->frame_crop_bottom_offset);      /* frame_crop_bottom_offset */
    }

    bitstream_put_ui(bs, 0, 1); /* vui_parameters_present_flag */
    rbsp_trailing_bits(bs);     /* rbsp_trailing_bits */
}

#if 0
static void build_nal_sps(FILE *avc_fp)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(&bs);
    bitstream_end(&bs, avc_fp);
}
#endif

static void pps_rbsp(bitstream *bs)
{
    VAEncPictureParameterBufferH264Ext *pic_param = &avcenc_context.pic_param;

    bitstream_put_ue(bs, pic_param->pic_parameter_set_id);      /* pic_parameter_set_id */
    bitstream_put_ue(bs, pic_param->seq_parameter_set_id);      /* seq_parameter_set_id */

    bitstream_put_ui(bs, pic_param->pic_fields.bits.entropy_coding_mode_flag, 1);  /* entropy_coding_mode_flag */

    bitstream_put_ui(bs, 0, 1);                         /* pic_order_present_flag: 0 */

    bitstream_put_ue(bs, 0);                            /* num_slice_groups_minus1 */

    bitstream_put_ue(bs, pic_param->num_ref_idx_l0_active_minus1);      /* num_ref_idx_l0_active_minus1 */
    bitstream_put_ue(bs, pic_param->num_ref_idx_l1_active_minus1);      /* num_ref_idx_l1_active_minus1 1 */

    bitstream_put_ui(bs, pic_param->pic_fields.bits.weighted_pred_flag, 1);     /* weighted_pred_flag: 0 */
    bitstream_put_ui(bs, pic_param->pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

    bitstream_put_se(bs, pic_param->pic_init_qp - 26);  /* pic_init_qp_minus26 */
    bitstream_put_se(bs, 0);                            /* pic_init_qs_minus26 */
    bitstream_put_se(bs, 0);                            /* chroma_qp_index_offset */

    bitstream_put_ui(bs, pic_param->pic_fields.bits.deblocking_filter_control_present_flag, 1); /* deblocking_filter_control_present_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constrained_intra_pred_flag */
    bitstream_put_ui(bs, 0, 1);                         /* redundant_pic_cnt_present_flag */

    rbsp_trailing_bits(bs);
}

#if 0
static void build_nal_pps(FILE *avc_fp)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(&bs);
    bitstream_end(&bs, avc_fp);
}

static void 
build_header(FILE *avc_fp)
{
    build_nal_sps(avc_fp);
    build_nal_pps(avc_fp);
}
#endif

static int
build_packed_pic_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static int
build_packed_seq_buffer(unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(&bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}


#if 0
static void 
slice_header(bitstream *bs, int frame_num, int display_frame, int slice_type, int nal_ref_idc, int is_idr)
{
    VAEncSequenceParameterBufferH264Ext *seq_param = &avcenc_context.seq_param;
    VAEncPictureParameterBufferH264Ext *pic_param = &avcenc_context.pic_param;
    int is_cabac = (pic_param->pic_fields.bits.entropy_coding_mode_flag == ENTROPY_MODE_CABAC);

    bitstream_put_ue(bs, 0);                   /* first_mb_in_slice: 0 */
    bitstream_put_ue(bs, slice_type);          /* slice_type */
    bitstream_put_ue(bs, 0);                   /* pic_parameter_set_id: 0 */
    bitstream_put_ui(bs, frame_num & 0x0F, seq_param->log2_max_frame_num_minus4 + 4);    /* frame_num */

    /* frame_mbs_only_flag == 1 */
    if (!seq_param->frame_mbs_only_flag) {
        /* FIXME: */
        assert(0);
    }

    if (is_idr)
        bitstream_put_ue(bs, 0);		/* idr_pic_id: 0 */

    if (seq_param->pic_order_cnt_type == 0) {
        bitstream_put_ui(bs, (display_frame*2) & 0x3F, seq_param->log2_max_pic_order_cnt_lsb_minus4 + 4);
        /* only support frame */
    } else {
        /* FIXME: */
        assert(0);
    }

    /* redundant_pic_cnt_present_flag == 0 */
    
    /* slice type */
    if (slice_type == SLICE_TYPE_P) {
        bitstream_put_ui(bs, 0, 1);            /* num_ref_idx_active_override_flag: 0 */
        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
    } else if (slice_type == SLICE_TYPE_B) {
        bitstream_put_ui(bs, 1, 1);            /* direct_spatial_mv_pred: 1 */
        bitstream_put_ui(bs, 0, 1);            /* num_ref_idx_active_override_flag: 0 */
        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l1: 0 */
    } 

    /* weighted_pred_flag == 0 */

    /* dec_ref_pic_marking */
    if (nal_ref_idc != 0) {
        if ( is_idr) {
            bitstream_put_ui(bs, 0, 1);            /* no_output_of_prior_pics_flag: 0 */
            bitstream_put_ui(bs, 0, 1);            /* long_term_reference_flag: 0 */
        } else {
            bitstream_put_ui(bs, 0, 1);            /* adaptive_ref_pic_marking_mode_flag: 0 */
        }
    }

    if (is_cabac && (slice_type != SLICE_TYPE_I))
        bitstream_put_ue(bs, 0);               /* cabac_init_idc: 0 */

    bitstream_put_se(bs, 0);                   /* slice_qp_delta: 0 */

    if (pic_param->pic_fields.bits.deblocking_filter_control_present_flag == 1) {
        bitstream_put_ue(bs, 0);               /* disable_deblocking_filter_idc: 0 */
        bitstream_put_se(bs, 2);               /* slice_alpha_c0_offset_div2: 2 */
        bitstream_put_se(bs, 2);               /* slice_beta_offset_div2: 2 */
    }
}

static void 
slice_data(bitstream *bs)
{
    VACodedBufferSegment *coded_buffer_segment;
    unsigned char *coded_mem;
    int i, slice_data_length;
    VAStatus va_status;
    VASurfaceStatus surface_status;

    va_status = vaSyncSurface(va_dpy, surface_ids[SID_INPUT_PICTURE]);
    CHECK_VASTATUS(va_status,"vaSyncSurface");

    surface_status = 0;
    va_status = vaQuerySurfaceStatus(va_dpy, surface_ids[SID_INPUT_PICTURE], &surface_status);
    CHECK_VASTATUS(va_status,"vaQuerySurfaceStatus");

    va_status = vaMapBuffer(va_dpy, avcenc_context.codedbuf_buf_id, (void **)(&coded_buffer_segment));
    CHECK_VASTATUS(va_status,"vaMapBuffer");
    coded_mem = coded_buffer_segment->buf;

    slice_data_length = get_coded_bitsteam_length(coded_mem, codedbuf_size);

    for (i = 0; i < slice_data_length; i++) {
        bitstream_put_ui(bs, *coded_mem, 8);
        coded_mem++;
    }

    vaUnmapBuffer(va_dpy, avcenc_context.codedbuf_buf_id);
}

static void 
build_nal_slice(FILE *avc_fp, int frame_num, int display_frame, int slice_type, int is_idr)
{
    bitstream bs;

    bitstream_start(&bs);
    slice_data(&bs);
    bitstream_end(&bs, avc_fp);
}

#endif

static int
store_coded_buffer(FILE *avc_fp, int slice_type)
{
    VACodedBufferSegment *coded_buffer_segment;
    unsigned char *coded_mem;
    int slice_data_length;
    VAStatus va_status;
    VASurfaceStatus surface_status;
    size_t w_items;

    va_status = vaSyncSurface(va_dpy, surface_ids[SID_INPUT_PICTURE]);
    CHECK_VASTATUS(va_status,"vaSyncSurface");

    surface_status = 0;
    va_status = vaQuerySurfaceStatus(va_dpy, surface_ids[SID_INPUT_PICTURE], &surface_status);
    CHECK_VASTATUS(va_status,"vaQuerySurfaceStatus");

    va_status = vaMapBuffer(va_dpy, avcenc_context.codedbuf_buf_id, (void **)(&coded_buffer_segment));
    CHECK_VASTATUS(va_status,"vaMapBuffer");
    coded_mem = coded_buffer_segment->buf;

    if (coded_buffer_segment->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) {
        if (slice_type == SLICE_TYPE_I)
            avcenc_context.codedbuf_i_size *= 2;
        else
            avcenc_context.codedbuf_pb_size *= 2;

        return -1;
    }

    slice_data_length = coded_buffer_segment->size;

    do {
        w_items = fwrite(coded_mem, slice_data_length, 1, avc_fp);
    } while (w_items != 1);

    if (slice_type == SLICE_TYPE_I) {
        if (avcenc_context.codedbuf_i_size > slice_data_length * 3 / 2) {
            avcenc_context.codedbuf_i_size = slice_data_length * 3 / 2;
        }
        
        if (avcenc_context.codedbuf_pb_size < slice_data_length) {
            avcenc_context.codedbuf_pb_size = slice_data_length;
        }
    } else {
        if (avcenc_context.codedbuf_pb_size > slice_data_length * 3 / 2) {
            avcenc_context.codedbuf_pb_size = slice_data_length * 3 / 2;
        }
    }

    vaUnmapBuffer(va_dpy, avcenc_context.codedbuf_buf_id);

    return 0;
}

static void
encode_picture(FILE *yuv_fp, FILE *avc_fp,
               int frame_num, int display_num,
               int is_idr,
               int slice_type, int next_is_bpic)
{
    VAStatus va_status;
    int count = 5, ret = 0, codedbuf_size;
    
    begin_picture(yuv_fp, frame_num, display_num, slice_type, is_idr);

    do {
        avcenc_destroy_buffers(&avcenc_context.codedbuf_buf_id, 1);
        avcenc_destroy_buffers(&avcenc_context.pic_param_buf_id, 1);


        if (SLICE_TYPE_I == slice_type) {
            codedbuf_size = avcenc_context.codedbuf_i_size;
        } else {
            codedbuf_size = avcenc_context.codedbuf_pb_size;
        }

        /* coded buffer */
        va_status = vaCreateBuffer(va_dpy,
                                   avcenc_context.context_id,
                                   VAEncCodedBufferType,
                                   codedbuf_size, 1, NULL,
                                   &avcenc_context.codedbuf_buf_id);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");

        /* picture parameter set */
        avcenc_update_picture_parameter(slice_type, frame_num, display_num, is_idr);

        avcenc_render_picture();
        ret = store_coded_buffer(avc_fp, slice_type);
    } while (ret && --count);

    end_picture(slice_type, next_is_bpic);
}

static void encode_i_picture(FILE *yuv_fp, FILE *avc_fp, int f, int is_idr)
{
    encode_picture(yuv_fp, avc_fp,
                   enc_frame_number, f,
                   is_idr,
                   SLICE_TYPE_I, 0);
}

static void encode_p_picture(FILE *yuv_fp, FILE *avc_fp, int f)
{
    encode_picture(yuv_fp, avc_fp,
                   enc_frame_number, f,
                   0,
                   SLICE_TYPE_P, 0);
}

static void encode_pb_pictures(FILE *yuv_fp, FILE *avc_fp, int f, int nbframes)
{
    int i;
    encode_picture(yuv_fp, avc_fp,
                   enc_frame_number, f + nbframes,
                   0,
                   SLICE_TYPE_P, 1);

    for( i = 0; i < nbframes - 1; i++) {
        encode_picture(yuv_fp, avc_fp,
                enc_frame_number + 1, f + i,
                0,
                SLICE_TYPE_B, 1);
    }
    
    encode_picture(yuv_fp, avc_fp,
                   enc_frame_number + 1, f + nbframes - 1,
                   0,
                   SLICE_TYPE_B, 0);
}

static void show_help()
{
    printf("Usage: avnenc <width> <height> <input_yuvfile> <output_avcfile> [qp=qpvalue|fb=framebitrate] [mode=1(I frames only)/2(I and P frames)/3(I, P and B frames)\n");
}

static void avcenc_context_seq_param_init(VAEncSequenceParameterBufferH264Ext *seq_param,
                                          int width, int height)

{
    int width_in_mbs = (width + 15) / 16;
    int height_in_mbs = (height + 15) / 16;
    int frame_cropping_flag = 0;
    int frame_crop_bottom_offset = 0;

    seq_param->seq_parameter_set_id = 0;
    seq_param->profile_idc = PROFILE_IDC_MAIN;
    seq_param->level_idc = 41;
    seq_param->intra_period = intra_period;
    seq_param->ip_period = 0;   /* FIXME: ??? */
    seq_param->max_num_ref_frames = 4;
    seq_param->picture_width_in_mbs = width_in_mbs;
    seq_param->picture_height_in_mbs = height_in_mbs;
    seq_param->frame_mbs_only_flag = 1;
    seq_param->target_usage = 1;
    
    /* 0:CBR, 1:VBR, 2:Constant QP */
    if (qp_value == -1)
        seq_param->rate_control_method = BR_CBR;
    else if (qp_value == -2)
        seq_param->rate_control_method = BR_VBR;
    else {
        assert(qp_value >= 0 && qp_value <= 51);
        seq_param->rate_control_method = BR_CQP;
    }

    if (frame_bit_rate > 0)
        seq_param->bits_per_second = 30 * frame_bit_rate;
    else
        seq_param->bits_per_second = 0;

    if (seq_param->rate_control_method == BR_VBR) {
        seq_param->max_bits_per_second = 0;     /* FIXME: set it later */
        seq_param->min_bits_per_second = 0;
    }

    seq_param->initial_hrd_buffer_fullness = 0; /* FIXME: ??? */
    seq_param->hrd_buffer_size = 0;             /* FIXME: ??? */
    seq_param->time_scale = 900;
    seq_param->num_units_in_tick = 15;

    if (height_in_mbs * 16 - height) {
        frame_cropping_flag = 1;
        frame_crop_bottom_offset = 
            (height_in_mbs * 16 - height) / (2 * (!seq_param->frame_mbs_only_flag + 1));
    }

    seq_param->frame_cropping_flag = frame_cropping_flag;
    seq_param->frame_crop_left_offset = 0;
    seq_param->frame_crop_right_offset = 0;
    seq_param->frame_crop_top_offset = 0;
    seq_param->frame_crop_bottom_offset = frame_crop_bottom_offset;

    seq_param->pic_order_cnt_type = 0;
    seq_param->direct_8x8_inference_flag = 0;
    
    seq_param->log2_max_frame_num_minus4 = 0;
    seq_param->log2_max_pic_order_cnt_lsb_minus4 = 2;

    seq_param->vui_flag = 0;
}

static void avcenc_context_pic_param_init(VAEncPictureParameterBufferH264Ext *pic_param)
{
    pic_param->seq_parameter_set_id = 0;
    pic_param->pic_parameter_set_id = 0;

    pic_param->last_picture = 0;
    pic_param->frame_num = 0;
    pic_param->coding_type = 0;
    
    pic_param->pic_init_qp = (qp_value >= 0 ?  qp_value : 26);
    pic_param->num_ref_idx_l0_active_minus1 = 0;
    pic_param->num_ref_idx_l1_active_minus1 = 0;

    pic_param->pic_fields.bits.idr_pic_flag = 0;
    pic_param->pic_fields.bits.reference_pic_flag = 0;
    pic_param->pic_fields.bits.entropy_coding_mode_flag = ENTROPY_MODE_CABAC;
    pic_param->pic_fields.bits.weighted_pred_flag = 0;
    pic_param->pic_fields.bits.weighted_bipred_idc = 0;
    pic_param->pic_fields.bits.transform_8x8_mode_flag = 0;
    pic_param->pic_fields.bits.deblocking_filter_control_present_flag = 1;
}

static void avcenc_context_init(int width, int height)
{
    int i;
    memset(&avcenc_context, 0, sizeof(avcenc_context));
    avcenc_context.seq_param_buf_id = VA_INVALID_ID;
    avcenc_context.pic_param_buf_id = VA_INVALID_ID;
    avcenc_context.dec_ref_pic_marking_buf_id = VA_INVALID_ID;
    avcenc_context.packed_seq_buf_id = VA_INVALID_ID;
    avcenc_context.packed_pic_buf_id = VA_INVALID_ID;
    avcenc_context.codedbuf_buf_id = VA_INVALID_ID;
    avcenc_context.codedbuf_i_size = width * height;
    avcenc_context.codedbuf_pb_size = 0;

    for (i = 0; i < MAX_SLICES; i++) {
        avcenc_context.slice_param_buf_id[i] = VA_INVALID_ID;
    }

    avcenc_context_seq_param_init(&avcenc_context.seq_param, width, height);
    avcenc_context_pic_param_init(&avcenc_context.pic_param);
}

int main(int argc, char *argv[])
{
    int f;
    FILE *yuv_fp;
    FILE *avc_fp;
    int frame_number;
    long file_size;
    clock_t start_clock, end_clock;
    float encoding_time;
    int i_frame_only=0,i_p_frame_only=1;
    int mode_value;

    va_init_display_args(&argc, argv);

    //TODO may be we should using option analytics library
    if(argc != 5 && argc != 6 && argc != 7) {
        show_help();
        return -1;
    }

    picture_width = atoi(argv[1]);
    picture_height = atoi(argv[2]);
    picture_width_in_mbs = (picture_width + 15) / 16;
    picture_height_in_mbs = (picture_height + 15) / 16;

    if (argc == 6) {
        qp_value = -1;
        sscanf(argv[5], "qp=%d", &qp_value);
        if ( qp_value == -1 ) {
            frame_bit_rate = -1;
            sscanf(argv[5], "fb=%d", &frame_bit_rate);
            if (  frame_bit_rate == -1 ) {
                show_help();
                return -1;
            }
        } else if (qp_value > 51) {
            qp_value = 51;
        } else if (qp_value < 0) {
            qp_value = 0;
        }
    } else
        qp_value = 28;                          //default const QP mode

    if (argc == 7) {
        qp_value = -1;
        sscanf(argv[6], "mode=%d", &mode_value);
        if ( mode_value == 0 ) {
                i_frame_only = 1;
		i_p_frame_only = 0;
        }
        else if ( mode_value == 1) {
		i_frame_only = 0;
                i_p_frame_only = 1;
        }
        else if ( mode_value == 2 ) {
		i_frame_only = 0;
                i_p_frame_only = 0;
        }
        else {
                printf("mode_value=%d\n",mode_value);
                show_help();
                return -1;
        }
    }

    yuv_fp = fopen(argv[3],"rb");
    if ( yuv_fp == NULL){
        printf("Can't open input YUV file\n");
        return -1;
    }
    fseek(yuv_fp,0l, SEEK_END);
    file_size = ftell(yuv_fp);
    frame_size = picture_width * picture_height +  ((picture_width * picture_height) >> 1) ;

    if ( (file_size < frame_size) || (file_size % frame_size) ) {
        printf("The YUV file's size is not correct\n");
        return -1;
    }
    frame_number = file_size / frame_size;
    fseek(yuv_fp, 0l, SEEK_SET);

    avc_fp = fopen(argv[4], "wb");	
    if ( avc_fp == NULL) {
        printf("Can't open output avc file\n");
        return -1;
    }	
    start_clock = clock();

    avcenc_context_init(picture_width, picture_height);
    create_encode_pipe();
    alloc_encode_resource();
	
    enc_frame_number = 0;
    for ( f = 0; f < frame_number; ) {		//picture level loop
        int is_intra = i_frame_only?1:(enc_frame_number % intra_period == 0);
        int is_idr = (f == 0);
        int is_bslice = 0;
		
        if ( ! is_intra && pb_period > 0) {
            is_bslice = i_p_frame_only?0:(f % pb_period == 1) && (f < frame_number - 1);	
        }
	
        if ( is_intra ) {
            encode_i_picture(yuv_fp, avc_fp, f, is_idr);
            f++;
            enc_frame_number++;
        } else if ( is_bslice) {
            encode_pb_pictures(yuv_fp, avc_fp, f, 2);   //last parameter is continue B frames number
            f += (1 + 2);
            enc_frame_number++;
        } else {
            encode_p_picture(yuv_fp, avc_fp, f);
            f++;
            enc_frame_number++;
        }
       
        printf("\r %d/%d ...", f+1, frame_number);
        fflush(stdout);
    }

    end_clock = clock();
    printf("\ndone!\n");
    encoding_time = (float)(end_clock-start_clock)/CLOCKS_PER_SEC;
    printf("encode %d frames in %f secondes, FPS is %.1f\n",frame_number, encoding_time, frame_number/encoding_time);

    release_encode_resource();
    destory_encode_pipe();

    return 0;
}
