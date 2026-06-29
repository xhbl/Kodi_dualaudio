/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIAudioManager.h"

#include "ServiceBroker.h"
#include "addons/AddonManager.h"
#include "addons/Skin.h"
#include "addons/addoninfo/AddonType.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "filesystem/Directory.h"
#include "input/WindowTranslator.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "input/actions/ActionTranslator.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "utils/URIUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/log.h"

#include <mutex>

using namespace KODI;

CGUIAudioManager::CGUIAudioManager()
  : m_settings(CServiceBroker::GetSettingsComponent()->GetSettings())
{
  m_bEnabled = false;
  m_bAudio2 = false;

  m_settings->RegisterCallback(this, {CSettings::SETTING_LOOKANDFEEL_SOUNDSKIN,
                                      CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDVOLUME,
                                      CSettings::SETTING_AUDIOOUTPUT_GUISOUNDVOLUME});
}

CGUIAudioManager::~CGUIAudioManager()
{
  m_settings->UnregisterCallback(this);
}

void CGUIAudioManager::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (setting == NULL)
    return;

  const std::string &settingId = setting->GetId();
  if (settingId == CSettings::SETTING_LOOKANDFEEL_SOUNDSKIN)
  {
    Enable(true);
    Load();
  }
}

bool CGUIAudioManager::OnSettingUpdate(const std::shared_ptr<CSetting>& setting,
                                       const char* oldSettingId,
                                       const TiXmlNode* oldSettingNode)
{
  if (setting == NULL)
    return false;

  if (setting->GetId() == CSettings::SETTING_LOOKANDFEEL_SOUNDSKIN)
  {
    //Migrate the old settings
    if (std::static_pointer_cast<CSettingString>(setting)->GetValue() == "SKINDEFAULT")
      std::static_pointer_cast<CSettingString>(setting)->Reset();
    else if (std::static_pointer_cast<CSettingString>(setting)->GetValue() == "OFF")
      std::static_pointer_cast<CSettingString>(setting)->SetValue("");
  }
  if (setting->GetId() == CSettings::SETTING_AUDIOOUTPUT_GUISOUNDVOLUME)
  {
    int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDVOLUME);
    SetVolume(0.01f * vol);
  }
  if (setting->GetId() == CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDVOLUME)
  {
    int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDVOLUME);
    SetVolume(0.01f * vol, true);
  }
  return true;
}


void CGUIAudioManager::Initialize()
{
}

void CGUIAudioManager::DeInitialize()
{
  std::unique_lock<CCriticalSection> lock(m_cs);
  UnLoad();
}

void CGUIAudioManager::Stop()
{
  std::unique_lock<CCriticalSection> lock(m_cs);
  for (const auto& windowSound : m_windowSoundMap)
  {
    if (windowSound.second.initSound)
      windowSound.second.initSound->Stop();
    if (windowSound.second.deInitSound)
      windowSound.second.deInitSound->Stop();
    if (windowSound.second.initSound2)
      windowSound.second.initSound2->Stop();
    if (windowSound.second.deInitSound2)
      windowSound.second.deInitSound2->Stop();
  }

  for (const auto& pythonSound : m_pythonSounds)
  {
    pythonSound.second.sound->Stop();
    if (pythonSound.second.sound2)
      pythonSound.second.sound2->Stop();
  }
}

// \brief Play a sound associated with a CAction
void CGUIAudioManager::PlayActionSound(const CAction& action)
{
  std::unique_lock<CCriticalSection> lock(m_cs);

  // it's not possible to play gui sounds when passthrough is active
  if (!m_bEnabled)
    return;

  const auto it = m_actionSoundMap.find(action.GetID());
  if (it == m_actionSoundMap.end())
    return;

  CheckAudio2();

  if (it->second.sound)
  {
    int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDVOLUME);
    it->second.sound->SetVolume(0.01f * vol);
    it->second.sound->Play();
  }
  if (m_bAudio2 && it->second.sound2)
  {
    int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDVOLUME);
    it->second.sound2->SetVolume(0.01f * vol);
    it->second.sound2->Play();
  }
}

// \brief Play a sound associated with a window and its event
// Events: SOUND_INIT, SOUND_DEINIT
void CGUIAudioManager::PlayWindowSound(int id, WINDOW_SOUND event)
{
  std::unique_lock<CCriticalSection> lock(m_cs);

  // it's not possible to play gui sounds when passthrough is active
  if (!m_bEnabled)
    return;

  const auto it = m_windowSoundMap.find(id);
  if (it==m_windowSoundMap.end())
    return;

  CheckAudio2();

  std::shared_ptr<IAESound> sound;
  std::shared_ptr<IAESound> sound2;
  switch (event)
  {
    case SOUND_INIT:
      sound = it->second.initSound;
      sound2 = it->second.initSound2;
      break;
    case SOUND_DEINIT:
      sound = it->second.deInitSound;
      sound2 = it->second.deInitSound2;
      break;
  }

  if (!sound)
    return;

  int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDVOLUME);
  sound->SetVolume(0.01f * vol);
  sound->Play();

  if (m_bAudio2 && sound2)
  {
    int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDVOLUME);
    sound2->SetVolume(0.01f * vol);
    sound2->Play();
  }
}

