/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "threads/SingleLock.h"
#include "DVDPlayerAudio.h"
#include "DVDPlayer.h"
#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/DVDCodecs.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "settings/Settings.h"
#include "video/VideoReferenceClock.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "utils/MathUtils.h"
#include "cores/AudioEngine/AEFactory.h"
#include "cores/AudioEngine/Utils/AEUtil.h"

#include <sstream>
#include <iomanip>
#include <math.h>

/* for sync-based resampling */
#define PROPORTIONAL 20.0
#define PROPREF       0.01
#define PROPDIVMIN    2.0
#define PROPDIVMAX   40.0
#define INTEGRAL    200.0

using namespace std;

void CPTSInputQueue::Add(int64_t bytes, double pts)
{
  CSingleLock lock(m_sync);

  m_list.insert(m_list.begin(), make_pair(bytes, pts));
}

void CPTSInputQueue::Flush()
{
  CSingleLock lock(m_sync);

  m_list.clear();
}
double CPTSInputQueue::Get(int64_t bytes, bool consume)
{
  CSingleLock lock(m_sync);

  IT it = m_list.begin();
  for(; it != m_list.end(); ++it)
  {
    if(bytes <= it->first)
    {
      double pts = it->second;
      if(consume)
      {
        it->second = DVD_NOPTS_VALUE;
        m_list.erase(++it, m_list.end());
      }
      return pts;
    }
    bytes -= it->first;
  }
  return DVD_NOPTS_VALUE;
}


class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo &hints, CDVDAudioCodec* codec, CDVDAudioCodec* codec2)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_codec2(codec2)
    , m_hints(hints)
  {}
 ~CDVDMsgAudioCodecChange()
  {
    if (m_codec)
      delete m_codec;
    if (m_codec2)
      delete m_codec2;
  }
  CDVDAudioCodec* m_codec;
  CDVDAudioCodec* m_codec2;
  CDVDStreamInfo  m_hints;
};

CAudio2Frames::CAudio2Frames()
{
  incr = 64*1024;
  capa = incr;
  data = (uint8_t*)malloc(capa);
  size = 0;
}

CAudio2Frames::~CAudio2Frames()
{
  if(data)
    free(data);
}

void CAudio2Frames::Add(DVDAudioFrame& af)
{
  if(!af.data || !af.size)
    return;
  if(size + af.size > capa)
  {
    capa = ((size + af.size) / incr + 1) * incr;
    data = (uint8_t*)realloc(data, capa);
  }
  memcpy(data+size, af.data, af.size);
  size += af.size;
  af.data = data + size;
  afs.push_back(af);
}

bool CAudio2Frames::Merge(DVDAudioFrame& af)
{
  if (!afs.size())
    return false;
  af = afs.front();
  af.data = data;
  af.size = size;
  af.duration = 0;
  for (std::list<DVDAudioFrame>::iterator it = afs.begin(); it != afs.end(); ++it)
    af.duration += it->duration;
  return true;
}

void CAudio2Frames::Clear()
{
  afs.clear();
  size = 0;
}


