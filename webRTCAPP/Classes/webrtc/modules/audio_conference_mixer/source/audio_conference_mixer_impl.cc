/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/audio/utility/audio_frame_operations.h"
#include "webrtc/modules/audio_conference_mixer/include/audio_conference_mixer_defines.h"
#include "webrtc/modules/audio_conference_mixer/source/audio_conference_mixer_impl.h"
#include "webrtc/modules/audio_conference_mixer/source/audio_frame_manipulator.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/system_wrappers/include/critical_section_wrapper.h"
#include "webrtc/system_wrappers/include/trace.h"

#include <webrtc/voice_engine/channel.h>

namespace webrtc {
    namespace {
        const int dead_water_level = 2;
        const int low_water_level = 20;
        const int high_water_level = 60;
        const int critical_water_level = 200;
        
        struct ParticipantFrameStruct {
            ParticipantFrameStruct(MixerParticipant* p, AudioFrame* a, bool m)
            : participant(p), audioFrame(a), muted(m) {}
            MixerParticipant* participant;
            AudioFrame* audioFrame;
            bool muted;
        };
        
        typedef std::list<ParticipantFrameStruct*> ParticipantFrameStructList;
        
        // Mix |frame| into |mixed_frame|, with saturation protection and upmixing.
        // These effects are applied to |frame| itself prior to mixing. Assumes that
        // |mixed_frame| always has at least as many channels as |frame|. Supports
        // stereo at most.
        //
        // TODO(andrew): consider not modifying |frame| here.
        void MixFrames(AudioFrame* mixed_frame, AudioFrame* frame, bool use_limiter) {
            assert(mixed_frame->num_channels_ >= frame->num_channels_);
            if (use_limiter) {
                // This is to avoid saturation in the mixing. It is only
                // meaningful if the limiter will be used.
                AudioFrameOperations::ApplyHalfGain(frame);
            }
            if (mixed_frame->num_channels_ > frame->num_channels_) {
                // We only support mono-to-stereo.
                assert(mixed_frame->num_channels_ == 2 &&
                       frame->num_channels_ == 1);
                AudioFrameOperations::MonoToStereo(frame);
            }
            
            AudioFrameOperations::Add(*frame, mixed_frame);
        }
        
        void MixFrames(AudioFrame* mixed_frame, AudioFrame** frames, int count)
        {
            if (count == 0) {
                return;
            }
            for (int i = 0; i < count; i++) {
                if (mixed_frame->num_channels_ > frames[i]->num_channels_) {
                    assert(mixed_frame->num_channels_ == 2 &&
                           frames[i]->num_channels_ == 1);
                    AudioFrameOperations::MonoToStereo(frames[i]);
                }
                if (mixed_frame->samples_per_channel_ != frames[i]->samples_per_channel_) {
                    mixed_frame->samples_per_channel_ = frames[i]->samples_per_channel_;
                }
            }
            
            int mix_factor = 1;
            AudioFrameOperations::AddMultiFrame(frames, count, mixed_frame, mix_factor);
        }
        
        // Return the max number of channels from a |list| composed of AudioFrames.
        size_t MaxNumChannels(const AudioFrameList* list) {
            size_t max_num_channels = 1;
            for (AudioFrameList::const_iterator iter = list->begin();
                 iter != list->end();
                 ++iter) {
                max_num_channels = std::max(max_num_channels, (*iter).frame->num_channels_);
            }
            return max_num_channels;
        }
        
    }  // namespace
    
    MixerParticipant::MixerParticipant()
    : _mixHistory(new MixHistory()) {
    }
    
    MixerParticipant::~MixerParticipant() {
        delete _mixHistory;
    }
    
    bool MixerParticipant::IsMixed() const {
        return _mixHistory->IsMixed();
    }
    
    MixHistory::MixHistory()
    : _isMixed(0) {
    }
    
    MixHistory::~MixHistory() {
    }
    
    bool MixHistory::IsMixed() const {
        return _isMixed;
    }
    
    bool MixHistory::WasMixed() const {
        // Was mixed is the same as is mixed depending on perspective. This function
        // is for the perspective of AudioConferenceMixerImpl.
        return IsMixed();
    }
    
    int32_t MixHistory::SetIsMixed(const bool mixed) {
        _isMixed = mixed;
        return 0;
    }
    
    void MixHistory::ResetMixedStatus() {
        _isMixed = false;
    }
    
    AudioConferenceMixer* AudioConferenceMixer::Create(int id) {
        AudioConferenceMixerImpl* mixer = new AudioConferenceMixerImpl(id);
        if(!mixer->Init()) {
            delete mixer;
            return NULL;
        }
        return mixer;
    }
    
    AudioConferenceMixerImpl::AudioConferenceMixerImpl(int id)
    : _id(id),
    _minimumMixingFreq(kLowestPossible),
    _mixReceiver(NULL),
    _outputFrequency(kDefaultFrequency),
    _sampleSize(0),
    _audioFramePool(NULL),
    _participantList(),
    _additionalParticipantList(),
    _numMixedParticipants(0),
    use_limiter_(true),
    _timeStamp(0),
    _timeScheduler(kProcessPeriodicityInMs),
    _processCalls(0) {}
    
    bool AudioConferenceMixerImpl::Init() {
        _crit.reset(CriticalSectionWrapper::CreateCriticalSection());
        if (_crit.get() == NULL)
            return false;
        
        _cbCrit.reset(CriticalSectionWrapper::CreateCriticalSection());
        if(_cbCrit.get() == NULL)
            return false;
        
        Config config;
        config.Set<ExperimentalAgc>(new ExperimentalAgc(false));
        _limiter.reset(AudioProcessing::Create(config));
        if(!_limiter.get())
            return false;
        
        MemoryPool<AudioFrame>::CreateMemoryPool(_audioFramePool,
                                                 DEFAULT_AUDIO_FRAME_POOLSIZE);
        if(_audioFramePool == NULL)
            return false;
        
        if(SetOutputFrequency(kDefaultFrequency) == -1)
            return false;
        
        if(_limiter->gain_control()->set_mode(GainControl::kFixedDigital) !=
           _limiter->kNoError)
            return false;
        
        // We smoothly limit the mixed frame to -7 dbFS. -6 would correspond to the
        // divide-by-2 but -7 is used instead to give a bit of headroom since the
        // AGC is not a hard limiter.
        if(_limiter->gain_control()->set_target_level_dbfs(7) != _limiter->kNoError)
            return false;
        
        if(_limiter->gain_control()->set_compression_gain_db(0)
           != _limiter->kNoError)
            return false;
        
        if(_limiter->gain_control()->enable_limiter(true) != _limiter->kNoError)
            return false;
        
        if(_limiter->gain_control()->Enable(true) != _limiter->kNoError)
            return false;
        
        middle_frame_ = new webrtc::AudioFrame();
        
        return true;
    }
    
    AudioConferenceMixerImpl::~AudioConferenceMixerImpl() {
        MemoryPool<AudioFrame>::DeleteMemoryPool(_audioFramePool);
        assert(_audioFramePool == NULL);
        if (middle_frame_) {
            delete middle_frame_;
            middle_frame_ = nullptr;
        }
    }
    
