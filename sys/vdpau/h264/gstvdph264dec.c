/* GStreamer
*
* Copyright (C) 2009 Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Library General Public
* License as published by the Free Software Foundation; either
* version 2 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* Library General Public License for more details.
*
* You should have received a copy of the GNU Library General Public
* License along with this library; if not, write to the
* Free Software Foundation, Inc., 59 Temple Place - Suite 330,
* Boston, MA 02111-1307, USA.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/base/gstadapter.h>
#include <gst/base/gstbitreader.h>
#include <string.h>

#include "../gstvdp/gstvdpvideosrcpad.h"

#include "gstvdph264frame.h"

#include "gstvdph264dec.h"

GST_DEBUG_CATEGORY_STATIC (gst_vdp_h264_dec_debug);
#define GST_CAT_DEFAULT gst_vdp_h264_dec_debug

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE (GST_BASE_VIDEO_DECODER_SINK_NAME,
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, " "parsed = (boolean) false")
    );

#define DEBUG_INIT(bla) \
    GST_DEBUG_CATEGORY_INIT (gst_vdp_h264_dec_debug, "vdpauh264dec", 0, \
    "VDPAU h264 decoder");

GST_BOILERPLATE_FULL (GstVdpH264Dec, gst_vdp_h264_dec, GstBaseVideoDecoder,
    GST_TYPE_BASE_VIDEO_DECODER, DEBUG_INIT);

#define SYNC_CODE_SIZE 3

#define READ_UINT8(reader, val, nbits) { \
  if (!gst_bit_reader_get_bits_uint8 (reader, &val, nbits)) { \
    GST_WARNING ("failed to read uint8, nbits: %d", nbits); \
    return FALSE; \
  } \
}

#define READ_UINT16(reader, val, nbits) { \
  if (!gst_bit_reader_get_bits_uint16 (reader, &val, nbits)) { \
  GST_WARNING ("failed to read uint16, nbits: %d", nbits); \
    return FALSE; \
  } \
}

#define SKIP(reader, nbits) { \
  if (!gst_bit_reader_skip (reader, nbits)) { \
  GST_WARNING ("failed to skip nbits: %d", nbits); \
    return FALSE; \
  } \
}

static gboolean
gst_vdp_h264_dec_set_sink_caps (GstBaseVideoDecoder * base_video_decoder,
    GstCaps * caps)
{
  GstVdpH264Dec *h264_dec;
  GstStructure *structure;
  const GValue *value;

  h264_dec = GST_VDP_H264_DEC (base_video_decoder);

  structure = gst_caps_get_structure (caps, 0);
  /* packetized video has a codec_data */
  if ((value = gst_structure_get_value (structure, "codec_data"))) {
    GstBuffer *buf;
    GstBitReader reader;
    guint8 version;
    guint8 n_sps, n_pps;
    gint i;

    GST_DEBUG_OBJECT (h264_dec, "have packetized h264");
    h264_dec->packetized = TRUE;

    buf = gst_value_get_buffer (value);
    GST_MEMDUMP ("avcC:", GST_BUFFER_DATA (buf), GST_BUFFER_SIZE (buf));

    /* parse the avcC data */
    if (GST_BUFFER_SIZE (buf) < 7) {
      GST_ERROR_OBJECT (h264_dec, "avcC size %u < 7", GST_BUFFER_SIZE (buf));
      return FALSE;
    }

    gst_bit_reader_init_from_buffer (&reader, buf);

    READ_UINT8 (&reader, version, 8);
    if (version != 1)
      return FALSE;

    SKIP (&reader, 30);

    READ_UINT8 (&reader, h264_dec->nal_length_size, 2);
    h264_dec->nal_length_size += 1;
    GST_DEBUG_OBJECT (h264_dec, "nal length %u", h264_dec->nal_length_size);

    SKIP (&reader, 3);

    READ_UINT8 (&reader, n_sps, 5);
    for (i = 0; i < n_sps; i++) {
      guint16 sps_length;
      guint8 *data;

      READ_UINT16 (&reader, sps_length, 16);
      sps_length -= 1;
      SKIP (&reader, 8);

      data = GST_BUFFER_DATA (buf) + gst_bit_reader_get_pos (&reader) / 8;
      if (!gst_h264_parser_parse_sequence (h264_dec->parser, data, sps_length))
        return FALSE;

      SKIP (&reader, sps_length * 8);
    }

    READ_UINT8 (&reader, n_pps, 8);
    for (i = 0; i < n_pps; i++) {
      guint16 pps_length;
      guint8 *data;

      READ_UINT16 (&reader, pps_length, 16);
      pps_length -= 1;
      SKIP (&reader, 8);

      data = GST_BUFFER_DATA (buf) + gst_bit_reader_get_pos (&reader) / 8;
      if (!gst_h264_parser_parse_picture (h264_dec->parser, data, pps_length))
        return FALSE;

      SKIP (&reader, pps_length * 8);
    }
  }

  return TRUE;
}

