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
#include <vector>

using namespace std;

typedef struct {
  std::string androidName;
  std::string gmpName;
} codec_desc_t;

bool
isSupported (const codec_desc_t& codec, bool isEncoder)
{
  DroidMediaCodecMetaData meta;
  meta.type = codec.androidName.c_str ();
  meta.flags = static_cast <DroidMediaCodecFlags> (DROID_MEDIA_CODEC_HW_ONLY);
  return droid_media_codec_is_supported (&meta, isEncoder);
}

bool
isSupportedDecoder (const codec_desc_t& codec)
{
  return isSupported (codec, false);
}

bool
isSupportedEncoder (const codec_desc_t& codec)
{
  return isSupported (codec, true);
}

void
printSupportedApi (std::string api, std::vector<std::string> codecs)
{
  bool first = true;

  cout << api << "[";

  for (std::string codec : codecs) {
    if (!first)
      cout << ":";
    else
      first = false;

    cout << codec;
  }

  cout << "]";
}

int
main (int argc, char **argv)
{
  std::vector<codec_desc_t> codecs = {
    { "video/avc", "h264" },
    { "video/x-vnd.on2.vp8", "vp8" },
    { "video/x-vnd.on2.vp9", "vp9" }
  };
  std::vector<std::string> supportedDecoders;
  std::vector<std::string> supportedEncoders;

  for (codec_desc_t codec : codecs) {
    if (isSupportedDecoder (codec))
      supportedDecoders.push_back (codec.gmpName);
    if (isSupportedEncoder (codec))
      supportedEncoders.push_back (codec.gmpName);
  }

  cout << "Name: gmp-droid\n"
       << "Description: gst-droid GMP plugin for Gecko\n"
       << "Version: 0.1\n";
 
  cout << "APIs: ";
  printSupportedApi ("decode-video", supportedDecoders);
  cout << ", ";
  printSupportedApi ("encode-video", supportedEncoders);
  cout << "\n";
}