    // Process should be called every kProcessPeriodicityInMs ms
    int64_t AudioConferenceMixerImpl::TimeUntilNextProcess() {
        int64_t timeUntilNextProcess = 0;
        CriticalSectionScoped cs(_crit.get());
        if(_timeScheduler.TimeToNextUpdate(timeUntilNextProcess) != 0) {
            WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                         "failed in TimeToNextUpdate() call");
            // Sanity check
            assert(false);
            return -1;
        }
        return timeUntilNextProcess;
    }
    
    void AudioConferenceMixerImpl::Process() {
        size_t remainingParticipantsAllowedToMix =
        kMaximumAmountOfMixedParticipants;
        {
            CriticalSectionScoped cs(_crit.get());
            assert(_processCalls == 0);
            _processCalls++;
            
            // Let the scheduler know that we are running one iteration.
            _timeScheduler.UpdateScheduler();
        }
        
        AudioFrameList mixList;
        AudioFrameList rampOutList;
        AudioFrameList additionalFramesList;
        std::map<int, MixerParticipant*> mixedParticipantsMap;
        {
            CriticalSectionScoped cs(_cbCrit.get());
            
            int32_t lowFreq = GetLowestMixingFrequency();
            // SILK can run in 12 kHz and 24 kHz. These frequencies are not
            // supported so use the closest higher frequency to not lose any
            // information.
            // TODO(henrike): this is probably more appropriate to do in
            //                GetLowestMixingFrequency().
            if (lowFreq == 12000) {
                lowFreq = 16000;
            } else if (lowFreq == 24000) {
                lowFreq = 32000;
            }
            if(lowFreq <= 0) {
                CriticalSectionScoped cs(_crit.get());
                _processCalls--;
                return;
            } else {
                switch(lowFreq) {
                    case 8000:
                        if(OutputFrequency() != kNbInHz) {
                            SetOutputFrequency(kNbInHz);
                        }
                        break;
                    case 16000:
                        if(OutputFrequency() != kWbInHz) {
                            SetOutputFrequency(kWbInHz);
                        }
                        break;
                    case 32000:
                        if(OutputFrequency() != kSwbInHz) {
                            SetOutputFrequency(kSwbInHz);
                        }
                        break;
                    case 48000:
                        if(OutputFrequency() != kFbInHz) {
                            SetOutputFrequency(kFbInHz);
                        }
                        break;
                    default:
                        assert(false);
                        
                        CriticalSectionScoped cs(_crit.get());
                        _processCalls--;
                        return;
                }
            }
            
            UpdateToMix(&mixList, &rampOutList, &mixedParticipantsMap,
                        &remainingParticipantsAllowedToMix);
            
            GetAdditionalAudio(&additionalFramesList);
            UpdateMixedStatus(mixedParticipantsMap);
        }
        
        // Get an AudioFrame for mixing from the memory pool.
        AudioFrame* mixedAudio = NULL;
        if(_audioFramePool->PopMemory(mixedAudio) == -1) {
            WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                         "failed PopMemory() call");
            assert(false);
            return;
        }
        
        {
            CriticalSectionScoped cs(_crit.get());
            
            // TODO(henrike): it might be better to decide the number of channels
            //                with an API instead of dynamically.
            
            // Find the max channels over all mixing lists.
            const size_t num_mixed_channels = std::max(MaxNumChannels(&mixList),
                                                       std::max(MaxNumChannels(&additionalFramesList),
                                                                MaxNumChannels(&rampOutList)));
            
            mixedAudio->UpdateFrame(-1, _timeStamp, NULL, 0, _outputFrequency,
                                    AudioFrame::kNormalSpeech,
                                    AudioFrame::kVadPassive, num_mixed_channels);
            
            _timeStamp += static_cast<uint32_t>(_sampleSize);
            
            // We only use the limiter if it supports the output sample rate and
            // we're actually mixing multiple streams.
//            use_limiter_ =
//            _numMixedParticipants > 2 &&
//            _outputFrequency <= AudioProcessing::kMaxNativeSampleRateHz;
            use_limiter_ = false;
            
            MixFromList(mixedAudio, mixList);
            MixAnonomouslyFromList(mixedAudio, additionalFramesList);
            MixAnonomouslyFromList(mixedAudio, rampOutList);
            
            if(mixedAudio->samples_per_channel_ == 0) {
                // Nothing was mixed, set the audio samples to silence.
                mixedAudio->samples_per_channel_ = _sampleSize;
                AudioFrameOperations::Mute(mixedAudio);
            } else {
                // Only call the limiter if we have something to mix.
                LimitMixedAudio(mixedAudio);
            }
        }
        
        {
            CriticalSectionScoped cs(_cbCrit.get());
            if(_mixReceiver != NULL) {
                const AudioFrame** dummy = NULL;
                _mixReceiver->NewMixedAudio(
                                            _id,
                                            *mixedAudio,
                                            dummy,
                                            0);
            }
        }
        
        // Reclaim all outstanding memory.
        _audioFramePool->PushMemory(mixedAudio);
        ClearAudioFrameList(&mixList);
        ClearAudioFrameList(&rampOutList);
        ClearAudioFrameList(&additionalFramesList);
        {
            CriticalSectionScoped cs(_crit.get());
            _processCalls--;
        }
        return;
    }
    
    int32_t AudioConferenceMixerImpl::RegisterMixedStreamCallback(
                                                                  AudioMixerOutputReceiver* mixReceiver) {
        CriticalSectionScoped cs(_cbCrit.get());
        if(_mixReceiver != NULL) {
            return -1;
        }
        _mixReceiver = mixReceiver;
        return 0;
    }
    
    int32_t AudioConferenceMixerImpl::UnRegisterMixedStreamCallback() {
        CriticalSectionScoped cs(_cbCrit.get());
        if(_mixReceiver == NULL) {
            return -1;
        }
        _mixReceiver = NULL;
        return 0;
    }
    
    int32_t AudioConferenceMixerImpl::SetOutputFrequency(
                                                         const Frequency& frequency) {
        CriticalSectionScoped cs(_crit.get());
        
        _outputFrequency = frequency;
        _sampleSize =
        static_cast<size_t>((_outputFrequency*kProcessPeriodicityInMs) / 1000);
        
        return 0;
    }
    
    AudioConferenceMixer::Frequency
    AudioConferenceMixerImpl::OutputFrequency() const {
        CriticalSectionScoped cs(_crit.get());
        return _outputFrequency;
    }
    
    int32_t AudioConferenceMixerImpl::SetMixabilityStatus(
                                                          MixerParticipant* participant, bool mixable) {
        if (!mixable) {
            // Anonymous participants are in a separate list. Make sure that the
            // participant is in the _participantList if it is being mixed.
            SetAnonymousMixabilityStatus(participant, false);
        }
        size_t numMixedParticipants;
        {
            CriticalSectionScoped cs(_cbCrit.get());
            const bool isMixed =
            IsParticipantInList(*participant, _participantList);
            // API must be called with a new state.
            if(!(mixable ^ isMixed)) {
                WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                             "Mixable is aready %s",
                             isMixed ? "ON" : "off");
                return -1;
            }
            bool success = false;
            if(mixable) {
                success = AddParticipantToList(participant, &_participantList);
            } else {
                success = RemoveParticipantFromList(participant, &_participantList);
            }
            if(!success) {
                WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                             "failed to %s participant",
                             mixable ? "add" : "remove");
                assert(false);
                return -1;
            }
            
            size_t numMixedNonAnonymous = _participantList.size();
            if (numMixedNonAnonymous > kMaximumAmountOfMixedParticipants) {
                numMixedNonAnonymous = kMaximumAmountOfMixedParticipants;
            }
            numMixedParticipants =
            numMixedNonAnonymous + _additionalParticipantList.size();
        }
        // A MixerParticipant was added or removed. Make sure the scratch
        // buffer is updated if necessary.
        // Note: The scratch buffer may only be updated in Process().
        CriticalSectionScoped cs(_crit.get());
        _numMixedParticipants = numMixedParticipants;
        return 0;
    }
    
    bool AudioConferenceMixerImpl::MixabilityStatus(const MixerParticipant& participant) const {
        CriticalSectionScoped cs(_cbCrit.get());
        return IsParticipantInList(participant, _participantList);
    }
    
    int32_t AudioConferenceMixerImpl::SetAnonymousMixabilityStatus(MixerParticipant* participant, bool anonymous) {
        CriticalSectionScoped cs(_cbCrit.get());
        if(IsParticipantInList(*participant, _additionalParticipantList)) {
            if(anonymous) {
                return 0;
            }
            if(!RemoveParticipantFromList(participant,
                                          &_additionalParticipantList)) {
                WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                             "unable to remove participant from anonymous list");
                assert(false);
                return -1;
            }
            return AddParticipantToList(participant, &_participantList) ? 0 : -1;
        }
        if(!anonymous) {
            return 0;
        }
        const bool mixable = RemoveParticipantFromList(participant,
                                                       &_participantList);
        if(!mixable) {
            WEBRTC_TRACE(
                         kTraceWarning,
                         kTraceAudioMixerServer,
                         _id,
                         "participant must be registered before turning it into anonymous");
            // Setting anonymous status is only possible if MixerParticipant is
            // already registered.
            return -1;
        }
        return AddParticipantToList(participant, &_additionalParticipantList) ?
        0 : -1;
    }
    
    bool AudioConferenceMixerImpl::AnonymousMixabilityStatus(
                                                             const MixerParticipant& participant) const {
        CriticalSectionScoped cs(_cbCrit.get());
        return IsParticipantInList(participant, _additionalParticipantList);
    }
    
    int32_t AudioConferenceMixerImpl::PreProcessTransmitMix()
    {
        return PreProcessTransmitMixV2();
        size_t remainingParticipantsAllowedToMix = kMaximumAmountOfMixedParticipants;
        std::map<int, MixerParticipant*> mixedParticipantsMap;
        {
            CriticalSectionScoped cs(_cbCrit.get());
            
            int32_t lowFreq = GetLowestMixingFrequency();
            // SILK can run in 12 kHz and 24 kHz. These frequencies are not
            // supported so use the closest higher frequency to not lose any
            // information.
            // TODO(henrike): this is probably more appropriate to do in
            //                GetLowestMixingFrequency().
            if (lowFreq == 12000) {
                lowFreq = 16000;
            } else if (lowFreq == 24000) {
                lowFreq = 32000;
            }
            if(lowFreq <= 0) {
                CriticalSectionScoped cs(_crit.get());
                _processCalls--;
                return 0;
            } else {
                switch(lowFreq) {
                    case 8000:
                        if(OutputFrequency() != kNbInHz) {
                            SetOutputFrequency(kNbInHz);
                        }
                        break;
                    case 16000:
                        if(OutputFrequency() != kWbInHz) {
                            SetOutputFrequency(kWbInHz);
                        }
                        break;
                    case 32000:
                        if(OutputFrequency() != kSwbInHz) {
                            SetOutputFrequency(kSwbInHz);
                        }
                        break;
                    case 48000:
                        if(OutputFrequency() != kFbInHz) {
                            SetOutputFrequency(kFbInHz);
                        }
                        break;
                    default:
                        assert(false);
                        
                        CriticalSectionScoped cs(_crit.get());
                        _processCalls--;
                        return -1;
                }
            }
            
//            SetOutputFrequency(kWbInHz);
            
            UpdateToMix(mixedParticipantsMap, remainingParticipantsAllowedToMix);
            GetAdditionalAudio(&_additionalFramesList);
            UpdateTransmitMixedStatus(mixedParticipantsMap);
//            _scratchParticipantsToMixAmount = mixedParticipantsMap.size();
        }
        
        return 0;
    }
    
    // mixaudio for special channel
    int32_t AudioConferenceMixerImpl::ProcessForChannel(const int32_t channelID, bool mix_all)
    {
        return ProcessForChannelInnerV2(channelID, mix_all);
        AudioFrameList mixList;
        AudioFrameList rampOutList;
        AudioFrameList additionalFramesList;
        {
            CriticalSectionScoped cs(_crit.get());
            assert(_processCalls == 0);
            _processCalls++;
            
            // Let the scheduler know that we are running one iteration.
            _timeScheduler.UpdateScheduler();
        }
        
        //删除特定通道的frame(如果channelID为－1，则是混合全部通道的数据)
        for (auto it = _mixList.begin(); it != _mixList.end(); ++it) {
            const FrameAndMuteInfo& frameinfo = *it;
            if (frameinfo.frame && !mix_all && frameinfo.frame->id_ == channelID) {
                continue;
            }
            mixList.push_back(frameinfo);
        }
        
        for (auto it = _rampOutList.begin(); it != _rampOutList.end(); ++it) {
            const FrameAndMuteInfo& frameinfo = *it;
            if (frameinfo.frame && !mix_all && frameinfo.frame->id_ == channelID) {
                continue;
            }
            rampOutList.push_back(frameinfo);
        }
        
        for (auto it = _additionalFramesList.begin(); it != _additionalFramesList.end(); ++it) {
            const FrameAndMuteInfo& frameinfo = *it;
            if (frameinfo.frame && !mix_all && frameinfo.frame->id_ == channelID) {
                continue;
            }
            additionalFramesList.push_back(frameinfo);
        }
        
        // Get an AudioFrame for mixing from the memory pool.
        AudioFrame* mixedAudio = NULL;
        if(_audioFramePool->PopMemory(mixedAudio) == -1) {
            WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                         "failed PopMemory() call");
            assert(false);
            return -1;
        }
        
        {
            CriticalSectionScoped cs(_crit.get());
            
            // TODO(henrike): it might be better to decide the number of channels
            //                with an API instead of dynamically.
            
            // Find the max channels over all mixing lists.
            const int num_mixed_channels = std::max(MaxNumChannels(&mixList),
                                                    std::max(MaxNumChannels(&additionalFramesList),
                                                             MaxNumChannels(&rampOutList)));
            
            mixedAudio->UpdateFrame(-1, _timeStamp, NULL, 0, _outputFrequency,
                                    AudioFrame::kNormalSpeech,
                                    AudioFrame::kVadPassive, num_mixed_channels);
            
            _timeStamp += static_cast<uint32_t>(_sampleSize);
            
            // We only use the limiter if it supports the output sample rate and
            // we're actually mixing multiple streams.
//            use_limiter_ =
//            _numMixedParticipants > 2 &&
//            _outputFrequency <= AudioProcessing::kMaxNativeSampleRateHz;
            use_limiter_ = false;
            
            MixFromList(mixedAudio, mixList);
            MixAnonomouslyFromList(mixedAudio, additionalFramesList);
            MixAnonomouslyFromList(mixedAudio, rampOutList);
            
            if(mixedAudio->samples_per_channel_ == 0) {
                // Nothing was mixed, set the audio samples to silence.
                mixedAudio->samples_per_channel_ = _sampleSize;
                AudioFrameOperations::Mute(mixedAudio);
            } else {
                // Only call the limiter if we have something to mix.
//                LimitMixedAudio(mixedAudio);
//                if(!LimitMixedAudio(mixedAudio))
//                    retval = -1;
            }
        }
        
        {
            CriticalSectionScoped cs(_cbCrit.get());
            if(_mixReceiver != NULL) {
                const AudioFrame** dummy = NULL;
                _mixReceiver->NewMixedAudio(
                                            _id,
                                            *mixedAudio,
                                            dummy,
                                            0);
            }
        }
        
        // Reclaim all outstanding memory.
        _audioFramePool->PushMemory(mixedAudio);
        {
            CriticalSectionScoped cs(_crit.get());
            _processCalls--;
        }
        return 0;
    }
    
    void AudioConferenceMixerImpl::ClearTransmitMixCache()
    {
        ClearAudioFrameList(&_mixList);
        ClearAudioFrameList(&_rampOutList);
        ClearAudioFrameList(&_additionalFramesList);
        
        for (auto it = available_mix_frame_container_.begin(); it != available_mix_frame_container_.end(); ++it) {
            _audioFramePool->PushMemory(it->second);
        }
        available_mix_frame_container_.clear();
    }
    
    int32_t AudioConferenceMixerImpl::SetMinimumMixingFrequency(
                                                                Frequency freq) {
        // Make sure that only allowed sampling frequencies are used. Use closest
        // higher sampling frequency to avoid losing information.
        if (static_cast<int>(freq) == 12000) {
            freq = kWbInHz;
        } else if (static_cast<int>(freq) == 24000) {
            freq = kSwbInHz;
        }
        
        if((freq == kNbInHz) || (freq == kWbInHz) || (freq == kSwbInHz) ||
           (freq == kLowestPossible)) {
            _minimumMixingFreq=freq;
            return 0;
        } else {
            WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                         "SetMinimumMixingFrequency incorrect frequency: %i",freq);
            assert(false);
            return -1;
        }
    }
    
    // Check all AudioFrames that are to be mixed. The highest sampling frequency
    // found is the lowest that can be used without losing information.
    int32_t AudioConferenceMixerImpl::GetLowestMixingFrequency() const {
        const int participantListFrequency =
        GetLowestMixingFrequencyFromList(_participantList);
        const int anonymousListFrequency =
        GetLowestMixingFrequencyFromList(_additionalParticipantList);
        const int highestFreq =
        (participantListFrequency > anonymousListFrequency) ?
        participantListFrequency : anonymousListFrequency;
        // Check if the user specified a lowest mixing frequency.
        if(_minimumMixingFreq != kLowestPossible) {
            if(_minimumMixingFreq > highestFreq) {
                return _minimumMixingFreq;
            }
        }
        return highestFreq;
    }
    
    int32_t AudioConferenceMixerImpl::GetLowestMixingFrequencyFromList(
                                                                       const MixerParticipantList& mixList) const {
        int32_t highestFreq = 8000;
        for (MixerParticipantList::const_iterator iter = mixList.begin();
             iter != mixList.end();
             ++iter) {
            const int32_t neededFrequency = (*iter)->NeededFrequency(_id);
            if(neededFrequency > highestFreq) {
                highestFreq = neededFrequency;
            }
        }
        return highestFreq;
    }
    
    void AudioConferenceMixerImpl::UpdateToMix(
                                               AudioFrameList* mixList,
                                               AudioFrameList* rampOutList,
                                               std::map<int, MixerParticipant*>* mixParticipantList,
                                               size_t* maxAudioFrameCounter) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "UpdateToMix(mixList,rampOutList,mixParticipantList,%d)",
                     *maxAudioFrameCounter);
        const size_t mixListStartSize = mixList->size();
        AudioFrameList activeList;
        // Struct needed by the passive lists to keep track of which AudioFrame
        // belongs to which MixerParticipant.
        ParticipantFrameStructList passiveWasNotMixedList;
        ParticipantFrameStructList passiveWasMixedList;
        for (MixerParticipantList::const_iterator participant =
             _participantList.begin(); participant != _participantList.end();
             ++participant) {
            // Stop keeping track of passive participants if there are already
            // enough participants available (they wont be mixed anyway).
            bool mustAddToPassiveList = (*maxAudioFrameCounter >
                                         (activeList.size() +
                                          passiveWasMixedList.size() +
                                          passiveWasNotMixedList.size()));
            
            bool wasMixed = false;
            wasMixed = (*participant)->_mixHistory->WasMixed();
            AudioFrame* audioFrame = NULL;
            if(_audioFramePool->PopMemory(audioFrame) == -1) {
                WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                             "failed PopMemory() call");
                assert(false);
                return;
            }
            audioFrame->sample_rate_hz_ = _outputFrequency;
            
            auto ret = (*participant)->GetAudioFrameWithMuted(_id, audioFrame);
            if (ret == MixerParticipant::AudioFrameInfo::kError) {
                WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                             "failed to GetAudioFrameWithMuted() from participant");
                _audioFramePool->PushMemory(audioFrame);
                continue;
            }
            const bool muted = (ret == MixerParticipant::AudioFrameInfo::kMuted);
            if (_participantList.size() != 1) {
                // TODO(wu): Issue 3390, add support for multiple participants case.
                audioFrame->ntp_time_ms_ = -1;
            }
            
            // TODO(henrike): this assert triggers in some test cases where SRTP is
            // used which prevents NetEQ from making a VAD. Temporarily disable this
            // assert until the problem is fixed on a higher level.
            // assert(audioFrame->vad_activity_ != AudioFrame::kVadUnknown);
            if (audioFrame->vad_activity_ == AudioFrame::kVadUnknown) {
                WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                             "invalid VAD state from participant");
            }
            
            if(audioFrame->vad_activity_ == AudioFrame::kVadActive) {
                if(!wasMixed && !muted) {
                    RampIn(*audioFrame);
                }
                
                if(activeList.size() >= *maxAudioFrameCounter) {
                    // There are already more active participants than should be
                    // mixed. Only keep the ones with the highest energy.
                    AudioFrameList::iterator replaceItem;
                    uint32_t lowestEnergy =
                    muted ? 0 : CalculateEnergy(*audioFrame);
                    
                    bool found_replace_item = false;
                    for (AudioFrameList::iterator iter = activeList.begin();
                         iter != activeList.end();
                         ++iter) {
                        const uint32_t energy =
                        muted ? 0 : CalculateEnergy(*iter->frame);
                        if(energy < lowestEnergy) {
                            replaceItem = iter;
                            lowestEnergy = energy;
                            found_replace_item = true;
                        }
                    }
                    if(found_replace_item) {
                        RTC_DCHECK(!muted);  // Cannot replace with a muted frame.
                        FrameAndMuteInfo replaceFrame = *replaceItem;
                        
                        bool replaceWasMixed = false;
                        std::map<int, MixerParticipant*>::const_iterator it =
                        mixParticipantList->find(replaceFrame.frame->id_);
                        
                        // When a frame is pushed to |activeList| it is also pushed
                        // to mixParticipantList with the frame's id. This means
                        // that the Find call above should never fail.
                        assert(it != mixParticipantList->end());
                        replaceWasMixed = it->second->_mixHistory->WasMixed();
                        
                        mixParticipantList->erase(replaceFrame.frame->id_);
                        activeList.erase(replaceItem);
                        
                        activeList.push_front(FrameAndMuteInfo(audioFrame, muted));
                        (*mixParticipantList)[audioFrame->id_] = *participant;
                        assert(mixParticipantList->size() <=
                               kMaximumAmountOfMixedParticipants);
                        
                        if (replaceWasMixed) {
                            if (!replaceFrame.muted) {
                                RampOut(*replaceFrame.frame);
                            }
                            rampOutList->push_back(replaceFrame);
                            assert(rampOutList->size() <=
                                   kMaximumAmountOfMixedParticipants);
                        } else {
                            _audioFramePool->PushMemory(replaceFrame.frame);
                        }
                    } else {
                        if(wasMixed) {
                            if (!muted) {
                                RampOut(*audioFrame);
                            }
                            rampOutList->push_back(FrameAndMuteInfo(audioFrame,
                                                                    muted));
                            assert(rampOutList->size() <=
                                   kMaximumAmountOfMixedParticipants);
                        } else {
                            _audioFramePool->PushMemory(audioFrame);
                        }
                    }
                } else {
                    activeList.push_front(FrameAndMuteInfo(audioFrame, muted));
                    (*mixParticipantList)[audioFrame->id_] = *participant;
                    assert(mixParticipantList->size() <=
                           kMaximumAmountOfMixedParticipants);
                }
            } else {
                if(wasMixed) {
                    ParticipantFrameStruct* part_struct =
                    new ParticipantFrameStruct(*participant, audioFrame, muted);
                    passiveWasMixedList.push_back(part_struct);
                } else if(mustAddToPassiveList) {
                    if (!muted) {
                        RampIn(*audioFrame);
                    }
                    ParticipantFrameStruct* part_struct =
                    new ParticipantFrameStruct(*participant, audioFrame, muted);
                    passiveWasNotMixedList.push_back(part_struct);
                } else {
                    _audioFramePool->PushMemory(audioFrame);
                }
            }
        }
        assert(activeList.size() <= *maxAudioFrameCounter);
        // At this point it is known which participants should be mixed. Transfer
        // this information to this functions output parameters.
        for (AudioFrameList::const_iterator iter = activeList.begin();
             iter != activeList.end();
             ++iter) {
            mixList->push_back(*iter);
        }
        activeList.clear();
        // Always mix a constant number of AudioFrames. If there aren't enough
        // active participants mix passive ones. Starting with those that was mixed
        // last iteration.
        for (ParticipantFrameStructList::const_iterator
             iter = passiveWasMixedList.begin(); iter != passiveWasMixedList.end();
             ++iter) {
            if(mixList->size() < *maxAudioFrameCounter + mixListStartSize) {
                mixList->push_back(FrameAndMuteInfo((*iter)->audioFrame,
                                                    (*iter)->muted));
                (*mixParticipantList)[(*iter)->audioFrame->id_] =
                (*iter)->participant;
                assert(mixParticipantList->size() <=
                       kMaximumAmountOfMixedParticipants);
            } else {
                _audioFramePool->PushMemory((*iter)->audioFrame);
            }
            delete *iter;
        }
        // And finally the ones that have not been mixed for a while.
        for (ParticipantFrameStructList::const_iterator iter =
             passiveWasNotMixedList.begin();
             iter != passiveWasNotMixedList.end();
             ++iter) {
            if(mixList->size() <  *maxAudioFrameCounter + mixListStartSize) {
                mixList->push_back(FrameAndMuteInfo((*iter)->audioFrame,
                                                    (*iter)->muted));
                (*mixParticipantList)[(*iter)->audioFrame->id_] =
                (*iter)->participant;
                assert(mixParticipantList->size() <=
                       kMaximumAmountOfMixedParticipants);
            } else {
                _audioFramePool->PushMemory((*iter)->audioFrame);
            }
            delete *iter;
        }
        assert(*maxAudioFrameCounter + mixListStartSize >= mixList->size());
        *maxAudioFrameCounter += mixListStartSize - mixList->size();
    }
    
    void AudioConferenceMixerImpl::GetAdditionalAudio(
                                                      AudioFrameList* additionalFramesList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "GetAdditionalAudio(additionalFramesList)");
        // The GetAudioFrameWithMuted() callback may result in the participant being
        // removed from additionalParticipantList_. If that happens it will
        // invalidate any iterators. Create a copy of the participants list such
        // that the list of participants can be traversed safely.
        MixerParticipantList additionalParticipantList;
        additionalParticipantList.insert(additionalParticipantList.begin(),
                                         _additionalParticipantList.begin(),
                                         _additionalParticipantList.end());
        
        for (MixerParticipantList::const_iterator participant =
             additionalParticipantList.begin();
             participant != additionalParticipantList.end();
             ++participant) {
            AudioFrame* audioFrame = NULL;
            if(_audioFramePool->PopMemory(audioFrame) == -1) {
                WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                             "failed PopMemory() call");
                assert(false);
                return;
            }
            audioFrame->sample_rate_hz_ = _outputFrequency;
            auto ret = (*participant)->GetAudioFrameWithMuted(_id, audioFrame);
            if (ret == MixerParticipant::AudioFrameInfo::kError) {
                WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                             "failed to GetAudioFrameWithMuted() from participant");
                _audioFramePool->PushMemory(audioFrame);
                continue;
            }
            if(audioFrame->samples_per_channel_ == 0) {
                // Empty frame. Don't use it.
                _audioFramePool->PushMemory(audioFrame);
                continue;
            }
            additionalFramesList->push_back(FrameAndMuteInfo(
                                                             audioFrame, ret == MixerParticipant::AudioFrameInfo::kMuted));
        }
    }
    
    void AudioConferenceMixerImpl::UpdateMixedStatus(
                                                     const std::map<int, MixerParticipant*>& mixedParticipantsMap) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "UpdateMixedStatus(mixedParticipantsMap)");
        assert(mixedParticipantsMap.size() <= kMaximumAmountOfMixedParticipants);
        
        // Loop through all participants. If they are in the mix map they
        // were mixed.
        for (MixerParticipantList::const_iterator
             participant =_participantList.begin();
             participant != _participantList.end();
             ++participant) {
            bool isMixed = false;
            for (std::map<int, MixerParticipant*>::const_iterator it =
                 mixedParticipantsMap.begin();
                 it != mixedParticipantsMap.end();
                 ++it) {
                if (it->second == *participant) {
                    isMixed = true;
                    break;
                }
            }
            (*participant)->_mixHistory->SetIsMixed(isMixed);
        }
    }
    
    void AudioConferenceMixerImpl::ClearAudioFrameList(
                                                       AudioFrameList* audioFrameList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "ClearAudioFrameList(audioFrameList)");
        for (AudioFrameList::iterator iter = audioFrameList->begin();
             iter != audioFrameList->end();
             ++iter) {
            _audioFramePool->PushMemory(iter->frame);
        }
        audioFrameList->clear();
    }
    
    bool AudioConferenceMixerImpl::IsParticipantInList(
                                                       const MixerParticipant& participant,
                                                       const MixerParticipantList& participantList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "IsParticipantInList(participant,participantList)");
        for (MixerParticipantList::const_iterator iter = participantList.begin();
             iter != participantList.end();
             ++iter) {
            if(&participant == *iter) {
                return true;
            }
        }
        return false;
    }
    
    bool AudioConferenceMixerImpl::AddParticipantToList(
                                                        MixerParticipant* participant,
                                                        MixerParticipantList* participantList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "AddParticipantToList(participant, participantList)");
        participantList->push_back(participant);
        // Make sure that the mixed status is correct for new MixerParticipant.
        participant->_mixHistory->ResetMixedStatus();
        return true;
    }
    
    bool AudioConferenceMixerImpl::RemoveParticipantFromList(
                                                             MixerParticipant* participant,
                                                             MixerParticipantList* participantList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "RemoveParticipantFromList(participant, participantList)");
        for (MixerParticipantList::iterator iter = participantList->begin();
             iter != participantList->end();
             ++iter) {
            if(*iter == participant) {
                GetBackFrameFromChannel(participant);
                participantList->erase(iter);
                // Participant is no longer mixed, reset to default.
                participant->_mixHistory->ResetMixedStatus();
                return true;
            }
        }
        return false;
    }
    
    int32_t AudioConferenceMixerImpl::MixFromList(
                                                  AudioFrame* mixedAudio,
                                                  const AudioFrameList& audioFrameList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "MixFromList(mixedAudio, audioFrameList)");
        if(audioFrameList.empty()) return 0;
        
        uint32_t position = 0;
        
        if (_numMixedParticipants == 1) {
            mixedAudio->timestamp_ = audioFrameList.front().frame->timestamp_;
            mixedAudio->elapsed_time_ms_ =
            audioFrameList.front().frame->elapsed_time_ms_;
        } else {
            // TODO(wu): Issue 3390.
            // Audio frame timestamp is only supported in one channel case.
            mixedAudio->timestamp_ = 0;
            mixedAudio->elapsed_time_ms_ = -1;
        }
        
