/*
 *      Copyright (C) 2005-2012 Team XBMC
 *      http://www.xbmc.org
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

#include "PAPlayer.h"
#include "CodecFactory.h"
#include "FileItem.h"
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "music/tags/MusicInfoTag.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "utils/MathUtils.h"

#include "threads/SingleLock.h"
#include "cores/AudioEngine/AEFactory.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/Interfaces/AEStream.h"

#define TIME_TO_CACHE_NEXT_FILE 5000 /* 5 seconds before end of song, start caching the next song */
#define FAST_XFADE_TIME           80 /* 80 milliseconds */
#define MAX_SKIP_XFADE_TIME     2000 /* max 2 seconds crossfade on track skip */

CAEChannelInfo ICodec::GetChannelInfo()
{
  return CAEUtil::GuessChLayout(m_Channels);
}

// PAP: Psycho-acoustic Audio Player
// Supporting all open  audio codec standards.
// First one being nullsoft's nsv audio decoder format

PAPlayer::PAPlayer(IPlayerCallback& callback) :
  IPlayer              (callback),
  CThread              ("PAPlayer"),
  m_signalSpeedChange  (false),
  m_playbackSpeed      (1    ),
  m_isPlaying          (false),
  m_isPaused           (false),
  m_isFinished         (false),
  m_defaultCrossfadeMS (0),
  m_upcomingCrossfadeMS(0),
  m_currentStream      (NULL ),
  m_audioCallback      (NULL ),
  m_FileItem           (new CFileItem())
{
  memset(&m_playerGUIData, 0, sizeof(m_playerGUIData));
  m_bAudio2 = false;
  m_iTimeSynced = 0;
  m_iAudio2DiscardSamples = 0;
}

PAPlayer::~PAPlayer()
{
  if (!m_isPaused)
    SoftStop(true, true);
  CloseAllStreams(false);

  /* wait for the thread to terminate */
  StopThread(true);//true - wait for end of thread
  delete m_FileItem;
}

bool PAPlayer::HandlesType(const CStdString &type)
{
  ICodec* codec = CodecFactory::CreateCodec(type);
  if (codec && codec->CanInit())
  {
    delete codec;
    return true;
  }

  return false;
}

void PAPlayer::SoftStart(bool wait/* = false */)
{
  CSharedLock lock(m_streamsLock);
  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
  {
    StreamInfo* si = *itt;
    if (si->m_fadeOutTriggered)
      continue;

    si->m_stream->FadeVolume(0.0f, 1.0f, FAST_XFADE_TIME);
    si->m_stream->Resume();
    if(m_bAudio2)
    {
	  si->m_stream2->FadeVolume(0.0f, 1.0f, FAST_XFADE_TIME);
      si->m_stream2->Resume();
    }
  }
  
  if (wait)
  {
    /* wait for them to fade in */
    lock.Leave();
    Sleep(FAST_XFADE_TIME);
    lock.Enter();

    /* be sure they have faded in */
    while(wait)
    {
      wait = false;
      for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
      {
        StreamInfo* si = *itt;
        if (si->m_stream->IsFading())
        {
          lock.Leave();
          wait = true;
          Sleep(1);
          lock.Enter();
          break;
        }
      }
    }
  }
}

void PAPlayer::SoftStop(bool wait/* = false */, bool close/* = true */)
{
  /* fade all the streams out fast for a nice soft stop */
  CSharedLock lock(m_streamsLock);
  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
  {
    StreamInfo* si = *itt;
    if (si->m_stream)
      si->m_stream->FadeVolume(1.0f, 0.0f, FAST_XFADE_TIME);
    if(m_bAudio2 && si->m_stream2)
      si->m_stream2->FadeVolume(1.0f, 0.0f, FAST_XFADE_TIME);

    if (close)
    {
      si->m_prepareTriggered  = true;
      si->m_playNextTriggered = true;
      si->m_fadeOutTriggered  = true;
    }
  }

  /* if we are going to wait for them to finish fading */
  if(wait)
  {
    /* wait for them to fade out */
    lock.Leave();
    Sleep(FAST_XFADE_TIME);
    lock.Enter();

    /* be sure they have faded out */
    while(wait && !CAEFactory::IsSuspended())
    {
      wait = false;
      for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
      {
        StreamInfo* si = *itt;
        if (si->m_stream && si->m_stream->IsFading())
        {
          lock.Leave();
          wait = true;
          Sleep(1);
          lock.Enter();
          break;
        }
      }
    }

    /* if we are not closing the streams, pause them */
    if (!close)
    {
      for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
      {
        StreamInfo* si = *itt;
        si->m_stream->Pause();
        if(m_bAudio2)
          si->m_stream2->Pause();
      }
    }
  }
}