static GstFlowReturn
gst_vdp_h264_dec_shape_output (GstBaseVideoDecoder * base_video_decoder,
    GstBuffer * buf)
{
  GstVdpVideoSrcPad *vdp_pad;

  vdp_pad =
      (GstVdpVideoSrcPad *) GST_BASE_VIDEO_DECODER_SRC_PAD (base_video_decoder);

  return gst_vdp_video_src_pad_push (vdp_pad, GST_VDP_VIDEO_BUFFER (buf));
}

static GstFlowReturn
gst_vdp_h264_dec_handle_frame (GstBaseVideoDecoder * base_video_decoder,
    GstVideoFrame * frame, GstClockTimeDiff deadline)
{
  GstVdpH264Frame *h264_frame;

  GST_DEBUG ("handle_frame");

  h264_frame = (GstVdpH264Frame *) frame;

  GST_DEBUG ("frame_num: %d", h264_frame->slice_hdr.frame_num);
  GST_DEBUG ("pic_order_cnt_type: %d",
      h264_frame->slice_hdr.picture->sequence->pic_order_cnt_type);
  GST_DEBUG ("pic_order_cnt_lsb: %d", h264_frame->slice_hdr.pic_order_cnt_lsb);
  GST_DEBUG ("delta_pic_order_cnt_bottom: %d",
      h264_frame->slice_hdr.delta_pic_order_cnt_bottom);

  gst_base_video_decoder_skip_frame (base_video_decoder, frame);
  return GST_FLOW_OK;
}

static gint
gst_vdp_h264_dec_scan_for_sync (GstBaseVideoDecoder * base_video_decoder,
    GstAdapter * adapter)
{
  GstVdpH264Dec *h264_dec = GST_VDP_H264_DEC (base_video_decoder);
  gint m;

  if (h264_dec->packetized)
    return 0;

  m = gst_adapter_masked_scan_uint32 (adapter, 0xffffff00, 0x00000100,
      0, gst_adapter_available (adapter));
  if (m == -1)
    return gst_adapter_available (adapter) - SYNC_CODE_SIZE;

  return m;
}

