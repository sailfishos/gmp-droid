/****************************************************************************
**
** Copyright (c) 2020-2021 Open Mobile Platform LLC.
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
#include "gmp-video-encode.h"
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

    if (m_resetting) {
      LOG (INFO, "Buffer submitted while resetting");
      return;
    }

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

    if (m_processing) {
      // Reset() will be called from DataAvailable() later
      LOG (INFO, "Reset while m_processing");
      m_stop_lock->Release ();
      return;
    }

    if (g_platform_api) {
      // Reset() was called. Execute it on main thread
      g_platform_api->runonmainthread (WrapTask (this,
              &DroidVideoDecoder::Reset_m));
    }
    m_stop_lock->Release ();
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
    m_resetting = true;

    if (g_platform_api) {
      // Reset() was called. Execute it on main thread
      g_platform_api->runonmainthread (WrapTask (this,
              &DroidVideoDecoder::Reset_m));
    }
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
      cb.error = DroidVideoDecoder::DroidError;
      cb.size_changed = DroidVideoDecoder::SizeChanged;
      cb.signal_eos = DroidVideoDecoder::SignalEOS;
      droid_media_codec_set_callbacks (m_codec, &cb, this);
    }

    {
      DroidMediaCodecDataCallbacks cb;
      cb.data_available = DroidVideoDecoder::DataAvailable;
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

  void ProcessFrame (DroidMediaCodecData * decoded)
  {
    m_stop_lock->Acquire ();

    // Delete the current colour converter if requested
    if (m_dropConverter) {
      if (m_conv)
        delete m_conv;
      m_conv = nullptr;
      m_dropConverter = false;
    }

    if (m_resetting || !m_callback || !m_host) {
        LOG(INFO, "Discarding decoded frame received while resetting");
        m_stop_lock->Release ();
        return;
    }

    m_processing = true;
    m_stop_lock->Release ();

    if (g_platform_api) {
      g_platform_api->syncrunonmainthread (WrapTask (this,
              &DroidVideoDecoder::ProcessFrame_m, decoded));
    }

    m_stop_lock->Acquire ();
    m_processing = false;
    if (m_resetting && g_platform_api) {
      // Reset() was called. Execute it on main thread
      g_platform_api->runonmainthread (WrapTask (this,
              &DroidVideoDecoder::Reset_m));
    }
    m_stop_lock->Release ();

  }

  // Return the decoded data back to the parent.
  void ProcessFrame_m (DroidMediaCodecData * data)
  {
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

  virtual void Reset_m ()
  {
    LOG (DEBUG, "Reset_m");
    if (m_codec) {
      ResetCodec ();
    }
    m_drain_lock->Acquire ();
    m_draining = false;
    m_drain_lock->Release ();
    m_resetting = false;
    if (m_callback) {
      m_callback->ResetComplete ();
    }
  }

  /*
   * Droidmedia callbacks
   */
  static void
  DataAvailable (void *data, DroidMediaCodecData * decoded)
  {
    DroidVideoDecoder *decoder = (DroidVideoDecoder *) data;
    decoder->ProcessFrame (decoded);
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
  bool m_processing = false;
  std::map <int64_t, uint64_t> m_dur;
};

class DroidVideoEncoder : public GMPVideoEncoder
{
public:
  explicit DroidVideoEncoder (GMPVideoHost * hostAPI)
      : m_host (hostAPI)
  {
    GMPErr err = g_platform_api->createmutex (&m_stop_lock);
    if (GMP_FAILED (err))
        Error (err);
  }

  virtual ~DroidVideoEncoder ()
  {
    m_stop_lock->Destroy ();
  }