void PAPlayer::CloseAllStreams(bool fade/* = true */)
{
  if (!fade) 
  {
    CExclusiveLock lock(m_streamsLock);
    while(!m_streams.empty())
    {
      StreamInfo* si = m_streams.front();
      m_streams.pop_front();
      
      if (si->m_stream)
      {
        CAEFactory::FreeStream(si->m_stream);
        si->m_stream = NULL;
      }
      if(m_bAudio2 && si->m_stream2)
      {
        CAEFactory::FreeStream(si->m_stream2);
        si->m_stream2 = NULL;
      }

      si->m_decoder.Destroy();
      if(si->m_usedecoder2)
        si->m_decoder2.Destroy();
      delete si;
    }

    while(!m_finishing.empty())
    {
      StreamInfo* si = m_finishing.front();
      m_finishing.pop_front();

      if (si->m_stream)
      {
        CAEFactory::FreeStream(si->m_stream);
        si->m_stream = NULL;
      }
      if(m_bAudio2 && si->m_stream2)
      {
        CAEFactory::FreeStream(si->m_stream2);
        si->m_stream2 = NULL;
      }

      si->m_decoder.Destroy();
      if(si->m_usedecoder2)
        si->m_decoder2.Destroy();
      delete si;
    }
    m_currentStream = NULL;
  }
  else
  {
    SoftStop(false, true);
    CExclusiveLock lock(m_streamsLock);
    m_currentStream = NULL;
  }  
}

bool PAPlayer::OpenFile(const CFileItem& file, const CPlayerOptions &options)
{
  m_defaultCrossfadeMS = g_guiSettings.GetInt("musicplayer.crossfade") * 1000;

  if (m_streams.size() > 1 || !m_defaultCrossfadeMS || m_isPaused)
  {
    CloseAllStreams(!m_isPaused);
    m_isPaused = false; // Make sure to reset the pause state
  }

  if (!QueueNextFileEx(file, false))
    return false;

  CSharedLock lock(m_streamsLock);
  if (m_streams.size() == 2)
  {
    //do a short crossfade on trackskip, set to max 2 seconds for these prev/next transitions
    m_upcomingCrossfadeMS = std::min(m_defaultCrossfadeMS, (unsigned int)MAX_SKIP_XFADE_TIME);

    //start transition to next track
    StreamInfo* si = m_streams.front();
    si->m_playNextAtFrame  = si->m_framesSent; //start next track at current frame
    si->m_prepareTriggered = true; //next track is ready to go
  }
  lock.Leave();

  if (!IsRunning())
    Create();

  /* trigger playback start */
  m_isPlaying = true;
  m_startEvent.Set();
  return true;
}

void PAPlayer::UpdateCrossfadeTime(const CFileItem& file)
{
  m_upcomingCrossfadeMS = m_defaultCrossfadeMS = g_guiSettings.GetInt("musicplayer.crossfade") * 1000;
  if (m_upcomingCrossfadeMS)
  {
    if (m_streams.size() == 0 ||
         (
            file.HasMusicInfoTag() && !g_guiSettings.GetBool("musicplayer.crossfadealbumtracks") &&
            m_FileItem->HasMusicInfoTag() &&
            (m_FileItem->GetMusicInfoTag()->GetAlbum() != "") &&
            (m_FileItem->GetMusicInfoTag()->GetAlbum() == file.GetMusicInfoTag()->GetAlbum()) &&
            (m_FileItem->GetMusicInfoTag()->GetDiscNumber() == file.GetMusicInfoTag()->GetDiscNumber()) &&
            (m_FileItem->GetMusicInfoTag()->GetTrackNumber() == file.GetMusicInfoTag()->GetTrackNumber() - 1)
         )
       )
    {
      //do not crossfade when playing consecutive albumtracks
      m_upcomingCrossfadeMS = 0;
    }
  }
}

bool PAPlayer::QueueNextFile(const CFileItem &file)
{
  return QueueNextFileEx(file);
}