static GstBaseVideoDecoderScanResult
gst_vdp_h264_dec_scan_for_packet_end (GstBaseVideoDecoder * base_video_decoder,
    GstAdapter * adapter, guint * size, gboolean at_eos)
{
  GstVdpH264Dec *h264_dec = GST_VDP_H264_DEC (base_video_decoder);
  guint avail;

  avail = gst_adapter_available (adapter);
  if (avail < h264_dec->nal_length_size)
    return GST_BASE_VIDEO_DECODER_SCAN_RESULT_NEED_DATA;

  if (h264_dec->packetized) {
    guint8 *data;
    gint i;
    guint32 nal_length;

    data = g_slice_alloc (h264_dec->nal_length_size);
    gst_adapter_copy (adapter, data, 0, h264_dec->nal_length_size);
    for (i = 0; i < h264_dec->nal_length_size; i++)
      nal_length = (nal_length << 8) | data[i];

    g_slice_free1 (h264_dec->nal_length_size, data);

    nal_length += h264_dec->nal_length_size;

    /* check for invalid NALU sizes, assume the size if the available bytes
     * when something is fishy */
    if (nal_length <= 1 || nal_length > avail) {
      nal_length = avail - h264_dec->nal_length_size;
      GST_DEBUG ("fixing invalid NALU size to %u", nal_length);
    }

    *size = nal_length;
  }

  else {
    guint8 *data;
    guint32 start_code;
    guint n;

    data = g_slice_alloc (SYNC_CODE_SIZE);
    gst_adapter_copy (adapter, data, 0, SYNC_CODE_SIZE);
    start_code = ((data[0] << 16) && (data[1] << 8) && data[2]);
    g_slice_free1 (SYNC_CODE_SIZE, data);

    GST_DEBUG ("start_code: %d", start_code);
    if (start_code == 0x000001)
      return GST_BASE_VIDEO_DECODER_SCAN_RESULT_LOST_SYNC;

    n = gst_adapter_masked_scan_uint32 (adapter, 0xffffff00, 0x00000100,
        SYNC_CODE_SIZE, avail - SYNC_CODE_SIZE);
    if (n == -1)
      return GST_BASE_VIDEO_DECODER_SCAN_RESULT_NEED_DATA;

    *size = n;
  }

  GST_DEBUG ("NAL size: %d", *size);

  return GST_BASE_VIDEO_DECODER_SCAN_RESULT_OK;
}

