/*
 *      Copyright (C) 2010-2015 Team Kodi
 *      http://kodi.tv
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
 *  along with Kodi; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"

#include "AEFactory.h"

#include "Engines/ActiveAE/ActiveAE.h"
#include "Utils/AEStreamInfo.h"

#include "guilib/LocalizeStrings.h"
#include "settings/lib/Setting.h"
#include "settings/Settings.h"
#include "utils/StringUtils.h"

IAE* CAEFactory::AE = NULL;
IAE* CAEFactory::AE2 = NULL;
static float  g_fVolume = 1.0f;
static bool   g_bMute = false;

IAE *CAEFactory::GetEngine(bool bAudio2)
{
  if(!bAudio2)
    return AE;
  else
    return AE2;
}

bool CAEFactory::LoadEngine()
{
  /* can only load the engine once, XBMC restart is required to change it */
  if (AE)
    return false;

  AE = new ActiveAE::CActiveAE();

  if (AE && !AE->CanInit())
  {
    delete AE;
    AE = NULL;
  }

  if (!AE2)
  {
    AE2 = new ActiveAE::CActiveAE();

    if (AE2)
        AE2->SetAudio2(true);

    if (AE2 && !AE2->CanInit())
    {
      delete AE2;
      AE2 = NULL;
    }
  }

  return AE != NULL;
}

void CAEFactory::UnLoadEngine()
{
  if(AE)
  {
    AE->Shutdown();
    delete AE;
    AE = NULL;
  }
  if(AE2)
  {
    AE2->Shutdown();
    delete AE2;
    AE2 = NULL;
  }
}

bool CAEFactory::StartEngine()
{
  if (!AE)
    return false;

  if (AE->Initialize())
  {
    if (AE2)
    {
      if(!AE2->Initialize())
      {
        delete AE2;
        AE2 = NULL;
      }
    }
    return true;
  }

  delete AE;
  AE = NULL;
  return false;
}

bool CAEFactory::Suspend()
{
  bool bRet = false;
  if(AE)
    bRet = AE->Suspend();
  if (AE2)
    AE2->Suspend();

  return bRet;
}

bool CAEFactory::Resume()
{
  bool bRet = false;
  if(AE)
    bRet = AE->Resume();
  if (AE2)
    AE2->Resume();

  return bRet;
}

bool CAEFactory::IsSuspended()
{
  if(AE)
    return AE->IsSuspended();

  /* No engine to process audio */
  return true;
}

/* engine wrapping */
IAESound *CAEFactory::MakeSound(const std::string &file, bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->MakeSound(file);
  if(bAudio2 && AE2)
    return AE2->MakeSound(file);
  
  return NULL;
}

void CAEFactory::FreeSound(IAESound *sound)
{
  if(!sound)
    return;
  bool bAudio2 = sound->IsAudio2();

  if(!bAudio2 && AE)
    AE->FreeSound(sound);
  if(bAudio2 && AE2)
    AE2->FreeSound(sound);
}

void CAEFactory::SetSoundMode(const int mode, bool bAudio2)
{
  if(!bAudio2 && AE)
    AE->SetSoundMode(mode);
  if(bAudio2 && AE2)
    AE2->SetSoundMode(mode);
}

void CAEFactory::OnSettingsChange(std::string setting, bool bAudio2)
{
  if(!bAudio2 && AE)
    AE->OnSettingsChange(setting);
  if(bAudio2 && AE2)
    AE2->OnSettingsChange(setting);
}

void CAEFactory::EnumerateOutputDevices(AEDeviceList &devices, bool passthrough, bool bAudio2)
{
  if(!bAudio2 && AE)
    AE->EnumerateOutputDevices(devices, passthrough);
  if(bAudio2 && AE2)
    AE2->EnumerateOutputDevices(devices, passthrough);
}

void CAEFactory::VerifyOutputDevice(std::string &device, bool passthrough)
{
  AEDeviceList devices;
  EnumerateOutputDevices(devices, passthrough);
  std::string firstDevice;

  for (AEDeviceList::const_iterator deviceIt = devices.begin(); deviceIt != devices.end(); ++deviceIt)
  {
    /* remember the first device so we can default to it if required */
    if (firstDevice.empty())
      firstDevice = deviceIt->second;

    if (deviceIt->second == device)
      return;
    else if (deviceIt->first == device)
    {
      device = deviceIt->second;
      return;
    }
  }

  /* if the device wasnt found, set it to the first viable output */
  device = firstDevice;
}

std::string CAEFactory::GetDefaultDevice(bool passthrough, bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->GetDefaultDevice(passthrough);
  if(bAudio2 && AE2)
    return AE2->GetDefaultDevice(passthrough);

  return "default";
}