bool PAPlayer::QueueNextFileEx(const CFileItem &file, bool fadeIn/* = true */)
{
  StreamInfo *si = new StreamInfo();

  m_bAudio2 = (g_guiSettings.GetInt("audiooutput2.mode") == AUDIO_NONE) ? false : !CAEFactory::IsDualAudioBetaExpired();

  si->m_decoder.SetCheckAudio2(m_bAudio2);
  if (!si->m_decoder.Create(file, (file.m_lStartOffset * 1000) / 75))
  {
    CLog::Log(LOGWARNING, "PAPlayer::QueueNextFileEx - Failed to create the decoder");

    delete si;
    m_callback.OnQueueNextItem();
    return false;
  }

  si->m_usedecoder2 = false;
  if (m_bAudio2)
  {
    si->m_decoder2.SetAudio2(true);
    if (si->m_decoder.IsReusableForAudio2())
      CLog::Log(LOGINFO, "PAPlayer::QueueNextFileEx - Reuse for 2nd decoder");
    else if (si->m_decoder2.Create(file, (file.m_lStartOffset * 1000) / 75))
      si->m_usedecoder2 = true;
    else
      CLog::Log(LOGWARNING, "PAPlayer::QueueNextFileEx - Failed to create 2nd decoder");
  }

  /* decode until there is data-available */
  si->m_decoder.Start();
  while(si->m_decoder.GetDataSize() == 0)
  {
    int status = si->m_decoder.GetStatus();
    if (status == STATUS_ENDED   ||
        status == STATUS_NO_FILE ||
        si->m_decoder.ReadSamples(PACKET_SIZE) == RET_ERROR)
    {
      CLog::Log(LOGINFO, "PAPlayer::QueueNextFileEx - Error reading samples");

      si->m_decoder.Destroy();
      if(si->m_usedecoder2)
	    si->m_decoder2.Destroy();
      delete si;
      m_callback.OnQueueNextItem();
      return false;
    }

    /* yield our time so that the main PAP thread doesnt stall */
    CThread::Sleep(1);
  }

  if (si->m_usedecoder2)
  {
    si->m_decoder2.Start();
	while(si->m_decoder2.GetDataSize() == 0)
	{
	  int status = si->m_decoder2.GetStatus();
	  if (status == STATUS_ENDED   ||
		  status == STATUS_NO_FILE ||
		  si->m_decoder2.ReadSamples(PACKET_SIZE) == RET_ERROR)
	  {
		CLog::Log(LOGINFO, "PAPlayer::QueueNextFileEx 2nd - Error reading samples");

        si->m_decoder2.Destroy();
        si->m_usedecoder2 = false;
        break;
	  }
	
	  /* yield our time so that the main PAP thread doesnt stall */
	  CThread::Sleep(1);
	}
  }

  UpdateCrossfadeTime(file);

  /* init the streaminfo struct */
  si->m_decoder.GetDataFormat(&si->m_channelInfo, &si->m_sampleRate, &si->m_encodedSampleRate, &si->m_dataFormat);
  si->m_startOffset        = file.m_lStartOffset * 1000 / 75;
  si->m_endOffset          = file.m_lEndOffset   * 1000 / 75;
  si->m_bytesPerSample     = CAEUtil::DataFormatToBits(si->m_dataFormat) >> 3;
  si->m_bytesPerFrame      = si->m_bytesPerSample * si->m_channelInfo.Count();
  if (si->m_usedecoder2)
  {
    si->m_decoder2.GetDataFormat(&si->m_channelInfo2, &si->m_sampleRate2, &si->m_encodedSampleRate2, &si->m_dataFormat2);
    si->m_bytesPerSample2    = CAEUtil::DataFormatToBits(si->m_dataFormat2) >> 3;
    si->m_bytesPerFrame2     = si->m_bytesPerSample2 * si->m_channelInfo2.Count();
  }
  else
  {
    si->m_channelInfo2       = si->m_channelInfo;
    si->m_sampleRate2        = si->m_sampleRate;
    si->m_encodedSampleRate2 = si->m_encodedSampleRate;
    si->m_dataFormat2        = si->m_dataFormat;
    si->m_bytesPerSample2    = si->m_bytesPerSample;
    si->m_bytesPerFrame2     = si->m_bytesPerFrame;
  }
  si->m_started            = false;
  si->m_finishing          = false;
  si->m_framesSent         = 0;
  si->m_framesSent2        = 0;
  si->m_seekNextAtFrame    = 0;
  si->m_seekFrame          = -1;
  si->m_seekFrame2         = -1;
  si->m_stream             = NULL;
  si->m_stream2            = NULL;
  si->m_volume             = (fadeIn && m_upcomingCrossfadeMS) ? 0.0f : 1.0f;
  si->m_fadeOutTriggered   = false;
  si->m_isSlaved           = false;

  int64_t streamTotalTime = si->m_decoder.TotalTime();
  if (si->m_endOffset)
    streamTotalTime = si->m_endOffset - si->m_startOffset;
  
  si->m_prepareNextAtFrame = 0;
  if (streamTotalTime >= TIME_TO_CACHE_NEXT_FILE + m_defaultCrossfadeMS)
    si->m_prepareNextAtFrame = (int)((streamTotalTime - TIME_TO_CACHE_NEXT_FILE - m_defaultCrossfadeMS) * si->m_sampleRate / 1000.0f);

  si->m_prepareTriggered = false;

  si->m_playNextAtFrame = 0;
  si->m_playNextTriggered = false;

  if (!PrepareStream(si))
  {
    CLog::Log(LOGINFO, "PAPlayer::QueueNextFileEx - Error preparing stream");
    
    si->m_decoder.Destroy();
    if(si->m_usedecoder2)
      si->m_decoder2.Destroy();
    delete si;
    m_callback.OnQueueNextItem();
    return false;
  }

  /* add the stream to the list */
  CExclusiveLock lock(m_streamsLock);
  m_streams.push_back(si);
  //update the current stream to start playing the next track at the correct frame.
  UpdateStreamInfoPlayNextAtFrame(m_currentStream, m_upcomingCrossfadeMS);

  *m_FileItem = file;

  return true;
}

void PAPlayer::UpdateStreamInfoPlayNextAtFrame(StreamInfo *si, unsigned int crossFadingTime)
{
  if (si)
  {
    int64_t streamTotalTime = si->m_decoder.TotalTime();
    if (si->m_endOffset)
      streamTotalTime = si->m_endOffset - si->m_startOffset;
    if (streamTotalTime < crossFadingTime)
      si->m_playNextAtFrame = (int)((streamTotalTime / 2) * si->m_sampleRate / 1000.0f);
    else
      si->m_playNextAtFrame = (int)((streamTotalTime - crossFadingTime) * si->m_sampleRate / 1000.0f);
  }
}