// \brief Play a sound given by filename
void CGUIAudioManager::PlayPythonSound(const std::string& strFileName, bool useCached /*= true*/)
{
  std::unique_lock<CCriticalSection> lock(m_cs);

  // it's not possible to play gui sounds when passthrough is active
  if (!m_bEnabled)
    return;

  CheckAudio2();

  // If we already loaded the sound, just play it
  const auto itsb = m_pythonSounds.find(strFileName);
  if (itsb != m_pythonSounds.end())
  {
    const auto& sound = itsb->second.sound;
    if (useCached)
    {
      sound->Play();
      const auto& sound2 = itsb->second.sound2;
      if (m_bAudio2 && sound2) sound2->Play();
      return;
    }
    else
    {
      m_pythonSounds.erase(itsb);
    }
  }

  CAPSounds aps = LoadSound(strFileName);
  if (!aps.sound)
    return;

  int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT_GUISOUNDVOLUME);
  aps.sound->SetVolume(0.01f * vol);
  aps.sound->Play();

  if (m_bAudio2 && aps.sound2)
  {
    int vol = m_settings->GetInt(CSettings::SETTING_AUDIOOUTPUT2_GUISOUNDVOLUME);
    aps.sound2->SetVolume(0.01f * vol);
    aps.sound2->Play();
  }

  m_pythonSounds.emplace(strFileName, std::move(aps));
}

void CGUIAudioManager::UnLoad()
{
  m_windowSoundMap.clear();
  m_pythonSounds.clear();
  m_actionSoundMap.clear();
  m_soundCache.clear();
}


std::string GetSoundSkinPath()
{
  auto setting = std::static_pointer_cast<CSettingString>(CServiceBroker::GetSettingsComponent()->GetSettings()->GetSetting(CSettings::SETTING_LOOKANDFEEL_SOUNDSKIN));
  auto value = setting->GetValue();
  if (value.empty())
    return "";

  ADDON::AddonPtr addon;
  if (!CServiceBroker::GetAddonMgr().GetAddon(value, addon, ADDON::AddonType::RESOURCE_UISOUNDS,
                                              ADDON::OnlyEnabled::CHOICE_YES))
  {
    CLog::Log(LOGINFO, "Unknown sounds addon '{}'. Setting default sounds.", value);
    setting->Reset();
  }
  return URIUtils::AddFileToFolder("resource://", setting->GetValue());
}


// \brief Load the config file (sounds.xml) for nav sounds
bool CGUIAudioManager::Load()
{
  std::unique_lock<CCriticalSection> lock(m_cs);
  UnLoad();
  CheckAudio2();

  m_strMediaDir = GetSoundSkinPath();
  if (m_strMediaDir.empty())
    return true;

  Enable(true);
  std::string strSoundsXml = URIUtils::AddFileToFolder(m_strMediaDir, "sounds.xml");

  //  Load our xml file
  CXBMCTinyXML xmlDoc;

  CLog::Log(LOGINFO, "Loading {}", strSoundsXml);

  //  Load the config file
  if (!xmlDoc.LoadFile(strSoundsXml))
  {
    CLog::Log(LOGINFO, "{}, Line {}\n{}", strSoundsXml, xmlDoc.ErrorRow(), xmlDoc.ErrorDesc());
    return false;
  }

  TiXmlElement* pRoot = xmlDoc.RootElement();
  std::string strValue = pRoot->Value();
  if ( strValue != "sounds")
  {
    CLog::Log(LOGINFO, "{} Doesn't contain <sounds>", strSoundsXml);
    return false;
  }

  //  Load sounds for actions
  TiXmlElement* pActions = pRoot->FirstChildElement("actions");
  if (pActions)
  {
    TiXmlNode* pAction = pActions->FirstChild("action");

    while (pAction)
    {
      TiXmlNode* pIdNode = pAction->FirstChild("name");
      unsigned int id = ACTION_NONE;    // action identity
      if (pIdNode && pIdNode->FirstChild())
      {
        ACTION::CActionTranslator::TranslateString(pIdNode->FirstChild()->Value(), id);
      }

      TiXmlNode* pFileNode = pAction->FirstChild("file");
      std::string strFile;
      if (pFileNode && pFileNode->FirstChild())
        strFile += pFileNode->FirstChild()->Value();

      if (id != ACTION_NONE && !strFile.empty())
      {
        std::string filename = URIUtils::AddFileToFolder(m_strMediaDir, strFile);
        CAPSounds aps = LoadSound(filename);
        if (aps.sound)
          m_actionSoundMap.emplace(id, std::move(aps));
      }

      pAction = pAction->NextSibling();
    }
  }

  //  Load window specific sounds
  TiXmlElement* pWindows = pRoot->FirstChildElement("windows");
  if (pWindows)
  {
    TiXmlNode* pWindow = pWindows->FirstChild("window");

    while (pWindow)
    {
      int id = 0;

      TiXmlNode* pIdNode = pWindow->FirstChild("name");
      if (pIdNode)
      {
        if (pIdNode->FirstChild())
          id = CWindowTranslator::TranslateWindow(pIdNode->FirstChild()->Value());
      }

      CWindowSounds sounds;
      CAPSounds aps;
      aps = LoadWindowSound(pWindow, "activate");
      sounds.initSound = aps.sound;
      sounds.initSound2 = aps.sound2;
      aps = LoadWindowSound(pWindow, "deactivate");
      sounds.deInitSound = aps.sound;
      sounds.deInitSound2 = aps.sound2;

      if (id > 0)
        m_windowSoundMap.insert(std::pair<int, CWindowSounds>(id, sounds));

      pWindow = pWindow->NextSibling();
    }
  }

  return true;
}

