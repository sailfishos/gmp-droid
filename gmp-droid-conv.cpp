/****************************************************************************
**
** Copyright (c) 2020 Open Mobile Platform LLC.
**
** This Source Code Form is subject to the terms of the
** Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
** with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
**
****************************************************************************/

#include <iostream>
#include <stdlib.h>

#include "gmp-droid-conv.h"
#include "gmp-video-frame-i420.h"
#include "gmp-video-frame-encoded.h"
#include "droidmediacodec.h"

class ConvertNative: public DroidColourConvert
{
private:
  DroidMediaConvert * m_convert;
public:
  ConvertNative (DroidMediaConvert * convert)
    : m_convert (convert) { }

  ~ConvertNative ()
  {
    droid_media_convert_destroy (m_convert);
  }

  GMPErr Convert (GMPVideoHost * host, DroidMediaData * in,
      GMPVideoi420Frame * out)
  {
    int32_t size = m_width * m_height;
    uint8_t *buf = (uint8_t *) malloc (size * 3 / 2);
    droid_media_convert_to_i420 (m_convert, in, buf);
    out->CreateFrame (size, buf,
        size / 4, buf + size,
        size / 4, buf + size + (size / 4),
        m_width, m_height, m_width, m_width / 2, m_width / 2);
    free (buf);
    return GMPNoErr;
  }

  void SetFormat (DroidMediaRect * rect, int32_t width, int32_t height)
  {
    this->DroidColourConvert::SetFormat (rect, width, height);
    droid_media_convert_set_crop_rect (m_convert, *rect, width, height);
  }

};

#define ALIGN_SIZE(size, to) (((size) + to  - 1) & ~(to - 1))

static void
CopyPackedPlanes (uint8_t * out0, uint8_t * out1, uint8_t * in, int32_t outSize)
{
  int x;
  uint8_t *place = in;
  for (x = 0; x < outSize; x++) {
    out0[x] = place[0];
    out1[x] = place[1];
    place += 2;
  }
}

class ConvertYUV420PackedSemiPlanar32m:public DroidColourConvert
{
public:
  GMPErr Convert (GMPVideoHost * host, DroidMediaData * in,
      GMPVideoi420Frame * out)
  {
    /* copy to the output buffer swapping the u and v planes and cropping if necessary */
    /* NV12 format with 128 byte alignment */

    uint8_t *y = (uint8_t *) in->data + (m_top * m_stride) + m_left;
    uint8_t *uv =
        (uint8_t *) in->data + (m_stride * m_slice_height) +
        (m_top * m_stride / 2) + m_left / 2;

    GMPPlane *outY, *outU, *outV;
    // Create plane buffers
    host->CreatePlane (&outY);
    host->CreatePlane (&outU);
    host->CreatePlane (&outV);
    // Copy Y directly
    outY->Copy (m_stride * m_height, m_stride, y);
    // U and V are packed, so we'll have to create empty buffers and copy manually
    outU->CreateEmptyPlane (m_stride * m_height / 4, m_stride / 2,
        m_stride * m_height / 4);
    outV->CreateEmptyPlane (m_stride * m_height / 4, m_stride / 2,
        m_stride * m_height / 4);
    CopyPackedPlanes (outU->Buffer (), outV->Buffer (), uv,
        m_stride * m_height / 4);
    // Create Frame from the plane buffers to return
    out->CreateFrame (m_stride * m_height, outY->Buffer (),
        m_stride * m_height / 4, outU->Buffer (),
        m_stride * m_height / 4, outV->Buffer (),
        m_width, m_height, m_stride, m_stride / 2, m_stride / 2);
    // Destroy the planes
    outY->Destroy ();
    outU->Destroy ();
    outV->Destroy ();
    return GMPNoErr;
  }

  void SetFormat (DroidMediaRect * rect, int32_t width, int32_t height)
  {
    this->DroidColourConvert::SetFormat (rect, width, height);
    m_stride = ALIGN_SIZE (m_stride, 128);
    m_slice_height = ALIGN_SIZE (m_slice_height, 32);
    m_top = ALIGN_SIZE (m_top, 2);
    m_left = ALIGN_SIZE (m_left, 2);
  }
};