inline bool PAPlayer::PrepareStream(StreamInfo *si)
{
  /* if we have a stream we are already prepared */
  if (si->m_stream)
    return true;

  /* get a paused stream */
  si->m_stream = CAEFactory::MakeStream(
    si->m_dataFormat,
    si->m_sampleRate,
    si->m_encodedSampleRate,
    si->m_channelInfo,
    AESTREAM_PAUSED
  );

  if (!si->m_stream)
  {
    CLog::Log(LOGDEBUG, "PAPlayer::PrepareStream - Failed to get IAEStream");
    return false;
  }

  si->m_stream->SetVolume    (si->m_volume);
  si->m_stream->SetReplayGain(si->m_decoder.GetReplayGain());

  if(m_bAudio2)
  {
    si->m_stream2 = CAEFactory::MakeStream(
      si->m_dataFormat2,
      si->m_sampleRate2,
      si->m_encodedSampleRate2,
      si->m_channelInfo2,
      AESTREAM_PAUSED, true
    );

    if (!si->m_stream2)
    {
      CLog::Log(LOGDEBUG, "PAPlayer::PrepareStream 2nd - Failed to get IAEStream");
      if (si->m_usedecoder2)
      {
        si->m_decoder2.Destroy();
        si->m_usedecoder2 = false;
      }
      m_bAudio2 = false;
    }
    else
    {
      si->m_stream2->SetVolume    (si->m_volume);
      si->m_stream2->SetReplayGain(si->m_usedecoder2 ? si->m_decoder2.GetReplayGain() : si->m_decoder.GetReplayGain());
    }
  }

  /* if its not the first stream and crossfade is not enabled */
  if (m_currentStream && m_currentStream != si && !m_upcomingCrossfadeMS)
  {
    /* slave the stream for gapless */
    si->m_isSlaved = true;
    m_currentStream->m_stream->RegisterSlave(si->m_stream);
    if(m_bAudio2)
      m_currentStream->m_stream2->RegisterSlave(si->m_stream2);
  }

  /* fill the stream's buffer */
  while(si->m_stream->IsBuffering())
  {
    int status = si->m_decoder.GetStatus();
    if (status == STATUS_ENDED   ||
        status == STATUS_NO_FILE ||
        si->m_decoder.ReadSamples(PACKET_SIZE) == RET_ERROR)
    {
      CLog::Log(LOGINFO, "PAPlayer::PrepareStream - Stream Finished");
      break;
    }

    if (!QueueData(si))
      break;

    /* yield our time so that the main PAP thread doesnt stall */
    CThread::Sleep(1);
  }

  while(si->m_usedecoder2 && si->m_stream2->IsBuffering())
  {
    int status = si->m_decoder2.GetStatus();
    if (status == STATUS_ENDED   ||
        status == STATUS_NO_FILE ||
        si->m_decoder2.ReadSamples(PACKET_SIZE) == RET_ERROR)
    {
      CLog::Log(LOGINFO, "PAPlayer::PrepareStream 2nd - Stream Finished");
      break;
    }

    if (!QueueData2(si))
      break;

    /* yield our time so that the main PAP thread doesnt stall */
    CThread::Sleep(1);
  }

  CLog::Log(LOGINFO, "PAPlayer::PrepareStream - Ready");

  return true;
}

bool PAPlayer::CloseFile()
{
  m_callback.OnPlayBackStopped();
  return true;
}

void PAPlayer::Process()
{
  if (!m_startEvent.WaitMSec(100))
  {
    CLog::Log(LOGDEBUG, "PAPlayer::Process - Failed to receive start event");
    return;
  }

  CLog::Log(LOGDEBUG, "PAPlayer::Process - Playback started");  
  while(m_isPlaying && !m_bStop)
  {
    /* this needs to happen outside of any locks to prevent deadlocks */
    if (m_signalSpeedChange)
    {
      m_callback.OnPlayBackSpeedChanged(m_playbackSpeed);
      m_signalSpeedChange = false;
    }

    double delay  = 100.0;
    double buffer = 100.0;
    ProcessStreams(delay, buffer);

    double watermark = buffer * 0.5;
#if defined(TARGET_DARWIN)
    // In CoreAudio the delay can be bigger then the buffer
    // because of delay from the HAL/Hardware
    // This is the case when the buffer is full (e.x. 1 sec)
    // and there is a HAL-Delay. In that case we would never sleep
    // but load one cpu core up to 100% (happens on osx/ios whenever
    // the first stream is finished and a prebuffered second stream
    // starts to play. A BIG FIXME HERE.
    if ((delay < buffer || buffer == 1) && delay > watermark)
#else
    if ((delay < buffer) && delay > watermark)
#endif
      CThread::Sleep(MathUtils::round_int((delay - watermark) * 1000.0));

    GetTimeInternal(); //update for GUI

    SyncStreams2();
  }
}