CGUIAudioManager::CAPSounds CGUIAudioManager::LoadSound(const std::string& filename)
{
  std::unique_lock<CCriticalSection> lock(m_cs);
  CAPSounds aps;
  aps.sound = aps.sound2 = nullptr;
  const auto it = m_soundCache.find(filename);
  if (it != m_soundCache.end())
  {
    aps.sound = it->second.sound.lock();
    aps.sound2 = it->second.sound2.lock();
    if (aps.sound)
      return aps;
    else
      m_soundCache.erase(it); // cleanup orphaned cache entry
  }

  IAE *ae = CServiceBroker::GetActiveAE();
  if (!ae)
    return aps;

  std::shared_ptr<IAESound> sound(ae->MakeSound(filename));
  if (!sound)
    return aps;

  IAE *ae2 = CServiceBroker::GetActiveAE(true);
  std::shared_ptr<IAESound> sound2(ae2 ? ae2->MakeSound(filename) : nullptr);

  CSoundInfo info;
  info.sound = sound;
  info.sound2 = sound2;
  m_soundCache[filename] = info;

  aps.sound = sound;
  aps.sound2 = sound2;
  return aps;
}

// \brief Load a window node of the config file (sounds.xml)
CGUIAudioManager::CAPSounds CGUIAudioManager::LoadWindowSound(TiXmlNode* pWindowNode,
                                                            const std::string& strIdentifier)
{
  CAPSounds aps;
  aps.sound = aps.sound2 = NULL;

  if (!pWindowNode)
    return aps;

  TiXmlNode* pFileNode = pWindowNode->FirstChild(strIdentifier);
  if (pFileNode && pFileNode->FirstChild())
    return LoadSound(URIUtils::AddFileToFolder(m_strMediaDir, pFileNode->FirstChild()->Value()));

  return aps;
}

// \brief Enable/Disable nav sounds
void CGUIAudioManager::Enable(bool bEnable)
{
  // always deinit audio when we don't want gui sounds
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetString(CSettings::SETTING_LOOKANDFEEL_SOUNDSKIN).empty())
    bEnable = false;

  std::unique_lock<CCriticalSection> lock(m_cs);
  m_bEnabled = bEnable;
}

// \brief Sets the volume of all playing sounds
void CGUIAudioManager::SetVolume(float level, bool bAudio2)
{
  std::unique_lock<CCriticalSection> lock(m_cs);

  {
    for (const auto& actionSound : m_actionSoundMap)
    {
      if (!bAudio2 && actionSound.second.sound)
        actionSound.second.sound->SetVolume(level);
      if (bAudio2 && actionSound.second.sound2)
        actionSound.second.sound2->SetVolume(level);
    }
  }

  for (const auto& windowSound : m_windowSoundMap)
  {
    if (!bAudio2 && windowSound.second.initSound)
      windowSound.second.initSound->SetVolume(level);
    if (!bAudio2 && windowSound.second.deInitSound)
      windowSound.second.deInitSound->SetVolume(level);
    if (bAudio2 && windowSound.second.initSound2)
      windowSound.second.initSound2->SetVolume(level);
    if (bAudio2 && windowSound.second.deInitSound2)
      windowSound.second.deInitSound2->SetVolume(level);
  }

  {
    for (const auto& pythonSound : m_pythonSounds)
    {
      if (!bAudio2 && pythonSound.second.sound)
        pythonSound.second.sound->SetVolume(level);
      if (bAudio2 && pythonSound.second.sound2)
        pythonSound.second.sound2->SetVolume(level);
    }
  }
}

void CGUIAudioManager::CheckAudio2()
{
  m_bAudio2 = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT2_ENABLED);
}
