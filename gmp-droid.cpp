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
#include <cstring>
#include <map>
#include <stdlib.h>
#include <arpa/inet.h>

#include "droidmediacodec.h"

#include "gmp-platform.h"
#include "gmp-video-host.h"

#include "gmp-video-decode.h"
#include "gmp-video-frame-i420.h"
#include "gmp-video-frame-encoded.h"
#include "gmp-droid-conv.h"
#include "gmp-task-utils.h"

#define CRITICAL 0
#define ERROR 1
#define INFO  2
#define DEBUG 3

const char *kLogStrings[] = {
  "GMP-DROID Critical: ",
  "GMP-DROID Error: ",
  "GMP-DROID Info: ",
  "GMP-DROID Debug: "
};

static int g_log_level = INFO;

#define LOG(l, x) do { \
        if (l >=0 && l <= g_log_level) { \
            std::cerr << kLogStrings[l] << x << std::endl; \
        } \
    } while(0)

static GMPPlatformAPI *g_platform_api = nullptr;

// Droidmedia callbacks
static void DroidError (void *data, int err);   // pass error and abord
static int SizeChanged (void *data, int32_t width, int32_t height);     // reconfigure
static void DataAvailable (void *data, DroidMediaCodecData * decoded);
static void SignalEOS (void *data);

class DroidVideoDecoder : public GMPVideoDecoder
{
public:
  explicit DroidVideoDecoder (GMPVideoHost * hostAPI)
      : m_host (hostAPI)
  {
    // Create the Mutex
    GMPErr err = g_platform_api->createmutex (&m_codec_lock);
    if (GMP_FAILED (err))
        Error (err);
    err = g_platform_api->createmutex (&m_stop_lock);
    if (GMP_FAILED (err))
        Error (err);
    err = g_platform_api->createmutex (&m_drain_lock);
    if (GMP_FAILED (err))
        Error (err);
  }

  virtual ~DroidVideoDecoder ()
  {
    // Destroy the Mutex
    m_codec_lock->Destroy ();
    m_stop_lock->Destroy ();
    m_drain_lock->Destroy ();
  }

// GMPVideoDecoder methods
  virtual void InitDecode (const GMPVideoCodec & codecSettings,
      const uint8_t * aCodecSpecific,
      uint32_t aCodecSpecificSize,
      GMPVideoDecoderCallback * callback, int32_t coreCount)
  {

    m_callback = callback;

    // Check if this device supports the codec we want
    memset (&m_metadata, 0x0, sizeof (m_metadata));
    m_metadata.parent.flags =
        static_cast <DroidMediaCodecFlags> (DROID_MEDIA_CODEC_HW_ONLY | DROID_MEDIA_CODEC_NO_MEDIA_BUFFER);

    switch (codecSettings.mCodecType) {
      case kGMPVideoCodecVP8:
        m_metadata.parent.type = "video/x-vnd.on2.vp8";
        break;
      case kGMPVideoCodecVP9:
        m_metadata.parent.type = "video/x-vnd.on2.vp9";
        break;
      case kGMPVideoCodecH264:
        m_metadata.parent.type = "video/avc";
        break;
      default:
        LOG (ERROR, "Unknown GMP codec");
        Error (GMPNotImplementedErr);
        return;
    }

    // Check that the requested codec is actually available on this device
    if (!droid_media_codec_is_supported (&m_metadata.parent, false)) {
      LOG (ERROR, "Codec not supported");
      Error (GMPNotImplementedErr);
    }
    // Set codec parameters
    m_metadata.parent.width = codecSettings.mWidth;
    m_metadata.parent.height = codecSettings.mHeight;

    if (codecSettings.mMaxFramerate) {
      /* variable fps with a max-framerate */
      m_metadata.parent.fps = codecSettings.mMaxFramerate;
    }

    if (aCodecSpecificSize && codecSettings.mCodecType == kGMPVideoCodecH264) {
      // Copy AVCC data
      m_metadata.codec_data.size = aCodecSpecificSize - 1;
      m_metadata.codec_data.data = malloc (aCodecSpecificSize - 1);
      const GMPVideoCodecH264 *h264 =
          (const GMPVideoCodecH264 *) (aCodecSpecific);
      memcpy (m_metadata.codec_data.data, &h264->mAVCC, aCodecSpecificSize - 1);
      LOG (DEBUG, "Got H264 codec data size: " << (aCodecSpecificSize - 1));
    } else {
      m_metadata.codec_data.size = 0;
    }
    LOG (INFO,
        "InitDecode: Codec metadata prepared: " << m_metadata.parent.type
        << " width=" << m_metadata.parent.width
        << " height=" << m_metadata.parent.height
        << " fps=" << m_metadata.parent.fps
        << " extra=" << m_metadata.codec_data.size);
  }