CDVDPlayerAudio::CDVDPlayerAudio(CDVDClock* pClock, CDVDMessageQueue& parent)
: CThread("DVDPlayerAudio")
, m_messageQueue("audio")
, m_messageParent(parent)
, m_dvdAudio((bool&)m_bStop)
, m_dvdAudio2((bool&)m_bStop)
{
  m_pClock = pClock;
  m_pAudioCodec = NULL;
  m_pAudioCodec2 = NULL;
  m_bAudio2 = false;
  m_bAudio2Skip = false;
  m_bAudio2Dumb = false;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_started = false;
  m_silence = false;
  m_resampleratio = 1.0;
  m_synctype = SYNC_DISCON;
  m_setsynctype = SYNC_DISCON;
  m_prevsynctype = -1;
  m_error = 0;
  m_errors.Flush();
  m_syncclock = true;
  m_integral = 0;
  m_prevskipped = false;
  m_maxspeedadjust = 0.0;

  m_messageQueue.SetMaxDataSize(6 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(8.0);
}

CDVDPlayerAudio::~CDVDPlayerAudio()
{
  StopThread();

  // close the stream, and don't wait for the audio to be finished
  // CloseStream(true);
}

bool CDVDPlayerAudio::OpenStream( CDVDStreamInfo &hints )
{
  m_bAudio2 = CSettings::Get().GetBool("audiooutput2.enabled") ? true : false;

  CLog::Log(LOGNOTICE, "Finding audio codec for: %i", hints.codec);
  CDVDAudioCodec* codec = CDVDFactoryCodec::CreateAudioCodec(hints);
  if( !codec )
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }
  CDVDAudioCodec* codec2 = NULL;
  if (m_bAudio2)
  {
    codec2 = CDVDFactoryCodec::CreateAudioCodec(hints, m_bAudio2);
    if( !codec2 )
    {
      CLog::Log(LOGERROR, "Unsupported 2nd audio codec");
      m_dvdAudio2.Destroy();
      m_bAudio2 = false;
    }
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new CDVDMsgAudioCodecChange(hints, codec, codec2), 0);
  else
  {
    OpenStream(hints, codec, codec2);
    m_messageQueue.Init();
    CLog::Log(LOGNOTICE, "Creating audio thread");
    Create();
  }
  return true;
}

void CDVDPlayerAudio::OpenStream( CDVDStreamInfo &hints, CDVDAudioCodec* codec, CDVDAudioCodec* codec2 )
{
  if (m_pAudioCodec)
    SAFE_DELETE(m_pAudioCodec);
  m_pAudioCodec = codec;
  if (m_pAudioCodec2)
    SAFE_DELETE(m_pAudioCodec2);
  m_pAudioCodec2 = codec2;

  /* store our stream hints */
  m_streaminfo = hints;

  /* update codec information from what codec gave out, if any */
  int channelsFromCodec = m_pAudioCodec->GetChannels();
  int samplerateFromCodec = m_pAudioCodec->GetEncodedSampleRate();

  if (channelsFromCodec > 0)
    m_streaminfo.channels = channelsFromCodec;
  if (samplerateFromCodec > 0)
    m_streaminfo.samplerate = samplerateFromCodec;

  /* check if we only just got sample rate, in which case the previous call
   * to CreateAudioCodec() couldn't have started passthrough */
  if (hints.samplerate != m_streaminfo.samplerate)
    SwitchCodecIfNeeded();

  m_audioClock = 0;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_started = false;

  m_synctype = SYNC_DISCON;
  m_setsynctype = SYNC_DISCON;
  if (CSettings::Get().GetBool("videoplayer.usedisplayasclock"))
    m_setsynctype = CSettings::Get().GetInt("videoplayer.synctype");
  m_prevsynctype = -1;

  m_error = 0;
  m_errors.Flush();
  m_integral = 0;
  m_prevskipped = false;
  m_syncclock = true;
  m_silence = false;

  m_maxspeedadjust = CSettings::Get().GetNumber("videoplayer.maxspeedadjust");
}

void CDVDPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  bool bWait = bWaitForBuffers && m_speed > 0 && !CAEFactory::IsSuspended();

  // wait until buffers are empty
  if (bWait) m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGNOTICE, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  // destroy audio device
  CLog::Log(LOGNOTICE, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_dvdAudio.Drain();
    if (m_bAudio2)
      m_dvdAudio2.Drain();
    m_bStop = true;
  }
  else
  {
    m_dvdAudio.Flush();
    if (m_bAudio2)
      m_dvdAudio2.Flush();
  }

  m_dvdAudio.Destroy();
  if (m_bAudio2)
    m_dvdAudio2.Destroy();

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGNOTICE, "Deleting audio codec");
  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    delete m_pAudioCodec;
    m_pAudioCodec = NULL;
  }
  if (m_pAudioCodec2)
  {
    m_pAudioCodec2->Dispose();
    delete m_pAudioCodec2;
    m_pAudioCodec2 = NULL;
  }

  m_bAudio2 = false;
}