class ConvertYUV420Planar:public DroidColourConvert
{
public:
  GMPErr Convert (GMPVideoHost * host, DroidMediaData * in,
      GMPVideoi420Frame * out)
  {
    /* Buffer is already I420, so we can copy it straight over */
    /* though we need to handle the cropping using stride and an offset */

    uint8_t *y = (uint8_t *) in->data + (m_top * m_stride) + m_left;
    uint8_t *u = (uint8_t *) in->data + (m_stride * m_slice_height) +
        (m_top * m_stride / 2) + (m_left / 2);
    uint8_t *v = (uint8_t *) in->data + (m_stride * m_slice_height) +
        (m_stride * m_slice_height / 4)
        + (m_top * m_stride / 2) + (m_left / 2);
    // Create plane buffers
    GMPPlane *outY, *outU, *outV;
    host->CreatePlane (&outY);
    host->CreatePlane (&outU);
    host->CreatePlane (&outV);
    // Copy all buffers directly
    outY->Copy (m_stride * m_height, m_stride, y);
    outU->Copy (m_stride * m_height / 4, m_stride / 2, u);
    outV->Copy (m_stride * m_height / 4, m_stride / 2, v);
    // Create Frame from the plane buffers to return
    out->CreateFrame (m_stride * m_height, outY->Buffer (),
        m_stride * m_height / 4, outU->Buffer (),
        m_stride * m_height / 4, outV->Buffer (),
        m_width, m_height, m_stride, m_stride / 2, m_stride / 2);
    // Destroy the planes
    outY->Destroy ();
    outU->Destroy ();
    outV->Destroy ();
    return GMPNoErr;
  }

  void SetFormat (DroidMediaRect * rect, int32_t width, int32_t height)
  {
    this->DroidColourConvert::SetFormat (rect, width, height);
    m_stride = ALIGN_SIZE (width, 4);
  }
};

class ConvertYUV420SemiPlanar:public DroidColourConvert
{
public:
  GMPErr Convert (GMPVideoHost * host, DroidMediaData * in,
      GMPVideoi420Frame * out)
  {
    uint8_t *y = (uint8_t *) in->data + (m_top * m_stride) + m_left;
    uint8_t *uv = (uint8_t *) in->data + (m_stride * m_slice_height) +
        (m_top * m_stride / 2) + m_left / 2;
    // Create plane buffers
    GMPPlane *outY, *outU, *outV;
    host->CreatePlane (&outY);
    host->CreatePlane (&outU);
    host->CreatePlane (&outV);
    // Copy Y directly
    outY->Copy (m_stride * m_height, m_stride, y);
    // U and V are packed, so we'll have to create empty buffers and copy manually
    outU->CreateEmptyPlane (m_stride * m_height, m_stride / 2,
        m_stride * m_height / 4);
    outV->CreateEmptyPlane (m_stride * m_height, m_stride / 2,
        m_stride * m_height / 4);
    CopyPackedPlanes (outU->Buffer (), outV->Buffer (), uv,
        m_stride * m_height / 4);
    // Create Frame from the plane buffers to return
    out->CreateFrame (m_stride * m_height, outY->Buffer (),
        m_stride * m_height / 4, outU->Buffer (),
        m_stride * m_height / 4, outV->Buffer (),
        m_width, m_height, m_stride, m_stride / 2, m_stride / 2);
    // Destroy the planes
    outY->Destroy ();
    outU->Destroy ();
    outV->Destroy ();
    return GMPNoErr;
  }

  void SetFormat (DroidMediaRect * rect, int32_t width, int32_t height)
  {
    this->DroidColourConvert::SetFormat (rect, width, height);
    m_stride = ALIGN_SIZE (m_stride, 16);
  }
};

DroidColourConvert *
DroidColourConvert::GetConverter (DroidMediaCodecMetaData * md,
    DroidMediaRect * rect, const char **conv_name)
{
  DroidColourConvert *converter;
  *conv_name = "None";
  DroidMediaConvert *droidConvert = droid_media_convert_create ();
  if (droidConvert) {
    //TODO: Check DONT_USE_DROID_CONVERT_VALUE quirk. May not be needed.
    converter = new ConvertNative (droidConvert);
    *conv_name = "ConvertNative";
  } else {
    DroidMediaColourFormatConstants constants;
    droid_media_colour_format_constants_init (&constants);

    if (md->hal_format == constants.QOMX_COLOR_FormatYUV420PackedSemiPlanar32m) {
      converter = new ConvertYUV420PackedSemiPlanar32m ();
      *conv_name = "ConvertYUV420PackedSemiPlanar32m";
    } else if (md->hal_format == constants.OMX_COLOR_FormatYUV420Planar) {
      converter = new ConvertYUV420Planar ();
      *conv_name = "ConvertYUV420Planar";
    } else if (md->hal_format == constants.OMX_COLOR_FormatYUV420SemiPlanar) {
      converter = new ConvertYUV420SemiPlanar ();
      *conv_name = "ConvertYUV420SemiPlanar";
    } else {
      return nullptr;
    }
  }

  //TODO: DONT_USE_CODEC_SPECIFIED_HEIGHT/WIDTH quirks (if a device needs this)
  converter->SetFormat (rect, md->width, md->height);
  return converter;
}