static GstFlowReturn
gst_vdp_h264_dec_parse_data (GstBaseVideoDecoder * base_video_decoder,
    GstBuffer * buf, gboolean at_eos)
{
  GstVdpH264Dec *h264_dec = GST_VDP_H264_DEC (base_video_decoder);
  GstBitReader reader;
  GstNalUnit nal_unit;
  guint8 forbidden_zero_bit;

  guint8 *data;
  guint size;
  gint i;

  GstVideoFrame *frame;

  GST_MEMDUMP ("data", GST_BUFFER_DATA (buf), GST_BUFFER_SIZE (buf));

  gst_bit_reader_init_from_buffer (&reader, buf);

  /* skip nal_length or sync code */
  gst_bit_reader_skip (&reader, h264_dec->nal_length_size * 8);

  if (!gst_bit_reader_get_bits_uint8 (&reader, &forbidden_zero_bit, 1))
    goto invalid_packet;
  if (forbidden_zero_bit != 0) {
    GST_WARNING ("forbidden_zero_bit != 0");
    return GST_FLOW_ERROR;
  }

  if (!gst_bit_reader_get_bits_uint16 (&reader, &nal_unit.ref_idc, 2))
    goto invalid_packet;
  GST_DEBUG ("nal_ref_idc: %u", nal_unit.ref_idc);

  /* read nal_unit_type */
  if (!gst_bit_reader_get_bits_uint16 (&reader, &nal_unit.type, 5))
    goto invalid_packet;

  GST_DEBUG ("nal_unit_type: %u", nal_unit.type);
  if (nal_unit.type == 14 || nal_unit.type == 20) {
    if (!gst_bit_reader_skip (&reader, 24))
      goto invalid_packet;
  }

  data = GST_BUFFER_DATA (buf) + gst_bit_reader_get_pos (&reader) / 8;
  size = gst_bit_reader_get_remaining (&reader) / 8;

  i = size - 1;
  while (size >= 0 && data[i] == 0x00) {
    size--;
    i--;
  }

  frame = gst_base_video_decoder_get_current_frame (base_video_decoder);

  /* does this mark the beginning of a new access unit */
  if (nal_unit.type == GST_NAL_AU_DELIMITER)
    gst_base_video_decoder_have_frame (base_video_decoder, &frame);

  if (GST_VIDEO_FRAME_FLAG_IS_SET (frame, GST_VDP_H264_FRAME_GOT_PRIMARY)) {
    if (nal_unit.type == GST_NAL_SPS || nal_unit.type == GST_NAL_PPS ||
        nal_unit.type == GST_NAL_SEI ||
        (nal_unit.type >= 14 && nal_unit.type <= 18))
      gst_base_video_decoder_have_frame (base_video_decoder, &frame);
  }

  if (nal_unit.type >= GST_NAL_SLICE && nal_unit.type <= GST_NAL_SLICE_IDR) {
    GstH264Slice slice;

    if (!gst_h264_parser_parse_slice_header (h264_dec->parser, &slice, data,
            size, nal_unit))
      goto invalid_packet;

    if (slice.redundant_pic_cnt == 0) {
      if (GST_VIDEO_FRAME_FLAG_IS_SET (frame, GST_VDP_H264_FRAME_GOT_PRIMARY)) {
        GstH264Slice *p_slice;
        guint8 pic_order_cnt_type, p_pic_order_cnt_type;

        p_slice = &(GST_VDP_H264_FRAME_CAST (frame)->slice_hdr);
        pic_order_cnt_type = slice.picture->sequence->pic_order_cnt_type;
        p_pic_order_cnt_type = p_slice->picture->sequence->pic_order_cnt_type;

        if (slice.frame_num != p_slice->frame_num)
          gst_base_video_decoder_have_frame (base_video_decoder, &frame);

        else if (slice.picture != p_slice->picture)
          gst_base_video_decoder_have_frame (base_video_decoder, &frame);

        else if (slice.bottom_field_flag != p_slice->bottom_field_flag)
          gst_base_video_decoder_have_frame (base_video_decoder, &frame);

        else if (nal_unit.ref_idc != p_slice->nal_unit.ref_idc &&
            (nal_unit.ref_idc == 0 || p_slice->nal_unit.ref_idc == 0))
          gst_base_video_decoder_have_frame (base_video_decoder, &frame);

        else if ((pic_order_cnt_type == 0 && p_pic_order_cnt_type == 0) &&
            (slice.pic_order_cnt_lsb != p_slice->pic_order_cnt_lsb ||
                slice.delta_pic_order_cnt_bottom !=
                p_slice->delta_pic_order_cnt_bottom))
          gst_base_video_decoder_have_frame (base_video_decoder, &frame);

        else if ((p_pic_order_cnt_type == 1 && p_pic_order_cnt_type == 1) &&
            (slice.delta_pic_order_cnt[0] != p_slice->delta_pic_order_cnt[0] ||
                slice.delta_pic_order_cnt[1] !=
                p_slice->delta_pic_order_cnt[1]))
          gst_base_video_decoder_have_frame (base_video_decoder, &frame);
      }

      if (!GST_VIDEO_FRAME_FLAG_IS_SET (frame, GST_VDP_H264_FRAME_GOT_PRIMARY)) {
        if (GST_H264_IS_I_SLICE (slice.type)
            || GST_H264_IS_SI_SLICE (slice.type))
          GST_VIDEO_FRAME_FLAG_SET (frame, GST_VIDEO_FRAME_FLAG_KEYFRAME);

        GST_VDP_H264_FRAME_CAST (frame)->slice_hdr = slice;
        GST_VIDEO_FRAME_FLAG_SET (frame, GST_VDP_H264_FRAME_GOT_PRIMARY);
      }
    }
    gst_vdp_h264_frame_add_slice ((GstVdpH264Frame *) frame, buf);
  }

  if (nal_unit.type == GST_NAL_SPS) {
    if (!gst_h264_parser_parse_sequence (h264_dec->parser, data, size))
      goto invalid_packet;
  }

  if (nal_unit.type == GST_NAL_PPS) {
    if (!gst_h264_parser_parse_picture (h264_dec->parser, data, size))
      goto invalid_packet;
  }

  if (nal_unit.type == GST_NAL_SEI) {
    GstH264Sequence *seq;
    GstH264SEIMessage sei;

    if (GST_VIDEO_FRAME_FLAG_IS_SET (frame, GST_VDP_H264_FRAME_GOT_PRIMARY))
      seq = GST_VDP_H264_FRAME_CAST (frame)->slice_hdr.picture->sequence;
    else
      seq = NULL;

    if (!gst_h264_parser_parse_sei_message (h264_dec->parser, seq, &sei, data,
            size))
      goto invalid_packet;
  }

  gst_buffer_unref (buf);
  return GST_FLOW_OK;

invalid_packet:
  GST_WARNING ("Invalid packet size!");
  gst_buffer_unref (buf);

  return GST_FLOW_OK;
}