std::string CAEFactory::GetCreateDevice(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->GetCreateDevice();
  if(bAudio2 && AE2)
    return AE2->GetCreateDevice();

  return "";
}

bool CAEFactory::SupportsRaw(AEAudioFormat &format, bool bAudio2)
{
  // check if passthrough is enabled
  if (!CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_PASSTHROUGH))
    return false;

  // fixed config disabled passthrough
  if (CSettings::GetInstance().GetInt(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_CONFIG : CSettings::SETTING_AUDIOOUTPUT2_CONFIG) == AE_CONFIG_FIXED)
    return false;

  // check if the format is enabled in settings
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_AC3 && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_AC3PASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_512 && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_DTSPASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_1024 && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_DTSPASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTS_2048 && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_DTSPASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_DTSPASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_EAC3PASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_TRUEHDPASSTHROUGH))
    return false;
  if (format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD && !CSettings::GetInstance().GetBool(!bAudio2 ? CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH : CSettings::SETTING_AUDIOOUTPUT2_DTSHDPASSTHROUGH))
    return false;

  if(!bAudio2 && AE)
    return AE->SupportsRaw(format);
  if(bAudio2 && AE2)
    return AE2->SupportsRaw(format);

  return false;
}

bool CAEFactory::SupportsSilenceTimeout(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->SupportsSilenceTimeout();
  if(bAudio2 && AE2)
    return AE2->SupportsSilenceTimeout();

  return false;
}

bool CAEFactory::HasStereoAudioChannelCount(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->HasStereoAudioChannelCount();
  if(bAudio2 && AE2)
    return AE2->HasStereoAudioChannelCount();

  return false;
}

bool CAEFactory::HasHDAudioChannelCount(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->HasHDAudioChannelCount();
  if(bAudio2 && AE2)
    return AE2->HasHDAudioChannelCount();

  return false;
}

/**
  * Returns true if current AudioEngine supports at lest two basic quality levels
  * @return true if quality setting is supported, otherwise false
  */
bool CAEFactory::SupportsQualitySetting(bool bAudio2) 
{
  if (!bAudio2 && AE)
    return ((AE->SupportsQualityLevel(AE_QUALITY_LOW)? 1 : 0) + 
            (AE->SupportsQualityLevel(AE_QUALITY_MID)? 1 : 0) +
            (AE->SupportsQualityLevel(AE_QUALITY_HIGH)? 1 : 0)) >= 2; 
  if (bAudio2 && AE2)
    return ((AE2->SupportsQualityLevel(AE_QUALITY_LOW)? 1 : 0) + 
            (AE2->SupportsQualityLevel(AE_QUALITY_MID)? 1 : 0) +
            (AE2->SupportsQualityLevel(AE_QUALITY_HIGH)? 1 : 0)) >= 2; 

  return false;
}
  
void CAEFactory::SetMute(const bool enabled)
{
  if(AE)
    AE->SetMute(enabled);
  if(AE2)
    AE2->SetMute(enabled);

  g_bMute = enabled;
}

bool CAEFactory::IsMuted()
{
  if(AE)
    return AE->IsMuted();

  return g_bMute || (g_fVolume == 0.0f);
}

bool CAEFactory::IsDumb(bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->IsDumb();
  if(bAudio2 && AE2)
    return AE2->IsDumb();

  return true;
}

float CAEFactory::GetVolume()
{
  if(AE)
    return AE->GetVolume();

  return g_fVolume;
}

void CAEFactory::SetVolume(const float volume)
{
  if(AE)
  {
    AE->SetVolume(volume);
    AE2->SetVolume(volume);
  }
  else
    g_fVolume = volume;
}

void CAEFactory::Shutdown()
{
  if(AE)
    AE->Shutdown();
  if(AE2)
    AE2->Shutdown();
}

IAEStream *CAEFactory::MakeStream(AEAudioFormat &audioFormat, unsigned int options, IAEClockCallback *clock, bool bAudio2)
{
  if(!bAudio2 && AE)
    return AE->MakeStream(audioFormat, options, clock);
  if(bAudio2 && AE2)
    return AE2->MakeStream(audioFormat, options, clock);

  return NULL;
}

bool CAEFactory::FreeStream(IAEStream *stream)
{
  if(!stream)
    return false;
  bool bAudio2 = stream->IsAudio2();

  if(!bAudio2 && AE)
    return AE->FreeStream(stream);
  if(bAudio2 && AE2)
    return AE2->FreeStream(stream);

  return false;
}

void CAEFactory::GarbageCollect()
{
  if(AE)
    AE->GarbageCollect();
  if(AE2)
    AE2->GarbageCollect();
}