  virtual void Decode (GMPVideoEncodedFrame * inputFrame,
      bool missingFrames,
      const uint8_t * aCodecSpecificInfo,
      uint32_t aCodecSpecificInfoLength, int64_t renderTimeMs = -1) {
    LOG (DEBUG, "Decode: frame size=" << inputFrame->Size ()
        << " timestamp=" << inputFrame->TimeStamp ()
        << " duration=" << inputFrame->Duration ()
        << " extra=" << aCodecSpecificInfoLength);

    DroidMediaBufferCallbacks cb;
    DroidMediaCodecData cdata;

    if (!strcmp (m_metadata.parent.type, "video/avc")
        && inputFrame->BufferType () != GMP_BufferSingle) {
      // H264: Replace each NAL length with the start code
      // The length is in network byte order, with size matching GMP_BufferLength
      uint32_t offset = 0;
      while (offset < inputFrame->Size ()) {
        // Get NAL length
        uint32_t len = 0;
        uint8_t *len8 = 0;
        uint32_t start_code_len = 0;

        switch (inputFrame->BufferType ()) {
          case GMP_BufferLength32:
            len =
                ntohl (*(reinterpret_cast <int32_t *>(inputFrame->Buffer () + offset)));
            start_code_len = 4;
            break;

          case GMP_BufferLength16:
            len =
                ntohs (*(reinterpret_cast <int16_t *>(inputFrame->Buffer () + offset)));
            start_code_len = 2;
            break;

          case GMP_BufferLength8:
            len8 = (inputFrame->Buffer () + offset);
            len = *len8;
            start_code_len = 1;
            break;

          case GMP_BufferLength24:
          case GMP_BufferInvalid:
          default:
            LOG (ERROR, "Unsupported H264 buffer size");
            Error (GMPDecodeErr);
            return;
        }


        if (len == 1) {
          // Start code already processed
          LOG (DEBUG, "NAL start code found. Skipping");
          break;
        } else if (offset + len + 4 > inputFrame->Size ()) {
          // Make sure that we won't run out of space in the buffer
          LOG (DEBUG,
              "NAL length more than buffer size: " << len << " bytes");
          break;
        } else {
            // Write NAL start code over the length
          static const uint8_t code[] = { 0x00, 0x00, 0x00, 0x01 };
          const uint8_t *start_code = code + (4 - start_code_len);
          memcpy (inputFrame->Buffer () + offset, start_code, start_code_len);
          offset += start_code_len + len;

          LOG (DEBUG, "Parsed nal unit of size " << len);
        }
      }
    }

    cdata.data.size = inputFrame->Size ();
    cdata.data.data = malloc (inputFrame->Size ());
    memcpy (cdata.data.data, inputFrame->Buffer (), inputFrame->Size ());

    cb.data = cdata.data.data;
    cb.unref = free;

    cdata.ts = inputFrame->TimeStamp ();
    // Android doesn't pass duration through the codec - we'll have to keep it
    m_dur[cdata.ts] = inputFrame->Duration ();
    cdata.sync = inputFrame->FrameType () == kGMPKeyFrame;

    inputFrame->Destroy ();

    if (!m_submit_thread) {
      GMPErr err = g_platform_api->createthread (&m_submit_thread);
      if (err != GMPNoErr) {
        LOG (ERROR, "Couldn't create new thread");
        Error (GMPGenericErr);
        return;
      }
    }
    // Queue new frame submission to codec in another thread so we don't block.
    m_submit_thread->Post (WrapTask (this,
            &DroidVideoDecoder::SubmitBufferThread, cdata, cb));
  }