  void InitEncode (const GMPVideoCodec& codecSettings,
      const uint8_t* aCodecSpecific,
      uint32_t aCodecSpecificSize,
      GMPVideoEncoderCallback* callback,
      int32_t aNumberOfCores,
      uint32_t aMaxPayloadSize)
  {
    LOG (DEBUG, "Init encode aCodecSpecificSize:" << aCodecSpecificSize
        << " aNumberOfCores:" << aNumberOfCores
        << " aMaxPayloadSize:" << aMaxPayloadSize);
    m_callback = callback;

    // Check if this device supports the codec we want
    memset (&m_metadata, 0x0, sizeof (m_metadata));
    m_metadata.parent.flags =
        static_cast <DroidMediaCodecFlags> (DROID_MEDIA_CODEC_HW_ONLY);

    m_codecType = codecSettings.mCodecType;

    switch (m_codecType) {
      case kGMPVideoCodecVP8:
        m_metadata.parent.type = "video/x-vnd.on2.vp8";
        break;
      case kGMPVideoCodecVP9:
        m_metadata.parent.type = "video/x-vnd.on2.vp9";
        break;
      case kGMPVideoCodecH264:
        m_metadata.parent.type = "video/avc";
        // TODO: Some devices may not support this feature. A workaround is
        // to save AVCC data and put it before every IDR manually.
        m_metadata.codec_specific.h264.prepend_header_to_sync_frames = true;
        break;
      default:
        LOG (ERROR, "Unknown GMP codec");
        Error (GMPNotImplementedErr);
        return;
    }

    // Check that the requested encoder is actually available on this device
    if (!droid_media_codec_is_supported (&m_metadata.parent, true)) {
      LOG (ERROR, "Codec not supported: " << m_metadata.parent.type);
      Error (GMPNotImplementedErr);
      return;
    }
    // Set codec parameters
    m_metadata.parent.width = codecSettings.mWidth;
    m_metadata.parent.height = codecSettings.mHeight;

    if (codecSettings.mMaxFramerate) {
      m_metadata.parent.fps = codecSettings.mMaxFramerate;
    }

    m_metadata.bitrate = codecSettings.mStartBitrate * 1024;
    m_metadata.stride = codecSettings.mWidth;
    m_metadata.slice_height = codecSettings.mHeight;
    m_metadata.meta_data = false;

    droid_media_colour_format_constants_init (&m_constants);
    m_metadata.color_format = -1;

    {
      uint32_t supportedFormats[32];
      unsigned int nFormats = droid_media_codec_get_supported_color_formats (
          &m_metadata.parent, 1, supportedFormats, 32);

      LOG (INFO, "Found " << nFormats << " color formats supported:");
      for (unsigned int i = 0; i < nFormats; i++) {
        int fmt = static_cast<int>(supportedFormats[i]);
        LOG (INFO, "  " << std::hex << fmt << std::dec);
        // The list of formats is sorted in order of codec's preference,
        // so pick the first one supported.
        if (m_metadata.color_format == -1 &&
            (fmt == m_constants.OMX_COLOR_FormatYUV420Planar ||
             fmt == m_constants.OMX_COLOR_FormatYUV420SemiPlanar)) {
          m_metadata.color_format = fmt;
        }
      }
    }

    if (m_metadata.color_format == -1) {
      LOG (ERROR, "No supported color format found");
      Error (GMPNotImplementedErr);
      return;
    }

    LOG (INFO,
        "InitEncode: Codec metadata prepared: " << m_metadata.parent.type
        << " width=" << m_metadata.parent.width
        << " height=" << m_metadata.parent.height
        << " fps=" << m_metadata.parent.fps
        << " bitrate=" << m_metadata.bitrate
        << " color_format=" << m_metadata.color_format);
  }