// decode one audio frame and returns its uncompressed size
int CDVDPlayerAudio::DecodeFrame(DVDAudioFrame &audioframe, DVDAudioFrame &audioframe2)
{
  int result = 0;

  // make sure the sent frame is clean
  memset(&audioframe, 0, sizeof(DVDAudioFrame));
  memset(&audioframe2, 0, sizeof(DVDAudioFrame));
  m_audio2frames.Clear();

  while (!m_bStop)
  {
    bool switched = false;
    /* NOTE: the audio packet can contain several frames */
    while( !m_bStop && m_decode.size > 0 )
    {
      if( !m_pAudioCodec || (m_bAudio2 && !m_pAudioCodec2) )
        return DECODE_FLAG_ERROR;

      /* the packet dts refers to the first audioframe that starts in the packet */
      double dts = m_ptsInput.Get(m_decode.size + m_pAudioCodec->GetBufferSize(), true);
      if (dts != DVD_NOPTS_VALUE)
        m_audioClock = dts;

      int len = m_pAudioCodec->Decode(m_decode.data, m_decode.size);
      if (m_bAudio2)
        m_pAudioCodec2->Decode(m_decode.data, m_decode.size);
      if (len < 0 || len > m_decode.size)
      {
        /* if error, we skip the packet */
        CLog::Log(LOGERROR, "CDVDPlayerAudio::DecodeFrame - Decode Error. Skipping audio packet (%d)", len);
        m_decode.Release();
        m_pAudioCodec->Reset();
        if (m_bAudio2)
          m_pAudioCodec2->Reset();
        return DECODE_FLAG_ERROR;
      }

      m_audioStats.AddSampleBytes(len);

      m_decode.data += len;
      m_decode.size -= len;

      // get decoded data and the size of it
      m_pAudioCodec->GetData(audioframe);

      if (m_bAudio2)
      {
        m_pAudioCodec2->GetData(audioframe2);
        if (audioframe2.size > 0)
        {
          m_audio2frames.Add(audioframe2);
        }
      }

      if (audioframe.size == 0)
        continue;

      if (audioframe.pts == DVD_NOPTS_VALUE)
        audioframe.pts = m_audioClock;

      if (m_bAudio2)
      {
        m_audio2frames.Merge(audioframe2);

        if (audioframe2.size > 0)
        {
          if (audioframe2.pts == DVD_NOPTS_VALUE)
            audioframe2.pts  = m_audioClock;
        }
      }

      if (m_streaminfo.samplerate != audioframe.encoded_sample_rate)
      {
        // The sample rate has changed or we just got it for the first time
        // for this stream. See if we should enable/disable passthrough due
        // to it.
        m_streaminfo.samplerate = audioframe.encoded_sample_rate;
        if (!switched && SwitchCodecIfNeeded()) {
          // passthrough has been enabled/disabled, reprocess the packet
          m_decode.data -= len;
          m_decode.size += len;
          switched = true;
          m_audio2frames.Clear();
          continue;
        }
      }

      // increase audioclock to after the packet
      m_audioClock += audioframe.duration;

      // if demux source want's us to not display this, continue
      if(m_decode.msg->GetPacketDrop())
      {
        m_audio2frames.Clear();
        result |= DECODE_FLAG_DROP;
      }

      return result;
    }
    // free the current packet
    m_decode.Release();

    if (m_messageQueue.ReceivedAbortRequest()) return DECODE_FLAG_ABORT;

    CDVDMsg* pMsg;
    int timeout  = (int)(1000 * m_dvdAudio.GetCacheTime()) + 100;

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_started == false                /* when not started */
    ||  m_speed   == DVD_PLAYSPEED_NORMAL /* when playing normally */
    ||  m_speed   <  DVD_PLAYSPEED_PAUSE  /* when rewinding */
    || (m_speed   >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, timeout, priority);

    if (ret == MSGQ_TIMEOUT)
      return DECODE_FLAG_TIMEOUT;

    if (MSGQ_IS_ERROR(ret))
      return DECODE_FLAG_ABORT;

    if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      m_decode.Attach((CDVDMsgDemuxerPacket*)pMsg);
      m_ptsInput.Add( m_decode.size, m_decode.dts );
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if(((CDVDMsgGeneralSynchronize*)pMsg)->Wait( 100, SYNCSOURCE_AUDIO ))
        CLog::Log(LOGDEBUG, "CDVDPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg->Acquire(), 1); /* push back as prio message, to process other prio messages */
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      CDVDMsgGeneralResync* pMsgGeneralResync = (CDVDMsgGeneralResync*)pMsg;
      CLog::Log(LOGDEBUG, "CDVDPlayerAudio - CDVDMsg::GENERAL_RESYNC(%f, %d)"
                        , pMsgGeneralResync->m_timestamp
                        , pMsgGeneralResync->m_clock);

      if (pMsgGeneralResync->m_timestamp != DVD_NOPTS_VALUE)
        m_audioClock = pMsgGeneralResync->m_timestamp;

      m_ptsInput.Flush();
      m_dvdAudio.SetPlayingPts(m_audioClock);
      if (m_bAudio2)
        m_dvdAudio2.SetPlayingPts(m_audioClock);
      if (pMsgGeneralResync->m_clock)
        m_pClock->Discontinuity(m_dvdAudio.GetPlayingPts());
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      if (m_pAudioCodec2)
        m_pAudioCodec2->Reset();
      m_decode.Release();
      m_started = false;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      m_dvdAudio.Flush();
      if (m_bAudio2)
        m_dvdAudio2.Flush();
      m_ptsInput.Flush();
      m_syncclock = true;
      m_stalled   = true;
      m_started   = false;

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      if (m_pAudioCodec2)
        m_pAudioCodec2->Reset();

      m_decode.Release();
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_STARTED))
    {
      if(m_started)
        m_messageParent.Put(new CDVDMsgInt(CDVDMsg::PLAYER_STARTED, DVDPLAYER_AUDIO));
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAYTIME))
    {
      CDVDPlayer::SPlayerState& state = ((CDVDMsgType<CDVDPlayer::SPlayerState>*)pMsg)->m_value;

      if(state.time_src == CDVDPlayer::ETIMESOURCE_CLOCK)
        state.time      = DVD_TIME_TO_MSEC(m_pClock->GetClock(state.timestamp) + state.time_offset);
      else
        state.timestamp = CDVDClock::GetAbsoluteClock();
      state.player    = DVDPLAYER_AUDIO;
      m_messageParent.Put(pMsg->Acquire());
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "CDVDPlayerAudio - CDVDMsg::GENERAL_EOF");
      m_dvdAudio.Finish();
      if (m_bAudio2)
        m_dvdAudio2.Finish();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_DELAY))
    {
      if (m_speed != DVD_PLAYSPEED_PAUSE)
      {
        double timeout = static_cast<CDVDMsgDouble*>(pMsg)->m_value;

        CLog::Log(LOGDEBUG, "CDVDPlayerAudio - CDVDMsg::GENERAL_DELAY(%f)", timeout);

        timeout *= (double)DVD_PLAYSPEED_NORMAL / abs(m_speed);
        timeout += CDVDClock::GetAbsoluteClock();

        while(!m_bStop && CDVDClock::GetAbsoluteClock() < timeout)
          Sleep(1);
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      double speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;

      if (speed == DVD_PLAYSPEED_NORMAL)
      {
        m_dvdAudio.Resume();
        if (m_bAudio2)
          m_dvdAudio2.Resume();
      }
      else
      {
        m_syncclock = true;
        if (speed != DVD_PLAYSPEED_PAUSE)
        {
          m_dvdAudio.Flush();
          if (m_bAudio2)
            m_dvdAudio2.Flush();
        }
        m_dvdAudio.Pause();
        if (m_bAudio2)
          m_dvdAudio2.Pause();
      }
      m_speed = speed;
    }
    else if (pMsg->IsType(CDVDMsg::AUDIO_SILENCE))
    {
      m_silence = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CDVDPlayerAudio - CDVDMsg::AUDIO_SILENCE(%f, %d)"
                        , m_audioClock, m_silence);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      CDVDMsgAudioCodecChange* msg(static_cast<CDVDMsgAudioCodecChange*>(pMsg));
      OpenStream(msg->m_hints, msg->m_codec, msg->m_codec2);
      msg->m_codec = NULL;
      msg->m_codec2 = NULL;
    }

    pMsg->Release();
  }
  return 0;
}