void CAEFactory::SettingOptionsAudioDevicesFiller(const CSetting *setting, std::vector< std::pair<std::string, std::string> > &list, std::string &current, void *data)
{
  SettingOptionsAudioDevicesFillerGeneral(setting, list, current, false);
}

void CAEFactory::SettingOptionsAudioDevicesPassthroughFiller(const CSetting *setting, std::vector< std::pair<std::string, std::string> > &list, std::string &current, void *data)
{
  SettingOptionsAudioDevicesFillerGeneral(setting, list, current, true);
}

void CAEFactory::SettingOptionsAudioQualityLevelsFiller(const CSetting *setting, std::vector< std::pair<std::string, int> > &list, int &current, void *data)
{
  IAE* _AE = StringUtils::StartsWithNoCase(setting->GetId(), "audiooutput2.") ? AE2 : AE;
	
  if (!_AE)
    return;

  if(_AE->SupportsQualityLevel(AE_QUALITY_LOW))
    list.push_back(std::make_pair(g_localizeStrings.Get(13506), AE_QUALITY_LOW));
  if(_AE->SupportsQualityLevel(AE_QUALITY_MID))
    list.push_back(std::make_pair(g_localizeStrings.Get(13507), AE_QUALITY_MID));
  if(_AE->SupportsQualityLevel(AE_QUALITY_HIGH))
    list.push_back(std::make_pair(g_localizeStrings.Get(13508), AE_QUALITY_HIGH));
  if(_AE->SupportsQualityLevel(AE_QUALITY_REALLYHIGH))
    list.push_back(std::make_pair(g_localizeStrings.Get(13509), AE_QUALITY_REALLYHIGH));
  if(_AE->SupportsQualityLevel(AE_QUALITY_GPU))
    list.push_back(std::make_pair(g_localizeStrings.Get(38010), AE_QUALITY_GPU));
}

void CAEFactory::SettingOptionsAudioStreamsilenceFiller(const CSetting *setting, std::vector< std::pair<std::string, int> > &list, int &current, void *data)
{
  IAE* _AE = StringUtils::StartsWithNoCase(setting->GetId(), "audiooutput2.") ? AE2 : AE;

  if (!_AE)
    return;

  list.push_back(std::make_pair(g_localizeStrings.Get(20422), XbmcThreads::EndTime::InfiniteValue));
  list.push_back(std::make_pair(g_localizeStrings.Get(13551), 0));

  if (_AE->SupportsSilenceTimeout())
  {
    list.push_back(std::make_pair(StringUtils::Format(g_localizeStrings.Get(13554).c_str(), 1), 1));
    for (int i = 2; i <= 10; i++)
    {
      list.push_back(std::make_pair(StringUtils::Format(g_localizeStrings.Get(13555).c_str(), i), i));
    }
  }
}

void CAEFactory::SettingOptionsAudioDevicesFillerGeneral(const CSetting *setting, std::vector< std::pair<std::string, std::string> > &list, std::string &current, bool passthrough)
{
  current = ((const CSettingString*)setting)->GetValue();
  std::string firstDevice;

  bool foundValue = false;
  AEDeviceList sinkList;
  EnumerateOutputDevices(sinkList, passthrough);
  if (sinkList.empty())
    list.push_back(std::make_pair("Error - no devices found", "error"));
  else
  {
    for (AEDeviceList::const_iterator sink = sinkList.begin(); sink != sinkList.end(); ++sink)
    {
      if (sink == sinkList.begin())
        firstDevice = sink->second;

      list.push_back(std::make_pair(sink->first, sink->second));

      if (StringUtils::EqualsNoCase(current, sink->second))
        foundValue = true;
    }
  }

  if (!foundValue)
    current = firstDevice;
}

void CAEFactory::RegisterAudioCallback(IAudioCallback* pCallback)
{
  if (AE)
    AE->RegisterAudioCallback(pCallback);
}

void CAEFactory::UnregisterAudioCallback(IAudioCallback* pCallback)
{
  if (AE)
    AE->UnregisterAudioCallback(pCallback);
}

bool CAEFactory::IsSettingVisible(const std::string &condition, const std::string &value, const CSetting *setting, void *data)
{
  IAE* _AE = StringUtils::StartsWithNoCase(setting->GetId(), "audiooutput2.") ? AE2 : AE;

  if (setting == NULL || value.empty() || !_AE)
    return false;

  return _AE->IsSettingVisible(value);
}

void CAEFactory::KeepConfiguration(unsigned int millis)
{
  if (AE)
    AE->KeepConfiguration(millis);
  if (AE2)
    AE2->KeepConfiguration(millis);
}

void CAEFactory::DeviceChange()
{
  if (AE)
    AE->DeviceChange();
  if (AE2)
    AE2->DeviceChange();
}