  void Encode (GMPVideoi420Frame* inputFrame,
      const uint8_t* codecSpecificInfo,
      uint32_t codecSpecificInfoLength,
      const GMPVideoFrameType* frameTypes,
      uint32_t frameTypesLength)
  {
    LOG (DEBUG, "Encode:"
        << " timestamp=" << inputFrame->Timestamp ()
        << " duration=" << inputFrame->Duration ()
        << " extra=" << codecSpecificInfoLength
        << " frameTypesLength=" << frameTypesLength
        << " frameType[0]=" << frameTypes[0]);

    DroidMediaCodecData data;
    DroidMediaBufferCallbacks cb;

    if (!m_codec && !CreateEncoder ()) {
      LOG (ERROR, "Cannot create encoder");
      return;
    }

    // Copy the frame to contiguous memory buffer
    const unsigned y_size = inputFrame->Width() * inputFrame->Height();
    const unsigned u_size = y_size / 4;
    const unsigned v_size = y_size / 4;
    uint8_t *buf;

    LOG (DEBUG, "plane sizes: " << y_size
        << " " << u_size
        << " " << v_size
        << " timestamp: " << inputFrame->Timestamp()
        << " sync: " << (frameTypes[0] == kGMPKeyFrame));

    buf = (uint8_t *)malloc (y_size + u_size + v_size);
    data.data.data = buf;
    data.data.size = y_size + u_size + v_size;

    memcpy(buf, inputFrame->Buffer(kGMPYPlane), y_size);
    buf += y_size;
    if (m_metadata.color_format == m_constants.OMX_COLOR_FormatYUV420Planar) {
      memcpy(buf, inputFrame->Buffer(kGMPUPlane), u_size);
      buf += u_size;
      memcpy(buf, inputFrame->Buffer(kGMPVPlane), v_size);
    } else {
      uint8_t *inpU = inputFrame->Buffer(kGMPUPlane);
      uint8_t *inpV = inputFrame->Buffer(kGMPVPlane);
      for (unsigned i = 0; i < u_size + v_size; i += 2) {
        buf[i] = *inpU++;
        buf[i + 1] = *inpV++;
      }
    }

    data.ts = inputFrame->Timestamp();
    data.sync = frameTypes[0] == kGMPKeyFrame;

    cb.unref = free;
    cb.data = data.data.data;

    droid_media_codec_queue (m_codec, &data, &cb);

    inputFrame->Destroy();
  }

  void SetChannelParameters(uint32_t aPacketLoss, uint32_t aRTT)
  {
      LOG (INFO, "SetChannelParameters: packetLoss:" << aPacketLoss << " RTT:" << aRTT);
  }

  void SetRates(uint32_t aNewBitRate, uint32_t aFrameRate)
  {
      LOG (INFO, "SetRates: newBitrate=" << aNewBitRate << " frameRate=" << aFrameRate);
  }

  void SetPeriodicKeyFrames(bool aEnable)
  {
      LOG (INFO, "SetPeriodicKeyFrames: enable=" << aEnable);
  }

  void EncodingComplete ()
  {
    // Do not try to stop the codec if it is hanging in data_available()
    m_stop_lock->Acquire();
    m_stopping = true;
    if (m_processing) {
      // EncodingComplete() will be called from DataAvailable() later
      m_stop_lock->Release();
      return;
    }
    m_stop_lock->Release();

    LOG (INFO, "EncodingComplete");
    droid_media_codec_stop(m_codec);
    droid_media_codec_destroy(m_codec);
    LOG (INFO, "EncodingComplete: Codec destroyed");
    m_stopping = false;
    m_codec = nullptr;
  }

  void Error (GMPErr error)
  {
    if (m_callback && g_platform_api) {
      g_platform_api->runonmainthread (WrapTask (m_callback,
              &GMPVideoEncoderCallback::Error, error));
    }
  }

private:
  GMPVideoHost *m_host;
  GMPVideoEncoderCallback *m_callback = nullptr;
  DroidMediaCodecEncoderMetaData m_metadata;
  DroidMediaCodec *m_codec = nullptr;
  GMPVideoCodecType m_codecType = kGMPVideoCodecInvalid;
  GMPMutex *m_stop_lock = nullptr;
  bool m_processing = false;
  bool m_stopping = false;
  DroidMediaColourFormatConstants m_constants;