inline void PAPlayer::ProcessStreams(double &delay, double &buffer)
{
  CSharedLock sharedLock(m_streamsLock);
  if (m_isFinished && m_streams.empty() && m_finishing.empty())
  {
    m_isPlaying = false;
    delay       = 0;
    m_callback.OnPlayBackEnded();
    return;
  }

  /* destroy any drained streams */
  for(StreamList::iterator itt = m_finishing.begin(); itt != m_finishing.end();)
  {
    StreamInfo* si = *itt;
    if (si->m_stream->IsDrained())
    {      
      itt = m_finishing.erase(itt);
      CAEFactory::FreeStream(si->m_stream);
      if(m_bAudio2)
        CAEFactory::FreeStream(si->m_stream2);
      delete si;
      CLog::Log(LOGDEBUG, "PAPlayer::ProcessStreams - Stream Freed");
    }
    else
      ++itt;
  }

  sharedLock.Leave();
  CExclusiveLock lock(m_streamsLock);

  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
  {
    StreamInfo* si = *itt;
    if (!m_currentStream && !si->m_started)
    {
      m_currentStream = si;
      CLog::Log(LOGDEBUG, "PAPlayer::ProcessStreams - Stream switched");
      UpdateGUIData(si); //update for GUI
    }
    /* if the stream is finishing */
    if ((si->m_playNextTriggered && si->m_stream && !si->m_stream->IsFading()) || !ProcessStream(si, delay, buffer))
    {
      if (!si->m_prepareTriggered)
      {
        si->m_prepareTriggered = true;
        m_callback.OnQueueNextItem();
      }

      /* remove the stream */
      itt = m_streams.erase(itt);
      /* if its the current stream */
      if (si == m_currentStream)
      {
        /* if it was the last stream */
        if (itt == m_streams.end())
        {
          /* if it didnt trigger the next queue item */
          if (!si->m_prepareTriggered)
          {
            m_callback.OnQueueNextItem();
            si->m_prepareTriggered = true;
          }
          m_currentStream = NULL;
        }
        else
        {
          m_currentStream = *itt;
          UpdateGUIData(*itt); //update for GUI
        }
      }

      /* unregister the audio callback */
      si->m_stream->UnRegisterAudioCallback();
      si->m_decoder.Destroy();      
      si->m_stream->Drain();
      if(m_bAudio2)
      {
        if (si->m_usedecoder2)
        {
          si->m_decoder2.Destroy();
          si->m_usedecoder2 = false;
        }
        si->m_stream2->Drain();
      }
      m_finishing.push_back(si);
      return;
    }

    if (!si->m_started)
      continue;

    /* is it time to prepare the next stream? */
    if (si->m_prepareNextAtFrame > 0 && !si->m_prepareTriggered && si->m_framesSent >= si->m_prepareNextAtFrame)
    {
      si->m_prepareTriggered = true;
      m_callback.OnQueueNextItem();
    }

    /* it is time to start playing the next stream? */
    if (si->m_playNextAtFrame > 0 && !si->m_playNextTriggered && si->m_framesSent >= si->m_playNextAtFrame)
    {
      if (!si->m_prepareTriggered)
      {
        si->m_prepareTriggered = true;
        m_callback.OnQueueNextItem();
      }

      if (!m_isFinished)
      {
        if (m_upcomingCrossfadeMS)
        {
          si->m_stream->FadeVolume(1.0f, 0.0f, m_upcomingCrossfadeMS);
          if(m_bAudio2)
            si->m_stream2->FadeVolume(1.0f, 0.0f, m_upcomingCrossfadeMS);
          si->m_fadeOutTriggered = true;
        }
        m_currentStream = NULL;

        /* unregister the audio callback */
        si->m_stream->UnRegisterAudioCallback();
      }

      si->m_playNextTriggered = true;
    }
  }
}