  void SubmitBufferThread (DroidMediaCodecData cdata,
      DroidMediaBufferCallbacks cb)
  {
    m_drain_lock->Acquire ();
    if (m_draining || (!m_codec && !CreateCodec ())) {
      LOG (ERROR, "Buffer submitted while draining");
      cb.unref (cb.data);
      m_drain_lock->Release ();
      return;
    }
    m_drain_lock->Release ();

    // This blocks when the input Source is full
    droid_media_codec_queue (m_codec, &cdata, &cb);

    m_drain_lock->Acquire ();
    if (!m_draining && m_callback && g_platform_api) {
      g_platform_api->runonmainthread (WrapTask (m_callback,
              &GMPVideoDecoderCallback::InputDataExhausted));
    }
    m_drain_lock->Release ();
  }

  virtual void Reset ()
  {
    m_stop_lock->Acquire ();
    if (m_resetting) {
      m_stop_lock->Release ();
      return;
    }
    m_resetting = true;
    m_stop_lock->Release ();

    if (m_codec) {
      ResetCodec ();
    }
    m_drain_lock->Acquire ();
    m_draining = false;
    m_drain_lock->Release ();
    m_resetting = false;
    m_callback->ResetComplete ();
  }

  virtual void Drain ()
  {
    if (m_codec) {
      droid_media_codec_drain (m_codec);
    }

    //TODO: This never happens because the codec never really drains, except for EOS
    m_drain_lock->Acquire ();
    if (!m_codec || m_dur.size () == 0) {
      m_callback->DrainComplete ();
      m_draining = false;
    } else {
      m_draining = true;
    }
    m_drain_lock->Release ();
  }

  virtual void DecodingComplete ()
  {
    m_callback = nullptr;
    m_host = nullptr;
    ResetCodec ();
  }

  bool CreateCodec ()
  {
    m_codec_lock->Acquire ();
    m_codec = droid_media_codec_create_decoder (&m_metadata);

    if (!m_codec) {
      m_codec = nullptr;
      LOG (ERROR, "Failed to start the decoder");
      Error (GMPDecodeErr);
      return false;
    }
    LOG (INFO, "Codec created for " << m_metadata.parent.type);

    {
      DroidMediaCodecCallbacks cb;
      cb.error = DroidError;
      cb.size_changed = SizeChanged;
      cb.signal_eos = SignalEOS;
      droid_media_codec_set_callbacks (m_codec, &cb, this);
    }

    {
      DroidMediaCodecDataCallbacks cb;
      cb.data_available = DataAvailable;
      droid_media_codec_set_data_callbacks (m_codec, &cb, this);
    }
    // Reset state
    m_drain_lock->Acquire ();
    m_draining = false;
    m_drain_lock->Release ();

    if (!droid_media_codec_start (m_codec)) {
      droid_media_codec_destroy (m_codec);
      m_codec_lock->Release ();
      m_codec = nullptr;
      LOG (ERROR, "Failed to start the decoder");
      Error (GMPDecodeErr);
      return false;
    }
    m_codec_lock->Release ();
    LOG (DEBUG, "Codec started for " << m_metadata.parent.type);
    return true;
  }