static GstVideoFrame *
gst_vdp_h264_dec_create_frame (GstBaseVideoDecoder * base_video_decoder)
{
  return GST_VIDEO_FRAME_CAST (gst_vdp_h264_frame_new ());
}

static GstPad *
gst_vdp_h264_dec_create_srcpad (GstBaseVideoDecoder * base_video_decoder,
    GstBaseVideoDecoderClass * base_video_decoder_class)
{
  GstPadTemplate *pad_template;
  GstVdpVideoSrcPad *vdp_pad;

  pad_template = gst_element_class_get_pad_template
      (GST_ELEMENT_CLASS (base_video_decoder_class),
      GST_BASE_VIDEO_DECODER_SRC_NAME);

  vdp_pad = gst_vdp_video_src_pad_new (pad_template,
      GST_BASE_VIDEO_DECODER_SRC_NAME);

  return GST_PAD (vdp_pad);
}

static gboolean
gst_vdp_h264_dec_flush (GstBaseVideoDecoder * base_video_decoder)
{
  return TRUE;
}

static gboolean
gst_vdp_h264_dec_start (GstBaseVideoDecoder * base_video_decoder)
{
  GstVdpH264Dec *h264_dec = GST_VDP_H264_DEC (base_video_decoder);

  h264_dec->packetized = FALSE;
  h264_dec->nal_length_size = SYNC_CODE_SIZE;
  h264_dec->parser = g_object_new (GST_TYPE_H264_PARSER, NULL);

  return TRUE;
}

static gboolean
gst_vdp_h264_dec_stop (GstBaseVideoDecoder * base_video_decoder)
{
  GstVdpH264Dec *h264_dec = GST_VDP_H264_DEC (base_video_decoder);

  g_object_unref (h264_dec->parser);

  return TRUE;
}

static void
gst_vdp_h264_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  GstCaps *src_caps;
  GstPadTemplate *src_template;

  gst_element_class_set_details_simple (element_class,
      "VDPAU H264 Decoder",
      "Decoder",
      "Decode h264 stream with vdpau",
      "Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));

  src_caps = gst_vdp_video_buffer_get_caps (TRUE, VDP_CHROMA_TYPE_420);
  src_template = gst_pad_template_new (GST_BASE_VIDEO_DECODER_SRC_NAME,
      GST_PAD_SRC, GST_PAD_ALWAYS, src_caps);

  gst_element_class_add_pad_template (element_class, src_template);
}

static void
gst_vdp_h264_dec_init (GstVdpH264Dec * h264_dec, GstVdpH264DecClass * klass)
{
}

static void
gst_vdp_h264_dec_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vdp_h264_dec_class_init (GstVdpH264DecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseVideoDecoderClass *base_video_decoder_class =
      GST_BASE_VIDEO_DECODER_CLASS (klass);

  gobject_class->finalize = gst_vdp_h264_dec_finalize;

  base_video_decoder_class->start = gst_vdp_h264_dec_start;
  base_video_decoder_class->stop = gst_vdp_h264_dec_stop;
  base_video_decoder_class->flush = gst_vdp_h264_dec_flush;

  base_video_decoder_class->create_srcpad = gst_vdp_h264_dec_create_srcpad;
  base_video_decoder_class->set_sink_caps = gst_vdp_h264_dec_set_sink_caps;

  base_video_decoder_class->scan_for_sync = gst_vdp_h264_dec_scan_for_sync;
  base_video_decoder_class->scan_for_packet_end =
      gst_vdp_h264_dec_scan_for_packet_end;
  base_video_decoder_class->parse_data = gst_vdp_h264_dec_parse_data;

  base_video_decoder_class->handle_frame = gst_vdp_h264_dec_handle_frame;
  base_video_decoder_class->create_frame = gst_vdp_h264_dec_create_frame;

  base_video_decoder_class->shape_output = gst_vdp_h264_dec_shape_output;
}