inline bool PAPlayer::ProcessStream(StreamInfo *si, double &delay, double &buffer)
{
  /* if playback needs to start on this stream, do it */
  if (si == m_currentStream && !si->m_started)
  {
    si->m_started = true;
    si->m_stream->RegisterAudioCallback(m_audioCallback);
    if (!si->m_isSlaved)
      si->m_stream->Resume();
    si->m_stream->FadeVolume(0.0f, 1.0f, m_upcomingCrossfadeMS);
    if(m_bAudio2)
    {
      if (!si->m_isSlaved)
        si->m_stream2->Resume();
      si->m_stream2->FadeVolume(0.0f, 1.0f, m_upcomingCrossfadeMS);
    }
    m_callback.OnPlayBackStarted();
  }

  /* if we have not started yet and the stream has been primed */
  unsigned int space = si->m_stream->GetSpace();
  if (!si->m_started && !space)
    return true;

  /* see if it is time yet to FF/RW or a direct seek */
  if (!si->m_playNextTriggered && ((m_playbackSpeed != 1 && si->m_framesSent >= si->m_seekNextAtFrame) || si->m_seekFrame > -1))
  {
    int64_t time = (int64_t)0;
    /* if its a direct seek */
    if (si->m_seekFrame > -1)
    {
      time = (int64_t)((float)si->m_seekFrame / (float)si->m_sampleRate * 1000.0f);
      si->m_framesSent = (int)(si->m_seekFrame - ((float)si->m_startOffset * (float)si->m_sampleRate) / 1000.0f);
      si->m_framesSent2 = (int)(si->m_seekFrame2 - ((float)si->m_startOffset * (float)si->m_sampleRate2) / 1000.0f);
      si->m_seekFrame  = -1;
      si->m_seekFrame2  = -1;
      m_playerGUIData.m_time = time; //update for GUI
    }
    /* if its FF/RW */
    else
    {
      si->m_framesSent      += si->m_sampleRate * (m_playbackSpeed  - 1);
      si->m_framesSent2     += si->m_sampleRate2 * (m_playbackSpeed  - 1);
      si->m_seekNextAtFrame  = si->m_framesSent + si->m_sampleRate / 2;
      time = (int64_t)(((float)si->m_framesSent / (float)si->m_sampleRate * 1000.0f) + (float)si->m_startOffset);
    }

    /* if we are seeking back before the start of the track start normal playback */
    if (time < si->m_startOffset || si->m_framesSent < 0)
    {
      time = si->m_startOffset;
      si->m_framesSent      = (int)(si->m_startOffset * si->m_sampleRate / 1000);
      si->m_framesSent2     = (int)(si->m_startOffset * si->m_sampleRate2 / 1000);
      si->m_seekNextAtFrame = 0;
      ToFFRW(1);
    }

    si->m_decoder.Seek(time);
    if(si->m_usedecoder2)
      si->m_decoder2.Seek(time);
  }

  int status = si->m_decoder.GetStatus();
  if (status == STATUS_ENDED   ||
      status == STATUS_NO_FILE ||
      si->m_decoder.ReadSamples(PACKET_SIZE) == RET_ERROR ||
      ((si->m_endOffset) && (si->m_framesSent / si->m_sampleRate >= (si->m_endOffset - si->m_startOffset) / 1000)))
  {
    CLog::Log(LOGINFO, "PAPlayer::ProcessStream - Stream Finished");
    return false;
  }

  if (!QueueData(si))
    return false;

  if (si->m_usedecoder2)
  {
    si->m_decoder2.ReadSamples(PACKET_SIZE);
    QueueData2(si);
  }

  /* update the delay time if we are running */
  if (si->m_started)
  {
    if (si->m_stream->IsBuffering())
      delay = 0.0;
    else
      delay = std::min(delay , si->m_stream->GetDelay());
    buffer = std::min(buffer, si->m_stream->GetCacheTotal());
    if (m_bAudio2)
    {
      if (si->m_stream2->IsBuffering())
        delay = 0.0;
      else
        delay = std::min(delay , si->m_stream2->GetDelay());
      buffer = std::min(buffer, si->m_stream2->GetCacheTotal());
    }
  }

  return true;
}

bool PAPlayer::QueueData(StreamInfo *si)
{
  bool bAudio2 = m_bAudio2 && !si->m_usedecoder2 && si->m_stream2;
  unsigned int space   = si->m_stream->GetSpace();
  if (bAudio2)
    space = std::min(space, si->m_stream2->GetSpace());
  unsigned int samples = std::min(si->m_decoder.GetDataSize(), space / si->m_bytesPerSample);
  if (!samples)
    return true;

  void* data = si->m_decoder.GetData(samples);
  if (!data)
  {
    CLog::Log(LOGERROR, "PAPlayer::QueueData - Failed to get data from the decoder");
    return false;
  }

  unsigned int added = si->m_stream->AddData(data, samples * si->m_bytesPerSample);
  si->m_framesSent += added / si->m_bytesPerFrame;

  if (bAudio2)
  {
    if(samples > m_iAudio2DiscardSamples)
      samples -= m_iAudio2DiscardSamples;
    else
      samples = samples - (samples * si->m_bytesPerSample / si->m_bytesPerFrame * si->m_bytesPerFrame / si->m_bytesPerSample);
    if(samples)
      si->m_stream2->AddData(data, samples * si->m_bytesPerSample);
    si->m_framesSent2 = si->m_framesSent;
    m_iAudio2DiscardSamples = 0;
  }

  const ICodec* codec = si->m_decoder.GetCodec();
  m_playerGUIData.m_cacheLevel = codec ? codec->GetCacheLevel() : 0; //update for GUI

  return true;
}

bool PAPlayer::QueueData2(StreamInfo *si)
{
  if (!si->m_usedecoder2)
    return false;

  unsigned int space   = si->m_stream2->GetSpace();
  unsigned int samples = std::min(si->m_decoder2.GetDataSize(), space / si->m_bytesPerSample2);
  if (!samples)
    return true;

  void* data = si->m_decoder2.GetData(samples);
  if (!data)
  {
    CLog::Log(LOGERROR, "PAPlayer::QueueData 2nd - Failed to get data from the decoder");
    return false;
  }

  unsigned int added = si->m_stream2->AddData(data, samples * si->m_bytesPerSample2);
  si->m_framesSent2 += added / si->m_bytesPerFrame2;

  return true;
}