  void ConfigureOutput (DroidMediaCodecData * data)
  {
    DroidMediaCodecMetaData md;
    DroidMediaRect rect;
    memset (&md, 0x0, sizeof (md));
    memset (&rect, 0x0, sizeof (rect));
    droid_media_codec_get_output_info (m_codec, &md, &rect);
    LOG (INFO,
        "ConfigureOutput: Configuring converter for stride:" << md.width
        << " slice-height: " << md.height << " top: " << rect.top
        << " left:" << rect.left << " width: " << rect.right - rect.left
        << " height: " << rect.bottom - rect.top << " format: " << md.hal_format);
    const char *convName;
    m_conv = DroidColourConvert::GetConverter (&md, &rect, &convName);
    LOG (INFO, "Colour converter class: " << convName);
  }

  void RequestNewConverter ()
  {
    LOG (DEBUG, "Resetting converter");
    m_dropConverter = true;
  }

  void ResetCodec ()
  {
    if (m_codec) {
      LOG (DEBUG, "Codec draining");
      droid_media_codec_drain (m_codec);
    }

    LOG (DEBUG, "Stopping submit thread");
    if (m_submit_thread) {
      m_submit_thread->Join ();
      m_submit_thread = nullptr;
    }
    LOG (DEBUG, "Stopped submit thread");
    m_codec_lock->Acquire ();
    if (m_codec) {
      LOG (DEBUG, "Codec stopping");
      droid_media_codec_stop (m_codec);
      LOG (DEBUG, "Destroying codec");
      droid_media_codec_destroy (m_codec);
      LOG (DEBUG, "Codec destroyed");
      m_codec = nullptr;
    }

    m_dur.clear ();
    RequestNewConverter ();
    m_codec_lock->Release ();
  }

  void ProcessFrameLock (DroidMediaCodecData * decoded)
  {
    m_stop_lock->Acquire ();
    if (m_resetting) {
      LOG (ERROR, "Received decoded frame while resetting codec");
      m_stop_lock->Release ();
      return;
    }

    if (g_platform_api) {
      g_platform_api->syncrunonmainthread (WrapTask (this,
              &DroidVideoDecoder::ProcessFrame, decoded));
    }
    m_stop_lock->Release ();
  }

  // Return the decoded data back to the parent.
  void ProcessFrame (DroidMediaCodecData * data)
  {
    // Delete the current colour converter if requested
    if (m_dropConverter) {
      if (m_conv)
        delete m_conv;
      m_conv = nullptr;
      m_dropConverter = false;
    }

    if (m_resetting || !m_callback || !m_host) {
        LOG(INFO, "Discarding decoded frame received while resetting");
        return;
    }

    if (!m_conv) {
      ConfigureOutput (data);
    }
    // Bail out if that didn't work
    if (!m_conv) {
      LOG (CRITICAL, "Converter not found");
      Error (GMPDecodeErr);
      return;
    }

    GMPVideoFrame *ftmp = nullptr;

    // Create new I420 frame
    GMPErr err = m_host->CreateFrame (kGMPI420VideoFrame, &ftmp);
    if (err != GMPNoErr) {
      LOG (ERROR, "Couldn't allocate empty I420 frame");
      Error (err);
      return;
    }
    // Fill it with the converter
    GMPVideoi420Frame *frame = static_cast <GMPVideoi420Frame *>(ftmp);
    err = m_conv->Convert (m_host, &data->data, frame);
    if (err != GMPNoErr) {
      LOG (ERROR, "Couldn't make decoded frame");
      Error (err);
      return;
    }
    // Set timestamp
    int64_t ts = data->ts / 1000;
    frame->SetTimestamp (ts);

    // Look up duration in our cache
    uint64_t dur = 0;
    std::map <int64_t, uint64_t>::iterator durIt = m_dur.find (ts);
    if (durIt != m_dur.end ()) {
      dur = durIt->second;
      m_dur.erase (durIt);
    }
    frame->SetDuration (dur);

    // Send the new frame back to Gecko
    m_callback->Decoded (frame);
    LOG (DEBUG, "ProcessFrame: Returning frame ts: " << ts << " dur: " << dur);
    m_drain_lock->Acquire ();
    if (m_dur.size () == 0 && m_draining) {
      // TODO: we never get the buffers down to 0 with the current SimpleDecodingSource, but EOS will do it
      m_callback->DrainComplete ();
      m_draining = false;
    } else {
      LOG (DEBUG, "Buffers still out " << m_dur.size ());
    }
    m_drain_lock->Release ();
  }