//        for (AudioFrameList::const_iterator iter = audioFrameList.begin();
//             iter != audioFrameList.end();
//             ++iter) {
//            if(position >= kMaximumAmountOfMixedParticipants) {
//                WEBRTC_TRACE(
//                             kTraceMemory,
//                             kTraceAudioMixerServer,
//                             _id,
//                             "Trying to mix more than max amount of mixed participants:%d!",
//                             kMaximumAmountOfMixedParticipants);
//                // Assert and avoid crash
//                assert(false);
//                position = 0;
//            }
//            if (!iter->muted) {
//                middle_frame_->CopyFrom(*iter->frame);
//                MixFrames(mixedAudio, middle_frame_, use_limiter_);
//            }
//            
//            position++;
//        }
        
        AudioFrame* frames[kMaximumAmountOfMixedParticipants];
        int i = 0;
        for (auto frame_info : audioFrameList) {
            if (i < kMaximumAmountOfMixedParticipants && !frame_info.muted) {
                frames[i++] = frame_info.frame;
            }
        }
        MixFrames(mixedAudio, frames, i);
        
        return 0;
    }
    
    // TODO(andrew): consolidate this function with MixFromList.
    int32_t AudioConferenceMixerImpl::MixAnonomouslyFromList(
                                                             AudioFrame* mixedAudio,
                                                             const AudioFrameList& audioFrameList) const {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "MixAnonomouslyFromList(mixedAudio, audioFrameList)");
        
        if(audioFrameList.empty()) return 0;
        
