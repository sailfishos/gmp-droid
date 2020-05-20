/****************************************************************************
**
** Copyright (c) 2020 Open Mobile Platform LLC.
**
** This Source Code Form is subject to the terms of the
** Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
** with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
**
****************************************************************************/

#ifndef GMP_DROID_CONV
#define GMP_DROID_CONV

#include "gmp-video-host.h"
#include "gmp-video-frame-i420.h"
#include "droidmediacodec.h"
#include "droidmediaconvert.h"
#include "droidmediaconstants.h"

class DroidColourConvert
{
public:
  virtual ~DroidColourConvert () { }

  virtual GMPErr Convert (GMPVideoHost * host, DroidMediaData * in,
      GMPVideoi420Frame * out) = 0;

  static DroidColourConvert *GetConverter (DroidMediaCodecMetaData * md,
      DroidMediaRect * rect, const char **conv_name);

  virtual void SetFormat (DroidMediaRect * rect, int32_t width, int32_t height)
  {
    m_stride = width;
    m_slice_height = height;
    m_top = rect->top;
    m_left = rect->left;
    m_width = rect->right - rect->left;
    m_height = rect->bottom - rect->top;
  }

  int32_t m_stride = 0;
  int32_t m_slice_height = 0;
  int32_t m_width = 0;
  int32_t m_height = 0;
  int32_t m_top = 0;
  int32_t m_left = 0;

};

#endif