  virtual void EOS ()
  {
    LOG (DEBUG, "Codec EOS");
    if (g_platform_api) {
      g_platform_api->runonmainthread (WrapTask (m_callback,
              &GMPVideoDecoderCallback::DrainComplete));
    }
    m_dur.clear ();
  }

  void Error (GMPErr error)
  {
    if (m_callback && g_platform_api) {
      g_platform_api->runonmainthread (WrapTask (m_callback,
              &GMPVideoDecoderCallback::Error, error));
    }
  }

private:
  GMPVideoHost *m_host;
  GMPVideoDecoderCallback *m_callback = nullptr;
  // Codec lock makes sure that the codec isn't recreated while it's being destroyed
  GMPMutex *m_codec_lock = nullptr;
  // Stop lock prevents a deadlock when droid_media_codec_loop can't quit during
  // shutdown because it's waiting to get a frame processed on the main thread.
  GMPMutex *m_stop_lock = nullptr;
  // Drain lock protects the m_draining flag
  GMPMutex *m_drain_lock = nullptr;
  GMPThread *m_submit_thread = nullptr;
  DroidMediaCodecDecoderMetaData m_metadata;
  DroidMediaCodec *m_codec = nullptr;
  DroidColourConvert *m_conv = nullptr;
  bool m_dropConverter = false;
  bool m_draining = false;
  bool m_resetting = false;
  std::map <int64_t, uint64_t> m_dur;
};

/*
 * Droidmedia callbacks
 */
static void
DataAvailable (void *data, DroidMediaCodecData * decoded)
{
  DroidVideoDecoder *decoder = (DroidVideoDecoder *) data;
  LOG (DEBUG, "Received decoded frame");
  decoder->ProcessFrameLock (decoded);
}

static int
SizeChanged (void *data, int32_t width, int32_t height)
{
  DroidVideoDecoder *decoder = (DroidVideoDecoder *) data;
  LOG (INFO, "Received size changed " << width << " x " << height);
  decoder->RequestNewConverter ();
  return 0;
}

static void
DroidError (void *data, int err)
{
  DroidVideoDecoder *decoder = (DroidVideoDecoder *) data;
  LOG (ERROR, "Droidmedia error");
  if (g_platform_api)
    g_platform_api->runonmainthread (WrapTask (decoder,
            &DroidVideoDecoder::Error, GMPDecodeErr));
}

static void
SignalEOS (void *data)
{
  DroidVideoDecoder *decoder = (DroidVideoDecoder *) data;
  decoder->EOS ();
}

/*
 * GMP Initialization functions
 */
extern "C"
{

GMPErr GMPInit (GMPPlatformAPI * platformAPI)
{
  LOG (DEBUG, "Initializing droidmedia!");
  g_platform_api = platformAPI;
  droid_media_init ();
  return GMPNoErr;
}

GMPErr GMPGetAPI (const char *apiName, void *hostAPI, void **pluginApi)
{
  if (!strcmp (apiName, "decode-video")) {
    *pluginApi = new DroidVideoDecoder (static_cast <GMPVideoHost *>(hostAPI));
    return GMPNoErr;
  }
  return GMPGenericErr;
}

void GMPShutdown (void)
{
  LOG (DEBUG, "Shutting down droidmedia!");
  droid_media_deinit ();
  g_platform_api = nullptr;
}

}