//        for (AudioFrameList::const_iterator iter = audioFrameList.begin();
//             iter != audioFrameList.end();
//             ++iter) {
//            if (!iter->muted) {
//                middle_frame_->CopyFrom(*iter->frame);
//                MixFrames(mixedAudio, middle_frame_, use_limiter_);
//            }
//        }
        
        AudioFrame* frames[kMaximumAmountOfMixedParticipants];
        int i = 0;
        for (auto frame_info : audioFrameList) {
            if (i < kMaximumAmountOfMixedParticipants && !frame_info.muted) {
                frames[i++] = frame_info.frame;
            }
        }
        MixFrames(mixedAudio, frames, i);
        
        return 0;
    }
    
    bool AudioConferenceMixerImpl::LimitMixedAudio(AudioFrame* mixedAudio) const {
        if (!use_limiter_) {
            return true;
        }
        
        // Smoothly limit the mixed frame.
        const int error = _limiter->ProcessStream(mixedAudio);
        
        // And now we can safely restore the level. This procedure results in
        // some loss of resolution, deemed acceptable.
        //
        // It's possible to apply the gain in the AGC (with a target level of 0 dbFS
        // and compression gain of 6 dB). However, in the transition frame when this
        // is enabled (moving from one to two participants) it has the potential to
        // create discontinuities in the mixed frame.
        //
        // Instead we double the frame (with addition since left-shifting a
        // negative value is undefined).
        AudioFrameOperations::Add(*mixedAudio, mixedAudio);
        
        if(error != _limiter->kNoError) {
            WEBRTC_TRACE(kTraceError, kTraceAudioMixerServer, _id,
                         "Error from AudioProcessing: %d", error);
            assert(false);
            return false;
        }
        return true;
    }
    
    void AudioConferenceMixerImpl::UpdateToMix(std::map<int, MixerParticipant *>& mixedParticipantsMap, size_t &maxAudioFrameCounter)
    {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "UpdateToMix(mixList,rampOutList,mixParticipantList,%d)",
                     maxAudioFrameCounter);
        AudioFrameList& mixList = _mixList;
        AudioFrameList& rampOutList = _rampOutList;
        
        const size_t mixListStartSize = mixList.size();
        AudioFrameList activeList;
        // Struct needed by the passive lists to keep track of which AudioFrame
        // belongs to which MixerParticipant.
        ParticipantFrameStructList passiveWasNotMixedList;
        ParticipantFrameStructList passiveWasMixedList;
        for (MixerParticipantList::iterator participant = _participantList.begin();
             participant != _participantList.end();
             ++participant) {
            
            // Stop keeping track of passive participants if there are already
            // enough participants available (they wont be mixed anyway).
            bool mustAddToPassiveList = (maxAudioFrameCounter >
                                         (activeList.size() +
                                          passiveWasMixedList.size() +
                                          passiveWasNotMixedList.size()));
            
            bool wasMixed = false;
            wasMixed = (*participant)->_mixHistory->WasMixed();
            
            AudioFrame* audioFrame = NULL;
            if(_audioFramePool->PopMemory(audioFrame) == -1) {
                WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                             "failed PopMemory() call");
                assert(false);
                return;
            }
            audioFrame->sample_rate_hz_ = _outputFrequency;
            
            auto ret = (*participant)->GetAudioFrameWithMuted(_id, audioFrame);
            if(ret == MixerParticipant::AudioFrameInfo::kError) {
                WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                             "failed to GetAudioFrame() from participant");
                _audioFramePool->PushMemory(audioFrame);
                continue;
            }
            const bool muted = (ret == MixerParticipant::AudioFrameInfo::kMuted);
            if (_participantList.size() != 1) {
                // TODO(wu): Issue 3390, add support for multiple participants case.
                audioFrame->ntp_time_ms_ = -1;
            }
            
            // TODO(henrike): this assert triggers in some test cases where SRTP is
            // used which prevents NetEQ from making a VAD. Temporarily disable this
            // assert until the problem is fixed on a higher level.
            // assert(audioFrame->vad_activity_ != AudioFrame::kVadUnknown);
            if (audioFrame->vad_activity_ == AudioFrame::kVadUnknown) {
                WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                             "invalid VAD state from participant");
            }
            
            if(audioFrame->vad_activity_ == AudioFrame::kVadActive) {
                if(!wasMixed && !muted) {
                    RampIn(*audioFrame);
                }
                
                if(activeList.size() >= maxAudioFrameCounter) {
                    // There are already more active participants than should be
                    // mixed. Only keep the ones with the highest energy.
                    AudioFrameList::iterator replaceItem;
                    uint32_t lowestEnergy =
                    muted ? 0 : CalculateEnergy(*audioFrame);
                    
                    bool found_replace_item = false;
                    for (AudioFrameList::iterator iter = activeList.begin();
                         iter != activeList.end();
                         ++iter) {
                        const uint32_t energy =
                        muted ? 0 : CalculateEnergy(*iter->frame);
                        if(energy < lowestEnergy) {
                            replaceItem = iter;
                            lowestEnergy = energy;
                            found_replace_item = true;
                        }
                    }
                    
                    if (found_replace_item) {
                        RTC_DCHECK(!muted);
                        FrameAndMuteInfo replaceFrame = *replaceItem;
                        
                        bool replacedWasMixed = false;
                        auto it = mixedParticipantsMap.find(replaceFrame.frame->id_);
                        if (it != mixedParticipantsMap.end()) {
                            replacedWasMixed = it->second->_mixHistory->WasMixed();
                            mixedParticipantsMap.erase(replaceFrame.frame->id_);
                            mixedParticipantsMap[audioFrame->id_] = *participant;
                            assert(mixedParticipantsMap.size() <= kMaximumAmountOfMixedParticipants);
                            
                            activeList.erase(replaceItem);
                            activeList.push_front(FrameAndMuteInfo(audioFrame, muted));
                            
                            if (replacedWasMixed) {
                                if (!replaceFrame.muted) {
                                    RampOut(*replaceFrame.frame);
                                }
                                rampOutList.push_back(replaceFrame);
                                assert(rampOutList.size() <= kMaximumAmountOfMixedParticipants);
                            } else {
                                _audioFramePool->PushMemory(replaceFrame.frame);
                            }
                        }
                    }
                    else {
                        if(wasMixed) {
                            if (!muted) {
                                RampOut(*audioFrame);
                            }
                            rampOutList.push_back(FrameAndMuteInfo(audioFrame,
                                                                   muted));
                            assert(rampOutList.size() <=
                                   kMaximumAmountOfMixedParticipants);
                        } else {
                            _audioFramePool->PushMemory(audioFrame);
                        }
                    }
                } else {
                    activeList.push_front(FrameAndMuteInfo(audioFrame, muted));
                    mixedParticipantsMap[audioFrame->id_] = *participant;
                    assert(mixedParticipantsMap.size() <=
                           kMaximumAmountOfMixedParticipants);
                }
            } else {
                if(wasMixed) {
                    ParticipantFrameStruct* part_struct = new ParticipantFrameStruct(*participant, audioFrame, muted);
                    passiveWasMixedList.push_back(part_struct);
                } else if(mustAddToPassiveList) {
                    if (!muted) {
                        RampIn(*audioFrame);
                    }
                    ParticipantFrameStruct* part_struct = new ParticipantFrameStruct(*participant, audioFrame, muted);
                    passiveWasNotMixedList.push_back(part_struct);
                } else {
                    _audioFramePool->PushMemory(audioFrame);
                }
            }
        }
        assert(activeList.size() <= maxAudioFrameCounter);
        // At this point it is known which participants should be mixed. Transfer
        // this information to this functions output parameters.
        for (AudioFrameList::iterator iter = activeList.begin();
             iter != activeList.end();
             ++iter) {
            mixList.push_back(*iter);
        }
        activeList.clear();
        
        // Always mix a constant number of AudioFrames. If there aren't enough
        // active participants mix passive ones. Starting with those that was mixed
        // last iteration.
        for (ParticipantFrameStructList::iterator iter = passiveWasMixedList.begin();
             iter != passiveWasMixedList.end();
             ++iter) {
            if(mixList.size() < maxAudioFrameCounter + mixListStartSize) {
                mixList.push_back(FrameAndMuteInfo((*iter)->audioFrame,
                                                   (*iter)->muted));
                mixedParticipantsMap[(*iter)->audioFrame->id_] =
                (*iter)->participant;
                assert(mixedParticipantsMap.size() <=
                       kMaximumAmountOfMixedParticipants);
            } else {
                _audioFramePool->PushMemory((*iter)->audioFrame);
            }
            delete *iter;
        }
        // And finally the ones that have not been mixed for a while.
        for (ParticipantFrameStructList::iterator iter =
             passiveWasNotMixedList.begin();
             iter != passiveWasNotMixedList.end();
             ++iter) {
            if(mixList.size() <  maxAudioFrameCounter + mixListStartSize) {
                mixList.push_back(FrameAndMuteInfo((*iter)->audioFrame,
                                                   (*iter)->muted));
                mixedParticipantsMap[(*iter)->audioFrame->id_] =
                (*iter)->participant;
                assert(mixedParticipantsMap.size() <=
                       kMaximumAmountOfMixedParticipants);
            } else {
                _audioFramePool->PushMemory((*iter)->audioFrame);
            }
            delete *iter;
        }
        assert(maxAudioFrameCounter + mixListStartSize >= mixList.size());
        maxAudioFrameCounter += mixListStartSize - mixList.size();
    }
    
    void AudioConferenceMixerImpl::UpdateTransmitMixedStatus(std::map<int, MixerParticipant*>& mixedParticipantsMap)
    {
        WEBRTC_TRACE(kTraceStream, kTraceAudioMixerServer, _id,
                     "UpdateMixedStatus(mixedParticipantsMap)");
        assert(mixedParticipantsMap.size() <= kMaximumAmountOfMixedParticipants);
        
        // Loop through all participants. If they are in the mix map they
        // were mixed.
        for (MixerParticipantList::iterator participant = _participantList.begin();
             participant != _participantList.end();
             ++participant) {
            bool isMixed = false;
            for (std::map<int, MixerParticipant*>::iterator it =
                 mixedParticipantsMap.begin();
                 it != mixedParticipantsMap.end();
                 ++it) {
                if (it->second == *participant) {
                    isMixed = true;
                    break;
                }
            }
            (*participant)->_mixHistory->SetIsMixed(isMixed);
        }
    }
    
    int AudioConferenceMixerImpl::PreProcessTransmitMixV1()
    {
        size_t remainingParticipantsAllowedToMix = kMaximumAmountOfMixedParticipants;
        std::map<int, MixerParticipant*> mixedParticipantsMap;
        {
            CriticalSectionScoped cs(_cbCrit.get());
            
            int32_t lowFreq = GetLowestMixingFrequency();
            // SILK can run in 12 kHz and 24 kHz. These frequencies are not
            // supported so use the closest higher frequency to not lose any
            // information.
            // TODO(henrike): this is probably more appropriate to do in
            //                GetLowestMixingFrequency().
            if (lowFreq == 12000) {
                lowFreq = 16000;
            } else if (lowFreq == 24000) {
                lowFreq = 32000;
            }
            if(lowFreq <= 0) {
                CriticalSectionScoped cs(_crit.get());
                _processCalls--;
                return 0;
            } else {
                switch(lowFreq) {
                    case 8000:
                        if(OutputFrequency() != kNbInHz) {
                            SetOutputFrequency(kNbInHz);
                        }
                        break;
                    case 16000:
                        if(OutputFrequency() != kWbInHz) {
                            SetOutputFrequency(kWbInHz);
                        }
                        break;
                    case 32000:
                        if(OutputFrequency() != kSwbInHz) {
                            SetOutputFrequency(kSwbInHz);
                        }
                        break;
                    case 48000:
                        if(OutputFrequency() != kFbInHz) {
                            SetOutputFrequency(kFbInHz);
                        }
                        break;
                    default:
                        assert(false);
                        
                        CriticalSectionScoped cs(_crit.get());
                        _processCalls--;
                        return -1;
                }
            }
            
            //            SetOutputFrequency(kWbInHz);
            
            UpdateToMix(mixedParticipantsMap, remainingParticipantsAllowedToMix);
            GetAdditionalAudio(&_additionalFramesList);
            UpdateTransmitMixedStatus(mixedParticipantsMap);
            //            _scratchParticipantsToMixAmount = mixedParticipantsMap.size();
        }
        return 0;
    }
    
    int AudioConferenceMixerImpl::PreProcessTransmitMixV2()
    {
        {
            CriticalSectionScoped cs(_cbCrit.get());
            
            int32_t lowFreq = GetLowestMixingFrequency();
            // SILK can run in 12 kHz and 24 kHz. These frequencies are not
            // supported so use the closest higher frequency to not lose any
            // information.
            // TODO(henrike): this is probably more appropriate to do in
            //                GetLowestMixingFrequency().
            if (lowFreq == 12000) {
                lowFreq = 16000;
            } else if (lowFreq == 24000) {
                lowFreq = 32000;
            }
            if(lowFreq <= 0) {
                CriticalSectionScoped cs(_crit.get());
                _processCalls--;
                return 0;
            } else {
                switch(lowFreq) {
                    case 8000:
                        if(OutputFrequency() != kNbInHz) {
                            SetOutputFrequency(kNbInHz);
                        }
                        break;
                    case 16000:
                        if(OutputFrequency() != kWbInHz) {
                            SetOutputFrequency(kWbInHz);
                        }
                        break;
                    case 32000:
                        if(OutputFrequency() != kSwbInHz) {
                            SetOutputFrequency(kSwbInHz);
                        }
                        break;
                    case 48000:
                        if(OutputFrequency() != kFbInHz) {
                            SetOutputFrequency(kFbInHz);
                        }
                        break;
                    default:
                        assert(false);
                        
                        CriticalSectionScoped cs(_crit.get());
                        _processCalls--;
                        return -1;
                }
            }
        }
        
        return GetAllMixAudioFrame();
    }
    
    int AudioConferenceMixerImpl::GetAllMixAudioFrame()
    {
        int temp_frame_count = 0;
        int per_channel_frame_max_count = 0;
        int per_channel_frame_min_count = 0;
        int can_process_count = 4;
        CriticalSectionScoped cs(_cbCrit.get());
        for (auto it = _participantList.begin(); it != _participantList.end(); ++it) {
            webrtc::voe::Channel* channel = static_cast<webrtc::voe::Channel*>(*it);
            while (true) {
                AudioFrame* audio_frame = NULL;
                if(_audioFramePool->PopMemory(audio_frame) == -1) {
                    WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                                 "failed PopMemory() call");
                    assert(false);
                    return -1;
                }
                audio_frame->sample_rate_hz_ = _outputFrequency;
                auto ret = (*it)->GetAudioFrameWithMuted(_id, audio_frame);
                if(ret == MixerParticipant::AudioFrameInfo::kError) {
                    WEBRTC_TRACE(kTraceWarning, kTraceAudioMixerServer, _id,
                                 "failed to GetAudioFrame() from participant");
                    _audioFramePool->PushMemory(audio_frame);
                    temp_frame_count = channel->GetDecoodedFrameNum();
                    break;
                }
                audio_frame->ntp_time_ms_ = -1;
                temp_frame_count = channel->AddDecoodedFrame(audio_frame);
            }
            
            if (temp_frame_count <= dead_water_level) {
                channel->AddLowWaterMark();
//                printf("channel(%d) add AddLowWaterMark, tempframecount=%d\n", channel->ChannelId(), temp_frame_count);
                continue;
            }
            else if (temp_frame_count > critical_water_level) {
//                printf("channel(%d) temp_frame_count > critical_water_level\n", channel->ChannelId());
                //清除一些缓存
                int drop_frame_num = temp_frame_count - low_water_level;
                for (int i = 0; i < drop_frame_num; i++) {
                    AudioFrame* audio_frame = channel->PopFrontFrame();
                    _audioFramePool->PushMemory(audio_frame);
                }
            }
            else if (temp_frame_count >= low_water_level && channel->InLowWaterStatus()) {
                channel->ClearLowWaterStatus();
//                printf("channel(%d) ClearLowWaterStatus\n", channel->ChannelId());
            }
            
            //计算所有混音通道中拥有帧数的最大值
            per_channel_frame_max_count = temp_frame_count > per_channel_frame_max_count ? temp_frame_count : per_channel_frame_max_count;
            if (per_channel_frame_min_count == 0) {
                per_channel_frame_min_count = temp_frame_count;
            }
            else if (temp_frame_count < per_channel_frame_min_count){
                per_channel_frame_min_count = temp_frame_count;
            }
        }
        
        can_process_count = 4;
        //计算本次循环将要混音的次数