  bool CreateEncoder ()
  {
    m_codec = droid_media_codec_create_encoder (&m_metadata);

    if (!m_codec) {
      LOG (ERROR, "Failed to create the encoder");
      Error (GMPEncodeErr);
      return false;
    }

    LOG (INFO, "Codec created for " << m_metadata.parent.type);

    {
      DroidMediaCodecCallbacks cb;
      cb.error = DroidVideoEncoder::DroidError;
      cb.signal_eos = DroidVideoEncoder::SignalEOS;
      droid_media_codec_set_callbacks (m_codec, &cb, this);
    }

    {
      DroidMediaCodecDataCallbacks cb;
      cb.data_available = DroidVideoEncoder::DataAvailableCallback;
      droid_media_codec_set_data_callbacks (m_codec, &cb, this);
    }

    LOG (DEBUG, "Starting the encoder..");
    int result = droid_media_codec_start (m_codec);
    if (result == 0) {
      droid_media_codec_stop (m_codec);
      droid_media_codec_destroy (m_codec);
      m_codec = nullptr;
      LOG (ERROR, "Failed to start the encoder!");
      Error (GMPEncodeErr);
      return false;
    }
    LOG (DEBUG, "Encoder started");
    return true;
  }

  // Called on a codec thread
  static void DataAvailableCallback (void *data, DroidMediaCodecData* encoded)
  {
    DroidVideoEncoder *encoder = (DroidVideoEncoder*) data;
    encoder->DataAvailable (data, encoded);
  }

  // Called on a codec thread
  void DataAvailable (void *data, DroidMediaCodecData* encoded)
  {
    m_stop_lock->Acquire();
    if (m_stopping) {
      LOG (ERROR, "DataAvailable() while m_stopping is set");
      m_stop_lock->Release();
      return;
    }

    m_processing = true;
    m_stop_lock->Release();

    if (g_platform_api)
      g_platform_api->syncrunonmainthread (WrapTask (this,
            &DroidVideoEncoder::FrameAvailable, encoded));

    m_stop_lock->Acquire();
    m_processing = false;
    if (m_stopping && g_platform_api) {
      // EncodingComplete() was called. Execute it on main thread.
      g_platform_api->runonmainthread (WrapTask (this,
              &DroidVideoEncoder::EncodingComplete));
    }
    m_stop_lock->Release();
  }

  void FrameAvailable (DroidMediaCodecData* encoded)
  {
    LOG (DEBUG, "Received encoded frame of length " << encoded->data.size
        << " ts " << encoded->ts
        << " sync " << encoded->sync
        << " codec_config " << encoded->codec_config);

    GMPVideoFrame* tmpFrame;
    GMPErr err = m_host->CreateFrame (kGMPEncodedVideoFrame, &tmpFrame);
    if (err != GMPNoErr) {
      LOG (ERROR, "Cannot create frame");
      return;
    }

    GMPVideoEncodedFrame* frame = static_cast<GMPVideoEncodedFrame*> (tmpFrame);
    err = frame->CreateEmptyFrame (encoded->data.size);
    if (err != GMPNoErr) {
      LOG (ERROR, "Cannot allocate memory");
      frame->Destroy();
      return;
    }

    // Copy encoded data to the output frame
    memcpy (frame->Buffer(), encoded->data.data, encoded->data.size);

    GMPBufferType bufferType = GMP_BufferSingle;

    frame->SetEncodedWidth (m_metadata.parent.width);
    frame->SetEncodedHeight (m_metadata.parent.height);
    frame->SetTimeStamp (encoded->ts / 1000); // Convert to usec
    frame->SetCompleteFrame (true);
    frame->SetFrameType (encoded->sync ? kGMPKeyFrame : kGMPDeltaFrame);

    GMPCodecSpecificInfo info;
    memset (&info, 0, sizeof (info));
    info.mCodecType = m_codecType;

    // Convert NAL Units. Gecko expects header in native byte order
    if (m_codecType == kGMPVideoCodecH264) {
      bufferType = GMP_BufferLength32; // FIXME: Can it change?
      info.mCodecSpecific.mH264.mSimulcastIdx = 0;

      ConvertNalUnits (frame->Buffer(), encoded->data.size, bufferType);
    }

    frame->SetBufferType (bufferType);
    info.mBufferType = bufferType;

    m_callback->Encoded (frame, reinterpret_cast<uint8_t*> (&info), sizeof (info));
  }

