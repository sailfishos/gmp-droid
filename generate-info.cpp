/****************************************************************************
**
** Copyright (c) 2020 Open Mobile Platform LLC.
**
** This Source Code Form is subject to the terms of the
** Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
** with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
**
****************************************************************************/

#include "droidmediacodec.h"
#include <iostream>

using namespace std;

bool
isSupported (const char *codec)
{
  DroidMediaCodecMetaData meta;
  meta.type = codec;
  meta.flags = static_cast <DroidMediaCodecFlags> (DROID_MEDIA_CODEC_HW_ONLY);
  return droid_media_codec_is_supported (&meta, false);
}

int
main (int argc, char **argv)
{

  cout << "Name: gmp-droid\n" << "Description: gst-droid GMP plugin for Gecko\n"
      << "Version: 0.1\n" << "APIs: decode-video[";

  bool first = true;
  if (isSupported ("video/avc")) {
    cout << "h264";
    first = false;
  }
  if (isSupported ("video/x-vnd.on2.vp8")) {
    if (!first)
      cout << ":";
    else
      first = false;

    cout << "vp8";
  }
  if (isSupported ("video/x-vnd.on2.vp9")) {
    if (!first)
      cout << ":";
    else
      first = false;

    cout << "vp9";
  }
  cout << "]\n";
}