void CDVDPlayerAudio::OnStartup()
{
  m_decode.Release();

#ifdef TARGET_WINDOWS
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
}

void CDVDPlayerAudio::UpdatePlayerInfo()
{
  std::ostringstream s;
  s << "aq:"     << setw(2) << min(99,m_messageQueue.GetLevel() + MathUtils::round_int(100.0/8.0*m_dvdAudio.GetCacheTime())) << "%";
  s << ", Kb/s:" << fixed << setprecision(2) << (double)GetAudioBitrate() / 1024.0;

  //print the inverse of the resample ratio, since that makes more sense
  //if the resample ratio is 0.5, then we're playing twice as fast
  if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << fixed << setprecision(5) << 1.0 / m_resampleratio;

  if (m_bAudio2)
    s << ", a1/a2:" << fixed << setprecision(3) << m_audiodiff;

  s << ", att:" << fixed << setprecision(1) << log(GetCurrentAttenuation()) * 20.0f << " dB";

  { CSingleLock lock(m_info_section);
    m_info = s.str();
  }
}

void CDVDPlayerAudio::Process()
{
  CLog::Log(LOGNOTICE, "running thread: CDVDPlayerAudio::Process()");

  bool packetadded(false);

  DVDAudioFrame audioframe;
  DVDAudioFrame audioframe2;
  m_audioStats.Start();
  m_audiodiff = 0.0;
  m_bAudio2Skip = false;

  while (!m_bStop)
  {
    int result = DecodeFrame(audioframe, audioframe2);

    //Drop when not playing normally
    if(m_speed   != DVD_PLAYSPEED_NORMAL
    && m_started == true)
    {
      result |= DECODE_FLAG_DROP;
    }

    UpdatePlayerInfo();

    if( result & DECODE_FLAG_ERROR )
    {
      CLog::Log(LOGDEBUG, "CDVDPlayerAudio::Process - Decode Error");
      continue;
    }

    if( result & DECODE_FLAG_TIMEOUT )
    {
      // Flush as the audio output may keep looping if we don't
      if(m_speed == DVD_PLAYSPEED_NORMAL && !m_stalled)
      {
        m_dvdAudio.Drain();
        m_dvdAudio.Flush();
        if (m_bAudio2)
        {
          m_dvdAudio2.Drain();
          m_dvdAudio2.Flush();
        }
        m_stalled = true;
      }

      continue;
    }

    if( result & DECODE_FLAG_ABORT )
    {
      CLog::Log(LOGDEBUG, "CDVDPlayerAudio::Process - Abort received, exiting thread");
      break;
    }

    if( audioframe.size == 0 )
      continue;

    packetadded = true;

    // we have succesfully decoded an audio frame, setup renderer to match
    if (!m_dvdAudio.IsValidFormat(audioframe))
    {
      if(m_speed)
        m_dvdAudio.Drain();

      m_dvdAudio.Destroy();

      if(m_speed)
        m_dvdAudio.Resume();
      else
        m_dvdAudio.Pause();

      if(!m_dvdAudio.Create(audioframe, m_streaminfo.codec, m_setsynctype == SYNC_RESAMPLE))
        CLog::Log(LOGERROR, "%s - failed to create audio renderer", __FUNCTION__);
    }
    if (m_bAudio2 && audioframe2.size > 0 && !m_dvdAudio2.IsValidFormat(audioframe2))
    {
      if(m_speed)
        m_dvdAudio2.Drain();
		
        m_dvdAudio2.Destroy();
		
      if(m_speed)
        m_dvdAudio2.Resume();
      else
        m_dvdAudio2.Pause();
		
      if(!m_dvdAudio2.Create(audioframe2, m_streaminfo.codec, m_setsynctype == SYNC_RESAMPLE, m_bAudio2))
        CLog::Log(LOGERROR, "%s - failed to create 2nd audio renderer", __FUNCTION__);
    }
	if (m_bAudio2)
		m_bAudio2Dumb = CAEFactory::IsDumb(true);

    // Zero out the frame data if we are supposed to silence the audio
    if (m_silence)
    {
      memset(audioframe.data, 0, audioframe.size);
      memset(audioframe2.data, 0, audioframe2.size);
    }

    if(result & DECODE_FLAG_DROP)
    {
      // keep output times in sync
     m_dvdAudio.SetPlayingPts(m_audioClock);
	 if(m_bAudio2)
       m_dvdAudio2.SetPlayingPts(m_audioClock);
    }
    else
    {
      SetSyncType(audioframe.passthrough);

      // add any packets play
      if (m_bAudio2)
        HandleSyncAudio2(audioframe2);
      packetadded = OutputPacket(audioframe, audioframe2);

      // we are not running until something is cached in output device
      if(m_stalled && m_dvdAudio.GetCacheTime() > 0.0)
        m_stalled = false;
    }

    // signal to our parent that we have initialized
    if(m_started == false)
    {
      m_started = true;
      m_messageParent.Put(new CDVDMsgInt(CDVDMsg::PLAYER_STARTED, DVDPLAYER_AUDIO));
    }

    if( m_dvdAudio.GetPlayingPts() == DVD_NOPTS_VALUE )
      continue;

    if( m_speed != DVD_PLAYSPEED_NORMAL )
      continue;

    if (packetadded)
      HandleSyncError(audioframe.duration);
  }
}