inline void PAPlayer::SyncStreams2()
{
  if(!m_bAudio2)
    return;

  if(CAEFactory::IsDumb() || CAEFactory::IsDumb(true))
    return;

  if(!m_currentStream || !m_currentStream->m_stream || !m_currentStream->m_stream2)
    return;

  if(m_currentStream->m_usedecoder2)
    return;

  if(m_playbackSpeed != 1)
    return;

  int iTimeSynced = (int)((double)clock()/CLOCKS_PER_SEC*1000);
  if(iTimeSynced - m_iTimeSynced < 50)
    return;
  m_iTimeSynced = iTimeSynced;

  double time1 = ((double)m_currentStream->m_framesSent / (double)m_currentStream->m_sampleRate);
  double time2 = ((double)m_currentStream->m_framesSent2 / (double)m_currentStream->m_sampleRate2);
  time1 -= m_currentStream->m_stream->GetDelay();
  time2 -= m_currentStream->m_stream2->GetDelay();
  double timediff = time2 - time1;

  m_iAudio2DiscardSamples = 0;
  if (timediff > 0.05)
  {
    unsigned int padsize = (unsigned int)(timediff * (double)m_currentStream->m_sampleRate2) * m_currentStream->m_bytesPerFrame2;
    if(padsize > m_currentStream->m_stream2->GetSpace())
      padsize = (m_currentStream->m_stream2->GetSpace() / m_currentStream->m_bytesPerFrame2) * m_currentStream->m_bytesPerFrame2;
    if(padsize)
    {
      void* padbuf = malloc(padsize);
      if(padbuf)
      {
        memset(padbuf, 0, padsize);
        m_currentStream->m_stream2->AddData(padbuf, padsize);
        free(padbuf);
      }
    }
  }
  else if(timediff < -0.05)
  {
    unsigned int discardsize = (unsigned int)(-timediff * (double)m_currentStream->m_sampleRate2) * m_currentStream->m_bytesPerFrame2;
	m_iAudio2DiscardSamples = discardsize / m_currentStream->m_bytesPerSample2;
    if (m_currentStream->m_usedecoder2)
    {
      if(m_iAudio2DiscardSamples > m_currentStream->m_decoder2.GetDataSize())
        m_currentStream->m_decoder2.ReadSamples(m_iAudio2DiscardSamples - m_currentStream->m_decoder2.GetDataSize());
      m_iAudio2DiscardSamples = std::min(m_currentStream->m_decoder2.GetDataSize(), m_iAudio2DiscardSamples);
	  m_iAudio2DiscardSamples = m_iAudio2DiscardSamples * m_currentStream->m_bytesPerSample2 / m_currentStream->m_bytesPerFrame2 * m_currentStream->m_bytesPerFrame2 / m_currentStream->m_bytesPerSample2;
      if (m_iAudio2DiscardSamples)
      {
        m_currentStream->m_decoder2.GetData(m_iAudio2DiscardSamples);
        m_currentStream->m_framesSent2 += (m_iAudio2DiscardSamples * m_currentStream->m_bytesPerSample2 / m_currentStream->m_bytesPerFrame2);
      }
      m_iAudio2DiscardSamples = 0;
    }
  }
}

void PAPlayer::OnExit()
{

}

void PAPlayer::RegisterAudioCallback(IAudioCallback* pCallback)
{
  CSharedLock lock(m_streamsLock);
  m_audioCallback = pCallback;
  if (m_currentStream && m_currentStream->m_stream)
    m_currentStream->m_stream->RegisterAudioCallback(pCallback);
}

void PAPlayer::UnRegisterAudioCallback()
{
  CSharedLock lock(m_streamsLock);
  /* only one stream should have the callback, but we do it to all just incase */
  for(StreamList::iterator itt = m_streams.begin(); itt != m_streams.end(); ++itt)
    if ((*itt)->m_stream)
      (*itt)->m_stream->UnRegisterAudioCallback();
  m_audioCallback = NULL;
}

void PAPlayer::OnNothingToQueueNotify()
{
  m_isFinished = true;
}

bool PAPlayer::IsPlaying() const
{
  return m_isPlaying;
}

bool PAPlayer::IsPaused() const
{
  return m_isPaused;
}

void PAPlayer::Pause()
{
  if (m_isPaused)
  {
    m_isPaused = false;
    SoftStart();
    m_callback.OnPlayBackResumed();
  }
  else
  {
    m_isPaused = true;    
    SoftStop(true, false);
    m_callback.OnPlayBackPaused();
  }
}

void PAPlayer::SetVolume(float volume)
{

}

void PAPlayer::SetDynamicRangeCompression(long drc)
{

}

void PAPlayer::ToFFRW(int iSpeed)
{
  m_playbackSpeed     = iSpeed;
  m_signalSpeedChange = true;
}

int64_t PAPlayer::GetTimeInternal()
{
  CSharedLock lock(m_streamsLock);
  if (!m_currentStream)
    return 0;

  double time = ((double)m_currentStream->m_framesSent / (double)m_currentStream->m_sampleRate);
  if (m_currentStream->m_stream)
    time -= m_currentStream->m_stream->GetDelay();
  time = time * 1000.0;

  m_playerGUIData.m_time = (int64_t)time; //update for GUI

  return (int64_t)time;
}