//        if (per_channel_frame_max_count >= critical_water_level) {
//            can_process_count = 6;
//        }
        if (per_channel_frame_min_count >= high_water_level) {
            can_process_count = 6;
        }
        
//        printf("can_process_count=%d, max_count=%d, min_count=%d\n", can_process_count, per_channel_frame_max_count, per_channel_frame_min_count);
        return can_process_count;
    }
    
    int32_t AudioConferenceMixerImpl::ProcessForChannelInnerV2(const int32_t channelID, bool mix_all)
    {
        CriticalSectionScoped cs(_cbCrit.get());
        AudioFrameList mixList;
        std::multimap<uint32_t, AudioFrame*> active_map;
        std::multimap<uint32_t, AudioFrame*> ramp_out_map;
        bool was_mixed = false;
        int mix_factor = 1;
        
        if (mix_all) {
            for (auto it = _participantList.begin(); it != _participantList.end(); ++it) {
                webrtc::voe::Channel* channel = static_cast<webrtc::voe::Channel*>(*it);
                AudioFrame* audio_frame = channel->GetFrontFrame();
                if (audio_frame) {
                    const uint32_t current_energy = CalculateEnergy(*audio_frame);
                    audio_frame->current_energy = current_energy;
//                    uint64_t energy_level = channel->PushEnergyToChannel(current_energy);
                    AddFrameToAvailableContainer(current_energy, audio_frame);
                }
                else {
//                    if(_audioFramePool->PopMemory(audio_frame) == -1) {
//                        continue;
//                    }
//                    audio_frame->UpdateFrame(channel->ChannelId(), _timeStamp, NULL, _outputFrequency / 100, _outputFrequency, AudioFrame::kNormalSpeech, AudioFrame::kVadPassive, 2);
////                    uint64_t energy_level = channel->PushEnergyToChannel(0);
//                    AddFrameToAvailableContainer(0, audio_frame);
                }
            }
            mix_factor = (int)_participantList.size();
            mix_factor = mix_factor > kMaximumAmountOfMixedParticipants ? kMaximumAmountOfMixedParticipants : mix_factor;
        }
        else {
            mix_factor = _participantList.size() > 1 ? (int)_participantList.size() - 1 : 1;
            mix_factor = mix_factor > kMaximumAmountOfMixedParticipants ? kMaximumAmountOfMixedParticipants : mix_factor;
        }
        
        const auto& mix_history_it = channel_mix_history_.find(channelID);
        std::set<int> new_mix_history;
        if (mix_history_it == channel_mix_history_.end()) {
            auto& mix_set = channel_mix_history_[channelID];
            for (auto it = available_mix_frame_container_.rbegin(); it != available_mix_frame_container_.rend(); ++it) {
                if (it->second->id_ == channelID) {
                    continue;
                }
                AudioFrame* audio_frame = NULL;
                if(_audioFramePool->PopMemory(audio_frame) == -1) {
                    return -1;
                }
                audio_frame->CopyFrom(*it->second);
                mixList.push_back(FrameAndMuteInfo(audio_frame, false));
                new_mix_history.insert(audio_frame->id_);
                if (mixList.size() == kMaximumAmountOfMixedParticipants) {
                    break;
                }
            }
            mix_set = new_mix_history;
        }
        else {
            auto& mix_set = mix_history_it->second;
            for (auto it = available_mix_frame_container_.rbegin(); it != available_mix_frame_container_.rend(); ++it) {
                if (it->second->id_ == channelID) {
                    continue;
                }
                AudioFrame* audio_frame = NULL;
                if(_audioFramePool->PopMemory(audio_frame) == -1) {
                    return -1;
                }
                audio_frame->CopyFrom(*it->second);
                
                auto mix_set_it = mix_set.find(it->second->id_);
                if (mix_set_it != mix_set.end()) {
                    //混合过
                    was_mixed = true;
                }
                
                if (!was_mixed) {
                    RampIn(*audio_frame);
                }
                
                const uint32_t current_energy = CalculateEnergy(*audio_frame);
                audio_frame->current_energy = current_energy;
                if (active_map.size() >= kMaximumAmountOfMixedParticipants) {
                    AudioFrame* replaced_frame = nullptr;
                    auto active_it = active_map.insert(std::pair<uint32_t, AudioFrame*>(current_energy, audio_frame));
                    if (active_it != active_map.begin()) {
                        replaced_frame = active_map.begin()->second;
                        active_map.erase(active_map.begin());
                    }
                    else {
                        active_map.erase(active_it);
                    }
                    
                    if (replaced_frame) {
                        RampOut(*replaced_frame);
                        
                        ramp_out_map.insert(std::pair<uint32_t, AudioFrame*>(current_energy, replaced_frame));
                    }
                    else {
                        if (was_mixed) {
                            RampOut(*audio_frame);
                            ramp_out_map.insert(std::pair<uint32_t, AudioFrame*>(current_energy, audio_frame));
                        }
                        else {
                            _audioFramePool->PushMemory(audio_frame);
                        }
                    }
                }
                else {
                    active_map.insert(std::pair<uint32_t, AudioFrame*>(current_energy, audio_frame));
                }
            }
            
            mix_set.clear();
            for (auto frame_info : active_map) {
                mixList.push_back(FrameAndMuteInfo(frame_info.second, false));
                mix_set.insert(frame_info.second->id_);
            }
            for (auto frame_info : ramp_out_map) {
                mixList.push_back(FrameAndMuteInfo(frame_info.second, false));
            }
        }
        
        if (mixList.size() == 0) {
            return -1;
        }
        
        // Get an AudioFrame for mixing from the memory pool.
        AudioFrame* mixedAudio = NULL;
        if(_audioFramePool->PopMemory(mixedAudio) == -1) {
            WEBRTC_TRACE(kTraceMemory, kTraceAudioMixerServer, _id,
                         "failed PopMemory() call");
            assert(false);
            return -1;
        }
        
        {
            CriticalSectionScoped cs(_crit.get());
            
            // TODO(henrike): it might be better to decide the number of channels
            //                with an API instead of dynamically.
            
            // Find the max channels over all mixing lists.
            const size_t num_mixed_channels = MaxNumChannels(&mixList);
            
            mixedAudio->UpdateFrame(-1, _timeStamp, NULL, 0, _outputFrequency,
                                    AudioFrame::kNormalSpeech,
                                    AudioFrame::kVadPassive, num_mixed_channels);
            
            _timeStamp += static_cast<uint32_t>(_sampleSize);
            
            MixFromList(mixedAudio, mixList, mix_factor);
            
            if(mixedAudio->samples_per_channel_ == 0) {
                // Nothing was mixed, set the audio samples to silence.
                mixedAudio->samples_per_channel_ = _sampleSize;
                AudioFrameOperations::Mute(mixedAudio);
            }
            
            //归还mixlist的frame到内存池
            for (auto frame_info : mixList) {
                _audioFramePool->PushMemory(frame_info.frame);
            }
        }
        
        {
            CriticalSectionScoped cs(_cbCrit.get());
            if(_mixReceiver != NULL) {
                const AudioFrame** dummy = NULL;
                _mixReceiver->NewMixedAudio(_id, *mixedAudio, dummy, 0);
            }
        }
        
        // Reclaim all outstanding memory.
        _audioFramePool->PushMemory(mixedAudio);
        return 0;
    }
    
    int32_t AudioConferenceMixerImpl::MixFromList(AudioFrame* mixed_frame, const AudioFrameList& audioFrameList, int mix_factor)
    {
        if (_numMixedParticipants == 1) {
            mixed_frame->timestamp_ = audioFrameList.front().frame->timestamp_;
            mixed_frame->elapsed_time_ms_ = audioFrameList.front().frame->elapsed_time_ms_;
        } else {
            // TODO(wu): Issue 3390.
            // Audio frame timestamp is only supported in one channel case.
            mixed_frame->timestamp_ = 0;
            mixed_frame->elapsed_time_ms_ = -1;
        }
        
        AudioFrame* frames[kMaximumAmountOfMixedParticipants];
        int count = 0;
        for (auto frame_info : audioFrameList) {
            if (count < kMaximumAmountOfMixedParticipants && !frame_info.muted) {

                if (mixed_frame->num_channels_ > frame_info.frame->num_channels_) {
                    assert(mixed_frame->num_channels_ == 2 &&
                           frame_info.frame->num_channels_ == 1);
                    AudioFrameOperations::MonoToStereo(frame_info.frame);
                }
                if (mixed_frame->samples_per_channel_ != frame_info.frame->samples_per_channel_) {
                    mixed_frame->samples_per_channel_ = frame_info.frame->samples_per_channel_;
                }
                
                mixed_frame->client_ssrcs[count*2] = frame_info.frame->source_ssrc;
                mixed_frame->client_ssrcs[count*2+1] = frame_info.frame->current_energy;
                frames[count++] = frame_info.frame;
            }
        }
        mixed_frame->client_ssrcs_num = count;
        
//        float factor = 1.0f;
//        auto it = factor_container_.find(channel_id);
//        if (it != factor_container_.end()) {
//            factor = it->second;
//        }
        
        AudioFrameOperations::AddMultiFrame(frames, count, mixed_frame, mix_factor);
        
//        factor_container_[channel_id] = factor;
        return 0;
    }
    
    void AudioConferenceMixerImpl::AddFrameToAvailableContainer(int64_t energy_level, AudioFrame* audio_frame)
    {
        available_mix_frame_container_.insert(std::make_pair(energy_level, audio_frame));
    }
    
    void AudioConferenceMixerImpl::GetBackFrameFromChannel(MixerParticipant* participant) const
    {
        webrtc::voe::Channel* channel = static_cast<webrtc::voe::Channel*>(participant);
        if (channel) {
            while (true) {
                AudioFrame* audio_frame = channel->PopFrontFrame();
                if (audio_frame) {
                    _audioFramePool->PushMemory(audio_frame);
                }
                else
                    break;
            }
        }
    }
}  // namespace webrtc