  static inline void UnalignedWrite32 (uint8_t *dest, uint32_t val)
  {
    dest[0] = val & 0xff;
    dest[1] = (val >> 8) & 0xff;
    dest[2] = (val >> 16) & 0xff;
    dest[3] = (val >> 24) & 0xff;
  }

  static void ConvertNalUnits (uint8_t *buf, size_t bufSize, GMPBufferType bufferType)
  {
    const uint8_t nalStartCode[] = {0, 0, 0, 1};
    uint8_t *p = buf, *end = buf + bufSize;
    uint8_t *prevNalStart = NULL, *nalStart = NULL;
    unsigned nalStartSize;

    switch (bufferType) {
      case GMP_BufferLength32:
        nalStartSize = 4;
        break;
      case GMP_BufferLength24:
        nalStartSize = 3;
        break;
      case GMP_BufferLength16:
        nalStartSize = 2;
        break;
      case GMP_BufferLength8:
        nalStartSize = 1;
        break;
      default:
        return;
    }

    while (p < end) {
      // NAL Unit start code found
      if (0 == memcmp (p, nalStartCode + (4 - nalStartSize), nalStartSize)) {
        prevNalStart = nalStart;
        nalStart = p;
        if (prevNalStart) {
          unsigned nalSize = p - prevNalStart - nalStartSize;
          UnalignedWrite32 (prevNalStart, nalSize);
          LOG (DEBUG, "found nal size: " << nalSize << " at " << prevNalStart - buf);
        }
        // Skip NALU Start code;
        p += nalStartSize;
        // VCL units are the last NALUs in the encoded chunk
        if ((p[0] & 0x1f) <= 5) {
          break;
        }
      }
      p += 1;
    }
    // Convert the last NALU
    if (nalStart) {
      unsigned nalSize = bufSize - (nalStart - buf) - nalStartSize;
      UnalignedWrite32 (nalStart, nalSize);
      LOG (DEBUG, "last nal size: " << nalSize << " at " << nalStart - buf);
    }
  }

  static void SignalEOS (void *data)
  {
    DroidVideoEncoder *encoder = (DroidVideoEncoder *) data;
    encoder->EOS ();
  }

  static void DroidError (void *data, int err)
  {
    DroidVideoEncoder *encoder = (DroidVideoEncoder *) data;
    LOG (ERROR, "Droidmedia encoder error " << err);
    if (g_platform_api)
      g_platform_api->runonmainthread (WrapTask (encoder,
            &DroidVideoEncoder::Error, GMPDecodeErr));
  }

  void EOS ()
  {
    LOG (INFO, "Encoder EOS");
  }
};

/*
 * GMP Initialization functions
 */
extern "C"
{

GMPErr GMPInit (GMPPlatformAPI * platformAPI)
{
  LOG (DEBUG, "Initializing droidmedia!");
  g_platform_api = platformAPI;
  if (droid_media_init ())
    return GMPNoErr;
  else
    return GMPNotImplementedErr;
}

GMPErr GMPGetAPI (const char *apiName, void *hostAPI, void **pluginApi)
{
  if (!strcmp (apiName, "decode-video")) {
    *pluginApi = new DroidVideoDecoder (static_cast <GMPVideoHost *>(hostAPI));
    return GMPNoErr;
  } else if (!strcmp (apiName, "encode-video")) {
    *pluginApi = new DroidVideoEncoder (static_cast <GMPVideoHost *>(hostAPI));
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
/* vim: set ts=2 et sw=2 tw=80: */