int64_t PAPlayer::GetTime()
{
  return m_playerGUIData.m_time;
}

int64_t PAPlayer::GetTotalTime64()
{
  CSharedLock lock(m_streamsLock);
  if (!m_currentStream)
    return 0;

  int64_t total = m_currentStream->m_decoder.TotalTime();
  if (m_currentStream->m_endOffset)
    total = m_currentStream->m_endOffset;
  total -= m_currentStream->m_startOffset;
  return total;
}

int64_t PAPlayer::GetTotalTime()
{
  return m_playerGUIData.m_totalTime;
}

int PAPlayer::GetCacheLevel() const
{
  return m_playerGUIData.m_cacheLevel;
}

int PAPlayer::GetChannels()
{
  return m_playerGUIData.m_channelCount;
}

int PAPlayer::GetBitsPerSample()
{
  return m_playerGUIData.m_bitsPerSample;
}

int PAPlayer::GetSampleRate()
{
  return m_playerGUIData.m_sampleRate;
}

CStdString PAPlayer::GetAudioCodecName()
{
  return m_playerGUIData.m_codec;
}

int PAPlayer::GetAudioBitrate()
{
  return m_playerGUIData.m_audioBitrate;
}

bool PAPlayer::CanSeek()
{
  return m_playerGUIData.m_canSeek;
}

void PAPlayer::Seek(bool bPlus, bool bLargeStep)
{
  if (!CanSeek()) return;

  __int64 seek;
  if (g_advancedSettings.m_musicUseTimeSeeking && GetTotalTime() > 2 * g_advancedSettings.m_musicTimeSeekForwardBig)
  {
    if (bLargeStep)
      seek = bPlus ? g_advancedSettings.m_musicTimeSeekForwardBig : g_advancedSettings.m_musicTimeSeekBackwardBig;
    else
      seek = bPlus ? g_advancedSettings.m_musicTimeSeekForward : g_advancedSettings.m_musicTimeSeekBackward;
    seek *= 1000;
    seek += GetTime();
  }
  else
  {
    float percent;
    if (bLargeStep)
      percent = bPlus ? (float)g_advancedSettings.m_musicPercentSeekForwardBig : (float)g_advancedSettings.m_musicPercentSeekBackwardBig;
    else
      percent = bPlus ? (float)g_advancedSettings.m_musicPercentSeekForward : (float)g_advancedSettings.m_musicPercentSeekBackward;
    seek = (__int64)(GetTotalTime64() * (GetPercentage() + percent) / 100);
  }

  SeekTime(seek);
}

void PAPlayer::SeekTime(int64_t iTime /*=0*/)
{
  if (!CanSeek()) return;

  CSharedLock lock(m_streamsLock);
  if (!m_currentStream)
    return;

  int seekOffset = (int)(iTime - GetTimeInternal());

  if (m_playbackSpeed != 1)
    ToFFRW(1);

  m_currentStream->m_seekFrame = (int)((float)m_currentStream->m_sampleRate * ((float)iTime + (float)m_currentStream->m_startOffset) / 1000.0f);
  m_currentStream->m_seekFrame2 = (int)((float)m_currentStream->m_sampleRate2 * ((float)iTime + (float)m_currentStream->m_startOffset) / 1000.0f);
  m_callback.OnPlayBackSeek((int)iTime, seekOffset);
}

void PAPlayer::SeekPercentage(float fPercent /*=0*/)
{
  if (fPercent < 0.0f  ) fPercent = 0.0f;
  if (fPercent > 100.0f) fPercent = 100.0f;
  SeekTime((int64_t)(fPercent * 0.01f * (float)GetTotalTime64()));
}

float PAPlayer::GetPercentage()
{
  if (m_playerGUIData.m_totalTime > 0)
    return m_playerGUIData.m_time * 100.0f / m_playerGUIData.m_totalTime;

  return 0.0f;
}

bool PAPlayer::SkipNext()
{
  return false;
}

void PAPlayer::UpdateGUIData(StreamInfo *si)
{
  /* Store data need by external threads in member
   * structure to prevent locking conflicts when
   * data required by GUI and main application
   */
  CSharedLock lock(m_streamsLock);

  m_playerGUIData.m_sampleRate    = si->m_sampleRate;
  m_playerGUIData.m_bitsPerSample = si->m_bytesPerSample << 3;
  m_playerGUIData.m_channelCount  = si->m_channelInfo.Count();
  m_playerGUIData.m_canSeek       = si->m_decoder.CanSeek();

  const ICodec* codec = si->m_decoder.GetCodec();

  m_playerGUIData.m_audioBitrate = codec ? codec->m_Bitrate : 0;
  strncpy(m_playerGUIData.m_codec,codec ? codec->m_CodecName : "",20);
  m_playerGUIData.m_cacheLevel   = codec ? codec->GetCacheLevel() : 0;

  int64_t total = si->m_decoder.TotalTime();
  if (si->m_endOffset)
    total = m_currentStream->m_endOffset;
  total -= m_currentStream->m_startOffset;
  m_playerGUIData.m_totalTime = total;
}