void CDVDPlayerAudio::SetSyncType(bool passthrough)
{
  //set the synctype from the gui
  //use skip/duplicate when resample is selected and passthrough is on
  m_synctype = m_setsynctype;
  if (passthrough && m_synctype == SYNC_RESAMPLE)
    m_synctype = SYNC_SKIPDUP;

  //tell dvdplayervideo how much it can change the speed
  //if SetMaxSpeedAdjust returns false, it means no video is played and we need to use clock feedback
  double maxspeedadjust = 0.0;
  if (m_synctype == SYNC_RESAMPLE)
    maxspeedadjust = m_maxspeedadjust;

  if (!m_pClock->SetMaxSpeedAdjust(maxspeedadjust))
    m_synctype = SYNC_DISCON;

  if (m_synctype != m_prevsynctype)
  {
    const char *synctypes[] = {"clock feedback", "skip/duplicate", "resample", "invalid"};
    int synctype = (m_synctype >= 0 && m_synctype <= 2) ? m_synctype : 3;
    CLog::Log(LOGDEBUG, "CDVDPlayerAudio:: synctype set to %i: %s", m_synctype, synctypes[synctype]);
    m_prevsynctype = m_synctype;
  }
}

void CDVDPlayerAudio::HandleSyncError(double duration)
{
  double clock = m_pClock->GetClock();
  double error = m_dvdAudio.GetPlayingPts() - clock;

  if( fabs(error) > DVD_MSEC_TO_TIME(100) || m_syncclock )
  {
    m_pClock->Discontinuity(clock+error);
    CLog::Log(LOGDEBUG, "CDVDPlayerAudio:: Discontinuity1 - was:%f, should be:%f, error:%f", clock, clock+error, error);

    m_errors.Flush();
    m_error = 0;
    m_syncclock = false;

    return;
  }

  m_errors.Add(error);

  //check if measured error for 2 seconds
  if (m_errors.Get(m_error))
  {
    if (m_synctype == SYNC_DISCON)
    {
      double limit, error;
      if (g_VideoReferenceClock.GetRefreshRate(&limit) > 0)
      {
        //when the videoreferenceclock is running, the discontinuity limit is one vblank period
        limit *= DVD_TIME_BASE;

        //make error a multiple of limit, rounded towards zero,
        //so it won't interfere with the sync methods in CXBMCRenderManager::WaitPresentTime
        if (m_error > 0.0)
          error = limit * floor(m_error / limit);
        else
          error = limit * ceil(m_error / limit);
      }
      else
      {
        limit = DVD_MSEC_TO_TIME(10);
        error = m_error;
      }

      if (fabs(error) > limit - 0.001)
      {
        m_pClock->Discontinuity(clock+error);
        CLog::Log(LOGDEBUG, "CDVDPlayerAudio:: Discontinuity2 - was:%f, should be:%f, error:%f", clock, clock+error, error);
      }
    }
    else if (m_synctype == SYNC_RESAMPLE)
    {
      //reset the integral on big errors, failsafe
      if (fabs(m_error) > DVD_TIME_BASE)
        m_integral = 0;
      else if (fabs(m_error) > DVD_MSEC_TO_TIME(5))
        m_integral += m_error / DVD_TIME_BASE / INTEGRAL;

      double proportional = 0.0;

      //on big errors use more proportional
      if (fabs(m_error / DVD_TIME_BASE) > 0.0)
      {
        double proportionaldiv = PROPORTIONAL * (PROPREF / fabs(m_error / DVD_TIME_BASE));
        if (proportionaldiv < PROPDIVMIN) proportionaldiv = PROPDIVMIN;
        else if (proportionaldiv > PROPDIVMAX) proportionaldiv = PROPDIVMAX;

        proportional = m_error / DVD_TIME_BASE / proportionaldiv;
      }
      m_resampleratio = 1.0 / g_VideoReferenceClock.GetSpeed() + proportional + m_integral;
    }
  }
}

