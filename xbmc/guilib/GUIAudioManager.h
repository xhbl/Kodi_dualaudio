/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GUIComponent.h"
#include "cores/AudioEngine/Interfaces/AESound.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"

#include <map>
#include <memory>
#include <string>

// forward definitions
class CAction;
class CSettings;
class TiXmlNode;
class IAESound;

enum WINDOW_SOUND { SOUND_INIT = 0, SOUND_DEINIT };

class CGUIAudioManager : public ISettingCallback
{
  class CWindowSounds
  {
  public:
    std::shared_ptr<IAESound> initSound;
    std::shared_ptr<IAESound> deInitSound;
    std::shared_ptr<IAESound> initSound2;
    std::shared_ptr<IAESound> deInitSound2;
  };

  class CAPSounds
  {
  public:
    std::shared_ptr<IAESound> sound;
    std::shared_ptr<IAESound> sound2;
  };

  class CSoundInfo
  {
  public:
    std::weak_ptr<IAESound> sound;
    std::weak_ptr<IAESound> sound2;
  };

  struct IAESoundDeleter
  {
    void operator()(IAESound* s);
  };

public:
  CGUIAudioManager();
  ~CGUIAudioManager() override;

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;
  bool OnSettingUpdate(const std::shared_ptr<CSetting>& setting,
                       const char* oldSettingId,
                       const TiXmlNode* oldSettingNode) override;

  void Initialize();
  void DeInitialize();

  bool Load();
  void UnLoad();


  void PlayActionSound(const CAction& action);
  void PlayWindowSound(int id, WINDOW_SOUND event);
  void PlayPythonSound(const std::string& strFileName, bool useCached = true);

  void CheckAudio2();
  void Enable(bool bEnable);
  void SetVolume(float level, bool bAudio2 = false);
  void Stop();

private:
  // Construction parameters
  std::shared_ptr<CSettings> m_settings;

  typedef std::map<const std::string, CSoundInfo> soundCache;
  typedef std::map<int, CAPSounds> actionSoundMap;
  typedef std::map<int, CWindowSounds> windowSoundMap;
  typedef std::map<const std::string, CAPSounds> pythonSoundsMap;

  soundCache          m_soundCache;
  actionSoundMap      m_actionSoundMap;
  windowSoundMap      m_windowSoundMap;
  pythonSoundsMap     m_pythonSounds;

  std::string          m_strMediaDir;
  bool                m_bEnabled;
  bool                m_bAudio2;

  CCriticalSection    m_cs;

  CAPSounds LoadSound(const std::string& filename);
  CAPSounds LoadWindowSound(TiXmlNode* pWindowNode,
                            const std::string& strIdentifier);
};