void CDVDPlayerAudio::HandleSyncAudio2(DVDAudioFrame &audioframe2)
{
  if(m_bAudio2Dumb)
  {
    m_audiodiff = 0.0;
	return;
  }
  if(audioframe2.size <= 0)
    return;

  double threshold = 50000.0;
  threshold = threshold > audioframe2.duration ? threshold : audioframe2.duration;

  double dtm1 = m_dvdAudio.GetDelay();
  double dtm2 = m_dvdAudio2.GetDelay();
  double ddiff = (dtm1 - dtm2);

  m_audiodiff = ddiff / DVD_TIME_BASE;

  if (ddiff > threshold)
  {
    memset(audioframe2.data, 0, audioframe2.size);
    m_dvdAudio2.AddPackets(audioframe2);
  }

  if (ddiff < -threshold)
  {
    m_bAudio2Skip = true;
  }
  else if (m_bAudio2Skip && ddiff > 0.0)
  {
    m_bAudio2Skip = false;
  }
}

bool CDVDPlayerAudio::OutputPacket(DVDAudioFrame &audioframe, DVDAudioFrame &audioframe2)
{
  bool bAddAudio2 = (m_bAudio2 && !m_bAudio2Dumb && !m_bAudio2Skip && audioframe2.size > 0);
  if (m_synctype == SYNC_DISCON)
  {
    m_dvdAudio.AddPackets(audioframe);
    if (bAddAudio2)
      m_dvdAudio2.AddPackets(audioframe2);
  }
  else if (m_synctype == SYNC_SKIPDUP)
  {
    double limit = std::max(DVD_MSEC_TO_TIME(10), audioframe.duration * 2.0 / 3.0);
    if (m_error < -limit)
    {
      m_prevskipped = !m_prevskipped;
      if (m_prevskipped)
      {
        m_dvdAudio.AddPackets(audioframe);
        if (bAddAudio2)
          m_dvdAudio2.AddPackets(audioframe2);
      }
      else
      {
        CLog::Log(LOGDEBUG, "CDVDPlayerAudio:: Dropping packet of %d ms", DVD_TIME_TO_MSEC(audioframe.duration));
        m_error += audioframe.duration;
      }
    }
    else if(m_error > limit)
    {
      CLog::Log(LOGDEBUG, "CDVDPlayerAudio:: Duplicating packet of %d ms", DVD_TIME_TO_MSEC(audioframe.duration));
      m_dvdAudio.AddPackets(audioframe);
      m_dvdAudio.AddPackets(audioframe);
      if (bAddAudio2)
      {
        m_dvdAudio2.AddPackets(audioframe2);
        m_dvdAudio2.AddPackets(audioframe2);
      }
      m_error -= audioframe.duration;
    }
    else
    {
      m_dvdAudio.AddPackets(audioframe);
      if (bAddAudio2)
        m_dvdAudio2.AddPackets(audioframe2);
    }
  }
  else if (m_synctype == SYNC_RESAMPLE)
  {
    m_dvdAudio.SetResampleRatio(m_resampleratio);
    m_dvdAudio.AddPackets(audioframe);
    if (bAddAudio2)
      m_dvdAudio2.AddPackets(audioframe2);
  }

  return true;
}

void CDVDPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGNOTICE, "thread end: CDVDPlayerAudio::OnExit()");
}

void CDVDPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put( new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed), 1 );
  else
    m_speed = speed;
}

void CDVDPlayerAudio::Flush()
{
  m_messageQueue.Flush();
  m_messageQueue.Put( new CDVDMsg(CDVDMsg::GENERAL_FLUSH), 1);
}

void CDVDPlayerAudio::WaitForBuffers()
{
  // make sure there are no more packets available
  m_messageQueue.WaitUntilEmpty();

  // make sure almost all has been rendered
  // leave 500ms to avound buffer underruns
  double delay = m_dvdAudio.GetCacheTime();
  if(delay > 0.5)
    Sleep((int)(1000 * (delay - 0.5)));
}

bool CDVDPlayerAudio::SwitchCodecIfNeeded()
{
  CLog::Log(LOGDEBUG, "CDVDPlayerAudio: Sample rate changed, checking for passthrough");
  bool bSwitched = false;
  CDVDAudioCodec *codec = CDVDFactoryCodec::CreateAudioCodec(m_streaminfo);
  if (!codec || codec->NeedPassthrough() == m_pAudioCodec->NeedPassthrough()) {
    // passthrough state has not changed
    delete codec;
    bSwitched = false;
  } else {
    delete m_pAudioCodec;
    m_pAudioCodec = codec;
    bSwitched = true;
  }

  if (m_bAudio2)
  {
    CDVDAudioCodec *codec2 = CDVDFactoryCodec::CreateAudioCodec(m_streaminfo, true);
    if (codec2 != NULL)
    {
      if (!codec2 || codec2->NeedPassthrough() == m_pAudioCodec2->NeedPassthrough()) {
        // passthrough state has not changed
        delete codec2;
      } else {
        delete m_pAudioCodec2;
        m_pAudioCodec2 = codec2;
      }
    }
  }

  return bSwitched;
}

string CDVDPlayerAudio::GetPlayerInfo()
{
  CSingleLock lock(m_info_section);
  return m_info;
}

int CDVDPlayerAudio::GetAudioBitrate()
{
  return (int)m_audioStats.GetBitrate();
}

bool CDVDPlayerAudio::IsPassthrough() const
{
  return m_pAudioCodec && m_pAudioCodec->NeedPassthrough() && (!m_bAudio2 || (m_pAudioCodec2 && m_pAudioCodec2->NeedPassthrough()));
}
